// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/internal/PluginBridge.hpp>
#include <alpakaMetrics/plugin/papi/PapiSession.hpp>
#include <papi.h>

namespace
{
    using namespace alpakaMetrics;

    void* create(plugin::Request const* requests, std::size_t size, plugin::Options const* options) noexcept
    {
        try
        {
            Config config;
            config.metrics.clear();
            config.counterScope = static_cast<MetricScope>(options->scope);
            for(std::size_t i = 0; i < size; ++i)
            {
                MetricRequest request{metric::native(
                    requests[i].nativeName,
                    static_cast<MetricUnit>(requests[i].unit),
                    requests[i].scale)};
                request.name = requests[i].name;
                config.metrics.push_back(std::move(request));
            }
            std::optional<papi::DeviceCounterTarget> target;
            std::string_view api{options->deviceApi};
            if(!api.empty())
                target = papi::DeviceCounterTarget{api == "hip" ? "rocp_sdk" : api, options->device};
            return new papi::PapiSession{config, target};
        }
        catch(...)
        {
            return nullptr;
        }
    }

    void destroy(void* state) noexcept
    {
        delete static_cast<papi::PapiSession*>(state);
    }

    void begin(void* state) noexcept
    {
        // PAPI calls record errors per metric; exceptions never cross the ABI.
        try
        {
            static_cast<papi::PapiSession*>(state)->begin();
        }
        catch(...)
        {
        }
    }

    bool hasEvents(void const* state) noexcept
    {
        return static_cast<papi::PapiSession const*>(state)->hasEvents();
    }

    bool isRunning(void const* state) noexcept
    {
        return static_cast<papi::PapiSession const*>(state)->isRunning();
    }

    void end(void* state, plugin::Emit emit, void* context) noexcept
    {
        try
        {
            for(auto const& result : static_cast<papi::PapiSession*>(state)->end())
                internal::emitMetric(result, emit, context);
        }
        catch(...)
        {
        }
    }

    void discover(plugin::Emit emit, void* context) noexcept
    {
        try
        {
            for(auto const& descriptor : papi::PapiSession::getAvailableMetrics())
                internal::emitMetric({descriptor, MetricStatus::unsupported, {}, {}}, emit, context);
        }
        catch(...)
        {
        }
    }

    std::string const version = std::to_string(PAPI_VERSION_MAJOR(PAPI_VERSION)) + "."
                                + std::to_string(PAPI_VERSION_MINOR(PAPI_VERSION)) + "."
                                + std::to_string(PAPI_VERSION_REVISION(PAPI_VERSION));
    plugin::Collector const collector{
        plugin::collectorAbiVersion,
        sizeof(plugin::Collector),
        "PAPI",
        version.c_str(),
        plugin::requiresDeviceSynchronization,
        create,
        destroy,
        begin,
        hasEvents,
        isRunning,
        end,
        discover};
} // namespace

extern "C" alpakaMetrics::plugin::Collector const* alpakaMetrics_getCollector() noexcept
{
    return &collector;
}
