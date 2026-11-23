# The Suffering (USA, Original Xbox, Title ID 0x4D570015), statically recompiled to Win64 & Linux x86_64
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.22)
project(TheSufferingRecomp LANGUAGES C CXX)
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Ensure portable x86-64-v2 ISA (SSE4.1/4.2/POPCNT) even on non-preset Clang invocations
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_FLAGS MATCHES "-march=")
    add_compile_options(-march=x86-64-v2)
endif()
# Guard FetchContent subprojects whose cmake_minimum_required predates CMake 4.x
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# Locate pinned XboxXBERecomp SDK
set(XBE_SDK_DIR "" CACHE PATH "Path to XboxXBERecomp SDK checkout")
if(XBE_SDK_DIR AND EXISTS "${XBE_SDK_DIR}/cmake/XbeRecomp.cmake")
    set(SDK "${XBE_SDK_DIR}")
elseif(DEFINED ENV{XBE_SDK_DIR} AND NOT "$ENV{XBE_SDK_DIR}" STREQUAL ""
       AND EXISTS "$ENV{XBE_SDK_DIR}/cmake/XbeRecomp.cmake")
    set(SDK "$ENV{XBE_SDK_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/XboxXBERecomp/cmake/XbeRecomp.cmake")
    set(SDK "${CMAKE_CURRENT_SOURCE_DIR}/XboxXBERecomp")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake")
    set(SDK "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
else()
    message(FATAL_ERROR
        "XboxXBERecomp SDK not found. Initialize the XboxXBERecomp submodule or set XBE_SDK_DIR.")
endif()

# Automated build-time private XBE extraction (read-only from user's XISO into .local/xbe/default.xbe)
if(DEFINED ENV{XBE_ISO_PATH} AND NOT "$ENV{XBE_ISO_PATH}" STREQUAL "")
    set(_default_iso "$ENV{XBE_ISO_PATH}")
else()
    set(_default_iso "${CMAKE_CURRENT_SOURCE_DIR}/Suffering, The (USA)/Suffering, The (USA).xiso.iso")
endif()
set(SUFFERING_ISO_PATH "${_default_iso}" CACHE FILEPATH
    "Path to The Suffering (USA) Original Xbox .iso/.xiso.iso disc image (opened strictly read-only)")
set(SUFFERING_XBE_RELPATH ".local/xbe/default.xbe" CACHE STRING
    "Workspace-relative path to the privately extracted default.xbe for build-time recompilation")

if(IS_ABSOLUTE "${SUFFERING_XBE_RELPATH}")
    set(_suffering_xbe_abs "${SUFFERING_XBE_RELPATH}")
else()
    set(_suffering_xbe_abs "${CMAKE_CURRENT_SOURCE_DIR}/${SUFFERING_XBE_RELPATH}")
endif()

option(SUFFERING_ALLOW_NO_XBE
    "Allow building host executable without a local XBE/ISO (CI/packaging mode; emits headers only)" OFF)
set(SUFFERING_EXPECTED_XBE_SHA1 "0a4c4e597da1f3b154099ab432c1326cc3dcb08c" CACHE STRING
    "Expected SHA-1 of default.xbe when building with SUFFERING_ALLOW_NO_XBE=ON")

if(NOT EXISTS "${_suffering_xbe_abs}" OR IS_DIRECTORY "${_suffering_xbe_abs}")
    if(EXISTS "${SUFFERING_ISO_PATH}" AND NOT IS_DIRECTORY "${SUFFERING_ISO_PATH}")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        message(STATUS "Extracting default.xbe (read-only) from ${SUFFERING_ISO_PATH}")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_xbe.py"
                    --iso "${SUFFERING_ISO_PATH}"
                    --out "${_suffering_xbe_abs}"
            RESULT_VARIABLE _extract_rc
        )
        if(NOT _extract_rc EQUAL 0)
            message(FATAL_ERROR
                "Failed to extract default.xbe from '${SUFFERING_ISO_PATH}' (exit code ${_extract_rc}).")
        endif()
    elseif(SUFFERING_ALLOW_NO_XBE)
        message(STATUS "SUFFERING_ALLOW_NO_XBE=ON: no local XBE/ISO found; building host binary with headers-only codegen.")
        set(XBE_NO_IMAGE ON)
        set(XBE_EXPECTED_SHA1 "${SUFFERING_EXPECTED_XBE_SHA1}")
    else()
        message(FATAL_ERROR
            "Private XBE not found at '${_suffering_xbe_abs}' and disc image not found at '${SUFFERING_ISO_PATH}'. "
            "Set XBE_ISO_PATH or -DSUFFERING_ISO_PATH=\"<path-to-xiso.iso>\" (or -DSUFFERING_ALLOW_NO_XBE=ON for CI).")
    endif()
endif()

include("${SDK}/cmake/XbeRecomp.cmake")

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
if(WIN32)
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/upscale/upscaler_vulkan\\.cpp$")
else()
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/upscale/upscaler_(manager|spatial|dlss|fsr3|xess)\\.cpp$")
endif()

# Partition count as written into config/game.toml (reconfigures automatically on edit)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml" GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 64)
endif()

# Target name = GAME_EXE in build.bat
add_xbe_image(suffering "${SUFFERING_XBE_RELPATH}" game.toml ${GAME_PARTITIONS} game)
target_include_directories(suffering PRIVATE "${sdl3_SOURCE_DIR}/include")
if(WIN32)
    target_sources(suffering PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/res/suffering.rc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/res/suffering.manifest"
    )
    target_link_libraries(suffering PRIVATE shell32 ole32 uuid wintrust)
else()
    target_compile_definitions(suffering PRIVATE _FILE_OFFSET_BITS=64)
endif()
