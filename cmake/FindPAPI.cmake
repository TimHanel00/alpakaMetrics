# SPDX-License-Identifier: MPL-2.0
find_path(PAPI_INCLUDE_DIR papi.h)
find_library(PAPI_LIBRARY NAMES papi HINTS "${alpakaMetrics_PAPI_LIBRARY_HINT}")
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PAPI REQUIRED_VARS PAPI_LIBRARY)
if(PAPI_FOUND AND NOT TARGET PAPI::PAPI)
    add_library(PAPI::PAPI UNKNOWN IMPORTED)
    set_target_properties(
        PAPI::PAPI
        PROPERTIES IMPORTED_LOCATION "${PAPI_LIBRARY}"
    )
    if(PAPI_INCLUDE_DIR)
        set_target_properties(
            PAPI::PAPI
            PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${PAPI_INCLUDE_DIR}"
        )
    endif()
endif()
mark_as_advanced(PAPI_INCLUDE_DIR PAPI_LIBRARY)
