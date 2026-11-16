#pragma once

namespace aco {

// Ambient NPC population (AC Origins v1.62).
//
// Systemic NPCs (civilians, workers, animals) are spawned by the engine's SpawningManager. Its data
// object SpawnSettings caps how many spawned entities may be alive at once:
//   cap = SpawnSettings.maxSpawned (+0x38, 200) * quality multiplier (Environment level: 0.75..1.2)
// When the count reaches the cap the manager stops spawning and evicts far entities, so in towns
// and cities the population sits at the cap. [world] npc_density=k multiplies maxSpawned by k.
// The value is game data (not saved); nothing else is changed. See docs/ENGINE_NOTES.md,
// "NPC population".
void npc_on_present();

} // namespace aco
