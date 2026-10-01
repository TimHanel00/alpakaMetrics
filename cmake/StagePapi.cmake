# SPDX-License-Identifier: MPL-2.0
# Stage only runtime libraries, headers and event definitions. Upstream make install
# invokes ldconfig and installs manuals; neither is needed for a private dependency.
file(
    MAKE_DIRECTORY
        "${PAPI_INSTALL_DIR}/lib"
        "${PAPI_INSTALL_DIR}/include"
        "${PAPI_INSTALL_DIR}/share/papi"
)
file(
    GLOB _libraries
    "${PAPI_BUILD_DIR}/libpapi.so*"
    "${PAPI_BUILD_DIR}/libpfm4/lib/libpfm.so*"
    "${PAPI_BUILD_DIR}/sde_lib/libsde.so*"
)
if(NOT _libraries)
    message(FATAL_ERROR "PAPI build produced no shared libraries")
endif()
file(COPY ${_libraries} DESTINATION "${PAPI_INSTALL_DIR}/lib")
if(EXISTS "${PAPI_INSTALL_DIR}/lib/libsde.so.1.0")
    file(CREATE_LINK libsde.so.1.0 "${PAPI_INSTALL_DIR}/lib/libsde.so" SYMBOLIC)
    file(
        CREATE_LINK libsde.so.1.0 "${PAPI_INSTALL_DIR}/lib/libsde.so.1"
        SYMBOLIC
    )
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB _papi_versions "${PAPI_INSTALL_DIR}/lib/libpapi.so.*")
    foreach(_library IN LISTS _papi_versions)
        if(NOT IS_SYMLINK "${_library}")
            file(
                RPATH_CHANGE
                FILE
                "${_library}"
                OLD_RPATH
                "${PAPI_INSTALL_DIR}/lib:${PAPI_BUILD_DIR}/libpfm4/lib:${PAPI_BUILD_DIR}"
                NEW_RPATH
                "$ORIGIN"
            )
        endif()
    endforeach()
endif()
file(GLOB _headers "${PAPI_BUILD_DIR}/papi*.h")
file(COPY ${_headers} DESTINATION "${PAPI_INSTALL_DIR}/include")
file(
    COPY "${PAPI_BUILD_DIR}/sde_lib/sde_lib.h"
    DESTINATION "${PAPI_INSTALL_DIR}/include"
)
file(
    COPY "${PAPI_BUILD_DIR}/papi_events.csv"
    DESTINATION "${PAPI_INSTALL_DIR}/share/papi"
)
