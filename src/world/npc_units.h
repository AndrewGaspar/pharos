#pragma once

namespace aco {

// [world] npc_units=k (experiment, default 1 = off): more MetaAI units, i.e. more people near the
// player rather than a higher cap. Each civilian spawner (MetaAIVirtualObjectiveSpawner) that comes
// within 260 m of the player while none of its units is activated yet gets k-1 extra units per unit:
// same archetype, placed 1-2 m from the original, created through the engine's own "add a unit of
// archetype A at P to spawner S" sequence (the one the save load uses to restore dead bodies,
// RVA 0x1dab8a0, without its kill). That runs on the MetaAI update thread: the MetaAI manager's
// vtable pointer is swapped to a copy whose update slot calls the game's update, then ours (a data
// write; no code is modified). Extra units die and are saved exactly like vanilla units: a dead one
// near the player at a save becomes a vanilla-format dead-body record (spawner, archetype,
// position). See docs/ENGINE_NOTES.md, "What the MetaAI save stores about units".
void npc_units_on_present();

} // namespace aco
