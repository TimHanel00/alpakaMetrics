// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/Config.hpp>
#include <alpakaMetrics/internal/PluginBridge.hpp>

#include <memory>
#include <thread>

namespace
{
    using namespace alpakaMetrics;

    struct Counters
    {
        std::vector<MetricResult> results;
        std::thread::id owner{std::this_thread::get_id()};
        bool running{};
    };

    void* create(plugin::Request const* requests, std::size_t size, plugin::Options const*) noexcept
    {
        try
        {
            auto counters = std::make_unique<Counters>();
            for(std::size_t i = 0; i < size; ++i)
                counters->results.push_back(
                    {{requests[i].name,
                      "test_exact_counter",
                      "Deterministic ABI fixture",
                      "test",
                      MetricUnit::count,
                      MetricScope::callingThread,
                      "count",
                      1},
                     MetricStatus::available,
                     std::uint64_t{9'007'199'254'740'993},
                     {}});
            return counters.release();
        }
        catch(...)
        {
            return nullptr;
        }
    }

    void destroy(void* ptr) noexcept
    {
        delete static_cast<Counters*>(ptr);
    }

    void begin(void* ptr) noexcept
    {
        auto& counters = *static_cast<Counters*>(ptr);
        counters.running = counters.owner == std::this_thread::get_id();
    }

    bool hasEvents(void const* ptr) noexcept
    {
        return !static_cast<Counters const*>(ptr)->results.empty();
    }

    bool isRunning(void const* ptr) noexcept
    {
        return static_cast<Counters const*>(ptr)->running;
    }

    void end(void* ptr, plugin::Emit emit, void* context) noexcept
    {
        auto& counters = *static_cast<Counters*>(ptr);
        for(auto const& result : counters.results)
            internal::emitMetric(result, emit, context);
        counters.running = false;
    }

    void discover(plugin::Emit emit, void* context) noexcept
    {
        internal::emitMetric(
            {{"test_exact_counter", "test_exact_counter", {}, "test", MetricUnit::count, MetricScope::callingThread},
             MetricStatus::unsupported,
             {},
             {}},
            emit,
            context);
    }
#if ALPAKA_METRICS_BAD_ABI
    constexpr std::uint32_t abi = 999;
#else
    constexpr std::uint32_t abi = plugin::collectorAbiVersion;
#endif
    plugin::Collector const collector{
        abi,
        sizeof(plugin::Collector),
        "test",
        "1",
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
