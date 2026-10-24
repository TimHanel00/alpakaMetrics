// SPDX-License-Identifier: MPL-2.0
#include <cupti.h>
#include <native/Activity.hpp>

#include <cstdlib>
#include <unordered_map>

namespace
{
    using namespace alpakaMetrics;

    void require(CUptiResult status)
    {
        if(status != CUPTI_SUCCESS)
        {
            char const* message = nullptr;
            cuptiGetResultString(status, &message);
            throw std::runtime_error{message ? message : "CUPTI call failed"};
        }
    }

    struct Runtime
    {
        internal::activity::Registry registry;
        std::once_flag initialized;
        std::string failure;
        CUpti_SubscriberHandle subscriber{};
        std::unique_ptr<internal::activity::FlushWorker> worker;
        std::mutex mutex;
        std::unordered_map<std::uint32_t, std::uint64_t> correlations;
        std::unordered_map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> pending;

        void initialize();
    };

    Runtime& runtime()
    {
        // CUPTI owns process-global callbacks. Keep their storage and worker resident with the module.
        static auto* instance = new Runtime;
        return *instance;
    }

    void CUPTIAPI reserveSubscriber(void*, CUpti_CallbackDomain, CUpti_CallbackId, void const*)
    {
    }

    void CUPTIAPI requestBuffer(std::uint8_t** buffer, std::size_t* size, std::size_t* maxRecords)
    {
        *size = 64 * 1024;
        *maxRecords = 0;
        *buffer = static_cast<std::uint8_t*>(std::malloc(*size));
        if(!*buffer)
            *size = 0;
    }

    void CUPTIAPI
    completeBuffer(CUcontext context, std::uint32_t stream, std::uint8_t* buffer, std::size_t, std::size_t validSize)
    {
        std::unique_ptr<std::uint8_t, decltype(&std::free)> storage{buffer, std::free};
        try
        {
            auto& service = runtime();
            std::size_t dropped{};
            require(cuptiActivityGetNumDroppedRecords(context, stream, &dropped));
            if(dropped)
            {
                service.registry.failAll("CUPTI dropped activity records");
                return;
            }
            if(!validSize)
                return;
            std::lock_guard lock{service.mutex};
            CUpti_Activity* record{};
            for(;;)
            {
                auto status = cuptiActivityGetNextRecord(buffer, validSize, &record);
                if(status == CUPTI_ERROR_MAX_LIMIT_REACHED)
                    break;
                require(status);
                if(record->kind == CUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION)
                {
                    auto const& correlation = *reinterpret_cast<CUpti_ActivityExternalCorrelation const*>(record);
                    if(correlation.externalKind != CUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0)
                        continue;
                    service.correlations[correlation.correlationId] = correlation.externalId;
                    if(auto found = service.pending.find(correlation.correlationId); found != service.pending.end())
                    {
                        service.registry.record(correlation.externalId, found->second.first, found->second.second);
                        service.pending.erase(found);
                        service.correlations.erase(correlation.correlationId);
                    }
                }
                else if(record->kind == CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL)
                {
                    auto const& kernel = *reinterpret_cast<CUpti_ActivityKernel9 const*>(record);
                    if(auto found = service.correlations.find(kernel.correlationId);
                       found != service.correlations.end())
                    {
                        service.registry.record(found->second, kernel.start, kernel.end);
                        service.correlations.erase(found);
                    }
                    else
                        service.pending[kernel.correlationId] = {kernel.start, kernel.end};
                }
                if(service.pending.size() > 4096 || service.correlations.size() > 32768)
                {
                    service.registry.failAll("CUPTI correlation storage capacity exceeded");
                    service.pending.clear();
                    service.correlations.clear();
                }
            }
        }
        catch(std::exception const& error)
        {
            runtime().registry.failAll(error.what());
        }
        catch(...)
        {
            runtime().registry.failAll("CUPTI activity callback failed");
        }
    }

