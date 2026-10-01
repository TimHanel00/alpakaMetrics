// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>

#include <atomic>
#include <iostream>

struct Work
{
    void operator()(auto const&, std::atomic<std::uint64_t>* output) const
    {
        for(std::uint64_t i = 0u; i < 100000u; ++i)
            output->fetch_add(i, std::memory_order_relaxed);
    }
};

int main()
{
    auto device = alpaka::onHost::makeDeviceSelector(alpaka::api::host, alpaka::deviceKind::cpu).makeDevice(0u);
    auto rawQueue = device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled);
    alpakaMetrics::Config config{
        .metrics = {alpakaMetrics::metric::elapsedTime, alpakaMetrics::metric::instructions},
        .label = "work"};
    auto queue = alpakaMetrics::makeQueue(rawQueue, config);
    auto hostSession = alpakaMetrics::HostSideInstrumentation(config);
    std::atomic<std::uint64_t> output{};
    hostSession.begin();
    auto spec = alpaka::onHost::FrameSpec{alpaka::Vec{4u}, alpaka::Vec{1u}, alpaka::exec::cpuSerial};
    auto first = queue.enqueue(spec, Work{}, &output);
    auto second = queue.enqueue(spec, Work{}, &output);
    alpaka::onHost::wait(queue);
    auto region = hostSession.end();
    for(auto const& result : {first.getResults(), second.getResults(), region})
    {
        std::cout << "Measurement " << result.measurementId << '\n';
        for(auto const& metric : result.metrics)
        {
            std::cout << "  " << metric.descriptor.name << ": ";
            if(metric.isAvailable())
                std::cout << metric.asDouble();
            else
                std::cout << "unavailable (" << metric.diagnostic << ')';
            std::cout << '\n';
        }
    }
}
