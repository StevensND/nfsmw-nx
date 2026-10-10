# Nintendo Switch / libnx cross toolchain for devkitPro devkitA64.
#
# Based on skate3recomp-nx's toolchain, which builds a ReXGlue title for Switch.
# Differences, and why:
#   - The SDK's own CMake handles the Switch specifics once REXGLUE_PLATFORM_SWITCH
#     is set (switch_compat headers, static runtime, NVK, NRO packaging); this
#     file only picks the compiler, flags and linker specs.
#   - The static NVK driver comes from the Mesa Horizon SDK
#     (REXGLUE_SWITCH_NVK_SDK).
#
# Usage:
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=<this file> \
#         -DREXGLUE_SWITCH_NVK_SDK=<mesa-...-horizon-sdk>/opt/devkitpro/portlibs/switch ...
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=<this file> -DREXGLUE_SWITCH_DEKO3D=ON ...   (deko3d, no Mesa)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(REXGLUE_PLATFORM_SWITCH ON CACHE BOOL "Build rexglue for Nintendo Switch/libnx")
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Tracy is unavailable on Switch" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "No perf counters on Switch" FORCE)
# REXGLUE_SWITCH_DEKO3D (-D on the command line): the deko3d renderer (docs/HowToDeko3d.md), without Vulkan.
if(REXGLUE_SWITCH_DEKO3D)
    set(REXGLUE_USE_VULKAN OFF CACHE BOOL "deko3d build: no Vulkan" FORCE)
else()
    set(REXGLUE_USE_VULKAN ON CACHE BOOL "NVK" FORCE)
endif()
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "No D3D12 on Switch" FORCE)

if(NOT DEFINED DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO "$ENV{DEVKITPRO}" CACHE PATH "devkitPro root")
    elseif(CMAKE_HOST_WIN32 AND EXISTS "C:/devkitPro")
        set(DEVKITPRO "C:/devkitPro" CACHE PATH "devkitPro root")
    else()
        set(DEVKITPRO "/opt/devkitpro" CACHE PATH "devkitPro root")
    endif()
endif()

set(_DEVKITA64 "${DEVKITPRO}/devkitA64")
set(_DEVKITA64_BIN "${_DEVKITA64}/bin")
set(_DEVKITA64_PREFIX "aarch64-none-elf-")
if(CMAKE_HOST_WIN32)
    set(_DEVKITA64_SUFFIX ".exe")
else()
    set(_DEVKITA64_SUFFIX "")
endif()

set(CMAKE_C_COMPILER "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}g++${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc-ar${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc-ranlib${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_NM "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}nm${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}objcopy${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}strip${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_AR "${CMAKE_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}" CACHE FILEPATH "" FORCE)

# Tegra X1 / libnx ABI. Local-exec TLS avoids the AArch64 linker relaxation
# corruption skate3recomp-nx observed in this very large static PIE on
# secondary threads.
set(_SWITCH_ARCH_FLAGS "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -fPIE -D__SWITCH__ -DNX -D_GNU_SOURCE -DSPDLOG_NO_TZ_OFFSET")
set(CMAKE_C_FLAGS_INIT "-ffunction-sections -fdata-sections ${_SWITCH_ARCH_FLAGS} -Wno-psabi")
set(CMAKE_CXX_FLAGS_INIT "-ffunction-sections -fdata-sections ${_SWITCH_ARCH_FLAGS} -Wno-psabi")
set(CMAKE_ASM_FLAGS_INIT "${_SWITCH_ARCH_FLAGS}")

# libnx's stock switch.specs resolves switch.ld through GCC's getenv spec
# function, which fails at the final link when DEVKITPRO is not exported in the
# shell (common on Windows). Write a build-local copy with the absolute path.
set(_SWITCH_SPECS_SOURCE "${DEVKITPRO}/libnx/switch.specs")
if(NOT EXISTS "${_SWITCH_SPECS_SOURCE}")
    message(FATAL_ERROR "libnx linker specs not found: ${_SWITCH_SPECS_SOURCE}")
endif()
file(READ "${_SWITCH_SPECS_SOURCE}" _SWITCH_SPECS_CONTENT)
string(REPLACE
    "%:getenv(DEVKITPRO /libnx/switch.ld)"
    "${DEVKITPRO}/libnx/switch.ld"
    _SWITCH_SPECS_CONTENT
    "${_SWITCH_SPECS_CONTENT}")
set(_SWITCH_SPECS_CONFIGURED "${CMAKE_BINARY_DIR}/switch.specs")
file(WRITE "${_SWITCH_SPECS_CONFIGURED}" "${_SWITCH_SPECS_CONTENT}")
# --allow-multiple-definition: NVK's Rust static libraries each bundle the Rust
# runtime.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=${_SWITCH_SPECS_CONFIGURED} -Wl,--gc-sections -Wl,--allow-multiple-definition -Wl,--no-relax")
set(CMAKE_DL_LIBS "")

include_directories(SYSTEM
    "${_DEVKITA64}/include"
    "${DEVKITPRO}/libnx/include"
    "${DEVKITPRO}/portlibs/switch/include")
link_directories(
    "${_DEVKITA64}/lib"
    "${DEVKITPRO}/libnx/lib"
    "${DEVKITPRO}/portlibs/switch/lib")

set(CMAKE_FIND_ROOT_PATH "${_DEVKITA64}" "${DEVKITPRO}/libnx" "${DEVKITPRO}/portlibs/switch")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
