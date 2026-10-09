// SPDX-License-Identifier: MPL-2.0
#include <native/Activity.hpp>

#include <atomic>

#include <rocprofiler-sdk/buffer.h>
#include <rocprofiler-sdk/buffer_tracing.h>
#include <rocprofiler-sdk/context.h>
#include <rocprofiler-sdk/external_correlation.h>
#include <rocprofiler-sdk/registration.h>
#include <rocprofiler-sdk/rocprofiler.h>

namespace
{
    using namespace alpakaMetrics;

    void require(rocprofiler_status_t status)
    {
        if(status != ROCPROFILER_STATUS_SUCCESS)
            throw std::runtime_error{"rocprofiler-sdk status " + std::to_string(static_cast<int>(status))};
    }

    struct Runtime
    {
        internal::activity::Registry registry;
        rocprofiler_context_id_t context{};
        rocprofiler_buffer_id_t buffer{};
        std::atomic<bool> ready{};
        std::mutex setupMutex;
        std::mutex flushMutex;
        std::string failure;
        std::unique_ptr<internal::activity::FlushWorker> worker;
    };

    Runtime& runtime()
    {
        // Tool registration and buffered callbacks have process lifetime, like the loaded module.
        static auto* instance = new Runtime;
        return *instance;
    }

    void records(
        rocprofiler_context_id_t,
        rocprofiler_buffer_id_t,
        rocprofiler_record_header_t** headers,
        std::size_t size,
        void*,
        std::uint64_t dropped)
    {
        try
        {
            if(dropped)
            {
                runtime().registry.failAll("rocprofiler-sdk dropped activity records");
                return;
            }
            for(std::size_t i = 0; i < size; ++i)
            {
                auto const* header = headers[i];
                if(!header || !header->payload)
                    throw std::runtime_error{"Invalid rocprofiler-sdk activity record"};
                if(header->category == ROCPROFILER_BUFFER_CATEGORY_TRACING
                   && header->kind == ROCPROFILER_BUFFER_TRACING_KERNEL_DISPATCH)
                {
                    auto const& record
                        = *static_cast<rocprofiler_buffer_tracing_kernel_dispatch_record_t const*>(header->payload);
                    runtime().registry.record(
                        record.correlation_id.external.value,
                        record.start_timestamp,
                        record.end_timestamp);
                }
            }
        }
        catch(std::exception const& error)
        {
            runtime().registry.failAll(error.what());
        }
        catch(...)
        {
            runtime().registry.failAll("rocprofiler-sdk activity callback failed");
        }
    }

    int initialize(rocprofiler_client_finalize_t, void*)
    {
        auto& service = runtime();
        std::lock_guard lock{service.setupMutex};
        try
        {
            require(rocprofiler_create_context(&service.context));
            // Lossless mode allocates both buffers used by SDK flush rotation.
            // Flushes run on provider threads; full vendor buffers may apply backpressure.
            require(rocprofiler_create_buffer(
                service.context,
                64 * 1024,
                32 * 1024,
                ROCPROFILER_BUFFER_POLICY_LOSSLESS,
                records,
                nullptr,
                &service.buffer));
            require(rocprofiler_configure_buffer_tracing_service(
                service.context,
                ROCPROFILER_BUFFER_TRACING_KERNEL_DISPATCH,
                nullptr,
                0,
                service.buffer));
            require(rocprofiler_start_context(service.context));
            service.worker = std::make_unique<internal::activity::FlushWorker>(
                service.registry,
                []
                {
                    auto& state = runtime();
                    std::lock_guard flushLock{state.flushMutex};
                    if(state.ready)
                        require(rocprofiler_flush_buffer(state.buffer));
                });
            service.ready = true;
            return 0;
        }
        catch(std::exception const& error)
        {
            service.failure = error.what();
            return -1;
        }
        catch(...)
        {
            service.failure = "rocprofiler-sdk initialization failed";
            return -1;
        }
    }

    void finalize(void*)
    {
        auto& service = runtime();
        std::lock_guard lock{service.flushMutex};
        if(service.ready.exchange(false))
        {
            rocprofiler_stop_context(service.context);
            rocprofiler_flush_buffer(service.buffer);
        }
        service.registry.failAll("rocprofiler-sdk finalized before activity delivery completed");
    }
} // namespace

extern "C" rocprofiler_tool_configure_result_t* rocprofiler_configure(
    std::uint32_t,
    char const*,
    std::uint32_t,
    rocprofiler_client_id_t* client)
{
    client->name = "alpakaMetrics";
    static rocprofiler_tool_configure_result_t configuration{
        sizeof(rocprofiler_tool_configure_result_t),
        initialize,
        finalize,
        nullptr};
    return &configuration;
}

namespace
{
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
            state = runtime().registry.create(
                requests,
                size,
                *options,
                *operation,
                *sink,
                "hip",
                "rocprofiler-sdk",
                "rocprofiler.kernel_duration");
            auto owner = std::make_unique<internal::activity::Handle>(state);
            if((*owner)->needsActivity && !runtime().ready)
            {
                auto status = rocprofiler_force_configure(rocprofiler_configure);
                if(status != ROCPROFILER_STATUS_SUCCESS || !runtime().ready)
                {
                    std::lock_guard lock{runtime().setupMutex};
                    (*owner)->fail(
                        runtime().failure.empty()
                            ? "Initialize the ROCm collector before HIP using ROCP_TOOL_LIBRARIES"
                            : runtime().failure);
                }
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
    }

    bool begin(void* session) noexcept
    {
        auto& state = **static_cast<internal::activity::Handle*>(session);
        try
        {
            if(state.needsActivity && runtime().ready)
            {
                rocprofiler_thread_id_t thread{};
                require(rocprofiler_get_thread_id(&thread));
                rocprofiler_user_data_t correlation{};
                correlation.value = state.id;
                require(rocprofiler_push_external_correlation_id(runtime().context, thread, correlation));
                state.pushed = true;
            }
        }
        catch(std::exception const& error)
        {
            state.fail(error.what());
        }
        return true;
    }

    void submitted(void* session) noexcept
    {
        auto& state = **static_cast<internal::activity::Handle*>(session);
        if(state.pushed)
        {
            state.pushed = false;
            rocprofiler_thread_id_t thread{};
            rocprofiler_user_data_t correlation{};
            if(rocprofiler_get_thread_id(&thread) != ROCPROFILER_STATUS_SUCCESS
               || rocprofiler_pop_external_correlation_id(runtime().context, thread, &correlation)
                      != ROCPROFILER_STATUS_SUCCESS
               || correlation.value != state.id)
                state.fail("rocprofiler-sdk external correlation stack could not be closed");
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
            (*static_cast<internal::activity::Handle*>(session))->fail("rocprofiler-sdk delivery progress failed");
        }
    }

    void discover(plugin::Emit emit, void* context) noexcept
    {
        try
        {
            internal::activity::discover("rocprofiler-sdk", "rocprofiler.kernel_duration", emit, context);
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
            std::uint32_t major{}, minor{}, patch{};
            require(rocprofiler_get_version(&major, &minor, &patch));
            return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
        }();
        static alpakaMetrics::plugin::AsyncCollector const collector{
            alpakaMetrics::plugin::asyncCollectorAbiVersion,
            sizeof(alpakaMetrics::plugin::AsyncCollector),
            "rocprofiler-sdk",
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
