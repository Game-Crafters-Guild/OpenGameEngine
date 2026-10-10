#pragma once

// Test-only access to ModelAsset::LoadFBX / LoadGLTF / LoadFromBlendData with
// injected FbxLoaderOptions. Not part of the public ModelAsset API —
// production loads go through LoadFromData → ResolveFbxLoaderOptions → the
// format loader.

#include "Assets/FbxLoaderOptions.h"
#include "Assets/ModelAsset.h"
#include "Types/Types.h"

namespace GameEngine {

struct ModelAssetFbxTestAccess
{
    static bool Load(ModelAsset& asset, const Vector<uint8>& data, const FbxLoaderOptions& options);
    static bool LoadGltf(ModelAsset& asset, const Vector<uint8>& data, const FbxLoaderOptions& options);
    static bool LoadBlend(ModelAsset& asset, const Vector<uint8>& data, const FbxLoaderOptions& options);
    static uint64 ParseOptionsHash(const ModelAsset& asset);
    static void DemoteMasks(ModelAsset& asset, Vector<Mesh> meshes,
                            Vector<ImportedMaterialData> materials, Vector<EmbeddedImage> images);
};

} // namespace GameEngine
