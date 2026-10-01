// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace alpakaMetrics
{
    enum class MetricStatus
    {
        available,
        unsupported,
        dependencyDisabled,
        permissionDenied,
        conflicting,
        unsupportedScope,
        collectionFailed
    };
    enum class MetricScope
    {
        hostRegion,
        callingThread,
        queueWorkerThread,
        queueInterval,
        device,
        context,
        providerDefined
    };
    enum class MetricUnit
    {
        seconds,
        count,
        joules,
        watts,
        hertz,
        ratio,
        bytes,
        providerDefined
    };
    using MetricValue = std::variant<std::int64_t, std::uint64_t, double>;

    struct MetricDescriptor
    {
        std::string name;
        std::string nativeName;
        std::string description;
        std::string provider;
        MetricUnit unit{MetricUnit::providerDefined};
        MetricScope scope{MetricScope::providerDefined};
        std::string nativeUnit;
        double nativeToValueScale{1.0};
    };

    struct MetricResult
    {
        MetricDescriptor descriptor;
        MetricStatus status{MetricStatus::unsupported};
        std::optional<MetricValue> value;
        std::string diagnostic;

        [[nodiscard]] bool isAvailable() const noexcept
        {
            return status == MetricStatus::available && value.has_value();
        }

        [[nodiscard]] double asDouble() const
        {
            if(!isAvailable())
                throw std::logic_error{"Requested metric has no valid value"};
            return std::visit([](auto value) { return static_cast<double>(value); }, *value);
        }
    };

    struct Result
    {
        std::uint64_t measurementId{};
        std::string label;
        std::vector<MetricResult> metrics;
        bool synchronized{};
        bool replayed{};
        std::uint32_t passCount{1u};
        std::chrono::steady_clock::time_point begin;
        std::chrono::steady_clock::time_point end;

        [[nodiscard]] MetricResult const& getMetric(std::string_view name) const
        {
            for(auto const& metric : metrics)
                if(metric.descriptor.name == name)
                    return metric;
            throw std::out_of_range{"Metric was not requested"};
        }
    };
} // namespace alpakaMetrics
