// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/plugin/papi/PapiSession.hpp>
#include <papi.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace alpakaMetrics::papi
{
    struct PapiSession::Impl
    {
        std::vector<MetricResult> results;

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
            std::atomic_flag* gate{};

            DeviceLease() = default;
            DeviceLease(DeviceLease const&) = delete;
            DeviceLease& operator=(DeviceLease const&) = delete;

            ~DeviceLease()
            {
                if(gate)
                    gate->clear(std::memory_order_release);
            }

            bool acquire(std::atomic_flag& candidate)
            {
                if(gate)
                    return true;
                if(candidate.test_and_set(std::memory_order_acquire))
                    return false;
                gate = &candidate;
                return true;
            }
        };

        std::vector<Group> groups;
        DeviceLease deviceLease;
        std::thread::id owner{std::this_thread::get_id()};
    };

    namespace
    {
        bool bindDeviceRequest(MetricRequest& request, DeviceCounterTarget target, MetricResult& result)
        {
            auto reject = [&](std::string diagnostic)
            {
                result.status = MetricStatus::unsupportedScope;
                result.diagnostic = std::move(diagnostic);
                return false;
            };
            if(target.component != "cuda" && target.component != "rocp_sdk")
                return reject("No device counter provider for this API");
            auto const prefix = std::string{target.component} + ":::";
            if(!request.nativeName.starts_with(prefix))
                return reject("Device queue counters require an explicit " + prefix + " native mapping");
            auto const qualifier = request.nativeName.find(":device=");
            if(qualifier == std::string::npos)
                request.nativeName += ":device=" + std::to_string(target.device);
            else
            {
                auto const begin = qualifier + std::string_view{":device="}.size();
                auto const end = request.nativeName.find(':', begin);
                std::string_view number{
                    request.nativeName.data() + begin,
                    (end == std::string::npos ? request.nativeName.size() : end) - begin};
                std::uint32_t device{};
                auto const parsed = std::from_chars(number.data(), number.data() + number.size(), device);
                if(parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || device != target.device
                   || request.nativeName.find(":device=", begin) != std::string::npos)
                    return reject("Native event device qualifier does not match the queue device");
            }
            result.descriptor.nativeName = request.nativeName;
            if(target.component == "rocp_sdk")
            {
                if(std::getenv("PAPI_ROCP_SDK_DISPATCH_MODE"))
                    return reject(
                        "The pinned PAPI dispatch mode cannot guarantee completed counter records; use "
                        "ROCP_SDK sampling mode");
                if(std::getenv("HIP_VISIBLE_DEVICES") || std::getenv("ROCR_VISIBLE_DEVICES")
                   || std::getenv("CUDA_VISIBLE_DEVICES") || std::getenv("GPU_DEVICE_ORDINAL"))
                    return reject("HIP device visibility remapping cannot be matched to PAPI agent indices yet");
            }
            return true;
        }
    } // namespace

    namespace
    {
        std::atomic_flag& deviceCounterGate()
        {
            static std::atomic_flag gate{};
            return gate;
        }

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
            if(error == PAPI_EMULPASS)
                return MetricStatus::unsupported;
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

    PapiSession::PapiSession(Config const& config, std::optional<DeviceCounterTarget> target)
        : m_impl{std::make_unique<Impl>()}
    {
        std::vector<MetricRequest> requests;
        std::vector<bool> eligible;
        std::vector<bool> mapped;
        for(auto request : config.metrics)
        {
            if(request.nativeName.empty())
            {
                static constexpr std::pair<std::string_view, std::string_view> presets[]
                    = {{"cycles", "PAPI_TOT_CYC"},
                       {"instructions", "PAPI_TOT_INS"},
                       {"floating_point_operations", "PAPI_FP_OPS"},
                       {"l2_misses", "PAPI_L2_TCM"},
                       {"l3_misses", "PAPI_L3_TCM"},
                       {"l1_data_misses", "PAPI_L1_DCM"},
                       {"l2_accesses", "PAPI_L2_TCA"},
                       {"l3_accesses", "PAPI_L3_TCA"},
                       {"branch_instructions", "PAPI_BR_INS"},
                       {"branch_mispredictions", "PAPI_BR_MSP"},
                       {"load_instructions", "PAPI_LD_INS"},
                       {"store_instructions", "PAPI_SR_INS"},
                       {"resource_stall_cycles", "PAPI_RES_STL"}};
                for(auto const& [name, event] : presets)
                    if(request.name == name)
                        request.nativeName = event;
            }
            if(request.name == "elapsed_time")
                continue;
            m_impl->results.push_back(
                makeUnavailable(request, MetricStatus::unsupported, "No semantic mapping for this metric"));
            requests.push_back(request);
            mapped.push_back(request.name != request.nativeName);
            bool accepted = !request.nativeName.empty();
            if(target && accepted)
            {
                if(config.counterScope == MetricScope::callingThread)
                {
                    auto& result = m_impl->results.back();
                    result.status = MetricStatus::unsupportedScope;
                    result.diagnostic = "Device queues cannot collect calling-thread counters";
                    accepted = false;
                }
                else
                    accepted = bindDeviceRequest(requests.back(), *target, m_impl->results.back());
            }
            eligible.push_back(accepted);
        }
        bool const needsPapi = std::any_of(eligible.begin(), eligible.end(), [](bool value) { return value; });
        int initialized = needsPapi ? initialize() : PAPI_OK;
        if(needsPapi && initialized == PAPI_OK)
            initialized = registerThread();
        for(std::size_t index = 0u; index < requests.size(); ++index)
        {
            auto const& request = requests[index];
            auto& result = m_impl->results[index];
            if(!eligible[index])
                continue;
            if(initialized != PAPI_OK)
            {
                setFailure(result, initialized);
                continue;
            }
            // Acquire before touching the component's event-set state. An atomic
            // lease also rejects nested regions on the same thread without relying
            // on recursive locking or invoking try_lock() on an owned mutex.
            if((request.nativeName.starts_with("cuda:::") || request.nativeName.starts_with("rocp_sdk:::"))
               && !m_impl->deviceLease.acquire(deviceCounterGate()))
            {
                result.status = MetricStatus::conflicting;
                result.diagnostic = "Another device counter region is active in this process";
                continue;
            }
            int code{};
            int error = PAPI_event_name_to_code(const_cast<char*>(request.nativeName.c_str()), &code);
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
            if(mapped[index])
                result.descriptor.unit = MetricUnit::count;
            if(request.unit != MetricUnit::providerDefined)
                result.descriptor.unit = request.unit;
            result.descriptor.nativeToValueScale = request.scale;
            auto const* componentInfo = PAPI_get_component_info(component);
            if(target && (!componentInfo || target->component != componentInfo->name))
            {
                result.status = MetricStatus::unsupportedScope;
                result.diagnostic = "Native event belongs to a different API component";
                continue;
            }
            if(config.counterScope == MetricScope::callingThread
               && result.descriptor.scope != MetricScope::callingThread)
            {
                result.status = MetricStatus::unsupportedScope;
                result.diagnostic = "Queue worker collection accepts only calling-thread counters";
                continue;
            }
            if(componentInfo
               && (std::string_view{componentInfo->name} == "cuda"
                   || std::string_view{componentInfo->name} == "rocp_sdk"))
            {
                if(!m_impl->deviceLease.acquire(deviceCounterGate()))
                {
                    result.status = MetricStatus::conflicting;
                    result.diagnostic = "Another device counter region is active in this process";
                    continue;
                }
            }
            auto group = std::find_if(
                m_impl->groups.begin(),
                m_impl->groups.end(),
                [component](auto const& candidate) { return candidate.component == component; });
            if(group == m_impl->groups.end())
            {
                m_impl->groups.push_back({component});
                group = std::prev(m_impl->groups.end());
                group->error = PAPI_create_eventset(&group->eventSet);
                if(group->error == PAPI_OK)
                    group->error = PAPI_assign_eventset_component(group->eventSet, component);
            }
            error = group->error;
            if(error == PAPI_OK)
                error = PAPI_add_event(group->eventSet, code);
            if(error != PAPI_OK)
                setFailure(result, error);
            else
            {
                group->resultIndices.push_back(index);
                group->dataTypes.push_back(info.data_type);
                group->scales.push_back(request.scale);
                result.status = MetricStatus::available;
                result.diagnostic.clear();
            }
        }
    }

    bool PapiSession::hasEvents() const
    {
        return std::any_of(
            m_impl->groups.begin(),
            m_impl->groups.end(),
            [](auto const& group) { return !group.resultIndices.empty(); });
    }

    bool PapiSession::isRunning() const
    {
        return std::any_of(
            m_impl->groups.begin(),
            m_impl->groups.end(),
            [](auto const& group) { return group.running; });
    }

    PapiSession::~PapiSession()
    {
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
    }

    void PapiSession::begin()
    {
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
    }

    std::vector<MetricResult> PapiSession::end()
    {
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
        return m_impl->results;
    }

    std::vector<MetricDescriptor> PapiSession::getAvailableMetrics()
    {
        std::vector<MetricDescriptor> descriptors;
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
        return descriptors;
    }
} // namespace alpakaMetrics::papi
