# The Suffering (USA, Original Xbox, Title ID 0x4D570015), statically recompiled to Win64 & Linux x86_64
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.22)
project(TheSufferingRecomp LANGUAGES C CXX)
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# Guard against half-initialized CMakeCache.txt entries where per-config Clang/GNU flags defaulted to ""
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT "x${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "xMSVC")
    foreach(_lang IN ITEMS C CXX)
        if("${CMAKE_${_lang}_FLAGS_RELEASE}" STREQUAL "")
            set(CMAKE_${_lang}_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "Flags used by the ${_lang} compiler during RELEASE builds." FORCE)
        endif()
        if("${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}" STREQUAL "")
            if(WIN32)
                set(CMAKE_${_lang}_FLAGS_RELWITHDEBINFO "-O2 -DNDEBUG" CACHE STRING "Flags used by the ${_lang} compiler during RELWITHDEBINFO builds." FORCE)
            else()
                set(CMAKE_${_lang}_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG" CACHE STRING "Flags used by the ${_lang} compiler during RELWITHDEBINFO builds." FORCE)
            endif()
        endif()
        if("${CMAKE_${_lang}_FLAGS_MINSIZEREL}" STREQUAL "")
            set(CMAKE_${_lang}_FLAGS_MINSIZEREL "-Os -DNDEBUG" CACHE STRING "Flags used by the ${_lang} compiler during MINSIZEREL builds." FORCE)
        endif()
        if("${CMAKE_${_lang}_FLAGS_DEBUG}" STREQUAL "")
            if(WIN32)
                set(CMAKE_${_lang}_FLAGS_DEBUG "-O0" CACHE STRING "Flags used by the ${_lang} compiler during DEBUG builds." FORCE)
            else()
                set(CMAKE_${_lang}_FLAGS_DEBUG "-O0 -g" CACHE STRING "Flags used by the ${_lang} compiler during DEBUG builds." FORCE)
            endif()
        endif()
    endforeach()
endif()

# Enforce NDEBUG across all targets (including FetchContent subprojects) in non-Debug configs, keeping asserts in Debug
add_compile_definitions($<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:NDEBUG>)

# Ensure portable x86-64-v2 ISA (SSE4.1/4.2/POPCNT) even on non-preset Clang invocations
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_FLAGS MATCHES "-march=")
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-march=x86-64-v2>)
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

# Build-time path privacy & deterministic binary sanitization:
# Remap all host/workspace/SDK/build/FetchContent/user-profile prefixes (forward-slash, backslash,
# and Windows drive-letter case variants) BEFORE FetchContent_MakeAvailable() so zero personal paths
# enter __FILE__, _CRT_WIDE(__FILE__), DWARF/CodeView debug info, or PE RSDS PDB records in any config.
function(_suffering_collect_prefix_variants out_var raw_path)
    set(_variants "")
    if(NOT "${raw_path}" STREQUAL "")
        set(_cands "")
        file(TO_CMAKE_PATH "${raw_path}" _norm)
        list(APPEND _cands "${_norm}")
        if(EXISTS "${raw_path}")
            get_filename_component(_real "${raw_path}" REALPATH)
            file(TO_CMAKE_PATH "${_real}" _real_norm)
            list(APPEND _cands "${_real_norm}")
        endif()
        foreach(_p IN LISTS _cands)
            string(REGEX REPLACE "[/\\\\]+$" "" _p "${_p}")
            string(LENGTH "${_p}" _plen)
            if(_plen GREATER 3 AND NOT "${_p}" MATCHES "^[A-Za-z]:$" AND NOT "${_p}" MATCHES "[,;]")
                list(APPEND _variants "${_p}")
                if("${_p}" MATCHES "^([A-Za-z]):(.*)$")
                    string(TOUPPER "${CMAKE_MATCH_1}" _drv_up)
                    string(TOLOWER "${CMAKE_MATCH_1}" _drv_lo)
                    set(_rest "${CMAKE_MATCH_2}")
                    string(REPLACE "/" "\\" _rest_bs "${_rest}")
                    string(TOLOWER "${_p}" _p_lo)
                    string(REPLACE "/" "\\" _p_bs_lo "${_p_lo}")
                    list(APPEND _variants
                        "${_drv_up}:${_rest}"
                        "${_drv_up}:${_rest_bs}"
                        "${_drv_lo}:${_rest}"
                        "${_drv_lo}:${_rest_bs}"
                        "${_p_lo}"
                        "${_p_bs_lo}")
                endif()
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _variants)
    endif()
    set(${out_var} "${_variants}" PARENT_SCOPE)
endfunction()

set(_suffering_deps_root "${CMAKE_BINARY_DIR}/_deps")
if(DEFINED FETCHCONTENT_BASE_DIR AND NOT "${FETCHCONTENT_BASE_DIR}" STREQUAL "")
    set(_suffering_deps_root "${FETCHCONTENT_BASE_DIR}")
endif()

