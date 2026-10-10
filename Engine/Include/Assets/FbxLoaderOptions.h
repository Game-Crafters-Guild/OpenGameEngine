#pragma once

// Geometry-affecting FBX loader options, resolved per-asset (kv overrides) over
// the project defaults (ProjectSettings.json). These change the PARSED geometry
// — pivot baking, geometry-transform baking, skin-weight cleanup, tangent
// generation — so they are an input to the cooked-LOD cache exactly like the
// source bytes and the LOD config. The cook and the runtime must resolve the
// SAME options or the simplified indices are computed against different
// geometry. See mesh-lod-pipeline review F1.

#include "Assets/ModelImportOptions.h"
#include "Assets/RuntimeAssetMetadata.h"
#include "Types/Types.h"

#include <filesystem>

namespace GameEngine {

// Salt folded into cooked-LOD (HashFbxLoaderOptions) and HLOD proxy
// (ComputeHlodConfigHash) so a loader-geometry change recooks. Current
// geometry: ModelImport::MakeEngineConversion with default Mirror X
// (the right-to-left-handed bake). FBX source is FbxImport::Make(ufbx axes).
inline constexpr uint8 kFbxImportGeometryVersion = 6u;

// Salt the glTF loader folds into its parse-options hash beside the one above,
// so a change to what LoadGLTF produces for the same file recooks its LODs
// (their generated shells store whole vertices). v1: generated tangents for a
// normal-mapped primitive without TANGENT.
inline constexpr uint8 kGltfImportGeometryVersion = 1u;

struct FbxLoaderOptions {
    bool GenerateMissingTangents = true;
    bool CleanSkinWeights = true;
    bool AdjustPivots = true;
    bool PreserveGeometryTransforms = true;
    bool ImportEmbeddedTextures = true;
    // Bake + engine-axis mirrors. Shared by FBX / glTF / Blend; ufbx flags
    // above are FBX-only. kv keys stay assets.fbx.*.
    ModelImport::AxisOptions Axis{};

    // kv keys (assets.fbx.*), shared by the loader and the build-pipeline
    // staging so both sides agree on the exact strings.
    static constexpr const char* kUseGlobalKey = kFbxUseGlobalMetaKey;
    static constexpr const char* kGenerateMissingTangentsKey = kFbxGenerateMissingTangentsMetaKey;
    static constexpr const char* kCleanSkinWeightsKey = kFbxCleanSkinWeightsMetaKey;
    static constexpr const char* kAdjustPivotsKey = kFbxAdjustPivotsMetaKey;
    static constexpr const char* kPreserveGeometryTransformsKey = kFbxPreserveGeometryTransformsMetaKey;
    static constexpr const char* kImportEmbeddedTexturesKey = kFbxImportEmbeddedTexturesMetaKey;
    static constexpr const char* kBakeRotationKey = kFbxBakeRotationMetaKey;
    static constexpr const char* kBakeRotationAxesKey = kFbxBakeRotationAxesMetaKey;
    static constexpr const char* kMirrorAxesKey = kFbxMirrorAxesMetaKey;
};

// If ModelAsset::UsesAxisImportOptions for this path and the registry has no
// assets.fbx.mirrorAxes row, write the default "1,0,0" (mirror X). Safe to
// call on discovery and on first resolve; a present row is left alone.
void EnsureFbxMirrorAxesMeta(const std::filesystem::path& assetPath);

// Resolve the options a model parse will actually use for `assetPath`: project
// defaults (ProjectSettings.json) layered under per-asset kv overrides. This is
// the single source of truth for LoadFBX, LoadGLTF, LoadFromBlendData, the
// cook, and manifest staging. Format classification is ModelAsset's.
FbxLoaderOptions ResolveFbxLoaderOptions(const std::filesystem::path& assetPath);

// Stable, non-zero hash of the resolved options, folded into the cooked-LOD
// cache key so a parse-option change invalidates stale LODs. Non-zero by
// construction so 0 can mean "this format has no geometry-affecting parse
// options" (OBJ), which leaves those caches un-perturbed.
uint64 HashFbxLoaderOptions(const FbxLoaderOptions& options);

} // namespace GameEngine
