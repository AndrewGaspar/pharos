cmake_minimum_required(VERSION 3.24)
project(pharos LANGUAGES C CXX)

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
  src/core/config.cpp
  src/core/latency.cpp
  src/core/iat.cpp
  src/core/tlsext.cpp
  src/core/dump.cpp
  src/core/memtool.cpp
  src/world/world.cpp
  src/world/lodpatch.cpp
  src/world/npc.cpp
  src/world/npc_units.cpp
  src/world/teleport.cpp
  src/core/framedump.cpp
  src/core/mapstats.cpp
  src/core/peek.cpp
  src/cloth/cloth.cpp
  src/reflex/reflex.cpp
  src/fg/fg.cpp
  src/fg/fg_d3d12.cpp
  src/fg/vk_device_ext.cpp
  src/core/ctxhooks.cpp
  src/stereo/record.cpp
  src/stereo/stereo.cpp
  src/stereo/camera.cpp
  src/stereo/temporal.cpp
  src/dlss/ngx.cpp
  src/dlss/kernels.cpp
  src/dlss/upscaler.cpp
  src/trace/trace.cpp
  src/proxy.cpp
  src/d3d11.def)
set_target_properties(aco_d3d11 PROPERTIES OUTPUT_NAME d3d11 PREFIX "")
set(VULKAN_HEADERS ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers-1.4.329 CACHE PATH "Khronos Vulkan-Headers root")
target_include_directories(aco_d3d11 PRIVATE src ${DLSS_SDK}/include ${VULKAN_HEADERS}/include)
target_compile_definitions(aco_d3d11 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE __USE_MINGW_ANSI_STDIO=1)
target_compile_options(aco_d3d11 PRIVATE -Wall -Wno-unknown-pragmas -fno-strict-aliasing)
target_link_options(aco_d3d11 PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
target_link_libraries(aco_d3d11 PRIVATE dxguid uuid ole32 shell32 advapi32)
