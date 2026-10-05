// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "alpakaMetrics/HostSideInstrumentation.hpp"
#include "alpakaMetrics/Measurement.hpp"
#include "alpakaMetrics/internal/Papi.hpp"

#include <alpaka/alpaka.hpp>

#include <exception>
#include <future>

namespace alpakaMetrics::internal
{
    namespace concepts
    {
        /** Queue capability contract, open to external adapters. */
        template<typename T>
        concept Queue = requires(T const& queue) {
            typename T::element_type;
            { queue.get() };
            { queue.getDevice() };
            { queue.getApi() } -> alpaka::concepts::Api;
            { queue.getQueueKind() } -> alpaka::concepts::QueueKind;
            { queue.getTiming() } -> alpaka::concepts::Timing;
        };
    } // namespace concepts

    struct Enqueue
    {
        template<typename T_Api>
        struct Op;
    };

    inline Config elapsedOnly(Config const& config)
    {
        Config result;
        result.label = config.label;
        result.metrics.clear();
        for(auto const& metric : config.metrics)
            if(metric.name == "elapsed_time")
                result.metrics.push_back(metric);
        return result;
    }

    inline void restoreUnsupportedCounters(Result& result, Config const& config, std::string diagnostic)
    {
        auto elapsed = std::move(result.metrics);
        result.metrics.clear();
        for(auto const& request : config.metrics)
        {
            if(request.name == "elapsed_time")
                result.metrics.push_back(elapsed.at(0u));
            else
                result.metrics.push_back(makeUnavailable(request, MetricStatus::unsupportedScope, diagnostic));
        }
    }

    template<concepts::Queue T_Queue, typename T_Task>
    requires std::invocable<T_Task const&>
    Measurement enqueueHostTask(T_Queue const& queue, Config config, T_Task task)
    {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future().share();
        auto state = std::make_shared<MeasurementState>();
        state->id = nextMeasurementId();
        state->isComplete = [future] { return future.wait_for(std::chrono::seconds{0}) == std::future_status::ready; };
        state->read = [future] { return future.get(); };
        config.counterScope = MetricScope::callingThread;
        queue.enqueueHostFn(
            [promise, task, config, id = state->id]
            {
                try
                {
                    HostSideInstrumentation instrumentation{config};
                    instrumentation.begin();
                    try
                    {
                        task();
                    }
                    catch(...)
                    {
                        static_cast<void>(instrumentation.end());
                        throw;
                    }
                    auto result = instrumentation.end();
                    result.measurementId = id;
                    for(auto& metric : result.metrics)
                        if(metric.descriptor.scope == MetricScope::callingThread)
                            metric.descriptor.scope = MetricScope::queueWorkerThread;
                    promise->set_value(std::move(result));
                }
                catch(...)
                {
                    promise->set_exception(std::current_exception());
                }
            });
        return Measurement{std::move(state)};
    }

    template<concepts::Queue T_Queue, typename T_Launch>
    Measurement enqueueHost(T_Queue const& queue, Config const& config, bool countersSupported, T_Launch launch)
    {
        struct Region
        {
            HostSideInstrumentation instrumentation;
            std::promise<Result> promise;
            std::exception_ptr failure;

            explicit Region(Config config) : instrumentation{std::move(config)}
            {
            }
        };

        auto workerConfig = countersSupported ? config : elapsedOnly(config);
        workerConfig.counterScope = MetricScope::callingThread;
        auto region = std::make_shared<Region>(std::move(workerConfig));
        auto future = region->promise.get_future().share();
        auto state = std::make_shared<MeasurementState>();
        state->id = nextMeasurementId();
        state->isComplete = [future] { return future.wait_for(std::chrono::seconds{0}) == std::future_status::ready; };
        state->read = [future] { return future.get(); };
        queue.enqueueHostFn(
            [region]
            {
                try
                {
                    region->instrumentation.begin();
                }
                catch(...)
                {
                    region->failure = std::current_exception();
                }
            });
        try
        {
            launch();
        }
        catch(...)
        {
            auto failure = std::current_exception();
            queue.enqueueHostFn(
                [region, failure]
                {
                    if(region->instrumentation.isActive())
                        static_cast<void>(region->instrumentation.end());
                    region->promise.set_exception(failure);
                });
            throw;
        }
        queue.enqueueHostFn(
            [region, config, countersSupported, id = state->id]
            {
                try
                {
                    if(region->failure)
                        std::rethrow_exception(region->failure);
                    auto result = region->instrumentation.end();
                    result.measurementId = id;
                    for(auto& metric : result.metrics)
                    {
                        if(metric.descriptor.name == "elapsed_time")
                            metric.descriptor.scope = MetricScope::queueInterval;
                        else if(metric.descriptor.scope == MetricScope::callingThread)
                            metric.descriptor.scope = MetricScope::queueWorkerThread;
                        if(metric.descriptor.name == "elapsed_time")
                            metric.descriptor.description
                                = "Execution interval between host queue markers; excludes preceding queue delay";
                    }
                    if(!countersSupported)
                        restoreUnsupportedCounters(
                            result,
                            config,
                            "CPU queue counters require an explicit CpuSerial executor; worker-team counting is not "
                            "implemented");
                    region->promise.set_value(std::move(result));
                }
                catch(...)
                {
                    region->promise.set_exception(std::current_exception());
                }
            });
        return Measurement{std::move(state)};
    }

