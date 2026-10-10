#pragma once

// HLOD bake driver (design v0.2 §8 "Bake HLOD" command). Composes the runtime
// half's world-facing bake: resolve the grid + admission config from the scene's
// HLODVolume, gather eligible static members (HlodMemberGather), bake the proxy
// clusters (HlodBaker::BakeScene), and write the sidecar .gehlod (HlodCache).
// This is the shared core the editor IPC command and the packaged cook both
// call; model geometry resolves through a caller-supplied ModelResolver so the
// driver is unit-testable with a World(nullptr) + in-memory models.

#include "Assets/HlodBaker.h"       // BakeStats
#include "Assets/HlodMemberGather.h" // ModelResolver

#include <cstdint>
#include <filesystem>
#include <string>

namespace GameEngine {

class AssetManager;
namespace ECS { class World; }

namespace Hlod {

// Resolve the sidecar .gehlod path for a scene, shared by the editor bake command
// and the scene-load reconcile so they always agree. Editor: a loose file under
// the project derived-artifact cache root (<cacheRoot>/Hlod/<scene>.gehlod).
// Packaged Player: a GUID-free sibling of the manifest (<assetRoot>/.hlod/
// <scene>.gehlod). Empty when neither root is available (raw test host).
std::filesystem::path ResolveHlodCacheFile(AssetManager& assetManager,
                                            const std::filesystem::path& scenePath);

// Resolve the grid + admission config from the scene's first enabled HLODVolume
// (v1: presence of one enabled volume is the scene-level opt-in; the first one
// applies globally). Returns false when the scene carries no enabled volume.
// Shared by the bake and by the scene-load reconcile, which recomputes the
// expected .gehlod ConfigHash from it so a stale bake is rejected instead of
// silently drawing old proxy geometry.
bool ResolveHlodVolumeConfig(GameEngine::ECS::World& world, GridConfig& outConfig);

enum class BakeOutcome : uint8 {
    Wrote,          // baked clusters written to the .gehlod
    NoVolume,       // no enabled HLODVolume in the scene -> nothing to bake
    NoMembers,      // a volume, but no eligible members were gathered
    WriteFailed,    // gather + bake succeeded, the atomic write did not
};

struct BakeDriverResult {
    BakeOutcome Outcome = BakeOutcome::NoVolume;
    BakeStats   Stats;              // valid when Outcome == Wrote
    uint32      GatheredMembers = 0;
    uint32      SkippedMembers = 0; // gather exclusions (skinned/morph/unresolved/...)
};

// Bake `world`'s HLOD clusters to `outPath`. Reads the first enabled HLODVolume
// for the grid + benefit + VB-budget config (v1: one volume applies globally).
// Returns NoVolume without touching outPath when no enabled volume is present,
// so a scene that never opted in pays nothing and writes nothing.
BakeDriverResult BakeHlodForWorld(GameEngine::ECS::World& world,
                                  const ModelResolver& resolveModel,
                                  const std::filesystem::path& outPath);

// The "Bake HLOD (project)" command: bake the open scene's live world to the
// scene's sidecar .gehlod (ResolveHlodCacheFile), resolving models through
// `assets`. A visible command rather than an auto-rebake, so artists check the
// proxies. The live editor world already has current WorldTransforms, and the
// reconcile at the next scene load reproduces the same values. Logs the stats
// on success; on failure returns false and fills outError (when non-null).
bool BakeHlodForScene(AssetManager& assets, GameEngine::ECS::World& world,
                      const std::filesystem::path& scenePath, std::string* outError);

} // namespace Hlod
} // namespace GameEngine
