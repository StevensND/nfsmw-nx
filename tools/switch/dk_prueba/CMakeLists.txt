# deko3d test NRO (dk_prueba.cpp): the renderer's core, uam and the game's shaders on the console.
# Built with devkitPro's toolchain (build.sh runs it in the devkitpro/devkita64 container):
#   cmake -S tools/switch/dk_prueba -B out/dk_prueba -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake
# Needs sdk/thirdparty with SPIRV-Cross, uam and xxHash (python tools/fetch_thirdparty.py --deko3d).
cmake_minimum_required(VERSION 3.20)
project(nfsmw_dk_prueba C CXX)

get_filename_component(NFSMW_RAIZ "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(NFSMW_TERCEROS "${NFSMW_RAIZ}/sdk/thirdparty")
set(NFSMW_APP "${NFSMW_RAIZ}/app/src")

add_subdirectory("${NFSMW_TERCEROS}/uam" uam EXCLUDE_FROM_ALL)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${NFSMW_TERCEROS}/SPIRV-Cross" spirv-cross EXCLUDE_FROM_ALL)

add_executable(dk_prueba
  dk_prueba.cpp
  "${NFSMW_APP}/nfsmw_dk_nucleo.cpp"
  "${NFSMW_APP}/nfsmw_dk_glsl.cpp"
  "${NFSMW_APP}/nfsmw_dk_cache_dksh.cpp"
  "${NFSMW_APP}/nfsmw_shader_library.cpp")
target_include_directories(dk_prueba PRIVATE "${NFSMW_APP}" "${NFSMW_TERCEROS}/xxHash")
set_target_properties(dk_prueba PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(dk_prueba PRIVATE -O2 -Wall)

# NFSMW_DEKO3D_DEPURAR: deko3d's debug library, which checks every call and reports errors readably
# (HowToDeko3d.md, section 6, item 1).
option(NFSMW_DEKO3D_DEPURAR "Enlazar la biblioteca de depuracion de deko3d (libdeko3dd)" OFF)
if(NFSMW_DEKO3D_DEPURAR)
  set(NFSMW_DEKO3D_LIB deko3dd)
else()
  set(NFSMW_DEKO3D_LIB deko3d)
endif()
target_link_libraries(dk_prueba PRIVATE uamlib spirv-cross-glsl spirv-cross-core ${NFSMW_DEKO3D_LIB})

nx_generate_nacp(dk_prueba.nacp NAME "nfsmw deko3d prueba" AUTHOR "nfsmw-nx" VERSION "0.1.0")
nx_create_nro(dk_prueba NACP dk_prueba.nacp)
