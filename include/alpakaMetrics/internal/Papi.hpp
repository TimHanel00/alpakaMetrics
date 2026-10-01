// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "alpakaMetrics/Config.hpp"
#include "alpakaMetrics/Result.hpp"

#include <memory>

namespace alpakaMetrics::internal
{
    class PapiCounters
    {
    public:
        explicit PapiCounters(Config const& config);
        ~PapiCounters();
        PapiCounters(PapiCounters const&) = delete;
        PapiCounters& operator=(PapiCounters const&) = delete;
        void begin();
        std::vector<MetricResult> end();
        static std::vector<MetricDescriptor> getAvailableMetrics();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    std::uint64_t nextMeasurementId();
    void validateConfig(Config const& config);
    MetricResult makeUnavailable(MetricRequest const& request, MetricStatus status, std::string diagnostic);
} // namespace alpakaMetrics::internal
