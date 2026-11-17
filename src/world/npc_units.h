#pragma once

namespace aco {

// [world] npc_units=k (experiment, default 1 = off): more MetaAI units, i.e. more people rather than
// a higher cap. Every unit of a civilian spawner (MetaAIVirtualObjectiveSpawner) within 400 m of the
// player gets k-1 extra units of its archetype, 1-2 m away, created through the engine's own "add a
// unit of archetype A at P to spawner S" sequence (the one the save load uses to restore dead
// bodies, RVA 0x1dab8a0, without its kill). Units the MetaAI has already activated near the camera
// and in view are left until they are out of view, so no character pops up in front of the player.
// Runs on the MetaAI update thread: the MetaAI manager's vtable pointer is swapped to a copy whose
// update slot calls the game's update, then ours (a data write; no code is modified). Needs a cap
// with headroom ([world] npc_density), or k only moves people toward the player. Extra units die
// and are saved exactly like vanilla units. See docs/ENGINE_NOTES.md ("npc_units").
void npc_units_on_present();

} // namespace aco
