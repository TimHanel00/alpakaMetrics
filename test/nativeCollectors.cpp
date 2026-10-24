// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>

#include <atomic>
#include <cmath>
#include <future>
#include <iostream>
#include <unordered_set>

namespace
{
    void check(bool condition, std::string const& message)
    {
        if(!condition)
            throw std::runtime_error{message};
    }

    struct Increment
    {
        ALPAKA_FN_ACC void operator()(
            alpaka::onAcc::concepts::Acc auto const& acc,
            alpaka::concepts::IView auto output) const
        {
            for(auto const index : alpaka::onAcc::makeIdxMap(
                    acc,
                    alpaka::onAcc::worker::threadsInGrid,
                    alpaka::IdxRange{alpaka::Vec{1u}}))
                ++output[index];
        }
    };

    using Release = void (*)(std::uint64_t) noexcept;

    bool testBackend(
        alpaka::concepts::BackendSpec auto const& backend,
        std::string const& provider,
        std::string const& fallback = {},
        Release release = nullptr)
    {
        using Api = decltype(alpaka::getApi(backend));
        if constexpr(!std::same_as<Api, alpaka::api::Cuda> && !std::same_as<Api, alpaka::api::Hip>)
            return false;
        else
        {
            bool const selected = (std::same_as<Api, alpaka::api::Cuda> && provider == "CUPTI")
                                  || (std::same_as<Api, alpaka::api::Hip> && provider == "rocprofiler-sdk");
            if(!selected)
                return false;
            auto selector = alpaka::onHost::makeDeviceSelector(alpaka::onHost::DeviceSpec{backend});
            if(!selector.isAvailable())
                return false;
            auto device = selector.makeDevice(0u);
            auto rawA = device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled);
            auto rawB = device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled);
            auto outputA = alpaka::onHost::alloc<std::uint32_t>(device, alpaka::Vec{1u});
            auto outputB = alpaka::onHost::alloc<std::uint32_t>(device, alpaka::Vec{1u});
            auto hostA = alpaka::onHost::allocHostLike(outputA);
            auto hostB = alpaka::onHost::allocHostLike(outputB);
            auto spec = alpaka::onHost::FrameSpec{alpaka::Vec{1u}, alpaka::Vec{1u}, alpaka::getExecutor(backend)};
            alpakaMetrics::Config config{
                .metrics
                = {alpakaMetrics::metric::elapsedTime,
                   alpakaMetrics::metric::deviceExecutionTime,
                   alpakaMetrics::metric::instructions},
                .defaultCollectorPlugin = fallback};
            alpakaMetrics::Session session;
            auto queueA = alpakaMetrics::makeQueue(rawA, session, config);
            auto queueB = alpakaMetrics::makeQueue(rawB, session, config);
            alpaka::onHost::memset(rawA, outputA, 0u);
            alpaka::onHost::memset(rawB, outputB, 0u);
            // Initialize JIT and the profiling service before testing ordinary submission.
            auto warmupMeasurement = queueA.enqueue(spec, Increment{}, outputA);
            if(release)
                release(warmupMeasurement.getId());
            auto warmup = warmupMeasurement.getResults();
            auto const& warmMetric = warmup.getMetric("device_execution_time");
            check(warmMetric.isAvailable(), "Direct collector unavailable: " + warmMetric.diagnostic);
            session.drain();
            queueA.clearMeasurements();
            static_cast<void>(session.takeCompletedMeasurements());
            alpaka::onHost::memset(rawA, outputA, 0u);
            alpaka::onHost::wait(rawA);

