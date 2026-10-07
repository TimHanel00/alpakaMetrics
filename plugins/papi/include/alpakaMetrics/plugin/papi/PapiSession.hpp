// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Config.hpp>
#include <alpakaMetrics/Result.hpp>
#include <papi.h>

#include <atomic>
#include <functional>
#include <optional>
#include <string_view>
#include <thread>

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
        struct Group
        {
            int component{};
            int eventSet{PAPI_NULL};
            int error{PAPI_OK};
            bool running{};
            std::vector<std::size_t> resultIndices;
            std::vector<int> dataTypes;
            std::vector<double> scales;
        };

        struct DeviceLease
        {
            std::optional<std::reference_wrapper<std::atomic_flag>> gate;

            DeviceLease() = default;
            DeviceLease(DeviceLease const&) = delete;
            DeviceLease& operator=(DeviceLease const&) = delete;

            ~DeviceLease()
            {
                if(gate)
                    gate->get().clear(std::memory_order_release);
            }

            bool acquire(std::atomic_flag& candidate)
            {
                if(gate)
                    return true;
                if(candidate.test_and_set(std::memory_order_acquire))
                    return false;
                gate = candidate;
                return true;
            }
        };

        std::vector<MetricResult> m_results;
        std::vector<Group> m_groups;
        DeviceLease m_deviceLease;
        std::thread::id m_owner{std::this_thread::get_id()};
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
