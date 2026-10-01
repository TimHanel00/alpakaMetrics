// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "alpakaMetrics/internal/interface.hpp"

#include <mutex>
#include <vector>

namespace alpakaMetrics
{
    template<internal::concepts::Queue T_Queue>
    class Queue
    {
        struct State
        {
            T_Queue queue;
            Config config;
            mutable std::mutex mutex;
            std::vector<Measurement> measurements;

            State(T_Queue queue, Config config) : queue{std::move(queue)}, config{std::move(config)}
            {
            }
        };

    public:
        using element_type = typename T_Queue::element_type;

        Queue(T_Queue queue, Config config) : m_state{std::make_shared<State>(std::move(queue), std::move(config))}
        {
            internal::validateConfig(m_state->config);
            if constexpr(!std::same_as<decltype(m_state->queue.getApi()), alpaka::api::Host>)
                if constexpr(!std::same_as<decltype(m_state->queue.getTiming()), alpaka::timing::Enabled>)
                    throw std::invalid_argument{"Device profiling requires a timing-enabled Alpaka queue"};
        }

        [[nodiscard]] auto get() const
        {
            return m_state->queue.get();
        }

        [[nodiscard]] auto getDevice() const
        {
            return m_state->queue.getDevice();
        }

        [[nodiscard]] constexpr auto getApi() const
        {
            return m_state->queue.getApi();
        }

        [[nodiscard]] constexpr auto getDeviceKind() const
        {
            return m_state->queue.getDeviceKind();
        }

        [[nodiscard]] constexpr auto getQueueKind() const
        {
            return m_state->queue.getQueueKind();
        }

        [[nodiscard]] constexpr auto getTiming() const
        {
            return m_state->queue.getTiming();
        }

        [[nodiscard]] auto getPolicyList() const requires requires(T_Queue queue) { queue.getPolicyList(); }
        {
            return m_state->queue.getPolicyList();
        }

        [[nodiscard]] auto getNativeHandle() const
        {
            return m_state->queue.getNativeHandle();
        }

        [[nodiscard]] auto getName() const
        {
            return m_state->queue.getName();
        }

        [[nodiscard]] T_Queue const& getUnderlyingQueue() const
        {
            return m_state->queue;
        }

        auto makeEvent(auto... policies) const
        {
            return m_state->queue.makeEvent(policies...);
        }

        bool operator==(Queue const& other) const
        {
            return m_state->queue == other.m_state->queue;
        }

        /** Exactly one kernel submission; returns a stable measurement handle. */
        template<alpaka::onHost::concepts::ThreadOrFrameSpec T_Spec, alpaka::concepts::KernelBundle T_Bundle>
        Measurement enqueue(T_Spec const& spec, T_Bundle const& bundle) const
        {
            std::lock_guard lock{m_state->mutex};
            auto measurement
                = internal::Enqueue::Op<decltype(getApi())>{}(m_state->queue, m_state->config, spec, bundle);
            m_state->measurements.push_back(measurement);
            return measurement;
        }

        Measurement enqueue(
            alpaka::onHost::concepts::ThreadOrFrameSpec auto const& spec,
            alpaka::concepts::KernelFn auto const& kernel,
            auto&&... args) const
        {
            return enqueue(
                spec,
                alpaka::KernelBundle{kernel, alpaka::onHost::makeAccessibleOnAcc(ALPAKA_FORWARD(args))...});
        }

        /** Synchronization events are forwarded without creating measurements. */
        void enqueue(auto const& event) const requires requires(T_Queue queue) { queue.enqueue(event); }
        {
            std::lock_guard lock{m_state->mutex};
            m_state->queue.enqueue(event);
        }

        Measurement enqueueHostFn(auto const& task) const
        {
            std::lock_guard lock{m_state->mutex};
            auto measurement = internal::enqueueHostTask(m_state->queue, m_state->config, task);
            m_state->measurements.push_back(measurement);
            return measurement;
        }

        void enqueueHostFnDeferred(auto const& task) const
        {
            std::lock_guard lock{m_state->mutex};
            m_state->queue.enqueueHostFnDeferred(task);
        }

        void enqueueNativeFn(auto const& task) const
        {
            std::lock_guard lock{m_state->mutex};
            m_state->queue.enqueueNativeFn(task);
        }

        [[nodiscard]] std::vector<Measurement> getMeasurements() const
        {
            std::lock_guard lock{m_state->mutex};
            return m_state->measurements;
        }

        /** Drops queue-owned result handles; handles retained by callers remain valid. */
        void clearMeasurements() const
        {
            std::lock_guard lock{m_state->mutex};
            m_state->measurements.clear();
        }

    private:
        alpaka::onHost::Handle<State> m_state;
    };

    auto makeQueue(internal::concepts::Queue auto queue, Config config = {})
    {
        return Queue<decltype(queue)>{std::move(queue), std::move(config)};
    }
} // namespace alpakaMetrics
