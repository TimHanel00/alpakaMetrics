// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Result.hpp>
#include <alpakaMetrics/plugin/Collector.hpp>

#include <concepts>

namespace alpakaMetrics::internal
{
    inline void emitMetric(MetricResult const& result, plugin::Emit emit, void* context)
    {
        auto const& d = result.descriptor;
        plugin::Metric metric{
            d.name.c_str(),
            d.nativeName.c_str(),
            d.description.c_str(),
            d.provider.c_str(),
            static_cast<std::uint32_t>(d.unit),
            static_cast<std::uint32_t>(d.scope),
            d.nativeUnit.c_str(),
            d.nativeToValueScale,
            static_cast<std::uint32_t>(result.status),
            0,
            0,
            0,
            0,
            result.diagnostic.c_str()};
        if(result.value)
            std::visit(
                [&](auto value)
                {
                    using T = decltype(value);
                    if constexpr(std::same_as<T, std::int64_t>)
                    {
                        metric.valueKind = 1;
                        metric.signedValue = value;
                    }
                    else if constexpr(std::same_as<T, std::uint64_t>)
                    {
                        metric.valueKind = 2;
                        metric.unsignedValue = value;
                    }
                    else
                    {
                        metric.valueKind = 3;
                        metric.doubleValue = value;
                    }
                },
                *result.value);
        emit(context, &metric);
    }
} // namespace alpakaMetrics::internal
