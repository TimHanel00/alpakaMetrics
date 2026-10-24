// SPDX-License-Identifier: MPL-2.0
#include <native/Activity.hpp>

#include <iostream>

namespace
{
    void check(bool condition, char const* message)
    {
        if(!condition)
            throw std::runtime_error{message};
    }

    struct Receiver
    {
        unsigned completions{};
        unsigned emissions{};
        bool success{};
        double value{};
        std::uint64_t id{};

        alpakaMetrics::plugin::AsyncSink sink()
        {
            return {
                this,
                [](void* context, std::uint64_t id, alpakaMetrics::plugin::Metric const* metric) noexcept
                {
                    auto& receiver = *static_cast<Receiver*>(context);
                    ++receiver.emissions;
                    receiver.id = id;
                    receiver.value = metric->doubleValue;
                },
                [](void* context, std::uint64_t id, bool success, char const*) noexcept
                {
                    auto& receiver = *static_cast<Receiver*>(context);
                    ++receiver.completions;
                    receiver.id = id;
                    receiver.success = success;
                }};
        }
    };
} // namespace

int main()
{
    using namespace alpakaMetrics;
    try
    {
        internal::activity::Registry registry;
        plugin::Request request{"device_execution_time", "", static_cast<std::uint32_t>(MetricUnit::seconds), 2.0};
        plugin::Options options{static_cast<std::uint32_t>(MetricScope::providerDefined), "cuda", 0};
        auto create = [&](Receiver& receiver, std::uint64_t id)
        { return registry.create(&request, 1, options, {id, 0}, receiver.sink(), "cuda", "test", "test.duration"); };

        Receiver first, second;
        auto a = create(first, 1);
        auto b = create(second, 2);
        registry.record(1, 100, 600);
        check(!first.completions, "Activity published before execution completed");
        registry.executed(b);
        check(!second.completions && registry.pending(), "Execution published before activity arrived");
        registry.record(2, 1000, 2000);
        check(second.success && second.id == 2 && !first.completions, "Out-of-order activity was misattributed");
        registry.executed(a);
        check(first.success && first.id == 1 && std::abs(first.value - 1.0e-6) < 1.0e-15, "Duration scaling failed");
        registry.record(1, 100, 600);
        registry.executed(a);
        check(first.completions == 1 && first.emissions == 1, "Operation delivered more than once");
        registry.destroy(a);
        registry.destroy(b);

        Receiver cancelled;
        auto c = create(cancelled, 3);
        registry.destroy(c);
        registry.record(3, 100, 600);
        registry.executed(c);
        check(!cancelled.completions && !cancelled.emissions, "Cancelled sink was called");

        Receiver invalid;
        auto d = create(invalid, 4);
        registry.record(4, 500, 100);
        check(invalid.completions == 1 && !invalid.success && !invalid.emissions, "Invalid timestamps accepted");
        registry.destroy(d);

        Receiver lost;
        auto e = create(lost, 5);
        registry.executed(e);
        {
            std::lock_guard lock{e->mutex};
            e->executedAt = std::chrono::steady_clock::now() - std::chrono::seconds{31};
        }
        check(!registry.pending() && lost.completions == 1 && !lost.success, "Missing activity did not expire");
        registry.destroy(e);
        return 0;
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
