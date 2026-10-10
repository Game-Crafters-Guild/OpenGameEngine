#pragma once

#include "Physics/PhysicsTypes.h"

#include <filesystem>

namespace GameEngine::Editor
{

// The project's physics settings, stored in the "physics" object of
// <ProjectRoot>/.Editor/ProjectSettings.json:
//
//   { "physics": { "gravity": [0,-9.81,0], "fixedTimeStep": 0.0166,
//                  "maxSubSteps": 4, "collisionSteps": 1, "maxBodies": 65536,
//                  "layerCollisionMatrix": [ ...32 masks... ], ... } }
//
// Sole owner of that JSON shape, so the world bootstrap and the settings page
// cannot drift apart about how far the block extends. Note the nesting:
// SettingsStore's own key helpers are flat and treat a dot as a literal
// character, so these go through Json() directly.
//
// An absent key means "leave the engine default standing", so a project that
// never opened the page stays byte-identical to one with no physics block.
namespace PhysicsProjectSettings
{

// Reads every physics key the engine understands. A key that is absent, or holds
// a value of the wrong type, keeps the PhysicsWorldSettings default and costs
// only itself -- never the keys read after it.
Physics::PhysicsWorldSettings Load(const std::filesystem::path& workspaceRoot);

// Read-modify-write of the four keys the Physics settings page can change --
// gravity, fixedTimeStep, maxSubSteps, collisionSteps. Every other key in the
// block (the Jolt capacity limits, numThreads, layerCollisionMatrix, and
// anything a newer build writes that this one has never heard of) is left
// exactly as stored: this function must never be the reason a key disappears.
//
// All four are written unconditionally, so none can go stale -- the page has no
// affordance for unsetting one, and its reset-to-default writes the default
// value rather than removing the key.
//
// Returns false without writing anything if the stored file cannot be read (a
// truncated or merge-conflicted document): there is nothing to merge into, and
// writing would replace the whole file. A file that does not exist yet is not
// that case -- it reads as empty and is created.
bool SaveSimulationSettings(const std::filesystem::path& workspaceRoot,
                            const Physics::PhysicsWorldSettings& settings);

} // namespace PhysicsProjectSettings

} // namespace GameEngine::Editor
