cmake_minimum_required(VERSION 3.24)
project(aco_dlss LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(DLSS_SDK ${CMAKE_SOURCE_DIR}/third_party/DLSS-310.9.16 CACHE PATH "NVIDIA DLSS SDK root")

# d3d11.dll proxy: loaded from the game directory, forwards to the system d3d11 (DXVK under Proton).
add_library(aco_d3d11 SHARED
  src/core/log.cpp
  src/core/hook.cpp
  src/core/vtidx.cpp
  src/core/shaders.cpp
  src/core/cbshadow.cpp
  src/trace/trace.cpp
  src/proxy.cpp
  src/d3d11.def)
set_target_properties(aco_d3d11 PROPERTIES OUTPUT_NAME d3d11 PREFIX "")
target_include_directories(aco_d3d11 PRIVATE src ${DLSS_SDK}/include)
target_compile_definitions(aco_d3d11 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE __USE_MINGW_ANSI_STDIO=1)
target_compile_options(aco_d3d11 PRIVATE -Wall -Wno-unknown-pragmas -fno-strict-aliasing)
target_link_options(aco_d3d11 PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
target_link_libraries(aco_d3d11 PRIVATE dxguid uuid)
