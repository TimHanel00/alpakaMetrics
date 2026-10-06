// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>
#include <alpakaTune/alpakaTune.hpp>

#include <atomic>
#include <iostream>
#include <stdexcept>

namespace
{
    inline constexpr auto amount = ALPAKA_TUNE_TUNABLE("amount");

    struct Work
    {
        void operator()(auto const&, std::atomic<std::uint64_t>* output, std::uint32_t value) const
        {
            output->fetch_add(value, std::memory_order_relaxed);
        }
    };
} // namespace

int main()
{
    try
    {
        auto device = alpaka::onHost::makeDeviceSelector(alpaka::api::host, alpaka::deviceKind::cpu).makeDevice(0u);
        auto queue = alpakaMetrics::makeQueue(device.makeQueue(alpaka::timing::enabled));
        auto config = alpakaTune::TunerConfig{};
        config.exploration = alpakaTune::ExplorationPolicy::online;
        config.selection = alpakaTune::SelectionPolicy::fixed;
        config.strategy = alpakaTune::StrategyKind::exhaustive;
        config.queue.reset();
        config.runsPerCandidate = 1u;
        config.minimumRunsPerCandidate = 1u;
        config.mannWhitneyEarlyStop = false;
        config.maximumExecutions = 2u;
        config.history.read = false;
        config.history.write = false;
        config.completeHistory.read = false;
        config.completeHistory.write = false;
        auto tunables = alpakaTune::TunableBundle{amount(alpakaTune::RVals{1u, 2u})};
        auto tuner = alpakaTune::makeTuner(
            config,
            tunables,
            device,
            alpakaTune::customMetric("execution_interval_seconds"),
            "alpakaMetrics-interoperability");
        std::atomic<std::uint64_t> output{};
        auto frameSpec = alpaka::onHost::FrameSpec{alpaka::Vec{1u}, alpaka::Vec{1u}, alpaka::exec::cpuSerial};
        auto bundle = alpaka::KernelBundle{Work{}, &output, amount};
        for(std::size_t i = 0u; i < 2u; ++i)
        {
            tuner.enqueue(queue, frameSpec, bundle);
            auto measurement = queue.getMeasurements().back();
            tuner.provideMetric(measurement.getResults().getMetric("elapsed_time").asDouble());
        }
        if(output != 3u || queue.getMeasurements().size() != 2u)
            throw std::runtime_error{"Tuner candidates did not execute exactly once through the profiled queue"};
        if(!tuner.lastConfig().metricValue.has_value())
            throw std::runtime_error{"Tuner did not receive the profiling objective"};
        // The built-in timer must also accept the same queue and forward its events.
        auto timedTuner = alpakaTune::makeTuner(config, tunables, device, "alpakaMetrics-timing-interoperability");
        timedTuner.enqueue(queue, frameSpec, bundle);
        if(queue.getMeasurements().size() != 3u)
            throw std::runtime_error{"Built-in timing duplicated profiling or measured synchronization events"};
        std::cout << "Tuner custom objectives and built-in timing interoperate without a core dependency\n";
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
