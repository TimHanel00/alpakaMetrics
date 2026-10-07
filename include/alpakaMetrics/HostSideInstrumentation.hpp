// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/internal/Counters.hpp>

#include <mutex>
#include <optional>
#include <thread>

namespace alpakaMetrics
{
    /** A region on the calling host thread. begin/end must use the same thread.
     * Asynchronous kernels are included only if the application waits before end().
     * CPU thread counters exclude work performed by other threads.
     */
    class HostSideInstrumentation
    {
    public:
        explicit HostSideInstrumentation(Config config = {}) : m_config{std::move(config)}
        {
            internal::validateConfig(m_config);
        }

        HostSideInstrumentation(HostSideInstrumentation const&) = delete;
        HostSideInstrumentation& operator=(HostSideInstrumentation const&) = delete;
        HostSideInstrumentation(HostSideInstrumentation&&) = delete;
        HostSideInstrumentation& operator=(HostSideInstrumentation&&) = delete;

        void begin()
        {
            std::lock_guard lock{m_mutex};
            if(m_active)
                throw std::logic_error{"Host instrumentation is already active"};
            m_counters.emplace(m_config);
            m_counters->begin();
            m_result = {};
            m_result.measurementId = internal::nextMeasurementId();
            m_result.label = m_config.label;
            m_result.provenance.api = "Host";
            m_result.begin = std::chrono::steady_clock::now();
            m_thread = std::this_thread::get_id();
            m_active = true;
            m_hasResult = false;
        }

        Result end()
        {
            std::lock_guard lock{m_mutex};
            if(!m_active)
                throw std::logic_error{"Host instrumentation is not active"};
            if(m_thread != std::this_thread::get_id())
                throw std::logic_error{"Host instrumentation must end on its begin thread"};
            m_result.end = std::chrono::steady_clock::now();
            auto counters = m_counters->end();
            std::size_t counterIndex = 0u;
            for(auto const& request : m_config.metrics)
            {
                if(request.name == "elapsed_time")
                    m_result.metrics.push_back(
                        {{request.name,
                          {},
                          "Wall time between begin() and end()",
                          "steady_clock",
                          MetricUnit::seconds,
                          MetricScope::hostRegion},
                         MetricStatus::available,
                         std::chrono::duration<double>{m_result.end - m_result.begin}.count(),
                         {}});
                else
                    m_result.metrics.push_back(std::move(counters.at(counterIndex++)));
            }
            m_counters.reset();
            m_active = false;
            m_hasResult = true;
            return m_result;
        }

        bool isActive() const
        {
            std::lock_guard lock{m_mutex};
            return m_active;
        }

        Result getResults() const
        {
            std::lock_guard lock{m_mutex};
            if(m_active || !m_hasResult)
                throw std::logic_error{"No completed host instrumentation region"};
            return m_result;
        }

        [[nodiscard]] static std::vector<MetricDescriptor> getAvailableMetrics()
        {
            auto result = internal::Counters::getAvailableMetrics();
            result.insert(
                result.begin(),
                {"elapsed_time",
                 {},
                 "Host region wall time",
                 "steady_clock",
                 MetricUnit::seconds,
                 MetricScope::hostRegion});
            return result;
        }

    private:
        Config m_config;
        mutable std::mutex m_mutex;
        std::optional<internal::Counters> m_counters;
        std::thread::id m_thread;
        bool m_active{};
        bool m_hasResult{};
        Result m_result;
    };
} // namespace alpakaMetrics
