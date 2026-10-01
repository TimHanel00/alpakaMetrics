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

        struct L1DataMisses
        {
        };

        struct L2Accesses
        {
        };

        struct L3Accesses
        {
        };

        struct BranchInstructions
        {
        };

        struct BranchMispredictions
        {
        };

        struct LoadInstructions
        {
        };

        struct StoreInstructions
        {
        };

        struct ResourceStallCycles
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

        struct TransferredBytes
        {
        };

        inline constexpr ElapsedTime elapsedTime{};
        inline constexpr Cycles cycles{};
        inline constexpr Instructions instructions{};
        inline constexpr FloatingPointOperations floatingPointOperations{};
        inline constexpr L2Misses l2Misses{};
        inline constexpr L3Misses l3Misses{};
        inline constexpr L1DataMisses l1DataMisses{};
        inline constexpr L2Accesses l2Accesses{};
        inline constexpr L3Accesses l3Accesses{};
        inline constexpr BranchInstructions branchInstructions{};
        inline constexpr BranchMispredictions branchMispredictions{};
        inline constexpr LoadInstructions loadInstructions{};
        inline constexpr StoreInstructions storeInstructions{};
        inline constexpr ResourceStallCycles resourceStallCycles{};
        inline constexpr AchievedOccupancy achievedOccupancy{};
        inline constexpr Energy energy{};
        inline constexpr CoreFrequency coreFrequency{};
        inline constexpr TransferredBytes transferredBytes{};

        struct Native
        {
            std::string name;
            MetricUnit unit{MetricUnit::providerDefined};
            double scale{1.0};
        };

        inline auto native(std::string name, MetricUnit unit = MetricUnit::providerDefined, double scale = 1.0)
            -> Native
        {
            return Native{std::move(name), unit, scale};
        }
    } // namespace metric

    struct MetricRequest
    {
        std::string name;
        std::string papiName;
        MetricUnit unit{MetricUnit::providerDefined};
        double scale{1.0};

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

        MetricRequest(metric::L1DataMisses) : name{"l1_data_misses"}, papiName{"PAPI_L1_DCM"}
        {
        }

        MetricRequest(metric::L2Accesses) : name{"l2_accesses"}, papiName{"PAPI_L2_TCA"}
        {
        }

        MetricRequest(metric::L3Accesses) : name{"l3_accesses"}, papiName{"PAPI_L3_TCA"}
        {
        }

        MetricRequest(metric::BranchInstructions) : name{"branch_instructions"}, papiName{"PAPI_BR_INS"}
        {
        }

        MetricRequest(metric::BranchMispredictions) : name{"branch_mispredictions"}, papiName{"PAPI_BR_MSP"}
        {
        }

        MetricRequest(metric::LoadInstructions) : name{"load_instructions"}, papiName{"PAPI_LD_INS"}
        {
        }

        MetricRequest(metric::StoreInstructions) : name{"store_instructions"}, papiName{"PAPI_SR_INS"}
        {
        }

        MetricRequest(metric::ResourceStallCycles) : name{"resource_stall_cycles"}, papiName{"PAPI_RES_STL"}
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

        MetricRequest(metric::TransferredBytes) : name{"transferred_bytes"}
        {
        }

        MetricRequest(metric::Native request)
            : name{request.name}
            , papiName{std::move(request.name)}
            , unit{request.unit}
            , scale{request.scale}
        {
        }
    };

    namespace metric
    {
        /** Bind a semantic metric to a provider event, with an explicit unit and conversion. */
        inline MetricRequest map(MetricRequest request, Native source)
        {
            request.papiName = std::move(source.name);
            request.unit = source.unit;
            request.scale = source.scale;
            return request;
        }
    } // namespace metric

    /** Collection is single-pass. Unavailable counters remain explicit result entries. */
    struct Config
    {
        std::vector<MetricRequest> metrics{metric::elapsedTime};
        std::string label;
        /** providerDefined permits native component scopes; callingThread restricts to CPU thread counters. */
        MetricScope counterScope{MetricScope::providerDefined};
    };
} // namespace alpakaMetrics
