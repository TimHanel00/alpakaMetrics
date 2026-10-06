// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Config.hpp>
#include <alpakaMetrics/Result.hpp>

#include <memory>
#include <string_view>

namespace alpakaMetrics::internal
{
    struct DeviceCounterTarget
    {
        std::string_view component;
        std::uint32_t device{};
    };

    class Counters
    {
    public:
        explicit Counters(Config const& config, std::optional<DeviceCounterTarget> target = std::nullopt);
        ~Counters();
        Counters(Counters const&) = delete;
        Counters& operator=(Counters const&) = delete;
        void begin();
        [[nodiscard]] bool hasEvents() const;
        [[nodiscard]] bool isRunning() const;
        std::vector<MetricResult> end();
        static std::vector<MetricDescriptor> getAvailableMetrics();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    std::uint64_t nextMeasurementId();
    void validateConfig(Config const& config);
    MetricResult makeUnavailable(MetricRequest const& request, MetricStatus status, std::string diagnostic);
} // namespace alpakaMetrics::internal
