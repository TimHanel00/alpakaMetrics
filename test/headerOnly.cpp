// SPDX-License-Identifier: MPL-2.0
#include "headerOnlySupport.hpp"

#include <iostream>

namespace
{
    void check(bool condition, std::string_view message)
    {
        if(!condition)
            throw std::runtime_error{std::string{message}};
    }
} // namespace

int main()
{
    try
    {
        auto const first = alpakaMetrics::internal::nextMeasurementId();
        check(headerOnlyTest::nextId() == first + 1, "Translation units have separate measurement IDs");
        check(alpakaMetrics::internal::nextMeasurementId() == first + 2, "Measurement sequence diverged");
        check(
            headerOnlyTest::sharesProgress(alpakaMetrics::internal::session::progress()),
            "Translation units have separate progress services");

        auto device = alpaka::onHost::makeDeviceSelector(alpaka::api::host, alpaka::deviceKind::cpu).makeDevice(0u);
        alpakaMetrics::Session session;
        std::atomic<bool> rejectedDrain{};
        headerOnlyTest::setCallback(session, rejectedDrain);
        auto queue = alpakaMetrics::makeQueue(device.makeQueue(), session);
        auto measurement = queue.enqueueHostFn([] {});
        session.drain();
        session.setResultCallback({});
        check(rejectedDrain, "Progress-thread guard is not shared across translation units");
        auto const result = measurement.getResults();
        check(
            headerOnlyTest::serialize(result) == alpakaMetrics::toJson(result),
            "JSON differs across translation units");
        check(headerOnlyTest::analyze().arithmeticIntensity == 0.5, "Header-only roofline calculation failed");
        std::cout << "Header-only linkage and shared-state checks passed\n";
    }
    catch(std::exception const& failure)
    {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
