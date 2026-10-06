// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Config.hpp>
#include <alpakaMetrics/Result.hpp>

#include <memory>
#include <string_view>

namespace alpakaMetrics::papi
{
    struct DeviceCounterTarget
    {
        std::string_view component;
        std::uint32_t device{};
    };

    class PapiSession
    {
    public:
        explicit PapiSession(Config const& config, std::optional<DeviceCounterTarget> target = std::nullopt);
        ~PapiSession();
        PapiSession(PapiSession const&) = delete;
        PapiSession& operator=(PapiSession const&) = delete;
        void begin();
        [[nodiscard]] bool hasEvents() const;
        [[nodiscard]] bool isRunning() const;
        std::vector<MetricResult> end();
        static std::vector<MetricDescriptor> getAvailableMetrics();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    inline MetricResult makeUnavailable(MetricRequest const& request, MetricStatus status, std::string diagnostic)
    {
        return {
            {request.name,
             request.nativeName,
             {},
             "PAPI",
             request.unit,
             MetricScope::providerDefined,
             {},
             request.scale},
            status,
            {},
            std::move(diagnostic)};
    }
} // namespace alpakaMetrics::papi
