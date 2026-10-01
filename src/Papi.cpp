// SPDX-License-Identifier: MPL-2.0
#include "alpakaMetrics/internal/Papi.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <mutex>
#include <thread>

#if ALPAKA_METRICS_HAS_PAPI
#    include <papi.h>
#endif

namespace alpakaMetrics::internal
{
    struct PapiCounters::Impl
    {
        std::vector<MetricResult> results;
#if ALPAKA_METRICS_HAS_PAPI
        struct Group
        {
            int component{};
            int eventSet{PAPI_NULL};
            bool running{};
            std::vector<std::size_t> resultIndices;
            std::vector<int> dataTypes;
            std::vector<double> scales;
        };

        std::vector<Group> groups;
        std::thread::id owner{std::this_thread::get_id()};
#endif
    };

#if ALPAKA_METRICS_HAS_PAPI
    namespace
    {
        unsigned long threadId()
        {
            static std::atomic<unsigned long> next{1ul};
            thread_local unsigned long const id = next.fetch_add(1ul);
            return id;
        }

        int initialize()
        {
            static std::once_flag once;
            static int result{PAPI_OK};
            std::call_once(
                once,
                []
                {
                    auto const initialized = PAPI_library_init(PAPI_VER_CURRENT);
                    if(initialized != PAPI_VER_CURRENT)
                        result = initialized < 0 ? initialized : PAPI_EINVAL;
                    else if(PAPI_is_initialized() != PAPI_THREAD_LEVEL_INITED)
                        result = PAPI_thread_init(threadId);
                });
            return result;
        }

        int registerThread()
        {
            struct Registration
            {
                int result{PAPI_register_thread()};

                ~Registration()
                {
                    if(result == PAPI_OK)
                        PAPI_unregister_thread();
                }
            };

            thread_local Registration registration;
            return registration.result;
        }

        MetricStatus statusFor(int error)
        {
            if(error == PAPI_EPERM)
                return MetricStatus::permissionDenied;
            if(error == PAPI_ECNFLCT || error == PAPI_EISRUN)
                return MetricStatus::conflicting;
            if(error == PAPI_ENOEVNT || error == PAPI_ENOSUPP || error == PAPI_ENOCMP)
                return MetricStatus::unsupported;
            return MetricStatus::collectionFailed;
        }

        void setFailure(MetricResult& result, int error)
        {
            result.status = statusFor(error);
            result.value.reset();
            result.diagnostic = PAPI_strerror(error);
            if(error == PAPI_ENOCMP)
                for(int component = 0; component < PAPI_num_components(); ++component)
                {
                    auto const* info = PAPI_get_component_info(component);
                    if(info && info->disabled && info->disabled_reason[0] != '\0')
                        result.diagnostic += "; " + std::string{info->name} + ": " + info->disabled_reason;
                }
        }

        MetricDescriptor descriptorFor(std::string name, PAPI_event_info_t const& info, int component)
        {
            auto const* componentInfo = PAPI_get_component_info(component);
            std::string provider = componentInfo ? componentInfo->name : "unknown";
            auto scope = component == 0 ? MetricScope::callingThread : MetricScope::providerDefined;
            if(provider == "cuda")
                scope = MetricScope::context;
            else if(
                provider == "rocp_sdk" || provider == "rocm" || provider == "intel_gpu" || provider == "nvml"
                || provider == "rocm_smi" || provider == "rapl" || provider == "powercap")
                scope = MetricScope::device;
            return {
                std::move(name),
                info.symbol,
                info.long_descr,
                "PAPI/" + provider,
                MetricUnit::providerDefined,
                scope,
                info.units};
        }
    } // namespace
#endif

    PapiCounters::PapiCounters(Config const& config) : m_impl{std::make_unique<Impl>()}
    {
        for(auto const& request : config.metrics)
        {
            if(request.name == "elapsed_time")
                continue;
            m_impl->results.push_back(
                makeUnavailable(request, MetricStatus::unsupported, "No semantic mapping for this metric"));
        }
#if ALPAKA_METRICS_HAS_PAPI
        bool const needsPapi = std::any_of(
            config.metrics.begin(),
            config.metrics.end(),
            [](auto const& request) { return !request.papiName.empty(); });
        int initialized = needsPapi ? initialize() : PAPI_OK;
        if(needsPapi && initialized == PAPI_OK)
            initialized = registerThread();
        std::size_t index = 0u;
        for(auto const& request : config.metrics)
        {
            if(request.name == "elapsed_time")
                continue;
            auto& result = m_impl->results[index++];
            if(request.papiName.empty())
                continue;
            if(initialized != PAPI_OK)
            {
                setFailure(result, initialized);
                continue;
            }
            int code{};
            int error = PAPI_event_name_to_code(const_cast<char*>(request.papiName.c_str()), &code);
            PAPI_event_info_t info{};
            if(error == PAPI_OK)
                error = PAPI_get_event_info(code, &info);
            if(error != PAPI_OK)
            {
                setFailure(result, error);
                continue;
            }
            int const component = PAPI_get_event_component(code);
            result.descriptor = descriptorFor(request.name, info, component);
            if(request.name != request.papiName)
                result.descriptor.unit = MetricUnit::count;
            if(request.unit != MetricUnit::providerDefined)
                result.descriptor.unit = request.unit;
            result.descriptor.nativeToValueScale = request.scale;
            if(config.counterScope == MetricScope::callingThread
               && result.descriptor.scope != MetricScope::callingThread)
            {
                result.status = MetricStatus::unsupportedScope;
                result.diagnostic = "Queue worker collection accepts only calling-thread counters";
                continue;
            }
            auto group = std::find_if(
                m_impl->groups.begin(),
                m_impl->groups.end(),
                [component](auto const& candidate) { return candidate.component == component; });
            if(group == m_impl->groups.end())
            {
                m_impl->groups.push_back({component});
                group = std::prev(m_impl->groups.end());
                error = PAPI_create_eventset(&group->eventSet);
                if(error == PAPI_OK)
                    error = PAPI_assign_eventset_component(group->eventSet, component);
            }
            if(error == PAPI_OK)
                error = PAPI_add_event(group->eventSet, code);
            if(error != PAPI_OK)
                setFailure(result, error);
            else
            {
                group->resultIndices.push_back(index - 1u);
                group->dataTypes.push_back(info.data_type);
                group->scales.push_back(request.scale);
                result.status = MetricStatus::available;
                result.diagnostic.clear();
            }
        }
#else
        std::size_t index = 0u;
        for(auto const& request : config.metrics)
        {
            if(request.name == "elapsed_time")
                continue;
            auto& result = m_impl->results[index++];
            if(!request.papiName.empty())
            {
                result.status = MetricStatus::dependencyDisabled;
                result.diagnostic = "Built with alpakaMetrics_DEP_PAPI=OFF";
            }
        }
#endif
    }

