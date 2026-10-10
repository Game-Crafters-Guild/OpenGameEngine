#pragma once

#include "AssetCore/GUID.h"
#include "ECS/Entity.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine::ECS { class World; }
namespace GameEngine { struct MaterialDocument; }

namespace GameEngine::Editor
{

// Fills doc.texturePaths[slot] (the path-hash self-heal companion) for any
// texture slot that stores a resolvable project-source GUID but has no path
// companion yet, by looking up the texture's source-relative path from the asset
// registry. Skips embedded ("__embedded:N") refs, non-project textures, and slots
// that already carry a path. Call before serializing a material so a plain
// re-save upgrades a guid-only material into a self-healing [path,guid] one.
void BackfillMaterialTexturePaths(MaterialDocument& doc);

// Single chokepoint for writing a material to disk: back-fills the path
// companions (so the save is self-heal-ready) and returns the serialized JSON
// text (dumped, 2-space indent). Every editor save path should go through this
// rather than calling SerializeMaterialDocument directly, so none can forget the
// back-fill.
std::string SerializeMaterialDocumentForSave(const MaterialDocument& doc);

// Captured state for a single texture-slot assignment so the action can be
// undone/redone from outside the function. Filled in by
// AssignTextureToEntityMaterialSlot when an outState pointer is supplied.
struct TextureOnMeshAssignState
{
    ECS::EntityHandle entity{};
    std::string slotKey;
    GUID textureGuid; // texture assigned by the drop (null = clear)

    // MeshRenderer material fields before the assignment.
    GUID prevMaterialGuid;

    // Previous value of doc.textures[slot] on the existing material (empty
    // string if the slot was unset or the material was synthesized fresh).
    std::string prevSlotValue;

    // True if the drop created a new .material file because the entity had no
    // resolvable material. Undo deletes the file + unregisters it; Redo
    // recreates it at the same path (the AssetDatabase project store preserves
    // the original GUID for that path, so references round-trip safely).
    bool createdNewMaterial = false;

    // Populated when createdNewMaterial is true: path of the newly-created
    // .material file and its serialized JSON contents (captured after the
    // texture slot was written), so Redo can recreate the exact file.
    std::filesystem::path newMaterialPath;
    std::string newMaterialJson;

    // MeshRenderer material fields after the assignment.
    GUID newMaterialGuid;
};

// Deletes the material file on disk and unregisters it from the asset
// registry. No-op when matGuid is null. Used by TextureDropOnMeshCommand::Undo
// for the createdNewMaterial case.
void RemoveCreatedMaterialFile(const GUID& matGuid, const std::filesystem::path& path);

// Recreates a material file on disk from serialized JSON, re-registers it
// with the asset manager (the project store preserves its original GUID for
// the path), and re-registers the runtime Material with RenderServices so
// GPU state is ready for subsequent texture-slot writes.
bool RecreateMaterialFile(const std::filesystem::path& path,
                          const std::string& serializedJson);

// Assigns a texture asset to a named slot on the material referenced by an
// entity's MeshRenderer. Resolves the material via MeshRenderer::materialAssetGuid,
// edits the .material document's textures[slotKey], saves it to disk, reloads the
// asset, clears the shader compile cache for that material, and pushes the new
// texture binding to the runtime Material on the GPU.
//
// When outState is non-null, captures enough state for TextureDropOnMeshCommand
// to undo and redo the operation.
//
// Returns true if the entity had a resolvable material and the assignment was
// applied. Returns false if the entity has no MeshRenderer, no material assigned,
// or the material asset could not be loaded.
bool AssignTextureToEntityMaterialSlot(ECS::World& world,
                                       ECS::EntityHandle entity,
                                       const GUID& textureGuid,
                                       std::string_view slotKey,
                                       TextureOnMeshAssignState* outState = nullptr);

// Same as above, but allows specifying a surface shader override when creating
// a new material (e.g., "Surfaces/standard_pbr_extended.glsl" for PBR with AO).
bool AssignTextureToEntityMaterialSlot(ECS::World& world,
                                       ECS::EntityHandle entity,
                                       const GUID& textureGuid,
                                       std::string_view slotKey,
                                       TextureOnMeshAssignState* outState,
                                       const char* surfaceShaderOverride);

// Sets the MeshRenderer material GUID on the entity. Used by the undo
// command to restore/replay the pointer without touching material documents.
void SetEntityMaterialPointer(ECS::World& world,
                              ECS::EntityHandle entity,
                              const GUID& matGuid);

// Writes a texture slot value into a material document on disk, reloads the
// asset, clears the shader compile cache, and pushes the new binding to the
// runtime Material on the GPU. A no-op when matGuid is null. slotValue is the
// raw string stored in the material document (empty string clears the slot).
bool RewriteMaterialTextureSlot(const GUID& matGuid,
                                std::string_view slotKey,
                                std::string_view slotValue);

} // namespace GameEngine::Editor
