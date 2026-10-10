#pragma once

// Helpers the ModelThumbnailHandler implementation files share; not part of the handler's API.

#include <cstdint>
#include <memory>

namespace GameEngine
{
namespace ECS { class World; }

namespace ModelThumbnailShared
{

// A freshly spawned model's GPU instance (MeshGPUData::instanceIndex) is
// assigned by the extraction tick, which can lag the spawn by a frame
// (deferred ECS create + extraction ordering). We defer the render until the
// instance is live so the slot never bakes a blank thumbnail; this bounds the
// wait so a genuinely geometry-less (or broken) model still resolves instead
// of looping — and, since such a model never goes ready, can't head-of-line-
// block the rest of the grid for long.
inline constexpr uint32_t kThumbnailGpuReadyMaxRetries = 32u;

// Once geometry HAS extracted, a remaining not-ready state means the model's
// material textures are still binding (async upload — see
// AreSpawnedModelTexturesReady). Unlike a geometry-less model, these textures
// WILL land: a heavy model (e.g. a 9 MB glb with several maps) on a cold load
// binds well past the geometry cap, and baking it blank at 32 frames is the
// "last/heaviest tile stays grey" bug. So bound the texture wait far more
// loosely — large enough that any real cold load completes, still finite so a
// genuinely broken material can't wedge the drain forever. A valid model
// advances the instant its textures bind, so this ceiling only bites the rare
// broken-material case.
inline constexpr uint32_t kThumbnailTextureReadyMaxRetries = 600u;

// Every thumbnail world is built here so the render-resource release hooks
// cannot be forgotten on one of the spawn paths. A thumbnail world draws its
// skeleton runtimes and GPUScene instances from the same shared stores the
// primary world does, so it needs the same registrar. Deliberately serial: null
// JobSystem — a thumbnail world holds one model, so systems (extraction,
// TLAS) take their serial paths on it.
std::unique_ptr<ECS::World> MakeThumbnailWorld();

} // namespace ModelThumbnailShared
} // namespace GameEngine
