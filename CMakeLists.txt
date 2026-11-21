cmake_minimum_required(VERSION 3.24)
project(pharos LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(DLSS_SDK ${CMAKE_SOURCE_DIR}/third_party/DLSS-310.9.16 CACHE PATH "NVIDIA DLSS SDK root")

# d3d11.dll proxy: loaded from the game directory, forwards to the system d3d11 (DXVK under Proton).
set(D3D11_RENDER_SOURCES
  src/core/d3d11_forward.cpp
  src/core/log.cpp
  src/core/lab.cpp
  src/core/hook.cpp
  src/core/vtidx.cpp
  src/core/shaders.cpp
  src/core/cbshadow.cpp
  src/core/config.cpp
  src/core/latency.cpp
  src/core/iat.cpp
  src/core/dump.cpp
  src/core/mapstats.cpp
  src/reflex/reflex.cpp
  src/fg/camera.cpp
  src/fg/fg.cpp
  src/fg/fg_d3d12.cpp
  src/fg/vk_device_ext.cpp
  src/core/ctxhooks.cpp
  src/rig/capture.cpp
  src/rig/projection.cpp
  src/stereo/record.cpp
  src/stereo/stereo.cpp
  src/stereo/camera.cpp
  src/stereo/marker_history.cpp
  src/stereo/markers.cpp
  src/world/marker_targets.cpp
  src/stereo/system_cursor.cpp
  src/stereo/cursor.cpp
  src/stereo/temporal.cpp
  src/core/hyprland.cpp
  src/core/quiver.cpp
  src/dlss/ngx.cpp
  src/dlss/kernels.cpp
  src/dlss/upscaler.cpp
  src/trace/bloom_probe.cpp
  src/trace/trace.cpp
  src/proxy.cpp
  src/d3d11.def)
add_library(aco_d3d11 SHARED ${D3D11_RENDER_SOURCES}
  src/core/tlsext.cpp
  src/core/memtool.cpp
  src/world/world.cpp
  src/world/lodpatch.cpp
  src/world/npc.cpp
  src/world/npc_units.cpp
  src/world/teleport.cpp
  src/core/framedump.cpp
  src/core/peek.cpp
  src/cloth/cloth.cpp)
set(VULKAN_HEADERS ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers-1.4.329 CACHE PATH "Khronos Vulkan-Headers root")
function(configure_d3d11_renderer target)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME d3d11 PREFIX "")
  target_include_directories(${target} PRIVATE src ${DLSS_SDK}/include ${VULKAN_HEADERS}/include)
  target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE __USE_MINGW_ANSI_STDIO=1)
  target_compile_options(${target} PRIVATE -Wall -Wno-unknown-pragmas -fno-strict-aliasing)
  target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
  target_link_libraries(${target} PRIVATE dxguid uuid ole32 shell32 advapi32 ws2_32)
endfunction()
configure_d3d11_renderer(aco_d3d11)

# Build each port explicitly. The default package is still Origins.
function(add_anvil_port name executable proxy)
  add_library(${name} SHARED EXCLUDE_FROM_ALL src/ports/common.cpp src/core/log.cpp
    src/core/hook.cpp src/dlss/ngx.cpp ${ARGN})
  set_target_properties(${name} PROPERTIES OUTPUT_NAME ${proxy} PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ports/${name}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ports/${name}/lib")
  target_include_directories(${name} PRIVATE src ${DLSS_SDK}/include)
  target_compile_definitions(${name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    __USE_MINGW_ANSI_STDIO=1 PORT_NAME="${name}" PORT_EXE=L"${executable}")
  target_compile_options(${name} PRIVATE -Wall -Wno-unknown-pragmas)
  target_link_options(${name} PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_libraries(${name} PRIVATE dxguid uuid advapi32)
endfunction()
add_library(aegis SHARED EXCLUDE_FROM_ALL ${D3D11_RENDER_SOURCES} src/ports/common.cpp)
configure_d3d11_renderer(aegis)
target_compile_definitions(aegis PRIVATE AEGIS_ODYSSEY PORT_NAME="aegis" PORT_EXE=L"ACOdyssey")
set_target_properties(aegis PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ports/aegis"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ports/aegis/lib")
add_anvil_port(aegis-probe ACOdyssey d3d11 src/ports/aegis.cpp src/ports/aegis_sr.cpp src/core/d3d11_forward.cpp src/core/vtidx.cpp src/d3d11.def)
add_anvil_port(bifrost ACValhalla d3d12
  src/stereo/system_cursor.cpp
  src/world/marker_targets.cpp
  src/ports/bifrost_markers.cpp
  src/core/quiver.cpp
  src/core/hyprland.cpp
  src/ports/bifrost_wide.cpp
  src/fg/camera.cpp
  src/core/vtidx.cpp
  src/ports/bifrost_camera.cpp
  src/ports/bifrost_rate.cpp
  src/ports/bifrost_lists.cpp
  src/ports/bifrost_dual.cpp
  src/ports/bifrost_eye.cpp
  src/ports/bifrost_stereo.cpp
  src/ports/bifrost_replay.cpp
  src/ports/bifrost_composite.cpp
  src/ports/bifrost_sr.cpp
  src/ports/bifrost_verify.cpp
  src/ports/bifrost_present.cpp
  src/ports/bifrost_scene.cpp
  src/ports/bifrost.cpp
  src/ports/bifrost_resources.cpp
  src/ports/bifrost_uploads.cpp
  src/ports/bifrost_slots.cpp
  src/ports/bifrost.def)
target_link_libraries(bifrost PRIVATE ws2_32)
target_compile_definitions(bifrost PRIVATE BIFROST_VALHALLA)
