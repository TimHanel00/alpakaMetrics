// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Collector.hpp>
#include <alpakaMetrics/internal/Config.hpp>

#include <algorithm>
#include <optional>
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
        std::optional<collector::Module> m_module;
        // Opaque C ABI resource; owned and destroyed through the selected collector.
        void* m_session{};
        std::vector<MetricResult> m_results;
    };

    inline Counters::Counters(Config const& config, std::optional<DeviceCounterTarget> target)
    {
        std::vector<plugin::Request> requests;
        for(auto const& request : config.metrics)
            if(request.name != "elapsed_time")
            {
                requests.push_back(
                    {request.name.c_str(),
                     request.getNativeName().c_str(),
                     static_cast<std::uint32_t>(request.unit),
                     request.scale});
                m_results.push_back(
                    makeUnavailable(request, MetricStatus::collectionFailed, "Collector did not return this metric"));
            }
        if(requests.empty())
            return;
        try
        {
            m_module.emplace(collector::loadConfiguredModule(config.collectorPlugin, config.defaultCollectorPlugin));
            if(!m_module->api)
            {
                for(auto& result : m_results)
                {
                    result.status = MetricStatus::unsupportedScope;
                    result.diagnostic = "Asynchronous collector requires a tracked queue operation";
                }
                return;
            }
            // Dual-interface modules may use a different identity/capability set for legacy regions.
            m_module->info.name = m_module->api->name;
            m_module->info.version = m_module->api->version;
            if(target && !config.allowSynchronization
               && (m_module->api->capabilities & plugin::requiresDeviceSynchronization) != 0)
            {
                for(auto& result : m_results)
                {
                    result.status = MetricStatus::unsupportedScope;
                    result.diagnostic = "Collector requires device synchronization; opt in with allowSynchronization";
                }
                return;
            }
            std::string api = target ? std::string{target->component} : std::string{};
            plugin::Options options{
                static_cast<std::uint32_t>(config.counterScope),
                api.c_str(),
                target ? target->device : 0};
            m_session = m_module->api->create(requests.data(), requests.size(), &options);
            if(!m_session)
                throw std::runtime_error{"Collector session creation failed"};
        }
        catch(std::exception const& error)
        {
            bool const explicitlySelected
                = !config.collectorPlugin.empty() || !config.defaultCollectorPlugin.empty()
                  || collector::hasEnvironmentSelection("ALPAKA_METRICS_COLLECTOR_PLUGIN")
                  || collector::hasEnvironmentSelection("ALPAKA_METRICS_DEFAULT_COLLECTOR_PLUGIN");
            for(auto& result : m_results)
            {
                result.status = !explicitlySelected && dynamic_cast<collector::CollectorUnavailable const*>(&error)
                                    ? MetricStatus::dependencyDisabled
                                    : MetricStatus::collectionFailed;
                result.diagnostic = error.what();
            }
        }
    }

    inline Counters::~Counters()
    {
        if(m_session)
            m_module->api->destroy(m_session);
    }

    inline void Counters::begin()
    {
        if(m_session)
            m_module->api->begin(m_session);
    }

    inline bool Counters::hasEvents() const
    {
        return m_session && m_module->api->hasEvents(m_session);
    }

    inline bool Counters::isRunning() const
    {
        return m_session && m_module->api->isRunning(m_session);
    }

    inline std::vector<MetricResult> Counters::end()
    {
        if(m_session)
        {
            collector::Receiver receiver{{}, {}, m_module->info};
            m_module->api->end(m_session, collector::Receiver::emit, &receiver);
            if(receiver.failure)
                for(auto& result : m_results)
                    result.diagnostic = "Invalid collector result";
            else
                for(auto& result : m_results)
                {
                    auto const found = std::find_if(
                        receiver.metrics.begin(),
                        receiver.metrics.end(),
                        [&](auto const& metric) { return metric.descriptor.name == result.descriptor.name; });
                    if(found != receiver.metrics.end())
                        result = *found;
                }
        }
        if(m_module)
            for(auto& result : m_results)
            {
                result.descriptor.collector = m_module->info.name;
                result.descriptor.collectorVersion = m_module->info.version;
                result.descriptor.collectorPath = m_module->info.path;
            }
        return m_results;
    }

    inline std::vector<MetricDescriptor> Counters::getAvailableMetrics()
    {
        try
        {
            return alpakaMetrics::getAvailableMetrics();
        }
        catch(std::exception const&)
        {
            return {};
        }
    }
} // namespace alpakaMetrics::internal
