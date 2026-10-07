// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Config.hpp>

#include <atomic>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace alpakaMetrics::internal
{
    inline std::uint64_t nextMeasurementId()
    {
        static std::atomic<std::uint64_t> next{1u};
        return next.fetch_add(1u, std::memory_order_relaxed);
    }

    inline void validateConfig(Config const& config)
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

    inline MetricResult makeUnavailable(MetricRequest const& request, MetricStatus status, std::string diagnostic)
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

} // namespace alpakaMetrics::internal
