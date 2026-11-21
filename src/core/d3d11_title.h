#pragma once

namespace aco::title {
// Shader reflection, in float4 registers. InstanceParams has the same layout in both games.
#ifdef AEGIS_ODYSSEY
inline constexpr bool origins = false;
inline constexpr auto log = L"aegis.log", ini = L"aegis.ini";
inline constexpr auto game_ini = L"\\Assassin's Creed Odyssey\\ACOdyssey.ini";
inline constexpr auto mod_id = "aegis", name = "Aegis", sdk_name = "aegis";
inline constexpr unsigned texture0 = 85, pass_slot = 2, instance_slot = 4, temporal_slot = 6;
inline constexpr int eye = 1, view_to_world = 5, world_to_view = 17, projection = 25, view_proj = 29,
                     view_translation = 49, frustum = 57, clip_to_view = 68, clip_to_world = 72;
#else
inline constexpr bool origins = true;
inline constexpr auto log = L"pharos.log", ini = L"pharos.ini";
inline constexpr auto game_ini = L"\\Assassin's Creed Origins\\ACO.ini";
inline constexpr auto mod_id = "aco-dlss", name = "Pharos", sdk_name = "pharos";
inline constexpr unsigned texture0 = 86, pass_slot = 1, instance_slot = 3, temporal_slot = 5;
inline constexpr int eye = 0, view_to_world = 4, world_to_view = 12, projection = 20, view_proj = 24,
                     view_translation = 40, frustum = 46, clip_to_view = 57, clip_to_world = 61;
#endif
}
