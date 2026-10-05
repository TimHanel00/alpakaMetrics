# SPDX-License-Identifier: MPL-2.0
add_executable(alpakaMetrics_contracts contracts.cpp)
target_link_libraries(
    alpakaMetrics_contracts
    PRIVATE alpakaMetrics::alpakaMetrics
)
alpaka_finalize(alpakaMetrics_contracts)
add_test(NAME alpakaMetrics.contracts COMMAND alpakaMetrics_contracts)
set_tests_properties(alpakaMetrics.contracts PROPERTIES TIMEOUT 30)

add_executable(alpakaMetrics_roofline roofline.cpp)
target_link_libraries(
    alpakaMetrics_roofline
    PRIVATE alpakaMetrics::alpakaMetrics
)
alpaka_finalize(alpakaMetrics_roofline)
add_test(NAME alpakaMetrics.roofline COMMAND alpakaMetrics_roofline)
set_tests_properties(alpakaMetrics.roofline PROPERTIES TIMEOUT 30)

add_executable(alpakaMetrics_deviceCounters deviceCounters.cpp)
target_link_libraries(
    alpakaMetrics_deviceCounters
    PRIVATE alpakaMetrics::alpakaMetrics
)
alpaka_finalize(alpakaMetrics_deviceCounters)
add_test(NAME alpakaMetrics.deviceCounters COMMAND alpakaMetrics_deviceCounters)
set_tests_properties(alpakaMetrics.deviceCounters PROPERTIES TIMEOUT 30)

add_executable(alpakaMetrics_deviceExecution deviceExecution.cpp)
target_link_libraries(
    alpakaMetrics_deviceExecution
    PRIVATE alpakaMetrics::alpakaMetrics
)
alpaka_finalize(alpakaMetrics_deviceExecution)
add_test(
    NAME alpakaMetrics.deviceExecution
    COMMAND alpakaMetrics_deviceExecution
)
set_tests_properties(
    alpakaMetrics.deviceExecution
    PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77
)

if(alpakaMetrics_TUNER_SOURCE_DIR)
    if(
        NOT EXISTS
            "${alpakaMetrics_TUNER_SOURCE_DIR}/include/alpakaTune/alpakaTune.hpp"
    )
        message(
            FATAL_ERROR
            "alpakaMetrics_TUNER_SOURCE_DIR is not an alpaka3-tuner checkout"
        )
    endif()
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        yaml_cpp
        URL https://codeload.github.com/jbeder/yaml-cpp/tar.gz/0.8.0
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    FetchContent_MakeAvailable(yaml_cpp)
    target_compile_features(yaml-cpp PRIVATE cxx_std_20)
    add_executable(alpakaMetrics_tunerIntegration tunerIntegration.cpp)
    target_include_directories(
        alpakaMetrics_tunerIntegration
        PRIVATE "${alpakaMetrics_TUNER_SOURCE_DIR}/include"
    )
    target_compile_definitions(
        alpakaMetrics_tunerIntegration
        PRIVATE
            ALPAKA_TUNE_HAS_JSON=0
            ALPAKA_TUNE_DEFAULT_CONFIG="${alpakaMetrics_TUNER_SOURCE_DIR}/config/alpakaTune.yaml"
    )
    target_link_libraries(
        alpakaMetrics_tunerIntegration
        PRIVATE alpakaMetrics::alpakaMetrics yaml-cpp::yaml-cpp
    )
    alpaka_finalize(alpakaMetrics_tunerIntegration)
    add_test(
        NAME alpakaMetrics.tunerIntegration
        COMMAND alpakaMetrics_tunerIntegration
    )
    set_tests_properties(alpakaMetrics.tunerIntegration PROPERTIES TIMEOUT 30)
endif()

if(
    TARGET alpakaMetrics_papi_build
    AND "sde" IN_LIST alpakaMetrics_PAPI_COMPONENTS
)
    add_library(PAPI::SDE SHARED IMPORTED)
    set_target_properties(
        PAPI::SDE
        PROPERTIES IMPORTED_LOCATION "${_papi_install}/lib/libsde.so"
    )
    add_dependencies(PAPI::SDE alpakaMetrics_papi_build)
    add_executable(alpakaMetrics_papiSde papiSde.cpp)
    target_link_libraries(
        alpakaMetrics_papiSde
        PRIVATE alpakaMetrics::alpakaMetrics PAPI::PAPI PAPI::SDE
    )
    alpaka_finalize(alpakaMetrics_papiSde)
    add_test(NAME alpakaMetrics.papiSde COMMAND alpakaMetrics_papiSde)
    set_tests_properties(alpakaMetrics.papiSde PROPERTIES TIMEOUT 30)
endif()

file(
    GLOB_RECURSE _headers
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/alpakaMetrics/*.hpp"
)
foreach(_header IN LISTS _headers)
    file(RELATIVE_PATH _relative "${PROJECT_SOURCE_DIR}/include" "${_header}")
    string(MAKE_C_IDENTIFIER "${_relative}" _name)
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/${_name}.cpp")
    file(WRITE "${_source}" "#include <${_relative}>\n")
    add_library(header_${_name} OBJECT "${_source}")
    target_link_libraries(header_${_name} PRIVATE alpakaMetrics::alpakaMetrics)
    alpaka_finalize(header_${_name})
endforeach()
