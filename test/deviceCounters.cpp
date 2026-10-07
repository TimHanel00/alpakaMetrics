// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>

#include <future>
#include <iostream>
#include <stdexcept>

namespace
{
    void check(bool condition, char const* message)
    {
        if(!condition)
            throw std::runtime_error{message};
    }

    void checkRouting()
    {
        using namespace alpakaMetrics;
#if !ALPAKA_METRICS_TEST_PAPI
        return; // Routing rules are owned by the optional PAPI module.
#endif
        for(auto const component : {"cuda", "rocp_sdk"})
        {
            Config config{
                .metrics
                = {metric::instructions,
                   metric::native("sde:::unrelated::event"),
                   metric::native(std::string{component} + ":::invalid:device=7"),
                   metric::native(std::string{component} + ":::invalid:device=bad"),
                   metric::native(std::string{component} + ":::invalid:device=0:device=0"),
                   metric::native(std::string{component} + ":::invalid")},
                .allowSynchronization = true};
            internal::Counters counters{config, internal::DeviceCounterTarget{component, 0u}};
            check(!counters.hasEvents() && !counters.isRunning(), "Invalid device events started collection");
            counters.begin();
            auto results = counters.end();
            for(std::size_t i = 0u; i < 5u; ++i)
                check(
                    results[i].status == MetricStatus::unsupportedScope && !results[i].value,
                    "Device provider accepted CPU counters, another component, or a mismatched device");
            check(!results.back().isAvailable(), "Unknown native event was reported available");
            check(
                results.back().descriptor.nativeName == std::string{component} + ":::invalid:device=0",
                "Implicit native device selection was lost");
        }
        internal::Counters scoped{
            Config{
                .metrics = {metric::native("cuda:::invalid")},
                .counterScope = MetricScope::callingThread,
                .allowSynchronization = true},
            internal::DeviceCounterTarget{"cuda", 0u}};
        check(
            scoped.end().at(0u).status == MetricStatus::unsupportedScope,
            "Calling-thread scope was accepted for a device queue");

        // A native component lookup claims a process-wide lease, even if the
        // requested event is unavailable. Nested regions must not enter that
        // component concurrently, including on the same thread.
        Config native{.metrics = {metric::native("cuda:::invalid")}, .allowSynchronization = true};
        {
            internal::Counters outer{native, internal::DeviceCounterTarget{"cuda", 0u}};
            internal::Counters nested{native, internal::DeviceCounterTarget{"cuda", 0u}};
            auto const status = nested.end().at(0u).status;
            check(
                status == MetricStatus::conflicting || status == MetricStatus::dependencyDisabled
                    || outer.end().at(0u).status == MetricStatus::collectionFailed,
                "Nested native component lookup did not report a conflict");
        }
        internal::Counters after{native, internal::DeviceCounterTarget{"cuda", 0u}};
        check(
            after.end().at(0u).status != MetricStatus::conflicting,
            "Completed native region retained its process-wide lease");
    }

    void checkAsyncFallback()
    {
        using namespace alpakaMetrics;
        auto device = alpaka::onHost::makeDeviceSelector(alpaka::api::host, alpaka::deviceKind::cpu).makeDevice(0u);
        auto queue = device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled);
        auto release = std::make_shared<std::promise<void>>();
        auto ready = release->get_future().share();
        // Block real queue work to prove enqueue does not wait when counters are unavailable.
        queue.enqueueHostFn([ready] { ready.wait(); });
        Config config{.metrics = {metric::native("cuda:::invalid")}};
        internal::Counters counters{config, internal::DeviceCounterTarget{"cuda", 0u}};
        std::uint64_t launches{};
        auto measurement = internal::enqueueDevice(
            queue,
            config,
            [&] { queue.enqueueHostFn([&launches] { ++launches; }); },
            std::ref(counters));
        check(!measurement.isComplete(), "Unavailable counters synchronized the queue");
        auto read = std::async(std::launch::async, [measurement] { return measurement.getResults(); });
        auto const premature = read.wait_for(std::chrono::milliseconds{20}) == std::future_status::ready;
        release->set_value();
        auto result = read.get();
        check(!premature, "Counter-only measurement returned before its queued work completed");
        check(
            launches == 1u && !result.synchronized && !result.replayed && result.passCount == 1u,
            "Fallback replayed or synchronously measured the launch");
        check(!result.metrics.at(0u).value, "Unavailable device counter has a fabricated value");
    }
} // namespace

int main()
{
    try
    {
        checkRouting();
        checkAsyncFallback();
        std::cout << "Device counter routing and asynchronous fallback checks passed\n";
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
