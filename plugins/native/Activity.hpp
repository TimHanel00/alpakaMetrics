// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/internal/PluginBridge.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace alpakaMetrics::internal::activity
{
    struct State
    {
        std::uint64_t id;
        plugin::AsyncSink sink;
        std::vector<MetricResult> metrics;
        std::mutex mutex;
        bool needsActivity{};
        bool executed{};
        bool closed{};
        bool pushed{}; // Submission-thread-only correlation stack state.
        std::optional<double> seconds;
        std::optional<std::chrono::steady_clock::time_point> executedAt;

        State(std::uint64_t id, plugin::AsyncSink sink) : id{id}, sink{sink}
        {
        }

        void publishLocked()
        {
            if(closed || (needsActivity && (!executed || !seconds)))
                return;
            for(auto& metric : metrics)
            {
                if(metric.status == MetricStatus::available)
                {
                    double const value = *seconds * metric.descriptor.nativeToValueScale * 1.0e9;
                    if(std::isfinite(value))
                        metric.value = value;
                    else
                    {
                        metric.status = MetricStatus::collectionFailed;
                        metric.diagnostic = "Kernel duration conversion overflowed";
                    }
                }
                emitMetric(
                    metric,
                    [](void* context, plugin::Metric const* record) noexcept
                    {
                        auto& state = *static_cast<State*>(context);
                        state.sink.emit(state.sink.context, state.id, record);
                    },
                    this);
            }
            closed = true;
            sink.complete(sink.context, id, true, "");
        }

        void fail(std::string const& diagnostic) noexcept
        {
            std::lock_guard lock{mutex};
            if(!closed)
            {
                closed = true;
                sink.complete(sink.context, id, false, diagnostic.c_str());
            }
        }
    };

    using Handle = std::shared_ptr<State>;

    class Registry
    {
    public:
        Handle create(
            plugin::Request const* requests,
            std::size_t size,
            plugin::Options const& options,
            plugin::Operation const& operation,
            plugin::AsyncSink sink,
            std::string_view api,
            std::string_view provider,
            std::string_view nativeName)
        {
            auto state = std::make_shared<State>(operation.measurementId, sink);
            for(std::size_t i = 0; i < size; ++i)
            {
                auto const& request = requests[i];
                bool const recognized = std::string_view{request.name} == "device_execution_time"
                                        || std::string_view{request.nativeName} == nativeName;
                MetricResult metric{
                    {request.name,
                     request.nativeName,
                     "Device execution time of one externally correlated kernel",
                     std::string{provider},
                     MetricUnit::seconds,
                     MetricScope::providerDefined,
                     "nanoseconds",
                     1.0e-9 * request.scale},
                    MetricStatus::unsupported,
                    {},
                    "Metric is not provided by this activity collector"};
                if(recognized)
                {
                    metric.descriptor.nativeName = nativeName;
                    if(std::string_view{options.deviceApi} != api
                       || options.scope != static_cast<std::uint32_t>(MetricScope::providerDefined))
                    {
                        metric.status = MetricStatus::unsupportedScope;
                        metric.diagnostic
                            = "Kernel activity requires a matching device API and provider-defined scope";
                    }
                    else if(
                        request.unit != static_cast<std::uint32_t>(MetricUnit::seconds)
                        && request.unit != static_cast<std::uint32_t>(MetricUnit::providerDefined))
                        metric.diagnostic = "Kernel duration requires seconds";
                    else
                    {
                        metric.status = MetricStatus::available;
                        metric.diagnostic.clear();
                        state->needsActivity = true;
                    }
                }
                state->metrics.push_back(std::move(metric));
            }
            {
                std::lock_guard lock{m_mutex};
                if(!m_states.emplace(state->id, state).second)
                    throw std::runtime_error{"Duplicate activity operation ID"};
            }
            if(!state->needsActivity)
            {
                std::lock_guard lock{state->mutex};
                state->publishLocked();
            }
            return state;
        }

        void destroy(Handle const& state)
        {
            {
                std::lock_guard lock{m_mutex};
                m_states.erase(state->id);
            }
            // Serialize with in-flight SDK callbacks before the core releases its sink.
            std::lock_guard lock{state->mutex};
            state->closed = true;
        }

        void record(std::uint64_t id, std::uint64_t start, std::uint64_t end)
        {
            Handle state;
            {
                std::lock_guard lock{m_mutex};
                if(auto found = m_states.find(id); found != m_states.end())
                    state = found->second;
            }
            if(!state)
                return;
            std::lock_guard lock{state->mutex};
            if(state->closed)
                return;
            if(state->seconds || start == 0 || end < start)
            {
                state->closed = true;
                state->sink.complete(state->sink.context, id, false, "Invalid or multiple kernel activity records");
                return;
            }
            state->seconds = static_cast<double>(end - start) * 1.0e-9;
            state->publishLocked();
        }

        void executed(Handle const& state)
        {
            std::lock_guard lock{state->mutex};
            if(!state->executed)
                state->executedAt = std::chrono::steady_clock::now();
            state->executed = true;
            state->publishLocked();
        }

        bool pending()
        {
            bool pending = false;
            for(auto const& state : snapshot())
            {
                std::lock_guard lock{state->mutex};
                if(!state->closed && state->executed && state->needsActivity)
                {
                    if(std::chrono::steady_clock::now() - *state->executedAt > std::chrono::seconds{30})
                    {
                        state->closed = true;
                        state->sink.complete(
                            state->sink.context,
                            state->id,
                            false,
                            "Correlated kernel activity was not delivered");
                    }
                    else
                        pending = true;
                }
            }
            return pending;
        }

        void failAll(std::string const& diagnostic)
        {
            for(auto const& state : snapshot())
                state->fail(diagnostic);
        }

    private:
        std::vector<Handle> snapshot()
        {
            std::lock_guard lock{m_mutex};
            std::vector<Handle> states;
            for(auto const& [id, state] : m_states)
                states.push_back(state);
            return states;
        }

        std::mutex m_mutex;
        std::map<std::uint64_t, Handle> m_states;
    };

    /** Blocking SDK flushes stay on a provider-owned worker, never the core progress thread. */
    class FlushWorker
    {
    public:
        FlushWorker(Registry& registry, std::function<void()> flush)
            : m_registry{registry}
            , m_flush{std::move(flush)}
            , m_worker{[this] { run(); }}
        {
        }

        void request()
        {
            {
                std::lock_guard lock{m_mutex};
                m_requested = true;
            }
            m_changed.notify_one();
        }

    private:
        void run()
        {
            for(;;)
            {
                {
                    std::unique_lock lock{m_mutex};
                    m_changed.wait(lock, [&] { return m_requested; });
                    m_requested = false;
                }
                try
                {
                    while(m_registry.pending())
                    {
                        m_flush();
                        std::this_thread::sleep_for(std::chrono::milliseconds{5});
                    }
                }
                catch(std::exception const& error)
                {
                    m_registry.failAll(error.what());
                }
                catch(...)
                {
                    m_registry.failAll("Activity buffer flush failed");
                }
            }
        }

        Registry& m_registry;
        std::function<void()> m_flush;
        std::mutex m_mutex;
        std::condition_variable m_changed;
        bool m_requested{};
        std::thread m_worker;
    };

    inline void discover(std::string const& provider, std::string const& nativeName, plugin::Emit emit, void* context)
    {
        emitMetric(
            {{"device_execution_time",
              nativeName,
              "Correlated kernel device timestamps",
              provider,
              MetricUnit::seconds,
              MetricScope::providerDefined,
              "nanoseconds",
              1.0e-9},
             MetricStatus::unsupported,
             {},
             {}},
            emit,
            context);
    }
} // namespace alpakaMetrics::internal::activity
