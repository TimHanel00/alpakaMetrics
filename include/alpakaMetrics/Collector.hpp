// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Result.hpp>

namespace alpakaMetrics
{
    struct CollectorInfo
    {
        std::string name;
        std::string version;
        std::string path;
        bool requiresDeviceSynchronization{};
    };

    /** Empty path uses ALPAKA_METRICS_COLLECTOR_PLUGIN or the module beside the core library.
     * Throws if unavailable or incompatible. Discovery does not guarantee collectibility.
     */
    CollectorInfo getCollectorInfo(std::string const& path = {});
    std::vector<MetricDescriptor> getAvailableMetrics(std::string const& path = {});
} // namespace alpakaMetrics