# Register most-specific directories first when deduplicating keys, then sort shortest-to-longest
# because Clang/GCC evaluate -ffile-prefix-map in reverse order (last matching flag wins).
set(_suffering_seen_prefixes "")
set(_suffering_keyed_maps "")
function(_suffering_register_prefix raw_path replacement)
    _suffering_collect_prefix_variants(_pfx_list "${raw_path}")
    set(_seen "${_suffering_seen_prefixes}")
    set(_maps "${_suffering_keyed_maps}")
    foreach(_old IN LISTS _pfx_list)
        list(FIND _seen "${_old}" _seen_idx)
        if(_seen_idx EQUAL -1)
            list(APPEND _seen "${_old}")
            string(LENGTH "${_old}" _olen)
            math(EXPR _olen_pad "100000 + ${_olen}")
            string(SUBSTRING "${_olen_pad}" 1 5 _olen_key)
            list(APPEND _maps "${_olen_key}|${_old}=${replacement}")
        endif()
    endforeach()
    set(_suffering_seen_prefixes "${_seen}" PARENT_SCOPE)
    set(_suffering_keyed_maps "${_maps}" PARENT_SCOPE)
endfunction()

_suffering_register_prefix("${FETCHCONTENT_SOURCE_DIR_IMGUI}" "deps/imgui")
_suffering_register_prefix("${FETCHCONTENT_SOURCE_DIR_SDL3}" "deps/sdl3")
_suffering_register_prefix("${FETCHCONTENT_SOURCE_DIR_ZYDIS}" "deps/zydis")
_suffering_register_prefix("${FETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS}" "deps/tomlplusplus")
_suffering_register_prefix("${_suffering_deps_root}/imgui-src" "deps/imgui")
_suffering_register_prefix("${_suffering_deps_root}/sdl3-src" "deps/sdl3")
_suffering_register_prefix("${_suffering_deps_root}/zydis-src" "deps/zydis")
_suffering_register_prefix("${_suffering_deps_root}/tomlplusplus-src" "deps/tomlplusplus")
_suffering_register_prefix("${_suffering_deps_root}" "deps")
_suffering_register_prefix("${CMAKE_BINARY_DIR}/_deps" "deps")
_suffering_register_prefix("${SDK}" "sdk")
_suffering_register_prefix("${XBE_SDK_DIR}" "sdk")
_suffering_register_prefix("${CMAKE_BINARY_DIR}" "build")
_suffering_register_prefix("${CMAKE_CURRENT_SOURCE_DIR}" ".")
_suffering_register_prefix("${CMAKE_SOURCE_DIR}" ".")
_suffering_register_prefix("$ENV{TMP}" "tmp")
_suffering_register_prefix("$ENV{TEMP}" "tmp")
_suffering_register_prefix("$ENV{TMPDIR}" "tmp")
_suffering_register_prefix("$ENV{USERPROFILE}" "user")
_suffering_register_prefix("$ENV{HOME}" "user")

list(SORT _suffering_keyed_maps)

include(CheckCXXCompilerFlag)
set(_saved_try_compile_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT "x${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "xMSVC")
    check_cxx_compiler_flag("-ffile-prefix-map=/a=/b" SUFFERING_HAS_FILE_PREFIX_MAP)
    check_cxx_compiler_flag("-ffile-compilation-dir=." SUFFERING_HAS_FILE_COMPILATION_DIR)
    check_cxx_compiler_flag("-fdebug-compilation-dir=." SUFFERING_HAS_DEBUG_COMPILATION_DIR)
    check_cxx_compiler_flag("-gno-record-command-line" SUFFERING_HAS_GNO_RECORD_CMDLINE)
    if(SUFFERING_HAS_FILE_COMPILATION_DIR)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-ffile-compilation-dir=.>)
    elseif(SUFFERING_HAS_DEBUG_COMPILATION_DIR)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-fdebug-compilation-dir=.>)
    endif()
    if(SUFFERING_HAS_GNO_RECORD_CMDLINE)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-gno-record-command-line>)
    endif()
    foreach(_entry IN LISTS _suffering_keyed_maps)
        string(SUBSTRING "${_entry}" 6 -1 _mapping)
        if(SUFFERING_HAS_FILE_PREFIX_MAP)
            add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${_mapping}>")
        else()
            add_compile_options(
                "$<$<COMPILE_LANGUAGE:C,CXX>:-fmacro-prefix-map=${_mapping}>"
                "$<$<COMPILE_LANGUAGE:C,CXX>:-fdebug-prefix-map=${_mapping}>")
        endif()
    endforeach()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND "x${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "xMSVC")
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/Brepro>)
    foreach(_entry IN LISTS _suffering_keyed_maps)
        string(SUBSTRING "${_entry}" 6 -1 _mapping)
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-clang:-ffile-prefix-map=${_mapping}>")
    endforeach()
elseif(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/Brepro>)
    foreach(_old IN LISTS _suffering_seen_prefixes)
        if("${_old}" MATCHES "\\\\")
            add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/d1trimfile:${_old}\\>")
        endif()
    endforeach()
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_saved_try_compile_type}")

if(WIN32 AND (MSVC OR "x${CMAKE_CXX_SIMULATE_ID}" STREQUAL "xMSVC"))
    add_link_options("LINKER:/Brepro" "LINKER:/PDBALTPATH:%_PDB%")
elseif(UNIX AND NOT APPLE)
    add_link_options("LINKER:--build-id=sha1" "$<$<CONFIG:Release>:LINKER:--strip-debug>")
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
    target_link_libraries(suffering PRIVATE shell32 ole32 uuid wintrust oldnames winhttp)
else()
    target_compile_definitions(suffering PRIVATE _FILE_OFFSET_BITS=64)
endif()
