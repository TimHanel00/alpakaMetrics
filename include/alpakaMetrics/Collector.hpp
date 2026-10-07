// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Result.hpp>
#include <alpakaMetrics/plugin/Collector.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#if defined(__unix__) || defined(__APPLE__)
#    include <dlfcn.h>
#endif

namespace alpakaMetrics
{
    struct CollectorInfo
    {
        std::string name;
        std::string version;
        std::string path;
        bool requiresDeviceSynchronization{};
    };

    /** Empty path uses the environment override or the default collector search.
     * Throws if unavailable or incompatible. Discovery does not guarantee collectibility.
     */
    inline CollectorInfo getCollectorInfo(std::string const& path = {});
    inline std::vector<MetricDescriptor> getAvailableMetrics(std::string const& path = {});
} // namespace alpakaMetrics

namespace alpakaMetrics::internal::collector
{
    inline constexpr char moduleAnchor{};
#if defined(ALPAKA_METRICS_INSTALL_LIBDIR)
    inline constexpr auto installLibDir = ALPAKA_METRICS_INSTALL_LIBDIR;
#else
    inline constexpr auto installLibDir = "lib";
#endif
    class CollectorUnavailable : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    struct Module
    {
        plugin::Collector const& api;
        CollectorInfo info;
    };

    inline std::string pluginPath(std::string const& requested)
    {
        if(!requested.empty())
            return std::filesystem::absolute(requested).lexically_normal().string();
        if(auto const* overridePath = std::getenv("ALPAKA_METRICS_COLLECTOR_PLUGIN"))
            if(*overridePath)
                return std::filesystem::absolute(overridePath).lexically_normal().string();
#if defined(__unix__) || defined(__APPLE__)
        Dl_info info{};
        if(dladdr(&moduleAnchor, &info) && info.dli_fname)
        {
            auto const directory = std::filesystem::absolute(info.dli_fname).parent_path();
            constexpr auto name = "libalpakaMetrics_papi.so";
            for(auto const& candidate :
                {directory / name, directory.parent_path() / name, directory.parent_path() / installLibDir / name})
                if(std::filesystem::exists(candidate))
                    return candidate.lexically_normal().string();
            // Let the platform loader search its configured library paths.
            return name;
        }
#endif
        throw CollectorUnavailable{"Runtime collector loading is unavailable on this platform"};
    }

#if defined(__unix__) || defined(__APPLE__)
    struct CloseModule
    {
        void operator()(void* handle) const noexcept
        {
            dlclose(handle);
        }
    };
#endif

    inline Module const& loadModule(std::string const& requested)
    {
        auto const selected = pluginPath(requested);
        auto const path = std::filesystem::path{selected}.has_parent_path()
                              ? std::filesystem::absolute(selected).lexically_normal().string()
                              : selected;
        static std::mutex mutex;
        static std::map<std::string, Module> modules;
        std::lock_guard lock{mutex};
        if(auto const it = modules.find(path); it != modules.end())
            return it->second;
#if defined(__unix__) || defined(__APPLE__)
        std::unique_ptr<void, CloseModule> handle{dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)};
        if(!handle)
            throw CollectorUnavailable{"Cannot load collector " + path + ": " + dlerror()};
        auto entry = reinterpret_cast<plugin::Entry>(dlsym(handle.get(), "alpakaMetrics_getCollector"));
        auto const* api = entry ? entry() : nullptr;
        if(!api || api->abiVersion != plugin::collectorAbiVersion || api->structSize < sizeof(plugin::Collector)
           || !api->name || !api->version || !api->create || !api->destroy || !api->begin || !api->hasEvents
           || !api->isRunning || !api->end || !api->discover)
        {
            throw std::runtime_error{"Incompatible collector ABI: " + path};
        }
        Dl_info loaded{};
        auto const resolved = dladdr(api, &loaded) && loaded.dli_fname
                                  ? std::filesystem::absolute(loaded.dli_fname).lexically_normal().string()
                                  : path;
        auto const it = modules
                            .emplace(
                                path,
                                Module{
                                    *api,
                                    {api->name,
                                     api->version,
                                     resolved,
                                     (api->capabilities & plugin::requiresDeviceSynchronization) != 0}})
                            .first;
        // Keep loaded modules resident for process-global state and thread-local teardown.
        static_cast<void>(handle.release());
        return it->second;
#else
        throw CollectorUnavailable{"Runtime collector loading is unavailable on this platform"};
#endif
    }

    struct Receiver
    {
        std::vector<MetricResult> metrics;
        std::exception_ptr failure;
        CollectorInfo info;

        static void emit(void* context, plugin::Metric const* source) noexcept
        {
            auto& receiver = *static_cast<Receiver*>(context);
            if(receiver.failure)
                return;
            try
            {
                if(!source || !source->name || !source->nativeName || !source->description || !source->provider
                   || !source->nativeUnit || !source->diagnostic
                   || source->unit > static_cast<std::uint32_t>(MetricUnit::providerDefined)
                   || source->scope > static_cast<std::uint32_t>(MetricScope::providerDefined)
                   || source->status > static_cast<std::uint32_t>(MetricStatus::collectionFailed)
                   || source->valueKind > 3)
                    throw std::runtime_error{"Invalid collector result"};
                MetricResult result{
                    {source->name,
                     source->nativeName,
                     source->description,
                     source->provider,
                     static_cast<MetricUnit>(source->unit),
                     static_cast<MetricScope>(source->scope),
                     source->nativeUnit,
                     source->scale},
                    static_cast<MetricStatus>(source->status),
                    {},
                    source->diagnostic};
                result.descriptor.collector = receiver.info.name;
                result.descriptor.collectorVersion = receiver.info.version;
                result.descriptor.collectorPath = receiver.info.path;
                switch(source->valueKind)
                {
                case 1:
                    result.value = source->signedValue;
                    break;
                case 2:
                    result.value = source->unsignedValue;
                    break;
                case 3:
                    result.value = source->doubleValue;
                    break;
                default:
                    break;
                }
                receiver.metrics.push_back(std::move(result));
            }
            catch(...)
            {
                receiver.failure = std::current_exception();
            }
        }
    };
} // namespace alpakaMetrics::internal::collector

namespace alpakaMetrics
{
    inline CollectorInfo getCollectorInfo(std::string const& path)
    {
        return internal::collector::loadModule(path).info;
    }

    inline std::vector<MetricDescriptor> getAvailableMetrics(std::string const& path)
    {
        auto const& collectorModule = internal::collector::loadModule(path);
        internal::collector::Receiver receiver{{}, {}, collectorModule.info};
        collectorModule.api.discover(internal::collector::Receiver::emit, &receiver);
        if(receiver.failure)
            std::rethrow_exception(receiver.failure);
        std::vector<MetricDescriptor> descriptors;
        for(auto& metric : receiver.metrics)
            descriptors.push_back(std::move(metric.descriptor));
        return descriptors;
    }
} // namespace alpakaMetrics
