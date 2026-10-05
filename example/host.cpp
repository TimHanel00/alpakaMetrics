// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>

#include <cstdint>
#include <iostream>

struct Work
{
    ALPAKA_FN_ACC void operator()(auto const& acc, auto output) const
    {
        for(auto const idx : alpaka::onAcc::makeIdxMap(
                acc,
                alpaka::onAcc::worker::threadsInGrid,
                alpaka::IdxRange{alpaka::Vec{100000u}}))
            alpaka::onAcc::atomicAdd(acc, &output[alpaka::Vec{0u}], static_cast<std::uint64_t>(idx.x()));
    }
};

auto example(alpaka::concepts::BackendSpec auto const& backend) -> int
{
    auto deviceSelector = alpaka::onHost::makeDeviceSelector(alpaka::onHost::DeviceSpec{backend});
    if(!deviceSelector.isAvailable())
        return 0;

    auto device = deviceSelector.makeDevice(0u);
    auto rawQueue = device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled);
    auto outputDevice = alpaka::onHost::alloc<std::uint64_t>(device, alpaka::Vec{1u});
    auto outputHost = alpaka::onHost::allocHostLike(outputDevice);
    alpaka::onHost::memset(rawQueue, outputDevice, 0u);
    alpakaMetrics::Config config{
        .metrics = {alpakaMetrics::metric::elapsedTime, alpakaMetrics::metric::instructions},
        .label = "work"};
    auto queue = alpakaMetrics::makeQueue(rawQueue, config);
    auto hostSession = alpakaMetrics::HostSideInstrumentation(config);
    hostSession.begin();
    auto spec = alpaka::onHost::FrameSpec{alpaka::Vec{100000u}, alpaka::Vec{1u}, alpaka::getExecutor(backend)};
    auto first = queue.enqueue(spec, Work{}, outputDevice);
    auto second = queue.enqueue(spec, Work{}, outputDevice);
    alpaka::onHost::memcpy(queue, outputHost, outputDevice);
    alpaka::onHost::wait(queue);
    auto region = hostSession.end();
    std::cout << "Backend " << device.getName() << " accumulated output: " << outputHost[0u] << '\n';
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
    return 0;
}

auto main() -> int
{
    return alpaka::onHost::executeForEach(
        [](alpaka::concepts::BackendSpec auto const& backend) { return example(backend); },
        alpaka::onHost::allBackends(alpaka::onHost::enabledDeviceSpecs, alpaka::exec::enabledExecutors));
}
