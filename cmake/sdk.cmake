# sdk.cmake: the BMDS SDK this repository builds with, before project() (its toolchain file is
# in it).
#
# Copyright Jason Howard. All rights reserved.
# SPDX-License-Identifier: MIT
#
# The SDK is a BMDS release's bmds-sdk-VERSION.tar.gz, downloaded once into the build directory
# and checked against its SHA-256 (pinned below: change both together to move to another SDK).
# -DBMDS_SDK_DIR=<bmds>/sdk builds against a BMDS checkout instead (BMDS's own developers, or an
# SDK not released yet).

set(BMDS_SDK_VERSION "" CACHE STRING "The BMDS release whose SDK to download")
set(BMDS_SDK_SHA256 "" CACHE STRING "Its bmds-sdk-VERSION.tar.gz's SHA-256")
set(BMDS_SDK_DIR "" CACHE PATH "An SDK already here (<bmds>/sdk): nothing downloaded")

if(NOT BMDS_SDK_DIR)
        if(NOT BMDS_SDK_VERSION OR NOT BMDS_SDK_SHA256)
                message(FATAL_ERROR "No SDK: give -DBMDS_SDK_DIR=<bmds>/sdk, or pin a release's SDK "
                                    "(BMDS_SDK_VERSION and BMDS_SDK_SHA256 in CMakeLists.txt)")
        endif()
        set(name "bmds-sdk-${BMDS_SDK_VERSION}")
        set(archive "${CMAKE_BINARY_DIR}/_sdk/${name}.tar.gz")
        set(BMDS_SDK_DIR "${CMAKE_BINARY_DIR}/_sdk/${name}/sdk" CACHE PATH "" FORCE)
        if(NOT EXISTS "${BMDS_SDK_DIR}/cmake/BmdsProgram.cmake")
                set(url "https://github.com/jthwho/bmds/releases/download/v${BMDS_SDK_VERSION}/${name}.tar.gz")
                message(STATUS "Downloading the BMDS SDK ${BMDS_SDK_VERSION}")
                file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${BMDS_SDK_SHA256} STATUS got)
                list(GET got 0 code)
                if(NOT code EQUAL 0)
                        message(FATAL_ERROR "${url}: ${got}")
                endif()
                file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${CMAKE_BINARY_DIR}/_sdk")
        endif()
endif()

get_filename_component(BMDS_SDK_DIR "${BMDS_SDK_DIR}" ABSOLUTE)
if(NOT EXISTS "${BMDS_SDK_DIR}/cmake/BmdsProgram.cmake")
        message(FATAL_ERROR "${BMDS_SDK_DIR} isn't a BMDS SDK (no cmake/BmdsProgram.cmake)")
endif()
if(NOT CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE "${BMDS_SDK_DIR}/cmake/bmds-toolchain.cmake")
endif()
