// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>

#include <filesystem>
#include <future>
#include <iostream>
#include <limits>

namespace
{
    void check(bool condition, char const* message)
    {
        if(!condition)
            throw std::runtime_error{message};
    }

    void testPlugins(std::string const& path, std::string const& badPath)
    {
        using namespace alpakaMetrics;
        auto info = getCollectorInfo(path);
        check(info.name == "test" && info.version == "1" && info.path == path, "Collector identity missing");
        check(
            getCollectorInfo(std::filesystem::relative(path).string()).path == info.path,
            "Relative collector path changed identity");
        check(getAvailableMetrics(path).at(0).collector == "test", "Discovery lost collector identity");
        for(auto const& unavailable : {badPath, path + ".missing"})
        {
            Config config{.metrics = {metric::elapsedTime, metric::instructions}, .collectorPlugin = unavailable};
            HostSideInstrumentation region{config};
            region.begin();
            auto result = region.end();
            check(result.getMetric("elapsed_time").isAvailable(), "Plugin failure lost timing");
            check(
                result.getMetric("instructions").status == MetricStatus::collectionFailed
                    && !result.getMetric("instructions").value,
                "Plugin failure fabricated a counter");
            check(!result.getMetric("instructions").diagnostic.empty(), "Plugin failure lost diagnostic");
        }
        Config config{.metrics = {metric::instructions}, .collectorPlugin = path};
        HostSideInstrumentation region{config};
        region.begin();
        auto result = region.end();
        auto const& metric = result.getMetric("instructions");
        check(std::get<std::uint64_t>(*metric.value) == 9'007'199'254'740'993, "Collector ABI lost integer precision");
        check(
            metric.descriptor.collector == "test" && metric.descriptor.collectorVersion == "1"
                && metric.descriptor.collectorPath == path,
            "Result lost collector provenance");
        internal::Counters denied{config, internal::DeviceCounterTarget{"cuda", 0}};
        check(
            !denied.hasEvents() && denied.end().at(0).status == MetricStatus::unsupportedScope,
            "Synchronous device collector enabled without opt-in");
        config.allowSynchronization = true;
        internal::Counters accepted{config, internal::DeviceCounterTarget{"cuda", 0}};
        check(accepted.hasEvents(), "Explicit synchronous opt-in was ignored");
    }

} // namespace

int main(int argc, char** argv)
{
    try
    {
        if(argc == 2 && std::string_view{argv[1]} == "--json")
        {
            alpakaMetrics::Result result;
            result.measurementId = 9'007'199'254'740'993;
            result.label = "quote\"slash\\line\n\t";
            result.provenance = {42, 7, alpakaMetrics::OperationKind::kernel, "queue", "device", "Host"};
            result.metrics
                = {{{"unsigned", {}, {}, "test"},
                    alpakaMetrics::MetricStatus::available,
                    std::uint64_t{9'007'199'254'740'993},
                    {}},
                   {{"signed", {}, {}, "test"}, alpakaMetrics::MetricStatus::available, std::int64_t{-5}, {}},
                   {{"double", {}, {}, "test"}, alpakaMetrics::MetricStatus::available, 1.25, {}},
                   {{"unavailable", {}, {}, "test"}, alpakaMetrics::MetricStatus::unsupported, {}, "missing"}};
            auto json = alpakaMetrics::toJson(result);
            result.metrics[2].value = std::numeric_limits<double>::infinity();
            bool rejected = false;
            try
            {
                static_cast<void>(alpakaMetrics::toJson(result));
            }
            catch(std::invalid_argument const&)
            {
                rejected = true;
            }
            check(rejected, "JSON accepted a non-finite metric");
            std::cout << json << '\n';
            return 0;
        }
        check(argc == 3, "Collector paths required");
        testPlugins(argv[1], argv[2]);
        std::cout << "Plugin ABI and provenance checks passed\n";
    }
    catch(std::exception const& failure)
    {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
