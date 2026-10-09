// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>
#include <alpakaMetrics/internal/AsyncCounters.hpp>

#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>

namespace
{
    using namespace alpakaMetrics;
    using Release = void (*)(std::uint64_t) noexcept;

    void check(bool condition, char const* message)
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

    Config configuration(std::string const& path, std::string const& mode = "delayed")
    {
        return Config{.metrics = {metric::elapsedTime, metric::native(mode)}, .collectorPlugin = path};
    }

    void checkFailures(std::string const& path, Release release)
    {
        auto const collectorModule = internal::collector::loadModule(path);
        for(auto const mode : {"missing", "wrong", "duplicate", "failed", "create_failed", "begin_failed", "stall"})
        {
            auto config = configuration(path, mode);
            auto const id = internal::nextMeasurementId();
            plugin::Options options{static_cast<std::uint32_t>(MetricScope::queueInterval), "host", 0};
            auto const timeout
                = std::string_view{mode} == "stall" ? std::chrono::milliseconds{20} : std::chrono::milliseconds{500};
            internal::AsyncCounters counters{collectorModule, config, options, {id, 0}, timeout};
            counters.begin();
            counters.submitted();
            if(std::string_view{mode} != "stall")
                release(id);
            auto results = counters.getResults();
            check(
                results.size() == 1 && results[0].status == MetricStatus::collectionFailed && !results[0].value
                    && !results[0].diagnostic.empty(),
                "Missing, invalid or stalled delivery fabricated a value");
            check(results[0].descriptor.collector == "test_async", "Failure lost collector identity");
            if(std::string_view{mode} == "wrong")
                check(
                    results[0].diagnostic.find("operation ID") != std::string::npos,
                    "Incorrect ID was not rejected");
            if(std::string_view{mode} == "duplicate")
                check(
                    results[0].diagnostic.find("duplicate") != std::string::npos,
                    "Duplicate metric was not rejected");
            if(std::string_view{mode} == "failed")
                check(results[0].diagnostic == "records dropped", "Provider failure diagnostic was lost");
        }
        // Outstanding callbacks must be cancelled safely when the last handle disappears.
        auto const id = internal::nextMeasurementId();
        auto config = configuration(path);
        auto counters = internal::makeAsyncCounters(config, id, "host", 0, 0);
        counters->begin();
        counters->submitted();
        counters.reset();
        release(id);
    }

