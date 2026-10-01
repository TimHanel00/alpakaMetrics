// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/HostSideInstrumentation.hpp>
#include <sde_lib.h>

#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        auto library = papi_sde_init("alpakaMetricsTest");
        long long integers = 13;
        if(!library
           || papi_sde_register_counter(
                  library,
                  "integers",
                  PAPI_SDE_RO | PAPI_SDE_DELTA,
                  PAPI_SDE_long_long,
                  &integers)
                  != SDE_OK)
            throw std::runtime_error{"Cannot register PAPI software-defined counters"};
        alpakaMetrics::HostSideInstrumentation instrumentation{alpakaMetrics::Config{
            .metrics
            = {alpakaMetrics::metric::native("sde:::alpakaMetricsTest::integers"),
               alpakaMetrics::metric::native("alpaka_metrics_invalid_event")}}};
        instrumentation.begin();
        integers += 7;
        auto result = instrumentation.end();
        auto const& intResult = result.getMetric("sde:::alpakaMetricsTest::integers");
        if(!intResult.isAvailable() || !std::holds_alternative<std::int64_t>(*intResult.value)
           || std::get<std::int64_t>(*intResult.value) != 7)
            throw std::runtime_error{"PAPI integer counter delta was not preserved: " + intResult.diagnostic};
        if(result.getMetric("alpaka_metrics_invalid_event").isAvailable())
            throw std::runtime_error{"Missing PAPI event was reported available"};
        instrumentation.begin();
        integers += 3;
        auto second = instrumentation.end();
        if(second.getMetric("sde:::alpakaMetricsTest::integers").asDouble() != 3.0)
            throw std::runtime_error{"Repeated PAPI regions did not reset their baselines"};
        std::cout << "Real PAPI SDE integer collection passed\n";
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
