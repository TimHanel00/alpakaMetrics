# SPDX-License-Identifier: MPL-2.0
add_executable(alpakaMetrics_host host.cpp)
target_link_libraries(alpakaMetrics_host PRIVATE alpakaMetrics::alpakaMetrics)
alpaka_finalize(alpakaMetrics_host)

add_executable(alpakaMetrics_asynchronousVectorAdd asynchronousVectorAdd.cpp)
target_link_libraries(
    alpakaMetrics_asynchronousVectorAdd
    PRIVATE alpakaMetrics::alpakaMetrics
)
alpaka_finalize(alpakaMetrics_asynchronousVectorAdd)
if(alpakaMetrics_BUILD_TESTING)
    add_test(
        NAME alpakaMetrics.asynchronousVectorAdd
        COMMAND alpakaMetrics_asynchronousVectorAdd
    )
    set_tests_properties(
        alpakaMetrics.asynchronousVectorAdd
        PROPERTIES TIMEOUT 60
    )
endif()
