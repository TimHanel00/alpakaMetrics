// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/Roofline.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void check(bool value, char const* message)
    {
        if(!value)
            throw std::runtime_error{message};
    }

    template<typename T_Exception, typename T_Fn>
    void expectThrow(T_Fn fn)
    {
        try
        {
            fn();
        }
        catch(T_Exception const&)
        {
            return;
        }
        throw std::runtime_error{"Invalid roofline input was accepted"};
    }

    alpakaMetrics::Result makeResult()
    {
        using namespace alpakaMetrics;
        Result result;
        result.measurementId = 17u;
        result.label = "test_region";
        result.metrics
            = {{{"elapsed_time", {}, {}, "test", MetricUnit::seconds, MetricScope::queueInterval},
                MetricStatus::available,
                2.0,
                {}},
               {{"floating_point_operations", {}, {}, "test", MetricUnit::count, MetricScope::queueWorkerThread},
                MetricStatus::available,
                std::uint64_t{400u},
                {}},
               {{"transferred_bytes", {}, {}, "test", MetricUnit::bytes, MetricScope::queueWorkerThread},
                MetricStatus::available,
                std::uint64_t{100u},
                {}}};
        return result;
    }
} // namespace

int main()
{
    using namespace alpakaMetrics;
    try
    {
        RooflineCeilings ceilings{1000.0, 100.0, "FP64", "DRAM", "synthetic test ceilings"};
        auto measured = makeResult();
        auto memory = analyzeRoofline(measured, ceilings);
        check(
            memory.arithmeticIntensity == 4.0 && memory.achievedFlopsPerSecond == 200.0
                && memory.achievedBytesPerSecond == 50.0 && memory.ridgePointFlopsPerByte == 10.0,
            "Roofline rates and intensity mismatch");
        check(
            memory.rooflineFlopsPerSecond == 400.0 && memory.fractionOfRoofline == 0.5
                && memory.limitingCeiling == RooflineLimit::memoryBandwidth && !memory.exceedsCeiling,
            "Bandwidth ceiling mismatch");
        check(
            memory.measurementId == 17u && memory.label == "test_region"
                && memory.timingScope == MetricScope::queueInterval
                && memory.counterScope == MetricScope::queueWorkerThread,
            "Roofline measurement provenance lost");
        auto compute = analyzeRoofline(RooflineSample{2.0, 400.0, 10.0}, ceilings);
        check(
            compute.rooflineFlopsPerSecond == 1000.0 && compute.fractionOfRoofline == 0.2
                && compute.limitingCeiling == RooflineLimit::compute,
            "Compute ceiling mismatch");
        auto balanced = analyzeRoofline(RooflineSample{2.0, 1000.0, 100.0}, ceilings);
        check(balanced.limitingCeiling == RooflineLimit::balanced, "Ridge point mismatch");
        auto exceeded = analyzeRoofline(RooflineSample{0.1, 400.0, 100.0}, ceilings);
        check(
            exceeded.exceedsCeiling && exceeded.fractionOfRoofline == 10.0,
            "Above-roofline observation was silently clamped");
        for(double invalid :
            {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        {
            expectThrow<std::invalid_argument>(
                [&] { static_cast<void>(analyzeRoofline(RooflineSample{invalid, 400.0, 100.0}, ceilings)); });
            expectThrow<std::invalid_argument>(
                [&] { static_cast<void>(analyzeRoofline(RooflineSample{2.0, invalid, 100.0}, ceilings)); });
            expectThrow<std::invalid_argument>(
                [&] { static_cast<void>(analyzeRoofline(RooflineSample{2.0, 400.0, invalid}, ceilings)); });
        }
        auto wrong = ceilings;
        wrong.computeFlopsPerSecond = 0.0;
        expectThrow<std::invalid_argument>([&] { static_cast<void>(analyzeRoofline(measured, wrong)); });
        wrong = ceilings;
        wrong.provenance.clear();
        expectThrow<std::invalid_argument>([&] { static_cast<void>(analyzeRoofline(measured, wrong)); });
        measured.metrics[2u].descriptor.unit = MetricUnit::count;
        expectThrow<std::invalid_argument>([&] { static_cast<void>(analyzeRoofline(measured, ceilings)); });
        measured = makeResult();
        measured.metrics[2u].status = MetricStatus::permissionDenied;
        measured.metrics[2u].value.reset();
        expectThrow<std::invalid_argument>([&] { static_cast<void>(analyzeRoofline(measured, ceilings)); });
        measured = makeResult();
        measured.metrics[2u].descriptor.scope = MetricScope::device;
        expectThrow<std::invalid_argument>([&] { static_cast<void>(analyzeRoofline(measured, ceilings)); });
        measured.metrics[1u].descriptor.scope = MetricScope::device;
        expectThrow<std::invalid_argument>([&] { static_cast<void>(analyzeRoofline(measured, ceilings)); });
        measured = makeResult();
        measured.metrics[0u].descriptor.scope = MetricScope::hostRegion;
        measured.metrics[1u].descriptor.scope = MetricScope::callingThread;
        measured.metrics[2u].descriptor.scope = MetricScope::callingThread;
        static_cast<void>(analyzeRoofline(measured, ceilings));
        measured.metrics[2u].descriptor.name = "dram_traffic";
        static_cast<void>(analyzeRoofline(measured, ceilings, RooflineMetrics{.transferredBytes = "dram_traffic"}));
        expectThrow<std::out_of_range>([&] { static_cast<void>(analyzeRoofline(measured, ceilings)); });
        expectThrow<std::overflow_error>(
            [&]
            {
                static_cast<void>(analyzeRoofline(
                    RooflineSample{std::numeric_limits<double>::denorm_min(), 400.0, 100.0},
                    ceilings));
            });
        std::cout << "Roofline arithmetic, validation and attribution checks passed\n";
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