    template<>
    struct Enqueue::Op<alpaka::api::Host>
    {
        template<
            concepts::Queue T_Queue,
            alpaka::onHost::concepts::ThreadOrFrameSpec T_Spec,
            alpaka::concepts::KernelBundle T_Bundle>
        Measurement operator()(T_Queue const& queue, Config const& config, T_Spec const& spec, T_Bundle const& bundle)
            const
        {
            constexpr bool serial = std::same_as<decltype(spec.getExecutor()), alpaka::exec::CpuSerial>;
            return enqueueHost(queue, config, serial, [&] { queue.enqueue(spec, bundle); });
        }
    };

    template<concepts::Queue T_Queue, typename T_Launch>
    requires std::invocable<T_Launch const&>
    Measurement enqueueDevice(
        T_Queue const& queue,
        Config const& config,
        T_Launch launch,
        PapiCounters* counters = nullptr)
    {
        if constexpr(std::same_as<decltype(queue.getTiming()), alpaka::timing::Enabled>)
        {
            auto start = queue.makeEvent();
            auto end = queue.makeEvent();
            auto state = std::make_shared<MeasurementState>();
            state->id = nextMeasurementId();
            bool synchronized = false;
            try
            {
                if(counters && counters->hasEvents())
                {
                    // PAPI GPU sets are thread-affine and cover a context/device, not a stream.
                    // Finish preceding queue work before opening the counter interval.
                    alpaka::onHost::wait(queue);
                    counters->begin();
                    synchronized = counters->isRunning();
                }
                queue.enqueue(start);
                launch();
                queue.enqueue(end);
                if(synchronized)
                    alpaka::onHost::wait(end);
            }
            catch(...)
            {
                // A launch can fail after partially submitting work. Complete it before
                // dismantling the profiling session, and preserve the original exception.
                if(counters && counters->isRunning())
                {
                    try
                    {
                        alpaka::onHost::wait(queue);
                        static_cast<void>(counters->end());
                    }
                    catch(...)
                    {
                    }
                }
                throw;
            }
            std::vector<MetricResult> counterResults;
            if(counters)
                counterResults = counters->end();
            else
                for(auto const& request : config.metrics)
                    if(request.name != "elapsed_time")
                        counterResults.push_back(makeUnavailable(
                            request,
                            MetricStatus::unsupportedScope,
                            "This queue backend has no native counter provider"));
            state->isComplete = [end] { return end.isComplete(); };
            // Capture values, never a live PAPI event set. Results can be read on any thread.
            state->read
                = [start, end, config, counterResults = std::move(counterResults), id = state->id, synchronized]
            {
                // Also wait for counter-only measurements on the asynchronous fallback path.
                alpaka::onHost::wait(end);
                Result result;
                result.measurementId = id;
                result.label = config.label;
                result.synchronized = synchronized;
                std::size_t counterIndex = 0u;
                for(auto const& request : config.metrics)
                {
                    if(request.name == "elapsed_time")
                        result.metrics.push_back(
                            {{request.name,
                              {},
                              "Execution interval between device queue markers",
                              "alpaka_event",
                              MetricUnit::seconds,
                              MetricScope::queueInterval},
                             MetricStatus::available,
                             alpaka::onHost::getElapsedTime(start, end).count(),
                             {}});
                    else
                        result.metrics.push_back(counterResults.at(counterIndex++));
                }
                return result;
            };
            return Measurement{std::move(state)};
        }
        else
            throw std::invalid_argument{"Device profiling requires a timing-enabled Alpaka queue"};
    }

    template<concepts::Queue T_Queue, typename T_Launch>
    requires std::invocable<T_Launch const&>
    Measurement enqueuePapiDevice(
        T_Queue const& queue,
        Config const& config,
        T_Launch launch,
        std::string_view component)
    {
        // CUDA/HIP execute native functions on the submit thread and select the queue's
        // device first. Do not call profiling APIs from a GPU host callback.
        queue.enqueueNativeFn([](auto) {});
        auto const device = alpaka::onHost::getNativeHandle(queue.getDevice());
        PapiCounters counters{config, DeviceCounterTarget{component, static_cast<std::uint32_t>(device)}};
        return enqueueDevice(queue, config, std::move(launch), &counters);
    }

    /** Portable timing fallback for APIs without a native counter provider. */
    template<typename T_Api>
    struct Enqueue::Op
    {
        template<
            concepts::Queue T_Queue,
            alpaka::onHost::concepts::ThreadOrFrameSpec T_Spec,
            alpaka::concepts::KernelBundle T_Bundle>
        Measurement operator()(T_Queue const& queue, Config const& config, T_Spec const& spec, T_Bundle const& bundle)
            const
        {
            return enqueueDevice(queue, config, [&] { queue.enqueue(spec, bundle); });
        }
    };

    template<>
    struct Enqueue::Op<alpaka::api::Cuda>
    {
        template<
            concepts::Queue T_Queue,
            alpaka::onHost::concepts::ThreadOrFrameSpec T_Spec,
            alpaka::concepts::KernelBundle T_Bundle>
        Measurement operator()(T_Queue const& queue, Config const& config, T_Spec const& spec, T_Bundle const& bundle)
            const
        {
            return enqueuePapiDevice(queue, config, [&] { queue.enqueue(spec, bundle); }, "cuda");
        }
    };

    template<>
    struct Enqueue::Op<alpaka::api::Hip>
    {
        template<
            concepts::Queue T_Queue,
            alpaka::onHost::concepts::ThreadOrFrameSpec T_Spec,
            alpaka::concepts::KernelBundle T_Bundle>
        Measurement operator()(T_Queue const& queue, Config const& config, T_Spec const& spec, T_Bundle const& bundle)
            const
        {
            return enqueuePapiDevice(queue, config, [&] { queue.enqueue(spec, bundle); }, "rocp_sdk");
        }
    };
} // namespace alpakaMetrics::internal
