# SPDX-License-Identifier: MPL-2.0
if(TARGET PAPI::PAPI)
    return()
endif()
if(alpakaMetrics_USE_SYSTEM_PAPI)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
    find_package(PAPI REQUIRED)
    if(NOT PAPI_INCLUDE_DIR)
        message(FATAL_ERROR "Building with system PAPI requires papi.h")
    endif()
    return()
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(
        FATAL_ERROR
        "Bundled PAPI currently requires Linux. Use system PAPI or disable alpakaMetrics_DEP_PAPI."
    )
endif()
include(ExternalProject)
set(alpakaMetrics_PAPI_REVISION
    72a3124d048dc5c89eb3f00c9f2866f4492b5383
    CACHE STRING
    "PAPI 7.2.0 source revision"
)
set(alpakaMetrics_PAPI_COMPONENTS
    ""
    CACHE STRING
    "Additional PAPI components, e.g. cuda;rocp_sdk;intel_gpu;rapl"
)
set(alpakaMetrics_PAPI_CONFIGURE_OPTIONS
    ""
    CACHE STRING
    "Additional PAPI Autotools configure arguments"
)
set(alpakaMetrics_PAPI_BUILD_JOBS
    2
    CACHE STRING
    "Parallel jobs for the PAPI build"
)
FetchContent_Declare(
    papi
    URL
        https://codeload.github.com/icl-utk-edu/papi/tar.gz/${alpakaMetrics_PAPI_REVISION}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR
    cmake-placeholder
)
FetchContent_MakeAvailable(papi)
find_program(alpakaMetrics_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
set(_papi_build "${papi_BINARY_DIR}/autotools")
set(_papi_install "${papi_BINARY_DIR}/install")
file(MAKE_DIRECTORY "${_papi_build}" "${_papi_install}/include")
string(REPLACE ";" " " _papi_components "${alpakaMetrics_PAPI_COMPONENTS}")
ExternalProject_Add(
    alpakaMetrics_papi_build
    SOURCE_DIR "${papi_SOURCE_DIR}/src"
    BINARY_DIR "${_papi_build}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR> <BINARY_DIR>
    COMMAND
        ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}"
        "CXX=${CMAKE_CXX_COMPILER}" <BINARY_DIR>/configure
        --prefix=${_papi_install} --libdir=${_papi_install}/lib
        --with-shared-lib=yes --with-static-lib=no
        --with-components=${_papi_components}
        ${alpakaMetrics_PAPI_CONFIGURE_OPTIONS}
    BUILD_COMMAND
        ${alpakaMetrics_MAKE_EXECUTABLE} -j${alpakaMetrics_PAPI_BUILD_JOBS}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -DPAPI_BUILD_DIR=<BINARY_DIR>
        -DPAPI_INSTALL_DIR=${_papi_install} -P
        ${CMAKE_CURRENT_LIST_DIR}/StagePapi.cmake
    BUILD_BYPRODUCTS
        "${_papi_install}/lib/libpapi.so"
        "${_papi_install}/lib/libsde.so"
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
    LOG_INSTALL TRUE
    LOG_OUTPUT_ON_FAILURE TRUE
)
ExternalProject_Add_StepDependencies(
    alpakaMetrics_papi_build
    install
    "${CMAKE_CURRENT_LIST_DIR}/StagePapi.cmake"
)
add_library(PAPI::PAPI SHARED IMPORTED GLOBAL)
set_target_properties(
    PAPI::PAPI
    PROPERTIES
        IMPORTED_LOCATION "${_papi_install}/lib/libpapi.so"
        INTERFACE_INCLUDE_DIRECTORIES "${_papi_install}/include"
)
add_dependencies(PAPI::PAPI alpakaMetrics_papi_build)
# Bundle only the runtime libraries; PAPI stays private to the implementation.
install(
    DIRECTORY "${_papi_install}/lib/"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    FILES_MATCHING
    PATTERN "*.so*"
)
install(
    FILES "${papi_SOURCE_DIR}/LICENSE.txt"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/alpakaMetrics/PAPI
)
install(
    FILES "${papi_SOURCE_DIR}/src/libpfm4/COPYING"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/alpakaMetrics/libpfm
)
