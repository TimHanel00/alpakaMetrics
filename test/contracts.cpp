// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace
{
    void check(bool condition, char const* message)
    {
        if(!condition)
            throw std::runtime_error{message};
    }

    template<typename T_Exception, typename T_Fn>
    void expectThrow(T_Fn fn)
    {
        bool caught{};
        try
        {
            fn();
        }
        catch(T_Exception const&)
        {
            caught = true;
        }
        check(caught, "Expected exception was not thrown");
    }

    struct Increment
    {
        void operator()(auto const&, std::atomic<std::uint64_t>* count) const
        {
            count->fetch_add(1u, std::memory_order_relaxed);
        }
    };

    auto makeDevice()
    {
        return alpaka::onHost::makeDeviceSelector(alpaka::api::host, alpaka::deviceKind::cpu).makeDevice(0u);
    }

    void testHostRegions()
    {
        using namespace alpakaMetrics;
        HostSideInstrumentation host{Config{
            .metrics = {metric::elapsedTime, metric::instructions, metric::native("does_not_exist")},
            .label = "host"}};
        expectThrow<std::logic_error>([&] { static_cast<void>(host.end()); });
        expectThrow<std::logic_error>([&] { static_cast<void>(host.getResults()); });
        host.begin();
        expectThrow<std::logic_error>([&] { host.begin(); });
        auto wrongThread = std::async(
            std::launch::async,
            [&] { expectThrow<std::logic_error>([&] { static_cast<void>(host.end()); }); });
        wrongThread.get();
        std::atomic<std::uint64_t> count{};
        for(std::uint64_t i = 0u; i < 100000u; ++i)
            count.fetch_add(i, std::memory_order_relaxed);
        auto result = host.end();
        check(result.label == "host", "Region label lost");
        check(result.getMetric("elapsed_time").isAvailable(), "Host time missing");
        check(result.getMetric("elapsed_time").asDouble() > 0.0, "Host time invalid");
        check(!result.getMetric("does_not_exist").isAvailable(), "Invalid counter reported available");
        check(!result.getMetric("does_not_exist").value, "Invalid counter has a fabricated value");
        expectThrow<std::logic_error>([&] { static_cast<void>(result.getMetric("does_not_exist").asDouble()); });
        auto const& instructions = result.getMetric("instructions");
        if(instructions.isAvailable())
            check(instructions.asDouble() > 0.0, "Live instruction counter did not observe work");
        else
            std::cout << "CPU instructions unavailable: " << instructions.diagnostic << '\n';
        host.begin();
        auto next = host.end();
        check(next.measurementId != result.measurementId, "Repeated regions reused measurement ID");
        check(host.getResults().measurementId == next.measurementId, "Latest region mismatch");
        expectThrow<std::invalid_argument>(
            [] { HostSideInstrumentation invalid{Config{.metrics = {metric::elapsedTime, metric::elapsedTime}}}; });
    }

    void testMetricMappings()
    {
        using namespace alpakaMetrics;
        auto duration = [](MetricUnit unit)
        { return metric::map(metric::deviceExecutionTime, metric::native("cupti.kernel_duration", unit)); };
        internal::validateConfig(Config{.metrics = {duration(MetricUnit::seconds)}});
        expectThrow<std::invalid_argument>(
            [&] { internal::validateConfig(Config{.metrics = {duration(MetricUnit::count)}}); });
        expectThrow<std::invalid_argument>(
            [&] { internal::validateConfig(Config{.metrics = {duration(MetricUnit::providerDefined)}}); });
        auto mapped = [](MetricUnit unit, double scale)
        { return metric::map(metric::energy, metric::native("invalid_energy_event", unit, scale)); };
        expectThrow<std::invalid_argument>(
            [&] { HostSideInstrumentation host{Config{.metrics = {mapped(MetricUnit::joules, 0.0)}}}; });
        expectThrow<std::invalid_argument>(
            [&]
            {
                HostSideInstrumentation host{
                    Config{.metrics = {mapped(MetricUnit::joules, std::numeric_limits<double>::infinity())}}};
            });
        expectThrow<std::invalid_argument>(
            [&] { HostSideInstrumentation host{Config{.metrics = {mapped(MetricUnit::hertz, 1.0)}}}; });
        expectThrow<std::invalid_argument>(
            [&] { HostSideInstrumentation host{Config{.metrics = {mapped(MetricUnit::providerDefined, 1.0)}}}; });
        HostSideInstrumentation host{Config{.metrics = {mapped(MetricUnit::joules, 1.0e-9)}}};
        host.begin();
        auto result = host.end();
        check(!result.getMetric("energy").isAvailable(), "Missing mapped event reported available");
        check(
            result.getMetric("energy").descriptor.unit == MetricUnit::joules,
            "Missing mapped event lost the requested unit");
    }

    void testAdditionalCounters()
    {
        using namespace alpakaMetrics;
        Config config{
            .metrics
            = {metric::elapsedTime,
               metric::l1DataMisses,
               metric::l2Accesses,
               metric::l3Accesses,
               metric::branchInstructions,
               metric::branchMispredictions,
               metric::loadInstructions,
               metric::storeInstructions,
               metric::resourceStallCycles}};
        HostSideInstrumentation host{config};
        host.begin();
        std::atomic<std::uint64_t> count{};
        for(std::uint64_t i = 0u; i < 10000u; ++i)
            count.fetch_add(i % 3u, std::memory_order_relaxed);
        auto result = host.end();
        check(result.metrics.size() == config.metrics.size(), "Mixed counter request lost result entries");
        check(result.getMetric("elapsed_time").isAvailable(), "Counter failures disabled timing");
        for(std::size_t i = 1u; i < result.metrics.size(); ++i)
        {
            auto const& metric = result.metrics[i];
            if(metric.isAvailable())
            {
                check(metric.descriptor.unit == MetricUnit::count, "CPU preset count unit lost");
                check(metric.asDouble() >= 0.0, "CPU preset returned a negative count");
            }
            else
            {
                check(!metric.value.has_value(), "Failed CPU preset has a fabricated value");
                check(!metric.diagnostic.empty(), "Failed CPU preset has no diagnostic");
            }
        }
    }

    void testQueue(bool blocking)
    {
        using namespace alpakaMetrics;
        auto device = makeDevice();
        auto exercise = [&](auto raw)
        {
            auto queue = makeQueue(raw, Config{.metrics = {metric::elapsedTime, metric::instructions}});
            static_assert(internal::concepts::Queue<decltype(queue)>);
            std::atomic<std::uint64_t> count{};
            auto spec = alpaka::onHost::ThreadSpec{alpaka::Vec{4u}, alpaka::Vec{1u}, alpaka::exec::cpuSerial};
            auto first = queue.enqueue(spec, Increment{}, &count);
            auto second = queue.enqueue(spec, alpaka::KernelBundle{Increment{}, &count});
            alpaka::onHost::wait(queue);
            check(count == 8u, "Profiling changed launch count or arguments");
            check(first.isComplete(), "Completed queue has pending measurement");
            check(first.getResults().measurementId == first.getId(), "Measurement ID mismatch");
            check(first.getId() != second.getId(), "Distinct launches share identity");
            check(
                first.getResults().getMetric("elapsed_time").descriptor.scope == MetricScope::queueInterval,
                "Queue interval mislabelled as kernel-exclusive");
            auto copy = queue;
            auto task = copy.enqueueHostFn([&count] { count.fetch_add(1u); });
            alpaka::onHost::wait(task);
            check(count == 9u, "Host operation was not forwarded once");
            check(queue.getMeasurements().size() == 3u, "Queue copies do not share measurement storage");
            auto event = queue.makeEvent();
            queue.enqueue(event);
            alpaka::onHost::wait(queue);
            check(queue.getMeasurements().size() == 3u, "Synchronization event created a measurement");
            queue.clearMeasurements();
            check(copy.getMeasurements().empty(), "Clearing measurements did not affect shared queue");
            check(first.getResults().getMetric("elapsed_time").isAvailable(), "Retained handle lost its result");
            auto failed = queue.enqueueHostFn([] { throw std::runtime_error{"Host task failed"}; });
            expectThrow<std::runtime_error>([&] { static_cast<void>(failed.getResults()); });
            auto recovery = queue.enqueueHostFn([&count] { count.fetch_add(1u); });
            recovery.wait();
            check(count == 10u, "Host task failure broke subsequent instrumentation");
        };
        if(blocking)
            exercise(device.makeQueue(alpaka::queueKind::blocking, alpaka::timing::enabled));
        else
            exercise(device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled));
    }

    void testQueueDelay()
    {
        auto device = makeDevice();
        auto raw = device.makeQueue();
        std::promise<void> started;
        std::promise<void> release;
        auto released = release.get_future().share();
        raw.enqueueHostFn(
            [&started, released]
            {
                started.set_value();
                released.wait();
            });
        started.get_future().wait();
        auto queue = alpakaMetrics::makeQueue(raw);
        std::atomic<std::uint64_t> count{};
        auto measurement = queue.enqueue(
            alpaka::onHost::FrameSpec{alpaka::Vec{1u}, alpaka::Vec{1u}, alpaka::exec::cpuSerial},
            Increment{},
            &count);
        check(!measurement.isComplete(), "Measurement completed before preceding work");
        auto const beforeRelease = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        auto const releasedAt = std::chrono::steady_clock::now();
        release.set_value();
        auto result = measurement.getResults();
        check(result.begin >= releasedAt, "Queue delay was included in measured interval");
        check(result.end >= result.begin && count == 1u, "Invalid queue execution interval");
        check(result.begin > beforeRelease, "Instrumentation did not execute on queue worker");
    }

    void testUnattributedTeamCounters()
    {
#if ALPAKA_OMP
        auto device = makeDevice();
        auto queue = alpakaMetrics::makeQueue(
            device.makeQueue(),
            alpakaMetrics::Config{
                .metrics = {alpakaMetrics::metric::elapsedTime, alpakaMetrics::metric::instructions}});
        std::atomic<std::uint64_t> count{};
        auto measurement = queue.enqueue(
            alpaka::onHost::ThreadSpec{alpaka::Vec{4u}, alpaka::Vec{1u}, alpaka::exec::cpuOmpBlocks},
            Increment{},
            &count);
        auto result = measurement.getResults();
        check(count == 4u, "OpenMP kernel execution changed");
        check(
            result.getMetric("instructions").status == alpakaMetrics::MetricStatus::unsupportedScope,
            "Launcher counters were passed off as team counters");
        check(result.getMetric("elapsed_time").isAvailable(), "OpenMP execution time unavailable");
#endif
    }

    void testPortableEventProvider()
    {
        struct EventProvider
        {
        };

        auto device = makeDevice();
        auto raw = device.makeQueue(alpaka::timing::enabled);
        std::atomic<std::uint64_t> count{};
        auto spec = alpaka::onHost::ThreadSpec{alpaka::Vec{1u}, alpaka::Vec{1u}, alpaka::exec::cpuSerial};
        auto measurement = alpakaMetrics::internal::Enqueue::Op<EventProvider>{}(
            raw,
            alpakaMetrics::Config{.metrics = {alpakaMetrics::metric::elapsedTime, alpakaMetrics::metric::cycles}},
            spec,
            alpaka::KernelBundle{Increment{}, &count});
        auto result = measurement.getResults();
        check(count == 1u, "Portable event provider repeated the kernel");
        check(result.getMetric("elapsed_time").isAvailable(), "Portable event timing is unavailable");
        check(result.getMetric("elapsed_time").descriptor.provider == "alpaka_event", "Event provider metadata lost");
        check(
            result.getMetric("cycles").status == alpakaMetrics::MetricStatus::unsupportedScope,
            "Portable fallback fabricated a hardware counter");
        auto untimed = device.makeQueue();
        expectThrow<std::invalid_argument>(
            [&]
            {
                static_cast<void>(alpakaMetrics::internal::Enqueue::Op<EventProvider>{}(
                    untimed,
                    alpakaMetrics::Config{},
                    spec,
                    alpaka::KernelBundle{Increment{}, &count}));
            });
        check(count == 1u, "Unsupported configuration executed a kernel before rejecting it");
    }
} // namespace

int main()
{
    try
    {
        testHostRegions();
        testAdditionalCounters();
        testMetricMappings();
        testQueue(false);
        testQueue(true);
        testQueueDelay();
        testUnattributedTeamCounters();
        testPortableEventProvider();
        std::cout << "All instrumentation contracts passed\n";
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