    void Runtime::initialize()
    {
        std::call_once(
            initialized,
            [this]
            {
                try
                {
                    require(cuptiSubscribe(&subscriber, reserveSubscriber, nullptr));
                    require(cuptiActivityRegisterCallbacks(requestBuffer, completeBuffer));
                    require(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION));
                    require(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_RUNTIME));
                    require(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DRIVER));
                    require(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
                    worker = std::make_unique<internal::activity::FlushWorker>(
                        registry,
                        [] { require(cuptiActivityFlushAll(0)); });
                }
                catch(std::exception const& error)
                {
                    failure = error.what();
                }
            });
    }

    void* create(
        plugin::Request const* requests,
        std::size_t size,
        plugin::Options const* options,
        plugin::Operation const* operation,
        plugin::AsyncSink const* sink) noexcept
    {
        internal::activity::Handle state;
        try
        {
            state = runtime()
                        .registry
                        .create(requests, size, *options, *operation, *sink, "cuda", "CUPTI", "cupti.kernel_duration");
            auto owner = std::make_unique<internal::activity::Handle>(state);
            if((*owner)->needsActivity)
            {
                runtime().initialize();
                if(!runtime().failure.empty())
                    (*owner)->fail(runtime().failure);
            }
            return owner.release();
        }
        catch(...)
        {
            if(state)
                runtime().registry.destroy(state);
            return nullptr;
        }
    }

    void destroy(void* session) noexcept
    {
        std::unique_ptr<internal::activity::Handle> state{static_cast<internal::activity::Handle*>(session)};
        runtime().registry.destroy(*state);
        std::lock_guard lock{runtime().mutex};
        std::erase_if(runtime().correlations, [&](auto const& entry) { return entry.second == (*state)->id; });
    }

    bool begin(void* session) noexcept
    {
        auto& state = **static_cast<internal::activity::Handle*>(session);
        try
        {
            if(state.needsActivity && runtime().failure.empty())
            {
                require(cuptiActivityPushExternalCorrelationId(CUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0, state.id));
                state.pushed = true;
            }
        }
        catch(std::exception const& error)
        {
            state.fail(error.what());
        }
        return true; // Specific failures are already delivered through the sink.
    }

    void submitted(void* session) noexcept
    {
        auto& state = **static_cast<internal::activity::Handle*>(session);
        if(state.pushed)
        {
            state.pushed = false;
            auto status = cuptiActivityPopExternalCorrelationId(CUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0, nullptr);
            if(status != CUPTI_SUCCESS)
                state.fail("CUPTI external correlation stack could not be closed");
        }
    }

    void poll(void* session, bool executionComplete) noexcept
    {
        try
        {
            auto const& state = *static_cast<internal::activity::Handle*>(session);
            if(executionComplete)
            {
                runtime().registry.executed(state);
                if(runtime().worker)
                    runtime().worker->request();
            }
        }
        catch(...)
        {
            (*static_cast<internal::activity::Handle*>(session))->fail("CUPTI delivery progress failed");
        }
    }

    void discover(plugin::Emit emit, void* context) noexcept
    {
        try
        {
            internal::activity::discover("CUPTI", "cupti.kernel_duration", emit, context);
        }
        catch(...)
        {
        }
    }
} // namespace

extern "C" alpakaMetrics::plugin::AsyncCollector const* alpakaMetrics_getAsyncCollector() noexcept
{
    try
    {
        static std::string const version = []
        {
            std::uint32_t value{};
            require(cuptiGetVersion(&value));
            return std::to_string(value);
        }();
        static alpakaMetrics::plugin::AsyncCollector const collector{
            alpakaMetrics::plugin::asyncCollectorAbiVersion,
            sizeof(alpakaMetrics::plugin::AsyncCollector),
            "CUPTI",
            version.c_str(),
            0,
            create,
            destroy,
            begin,
            submitted,
            poll,
            discover};
        return &collector;
    }
    catch(...)
    {
        return nullptr;
    }
}
