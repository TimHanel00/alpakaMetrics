// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>

#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>

namespace
{
    struct Work
    {
        ALPAKA_FN_ACC void operator()(auto const& acc, auto output) const
        {
            for(auto const idx : alpaka::onAcc::makeIdxMap(
                    acc,
                    alpaka::onAcc::worker::threadsInGrid,
                    alpaka::IdxRange{alpaka::Vec{256u}}))
            {
                static_cast<void>(idx);
                alpaka::onAcc::atomicAdd(acc, &output[alpaka::Vec{0u}], std::uint64_t{1u});
            }
        }
    };

    void testDevice(
        alpaka::concepts::Api auto api,
        alpaka::concepts::DeviceKind auto kind,
        alpaka::concepts::Executor auto executor,
        std::string event)
    {
        auto device = alpaka::onHost::makeDeviceSelector(api, kind).makeDevice(0u);
        auto raw = device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled);
        auto output = alpaka::onHost::alloc<std::uint64_t>(device, alpaka::Vec{1u});
        auto host = alpaka::onHost::allocHostLike(output);
        alpaka::onHost::memset(raw, output, 0u);
        alpakaMetrics::Config config{
            .metrics = {alpakaMetrics::metric::elapsedTime, alpakaMetrics::metric::native(event)},
            .label = "native-device",
            .allowSynchronization = true};
        auto queue = alpakaMetrics::makeQueue(raw, config);
        auto spec = alpaka::onHost::FrameSpec{alpaka::Vec{4u}, alpaka::Vec{64u}, executor};
        auto first = queue.enqueue(spec, Work{}, output);
        auto second = queue.enqueue(spec, Work{}, output);
        alpaka::onHost::memcpy(raw, host, output);
        alpaka::onHost::wait(raw);
        if(host[0u] != 512u || queue.getMeasurements().size() != 2u)
            throw std::runtime_error{"Native collection replayed or lost a launch"};
        for(auto measurement : {first, second})
        {
            auto result = std::async(std::launch::async, [measurement] { return measurement.getResults(); }).get();
            auto const& counter = result.getMetric(event);
            if(!counter.isAvailable())
                throw std::runtime_error{"Native device collection failed: " + counter.diagnostic};
            if(counter.asDouble() <= 0.0 || !result.getMetric("elapsed_time").isAvailable() || !result.synchronized
               || result.replayed || result.passCount != 1u
               || (counter.descriptor.scope != alpakaMetrics::MetricScope::context
                   && counter.descriptor.scope != alpakaMetrics::MetricScope::device))
                throw std::runtime_error{"Native result lost its counter value or measurement semantics"};
            if(measurement.getResults().getMetric(event).asDouble() != counter.asDouble())
                throw std::runtime_error{"Native counter snapshot changed on a repeated read"};
            std::cout << counter.descriptor.nativeName << " = " << counter.asDouble() << '\n';
        }
    }
} // namespace

int main()
{
    try
    {
        bool tested{};
#if ALPAKA_LANG_CUDA
        if(auto const* event = std::getenv("ALPAKA_METRICS_TEST_CUDA_EVENT"))
        {
            testDevice(alpaka::api::cuda, alpaka::deviceKind::nvidiaGpu, alpaka::exec::gpuCuda, event);
            tested = true;
        }
#else
        if(std::getenv("ALPAKA_METRICS_TEST_CUDA_EVENT"))
            throw std::runtime_error{"CUDA hardware test requested without an enabled CUDA backend"};
#endif
#if ALPAKA_LANG_HIP
        if(auto const* event = std::getenv("ALPAKA_METRICS_TEST_HIP_EVENT"))
        {
            testDevice(alpaka::api::hip, alpaka::deviceKind::amdGpu, alpaka::exec::gpuHip, event);
            tested = true;
        }
#else
        if(std::getenv("ALPAKA_METRICS_TEST_HIP_EVENT"))
            throw std::runtime_error{"HIP hardware test requested without an enabled HIP backend"};
#endif
        if(!tested)
        {
            std::cout << "Set ALPAKA_METRICS_TEST_CUDA_EVENT or ALPAKA_METRICS_TEST_HIP_EVENT to a positive "
                         "single-pass execution counter on device 0 to run hardware validation\n";
            return 77;
        }
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
