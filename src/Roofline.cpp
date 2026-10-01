// SPDX-License-Identifier: MPL-2.0
#include "alpakaMetrics/Roofline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace alpakaMetrics::internal::roofline
{
    void requirePositive(double value, char const* name)
    {
        if(!std::isfinite(value) || value <= 0.0)
            throw std::invalid_argument{std::string{name} + " must be finite and positive"};
    }

    double checked(long double value)
    {
        if(!std::isfinite(value) || value > std::numeric_limits<double>::max()
           || value < std::numeric_limits<double>::denorm_min())
            throw std::overflow_error{"Roofline calculation exceeds the representable positive range"};
        return static_cast<double>(value);
    }

    double read(MetricResult const& metric, MetricUnit unit)
    {
        if(!metric.isAvailable())
            throw std::invalid_argument{
                "Roofline metric unavailable: " + metric.descriptor.name + ": " + metric.diagnostic};
        if(metric.descriptor.unit != unit)
            throw std::invalid_argument{"Incorrect roofline metric unit: " + metric.descriptor.name};
        return metric.asDouble();
    }

    bool compatibleScopes(MetricScope timing, MetricScope counters)
    {
        if(timing == MetricScope::hostRegion)
            return counters == MetricScope::callingThread || counters == MetricScope::hostRegion;
        if(timing == MetricScope::queueInterval)
            return counters == MetricScope::queueWorkerThread || counters == MetricScope::queueInterval;
        return false;
    }
} // namespace alpakaMetrics::internal::roofline

namespace alpakaMetrics
{
    RooflineAnalysis analyzeRoofline(RooflineSample const& sample, RooflineCeilings const& ceilings)
    {
        using internal::roofline::checked;
        using internal::roofline::requirePositive;
        requirePositive(sample.elapsedSeconds, "Elapsed seconds");
        requirePositive(sample.floatingPointOperations, "Floating-point operation count");
        requirePositive(sample.transferredBytes, "Transferred byte count");
        requirePositive(ceilings.computeFlopsPerSecond, "Compute ceiling");
        requirePositive(ceilings.memoryBytesPerSecond, "Memory bandwidth ceiling");
        if(ceilings.computeMode.empty() || ceilings.memoryLevel.empty() || ceilings.provenance.empty())
            throw std::invalid_argument{"Roofline ceilings require compute mode, memory level and provenance"};

        long double const operations = sample.floatingPointOperations;
        long double const bytes = sample.transferredBytes;
        long double const seconds = sample.elapsedSeconds;
        long double const intensity = operations / bytes;
        long double const achieved = operations / seconds;
        long double const memoryCeiling = static_cast<long double>(ceilings.memoryBytesPerSecond) * intensity;
        long double const computeCeiling = ceilings.computeFlopsPerSecond;
        long double const ceiling = std::min(computeCeiling, memoryCeiling);
        RooflineAnalysis result;
        result.sample = sample;
        result.ceilings = ceilings;
        result.arithmeticIntensity = checked(intensity);
        result.achievedFlopsPerSecond = checked(achieved);
        result.achievedBytesPerSecond = checked(bytes / seconds);
        result.ridgePointFlopsPerByte = checked(computeCeiling / ceilings.memoryBytesPerSecond);
        result.rooflineFlopsPerSecond = checked(ceiling);
        result.fractionOfRoofline = checked(achieved / ceiling);
        result.limitingCeiling = memoryCeiling < computeCeiling   ? RooflineLimit::memoryBandwidth
                                 : memoryCeiling > computeCeiling ? RooflineLimit::compute
                                                                  : RooflineLimit::balanced;
        result.exceedsCeiling = achieved > ceiling;
        return result;
    }

    RooflineAnalysis analyzeRoofline(
        Result const& result,
        RooflineCeilings const& ceilings,
        RooflineMetrics const& metrics)
    {
        auto const& time = result.getMetric(metrics.elapsedTime);
        auto const& operations = result.getMetric(metrics.floatingPointOperations);
        auto const& bytes = result.getMetric(metrics.transferredBytes);
        RooflineSample sample{
            internal::roofline::read(time, MetricUnit::seconds),
            internal::roofline::read(operations, MetricUnit::count),
            internal::roofline::read(bytes, MetricUnit::bytes)};
        if(operations.descriptor.scope != bytes.descriptor.scope
           || !internal::roofline::compatibleScopes(time.descriptor.scope, operations.descriptor.scope))
            throw std::invalid_argument{
                "Roofline timing and counter attribution scopes do not describe a common region"};
        auto analysis = analyzeRoofline(sample, ceilings);
        analysis.measurementId = result.measurementId;
        analysis.label = result.label;
        analysis.timingScope = time.descriptor.scope;
        analysis.counterScope = operations.descriptor.scope;
        return analysis;
    }
} // namespace alpakaMetrics
