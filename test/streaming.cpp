// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>

#include <future>
#include <iostream>

namespace
{
    void check(bool condition, char const* message)
    {
        if(!condition)
            throw std::runtime_error{message};
    }

    void testStreaming()
    {
        using namespace alpakaMetrics;
        auto device = alpaka::onHost::makeDeviceSelector(alpaka::api::host, alpaka::deviceKind::cpu).makeDevice(0u);
        Session session;
        std::promise<void> release;
        auto gate = release.get_future().share();
        std::promise<Result> delivered;
        session.setResultCallback(
            [&](Result const& result)
            {
                if(result.label == "fast")
                    delivered.set_value(result);
            });
        auto slow = makeQueue(device.makeQueue(alpaka::queueKind::nonBlocking), session, Config{.label = "slow"});
        auto fast = makeQueue(device.makeQueue(alpaka::queueKind::nonBlocking), session, Config{.label = "fast"});
        auto first = slow.enqueueHostFn([gate] { gate.wait(); });
        auto second = fast.enqueueHostFn([] {});
        bool const incomplete = !first.tryGetResults().has_value();
        auto ready = delivered.get_future();
        bool const streamed = ready.wait_for(std::chrono::seconds{3}) == std::future_status::ready;
        release.set_value(); // Always release blocked queue work before asserting.
        session.drain();
        check(incomplete && streamed, "Result streaming waited for the earlier operation or an explicit read");
        auto result = ready.get();
        check(
            result.measurementId == second.getId() && result.provenance.sessionId == session.getId(),
            "Stream result lost its operation/session ID");
        auto slowResult = first.getResults();
        check(
            result.provenance.queueId != slowResult.provenance.queueId
                && result.provenance.kind == OperationKind::hostTask && !result.provenance.deviceName.empty(),
            "Queue/device/operation provenance missing");
        check(
            session.takeCompletedMeasurements().size() == 2 && session.getStats().pending == 0,
            "Session lost completed operations");
        fast.clearMeasurements();
        check(second.tryGetResults()->measurementId == result.measurementId, "History clearing invalidated results");
        session.setResultCallback([session](Result const&) { session.drain(); });
        auto callbackFailure = fast.enqueueHostFn([] {});
        session.drain();
        check(
            session.getStats().failedDeliveries == 1 && callbackFailure.getResults().metrics.at(0).isAvailable(),
            "Callback failure blocked progress or invalidated the operation");
        session.setResultCallback({});
        auto failed = fast.enqueueHostFn([] { throw std::runtime_error{"operation failure"}; });
        session.drain();
        bool threw = false;
        try
        {
            static_cast<void>(failed.getResults());
        }
        catch(std::runtime_error const&)
        {
            threw = true;
        }
        check(threw && session.getStats().failedDeliveries == 2, "Operation exception was swallowed by streaming");
    }

    void testCapacity()
    {
        using namespace alpakaMetrics;
        auto device = alpaka::onHost::makeDeviceSelector(alpaka::api::host, alpaka::deviceKind::cpu).makeDevice(0u);
        Session session{{1, 1}};
        auto queue = makeQueue(device.makeQueue(alpaka::queueKind::nonBlocking), session);
        std::promise<void> release;
        auto gate = release.get_future().share();
        auto first = queue.enqueueHostFn([gate] { gate.wait(); });
        auto second = queue.enqueueHostFn([] {});
        auto stats = session.getStats();
        release.set_value();
        session.drain();
        check(stats.droppedPending == 1 && stats.pending == 1, "Capacity overflow waited or was not reported");
        auto third = queue.enqueueHostFn([] {});
        session.drain();
        check(
            session.getStats().droppedCompleted == 1
                && session.takeCompletedMeasurements().at(0).getId() == third.getId(),
            "Completed storage did not enforce its capacity");
        check(
            first.getResults().measurementId == first.getId() && second.getResults().measurementId == second.getId(),
            "Stream capacity overflow invalidated direct handles");
    }
} // namespace

int main()
{
    try
    {
        testStreaming();
        testCapacity();
        std::cout << "Streaming, provenance and capacity checks passed\n";
    }
    catch(std::exception const& failure)
    {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
