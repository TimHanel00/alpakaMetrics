// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/Collector.hpp>
#include <alpakaMetrics/internal/Counters.hpp>
#include <alpakaMetrics/plugin/Collector.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#if defined(__unix__) || defined(__APPLE__)
#    include <dlfcn.h>
#endif

namespace alpakaMetrics
{
    namespace
    {
        class CollectorUnavailable : public std::runtime_error
        {
        public:
            using std::runtime_error::runtime_error;
        };

        struct Module
        {
            plugin::Collector const* api{};
            CollectorInfo info;
        };

        std::string pluginPath(std::string const& requested)
        {
            if(!requested.empty())
                return requested;
            if(auto const* overridePath = std::getenv("ALPAKA_METRICS_COLLECTOR_PLUGIN"))
                if(*overridePath)
                    return overridePath;
#if defined(__unix__) || defined(__APPLE__)
            Dl_info info{};
            if(dladdr(reinterpret_cast<void*>(&getCollectorInfo), &info) && info.dli_fname)
            {
                constexpr auto name = "libalpakaMetrics_papi.so";
                return (std::filesystem::absolute(info.dli_fname).parent_path() / name).string();
            }
#endif
            throw CollectorUnavailable{"Runtime collector loading is unavailable on this platform"};
        }

        std::shared_ptr<Module> loadModule(std::string const& requested)
        {
            auto const path = std::filesystem::absolute(pluginPath(requested)).lexically_normal().string();
            static std::mutex mutex;
            static std::unordered_map<std::string, std::shared_ptr<Module>> modules;
            std::lock_guard lock{mutex};
            if(auto const it = modules.find(path); it != modules.end())
                return it->second;
#if defined(__unix__) || defined(__APPLE__)
            void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if(!handle)
                throw CollectorUnavailable{"Cannot load collector " + path + ": " + dlerror()};
            auto entry = reinterpret_cast<plugin::Entry>(dlsym(handle, "alpakaMetrics_getCollector"));
            auto const* api = entry ? entry() : nullptr;
            if(!api || api->abiVersion != plugin::collectorAbiVersion || api->structSize < sizeof(plugin::Collector)
               || !api->name || !api->version || !api->create || !api->destroy || !api->begin || !api->hasEvents
               || !api->isRunning || !api->end || !api->discover)
            {
                dlclose(handle);
                throw std::runtime_error{"Incompatible collector ABI: " + path};
            }
            auto collectorModule = std::make_shared<Module>();
            collectorModule->api = api;
            collectorModule->info
                = {api->name, api->version, path, (api->capabilities & plugin::requiresDeviceSynchronization) != 0};
            // Keep successfully loaded modules resident: PAPI has thread-local teardown
            // and process-global state, whose lifetimes exceed individual counter sessions.
            modules.emplace(path, collectorModule);
            return collectorModule;
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
    } // namespace

    CollectorInfo getCollectorInfo(std::string const& path)
    {
        return loadModule(path)->info;
    }

    std::vector<MetricDescriptor> getAvailableMetrics(std::string const& path)
    {
        auto collectorModule = loadModule(path);
        Receiver receiver{{}, {}, collectorModule->info};
        collectorModule->api->discover(Receiver::emit, &receiver);
        if(receiver.failure)
            std::rethrow_exception(receiver.failure);
        std::vector<MetricDescriptor> descriptors;
        for(auto& metric : receiver.metrics)
            descriptors.push_back(std::move(metric.descriptor));
        return descriptors;
    }

    namespace internal
    {
        struct Counters::Impl
        {
            std::shared_ptr<Module> collectorModule;
            void* session{};
            std::vector<MetricResult> results;
        };

        Counters::Counters(Config const& config, std::optional<DeviceCounterTarget> target)
            : m_impl{std::make_unique<Impl>()}
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
                    m_impl->results.push_back(makeUnavailable(
                        request,
                        MetricStatus::collectionFailed,
                        "Collector did not return this metric"));
                }
            if(requests.empty())
                return;
            try
            {
                m_impl->collectorModule = loadModule(config.collectorPlugin);
                if(target && !config.allowSynchronization
                   && m_impl->collectorModule->info.requiresDeviceSynchronization)
                {
                    for(auto& result : m_impl->results)
                    {
                        result.status = MetricStatus::unsupportedScope;
                        result.diagnostic
                            = "Collector requires device synchronization; opt in with allowSynchronization";
                    }
                    return;
                }
                std::string api = target ? std::string{target->component} : std::string{};
                plugin::Options options{
                    static_cast<std::uint32_t>(config.counterScope),
                    api.c_str(),
                    target ? target->device : 0};
                m_impl->session = m_impl->collectorModule->api->create(requests.data(), requests.size(), &options);
                if(!m_impl->session)
                    throw std::runtime_error{"Collector session creation failed"};
            }
            catch(std::exception const& error)
            {
                bool const explicitlySelected
                    = !config.collectorPlugin.empty() || std::getenv("ALPAKA_METRICS_COLLECTOR_PLUGIN");
                for(auto& result : m_impl->results)
                {
                    result.status = !explicitlySelected && dynamic_cast<CollectorUnavailable const*>(&error)
                                        ? MetricStatus::dependencyDisabled
                                        : MetricStatus::collectionFailed;
                    result.diagnostic = error.what();
                }
            }
        }

        Counters::~Counters()
        {
            if(m_impl->session)
                m_impl->collectorModule->api->destroy(m_impl->session);
        }

        void Counters::begin()
        {
            if(m_impl->session)
                m_impl->collectorModule->api->begin(m_impl->session);
        }

        bool Counters::hasEvents() const
        {
            return m_impl->session && m_impl->collectorModule->api->hasEvents(m_impl->session);
        }

        bool Counters::isRunning() const
        {
            return m_impl->session && m_impl->collectorModule->api->isRunning(m_impl->session);
        }

        std::vector<MetricResult> Counters::end()
        {
            if(m_impl->session)
            {
                Receiver receiver{{}, {}, m_impl->collectorModule->info};
                m_impl->collectorModule->api->end(m_impl->session, Receiver::emit, &receiver);
                if(receiver.failure)
                    for(auto& result : m_impl->results)
                        result.diagnostic = "Invalid collector result";
                else
                    for(auto& result : m_impl->results)
                    {
                        auto const found = std::find_if(
                            receiver.metrics.begin(),
                            receiver.metrics.end(),
                            [&](auto const& metric) { return metric.descriptor.name == result.descriptor.name; });
                        if(found != receiver.metrics.end())
                            result = *found;
                    }
            }
            if(m_impl->collectorModule)
                for(auto& result : m_impl->results)
                {
                    result.descriptor.collector = m_impl->collectorModule->info.name;
                    result.descriptor.collectorVersion = m_impl->collectorModule->info.version;
                    result.descriptor.collectorPath = m_impl->collectorModule->info.path;
                }
            return m_impl->results;
        }

        std::vector<MetricDescriptor> Counters::getAvailableMetrics()
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
    } // namespace internal
} // namespace alpakaMetrics