            std::mutex mutex;
            std::vector<alpakaMetrics::Result> results;
            session.setResultCallback(
                [&](alpakaMetrics::Result const& result)
                {
                    std::lock_guard lock{mutex};
                    results.push_back(result);
                });
            auto gate = std::make_shared<std::promise<void>>();
            auto ready = gate->get_future().share();
            rawA.enqueueHostFn([ready] { ready.wait(); });
            std::promise<void> submitted;
            auto submittedReady = submitted.get_future();
            std::atomic<bool> timedOut{};
            std::jthread watchdog{[gate, &submittedReady, &timedOut]
                                  {
                                      if(submittedReady.wait_for(std::chrono::seconds{5}) != std::future_status::ready)
                                      {
                                          timedOut = true;
                                          gate->set_value();
                                      }
                                  }};
            std::vector<alpakaMetrics::Measurement> measurements;
            for(std::uint32_t i = 0; i < 8; ++i)
            {
                measurements.push_back(queueA.enqueue(spec, Increment{}, outputA));
                if(release)
                    release(measurements.back().getId());
                measurements.push_back(queueB.enqueue(spec, Increment{}, outputB));
                if(release)
                    release(measurements.back().getId());
            }
            bool const pending = !measurements.front().tryGetResults();
            submitted.set_value();
            watchdog.join();
            check(!timedOut && pending, "Direct provider synchronized ordinary submission");
            gate->set_value();
            session.drain();
            check(
                results.size() == measurements.size() && !session.getStats().failedDeliveries,
                "Direct provider lost operation delivery");
            std::unordered_set<std::uint64_t> ids;
            for(auto const& result : results)
            {
                auto const& duration = result.getMetric("device_execution_time");
                check(duration.isAvailable(), "Native kernel activity unavailable: " + duration.diagnostic);
                check(
                    duration.descriptor.provider == provider
                        && duration.descriptor.unit == alpakaMetrics::MetricUnit::seconds
                        && duration.descriptor.scope == alpakaMetrics::MetricScope::providerDefined
                        && std::isfinite(duration.asDouble()) && duration.asDouble() > 0,
                    "Native activity lost its unit, attribution or positive duration");
                auto const& instructions = result.getMetric("instructions");
                if(release)
                    check(
                        instructions.isAvailable() && instructions.descriptor.collector == "test_async",
                        "Combined measurement lost its asynchronous fallback");
                else
                    check(!instructions.isAvailable(), "Activity provider fabricated hardware counters");
                check(
                    result.getMetric("instructions").descriptor.collector != provider,
                    "Non-activity metrics bypassed the independent legacy collector");
#if ALPAKA_METRICS_TEST_PAPI
                if(!release)
                    check(
                        instructions.descriptor.collector == "PAPI",
                        "Mixed measurement lost its independently enabled PAPI provider");
#endif
                check(
                    !result.synchronized && !result.replayed && result.passCount == 1,
                    "Direct provider changed launch semantics");
                check(ids.insert(result.measurementId).second, "Direct provider delivered an operation twice");
            }
            for(auto const& measurement : measurements)
                check(
                    ids.contains(measurement.getId()) && measurement.tryGetResults().has_value(),
                    "Native callback and direct result handles disagree");
            alpaka::onHost::memcpy(rawA, hostA, outputA);
            alpaka::onHost::memcpy(rawB, hostB, outputB);
            alpaka::onHost::wait(rawA);
            alpaka::onHost::wait(rawB);
            check(hostA[0] == 8 && hostB[0] == 8, "Native profiling replayed or lost kernels");
            std::cout << provider << " native kernel activity passed on " << device.getName() << ": " << results.size()
                      << " correlated operations\n";
            return true;
        }
    }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        check(argc == 3, "Expected native and asynchronous fallback module paths");
        auto info = alpakaMetrics::getCollectorInfo(argv[1]);
        check(
            info.asynchronous && !info.requiresDeviceSynchronization,
            "Native provider advertised wrong capabilities");
        check(
            alpakaMetrics::getAvailableMetrics(argv[1]).at(0).name == "device_execution_time",
            "Native discovery failed");
        auto selected = alpakaMetrics::internal::collector::loadOperationModule(
            {},
            info.name == "CUPTI" ? "cuda" : "hip",
            true,
            std::string{argv[1]} + ".missing");
        check(
            selected.nativeSelected && selected.module.info.name == info.name,
            "Enabled native provider did not take precedence over configurable fallback");
        static_cast<void>(alpakaMetrics::getCollectorInfo(argv[2]));
        auto* fallbackModule = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
        auto release = reinterpret_cast<Release>(dlsym(fallbackModule, "alpakaMetrics_testRelease"));
        check(release != nullptr, "Asynchronous fallback fixture release missing");
        bool tested = false;
        alpaka::onHost::executeForEach(
            [&](alpaka::concepts::BackendSpec auto const& backend)
            {
                tested = testBackend(backend, info.name) || tested;
                testBackend(backend, info.name, argv[2], release);
                return 0;
            },
            alpaka::onHost::allBackends(alpaka::onHost::enabledDeviceSpecs, alpaka::exec::enabledExecutors));
        dlclose(fallbackModule);
        return tested ? 0 : 77;
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
