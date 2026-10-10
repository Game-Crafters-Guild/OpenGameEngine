#pragma once

#include "Components/AssetRef.h"
#include "Types/StringId.h"
#include "Types/Types.h"
#include <string_view>
#include <utility>
#include <type_traits>

namespace GameEngine
{
namespace Components
{

struct MeshRenderer
{
    uint32 meshId = 0;     // Submesh index within the source ModelAsset
    // Optional submesh selector by source-node name (FBX node / DCC object).
    // When non-zero and `meshId` is 0, model resolution picks the submesh whose
    // name hashes to this id — the only way a scene importer that cannot know the
    // positional index (its source format addresses a mesh by file id, not by index) can address a
    // specific submesh of a multi-submesh model. `meshId != 0` always wins.
    // POD 8-byte hash, kept that way so the component stays one cache line.
    // Produce it with Engine::Renderer::InternMeshName, never with a bare
    // HashMeshName: interning is what lets the scene serializer write the NAME
    // and re-derive the id on load, so changing HashMeshName re-keys existing
    // content instead of orphaning it.
    StringId MeshNameId = 0;
    // MeshGPURegistry handle stored as raw uint64 (trivially copyable for ECS).
    // Cache filled on first bind from modelAssetGuid. Extraction uses this
    // when non-zero; zero means "not bound yet" (scene deserialize, spawn).
    uint64 meshGpuHandleId = 0;
    // Asset-backed material reference.
    // The extraction system resolves this via MaterialRegistry::Find.
    Components::MaterialRef materialAssetGuid;
    // Asset-backed model reference. Extraction bind uses this when the
    // handle is still zero (Ensure model GPU resources, then cache the handle).
    Components::ModelRef modelAssetGuid;
    uint32 renderLayerMask = 1u;
    bool castShadows = true;
    bool receiveShadows = true;
    bool motionVectors = true;
};

static_assert(std::is_trivially_copyable_v<MeshRenderer>, "MeshRenderer must be trivially copyable for ECS");
static_assert(std::is_standard_layout_v<MeshRenderer>, "MeshRenderer must be standard layout for ECS");
static_assert(sizeof(MeshRenderer) == 64,
              "MeshRenderer is one cache line; MeshNameId stays an 8-byte StringId (see MeshNameRegistry) for that reason");

// Fold a trailing "_LOD<N>" suffix (case-insensitive; N = one or more digits)
// off a mesh name, returning the base view. Consumed by ResolveSubmeshIndex's
// FALLBACK pass only: after the exact HashMeshName match fails, a bare base-name
// query ("Torso") can still bind a suffixed submesh ("Torso_LOD0"). A plain part
// suffix like "_0" (no "_LOD") is left untouched. Never applied inside
// HashMeshName — doing so would collapse distinct siblings to one id.
inline std::string_view FoldMeshLodSuffix(std::string_view name)
{
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    size_t digitsBegin = name.size();
    while (digitsBegin > 0 && name[digitsBegin - 1] >= '0' && name[digitsBegin - 1] <= '9')
        --digitsBegin;
    if (digitsBegin < name.size() && digitsBegin >= 4 &&
        name[digitsBegin - 4] == '_' &&
        lower(name[digitsBegin - 3]) == 'l' &&
        lower(name[digitsBegin - 2]) == 'o' &&
        lower(name[digitsBegin - 1]) == 'd')
    {
        return name.substr(0, digitsBegin - 4);
    }
    return name;
}

// Split a submesh name around an INTERIOR "_LOD<N>" token that a part suffix
// follows: "Tree_LOD1_1" -> {"Tree", "_1"}. A multi-material source node names
// its submeshes "<node>_<part>", so when the node itself carries the level tag
// the tag lands in the middle and the part index trails it. Which level a group
// exposes as its base is a property of the file's authoring, not of the
// geometry, so two imports of one mesh can differ only in that token; hashing
// the name without it lets a renderer authored against either one bind.
// A TRAILING "_LOD<N>" (no part suffix) is not a split: "Torso" and "Torso_LOD1"
// are distinct siblings and must keep distinct ids. Returns {name, {}} then.
inline std::pair<std::string_view, std::string_view> SplitMeshLodToken(std::string_view name)
{
    size_t partBegin = name.size();
    while (partBegin > 0 && name[partBegin - 1] >= '0' && name[partBegin - 1] <= '9')
        --partBegin;
    if (partBegin == name.size() || partBegin == 0 || name[partBegin - 1] != '_')
        return {name, {}};
    const std::string_view stem = name.substr(0, partBegin - 1);
    const std::string_view head = FoldMeshLodSuffix(stem);
    if (head.size() == stem.size())
        return {name, {}};
    return {head, name.substr(partBegin - 1)};
}

// Hash a submesh name into a MeshRenderer::MeshNameId. Folds ASCII case and an
// interior "_LOD<N>" token (see SplitMeshLodToken), so the same submesh binds
// whichever level of its group the importer exposes as the base. A trailing
// "_LOD<N>" is NOT folded here — that would collapse the distinct siblings
// "Torso" and "Torso_LOD1"; its tolerance is a separate fallback pass in
// ResolveSubmeshIndex via FoldMeshLodSuffix. FNV-1a keeps the id space
// consistent with the engine's StringId hashing. Applied identically on both
// sides of the match (schema authoring and the exact-match resolve pass), so it
// lives next to the component that owns the field.
inline StringId HashMeshName(std::string_view name)
{
    constexpr StringId kFnvOffsetBasis = 14695981039346656037ull;
    constexpr StringId kFnvPrime = 1099511628211ull;
    const auto [head, tail] = SplitMeshLodToken(name);
    StringId hash = kFnvOffsetBasis;
    for (std::string_view part : {head, tail})
    {
        for (char c : part)
        {
            const char folded = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            hash ^= static_cast<unsigned char>(folded);
            hash *= kFnvPrime;
        }
    }
    return hash;
}

} // namespace Components
} // namespace GameEngine
