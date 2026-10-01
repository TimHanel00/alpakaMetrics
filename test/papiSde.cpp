// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/HostSideInstrumentation.hpp>
#include <sde_lib.h>

#include <iostream>
#include <limits>
#include <stdexcept>

int main()
{
    try
    {
        auto library = papi_sde_init("alpakaMetricsTest");
        long long integers = 13;
        long long nanojoules = 1'000'000'000ll;
        if(!library
           || papi_sde_register_counter(
                  library,
                  "integers",
                  PAPI_SDE_RO | PAPI_SDE_DELTA,
                  PAPI_SDE_long_long,
                  &integers)
                  != SDE_OK)
            throw std::runtime_error{"Cannot register PAPI software-defined counters"};
        if(papi_sde_register_counter(
               library,
               "nanojoules",
               PAPI_SDE_RO | PAPI_SDE_DELTA,
               PAPI_SDE_long_long,
               &nanojoules)
           != SDE_OK)
            throw std::runtime_error{"Cannot register PAPI conversion test counter"};
        alpakaMetrics::HostSideInstrumentation instrumentation{alpakaMetrics::Config{
            .metrics
            = {alpakaMetrics::metric::native("sde:::alpakaMetricsTest::integers"),
               alpakaMetrics::metric::map(
                   alpakaMetrics::metric::energy,
                   alpakaMetrics::metric::native(
                       "sde:::alpakaMetricsTest::nanojoules",
                       alpakaMetrics::MetricUnit::joules,
                       1.0e-9)),
               alpakaMetrics::metric::native("alpaka_metrics_invalid_event")}}};
        instrumentation.begin();
        integers += 7;
        nanojoules += 2'500'000'000ll;
        auto result = instrumentation.end();
        auto const& intResult = result.getMetric("sde:::alpakaMetricsTest::integers");
        if(!intResult.isAvailable() || !std::holds_alternative<std::int64_t>(*intResult.value)
           || std::get<std::int64_t>(*intResult.value) != 7)
            throw std::runtime_error{"PAPI integer counter delta was not preserved: " + intResult.diagnostic};
        if(result.getMetric("alpaka_metrics_invalid_event").isAvailable())
            throw std::runtime_error{"Missing PAPI event was reported available"};
        auto const& energy = result.getMetric("energy");
        if(!energy.isAvailable() || energy.asDouble() != 2.5
           || energy.descriptor.unit != alpakaMetrics::MetricUnit::joules
           || energy.descriptor.nativeToValueScale != 1.0e-9
           || energy.descriptor.scope != alpakaMetrics::MetricScope::providerDefined)
            throw std::runtime_error{"Explicit unit conversion or provider scope was lost"};
        instrumentation.begin();
        integers += 3;
        nanojoules += 1'250'000'000ll;
        auto second = instrumentation.end();
        if(second.getMetric("sde:::alpakaMetricsTest::integers").asDouble() != 3.0
           || second.getMetric("energy").asDouble() != 1.25)
            throw std::runtime_error{"Repeated PAPI regions did not reset their baselines"};
        alpakaMetrics::HostSideInstrumentation overflowing{alpakaMetrics::Config{
            .metrics = {alpakaMetrics::metric::native(
                "sde:::alpakaMetricsTest::integers",
                alpakaMetrics::MetricUnit::count,
                std::numeric_limits<double>::max())}}};
        overflowing.begin();
        integers += 2;
        auto overflowResult = overflowing.end().metrics.at(0u);
        if(overflowResult.status != alpakaMetrics::MetricStatus::collectionFailed || overflowResult.value)
            throw std::runtime_error{"Overflowed conversion was reported as a usable metric"};
        alpakaMetrics::HostSideInstrumentation scoped{alpakaMetrics::Config{
            .metrics = {alpakaMetrics::metric::map(
                alpakaMetrics::metric::energy,
                alpakaMetrics::metric::native(
                    "sde:::alpakaMetricsTest::nanojoules",
                    alpakaMetrics::MetricUnit::joules,
                    1.0e-9))},
            .counterScope = alpakaMetrics::MetricScope::callingThread}};
        scoped.begin();
        auto scopeResult = scoped.end().getMetric("energy");
        if(scopeResult.status != alpakaMetrics::MetricStatus::unsupportedScope || scopeResult.value
           || scopeResult.descriptor.unit != alpakaMetrics::MetricUnit::joules)
            throw std::runtime_error{"Explicit mapping overrode the provider attribution scope"};
        std::cout << "Real PAPI SDE collection, conversion and scope checks passed\n";
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
