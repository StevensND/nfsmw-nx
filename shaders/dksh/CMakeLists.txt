# nfsmw_dksh: the deko3d shader library to a DKSH cache, on the PC (docs/HowToDeko3d.md, section 3).
# Needs sdk/thirdparty with SPIRV-Cross, uam and xxHash (python tools/fetch_thirdparty.py [--deko3d]).
#   cmake -S shaders/dksh -B out/dksh -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build out/dksh
cmake_minimum_required(VERSION 3.20)
project(nfsmw_dksh C CXX)

get_filename_component(NFSMW_RAIZ "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(NFSMW_TERCEROS "${NFSMW_RAIZ}/sdk/thirdparty")
foreach(fuente SPIRV-Cross/spirv_glsl.cpp uam/uam_api.h xxHash/xxhash.h)
  if(NOT EXISTS "${NFSMW_TERCEROS}/${fuente}")
    message(FATAL_ERROR "Missing sdk/thirdparty/${fuente}: run python tools/fetch_thirdparty.py --deko3d")
  endif()
endforeach()

# uam as the library uamlib (its own CMakeLists, from SwitchWakerHD)
add_subdirectory("${NFSMW_TERCEROS}/uam" uam EXCLUDE_FROM_ALL)

# SPIRV-Cross: only the core and the GLSL backend
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

add_executable(nfsmw_dksh
  nfsmw_dksh.cpp
  "${NFSMW_RAIZ}/app/src/nfsmw_dk_glsl.cpp"
  "${NFSMW_RAIZ}/app/src/nfsmw_dk_cache_dksh.cpp"
  "${NFSMW_RAIZ}/app/src/nfsmw_shader_library.cpp")
target_include_directories(nfsmw_dksh PRIVATE "${NFSMW_RAIZ}/app/src" "${NFSMW_TERCEROS}/xxHash")
target_link_libraries(nfsmw_dksh PRIVATE uamlib spirv-cross-glsl spirv-cross-core)
set_target_properties(nfsmw_dksh PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
