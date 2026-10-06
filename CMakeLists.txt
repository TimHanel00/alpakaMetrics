# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.25)
project(alpakaMetrics VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(alpakaMetrics_DEP_PAPI "Enable PAPI counter collection" ON)
option(
    alpakaMetrics_USE_SYSTEM_PAPI
    "Use installed PAPI instead of FetchContent"
    OFF
)
option(alpakaMetrics_BUILD_TESTING "Build contract and execution tests" OFF)
option(alpakaMetrics_BUILD_EXAMPLES "Build examples" OFF)
set(alpakaMetrics_TUNER_SOURCE_DIR
    ""
    CACHE PATH
    "Optional alpaka3-tuner checkout for interoperability tests only"
)
set(alpakaMetrics_ALPAKA_REVISION
    b7d339d07056a9a2a6c4051cc3927157bc5f0d51
    CACHE STRING
    "Alpaka source revision"
)

if(NOT TARGET alpaka::alpaka)
    FetchContent_Declare(
        alpaka3
        URL
            https://codeload.github.com/alpaka-group/alpaka3/tar.gz/${alpakaMetrics_ALPAKA_REVISION}
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    FetchContent_MakeAvailable(alpaka3)
endif()
find_package(Threads REQUIRED)

# An imported forwarding target keeps Alpaka's recursive finalize discovery working
# without exporting the FetchContent dependency's non-exported build targets.
add_library(alpakaMetrics::alpakaDependency INTERFACE IMPORTED GLOBAL)
set_target_properties(
    alpakaMetrics::alpakaDependency
    PROPERTIES INTERFACE_LINK_LIBRARIES alpaka::alpaka
)

add_library(
    alpakaMetrics
    SHARED
    src/HostSideInstrumentation.cpp
    src/Collector.cpp
    src/Json.cpp
    src/Session.cpp
    src/Roofline.cpp
)
add_library(alpakaMetrics::alpakaMetrics ALIAS alpakaMetrics)
target_compile_features(alpakaMetrics PUBLIC cxx_std_20)
target_include_directories(
    alpakaMetrics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(
    alpakaMetrics
    PUBLIC alpakaMetrics::alpakaDependency Threads::Threads
    PRIVATE ${CMAKE_DL_LIBS}
)
set_target_properties(alpakaMetrics PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
if(UNIX)
    set_target_properties(alpakaMetrics PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
alpaka_finalize(alpakaMetrics)

if(alpakaMetrics_DEP_PAPI)
    include(cmake/Papi.cmake)
    add_library(
        alpakaMetrics_papi
        MODULE
        plugins/papi/Papi.cpp
        plugins/papi/Plugin.cpp
    )
    target_compile_features(alpakaMetrics_papi PRIVATE cxx_std_20)
    target_include_directories(
        alpakaMetrics_papi
        PRIVATE include plugins/papi/include
    )
    target_link_libraries(
        alpakaMetrics_papi
        PRIVATE PAPI::PAPI Threads::Threads
    )
    set_target_properties(alpakaMetrics_papi PROPERTIES INSTALL_RPATH "$ORIGIN")
    install(
        TARGETS alpakaMetrics_papi
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if(alpakaMetrics_BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
if(alpakaMetrics_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

install(TARGETS alpakaMetrics EXPORT alpakaMetricsTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    EXPORT alpakaMetricsTargets
    NAMESPACE alpakaMetrics::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/alpakaMetrics
)
configure_package_config_file(
    cmake/alpakaMetricsConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/alpakaMetricsConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/alpakaMetrics
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/alpakaMetricsConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/alpakaMetricsConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/alpakaMetricsConfigVersion.cmake
        cmake/FindPAPI.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/alpakaMetrics
)
