#pragma once

#include "AssetCore/AssetTypes.h"

namespace GameEngine
{

// Runtime readers use these constants; package staging enumerates the same
// catalog so a declared runtime setting cannot be omitted from the manifest.
#define GE_RUNTIME_ASSET_METADATA_KEYS(ENTRY) \
    ENTRY(kAudioAllowVirtualizationMetaKey, Audio, "audio.allowVirtualization") \
    ENTRY(kAudioLoadPolicyMetaKey, Audio, "audio.loadPolicy") \
    ENTRY(kFbxAdjustPivotsMetaKey, Model, "assets.fbx.adjustPivots") \
    ENTRY(kFbxBakeRotationMetaKey, Model, "assets.fbx.bakeRotation") \
    ENTRY(kFbxBakeRotationAxesMetaKey, Model, "assets.fbx.bakeRotationAxes") \
    ENTRY(kFbxCleanSkinWeightsMetaKey, Model, "assets.fbx.cleanSkinWeights") \
    ENTRY(kFbxGenerateMissingTangentsMetaKey, Model, "assets.fbx.generateMissingTangents") \
    ENTRY(kFbxImportEmbeddedTexturesMetaKey, Model, "assets.fbx.importEmbeddedTextures") \
    ENTRY(kFbxMirrorAxesMetaKey, Model, "assets.fbx.mirrorAxes") \
    ENTRY(kFbxPreserveGeometryTransformsMetaKey, Model, "assets.fbx.preserveGeometryTransforms") \
    ENTRY(kFbxUseGlobalMetaKey, Model, "assets.fbx.useGlobalSettings") \
    ENTRY(kLodBorderRuleMetaKey, Model, "assets.lod.borderRule") \
    ENTRY(kLodCountMetaKey, Model, "assets.lod.count") \
    ENTRY(kLodErrorsMetaKey, Model, "assets.lod.errors") \
    ENTRY(kLodGenerateMetaKey, Model, "assets.lod.generate") \
    ENTRY(kLodRatiosMetaKey, Model, "assets.lod.ratios") \
    ENTRY(kLodSkinnedMetaKey, Model, "assets.lod.skinned") \
    ENTRY(kLodSlot1MetaKey, Model, "assets.lod.slot1") \
    ENTRY(kLodSlot2MetaKey, Model, "assets.lod.slot2") \
    ENTRY(kLodSlot3MetaKey, Model, "assets.lod.slot3") \
    ENTRY(kLodUseGlobalMetaKey, Model, "assets.lod.useGlobal") \
    ENTRY(kModelRigKindMetaKey, Model, "assets.model.rigKind") \
    ENTRY(kTextureAlphaCoverageMetaKey, Texture, "assets.texture.alphaCoverage") \
    ENTRY(kTextureAlphaCutoffMetaKey, Texture, "assets.texture.alphaCutoff") \
    ENTRY(kTextureColorSpaceMetaKey, Texture, "assets.texture.colorSpace") \
    ENTRY(kTextureCompressionMetaKey, Texture, "assets.texture.compression") \
    ENTRY(kTextureFilterMetaKey, Texture, "assets.texture.filter") \
    ENTRY(kTextureMipLimitMetaKey, Texture, "assets.texture.mipLimit") \
    ENTRY(kTextureMipsMetaKey, Texture, "assets.texture.mips") \
    ENTRY(kTextureNineSliceMetaKey, Texture, "assets.texture.nineSlice") \
    ENTRY(kSvgRasterSizeMetaKey, Texture, "assets.texture.svgRasterSize") \
    ENTRY(kTextureSwizzleMetaKey, Texture, "assets.texture.swizzle") \
    ENTRY(kTextureUsageMetaKey, Texture, "assets.texture.usage")

#define GE_DECLARE_RUNTIME_ASSET_METADATA_KEY(name, type, value) inline constexpr const char* name = value;
GE_RUNTIME_ASSET_METADATA_KEYS(GE_DECLARE_RUNTIME_ASSET_METADATA_KEY)
#undef GE_DECLARE_RUNTIME_ASSET_METADATA_KEY

/** A per-asset setting retained in the read-only packaged manifest. */
struct RuntimeAssetMetadataKey
{
    const char* Name;
    AssetType Type;
};

#define GE_RUNTIME_ASSET_METADATA_ENTRY(name, type, value) {name, AssetType::type},
inline constexpr RuntimeAssetMetadataKey kRuntimeAssetMetadataKeys[] = {
    GE_RUNTIME_ASSET_METADATA_KEYS(GE_RUNTIME_ASSET_METADATA_ENTRY)
};
#undef GE_RUNTIME_ASSET_METADATA_ENTRY
#undef GE_RUNTIME_ASSET_METADATA_KEYS

} // namespace GameEngine
