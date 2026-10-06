/* Copyright 2024 Jan Stephan, Luca Ferragina, Aurora Perego, Andrea Bocci, René Widera,
 * Mehmet Yusufoglu, Tim Hanel.
 * SPDX-License-Identifier: ISC
 */

// Adapted from alpaka3/example/vectorAdd/src/vectorAdd.cpp.
#include <alpakaMetrics/alpakaMetrics.hpp>

#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <syncstream>
#include <vector>

namespace
{
    class VectorAddKernel
    {
    public:
        ALPAKA_FN_ACC void operator()(
            alpaka::onAcc::concepts::Acc auto const& acc,
            alpaka::concepts::IView auto const a,
            alpaka::concepts::IView auto const b,
            alpaka::concepts::IView auto c,
            alpaka::concepts::Vector auto const extent) const
        {
            auto simdGrid = alpaka::onAcc::SimdAlgo{alpaka::onAcc::worker::threadsInGrid};
            simdGrid.concurrent(
                acc,
                extent,
                [](auto const&, auto&& simdA, auto&& simdB, auto&& simdC) constexpr
                { simdC = simdA.load() + simdB.load(); },
                a,
                b,
                c);
        }
    };

    int example(alpaka::concepts::BackendSpec auto const& backend)
    {
        auto selector = alpaka::onHost::makeDeviceSelector(alpaka::onHost::DeviceSpec{backend});
        if(!selector.isAvailable())
            return 0;

        using Data = std::uint32_t;
        constexpr std::size_t numberOfRuns = 16;
        auto const extent = alpaka::Vec<std::size_t, 1>{123456};
        auto device = selector.makeDevice(0u);
        auto rawQueue = device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled);
        auto hostA = alpaka::onHost::allocHost<Data>(extent);
        auto hostB = alpaka::onHost::allocHostLike(hostA);
        auto hostC = alpaka::onHost::allocHostLike(hostA);
        for(std::size_t i = 0; i < extent.x(); ++i)
        {
            hostA[i] = static_cast<Data>(i % 42 + 1);
            hostB[i] = static_cast<Data>(i % 17 + 1);
        }
        auto a = alpaka::onHost::allocLike(device, hostA);
        auto b = alpaka::onHost::allocLike(device, hostB);
        auto c = alpaka::onHost::allocLike(device, hostC);
        alpaka::onHost::memcpy(rawQueue, a, hostA);
        alpaka::onHost::memcpy(rawQueue, b, hostB);

        // Results arrive on the progress thread while the application can submit more work.
        std::mutex resultsMutex;
        std::vector<alpakaMetrics::Result> results;
        alpakaMetrics::Session session;
        session.setResultCallback(
            [&](alpakaMetrics::Result const& result)
            {
                std::lock_guard lock{resultsMutex};
                results.push_back(result);
                std::osyncstream{std::cout} << "Completed " << result.measurementId << ": "
                                            << result.getMetric("elapsed_time").asDouble() << " s on "
                                            << result.provenance.api << '\n';
            });
        auto queue = alpakaMetrics::makeQueue(rawQueue, session, alpakaMetrics::Config{.label = "vector_add"});
        auto const chunkSize = alpaka::Vec<std::size_t, 1>{256};
        auto const elementsPerWorker = alpaka::getNumElemPerThread<Data>(rawQueue);
        auto spec = alpaka::onHost::FrameSpec{
            alpaka::divCeil(extent, chunkSize * elementsPerWorker),
            chunkSize,
            alpaka::getExecutor(backend)};
        auto bundle = alpaka::KernelBundle{VectorAddKernel{}, a, b, c, extent};
        std::vector<alpakaMetrics::Measurement> measurements;
        measurements.reserve(numberOfRuns);
        std::cout << "Submitting " << numberOfRuns << " vector additions on " << device.getName() << " / "
                  << alpaka::getExecutor(backend).getName() << '\n';
        for(std::size_t run = 0; run < numberOfRuns; ++run)
            measurements.push_back(queue.enqueue(spec, bundle));

        // One batch boundary, after every launch and the copy have been submitted.
        alpaka::onHost::memcpy(rawQueue, hostC, c);
        session.drain();
        alpaka::onHost::wait(rawQueue);
        auto stats = session.getStats();
        if(stats.failedDeliveries || stats.droppedPending || stats.droppedCompleted || results.size() != numberOfRuns)
            throw std::runtime_error{"Incomplete result delivery: " + stats.lastError};
        for(std::size_t i = 0; i < extent.x(); ++i)
            if(hostC[i] != hostA[i] + hostB[i])
                throw std::runtime_error{"Vector addition produced an incorrect result"};
        // Stream delivery and direct retrieval refer to the same retained measurements.
        for(auto const& measurement : measurements)
            if(!measurement.tryGetResults())
                throw std::runtime_error{"Completed measurement has no retained snapshot"};
        std::cout << "Validated vector addition and " << results.size() << " completion callbacks\n";
        return 0;
    }
} // namespace

int main()
{
    try
    {
        return alpaka::onHost::executeForEach(
            [](alpaka::concepts::BackendSpec auto const& backend) { return example(backend); },
            alpaka::onHost::allBackends(alpaka::onHost::enabledDeviceSpecs, alpaka::exec::enabledExecutors));
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
