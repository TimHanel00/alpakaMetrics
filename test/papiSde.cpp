// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>
#include <sde_lib.h>

#include <future>
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
        // Exercise the device-provider lifecycle with real PAPI SDE counters and
        // a real asynchronous Alpaka host queue; this does not require GPU hardware.
        auto device = alpaka::onHost::makeDeviceSelector(alpaka::api::host, alpaka::deviceKind::cpu).makeDevice(0u);
        auto queue = device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled);
        alpakaMetrics::Config deviceConfig{
            .metrics
            = {alpakaMetrics::metric::elapsedTime, alpakaMetrics::metric::native("sde:::alpakaMetricsTest::integers")},
            .label = "device-lifecycle"};
        std::uint64_t launches{};
        queue.enqueueHostFn([&integers] { integers += 1000; });
        auto measurement = [&]
        {
            alpakaMetrics::internal::Counters counters{deviceConfig};
            return alpakaMetrics::internal::enqueueDevice(
                queue,
                deviceConfig,
                [&]
                {
                    queue.enqueueHostFn(
                        [&]
                        {
                            integers += 5;
                            ++launches;
                        });
                },
                std::ref(counters));
        }();
        // The thread-affine event set has already been destroyed. Reading the
        // retained snapshot from another thread must still work.
        auto deviceResult = std::async(std::launch::async, [measurement] { return measurement.getResults(); }).get();
        if(deviceResult.getMetric("sde:::alpakaMetricsTest::integers").asDouble() != 5.0
           || !deviceResult.getMetric("elapsed_time").isAvailable() || !deviceResult.synchronized
           || deviceResult.replayed || deviceResult.passCount != 1u || launches != 1u
           || deviceResult.label != "device-lifecycle")
            throw std::runtime_error{"Device lifecycle included preceding work, replayed, or lost its snapshot"};
        if(measurement.getResults().getMetric("sde:::alpakaMetricsTest::integers").asDouble() != 5.0)
            throw std::runtime_error{"Repeated measurement reads changed the native counter result"};
        {
            alpakaMetrics::internal::Counters counters{deviceConfig};
            bool threw{};
            try
            {
                static_cast<void>(alpakaMetrics::internal::enqueueDevice(
                    queue,
                    deviceConfig,
                    [&]
                    {
                        queue.enqueueHostFn([&integers] { integers += 2; });
                        throw std::runtime_error{"launch failure"};
                    },
                    std::ref(counters)));
            }
            catch(std::runtime_error const& error)
            {
                threw = std::string_view{error.what()} == "launch failure";
            }
            if(!threw || counters.isRunning())
                throw std::runtime_error{"Failed device launch left live counters or lost the exception"};
        }
        std::cout << "Real PAPI SDE collection, conversion and scope checks passed\n";
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
