// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/HostSideInstrumentation.hpp>
#include <alpakaMetrics/internal/Counters.hpp>

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace alpakaMetrics::internal
{
    std::uint64_t nextMeasurementId()
    {
        static std::atomic<std::uint64_t> next{1u};
        return next.fetch_add(1u, std::memory_order_relaxed);
    }

    void validateConfig(Config const& config)
    {
        if(config.counterScope != MetricScope::providerDefined && config.counterScope != MetricScope::callingThread)
            throw std::invalid_argument{"Counter scope must be providerDefined or callingThread"};
        std::unordered_set<std::string> names;
        for(auto const& request : config.metrics)
        {
            if(request.name.empty() || !names.insert(request.name).second)
                throw std::invalid_argument{"Metric names must be non-empty and unique"};
            if(!std::isfinite(request.scale) || request.scale <= 0.0)
                throw std::invalid_argument{"Metric conversion scale must be finite and positive"};
            if(request.name == "elapsed_time" && (!request.getNativeName().empty() || request.scale != 1.0))
                throw std::invalid_argument{"Elapsed time cannot be rebound to a counter"};
            if(!request.getNativeName().empty())
            {
                auto expected = MetricUnit::count;
                if(request.name == "energy")
                    expected = MetricUnit::joules;
                else if(request.name == "core_frequency")
                    expected = MetricUnit::hertz;
                else if(request.name == "achieved_occupancy")
                    expected = MetricUnit::ratio;
                else if(request.name == "transferred_bytes")
                    expected = MetricUnit::bytes;
                if(request.name != request.getNativeName() && request.unit != MetricUnit::providerDefined
                   && request.unit != expected)
                    throw std::invalid_argument{"Native mapping unit does not match the semantic metric"};
                if(expected != MetricUnit::count && request.unit == MetricUnit::providerDefined)
                    throw std::invalid_argument{"Native mapping requires an explicit semantic unit"};
            }
        }
    }

    MetricResult makeUnavailable(MetricRequest const& request, MetricStatus status, std::string diagnostic)
    {
        return {
            {request.name,
             request.getNativeName(),
             {},
             "none",
             request.unit,
             MetricScope::providerDefined,
             {},
             request.scale},
            status,
            {},
            std::move(diagnostic)};
    }

    class HostInstrumentation
    {
    public:
        explicit HostInstrumentation(Config config) : m_config{std::move(config)}
        {
            validateConfig(m_config);
        }

        void begin()
        {
            std::lock_guard lock{m_mutex};
            if(m_active)
                throw std::logic_error{"Host instrumentation is already active"};
            m_counters = std::make_unique<Counters>(m_config);
            m_counters->begin();
            m_result = {};
            m_result.measurementId = nextMeasurementId();
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

    private:
        Config m_config;
        mutable std::mutex m_mutex;
        std::unique_ptr<Counters> m_counters;
        std::thread::id m_thread;
        bool m_active{};
        bool m_hasResult{};
        Result m_result;
    };
} // namespace alpakaMetrics::internal

namespace alpakaMetrics
{
    HostSideInstrumentation::HostSideInstrumentation(Config config)
        : m_instrumentation{std::make_unique<internal::HostInstrumentation>(std::move(config))}
    {
    }

    HostSideInstrumentation::~HostSideInstrumentation() = default;

    void HostSideInstrumentation::begin()
    {
        m_instrumentation->begin();
    }

    Result HostSideInstrumentation::end()
    {
        return m_instrumentation->end();
    }

    bool HostSideInstrumentation::isActive() const
    {
        return m_instrumentation->isActive();
    }

    Result HostSideInstrumentation::getResults() const
    {
        return m_instrumentation->getResults();
    }

    std::vector<MetricDescriptor> HostSideInstrumentation::getAvailableMetrics()
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
} // namespace alpakaMetrics
