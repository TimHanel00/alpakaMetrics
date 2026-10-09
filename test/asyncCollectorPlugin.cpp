// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/internal/PluginBridge.hpp>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
    using namespace alpakaMetrics;
    struct State;
    std::mutex mutex;
    std::map<std::uint64_t, State*> states;

    struct State
    {
        plugin::AsyncSink sink;
        std::uint64_t id;
        std::vector<MetricResult> metrics;
        std::string mode;
        std::thread::id submitThread{std::this_thread::get_id()};
        std::mutex mutex;
        std::condition_variable changed;
        bool submitted{};
        bool executed{};
        bool released{};
        bool cancelled{};
        std::thread worker;

        State(plugin::AsyncSink sink, std::uint64_t id) : sink{sink}, id{id}
        {
        }

        ~State()
        {
            {
                std::lock_guard lock{mutex};
                cancelled = true;
            }
            changed.notify_all();
            if(worker.joinable())
                worker.join();
        }

        void run()
        {
            {
                std::unique_lock lock{mutex};
                changed.wait(lock, [&] { return cancelled || (submitted && executed && released); });
                if(cancelled)
                    return;
            }
            auto const deliveredId = mode == "wrong" ? id + 1 : id;
            if(mode != "missing" && mode != "failed")
                for(auto const& metric : metrics)
                {
                    auto emit = [&](plugin::Metric const* value) { sink.emit(sink.context, deliveredId, value); };
                    auto bridge = [](void* context, plugin::Metric const* value) noexcept
                    { (*static_cast<decltype(emit)*>(context))(value); };
                    internal::emitMetric(metric, bridge, &emit);
                    if(mode == "duplicate")
                        internal::emitMetric(metric, bridge, &emit);
                }
            sink.complete(sink.context, deliveredId, mode != "failed", mode == "failed" ? "records dropped" : "");
        }
    };

    void* create(
        plugin::Request const* requests,
        std::size_t size,
        plugin::Options const*,
        plugin::Operation const* operation,
        plugin::AsyncSink const* sink) noexcept
    {
        try
        {
            auto state = std::make_unique<State>(*sink, operation->measurementId);
            for(std::size_t i = 0; i < size; ++i)
            {
                state->mode = requests[i].nativeName;
                state->metrics.push_back(
                    {{requests[i].name,
                      requests[i].nativeName,
                      "Delayed operation-correlated fixture",
                      "test_async",
                      MetricUnit::count,
                      MetricScope::queueInterval,
                      "count",
                      1},
                     MetricStatus::available,
                     operation->measurementId,
                     {}});
            }
            if(state->mode == "create_failed")
                return nullptr;
            state->worker = std::thread{[ptr = state.get()] { ptr->run(); }};
            {
                std::lock_guard lock{mutex};
                states.emplace(state->id, state.get());
            }
            return state.release();
        }
        catch(...)
        {
            return nullptr;
        }
    }

    void destroy(void* ptr) noexcept
    {
        auto* state = static_cast<State*>(ptr);
        {
            std::lock_guard lock{mutex};
            states.erase(state->id);
        }
        delete state;
    }

    bool begin(void* ptr) noexcept
    {
        auto& state = *static_cast<State*>(ptr);
        return state.submitThread == std::this_thread::get_id() && state.mode != "begin_failed";
    }

    void submitted(void* ptr) noexcept
    {
        auto& state = *static_cast<State*>(ptr);
        std::lock_guard lock{state.mutex};
        state.submitted = state.submitThread == std::this_thread::get_id();
    }

    void poll(void* ptr, bool executionComplete) noexcept
    {
        auto& state = *static_cast<State*>(ptr);
        {
            std::lock_guard lock{state.mutex};
            state.executed = executionComplete;
        }
        state.changed.notify_all();
    }

    void discover(plugin::Emit emit, void* context) noexcept
    {
        internal::emitMetric(
            {{"delayed", "delayed", {}, "test_async", MetricUnit::count, MetricScope::queueInterval},
             MetricStatus::unsupported,
             {},
             {}},
            emit,
            context);
    }

#if ALPAKA_METRICS_BAD_ASYNC_ABI
    constexpr std::uint32_t abi = 999;
#else
    constexpr std::uint32_t abi = plugin::asyncCollectorAbiVersion;
#endif
#if ALPAKA_METRICS_SYNC_ASYNC
    constexpr std::uint32_t capabilities = plugin::requiresDeviceSynchronization;
#else
    constexpr std::uint32_t capabilities = 0;
#endif
    plugin::AsyncCollector const collector{
        abi,
        sizeof(plugin::AsyncCollector),
        "test_async",
        "1",
        capabilities,
        create,
        destroy,
        begin,
        submitted,
        poll,
        discover};
} // namespace

extern "C" alpakaMetrics::plugin::AsyncCollector const* alpakaMetrics_getAsyncCollector() noexcept
{
    return &collector;
}

extern "C" void alpakaMetrics_testRelease(std::uint64_t id) noexcept
{
    std::lock_guard lock{mutex};
    if(auto found = states.find(id); found != states.end())
    {
        auto& state = *found->second;
        {
            std::lock_guard stateLock{state.mutex};
            state.released = true;
        }
        state.changed.notify_all();
    }
}
