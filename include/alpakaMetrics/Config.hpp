// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "alpakaMetrics/Result.hpp"

#include <string>
#include <utility>
#include <vector>

namespace alpakaMetrics
{
    namespace metric
    {
        struct ElapsedTime
        {
        };

        struct Cycles
        {
        };

        struct Instructions
        {
        };

        struct FloatingPointOperations
        {
        };

        struct L2Misses
        {
        };

        struct L3Misses
        {
        };

        struct AchievedOccupancy
        {
        };

        struct Energy
        {
        };

        struct CoreFrequency
        {
        };

        inline constexpr ElapsedTime elapsedTime{};
        inline constexpr Cycles cycles{};
        inline constexpr Instructions instructions{};
        inline constexpr FloatingPointOperations floatingPointOperations{};
        inline constexpr L2Misses l2Misses{};
        inline constexpr L3Misses l3Misses{};
        inline constexpr AchievedOccupancy achievedOccupancy{};
        inline constexpr Energy energy{};
        inline constexpr CoreFrequency coreFrequency{};

        struct Native
        {
            std::string name;
        };

        inline auto native(std::string name) -> Native
        {
            return Native{std::move(name)};
        }
    } // namespace metric

    struct MetricRequest
    {
        std::string name;
        std::string papiName;

        MetricRequest(metric::ElapsedTime) : name{"elapsed_time"}
        {
        }

        MetricRequest(metric::Cycles) : name{"cycles"}, papiName{"PAPI_TOT_CYC"}
        {
        }

        MetricRequest(metric::Instructions) : name{"instructions"}, papiName{"PAPI_TOT_INS"}
        {
        }

        MetricRequest(metric::FloatingPointOperations) : name{"floating_point_operations"}, papiName{"PAPI_FP_OPS"}
        {
        }

        MetricRequest(metric::L2Misses) : name{"l2_misses"}, papiName{"PAPI_L2_TCM"}
        {
        }

        MetricRequest(metric::L3Misses) : name{"l3_misses"}, papiName{"PAPI_L3_TCM"}
        {
        }

        MetricRequest(metric::AchievedOccupancy) : name{"achieved_occupancy"}
        {
        }

        MetricRequest(metric::Energy) : name{"energy"}
        {
        }

        MetricRequest(metric::CoreFrequency) : name{"core_frequency"}
        {
        }

        MetricRequest(metric::Native request) : name{request.name}, papiName{std::move(request.name)}
        {
        }
    };

    /** Collection is single-pass. Unavailable counters remain explicit result entries. */
    struct Config
    {
        std::vector<MetricRequest> metrics{metric::elapsedTime};
        std::string label;
        /** providerDefined permits native component scopes; callingThread restricts to CPU thread counters. */
        MetricScope counterScope{MetricScope::providerDefined};
    };
} // namespace alpakaMetrics
