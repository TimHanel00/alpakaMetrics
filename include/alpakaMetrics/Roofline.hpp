// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Result.hpp>

#include <string>

namespace alpakaMetrics
{
    /** Independently established ceilings for one compute mode and memory level. */
    struct RooflineCeilings
    {
        double computeFlopsPerSecond{};
        double memoryBytesPerSecond{};
        std::string computeMode;
        std::string memoryLevel;
        std::string provenance;
    };

    /** All three quantities must describe the same workload and execution interval. */
    struct RooflineSample
    {
        double elapsedSeconds{};
        double floatingPointOperations{};
        double transferredBytes{};
    };

    struct RooflineMetrics
    {
        std::string elapsedTime{"elapsed_time"};
        std::string floatingPointOperations{"floating_point_operations"};
        std::string transferredBytes{"transferred_bytes"};
    };

    enum class RooflineLimit
    {
        memoryBandwidth,
        compute,
        balanced
    };

    struct RooflineAnalysis
    {
        RooflineSample sample;
        RooflineCeilings ceilings;
        double arithmeticIntensity{};
        double achievedFlopsPerSecond{};
        double achievedBytesPerSecond{};
        double ridgePointFlopsPerByte{};
        double rooflineFlopsPerSecond{};
        double fractionOfRoofline{};
        RooflineLimit limitingCeiling{RooflineLimit::balanced};
        bool exceedsCeiling{};
        std::uint64_t measurementId{};
        std::string label;
        MetricScope timingScope{MetricScope::providerDefined};
        MetricScope counterScope{MetricScope::providerDefined};
    };

    /** Analyze supplied positive finite measurements. Does not calibrate or collect counters. */
    [[nodiscard]] RooflineAnalysis analyzeRoofline(RooflineSample const& sample, RooflineCeilings const& ceilings);

    /** Analyze one result after validating metric units and attribution scopes. */
    [[nodiscard]] RooflineAnalysis analyzeRoofline(
        Result const& result,
        RooflineCeilings const& ceilings,
        RooflineMetrics const& metrics = {});
} // namespace alpakaMetrics
