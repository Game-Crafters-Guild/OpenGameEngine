#pragma once

// MeshReprovisionSource: resolves a MeshGPUKey to the CPU geometry that
// MeshGPURegistry::ReprovisionAfterDeviceRebuild re-uploads after a device
// rebuild.
//
// Responsibilities:
//   - Resolve asset-backed geometry from the AssetManager-resident ModelAsset
//   - Regenerate engine built-in primitives from their deterministic GUIDs
//   - Own the regenerated geometry for as long as the re-upload reads it
//
// Ownership:
//   - Holds the regenerated primitive meshes. One instance per reprovision pass;
//     pointers it returns are valid until it is destroyed.

// Mesh is held by value in a node-based map below; unordered_map is not specified
// for incomplete types, so this needs the definition rather than a declaration.
#include "Assets/ModelAsset.h"
#include "Engine/Rendering/MeshGPURegistry.h"

#include <unordered_map>

namespace GameEngine
{
namespace Engine::Renderer
{

class MeshReprovisionSource
{
  public:
    /// Resolve one entry's CPU geometry. Never triggers a disk load: a device
    /// rebuild is a synchronous RAM->VRAM re-upload, not streaming.
    Rendering::MeshGPUCpuSource operator()(const Rendering::MeshGPUKey& key);

  private:
    // Regenerated built-ins, keyed by primitive GUID. Node-based so a pointer
    // handed out for one key stays valid as later keys are inserted.
    std::unordered_map<GUID, Mesh> m_Regenerated;
};

} // namespace Engine::Renderer
} // namespace GameEngine
