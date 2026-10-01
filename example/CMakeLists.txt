# SPDX-License-Identifier: MPL-2.0
add_executable(alpakaMetrics_host host.cpp)
target_link_libraries(alpakaMetrics_host PRIVATE alpakaMetrics::alpakaMetrics)
alpaka_finalize(alpakaMetrics_host)