    PapiCounters::~PapiCounters()
    {
#if ALPAKA_METRICS_HAS_PAPI
        // Event sets belong to the creating thread. Active host instrumentation is thread-affine.
        if(m_impl->owner != std::this_thread::get_id())
            return;
        for(auto& group : m_impl->groups)
        {
            if(group.eventSet == PAPI_NULL)
                continue;
            if(group.running)
            {
                std::vector<long long> values(group.resultIndices.size());
                PAPI_stop(group.eventSet, values.data());
            }
            PAPI_cleanup_eventset(group.eventSet);
            PAPI_destroy_eventset(&group.eventSet);
        }
#endif
    }

    void PapiCounters::begin()
    {
#if ALPAKA_METRICS_HAS_PAPI
        for(auto& group : m_impl->groups)
        {
            if(group.resultIndices.empty())
                continue;
            int const error = PAPI_start(group.eventSet);
            group.running = error == PAPI_OK;
            if(!group.running)
                for(auto index : group.resultIndices)
                    setFailure(m_impl->results[index], error);
        }
#endif
    }

    std::vector<MetricResult> PapiCounters::end()
    {
#if ALPAKA_METRICS_HAS_PAPI
        static_assert(sizeof(long long) == sizeof(std::int64_t));
        for(auto& group : m_impl->groups)
        {
            if(!group.running)
                continue;
            std::vector<long long> values(group.resultIndices.size());
            int const error = PAPI_stop(group.eventSet, values.data());
            group.running = false;
            for(std::size_t i = 0u; i < values.size(); ++i)
            {
                auto& result = m_impl->results[group.resultIndices[i]];
                if(error != PAPI_OK)
                    setFailure(result, error);
                else if(group.dataTypes[i] == PAPI_DATATYPE_FP64)
                    result.value = std::bit_cast<double>(values[i]);
                else if(group.dataTypes[i] == PAPI_DATATYPE_UINT64 || group.dataTypes[i] == PAPI_DATATYPE_BIT64)
                    result.value = std::bit_cast<std::uint64_t>(values[i]);
                else
                    result.value = static_cast<std::int64_t>(values[i]);
                if(result.isAvailable() && group.scales[i] != 1.0)
                {
                    auto const scaled = result.asDouble() * group.scales[i];
                    if(std::isfinite(scaled))
                        result.value = scaled;
                    else
                    {
                        result.status = MetricStatus::collectionFailed;
                        result.value.reset();
                        result.diagnostic = "Native metric conversion overflowed";
                    }
                }
            }
        }
#endif
        return m_impl->results;
    }

    std::vector<MetricDescriptor> PapiCounters::getAvailableMetrics()
    {
        std::vector<MetricDescriptor> descriptors;
#if ALPAKA_METRICS_HAS_PAPI
        if(initialize() != PAPI_OK)
            return descriptors;
        int code = PAPI_PRESET_MASK;
        int error = PAPI_enum_event(&code, PAPI_ENUM_FIRST);
        while(error == PAPI_OK)
        {
            PAPI_event_info_t info{};
            if(PAPI_get_event_info(code, &info) == PAPI_OK && PAPI_query_event(code) == PAPI_OK)
                descriptors.push_back(descriptorFor(info.symbol, info, PAPI_get_event_component(code)));
            error = PAPI_enum_event(&code, PAPI_PRESET_ENUM_AVAIL);
        }
        for(int component = 0; component < PAPI_num_components(); ++component)
        {
            auto const* info = PAPI_get_component_info(component);
            if(!info || info->disabled)
                continue;
            code = PAPI_NATIVE_MASK;
            error = PAPI_enum_cmp_event(&code, PAPI_ENUM_FIRST, component);
            while(error == PAPI_OK)
            {
                PAPI_event_info_t eventInfo{};
                if(PAPI_get_event_info(code, &eventInfo) == PAPI_OK)
                    descriptors.push_back(descriptorFor(eventInfo.symbol, eventInfo, component));
                error = PAPI_enum_cmp_event(&code, PAPI_ENUM_EVENTS, component);
            }
        }
#endif
        return descriptors;
    }
} // namespace alpakaMetrics::internal
