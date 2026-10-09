// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Result.hpp>

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

        /** Device timestamps for one correlated kernel, excluding queue markers. */
        struct DeviceExecutionTime
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
        inline constexpr DeviceExecutionTime deviceExecutionTime{};
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
        std::string nativeName;
        MetricUnit unit{MetricUnit::providerDefined};
        double scale{1.0};
        /** Compatibility spelling; new code should use nativeName or metric::map(). */
        std::string papiName;

        [[nodiscard]] std::string const& getNativeName() const
        {
            return nativeName.empty() ? papiName : nativeName;
        }

        MetricRequest(metric::ElapsedTime) : name{"elapsed_time"}
        {
        }

        MetricRequest(metric::DeviceExecutionTime) : name{"device_execution_time"}, unit{MetricUnit::seconds}
        {
        }

        MetricRequest(metric::Cycles) : name{"cycles"}
        {
        }

        MetricRequest(metric::Instructions) : name{"instructions"}
        {
        }

        MetricRequest(metric::FloatingPointOperations) : name{"floating_point_operations"}
        {
        }

        MetricRequest(metric::L2Misses) : name{"l2_misses"}
        {
        }

        MetricRequest(metric::L3Misses) : name{"l3_misses"}
        {
        }

        MetricRequest(metric::L1DataMisses) : name{"l1_data_misses"}
        {
        }

        MetricRequest(metric::L2Accesses) : name{"l2_accesses"}
        {
        }

        MetricRequest(metric::L3Accesses) : name{"l3_accesses"}
        {
        }

        MetricRequest(metric::BranchInstructions) : name{"branch_instructions"}
        {
        }

        MetricRequest(metric::BranchMispredictions) : name{"branch_mispredictions"}
        {
        }

        MetricRequest(metric::LoadInstructions) : name{"load_instructions"}
        {
        }

        MetricRequest(metric::StoreInstructions) : name{"store_instructions"}
        {
        }

        MetricRequest(metric::ResourceStallCycles) : name{"resource_stall_cycles"}
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
            , nativeName{std::move(request.name)}
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
            request.nativeName = std::move(source.name);
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
        /** Empty selects the environment override or the installed default collector. */
        std::string collectorPlugin;
        /** Permit providers that must wait around device counter collection. */
        bool allowSynchronization{false};
        /** Fallback module when no enabled API-specific provider handles the metric; empty defaults to PAPI. */
        std::string defaultCollectorPlugin;
    };
} // namespace alpakaMetrics