    bool checkBackend(alpaka::concepts::BackendSpec auto const& backend, std::string const& path, Release release)
    {
        using Api = decltype(alpaka::getApi(backend));
        if constexpr(
            !std::same_as<Api, alpaka::api::Host> && !std::same_as<Api, alpaka::api::Cuda>
            && !std::same_as<Api, alpaka::api::Hip>)
            return false;
        else
        {
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
            alpaka::onHost::memset(rawA, outputA, 0u);
            alpaka::onHost::memset(rawB, outputB, 0u);
            // Warm up compilation/runtime initialization before blocking a queue.
            rawA.enqueue(spec, Increment{}, outputA);
            alpaka::onHost::memset(rawA, outputA, 0u);
            alpaka::onHost::wait(rawA);
            alpaka::onHost::wait(rawB);

            std::mutex mutex;
            std::condition_variable changed;
            std::vector<Result> delivered;
            Session session;
            session.setResultCallback(
                [&](Result const& result)
                {
                    {
                        std::lock_guard lock{mutex};
                        delivered.push_back(result);
                    }
                    changed.notify_all();
                });
            auto queueA = makeQueue(rawA, session, configuration(path));
            auto queueB = makeQueue(rawB, session, configuration(path));
            auto gate = std::make_shared<std::promise<void>>();
            auto ready = gate->get_future().share();
            rawA.enqueueHostFn([ready] { ready.wait(); });
            // Watchdog prevents a regression that synchronizes submission from hanging the test.
            std::promise<void> submitted;
            auto submissionReady = submitted.get_future();
            std::atomic<bool> timedOut{};
            std::jthread watchdog{
                [gate, &submissionReady, &timedOut]
                {
                    if(submissionReady.wait_for(std::chrono::seconds{5}) != std::future_status::ready)
                    {
                        timedOut = true;
                        gate->set_value();
                    }
                }};
            auto a = queueA.enqueue(spec, Increment{}, outputA);
            auto b = queueB.enqueue(spec, Increment{}, outputB);
            bool const queued = !a.isComplete();
            submitted.set_value();
            watchdog.join();
            if(!queued || timedOut)
                throw std::runtime_error{"Asynchronous collection waited during submission"};
            gate->set_value();
            alpaka::onHost::wait(rawA);
            alpaka::onHost::wait(rawB);
            check(!a.isComplete() && !a.tryGetResults(), "Execution completion bypassed collector readiness");
            release(b.getId());
            {
                std::unique_lock lock{mutex};
                check(
                    changed.wait_for(lock, std::chrono::seconds{5}, [&] { return delivered.size() == 1; }),
                    "Ready second queue was blocked by first collector");
                check(delivered[0].measurementId == b.getId(), "Out-of-order delivery lost operation correlation");
            }
            auto direct = std::async(std::launch::async, [a] { return a.getResults(); });
            check(
                direct.wait_for(std::chrono::milliseconds{20}) != std::future_status::ready,
                "Blocking result read bypassed pending collector records");
            release(a.getId());
            auto result = direct.get();
            session.drain();
            check(delivered.size() == 2 && session.getStats().failedDeliveries == 0, "Session lost async results");
            for(auto const& measurement : {a, b})
            {
                auto snapshot = measurement.getResults();
                check(
                    std::get<std::uint64_t>(*snapshot.getMetric("delayed").value) == measurement.getId(),
                    "Provider callback was attributed to the wrong operation");
                check(
                    !snapshot.synchronized && !snapshot.replayed && snapshot.passCount == 1,
                    "Async provider changed execution semantics");
                check(snapshot.getMetric("elapsed_time").isAvailable(), "Async provider lost timing");
                check(snapshot.provenance.sessionId == session.getId(), "Async result lost session provenance");
            }
            queueA.clearMeasurements();
            check(a.tryGetResults().has_value(), "Queue history clearing invalidated async results");
            alpaka::onHost::memcpy(rawA, hostA, outputA);
            alpaka::onHost::memcpy(rawB, hostB, outputB);
            alpaka::onHost::wait(rawA);
            alpaka::onHost::wait(rawB);
            check(hostA[0] == 1 && hostB[0] == 1, "Async collection replayed or lost work");
            std::cout << "Async collector passed: " << alpaka::getApi(backend).getName() << " / "
                      << alpaka::getExecutor(backend).getName() << '\n';
            return true;
        }
    }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        check(argc == 4, "Expected asynchronous, synchronized and incompatible collector paths");
        std::string path{argv[1]};
        check(getCollectorInfo(path).asynchronous, "Async capability was not discovered");
        check(getAvailableMetrics(path).size() == 1, "Async discovery failed");
        auto* collectorModule = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        auto release = reinterpret_cast<Release>(dlsym(collectorModule, "alpakaMetrics_testRelease"));
        check(release != nullptr, "Fixture release entry point missing");
        checkFailures(path, release);
        bool rejected = false;
        try
        {
            static_cast<void>(getCollectorInfo(argv[3]));
        }
        catch(std::runtime_error const&)
        {
            rejected = true;
        }
        check(rejected, "Incompatible asynchronous ABI was accepted");
        auto config = configuration(argv[2]);
        auto denied = internal::makeAsyncCounters(config, internal::nextMeasurementId(), "cuda", 0, 0);
        denied->begin();
        denied->submitted();
        check(
            !denied->requiresSynchronization() && denied->getResults().at(0).status == MetricStatus::unsupportedScope,
            "Asynchronous provider synchronized without opt-in");
        config.allowSynchronization = true;
        auto accepted = internal::makeAsyncCounters(config, internal::nextMeasurementId(), "cuda", 0, 0);
        check(accepted->requiresSynchronization(), "Asynchronous synchronization opt-in was ignored");
        accepted.reset();
        alpaka::onHost::executeForEach(
            [&](alpaka::concepts::BackendSpec auto const& backend)
            {
                checkBackend(backend, path, release);
                return 0;
            },
            alpaka::onHost::allBackends(alpaka::onHost::enabledDeviceSpecs, alpaka::exec::enabledExecutors));
        dlclose(collectorModule);
        std::cout << "Delayed delivery, failures, timeout and cancellation passed\n";
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
