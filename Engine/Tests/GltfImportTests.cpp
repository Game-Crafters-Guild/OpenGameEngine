// glTF import refusals and channel mapping: what the loader accepts before any accessor is read
// (cgltf_validate and the byteStride and extent limits it misses, extensionsRequired, buffers shorter
// than they declare), how morph targets, base attributes and indices import through the sparse unpack
// and what it refuses, the per-file budget on the arrays that primitives and clip channels sharing an
// accessor size, which animation channel paths the glTF clip path keeps, and which nodes a skin's
// skeleton counts, which objects' extras a model keeps within its bounds, what an animation's clip
// schema sets on its clip and when it is refused, what a material's emission imports as, and where
// each node that draws a shared mesh places it. Every document is built here; none is licensed content.

#include <gtest/gtest.h>

#include "Animation/AnimationClip.h"
#include "AnimationClipSchema.h"
#include "Assets/FbxLoaderOptions.h"
#include "Assets/ModelAsset.h"
#include "Assets/ModelExtras.h"
#include "Engine/Rendering/ModelMaterialBridge.h"
#include "AssetCore/GUID.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "EngineLogCapture.h"
#include "GltfAccessorUnpack.h"
#include "GltfAllocationBudget.h"
#include "GltfTestFiles.h"
#include "ModelAssetFbxTestAccess.h"
#include "ModelAssetLoadGltf.h"
#include "StagedTestPaths.h"
#include "Types/Fnv1a.h"

#include <cgltf.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

using Animation::AnimChannel;
using Animation::AnimationClip;
using Animation::AnimationClipSettings;
using Animation::AnimationEvent;
using Animation::AnimPath;
using Animation::ClipTranslationMode;

Vector<uint8> Bytes(std::string_view json)
{
    return Vector<uint8>(json.begin(), json.end());
}

// A triangle whose one material is `materialJson`, beside one image and one texture over it.
std::string TriangleWithMaterial(std::string_view materialJson)
{
    return std::string(R"({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_materials_emissive_strength"],
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}],
  "materials": [)") + std::string(materialJson) + R"(],
  "images": [{"uri": "data:image/png;base64,AA=="}],
  "textures": [{"source": 0}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";
}

// The one material `materialJson` imports as.
ImportedMaterialData ImportOnlyMaterial(std::string_view materialJson)
{
    ModelAsset asset(GUID::Generate(), "emission.gltf");
    EXPECT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(TriangleWithMaterial(materialJson)),
                                                  FbxLoaderOptions{}));
    EXPECT_EQ(asset.GetMaterials().size(), 1u);
    return asset.GetMaterials().empty() ? ImportedMaterialData{} : asset.GetMaterials()[0];
}

void ExpectEmissiveColor(const ImportedMaterialData& material, float r, float g, float b)
{
    EXPECT_FLOAT_EQ(material.EmissiveColor[0], r);
    EXPECT_FLOAT_EQ(material.EmissiveColor[1], g);
    EXPECT_FLOAT_EQ(material.EmissiveColor[2], b);
}

struct ModelLoad
{
    bool Loaded = false;
    std::vector<std::string> Lines;
    ModelExtras Extras;
};

// Loads `json` as `name` through the glTF loader with default import options, capturing the
// engine's warnings and errors.
ModelLoad LoadModel(std::string_view json, const std::filesystem::path& name)
{
    ModelLoad result;
    ModelAsset asset(GUID::Generate(), name);
    {
        TestLog::ScopedEngineLogCapture capture(&result.Lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("GltfImportTests sink is live");
        result.Loaded = ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(json), FbxLoaderOptions{});
        Logger::Log::Flush();
    }
    result.Extras = asset.GetExtras();
    return result;
}

// Loads `json` as `name` through the glTF loader with default import options; its only mesh.
Mesh LoadOnlyMesh(std::string_view json, const std::filesystem::path& name)
{
    ModelAsset asset(GUID::Generate(), name);
    EXPECT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(json), FbxLoaderOptions{}));
    EXPECT_EQ(asset.GetMeshCount(), 1u);
    return asset.GetMeshCount() == 1u ? asset.GetMesh(0) : Mesh{};
}

// Three float positions (1,0,0), (0,1,0), (0,0,1), then one u16 sparse index (3, two bytes of
// padding) and one float sparse value: the sparse index equals the accessor's count.
constexpr std::string_view kSparseIndexPastCount = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{
    "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
    "min": [0, 0, 0], "max": [1, 1, 1],
    "sparse": {"count": 1,
               "indices": {"bufferView": 1, "componentType": 5123},
               "values": {"bufferView": 2}}
  }],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 40, "byteLength": 12}],
  "buffers": [{"byteLength": 52, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AwAAAAAAoEAAAKBAAACgQA=="}]
})";

// Six positions declared over a 36-byte buffer that holds three: the accessor runs past the end of
// its buffer view and of the loaded buffer.
constexpr std::string_view kAccessorPastBufferView = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 6, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";

// A valid triangle whose file requires two extensions the engine does not implement and one it
// does.
constexpr std::string_view kRequiresUnimplementedExtensions = R"({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["EXT_meshopt_compression", "KHR_mesh_quantization", "KHR_draco_mesh_compression"],
  "extensionsRequired": ["EXT_meshopt_compression", "KHR_mesh_quantization", "KHR_draco_mesh_compression"],
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";

// Positions (1,0,0), (0,1,0), (0,0,1) stored as SHORT, eight bytes apart, under
// KHR_mesh_quantization.
constexpr std::string_view kRequiresMeshQuantization = R"({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_mesh_quantization"],
  "extensionsRequired": ["KHR_mesh_quantization"],
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5122, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 24, "byteStride": 8}],
  "buffers": [{"byteLength": 24, "uri": "data:application/octet-stream;base64,AQAAAAAAAAAAAAEAAAAAAAAAAAABAAAA"}]
})";

// A valid triangle whose file uses, without requiring, one extension the engine ignores and one it
// implements.
constexpr std::string_view kUsesOptionalExtensions = R"({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_materials_sheen", "MSFT_lod"],
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";

// A skin of three joints whose inverseBindMatrices accessor holds one identity matrix, the last 64
// bytes of the buffer.
constexpr std::string_view kSkinShortOfInverseBindMatrices = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0, 1]}],
  "nodes": [{"mesh": 0, "skin": 0}, {"name": "Hip", "children": [2]}, {"name": "Spine", "children": [3]},
            {"name": "Head"}],
  "skins": [{"joints": [1, 2, 3], "inverseBindMatrices": 1}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]},
                {"bufferView": 1, "componentType": 5126, "count": 1, "type": "MAT4"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 64}],
  "buffers": [{"byteLength": 100, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAACAPw=="}]
})";

// "Body" is skinned to "Hip", "Spine" and "Head" under "Armature", listed in the skin as Head, Hip,
// Spine. "HipSocket", "ArmatureProp" and "Camera" are nodes the skin never reaches.
constexpr std::string_view kSkinWithDecorationNodes = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0, 1, 7]}],
  "nodes": [{"name": "Body", "mesh": 0, "skin": 0}, {"name": "Armature", "children": [2, 6]},
            {"name": "Hip", "children": [3, 5]}, {"name": "Spine", "children": [4]}, {"name": "Head"},
            {"name": "HipSocket"}, {"name": "ArmatureProp"}, {"name": "Camera"}],
  "skins": [{"joints": [4, 2, 3]}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";

// "Mover" translates over four keys. A second channel names no node and samples four key times into
// one translation, the last 12 bytes of the buffer.
constexpr std::string_view kNodelessChannelClip = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "Mover"}],
  "animations": [{
    "name": "Slide",
    "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"},
                 {"input": 0, "output": 2, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}},
                 {"sampler": 1, "target": {"path": "translation"}}]
  }],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 4, "type": "SCALAR", "min": [0], "max": [3]},
    {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 1, "type": "VEC3"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 16},
                  {"buffer": 0, "byteOffset": 16, "byteLength": 48},
                  {"buffer": 0, "byteOffset": 64, "byteLength": 12}],
  "buffers": [{"byteLength": 76, "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAABAAABAQAAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAEAAAAAAAAAAAAAAQEAAAAAAAAAAAAAAEEEAABBBAAAQQQ=="}]
})";

// "Body" carries a one-target morph mesh; the animation drives its weights and translates "Mover".
constexpr std::string_view kWeightsAndTranslationClip = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0, 1]}],
  "nodes": [{"name": "Body", "mesh": 0}, {"name": "Mover"}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "targets": [{"POSITION": 1}]}],
              "weights": [0]}],
  "animations": [{
    "name": "Blink",
    "samplers": [{"input": 2, "output": 3, "interpolation": "LINEAR"},
                 {"input": 2, "output": 4, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": 0, "path": "weights"}},
                 {"sampler": 1, "target": {"node": 1, "path": "translation"}}]
  }],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 1]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
    {"bufferView": 3, "componentType": 5126, "count": 2, "type": "SCALAR"},
    {"bufferView": 4, "componentType": 5126, "count": 2, "type": "VEC3"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 72, "byteLength": 8},
                  {"buffer": 0, "byteOffset": 80, "byteLength": 8},
                  {"buffer": 0, "byteOffset": 88, "byteLength": 24}],
  "buffers": [{"byteLength": 112, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAD8AAAAAAAAAAAAAAD8AAAAAAAAAAAAAAD8AAAAAAAAAAAAAgD8AAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAA=="}]
})";

// A quad (positions (0,0,0), (1,0,0), (1,1,0), (0,1,0), normals +Z) with three morph targets named
// through extras.targetNames, default weights 0.25, 0.5, 0.75:
// - "Smile": POSITION sparse without a bufferView, vertex 1 += (0, 0.5, 0), vertex 3 += (0.25, 0, 0);
// - "Blink": POSITION with neither a bufferView nor a sparse block, so every delta is zero;
// - "Frown": POSITION dense, vertex 2 += (0, -0.5, 0.125); NORMAL sparse, vertex 2 += (0, 0.25, -0.25);
//   TANGENT sparse, vertex 0 += (0.125, 0, 0), vertex 2 += (0, 0.5, 0).
constexpr std::string_view kThreeSparseMorphTargets = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{
    "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2,
                    "targets": [{"POSITION": 3}, {"POSITION": 4}, {"POSITION": 5, "NORMAL": 6, "TANGENT": 7}]}],
    "weights": [0.25, 0.5, 0.75],
    "extras": {"targetNames": ["Smile", "Blink", "Frown"]}
  }],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"},
    {"componentType": 5126, "count": 4, "type": "VEC3",
     "sparse": {"count": 2, "indices": {"bufferView": 3, "componentType": 5123}, "values": {"bufferView": 4}}},
    {"componentType": 5126, "count": 4, "type": "VEC3"},
    {"bufferView": 5, "componentType": 5126, "count": 4, "type": "VEC3"},
    {"componentType": 5126, "count": 4, "type": "VEC3",
     "sparse": {"count": 1, "indices": {"bufferView": 6, "componentType": 5123}, "values": {"bufferView": 7}}},
    {"componentType": 5126, "count": 4, "type": "VEC3",
     "sparse": {"count": 2, "indices": {"bufferView": 8, "componentType": 5123}, "values": {"bufferView": 9}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 48},
                  {"buffer": 0, "byteOffset": 48, "byteLength": 48},
                  {"buffer": 0, "byteOffset": 96, "byteLength": 12},
                  {"buffer": 0, "byteOffset": 108, "byteLength": 4},
                  {"buffer": 0, "byteOffset": 112, "byteLength": 24},
                  {"buffer": 0, "byteOffset": 136, "byteLength": 48},
                  {"buffer": 0, "byteOffset": 184, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 188, "byteLength": 12},
                  {"buffer": 0, "byteOffset": 200, "byteLength": 4},
                  {"buffer": 0, "byteOffset": 204, "byteLength": 24}],
  "buffers": [{"byteLength": 228, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAACAPwAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIAAAACAAMAAQADAAAAAAAAAAA/AAAAAAAAgD4AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAC/AAAAPgAAAAAAAAAAAAAAAAIAAAAAAAAAAACAPgAAgL4AAAIAAAAAPgAAAAAAAAAAAAAAAAAAAD8AAAAA"}]
})";

// A triangle whose one morph target reads its POSITION deltas from a buffer with no uri, which holds
// no data once the buffers load.
constexpr std::string_view kMorphTargetInUnloadedBuffer = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "targets": [{"POSITION": 1}]}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]},
                {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 1, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"},
              {"byteLength": 36}]
})";

// A triangle with one morph target whose POSITION accessor has a dense base of zeros in a view with
// byteStride 16 and a sparse block: u16 indices 0 and 2, values (1, 0, 0) and (0, 1, 0) tightly
// packed in the last 24 bytes of the buffer.
constexpr std::string_view kSparseMorphBesideAStridedView = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "targets": [{"POSITION": 1}]}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3",
     "sparse": {"count": 2, "indices": {"bufferView": 2, "componentType": 5123}, "values": {"bufferView": 3}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 48, "byteStride": 16},
                  {"buffer": 0, "byteOffset": 84, "byteLength": 4},
                  {"buffer": 0, "byteOffset": 88, "byteLength": 24}],
  "buffers": [{"byteLength": 112, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAA=="}]
})";

// A triangle whose POSITION view declares byteStride STRIDE; TriangleWithPositionStride fills it in.
constexpr std::string_view kTriangleWithPositionStride = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 0]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36, "byteStride": STRIDE}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]
})";

// `json` with its one `placeholder` replaced by `value`.
std::string ReplacePlaceholder(std::string_view json, std::string_view placeholder, std::string_view value)
{
    std::string filled(json);
    filled.replace(filled.find(placeholder), placeholder.size(), value);
    return filled;
}

std::string TriangleWithPositionStride(std::string_view stride)
{
    return ReplacePlaceholder(kTriangleWithPositionStride, "STRIDE", stride);
}

// One vertex whose morph target's POSITION view declares byteStride 1 GiB, with a sparse block of two
// u8 indices (0, 0).
constexpr std::string_view kMorphViewWithAGibibyteStride = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "targets": [{"POSITION": 1}]}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 1, "type": "VEC3", "min": [0, 0, 0], "max": [0, 0, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 1, "type": "VEC3",
     "sparse": {"count": 2, "indices": {"bufferView": 2, "componentType": 5121}, "values": {"bufferView": 3}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 12},
                  {"buffer": 0, "byteOffset": 12, "byteLength": 12, "byteStride": 1073741824},
                  {"buffer": 0, "byteOffset": 24, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 28, "byteLength": 24}],
  "buffers": [{"byteLength": 52, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAA=="}]
})";

// A triangle whose POSITION accessor declares 2^62 + 1 elements four bytes apart in a 36-byte view:
// offset + stride * (count - 1) + 12, the extent cgltf_validate compares with the view, wraps to 12.
constexpr std::string_view kPositionCountWrappingItsExtent = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4611686018427387905, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 0]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36, "byteStride": 4}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]
})";

// A triangle whose morph target is a sparse block of 2^62 entries, u32 indices in a 4-byte view and
// VEC3 values in a 12-byte view: both sizes cgltf_validate compares with the views wrap to 0.
constexpr std::string_view kSparseCountWrappingItsExtent = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "targets": [{"POSITION": 1}]}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"componentType": 5126, "count": 3, "type": "VEC3",
     "sparse": {"count": 4611686018427387904, "indices": {"bufferView": 1, "componentType": 5125},
                "values": {"bufferView": 2}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 4},
                  {"buffer": 0, "byteOffset": 40, "byteLength": 12}],
  "buffers": [{"byteLength": 52, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAA=="}]
})";

// A triangle in a 52-byte buffer whose morph target is a sparse block of ENTRIES u32 indices and VEC3
// values, in buffer views of INDEX_BYTES and VALUE_BYTES bytes that start inside the buffer and run
// past its end; SparseViewsPastTheirBuffer fills them in.
constexpr std::string_view kSparseViewsPastTheirBuffer = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "targets": [{"POSITION": 1}]}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"componentType": 5126, "count": 3, "type": "VEC3",
     "sparse": {"count": ENTRIES, "indices": {"bufferView": 1, "componentType": 5125}, "values": {"bufferView": 2}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": INDEX_BYTES},
                  {"buffer": 0, "byteOffset": 40, "byteLength": VALUE_BYTES}],
  "buffers": [{"byteLength": 52, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAAAAAA=="}]
})";

std::string SparseViewsPastTheirBuffer(size_t entries)
{
    const std::string entriesText = std::to_string(entries);
    const std::string withEntries = ReplacePlaceholder(kSparseViewsPastTheirBuffer, "ENTRIES", entriesText);
    const std::string withIndices = ReplacePlaceholder(withEntries, "INDEX_BYTES", std::to_string(entries * 4u));
    return ReplacePlaceholder(withIndices, "VALUE_BYTES", std::to_string(entries * 12u));
}

// A triangle whose morph target's POSITION accessor is VEC4: sparse, one u16 index 2 and one value
// (1, 2, 3, 4).
constexpr std::string_view kMorphTargetOfFourComponents = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "targets": [{"POSITION": 1}]}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"componentType": 5126, "count": 3, "type": "VEC4",
     "sparse": {"count": 1, "indices": {"bufferView": 1, "componentType": 5123}, "values": {"bufferView": 2}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 40, "byteLength": 16}],
  "buffers": [{"byteLength": 56, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAgAAAAAAgD8AAABAAABAQAAAgEA="}]
})";

// A triangle whose POSITION accessor reads (0,0,0), (1,0,0), (0,1,0) from its buffer view and carries a
// sparse block: one u16 index 2 and one value (0, 0, 1).
constexpr std::string_view kSparsePositionOverABufferView = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{
    "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 1],
    "sparse": {"count": 1, "indices": {"bufferView": 1, "componentType": 5123}, "values": {"bufferView": 2}}
  }],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 40, "byteLength": 12}],
  "buffers": [{"byteLength": 52, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAgAAAAAAAAAAAAAAAACAPw=="}]
})";

// A triangle whose POSITION accessor has no buffer view: a sparse block of u8 indices 1 and 2 and values
// (1, 0, 0) and (0, 1, 0) over zeros.
constexpr std::string_view kSparsePositionWithoutABufferView = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{
    "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0],
    "sparse": {"count": 2, "indices": {"bufferView": 0, "componentType": 5121}, "values": {"bufferView": 1}}
  }],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 4, "byteLength": 24}],
  "buffers": [{"byteLength": 28, "uri": "data:application/octet-stream;base64,AQIAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAA=="}]
})";

// A triangle (0,0,0), (1,0,0), (0,1,0) with two sparse attributes:
// - NORMAL: (0, 0, 1) for every vertex in its buffer view, and u16 index 1 with value (0, 1, 0);
// - TEXCOORD_0: no buffer view, and u16 index 2 with value (0.5, 0.25).
constexpr std::string_view kSparseNormalAndTexcoord = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3",
     "sparse": {"count": 1, "indices": {"bufferView": 2, "componentType": 5123}, "values": {"bufferView": 3}}},
    {"componentType": 5126, "count": 3, "type": "VEC2",
     "sparse": {"count": 1, "indices": {"bufferView": 4, "componentType": 5123}, "values": {"bufferView": 5}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 72, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 76, "byteLength": 12},
                  {"buffer": 0, "byteOffset": 88, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 92, "byteLength": 8}],
  "buffers": [{"byteLength": 100, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AQAAAAAAAAAAAIA/AAAAAAIAAAAAAAA/AACAPg=="}]
})";

// A triangle whose JOINTS_0 accessor (u8 VEC4) reads (1, 0, 0, 0) for every vertex from its buffer view
// and carries a sparse block: one u16 index 1 and one value (2, 3, 0, 0).
constexpr std::string_view kSparseJoints = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1}}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5121, "count": 3, "type": "VEC4",
     "sparse": {"count": 1, "indices": {"bufferView": 2, "componentType": 5123}, "values": {"bufferView": 3}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 12},
                  {"buffer": 0, "byteOffset": 48, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 52, "byteLength": 4}],
  "buffers": [{"byteLength": 56, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAQAAAAEAAAABAAAAAQAAAAIDAAA="}]
})";

// A quad (0,0,0), (1,0,0), (1,1,0), (0,1,0) whose u16 indices read 0, 1, 2, 0, 0, 0 from their buffer
// view and carry a sparse block: u8 indices 4 and 5, values 2 and 3.
constexpr std::string_view kSparseIndices = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5123, "count": 6, "type": "SCALAR",
     "sparse": {"count": 2, "indices": {"bufferView": 2, "componentType": 5121}, "values": {"bufferView": 3}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 48},
                  {"buffer": 0, "byteOffset": 48, "byteLength": 12},
                  {"buffer": 0, "byteOffset": 60, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 64, "byteLength": 4}],
  "buffers": [{"byteLength": 68, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAACAPwAAgD8AAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAAAABAUAAAIAAwA="}]
})";

// A triangle whose u16 indices read 0, 1, 2 from their buffer view and carry a sparse block that sets
// index 2 to vertex 3, past the three vertices.
constexpr std::string_view kSparseIndexPastTheVertices = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR",
     "sparse": {"count": 1, "indices": {"bufferView": 2, "componentType": 5123}, "values": {"bufferView": 3}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 6},
                  {"buffer": 0, "byteOffset": 44, "byteLength": 2},
                  {"buffer": 0, "byteOffset": 48, "byteLength": 2}],
  "buffers": [{"byteLength": 50, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAACAAAAAwA="}]
})";

// A triangle whose TEXCOORD_0 accessor is VEC3.
constexpr std::string_view kTexcoordOfThreeComponents = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 36}],
  "buffers": [{"byteLength": 72, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]
})";

// A triangle with an RGB float COLOR_0: (1, 0, 0), (0, 0.5, 0), (0, 0, 0.25).
constexpr std::string_view kColor0RgbFloat = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 36}],
  "buffers": [{"byteLength": 72, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAACAPwAAAAAAAAAAAAAAAAAAAD8AAAAAAAAAAAAAAAAAAIA+"}]
})";

// A triangle with an RGBA normalized UNSIGNED_BYTE COLOR_0: (255, 0, 0, 255), (0, 128, 0, 51), (0, 0, 255, 0).
constexpr std::string_view kColor0RgbaNormalizedBytes = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5121, "normalized": true, "count": 3, "type": "VEC4"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 12}],
  "buffers": [{"byteLength": 48, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA/wAA/wCAADMAAP8A"}]
})";

// A triangle whose COLOR_0 accessor is VEC2, which glTF does not allow for a colour.
constexpr std::string_view kColor0OfTwoComponents = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 24}],
  "buffers": [{"byteLength": 60, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAACAPwAAAAAAAAAAAACAPwAAgD8AAIA/"}]
})";

// Two triangles of one mesh that each carry COLOR_0 (1, 0, 0), (0, 0.5, 0), (0, 0, 0.25) and a different COLOR_1
// (0, 1, 1), (1, 0, 1), (1, 1, 0): COLOR_1 is not read.
constexpr std::string_view kColor1OnTwoPrimitives = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1, "COLOR_1": 2}},
                             {"attributes": {"POSITION": 0, "COLOR_0": 1, "COLOR_1": 2}}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 72, "byteLength": 36}],
  "buffers": [{"byteLength": 108, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAACAPwAAAAAAAAAAAAAAAAAAAD8AAAAAAAAAAAAAAAAAAIA+AAAAAAAAgD8AAIA/AACAPwAAAAAAAIA/AACAPwAAgD8AAAAA"}]
})";

// A triangle whose COLOR_0 is UNSIGNED_BYTE without "normalized", which glTF does not allow for a colour.
constexpr std::string_view kColor0BytesNotNormalized = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5121, "count": 3, "type": "VEC4"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 12}],
  "buffers": [{"byteLength": 48, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA/wAA/wCAADMAAP8A"}]
})";

// A triangle whose NORMAL accessor reads from a buffer with no uri, which holds no data once the
// buffers load.
constexpr std::string_view kNormalInUnloadedBuffer = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]},
                {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 1, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"},
              {"byteLength": 36}]
})";

// A triangle whose u32 index accessor has no buffer view and declares 2^40 elements: a sparse block
// of one u8 index 0 and one value 0 over zeros.
constexpr std::string_view kSparseIndicesWithoutAViewOfAHugeCount = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"componentType": 5125, "count": 1099511627776, "type": "SCALAR",
     "sparse": {"count": 1, "indices": {"bufferView": 1, "componentType": 5121}, "values": {"bufferView": 2}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 1},
                  {"buffer": 0, "byteOffset": 40, "byteLength": 4}],
  "buffers": [{"byteLength": 44, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAA="}]
})";

// A mesh whose POSITION accessor has no buffer view and declares 2^40 elements: a sparse block of one
// u8 index 0 and one value (1, 0, 0) over zeros.
constexpr std::string_view kSparsePositionWithoutAViewOfAHugeCount = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{
    "componentType": 5126, "count": 1099511627776, "type": "VEC3", "min": [0, 0, 0], "max": [1, 0, 0],
    "sparse": {"count": 1, "indices": {"bufferView": 0, "componentType": 5121}, "values": {"bufferView": 1}}
  }],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 1},
                  {"buffer": 0, "byteOffset": 4, "byteLength": 12}],
  "buffers": [{"byteLength": 16, "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAA=="}]
})";

// A triangle whose u32 index accessor (INDEX_COUNT elements) and a VEC3 accessor no primitive reads
// (OTHER_COUNT elements) have neither a buffer view nor a sparse block, so the loader reads neither;
// AccessorsWithoutAView fills the counts in.
constexpr std::string_view kAccessorsWithoutAView = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 1]},
    {"componentType": 5125, "count": INDEX_COUNT, "type": "SCALAR"},
    {"componentType": 5126, "count": OTHER_COUNT, "type": "VEC3"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";

std::string AccessorsWithoutAView(size_t indexCount, size_t otherCount)
{
    const std::string withIndices = ReplacePlaceholder(kAccessorsWithoutAView, "INDEX_COUNT", std::to_string(indexCount));
    return ReplacePlaceholder(withIndices, "OTHER_COUNT", std::to_string(otherCount));
}

// A mesh whose one primitive reads one accessor, u32 with 2^26 + 1 elements and neither a buffer view
// nor a sparse block, as both its indices and its TEXCOORD_1.
constexpr std::string_view kIndicesAlsoReadAsAnAttribute = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"TEXCOORD_1": 0}, "indices": 0}]}],
  "accessors": [{"componentType": 5125, "count": 67108865, "type": "SCALAR"}]
})";

// "Mover" translates over 2^40 keys whose time and translation accessors have no buffer view.
constexpr std::string_view kClipOfAHugeKeyCountWithoutAView = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "Mover"}],
  "animations": [{
    "name": "Slide",
    "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]
  }],
  "accessors": [
    {"componentType": 5126, "count": 1099511627776, "type": "SCALAR", "min": [0], "max": [0]},
    {"componentType": 5126, "count": 1099511627776, "type": "VEC3"}
  ]
})";

// `json` with every `placeholder` replaced by `value`.
std::string ReplaceEveryPlaceholder(std::string_view json, std::string_view placeholder, std::string_view value)
{
    std::string filled(json);
    for (size_t at = filled.find(placeholder); at != std::string::npos;
         at = filled.find(placeholder, at + value.size()))
        filled.replace(at, placeholder.size(), value);
    return filled;
}

// `count` copies of `item`, comma-separated.
std::string Repeated(size_t count, std::string_view item)
{
    std::string items;
    for (size_t i = 0; i < count; ++i)
        items += std::string(i == 0 ? "" : ", ") + std::string(item);
    return items;
}

// One mesh whose PRIMITIVES all read one POSITION accessor of COUNT elements without a buffer view:
// a sparse block of one u8 index 0 and one value (1, 0, 0) over zeros.
constexpr std::string_view kPrimitivesSharingASparsePosition = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [PRIMITIVES]}],
  "accessors": [{
    "componentType": 5126, "count": COUNT, "type": "VEC3", "min": [0, 0, 0], "max": [1, 0, 0],
    "sparse": {"count": 1, "indices": {"bufferView": 0, "componentType": 5121}, "values": {"bufferView": 1}}
  }],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 1},
                  {"buffer": 0, "byteOffset": 4, "byteLength": 12}],
  "buffers": [{"byteLength": 16, "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAA=="}]
})";

// `primitiveCount` primitives sharing kPrimitivesSharingASparsePosition's POSITION of `count` elements.
std::string PrimitivesSharingASparsePosition(size_t primitiveCount, size_t count)
{
    const std::string json = ReplacePlaceholder(kPrimitivesSharingASparsePosition, "PRIMITIVES",
                                                Repeated(primitiveCount, R"({"attributes": {"POSITION": 0}})"));
    return ReplacePlaceholder(json, "COUNT", std::to_string(count));
}

// One mesh whose PRIMITIVES all read one dense POSITION accessor of 65536 zero vertices: 786 432 bytes
// of buffer, written as ZEROS, a data URI of 1 048 576 base64 characters.
constexpr std::string_view kPrimitivesSharingAZeroedPosition = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [PRIMITIVES]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 65536, "type": "VEC3",
                 "min": [0, 0, 0], "max": [0, 0, 0]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 786432}],
  "buffers": [{"byteLength": 786432, "uri": "data:application/octet-stream;base64,ZEROS"}]
})";

constexpr size_t kZeroedPositionVertices = 65536;

std::string PrimitivesSharingAZeroedPosition(size_t primitiveCount)
{
    constexpr size_t kBase64CharactersPerThreeBytes = 4;
    const std::string zeros(kZeroedPositionVertices * 12u / 3u * kBase64CharactersPerThreeBytes, 'A');
    const std::string json = ReplacePlaceholder(kPrimitivesSharingAZeroedPosition, "PRIMITIVES",
                                                Repeated(primitiveCount, R"({"attributes": {"POSITION": 0}})"));
    return ReplacePlaceholder(json, "ZEROS", zeros);
}

// One mesh whose PRIMITIVES all read three float positions and one u32 index accessor of COUNT elements
// without a buffer view: a sparse block of one u8 index 0 and one value 0 over zeros.
constexpr std::string_view kPrimitivesSharingSparseIndices = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [PRIMITIVES]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"componentType": 5125, "count": COUNT, "type": "SCALAR",
     "sparse": {"count": 1, "indices": {"bufferView": 1, "componentType": 5121}, "values": {"bufferView": 2}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 1},
                  {"buffer": 0, "byteOffset": 40, "byteLength": 4}],
  "buffers": [{"byteLength": 44, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAA="}]
})";

std::string PrimitivesSharingSparseIndices(size_t primitiveCount, size_t count)
{
    const std::string json =
        ReplacePlaceholder(kPrimitivesSharingSparseIndices, "PRIMITIVES",
                           Repeated(primitiveCount, R"({"attributes": {"POSITION": 0}, "indices": 1})"));
    return ReplacePlaceholder(json, "COUNT", std::to_string(count));
}

// One mesh whose PRIMITIVES all read a POSITION and a float VEC4 accessor of COUNT elements each, both
// without a buffer view: sparse blocks of one u8 index 0 over zeros, with the values (1, 0, 0) and
// (1, 0, 0, 0).
constexpr std::string_view kPrimitivesSharingASparseVec4 = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [PRIMITIVES]}],
  "accessors": [
    {"componentType": 5126, "count": COUNT, "type": "VEC3", "min": [0, 0, 0], "max": [1, 0, 0],
     "sparse": {"count": 1, "indices": {"bufferView": 0, "componentType": 5121}, "values": {"bufferView": 1}}},
    {"componentType": 5126, "count": COUNT, "type": "VEC4",
     "sparse": {"count": 1, "indices": {"bufferView": 0, "componentType": 5121}, "values": {"bufferView": 2}}}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 1},
                  {"buffer": 0, "byteOffset": 4, "byteLength": 12},
                  {"buffer": 0, "byteOffset": 16, "byteLength": 16}],
  "buffers": [{"byteLength": 32, "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAA="}]
})";

// kPrimitivesSharingASparseVec4 with `primitiveCount` primitives that read its VEC4 accessor as
// `attribute`, such as WEIGHTS_0 or COLOR_0.
std::string PrimitivesSharingASparseVec4(std::string_view attribute, size_t primitiveCount, size_t count)
{
    const std::string primitive = R"({"attributes": {"POSITION": 0, ")" + std::string(attribute) + R"(": 1}})";
    const std::string json =
        ReplacePlaceholder(kPrimitivesSharingASparseVec4, "PRIMITIVES", Repeated(primitiveCount, primitive));
    return ReplaceEveryPlaceholder(json, "COUNT", std::to_string(count));
}

// One primitive whose POSITION accessor of COUNT elements has no buffer view (a sparse block of one u8
// index 0 and one value (1, 0, 0) over zeros), and whose TARGETS all read one POSITION delta accessor of
// COUNT elements with neither a buffer view nor a sparse block.
constexpr std::string_view kMorphTargetsSharingAnAccessor = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "targets": [TARGETS]}]}],
  "accessors": [
    {"componentType": 5126, "count": COUNT, "type": "VEC3", "min": [0, 0, 0], "max": [1, 0, 0],
     "sparse": {"count": 1, "indices": {"bufferView": 0, "componentType": 5121}, "values": {"bufferView": 1}}},
    {"componentType": 5126, "count": COUNT, "type": "VEC3"}
  ],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 1},
                  {"buffer": 0, "byteOffset": 4, "byteLength": 12}],
  "buffers": [{"byteLength": 16, "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAA=="}]
})";

std::string MorphTargetsSharingAnAccessor(size_t targetCount, size_t count)
{
    const std::string json =
        ReplacePlaceholder(kMorphTargetsSharingAnAccessor, "TARGETS", Repeated(targetCount, R"({"POSITION": 1})"));
    return ReplaceEveryPlaceholder(json, "COUNT", std::to_string(count));
}

// ANIMATIONS that each translate NODES, one channel each; every channel reads sampler 0 of its
// animation, whose time and translation accessors have KEY_COUNT elements and no buffer view.
constexpr std::string_view kAnimationsSharingASampler = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [ROOTS]}],
  "nodes": [NODES],
  "animations": [ANIMATIONS],
  "accessors": [
    {"componentType": 5126, "count": KEY_COUNT, "type": "SCALAR", "min": [0], "max": [0]},
    {"componentType": 5126, "count": KEY_COUNT, "type": "VEC3"}
  ]
})";

// `animationCount` animations named "Slide" of `channelCount` channels, one per node, all reading one
// sampler of `keyCount` keys without a buffer view.
std::string AnimationsSharingASampler(size_t animationCount, size_t channelCount, size_t keyCount)
{
    std::string roots;
    std::string nodes;
    std::string channels;
    for (size_t i = 0; i < channelCount; ++i)
    {
        const std::string separator = i == 0 ? "" : ", ";
        const std::string index = std::to_string(i);
        roots += separator + index;
        nodes += separator + R"({"name": "Mover)" + index + R"("})";
        channels += separator + R"({"sampler": 0, "target": {"node": )" + index + R"(, "path": "translation"}})";
    }
    const std::string animation =
        R"({"name": "Slide", "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}], "channels": [)" +
        channels + "]}";
    std::string json = ReplacePlaceholder(kAnimationsSharingASampler, "ROOTS", roots);
    json = ReplacePlaceholder(json, "NODES", nodes);
    json = ReplacePlaceholder(json, "ANIMATIONS", Repeated(animationCount, animation));
    return ReplaceEveryPlaceholder(json, "KEY_COUNT", std::to_string(keyCount));
}

// Extras as the documents below write them: their spacing, key order and escapes are the file's, so a
// read-back equal to these is the file's bytes.
constexpr std::string_view kSceneExtras = R"({"faction": "north"})";
constexpr std::string_view kNodeExtras = R"({ "socket" : [0.5, 1, -2] })";
constexpr std::string_view kMeshExtras = R"({"regions":"head \"torso\""})";
constexpr std::string_view kMaterialExtras = R"({"tint":  [1, 0.5, 0.25]})";
constexpr std::string_view kAnimationExtras = R"({"events": [{"frame": 19, "name": "hit"}]})";

// Scene "Stage", node "Body" with the triangle mesh "BodyMesh" of material "Skin", node "Plain" with no
// extras, and animation "Walk" translating "Body"; every object but "Plain" carries the extras above.
std::string ExtrasOnEveryObjectKind()
{
    return std::string(R"({"asset": {"version": "2.0"}, "scene": 0,
  "scenes": [{"name": "Stage", "nodes": [0, 1], "extras": )") + std::string(kSceneExtras) + R"(}],
  "nodes": [{"name": "Body", "mesh": 0, "extras": )" + std::string(kNodeExtras) + R"(}, {"name": "Plain"}],
  "meshes": [{"name": "BodyMesh", "primitives": [{"attributes": {"POSITION": 0}, "material": 0}],
              "extras": )" + std::string(kMeshExtras) + R"(}],
  "materials": [{"name": "Skin", "extras": )" + std::string(kMaterialExtras) + R"(}],
  "animations": [{"name": "Walk",
                  "samplers": [{"input": 1, "output": 0, "interpolation": "LINEAR"}],
                  "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}],
                  "extras": )" + std::string(kAnimationExtras) + R"(}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]},
                {"bufferView": 1, "componentType": 5126, "count": 3, "type": "SCALAR", "min": [0], "max": [1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 12}],
  "buffers": [{"byteLength": 48, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAD8AAIA/"}]
})";
}

// An extras object of exactly `bytes` bytes (at least 8): {"b":"xx...x"}.
std::string ExtrasBlockOf(size_t bytes)
{
    constexpr std::string_view kOpen = R"({"b":")";
    constexpr std::string_view kClose = R"("})";
    return std::string(kOpen) + std::string(bytes - kOpen.size() - kClose.size(), 'x') + std::string(kClose);
}

// Node 0 holds the triangle mesh and no extras; nodes "Block1", "Block2", ... follow it, one per entry of
// `blocks`, each with those extras.
std::string NodesWithExtras(const std::vector<std::string>& blocks)
{
    std::string nodes = R"({"mesh": 0})";
    for (size_t block = 0; block < blocks.size(); ++block)
        nodes += R"(, {"name": "Block)" + std::to_string(block + 1) + R"(", "extras": )" + blocks[block] + "}";
    return R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [)" + nodes + R"(],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";
}

constexpr std::string_view kBinaryNodeExtras = R"({"glbNode" : true})";
constexpr std::string_view kBinaryMeshExtras = R"({"glbMesh": [1, 2, 3], "s": "a\"b"})";
constexpr std::string_view kBinarySecondMeshExtras = R"({"last": "mesh"})";

// A GLB whose node "N" holds mesh "M"; meshes "M" and "M2" share the triangle in the BIN chunk. The node and
// both meshes carry the extras above; a mesh's extras are read at cgltf's offsets into the JSON chunk.
std::string BinaryGltfWithMeshExtras()
{
    const std::string json =
        std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"N","mesh":0,"extras":)") +
        std::string(kBinaryNodeExtras) +
        R"(}],"meshes":[{"name":"M","primitives":[{"attributes":{"POSITION":0}}],"extras":)" +
        std::string(kBinaryMeshExtras) + R"(},{"name":"M2","primitives":[{"attributes":{"POSITION":0}}],"extras":)" +
        std::string(kBinarySecondMeshExtras) +
        R"(}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,1]}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],"buffers":[{"byteLength":36}]})";
    const float positions[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    return TestFiles::BinaryGltf(json, std::string(reinterpret_cast<const char*>(positions), sizeof(positions)));
}

// Two nodes named "Socket", then two unnamed nodes, each with extras, and a mesh also named "Socket".
constexpr std::string_view kExtrasUnderSharedNames = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0, 1, 2, 3]}],
  "nodes": [{"name": "Socket", "mesh": 0, "extras": {"first": 1}},
            {"name": "Socket", "extras": {"second": 2}},
            {"extras": {"unnamed": 1}},
            {"extras": {"unnamed": 2}}],
  "meshes": [{"name": "Socket", "primitives": [{"attributes": {"POSITION": 0}}], "extras": {"mesh": 1}}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";

// The positions of the vertices of `mesh`, three floats each.
Vector<float> VertexPositions(const Mesh& mesh)
{
    Vector<float> positions;
    for (const Vertex& vertex : mesh.Vertices)
        positions.insert(positions.end(), vertex.Position, vertex.Position + 3);
    return positions;
}

// A GLB whose animation "Swing" translates node "Mover" over eleven keys a tenth of a second apart, so the clip
// is sampled at 10 frames per second and lasts one second, with `extras` as the animation's extras.
std::string SwingClipWithExtras(std::string_view extras)
{
    constexpr size_t kKeys = 11u;
    constexpr float kFramesPerSecond = 10.0f;
    return TestFiles::ClipGlb(kKeys, kFramesPerSecond, extras);
}

// A clip schema that sets only `"loop": false`, then a vendor key whose arrays nest the extras `levels` levels
// deep in all, the extras object itself being the first.
std::string ClipExtrasNested(size_t levels)
{
    return R"({"clip": {"schemaVersion": 1, "loop": false}, "vendor": )" + std::string(levels - 1u, '[') +
           std::string(levels - 1u, ']') + "}";
}

// A clip schema that sets only `"loop": false`, then a vendor string padding the extras to exactly `bytes` bytes.
std::string ClipExtrasOf(size_t bytes)
{
    constexpr std::string_view kOpen = R"({"clip": {"schemaVersion": 1, "loop": false}, "pad": ")";
    constexpr std::string_view kClose = R"("})";
    return std::string(kOpen) + std::string(bytes - kOpen.size() - kClose.size(), 'x') + std::string(kClose);
}

struct ClipLoad
{
    std::unique_ptr<AnimationClip> Clip;
    bool Loaded = false;
    std::vector<std::string> Lines;
};

// Loads `bytes` as the clip `name`, capturing the engine's warnings and errors.
ClipLoad LoadClip(std::string_view bytes, const std::filesystem::path& name)
{
    ClipLoad result;
    result.Clip = std::make_unique<AnimationClip>(GUID::Generate(), name);
    {
        TestLog::ScopedEngineLogCapture capture(&result.Lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("GltfImportTests sink is live");
        result.Loaded = result.Clip->LoadFromData(Bytes(bytes));
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(result.Lines, "sink is live"), 1u);
    return result;
}

// Expects `load` to hold Swing's one channel and no settings or events: its clip schema was refused.
void ExpectLoadedWithoutItsSchema(const ClipLoad& load)
{
    ASSERT_TRUE(load.Loaded);
    EXPECT_EQ(load.Clip->GetChannels().size(), 1u);
    EXPECT_FALSE(load.Clip->GetSettings().Loop.has_value());
    EXPECT_TRUE(load.Clip->GetEventTrack().GetEvents().empty());
}

// The time of `frame` at `framesPerSecond` as an export stores it: the quotient rounded to a 32-bit float.
float KeyTime(size_t frame, double framesPerSecond)
{
    return static_cast<float>(static_cast<double>(frame) / framesPerSecond);
}

// A translation channel of "Mover" keyed `keys` times at `framesPerSecond`, from time 0.
AnimChannel ChannelKeyedAt(size_t keys, double framesPerSecond)
{
    AnimChannel channel{};
    channel.targetName = "Mover";
    channel.path = AnimPath::Translation;
    channel.keys.reserve(keys);
    for (size_t key = 0; key < keys; ++key)
    {
        Animation::AnimKeyframe keyframe{};
        keyframe.time = KeyTime(key, framesPerSecond);
        channel.keys.push_back(keyframe);
    }
    return channel;
}

// A translation channel of "Mover" with two keys, at 0 and at `endTime`.
AnimChannel ChannelEndingAt(float endTime)
{
    AnimChannel channel = ChannelKeyedAt(2u, 1.0);
    channel.keys[1].time = endTime;
    return channel;
}

// A clip schema with one event, "e", at `frame`.
std::string ClipEventAt(size_t frame)
{
    return R"({"clip": {"schemaVersion": 1, "events": [{"frame": )" + std::to_string(frame) + R"(, "name": "e"}]}})";
}

// A clip schema that sets only `"loop": false`, then `arrays` empty arrays, one after another, as members of the
// extras object (`inVendorObject` false) or of a "vendor" object inside it.
std::string ClipExtrasWithSiblingArrays(size_t arrays, bool inVendorObject)
{
    std::string members;
    for (size_t array = 0; array < arrays; ++array)
        members += R"(, "a)" + std::to_string(array) + R"(": [])";
    const std::string clip = R"({"clip": {"schemaVersion": 1, "loop": false})";
    if (!inVendorObject)
        return clip + members + "}";
    return clip + R"(, "vendor": {"first": 0)" + members + "}}";
}

// A clip schema that sets only `"loop": false`, then a vendor key whose objects nest the extras `levels` levels deep
// in all, the extras object itself being the first.
std::string ClipExtrasNestedInObjects(size_t levels)
{
    std::string extras = R"({"clip": {"schemaVersion": 1, "loop": false}, "vendor": )";
    for (size_t level = 1; level < levels; ++level)
        extras += R"({"k": )";
    extras += "1";
    return extras + std::string(levels - 1u, '}') + "}";
}

// What ReadClipSchema made of one extras block.
struct SchemaRead
{
    std::string Refusal;
    AnimationClipSettings Settings;
    Animation::AnimationEventTrack Events;
};

// Reads `extras` as the extras of a clip of `channels` lasting `duration` seconds; by default Swing's one channel,
// keyed ten times a second for one second.
SchemaRead ReadSchema(std::string_view extras, const std::vector<AnimChannel>& channels = {ChannelKeyedAt(11u, 10.0)},
                      float duration = 1.0f)
{
    SchemaRead read;
    read.Refusal = ReadClipSchema(extras, channels, duration, read.Settings, read.Events);
    return read;
}

// Expects `clip` to hold no clip settings and no events.
void ExpectNoSettingsOrEvents(const AnimationClip& clip)
{
    EXPECT_FALSE(clip.GetSettings().Loop.has_value());
    EXPECT_FALSE(clip.GetSettings().Speed.has_value());
    EXPECT_TRUE(clip.GetEventTrack().GetEvents().empty());
}

} // namespace

TEST(GltfImport, SparseIndexPastAccessorCountIsRefused)
{
    const ModelLoad load = LoadModel(kSparseIndexPastCount, "sparse_index_past_count.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "sparse_index_past_count.gltf");
    EXPECT_NE(refusal.find("data_too_short"), std::string::npos) << refusal;
}

TEST(GltfImport, AccessorPastItsBufferViewIsRefused)
{
    const ModelLoad load = LoadModel(kAccessorPastBufferView, "accessor_past_view.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "accessor_past_view.gltf");
    EXPECT_NE(refusal.find("data_too_short"), std::string::npos) << refusal;
}

TEST(GltfImport, ExternalBuffersShareOneFileWithIndependentDeclaredLengths)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("gltf-shared-buffer-" + GUID::Generate().ToString());
    std::filesystem::create_directories(root);
    const float positions[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};
    std::ofstream(root / "shared.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(positions), sizeof(positions));
    const auto json = Bytes(R"({"asset": {"version": "2.0"},
  "buffers": [{"byteLength": 12, "uri": "shared.bin"}, {"byteLength": 24, "uri": "./shared.bin"}],
  "bufferViews": [{"buffer": 0, "byteLength": 12}, {"buffer": 1, "byteOffset": 12, "byteLength": 12}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 1, "type": "VEC3"},
                {"bufferView": 1, "componentType": 5126, "count": 1, "type": "VEC3"}]})");
    const auto document = OpenGltfDocument(json, root / "shared.gltf");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    ASSERT_TRUE(document);
    ASSERT_EQ(document->buffers_count, 2u);
    EXPECT_EQ(document->buffers[0].size, 12u);
    EXPECT_EQ(document->buffers[1].size, 24u);
    EXPECT_EQ(document->buffers[0].data, document->buffers[1].data);
    for (size_t accessor = 0; accessor < 2; ++accessor) {
        float position[3]{};
        ASSERT_TRUE(cgltf_accessor_read_float(&document->accessors[accessor], 0, position, 3));
        for (size_t component = 0; component < 3; ++component)
            EXPECT_FLOAT_EQ(position[component], positions[accessor * 3 + component]);
    }
}

TEST(GltfImport, SmallExternalBuffersKeepOnlyOneDeclaredPrefixOfALargeFile)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("gltf-buffer-prefix-" + GUID::Generate().ToString());
    std::filesystem::create_directories(root / "sub");
    {
        const Vector<uint8> fileBytes(1024u * 1024u, 0x5a);
        std::ofstream(root / "shared.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
    }
    const auto json = Bytes(R"({"asset": {"version": "2.0"}, "buffers": [
  {"byteLength": 4, "uri": "shared.bin"}, {"byteLength": 16, "uri": "./shared.bin"},
  {"byteLength": 64, "uri": "sub/../shared.bin"}, {"byteLength": 32, "uri": "%73hared.bin"},
  {"byteLength": 8, "uri": "shared.bin"}, {"byteLength": 12, "uri": "./shared.bin"}]})");
    auto loaded = OpenGltfDocument(json, root / "prefix.gltf");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    // Moving the document keeps the shared bytes alive after the source file has gone.
    auto document = std::move(loaded);
    ASSERT_TRUE(document);
    ASSERT_EQ(document->buffers_count, 6u);
    const size_t declared[] = {4, 16, 64, 32, 8, 12};
    for (size_t index = 0; index < document->buffers_count; ++index) {
        const auto& buffer = document->buffers[index];
        EXPECT_EQ(buffer.size, declared[index]);
        ASSERT_NE(buffer.data, nullptr);
        EXPECT_EQ(buffer.data, document->buffers[0].data);
        const auto* bytes = static_cast<const uint8*>(buffer.data);
        EXPECT_EQ(bytes[0], 0x5a);
        EXPECT_EQ(bytes[declared[index] - 1], 0x5a);
    }
    const auto& backing = document.get_deleter().ExternalBuffers;
    ASSERT_TRUE(backing);
    ASSERT_EQ(backing->size(), 1u);
    EXPECT_EQ(backing->begin()->second.size(), 64u);
    EXPECT_EQ(backing->begin()->second.capacity(), 64u);
}

TEST(GltfImport, ExternalBufferShorterThanDeclaredIsRefused)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("gltf-import-" + GUID::Generate().ToString());
    std::filesystem::create_directories(root);
    const std::filesystem::path modelPath = root / "short_buffer.gltf";
    {
        // The buffer declares three float positions (36 bytes); the file holds one.
        const float onePosition[3] = {1.0f, 0.0f, 0.0f};
        std::ofstream(root / "short_buffer.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(onePosition), sizeof(onePosition));
        std::ofstream(modelPath) << R"({"asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "short_buffer.bin"}]})";
    }

    std::vector<std::string> lines;
    bool loaded = false;
    {
        ModelAsset asset(GUID::Generate(), modelPath);
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("GltfImportTests sink is live");
        loaded = asset.Load();
        Logger::Log::Flush();
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    ASSERT_EQ(TestLog::CountLinesContaining(lines, "sink is live"), 1u);
    EXPECT_FALSE(loaded);
    const std::string refusal = TestLog::FirstLineContaining(lines, "short_buffer.gltf");
    EXPECT_NE(refusal.find("data_too_short"), std::string::npos) << refusal;
}

TEST(GltfImport, ByteStrideOutsideTheGltfRangeIsRefused)
{
    for (const std::string_view stride : {"2", "18", "256", "9223372036854775807"})
    {
        const ModelLoad load = LoadModel(TriangleWithPositionStride(stride), "strided_triangle.gltf");
        ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
        EXPECT_FALSE(load.Loaded) << "byteStride " << stride;
        const std::string refusal = TestLog::FirstLineContaining(load.Lines, "strided_triangle.gltf");
        EXPECT_NE(refusal.find("byteStride " + std::string(stride)), std::string::npos) << refusal;
    }

    const ModelLoad load = LoadModel(kMorphViewWithAGibibyteStride, "morph_view_gibibyte_stride.gltf");
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "morph_view_gibibyte_stride.gltf");
    EXPECT_NE(refusal.find("byteStride 1073741824"), std::string::npos) << refusal;
}

TEST(GltfImport, BufferViewRunningPastItsBufferIsRefused)
{
    for (const size_t entries : {size_t{64}, size_t{16} * 1024u * 1024u})
    {
        const ModelLoad load = LoadModel(SparseViewsPastTheirBuffer(entries), "views_past_buffer.gltf");
        ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
        EXPECT_FALSE(load.Loaded);
        const std::string refusal = TestLog::FirstLineContaining(load.Lines, "views_past_buffer.gltf");
        const std::string expected = "buffer view 1 holds " + std::to_string(entries * 4u) +
                                     " bytes from byte 36, past the 52 bytes of buffer 0";
        EXPECT_NE(refusal.find(expected), std::string::npos) << refusal;
    }
}

TEST(GltfImport, AccessorWhoseExtentWrapsIsRefused)
{
    for (const std::string_view json : {kPositionCountWrappingItsExtent, kSparseCountWrappingItsExtent})
    {
        const ModelLoad load = LoadModel(json, "extent_wraps.gltf");
        ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
        EXPECT_FALSE(load.Loaded);
        const std::string refusal = TestLog::FirstLineContaining(load.Lines, "extent_wraps.gltf");
        EXPECT_NE(refusal.find("more bytes than a buffer can hold"), std::string::npos) << refusal;
    }
}

TEST(GltfImport, SkinWithFewerInverseBindMatricesThanJointsIsRefused)
{
    const ModelLoad load = LoadModel(kSkinShortOfInverseBindMatrices, "short_inverse_binds.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "short_inverse_binds.gltf");
    EXPECT_NE(refusal.find("inverseBindMatrices"), std::string::npos) << refusal;
}

TEST(GltfImport, RequiredExtensionsTheEngineLacksAreRefusedByName)
{
    const ModelLoad load = LoadModel(kRequiresUnimplementedExtensions, "requires_meshopt.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "requires_meshopt.gltf");
    EXPECT_NE(refusal.find("EXT_meshopt_compression"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("KHR_draco_mesh_compression"), std::string::npos) << refusal;
    EXPECT_EQ(refusal.find("KHR_mesh_quantization"), std::string::npos) << refusal;
}

TEST(GltfImport, RequiredMeshQuantizationLoads)
{
    ModelAsset asset(GUID::Generate(), "quantized.gltf");
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(kRequiresMeshQuantization), FbxLoaderOptions{}));
    ASSERT_EQ(asset.GetMeshCount(), 1u);
    const Mesh& mesh = asset.GetMesh(0);
    ASSERT_EQ(mesh.Vertices.size(), 3u);
    // Default import mirrors X.
    EXPECT_FLOAT_EQ(mesh.Vertices[0].Position[0], -1.0f);
    EXPECT_FLOAT_EQ(mesh.Vertices[1].Position[1], 1.0f);
    EXPECT_FLOAT_EQ(mesh.Vertices[2].Position[2], 1.0f);
}

TEST(GltfImport, OptionalExtensionsTheEngineIgnoresAreListedOnce)
{
    ModelAsset asset(GUID::Generate(), "optional_extensions.gltf");
    std::vector<std::string> lines;
    bool loaded = false;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Info);
        Logger::Log::Info("GltfImportTests sink is live");
        loaded = ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(kUsesOptionalExtensions), FbxLoaderOptions{});
        Logger::Log::Flush();
    }
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "sink is live"), 1u);
    EXPECT_TRUE(loaded);
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "KHR_materials_sheen"), 1u);
    const std::string listed = TestLog::FirstLineContaining(lines, "KHR_materials_sheen");
    EXPECT_NE(listed.find("optional_extensions.gltf"), std::string::npos) << listed;
    EXPECT_EQ(listed.find("MSFT_lod"), std::string::npos) << listed;
}

TEST(GltfImport, SkeletonCountsTheJointClosureNotEveryNode)
{
    ModelAsset asset(GUID::Generate(), "decorated_skin.gltf");
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(kSkinWithDecorationNodes), FbxLoaderOptions{}));
    const auto* skeleton = Engine::Renderer::SkeletonStore::Instance().Get(asset.GetSkeletonId());
    ASSERT_NE(skeleton, nullptr);
    EXPECT_EQ(skeleton->BoneCount, 8u);
    // Armature, Hip, Spine, Head: the joints and their one ancestor that is not a joint, a level each.
    EXPECT_EQ(skeleton->JointClosure, (std::vector<uint32>{1, 2, 3, 4}));
    EXPECT_EQ(skeleton->JointClosureLevelOffsets, (std::vector<uint32>{0, 1, 2, 3, 4}));
}

TEST(GltfImport, MorphTargetsImportInFileOrderWithEmptyTargetsKept)
{
    const Mesh mesh = LoadOnlyMesh(kThreeSparseMorphTargets, "three_sparse_morph_targets.gltf");
    ASSERT_EQ(mesh.MorphTargets.size(), 3u);
    EXPECT_EQ(mesh.MorphTargets[0].Name, "Smile");
    EXPECT_EQ(mesh.MorphTargets[1].Name, "Blink");
    EXPECT_EQ(mesh.MorphTargets[2].Name, "Frown");
    EXPECT_EQ(mesh.MorphTargetDefaultWeights, (Vector<float>{0.25f, 0.5f, 0.75f}));

    // The default import mirrors X: a file delta (x, y, z) is (-x, y, z) in the engine.
    const MorphTarget& smile = mesh.MorphTargets[0];
    EXPECT_EQ(smile.VertexIndices, (Vector<uint32>{1u, 3u}));
    EXPECT_EQ(smile.PositionDeltas, (Vector<float>{0.0f, 0.5f, 0.0f, -0.25f, 0.0f, 0.0f}));
    EXPECT_TRUE(smile.NormalDeltas.empty());

    const MorphTarget& blink = mesh.MorphTargets[1];
    EXPECT_TRUE(blink.VertexIndices.empty());
    EXPECT_TRUE(blink.PositionDeltas.empty());

    const MorphTarget& frown = mesh.MorphTargets[2];
    EXPECT_EQ(frown.VertexIndices, (Vector<uint32>{0u, 2u}));
    EXPECT_EQ(frown.PositionDeltas, (Vector<float>{0.0f, 0.0f, 0.0f, 0.0f, -0.5f, 0.125f}));
    EXPECT_EQ(frown.NormalDeltas, (Vector<float>{0.0f, 0.0f, 0.0f, 0.0f, 0.25f, -0.25f}));
}

TEST(GltfImport, MorphTargetTangentDeltasAreKept)
{
    const Mesh mesh = LoadOnlyMesh(kThreeSparseMorphTargets, "three_sparse_morph_targets.gltf");
    ASSERT_EQ(mesh.MorphTargets.size(), 3u);
    EXPECT_TRUE(mesh.MorphTargets[0].TangentDeltas.empty());
    EXPECT_EQ(mesh.MorphTargets[2].TangentDeltas, (Vector<float>{-0.125f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f}));
}

TEST(GltfImport, MorphTargetWhoseDataDoesNotLoadIsRefused)
{
    const ModelLoad load = LoadModel(kMorphTargetInUnloadedBuffer, "morph_target_in_unloaded_buffer.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "morph_target_in_unloaded_buffer.gltf");
    EXPECT_NE(refusal.find("morph target 0 ('morph_0') of mesh 0 has a POSITION accessor whose data does not load"),
              std::string::npos)
        << refusal;
}

TEST(GltfImport, SparseMorphValuesAreReadTightlyPackedBesideAStridedView)
{
    const Mesh mesh = LoadOnlyMesh(kSparseMorphBesideAStridedView, "sparse_morph_beside_strided_view.gltf");
    ASSERT_EQ(mesh.MorphTargets.size(), 1u);
    EXPECT_EQ(mesh.MorphTargets[0].VertexIndices, (Vector<uint32>{0u, 2u}));
    EXPECT_EQ(mesh.MorphTargets[0].PositionDeltas, (Vector<float>{-1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}));
}

TEST(GltfImport, MorphTargetAccessorThatIsNotVec3IsRefused)
{
    const ModelLoad load = LoadModel(kMorphTargetOfFourComponents, "morph_target_four_components.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "morph_target_four_components.gltf");
    EXPECT_NE(refusal.find("POSITION accessor of 4 components"), std::string::npos) << refusal;
}

// cgltf_validate refuses a file whose sparse index is at or past its accessor's count before the loader
// reads it, so the accessor is built here: count 3, one u32 sparse index 3 and one VEC3 value.
TEST(GltfImport, UnpackingASparseIndexAtTheAccessorCountIsRefused)
{
    unsigned char bytes[16] = {};
    const uint32 index = 3u;
    const float value[3] = {5.0f, 6.0f, 7.0f};
    std::memcpy(bytes, &index, sizeof(index));
    std::memcpy(bytes + sizeof(index), value, sizeof(value));

    cgltf_buffer buffer{};
    buffer.size = sizeof(bytes);
    buffer.data = bytes;
    cgltf_buffer_view indicesView{};
    indicesView.buffer = &buffer;
    indicesView.size = sizeof(index);
    cgltf_buffer_view valuesView{};
    valuesView.buffer = &buffer;
    valuesView.offset = sizeof(index);
    valuesView.size = sizeof(value);

    cgltf_accessor accessor{};
    accessor.component_type = cgltf_component_type_r_32f;
    accessor.type = cgltf_type_vec3;
    accessor.count = 3u;
    accessor.stride = sizeof(value);
    accessor.is_sparse = 1;
    accessor.sparse.count = 1u;
    accessor.sparse.indices_buffer_view = &indicesView;
    accessor.sparse.indices_component_type = cgltf_component_type_r_32u;
    accessor.sparse.values_buffer_view = &valuesView;

    Vector<float> out;
    GltfSparseScratch scratch;
    const std::string problem = UnpackAccessorFloats(accessor, out, scratch);
    EXPECT_NE(problem.find("sparse index 3 is at or past its count of 3"), std::string::npos) << problem;
}

TEST(GltfImport, SparsePositionOverABufferViewImportsTheAppliedValues)
{
    const Mesh mesh = LoadOnlyMesh(kSparsePositionOverABufferView, "sparse_position_over_view.gltf");
    // The default import mirrors X: the view's (1, 0, 0) is (-1, 0, 0), and the sparse value replaces
    // vertex 2's (0, 1, 0).
    EXPECT_EQ(VertexPositions(mesh), (Vector<float>{0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}));
}

TEST(GltfImport, SparsePositionWithoutABufferViewImportsItsValuesOverZeros)
{
    const Mesh mesh = LoadOnlyMesh(kSparsePositionWithoutABufferView, "sparse_position_without_view.gltf");
    EXPECT_EQ(VertexPositions(mesh), (Vector<float>{0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}));
}

TEST(GltfImport, SparseNormalAndTexcoordImportTheAppliedValues)
{
    const Mesh mesh = LoadOnlyMesh(kSparseNormalAndTexcoord, "sparse_normal_and_texcoord.gltf");
    ASSERT_EQ(mesh.Vertices.size(), 3u);
    const Vertex* vertices = mesh.Vertices.data();
    EXPECT_EQ(Vector<float>(vertices[0].Normal, vertices[0].Normal + 3), (Vector<float>{0.0f, 0.0f, 1.0f}));
    EXPECT_EQ(Vector<float>(vertices[1].Normal, vertices[1].Normal + 3), (Vector<float>{0.0f, 1.0f, 0.0f}));
    EXPECT_EQ(Vector<float>(vertices[1].TexCoords, vertices[1].TexCoords + 2), (Vector<float>{0.0f, 0.0f}));
    EXPECT_EQ(Vector<float>(vertices[2].TexCoords, vertices[2].TexCoords + 2), (Vector<float>{0.5f, 0.25f}));
}

// A triangle with normals and UVs and no TANGENT, under a material that is `materialJson`. U runs
// along +X in the file, V along +Y, the normal is +Z.
std::string TriangleWithoutTangents(std::string_view materialJson)
{
    return std::string(R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "material": 0}]}],
  "materials": [)") + std::string(materialJson) + R"(],
  "images": [{"uri": "data:image/png;base64,AA=="}],
  "textures": [{"source": 0}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 0, "byteOffset": 36, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 0, "byteOffset": 72, "componentType": 5126, "count": 3, "type": "VEC2"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 96}],
  "buffers": [{"byteLength": 96, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/"}]
})";
}

TEST(GltfImport, ANormalMappedPrimitiveWithoutTangentsGetsAGeneratedFrame)
{
    const Mesh mesh =
        LoadOnlyMesh(TriangleWithoutTangents(R"({"normalTexture": {"index": 0}})"), "normal_mapped.gltf");
    ASSERT_EQ(mesh.Vertices.size(), 3u);
    for (const Vertex& vertex : mesh.Vertices)
    {
        // The frame follows +U, which the default Mirror X conversion turns into -X.
        EXPECT_NEAR(vertex.Tangent[0], -1.0f, 1e-5f);
        EXPECT_NEAR(vertex.Tangent[1], 0.0f, 1e-5f);
        EXPECT_NEAR(vertex.Tangent[2], 0.0f, 1e-5f);
        EXPECT_EQ(vertex.Tangent[3], 1.0f) << "the handedness an authored glTF tangent of this primitive carries";
    }
}

// A cooked LOD cache stores whole vertices, so what the glTF loader produces is part of its key: the
// glTF parse-options hash carries kGltfImportGeometryVersion beside the options, and differs from
// the hash of the same options alone. Without it a project's LODs cooked before an import change
// keep the old vertices (zero tangents beside a LOD0 that has them).
TEST(GltfImport, TheLodCacheKeyCarriesTheGltfImportVersion)
{
    const FbxLoaderOptions options{};
    ModelAsset asset(GUID::Generate(), "plain.gltf");
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(TriangleWithoutTangents(R"({"name": "Plain"})")),
                                                  options));
    EXPECT_EQ(ModelAssetFbxTestAccess::ParseOptionsHash(asset),
              Hashing::Fnv1a64Value(HashFbxLoaderOptions(options), kGltfImportGeometryVersion));
    EXPECT_NE(ModelAssetFbxTestAccess::ParseOptionsHash(asset), HashFbxLoaderOptions(options));
}

// The tangents the importer generates against the tangents an artist's tool wrote for the same
// mesh: one primitive of the Khronos Lantern with its authored TANGENT stream, imported as it is
// and again with the stream removed from the file. The generated frame must point the way the
// authored one does and keep its handedness, or the normal map's green channel reads inverted.
TEST(GltfImport, GeneratedTangentsAgreeWithTheAuthoredTangentsOfTheSameMesh)
{
    const std::filesystem::path fixture =
        GameEngine::TestPaths::StagedRoot() / "Engine/Tests/Fixtures/lantern_body_tangents.gltf";
    std::ifstream input(fixture, std::ios::binary);
    ASSERT_TRUE(input.is_open()) << fixture;
    const std::string authoredJson((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    constexpr std::string_view kTangentAttribute = R"(,"TANGENT":3)";
    ASSERT_NE(authoredJson.find(kTangentAttribute), std::string::npos) << "the fixture's TANGENT attribute";
    const std::string strippedJson = ReplacePlaceholder(authoredJson, kTangentAttribute, "");

    const Mesh authored = LoadOnlyMesh(authoredJson, "lantern_authored.gltf");
    const Mesh generated = LoadOnlyMesh(strippedJson, "lantern_generated.gltf");
    ASSERT_EQ(generated.Vertices.size(), authored.Vertices.size());
    ASSERT_GT(authored.Vertices.size(), 0u);

    size_t handednessAgrees = 0;
    double directionDotSum = 0.0;
    for (size_t i = 0; i < authored.Vertices.size(); ++i)
    {
        const float* a = authored.Vertices[i].Tangent;
        const float* g = generated.Vertices[i].Tangent;
        handednessAgrees += (a[3] > 0.0f) == (g[3] > 0.0f) ? 1u : 0u;
        const double aLength = std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2]);
        directionDotSum += (double(a[0]) * g[0] + double(a[1]) * g[1] + double(a[2]) * g[2]) / aLength;
    }
    const double count = static_cast<double>(authored.Vertices.size());
    EXPECT_GT(handednessAgrees / count, 0.99)
        << handednessAgrees << " of " << authored.Vertices.size() << " vertices keep the authored handedness";
    EXPECT_GT(directionDotSum / count, 0.95) << "mean cosine between the generated and authored tangent";
}

TEST(GltfImport, APrimitiveWithoutANormalMapGetsNoTangents)
{
    const Mesh mesh = LoadOnlyMesh(TriangleWithoutTangents(R"({"name": "Plain"})"), "plain.gltf");
    ASSERT_EQ(mesh.Vertices.size(), 3u);
    // A zero tangent direction is what keeps the stream off the mesh (MeshGPURegistry).
    for (const Vertex& vertex : mesh.Vertices)
        EXPECT_EQ(Vector<float>(vertex.Tangent, vertex.Tangent + 3), (Vector<float>{0.0f, 0.0f, 0.0f}));
}

TEST(GltfImport, SparseJointsImportTheAppliedValues)
{
    const Mesh mesh = LoadOnlyMesh(kSparseJoints, "sparse_joints.gltf");
    EXPECT_EQ(mesh.Joints0, (Vector<uint16>{1, 0, 0, 0, 2, 3, 0, 0, 1, 0, 0, 0}));
}

TEST(GltfImport, SparseIndicesImportTheAppliedValues)
{
    const Mesh mesh = LoadOnlyMesh(kSparseIndices, "sparse_indices.gltf");
    // 0, 1, 2, 0, 2, 3 with each triangle's winding reversed by the default X mirror.
    EXPECT_EQ(mesh.Indices, (Vector<uint32>{0u, 2u, 1u, 0u, 3u, 2u}));
}

TEST(GltfImport, SparseIndexPastTheVertexCountIsRefused)
{
    const ModelLoad load = LoadModel(kSparseIndexPastTheVertices, "sparse_index_past_vertices.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "sparse_index_past_vertices.gltf");
    EXPECT_NE(refusal.find("primitive 0 of mesh 0 has an accessor for indices whose element 2 is vertex 3"),
              std::string::npos)
        << refusal;
}

TEST(GltfImport, BaseAttributeOfTheWrongTypeIsRefused)
{
    const ModelLoad load = LoadModel(kTexcoordOfThreeComponents, "texcoord_of_three_components.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "texcoord_of_three_components.gltf");
    EXPECT_NE(refusal.find("accessor for TEXCOORD_0 that is not VEC2"), std::string::npos) << refusal;
}

TEST(GltfImport, MaterialWithoutEmissionImportsNone)
{
    const ImportedMaterialData material = ImportOnlyMaterial(R"({"name": "Plain"})");
    ExpectEmissiveColor(material, 0.0f, 0.0f, 0.0f);
    EXPECT_TRUE(material.EmissiveTexture.empty());
}

TEST(GltfImport, EmissiveFactorImportsAsTheEmissiveColor)
{
    const ImportedMaterialData material = ImportOnlyMaterial(R"({"emissiveFactor": [0.25, 0.5, 1.0]})");
    ExpectEmissiveColor(material, 0.25f, 0.5f, 1.0f);
    EXPECT_TRUE(material.EmissiveTexture.empty());
}

TEST(GltfImport, EmissiveTextureImportsAsTheEmissiveMap)
{
    const ImportedMaterialData material =
        ImportOnlyMaterial(R"({"emissiveFactor": [1, 1, 1], "emissiveTexture": {"index": 0}})");
    EXPECT_EQ(material.EmissiveTexture, "__embedded:0");
    ExpectEmissiveColor(material, 1.0f, 1.0f, 1.0f);
}

TEST(GltfImport, EmissiveStrengthScalesTheEmissiveColor)
{
    const ImportedMaterialData material = ImportOnlyMaterial(
        R"({"emissiveFactor": [1.0, 0.5, 0.25],
            "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4.0}}})");
    ExpectEmissiveColor(material, 4.0f, 2.0f, 1.0f);
}

// The specification's default factor is black, so a texture alone emits nothing.
TEST(GltfImport, EmissiveTextureUnderTheDefaultFactorImportsBlack)
{
    const ImportedMaterialData material = ImportOnlyMaterial(R"({"emissiveTexture": {"index": 0}})");
    EXPECT_EQ(material.EmissiveTexture, "__embedded:0");
    ExpectEmissiveColor(material, 0.0f, 0.0f, 0.0f);
}

// A 1x1 RGBA PNG, base64.
constexpr std::string_view kOnePixelPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGM4UaHxHwAF3AJoiJKY8QAAAABJRU5ErkJggg==";

// A triangle whose one material is `materialJson`, beside one image per URI in `imageUris` and one
// texture over each, in order.
std::string TriangleWithImageUris(std::string_view materialJson, const std::vector<std::string>& imageUris)
{
    std::string images;
    std::string textures;
    for (size_t index = 0; index < imageUris.size(); ++index) {
        images += (index ? ", " : "") + std::string(R"({"uri": ")") + imageUris[index] + R"("})";
        textures += (index ? ", " : "") + std::string(R"({"source": )") + std::to_string(index) + "}";
    }
    return std::string(R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}],
  "materials": [)") + std::string(materialJson) + R"(],
  "images": [)" + images + R"(],
  "textures": [)" + textures + R"(],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";
}

// glTF-Embedded files carry their images as base64 data URIs. Every slot a material maps reads the
// decoded image bytes, and the MIME type comes from the URI, lower-cased and without parameters.
TEST(GltfImport, DataUriImagesImportTheirBytesForEverySlot)
{
    const std::string png(kOnePixelPngBase64);
    const std::string json = TriangleWithImageUris(
        R"({"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 1}},
            "normalTexture": {"index": 2}, "emissiveFactor": [1, 1, 1], "emissiveTexture": {"index": 3}})",
        {"data:image/png;base64," + png, "data:IMAGE/PNG;base64," + png, "data:image/png;name=normal;base64," + png,
         "data:image/png;base64," + png});
    ModelAsset asset(GUID::Generate(), "embedded.gltf");
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(json), FbxLoaderOptions{}));
    ASSERT_EQ(asset.GetMaterials().size(), 1u);
    const ImportedMaterialData& material = asset.GetMaterials()[0];
    const Vector<EmbeddedImage>& images = asset.GetEmbeddedImages();
    ASSERT_EQ(images.size(), 4u);

    const std::pair<const char*, const String*> slots[] = {{"base color", &material.DiffuseTexture},
                                                           {"metal-roughness", &material.SpecularTexture},
                                                           {"normal", &material.NormalTexture},
                                                           {"emissive", &material.EmissiveTexture}};
    for (size_t slot = 0; slot < std::size(slots); ++slot) {
        SCOPED_TRACE(slots[slot].first);
        EXPECT_EQ(*slots[slot].second, "__embedded:" + std::to_string(slot));
        const EmbeddedImage& image = images[slot];
        ASSERT_EQ(image.Data.size(), 70u);
        EXPECT_EQ(std::memcmp(image.Data.data(), "\x89PNG\r\n\x1a\n", 8), 0);
        EXPECT_EQ(image.Data.back(), 0x82);
        EXPECT_EQ(image.MimeType, "image/png");
    }
}

// A data URI image that is not base64, or whose payload does not decode, imports empty, never as
// partly decoded bytes, with one warning per image that names the file, the image and the fix.
TEST(GltfImport, DataUriImagesThatDoNotDecodeImportEmptyWithAWarningEach)
{
    std::string badCharacter(kOnePixelPngBase64);
    badCharacter[badCharacter.size() / 2] = '!';
    const std::string json =
        TriangleWithImageUris("{}", {"data:image/png;base64," + badCharacter, "data:image/png,plain"});
    std::vector<std::string> lines;
    ModelAsset asset(GUID::Generate(), "undecodable.gltf");
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(json), FbxLoaderOptions{}));
        Logger::Log::Flush();
    }
    const Vector<EmbeddedImage>& images = asset.GetEmbeddedImages();
    ASSERT_EQ(images.size(), 2u);
    for (size_t index = 0; index < images.size(); ++index) {
        SCOPED_TRACE(index);
        EXPECT_TRUE(images[index].Data.empty());
        const std::string named = "glTF 'undecodable.gltf': image " + std::to_string(index) + " has a data URI";
        const auto count = std::count_if(lines.begin(), lines.end(), [&](const std::string& line) {
            return line.find(named) != std::string::npos && line.find("re-export") != std::string::npos;
        });
        EXPECT_EQ(count, 1);
    }
}

// The occlusion map imports into each material's occlusion slot: a material with an occlusion map
// of its own names that image, and a file that packs occlusion, roughness and metalness into one
// image (ORM) names that image for both slots, since occlusion reads its R channel.
TEST(GltfImport, OcclusionTextureImportsAsTheOcclusionMap)
{
    const std::string png = "data:image/png;base64," + std::string(kOnePixelPngBase64);
    const std::string json = TriangleWithImageUris(
        R"({"name": "SeparateOcclusion", "occlusionTexture": {"index": 0}},
           {"name": "PackedOrm", "pbrMetallicRoughness": {"metallicRoughnessTexture": {"index": 1}},
            "occlusionTexture": {"index": 1, "strength": 1.0}})",
        {png, png});
    ModelAsset asset(GUID::Generate(), "occlusion.gltf");
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(json), FbxLoaderOptions{}));
    ASSERT_EQ(asset.GetMaterials().size(), 2u);
    const ImportedMaterialData& separate = asset.GetMaterials()[0];
    const ImportedMaterialData& packed = asset.GetMaterials()[1];
    EXPECT_EQ(separate.OcclusionTexture, "__embedded:0");
    EXPECT_TRUE(separate.SpecularTexture.empty());
    EXPECT_EQ(packed.OcclusionTexture, "__embedded:1");
    EXPECT_EQ(packed.SpecularTexture, "__embedded:1");
    ASSERT_EQ(asset.GetEmbeddedImages().size(), 2u);
    for (const EmbeddedImage& image : asset.GetEmbeddedImages())
        EXPECT_EQ(image.Data.size(), 70u) << "the occlusion image's bytes did not survive the import";

    // The material each converts to samples the occlusion map in its aoMap slot.
    for (uint32_t index = 0; index < 2; ++index) {
        SCOPED_TRACE(index);
        const auto converted =
            Engine::Renderer::ModelMaterialBridge::Convert(asset.GetGUID(), index, asset.GetMaterials()[index]);
        const auto aoMap = converted.document.textures.find("aoMap");
        ASSERT_NE(aoMap, converted.document.textures.end());
        EXPECT_EQ(aoMap->second, "__embedded:" + std::to_string(index));
    }
}

// The surfaces have no occlusion strength input, so a strength other than 1 is reported once,
// naming the file, the material and the strength, and the map still imports.
TEST(GltfImport, OcclusionStrengthOtherThanOneIsReportedAsIgnored)
{
    const std::string json = TriangleWithImageUris(
        R"({"name": "HalfOcclusion", "occlusionTexture": {"index": 0, "strength": 0.5}})",
        {"data:image/png;base64," + std::string(kOnePixelPngBase64)});
    std::vector<std::string> lines;
    ModelAsset asset(GUID::Generate(), "occlusion_strength.gltf");
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Info);
        ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(json), FbxLoaderOptions{}));
        Logger::Log::Flush();
    }
    ASSERT_EQ(asset.GetMaterials().size(), 1u);
    EXPECT_EQ(asset.GetMaterials()[0].OcclusionTexture, "__embedded:0");
    const auto reported = std::count_if(lines.begin(), lines.end(), [](const std::string& line) {
        return line.find("glTF 'occlusion_strength.gltf': material 'HalfOcclusion' sets occlusion strength 0.5") !=
                   std::string::npos &&
               line.find("full strength") != std::string::npos;
    });
    EXPECT_EQ(reported, 1);
}

TEST(GltfImport, Color0RgbImportsWithOpaqueAlpha)
{
    const Mesh mesh = LoadOnlyMesh(kColor0RgbFloat, "color0_rgb_float.gltf");
    ASSERT_TRUE(mesh.HasColor0());
    EXPECT_EQ(mesh.Color0, (Vector<float>{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 0.25f, 1.0f}));
}

TEST(GltfImport, Color0NormalizedBytesImportAsZeroToOne)
{
    const Mesh mesh = LoadOnlyMesh(kColor0RgbaNormalizedBytes, "color0_rgba_bytes.gltf");
    ASSERT_TRUE(mesh.HasColor0());
    const Vector<float> expected{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 128.0f / 255.0f, 0.0f, 51.0f / 255.0f,
                                 0.0f, 0.0f, 1.0f, 0.0f};
    ASSERT_EQ(mesh.Color0.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        EXPECT_FLOAT_EQ(mesh.Color0[i], expected[i]) << "component " << i;
}

TEST(GltfImport, Color0OfTwoComponentsIsRefused)
{
    const ModelLoad load = LoadModel(kColor0OfTwoComponents, "color0_of_two_components.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "color0_of_two_components.gltf");
    EXPECT_NE(refusal.find("accessor for COLOR_0 that is not VEC3 or VEC4"), std::string::npos) << refusal;
}

TEST(GltfImport, ColorSetsPastColor0AreReportedOnce)
{
    ModelAsset asset(GUID::Generate(), "color1_on_two_primitives.gltf");
    std::vector<std::string> lines;
    bool loaded = false;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Info);
        Logger::Log::Info("GltfImportTests sink is live");
        loaded = ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(kColor1OnTwoPrimitives), FbxLoaderOptions{});
        Logger::Log::Flush();
    }
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "sink is live"), 1u);
    EXPECT_TRUE(loaded);
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "COLOR_1"), 1u);
    const std::string reported = TestLog::FirstLineContaining(lines, "COLOR_1");
    EXPECT_NE(reported.find("color1_on_two_primitives.gltf"), std::string::npos) << reported;
    EXPECT_NE(reported.find("only COLOR_0 is read"), std::string::npos) << reported;
    ASSERT_GE(asset.GetMeshCount(), 1u);
    const Mesh& mesh = asset.GetMesh(0);
    ASSERT_TRUE(mesh.HasColor0());
    // COLOR_0's colours, not COLOR_1's, on every vertex of both triangles.
    for (size_t v = 0; v < mesh.Color0.size() / 4; ++v) {
        const float* rgba = mesh.Color0.data() + v * 4;
        const size_t corner = v % 3;
        const float expected[3][4] = {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.5f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.25f, 1.0f}};
        EXPECT_EQ(Vector<float>(rgba, rgba + 4), Vector<float>(expected[corner], expected[corner] + 4)) << "vertex " << v;
    }
}

TEST(GltfImport, Color0BytesThatAreNotNormalizedAreRefused)
{
    const ModelLoad load = LoadModel(kColor0BytesNotNormalized, "color0_bytes_not_normalized.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "color0_bytes_not_normalized.gltf");
    EXPECT_NE(refusal.find("accessor for COLOR_0 that is not FLOAT, normalized UNSIGNED_BYTE or normalized UNSIGNED_SHORT"),
              std::string::npos)
        << refusal;
}

TEST(GltfImport, BaseAttributeWhoseDataDoesNotLoadIsRefused)
{
    const ModelLoad load = LoadModel(kNormalInUnloadedBuffer, "normal_in_unloaded_buffer.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "normal_in_unloaded_buffer.gltf");
    EXPECT_NE(refusal.find("accessor for NORMAL whose data does not load"), std::string::npos) << refusal;
}

TEST(GltfImport, SparseAccessorWithoutAViewOfAHugeCountIsRefused)
{
    struct Case
    {
        std::string_view Json;
        std::string_view Name;
        std::string_view Expected;
    };
    const Case cases[] = {
        {kSparseIndicesWithoutAViewOfAHugeCount, "huge_sparse_indices.gltf",
         "accessor 1 declares 1099511627776 elements without a buffer view, past the limit of 134217728"},
        {kSparsePositionWithoutAViewOfAHugeCount, "huge_sparse_position.gltf",
         "accessor 0 declares 1099511627776 elements without a buffer view, past the limit of 67108864"},
    };
    for (const Case& refused : cases)
    {
        const ModelLoad load = LoadModel(refused.Json, refused.Name);
        ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
        EXPECT_FALSE(load.Loaded) << refused.Name;
        const std::string refusal = TestLog::FirstLineContaining(load.Lines, refused.Name);
        EXPECT_NE(refusal.find(refused.Expected), std::string::npos) << refusal;
    }
}

TEST(GltfImport, AccessorWithoutAViewPastItsCountLimitIsRefused)
{
    struct Case
    {
        std::string Json;
        std::string_view Expected;
    };
    const Case cases[] = {
        {AccessorsWithoutAView(134217729u, 1u),
         "accessor 1 declares 134217729 elements without a buffer view, past the limit of 134217728"},
        {AccessorsWithoutAView(3u, 67108865u),
         "accessor 2 declares 67108865 elements without a buffer view, past the limit of 67108864"},
        {std::string(kIndicesAlsoReadAsAnAttribute),
         "accessor 0 declares 67108865 elements without a buffer view, past the limit of 67108864"},
    };
    for (const Case& refused : cases)
    {
        const ModelLoad load = LoadModel(refused.Json, "accessors_without_view.gltf");
        ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
        EXPECT_FALSE(load.Loaded) << refused.Expected;
        const std::string refusal = TestLog::FirstLineContaining(load.Lines, "accessors_without_view.gltf");
        EXPECT_NE(refusal.find(refused.Expected), std::string::npos) << refusal;
    }
}

TEST(GltfImport, AccessorsWithoutAViewAtTheirCountLimitsLoad)
{
    // The index accessor holds no data, so the triangle imports unindexed.
    const Mesh mesh = LoadOnlyMesh(AccessorsWithoutAView(134217728u, 67108864u), "accessors_at_limit.gltf");
    EXPECT_EQ(mesh.Indices.size(), 3u);
}

// As UnpackingASparseIndexAtTheAccessorCountIsRefused, through the integer form: count 3 of u8 VEC4 (a
// JOINTS_0 accessor), one u32 sparse index 3 and one value.
TEST(GltfImport, UnpackingASparseIntegerIndexAtTheAccessorCountIsRefused)
{
    unsigned char bytes[8] = {};
    const uint32 index = 3u;
    const uint8 value[4] = {1u, 2u, 3u, 4u};
    std::memcpy(bytes, &index, sizeof(index));
    std::memcpy(bytes + sizeof(index), value, sizeof(value));

    cgltf_buffer buffer{};
    buffer.size = sizeof(bytes);
    buffer.data = bytes;
    cgltf_buffer_view indicesView{};
    indicesView.buffer = &buffer;
    indicesView.size = sizeof(index);
    cgltf_buffer_view valuesView{};
    valuesView.buffer = &buffer;
    valuesView.offset = sizeof(index);
    valuesView.size = sizeof(value);

    cgltf_accessor accessor{};
    accessor.component_type = cgltf_component_type_r_8u;
    accessor.type = cgltf_type_vec4;
    accessor.count = 3u;
    accessor.stride = sizeof(value);
    accessor.is_sparse = 1;
    accessor.sparse.count = 1u;
    accessor.sparse.indices_buffer_view = &indicesView;
    accessor.sparse.indices_component_type = cgltf_component_type_r_32u;
    accessor.sparse.values_buffer_view = &valuesView;

    Vector<uint32> out;
    GltfSparseScratch scratch;
    const std::string problem = UnpackAccessorIntegers(accessor, out, scratch);
    EXPECT_NE(problem.find("sparse index 3 is at or past its count of 3"), std::string::npos) << problem;
}

TEST(GltfImport, ExtrasOfEveryObjectKindReadBackByNameAsTheFileWritesThem)
{
    const ModelLoad load = LoadModel(ExtrasOnEveryObjectKind(), "extras_every_kind.gltf");
    ASSERT_TRUE(load.Loaded);
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Scene, "Stage"), kSceneExtras);
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Node, "Body"), kNodeExtras);
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Mesh, "BodyMesh"), kMeshExtras);
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Material, "Skin"), kMaterialExtras);
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Animation, "Walk"), kAnimationExtras);
    EXPECT_TRUE(load.Extras.Find(ModelObjectKind::Mesh, "Body").empty()) << "a name reads only its own kind";
    EXPECT_TRUE(load.Extras.Find(ModelObjectKind::Node, "Plain").empty()) << "an object without extras reads empty";
    EXPECT_TRUE(load.Extras.Find(ModelObjectKind::Node, "Absent").empty());
}

TEST(GltfImport, MeshExtrasOfABinaryFileAreReadFromItsJsonChunk)
{
    const ModelLoad load = LoadModel(BinaryGltfWithMeshExtras(), "extras_binary.glb");
    ASSERT_TRUE(load.Loaded);
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Mesh, "M"), kBinaryMeshExtras);
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Mesh, "M2"), kBinarySecondMeshExtras);
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Node, "N"), kBinaryNodeExtras);
}

TEST(GltfImport, ExtrasOverTheObjectBoundAreNotKeptAndTheObjectIsNamed)
{
    const std::string atTheBound = ExtrasBlockOf(ModelExtras::kObjectBoundBytes);
    const ModelLoad load =
        LoadModel(NodesWithExtras({atTheBound, ExtrasBlockOf(ModelExtras::kObjectBoundBytes + 1u)}),
                  "extras_over_object_bound.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    ASSERT_TRUE(load.Loaded) << "the model loads without the refused block";
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Node, "Block1"), atTheBound);
    EXPECT_TRUE(load.Extras.Find(ModelObjectKind::Node, "Block2").empty());
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "extras_over_object_bound.gltf"), 1u);
    const std::string warning = TestLog::FirstLineContaining(load.Lines, "extras_over_object_bound.gltf");
    EXPECT_NE(warning.find("node 2 ('Block2') are not kept: they hold 262145 bytes"), std::string::npos) << warning;
    EXPECT_NE(warning.find("at most 262144 bytes"), std::string::npos) << warning;
}

TEST(GltfImport, ExtrasPastTheModelBoundKeepTheEarlierBlocksAndNameTheRest)
{
    // The bound charges each block with its object's name, "Block1" to "Block7" here: six bytes each.
    constexpr size_t kNameBytes = 6u;
    const std::string full = ExtrasBlockOf(ModelExtras::kObjectBoundBytes - kNameBytes);
    // Four full blocks and their names fill the model bound exactly; the fifth does not fit.
    const ModelLoad filled = LoadModel(NodesWithExtras({full, full, full, full, ExtrasBlockOf(10)}),
                                       "extras_fill_model_bound.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(filled.Lines, "sink is live"), 1u);
    ASSERT_TRUE(filled.Loaded);
    EXPECT_EQ(filled.Extras.Find(ModelObjectKind::Node, "Block4"), full);
    EXPECT_TRUE(filled.Extras.Find(ModelObjectKind::Node, "Block5").empty());
    ASSERT_EQ(TestLog::CountLinesContaining(filled.Lines, "extras_fill_model_bound.gltf"), 1u);
    const std::string filledWarning = TestLog::FirstLineContaining(filled.Lines, "extras_fill_model_bound.gltf");
    EXPECT_NE(filledWarning.find("node 5 ('Block5') (16 bytes with its name) are not kept: a model keeps at most "
                                 "1048576 bytes"),
              std::string::npos) << filledWarning;

    // 20 bytes short of the bound, a block that charges 30 does not fit; the blocks after it, charging 20 and 16,
    // would, and are not kept either.
    const std::string shortOfFull = ExtrasBlockOf(ModelExtras::kObjectBoundBytes - kNameBytes - 20u);
    const ModelLoad past = LoadModel(NodesWithExtras({full, full, full, shortOfFull, ExtrasBlockOf(30u - kNameBytes),
                                                      ExtrasBlockOf(20u - kNameBytes), ExtrasBlockOf(10)}),
                                     "extras_past_model_bound.gltf");
    ASSERT_TRUE(past.Loaded);
    EXPECT_EQ(past.Extras.Find(ModelObjectKind::Node, "Block1"), full);
    EXPECT_EQ(past.Extras.Find(ModelObjectKind::Node, "Block4"), shortOfFull);
    EXPECT_TRUE(past.Extras.Find(ModelObjectKind::Node, "Block5").empty());
    EXPECT_TRUE(past.Extras.Find(ModelObjectKind::Node, "Block6").empty());
    EXPECT_TRUE(past.Extras.Find(ModelObjectKind::Node, "Block7").empty());
    ASSERT_EQ(TestLog::CountLinesContaining(past.Lines, "extras_past_model_bound.gltf"), 1u);
    const std::string pastWarning = TestLog::FirstLineContaining(past.Lines, "extras_past_model_bound.gltf");
    EXPECT_NE(pastWarning.find("node 5 ('Block5') (30 bytes with its name) and of 2 later objects are not kept"),
              std::string::npos) << pastWarning;
}

TEST(GltfImport, ExtrasUnderANameTwoObjectsOfOneKindShareReadTheFirst)
{
    const ModelLoad load = LoadModel(kExtrasUnderSharedNames, "extras_shared_names.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    ASSERT_TRUE(load.Loaded);
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Node, "Socket"), R"({"first": 1})");
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Node, ""), R"({"unnamed": 1})") << "unnamed objects share the empty name";
    EXPECT_EQ(load.Extras.Find(ModelObjectKind::Mesh, "Socket"), R"({"mesh": 1})") << "names are per kind";
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "extras_shared_names.gltf"), 1u);
    const std::string warning = TestLog::FirstLineContaining(load.Lines, "extras_shared_names.gltf");
    EXPECT_NE(warning.find("node 1 ('Socket') and of 1 later object are not readable"), std::string::npos)
        << warning;
}

TEST(GltfImport, ReloadAdoptionReplacesTheExtrasAndUnloadClearsThem)
{
    ModelAsset live(GUID::Generate(), std::filesystem::path("extras_reload.gltf"));
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(live, Bytes(NodesWithExtras({R"({"v": 1})", R"({"gone": 1})"})),
                                                  FbxLoaderOptions{}));
    auto staged = std::make_shared<ModelAsset>(GUID::Generate(), std::filesystem::path("extras_reload.gltf"));
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(*staged, Bytes(NodesWithExtras({R"({"v": 2})"})), FbxLoaderOptions{}));
    ASSERT_EQ(live.GetExtras().Find(ModelObjectKind::Node, "Block1"), R"({"v": 1})");

    ASSERT_TRUE(live.AdoptReload(*staged));
    EXPECT_EQ(live.GetExtras().Find(ModelObjectKind::Node, "Block1"), R"({"v": 2})") << "the reload's text is read";
    EXPECT_TRUE(live.GetExtras().Find(ModelObjectKind::Node, "Block2").empty()) << "a block the reload dropped is gone";
    EXPECT_EQ(staged->GetExtras().Find(ModelObjectKind::Node, "Block1"), R"({"v": 1})") << "staged holds the old payload";

    staged.reset();
    live.Unload();
    EXPECT_TRUE(live.GetExtras().Find(ModelObjectKind::Node, "Block1").empty()) << "Unload clears the table";
}

TEST(GltfImport, WeightsChannelIsSkippedNotMappedToTranslation)
{
    AnimationClip clip(GUID::Generate(), "blink.gltf");
    std::vector<std::string> lines;
    bool loaded = false;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("GltfImportTests sink is live");
        loaded = clip.LoadFromData(Bytes(kWeightsAndTranslationClip));
        Logger::Log::Flush();
    }
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "sink is live"), 1u);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(clip.GetChannels().size(), 1u);
    const AnimChannel& channel = clip.GetChannels()[0];
    EXPECT_EQ(channel.targetName, "Mover");
    EXPECT_EQ(channel.path, AnimPath::Translation);
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "weights"), 1u);
    const std::string skipped = TestLog::FirstLineContaining(lines, "weights");
    EXPECT_NE(skipped.find("Body"), std::string::npos) << skipped;
    EXPECT_NE(skipped.find("Blink"), std::string::npos) << skipped;
}

TEST(GltfImport, ChannelWithoutATargetNodeIsSkipped)
{
    AnimationClip clip(GUID::Generate(), "slide.gltf");
    std::vector<std::string> lines;
    bool loaded = false;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("GltfImportTests sink is live");
        loaded = clip.LoadFromData(Bytes(kNodelessChannelClip));
        Logger::Log::Flush();
    }
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "sink is live"), 1u);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(clip.GetChannels().size(), 1u);
    EXPECT_EQ(clip.GetChannels()[0].targetName, "Mover");
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "no target node"), 1u);
    const std::string skipped = TestLog::FirstLineContaining(lines, "no target node");
    EXPECT_NE(skipped.find("Slide"), std::string::npos) << skipped;
}

TEST(GltfImport, ClipAccessorWithoutAViewOfAHugeCountIsRefused)
{
    AnimationClip clip(GUID::Generate(), "huge_clip.gltf");
    std::vector<std::string> lines;
    bool loaded = false;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("GltfImportTests sink is live");
        loaded = clip.LoadFromData(Bytes(kClipOfAHugeKeyCountWithoutAView));
        Logger::Log::Flush();
    }
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "sink is live"), 1u);
    EXPECT_FALSE(loaded);
    const std::string refusal = TestLog::FirstLineContaining(lines, "huge_clip.gltf");
    EXPECT_NE(refusal.find("accessor 0 declares 1099511627776 elements without a buffer view"), std::string::npos)
        << refusal;
}

// The budget is 64 times the bytes of a file's JSON and loaded buffers plus 256 MiB (268 435 456 bytes).
// Each case below is sized so that a loader without the budget, which sizes what is charged, stays
// under 1 GB of memory.
constexpr size_t kBudgetAllowance = size_t{256} * 1024 * 1024;

// Sixteen primitives share one POSITION of 2^19 elements without a buffer view: each sizes 2^19
// vertices and, unindexed, 2^19 indices, about 27 MB, and sixteen of them pass the budget of a file of
// under 2 KB.
TEST(GltfImport, PrimitivesSharingAPositionWithoutAViewPastTheBudgetAreRefused)
{
    const std::string json = PrimitivesSharingASparsePosition(16u, 524288u);
    ASSERT_LT(json.size(), 2048u);
    const ModelLoad load = LoadModel(json, "shared_sparse_position.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "shared_sparse_position.gltf");
    const std::string expected =
        "of mesh 0 has an accessor for POSITION (accessor 0) whose 524288 elements are sized into " +
        std::to_string(sizeof(Vertex) + sizeof(uint32)) + " bytes each, more than the ";
    EXPECT_NE(refusal.find(expected), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("bytes left of the file's budget of "), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("16 primitives read this accessor"), std::string::npos) << refusal;
    // The tenth primitive: 10 x 524288 x 52 bytes, past the budget of a file of under 2 KB.
    EXPECT_NE(refusal.find("With this copy, the file is charged 272629760 bytes."), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("No import setting changes this budget."), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("reads its own accessor, or merge the primitives that share an accessor. Engine issue "
                           "#2469 removes the copies."),
              std::string::npos)
        << refusal;
}

// As above at the count limit of an accessor without a buffer view (2^26): the first primitive alone
// passes the budget, so the file is refused before any primitive's arrays are sized. Not in a run of a
// loader without the budget, which would size about 56 GB here.
TEST(GltfImport, PrimitivesSharingAPositionAtItsCountLimitAreRefusedAtTheFirstPrimitive)
{
    const std::string json = PrimitivesSharingASparsePosition(16u, 67108864u);
    const ModelLoad load = LoadModel(json, "shared_position_at_limit.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "shared_position_at_limit.gltf");
    EXPECT_NE(refusal.find("primitive 0 of mesh 0 has an accessor for POSITION (accessor 0) whose 67108864 elements"),
              std::string::npos)
        << refusal;
}

// One primitive alone reads a POSITION of 2^26 elements without a buffer view: nothing shares the
// accessor, so the refusal states its fix in the declared count and gives no sharing advice.
TEST(GltfImport, OnePrimitivePastTheBudgetIsRefusedWithoutSharingAdvice)
{
    const ModelLoad load = LoadModel(PrimitivesSharingASparsePosition(1u, 67108864u), "unshared_position.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "unshared_position.gltf");
    EXPECT_NE(refusal.find("primitive 0 of mesh 0 has an accessor for POSITION (accessor 0) whose 67108864 elements"),
              std::string::npos)
        << refusal;
    EXPECT_NE(refusal.find("No import setting changes this budget. To load the file, re-export it so that this "
                           "accessor declares no more elements than the file holds data for."),
              std::string::npos)
        << refusal;
    EXPECT_EQ(refusal.find("read this accessor"), std::string::npos) << refusal;
    EXPECT_EQ(refusal.find("its own accessor"), std::string::npos) << refusal;
}

// Sixteen primitives share a POSITION of 2^18 elements without a buffer view, about 236 MB with the
// read scratch, which fits the budget of a file of about 2 KB. One channel then reads its own key times
// of 2^19 elements, about 67 MB: alone they fit the budget, but not the bytes the shared copies left.
// The refusal names those bytes and both fixes, not the count fix of an accessor past the whole budget.
TEST(GltfImport, UnsharedAccessorPastTheBytesEarlierChargesLeftIsRefusedWithBothFixes)
{
    std::string json = ReplacePlaceholder(
        PrimitivesSharingASparsePosition(16u, 262144u), R"("nodes": [{"mesh": 0}],)",
        R"("nodes": [{"mesh": 0}], "animations": [{"name": "Slide",
          "samplers": [{"input": 1, "output": 2, "interpolation": "LINEAR"}],
          "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]}],)");
    json = ReplacePlaceholder(json, R"({"bufferView": 1}}
  }])",
                              R"({"bufferView": 1}}
  }, {"componentType": 5126, "count": 524288, "type": "SCALAR", "min": [0], "max": [0]},
     {"componentType": 5126, "count": 524288, "type": "VEC3"}])");
    const ModelLoad load = LoadModel(json, "unshared_keys_after_shared_position.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "unshared_keys_after_shared_position.gltf");
    EXPECT_NE(refusal.find("has a key time accessor (accessor 1) whose 524288 elements"), std::string::npos)
        << refusal;
    EXPECT_NE(refusal.find("Other arrays of the file already charge "), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("To load the file, re-export it so that each primitive, morph target and animation channel "
                           "reads its own accessor, and each accessor declares no more elements than the file holds "
                           "data for. Engine issue #2469 removes the copies of shared accessors."),
              std::string::npos)
        << refusal;
    EXPECT_EQ(refusal.find("this accessor declares no more elements"), std::string::npos) << refusal;
}

// A buffer without a URI holds no data, so its declared byteLength does not raise the budget: the file
// of the first test with one more buffer of 9 * 10^18 bytes is refused as that file is.
TEST(GltfImport, BufferWithoutAUriDoesNotRaiseTheBudget)
{
    const std::string json =
        ReplacePlaceholder(PrimitivesSharingASparsePosition(16u, 524288u), R"(=="}])",
                           R"(=="}, {"byteLength": 9000000000000000000}])");
    const ModelLoad load = LoadModel(json, "unloaded_buffer.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "unloaded_buffer.gltf");
    EXPECT_NE(refusal.find("has an accessor for POSITION (accessor 0) whose 524288 elements"), std::string::npos)
        << refusal;
}

// A dense POSITION of 65536 vertices in a file of F = about 1.05 MB (its JSON, which holds the 0.79 MB
// buffer as a base64 data URI, counted once) costs 65536 x 52 bytes, 3 407 872, per unindexed
// primitive that reads it, plus 65536 x 68 bytes of read scratch once. 85 primitives charge
// 294 125 568 bytes: past 256 MiB + F (about 269.5 MB), within 256 MiB + 64 F (about 335.8 MB), so the
// multiple of the file's bytes admits them.
TEST(GltfImport, DenseAccessorSharedPastTheAllowanceLoadsWithinTheMultiple)
{
    const std::string json = PrimitivesSharingAZeroedPosition(85u);
    ASSERT_GT(json.size(), 1048576u);
    ASSERT_LT(json.size(), 1056768u);
    const ModelLoad load = LoadModel(json, "shared_dense_within.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_TRUE(load.Loaded);
    EXPECT_EQ(TestLog::CountLinesContaining(load.Lines, "is not loaded"), 0u);
}

// As above with 120 primitives: 413 401 088 bytes, past 256 MiB + 64 F (about 335.8 MB).
TEST(GltfImport, DenseAccessorSharedPastTheMultipleIsRefused)
{
    const std::string json = PrimitivesSharingAZeroedPosition(120u);
    ASSERT_LT(json.size(), 1056768u);
    const ModelLoad load = LoadModel(json, "shared_dense_past.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "shared_dense_past.gltf");
    EXPECT_NE(refusal.find("has an accessor for POSITION (accessor 0) whose 65536 elements"), std::string::npos)
        << refusal;
}

// Twenty primitives of three vertices share one index accessor of 2^22 elements without a buffer view:
// 20 x 2^22 x 4 bytes, about 335 MB, of indices pass the budget of a file of about 2 KB.
TEST(GltfImport, IndicesSharedWithoutAViewPastTheBudgetAreRefused)
{
    const ModelLoad load = LoadModel(PrimitivesSharingSparseIndices(20u, 4194304u), "shared_indices.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "shared_indices.gltf");
    EXPECT_NE(refusal.find("has an accessor for indices (accessor 1) whose 4194304 elements are sized into 4 bytes "
                           "each"),
              std::string::npos)
        << refusal;
}

// Four primitives share a POSITION and a WEIGHTS_0 of J = 1 266 000 elements without a buffer view.
// Each charges 16 J of weights, then 52 J of vertices: the budget, about 212.1 J, holds three primitives
// (204 J), and the weights of the fourth do not fit the 8.1 J left.
TEST(GltfImport, WeightsPastTheBudgetAreRefusedByName)
{
    const ModelLoad load = LoadModel(PrimitivesSharingASparseVec4("WEIGHTS_0", 4u, 1266000u), "shared_weights.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "shared_weights.gltf");
    EXPECT_NE(refusal.find("primitive 3 of mesh 0 has an accessor for WEIGHTS_0 (accessor 1) whose 1266000 "
                           "elements are sized into 16 bytes each"),
              std::string::npos)
        << refusal;
}

// As above with COLOR_0, which LoadGLTF sizes as four floats per element of its own count.
TEST(GltfImport, VertexColorsPastTheBudgetAreRefusedByName)
{
    const ModelLoad load = LoadModel(PrimitivesSharingASparseVec4("COLOR_0", 4u, 1266000u), "shared_colors.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "shared_colors.gltf");
    EXPECT_NE(refusal.find("primitive 3 of mesh 0 has an accessor for COLOR_0 (accessor 1) whose 1266000 "
                           "elements are sized into 16 bytes each"),
              std::string::npos)
        << refusal;
}

// Twenty morph targets of one primitive of 2^20 vertices share one POSITION delta accessor. The vertices
// charge 2^20 x 52 bytes, about 55 MB, and each target 2^20 x 16 bytes (a vertex index and a position
// delta), about 17 MB: twelve targets fit the budget of a file of about 1 KB, and the thirteenth does not.
TEST(GltfImport, MorphTargetsSharingAnAccessorPastTheBudgetAreRefused)
{
    const ModelLoad load = LoadModel(MorphTargetsSharingAnAccessor(20u, 1048576u), "shared_morph.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "shared_morph.gltf");
    EXPECT_NE(refusal.find("has a POSITION accessor (accessor 1) whose 1048576 elements are sized into 16 bytes each"),
              std::string::npos)
        << refusal;
}

// Sixteen channels share one sampler of 2^18 keys without a buffer view: each sizes 2^18 keys of about
// 34 MB, and sixteen of them pass the budget of a standalone clip load.
TEST(GltfImport, ChannelsSharingASamplerWithoutAViewPastTheBudgetAreRefused)
{
    const ClipLoad load = LoadClip(AnimationsSharingASampler(1u, 16u, 262144u), "shared_sampler.gltf");
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "shared_sampler.gltf");
    EXPECT_NE(refusal.find("the clip of animation 0 is not loaded"), std::string::npos) << refusal;
    const std::string expected =
        "of animation 0 ('Slide') has a key time accessor (accessor 0) whose 262144 elements are sized into " +
        std::to_string(sizeof(Animation::AnimKeyframe)) + " bytes each, more than the ";
    EXPECT_NE(refusal.find(expected), std::string::npos) << refusal;
}

// A model loads each animation as an embedded clip that stays resident. Four animations of 32 channels
// share a sampler of 2^15 keys: each clip, 32 x 2^15 x 128 bytes (134 MB), fits a budget of its own,
// but the four pass the one budget the model charges them to.
TEST(GltfImport, AnimationsOfAModelShareOneBudget)
{
    const ModelLoad load = LoadModel(AnimationsSharingASampler(4u, 32u, 32768u), "shared_animations.gltf");
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "is not loaded");
    EXPECT_NE(refusal.find("shared_animations.gltf' is not loaded: channel "), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("('Slide') has a key time accessor (accessor 0) whose 32768 elements"), std::string::npos)
        << refusal;
}

// A folder of its own under the temporary directory, holding shared.bin (the 786 432 zero bytes of
// kPrimitivesSharingAZeroedPosition's buffer) and an empty folder sub; removed with all it holds when the
// test ends.
struct SharedBinFolder
{
    SharedBinFolder()
        : Root(std::filesystem::temp_directory_path() / ("gltf-import-" + GUID::Generate().ToString()))
    {
        std::filesystem::create_directories(Root / "sub");
        const std::string zeros(kZeroedPositionVertices * 12u, '\0');
        std::ofstream(Root / "shared.bin", std::ios::binary)
            .write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }

    ~SharedBinFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all(Root, ignored);
    }

    std::filesystem::path Root;
};

// Loads shared_file.gltf in `folder`: 100 primitives share kPrimitivesSharingAZeroedPosition's POSITION,
// whose buffer is given once per URI in `uris`, each of 786 432 bytes. The primitives charge
// 100 x 65536 x 52 + 65536 x 68 = 345 243 648 bytes. A budget that counts shared.bin once, 256 MiB + 64 x
// about 0.79 MB (about 319.0 MB), refuses them; one that counts it twice (about 369.4 MB) admits them.
ModelLoad LoadPrimitivesNamingSharedBin(const SharedBinFolder& folder, const std::vector<std::string_view>& uris)
{
    std::string buffers;
    for (const std::string_view uri : uris)
    {
        buffers += std::string(buffers.empty() ? "" : ", ") + R"({"byteLength": 786432, "uri": ")" +
                   std::string(uri) + R"("})";
    }
    const std::string primitives = ReplacePlaceholder(kPrimitivesSharingAZeroedPosition, "PRIMITIVES",
                                                      Repeated(100u, R"({"attributes": {"POSITION": 0}})"));
    const std::string json = ReplacePlaceholder(
        primitives, R"({"byteLength": 786432, "uri": "data:application/octet-stream;base64,ZEROS"})", buffers);
    return LoadModel(json, folder.Root / "shared_file.gltf");
}

// Buffers name shared.bin three times as "shared.bin", and once each as "./shared.bin",
// "sub/../shared.bin", "shared%2Ebin" and "/shared.bin", which cgltf_load_buffers appends to the glTF
// file's folder as it does any URI. Where the file system folds case (NTFS, APFS by default), one more
// names it "SHARED.BIN", which only resolving the name on disk gives shared.bin's key; a case-sensitive
// file system has no such file, so the spelling is left out there. The file counts once, so the
// primitives are refused.
TEST(GltfImport, BuffersNamingOneFileCountItOnce)
{
    const SharedBinFolder folder;
    std::vector<std::string_view> uris = {"shared.bin",        "shared.bin",   "shared.bin", "./shared.bin",
                                          "sub/../shared.bin", "shared%2Ebin", "/shared.bin"};
    std::error_code error;
    const bool caseFolds = std::filesystem::equivalent(folder.Root / "SHARED.BIN", folder.Root / "shared.bin", error);
    if (caseFolds)
        uris.push_back("SHARED.BIN");
    RecordProperty("caseVariant", caseFolds ? std::string("SHARED.BIN added") : "left out: " + error.message());
    const ModelLoad load = LoadPrimitivesNamingSharedBin(folder, uris);
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "shared_file.gltf");
    EXPECT_NE(refusal.find("has an accessor for POSITION (accessor 0) whose 65536 elements"), std::string::npos)
        << refusal;
}

// As above with "shared.bin" and "d0/shared.bin", a symbolic link to "../shared.bin": the link counts as
// the file it names, so the primitives are refused.
TEST(GltfImport, BufferNamingASymbolicLinkCountsTheLinkedFileOnce)
{
    const SharedBinFolder folder;
    std::filesystem::create_directories(folder.Root / "d0");
    std::error_code error;
    std::filesystem::create_symlink("../shared.bin", folder.Root / "d0" / "shared.bin", error);
    if (error)
        GTEST_SKIP() << "this process cannot create symbolic links (Windows needs Developer Mode or elevation): "
                     << error.message();
    const ModelLoad load = LoadPrimitivesNamingSharedBin(folder, {"shared.bin", "d0/shared.bin"});
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "sink is live"), 1u);
    EXPECT_FALSE(load.Loaded);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "shared_file.gltf");
    EXPECT_NE(refusal.find("has an accessor for POSITION (accessor 0) whose 65536 elements"), std::string::npos)
        << refusal;
}

// GltfAllocationBudget::Charge on a document of no bytes, whose budget is the allowance alone: a charge
// fits up to the last byte and not one past it; a charge whose product would pass a size_t is refused
// and charges nothing; zero bytes per element charge nothing; and file bytes that pass a size_t
// saturate the budget instead of wrapping to a small one.
TEST(GltfImport, AllocationBudgetChargesToItsLastByteAndNeverWraps)
{
    constexpr size_t kLargest = std::numeric_limits<size_t>::max();
    cgltf_accessor accessor{};
    cgltf_data document{};
    document.accessors = &accessor;
    document.accessors_count = 1;

    GltfAllocationBudget exact(document, {});
    EXPECT_TRUE(exact.Charge(accessor, kBudgetAllowance - 1u, 1u).empty());
    EXPECT_TRUE(exact.Charge(accessor, 1u, 1u).empty());
    const std::string pastTheLastByte = exact.Charge(accessor, 1u, 1u);
    EXPECT_NE(pastTheLastByte.find("(accessor 0) whose 1 elements are sized into 1 bytes each, more than the 0 bytes "
                                   "left of the file's budget of 268435456"),
              std::string::npos)
        << pastTheLastByte;

    GltfAllocationBudget wrapping(document, {});
    EXPECT_FALSE(wrapping.Charge(accessor, kLargest, 2u).empty());
    EXPECT_FALSE(wrapping.Charge(accessor, 2u, kLargest).empty());
    EXPECT_FALSE(wrapping.Charge(accessor, kLargest / 2u + 1u, 2u).empty());
    EXPECT_FALSE(wrapping.Charge(accessor, kLargest / 128u + 1u, 128u).empty());
    EXPECT_TRUE(wrapping.Charge(accessor, kLargest, 0u).empty());
    EXPECT_TRUE(wrapping.Charge(accessor, kBudgetAllowance, 1u).empty());

    uint8 byte = 0;
    cgltf_buffer buffers[2] = {};
    buffers[0].size = kLargest;
    buffers[0].data = &byte;
    buffers[1].size = 2u;
    buffers[1].data = &byte;
    document.buffers = buffers;
    document.buffers_count = 2;
    GltfAllocationBudget saturated(document, {});
    EXPECT_TRUE(saturated.Charge(accessor, kBudgetAllowance * 2u, 1u).empty());
}

TEST(GltfImport, ClipSchemaSetsTheClipSettings)
{
    const ClipLoad load = LoadClip(SwingClipWithExtras(R"({"clip": {"schemaVersion": 1, "loop": false,
        "speed": 1.25, "rootMotion": {"bone": "Mover", "rotation": false}, "translations": "restRelative"}})"),
                                   "swing.glb");
    ASSERT_TRUE(load.Loaded);
    const AnimationClipSettings& settings = load.Clip->GetSettings();
    EXPECT_EQ(settings.Loop, std::optional<bool>(false));
    EXPECT_EQ(settings.Speed, std::optional<float32>(1.25f));
    ASSERT_TRUE(settings.RootMotion.has_value());
    EXPECT_EQ(settings.RootMotion->Bone, "Mover");
    EXPECT_TRUE(settings.RootMotion->Translation) << "a part of the motion the file does not mention is on";
    EXPECT_FALSE(settings.RootMotion->Rotation);
    EXPECT_EQ(settings.Translations, ClipTranslationMode::RestRelative);
    EXPECT_EQ(TestLog::CountLinesContaining(load.Lines, "clip schema"), 0u);
}

TEST(GltfImport, ClipSchemaEventsImportAtTheirFramesTimes)
{
    // Swing is keyed ten times a second, so frame 3 is at 0.3 s and frame 7 at 0.7 s; the track is in time order.
    const ClipLoad load = LoadClip(SwingClipWithExtras(R"({"clip": {"schemaVersion": 1,
        "events": [{"frame": 7, "name": "land"}, {"frame": 3, "name": "hit"}]}})"),
                                   "swing.glb");
    ASSERT_TRUE(load.Loaded);
    const std::vector<AnimationEvent>& events = load.Clip->GetEventTrack().GetEvents();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].Name, "hit");
    EXPECT_FLOAT_EQ(events[0].Time, 0.3f);
    EXPECT_EQ(events[1].Name, "land");
    EXPECT_FLOAT_EQ(events[1].Time, 0.7f);
}

TEST(GltfImport, ClipSchemaOfAnUnknownVersionIsRefusedByNameAndTheClipLoads)
{
    const ClipLoad load = LoadClip(SwingClipWithExtras(R"({"clip": {"schemaVersion": 99, "loop": false,
        "events": [{"frame": 3, "name": "hit"}]}})"),
                                   "swing.glb");
    ExpectLoadedWithoutItsSchema(load);
    ASSERT_EQ(TestLog::CountLinesContaining(load.Lines, "clip schema"), 1u);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "clip schema");
    EXPECT_NE(refusal.find("animation 'Swing'"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("schemaVersion 99"), std::string::npos) << refusal;
}

TEST(GltfImport, ClipSchemaWithAMalformedFieldIsRefusedByName)
{
    // Every field is well formed but the second event, which has no name.
    const ClipLoad load = LoadClip(SwingClipWithExtras(R"({"clip": {"schemaVersion": 1, "loop": false,
        "events": [{"frame": 3, "name": "hit"}, {"frame": 7}]}})"),
                                   "swing.glb");
    ExpectLoadedWithoutItsSchema(load);
    const std::string refusal = TestLog::FirstLineContaining(load.Lines, "clip schema");
    EXPECT_NE(refusal.find("animation 'Swing'"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("clip.events[1] has no name"), std::string::npos) << refusal;
}

TEST(GltfImport, ClipSchemaInExtrasNestedPastSixteenLevelsIsRefused)
{
    const ClipLoad atLimit = LoadClip(SwingClipWithExtras(ClipExtrasNested(16u)), "swing.glb");
    ASSERT_TRUE(atLimit.Loaded);
    EXPECT_EQ(atLimit.Clip->GetSettings().Loop, std::optional<bool>(false)) << "extras 16 levels deep are read";

    const ClipLoad pastLimit = LoadClip(SwingClipWithExtras(ClipExtrasNested(17u)), "swing.glb");
    ExpectLoadedWithoutItsSchema(pastLimit);
    const std::string refusal = TestLog::FirstLineContaining(pastLimit.Lines, "clip schema");
    EXPECT_NE(refusal.find("animation 'Swing'"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("deeper than 16 levels"), std::string::npos) << refusal;
}

TEST(GltfImport, ClipSchemaInExtrasOverTheObjectBoundIsRefused)
{
    const ClipLoad atBound = LoadClip(SwingClipWithExtras(ClipExtrasOf(ModelExtras::kObjectBoundBytes)), "swing.glb");
    ASSERT_TRUE(atBound.Loaded);
    EXPECT_EQ(atBound.Clip->GetSettings().Loop, std::optional<bool>(false)) << "extras of 256 KiB are read";

    const ClipLoad overBound =
        LoadClip(SwingClipWithExtras(ClipExtrasOf(ModelExtras::kObjectBoundBytes + 1u)), "swing.glb");
    ExpectLoadedWithoutItsSchema(overBound);
    const std::string refusal = TestLog::FirstLineContaining(overBound.Lines, "clip schema");
    EXPECT_NE(refusal.find("animation 'Swing'"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("262145 bytes"), std::string::npos) << refusal;
}

TEST(GltfImport, ClipSchemaWithAFieldTheSchemaDoesNotDefineIsRefusedByName)
{
    const SchemaRead read = ReadSchema(R"({"clip": {"schemaVersion": 1, "loop": false, "looping": true}})");
    EXPECT_NE(read.Refusal.find(R"(clip has the field "looping")"), std::string::npos) << read.Refusal;
    EXPECT_FALSE(read.Settings.Loop.has_value());
}

TEST(GltfImport, ClipSchemaRefusedAfterAValidFieldSetsNoField)
{
    // "loop" is read before "speed" (fields are read in key order); the refusal of "speed" discards it.
    const SchemaRead read = ReadSchema(R"({"clip": {"schemaVersion": 1, "loop": false, "speed": "fast"}})");
    EXPECT_NE(read.Refusal.find("clip.speed is not a number"), std::string::npos) << read.Refusal;
    EXPECT_FALSE(read.Settings.Loop.has_value());
}

TEST(GltfImport, ClipSchemaSpeedOfZeroIsRefused)
{
    const SchemaRead read = ReadSchema(R"({"clip": {"schemaVersion": 1, "speed": 0}})");
    EXPECT_NE(read.Refusal.find("clip.speed 0 is not greater than 0"), std::string::npos) << read.Refusal;
    EXPECT_FALSE(read.Settings.Speed.has_value());
}

TEST(GltfImport, ClipSchemaSpeedThatRoundsToZeroAsAFloatIsRefused)
{
    const SchemaRead read = ReadSchema(R"({"clip": {"schemaVersion": 1, "speed": 1e-50}})");
    EXPECT_NE(read.Refusal.find("clip.speed 1e-50 rounds to 0 as a 32-bit float"), std::string::npos) << read.Refusal;
    EXPECT_FALSE(read.Settings.Speed.has_value());
}

TEST(GltfImport, ClipSchemaRootMotionWithoutABoneIsRefused)
{
    const SchemaRead read = ReadSchema(R"({"clip": {"schemaVersion": 1, "rootMotion": {"translation": true}}})");
    EXPECT_NE(read.Refusal.find("clip.rootMotion names no bone"), std::string::npos) << read.Refusal;
    EXPECT_FALSE(read.Settings.RootMotion.has_value());
}

TEST(GltfImport, ClipSchemaNameWithAControlCharacterIsRefused)
{
    const SchemaRead event = ReadSchema(R"({"clip": {"schemaVersion": 1, "events": [{"frame": 3, "name": "a\u0000b"}]}})");
    EXPECT_NE(event.Refusal.find("clip.events[0].name holds a control character"), std::string::npos) << event.Refusal;
    EXPECT_TRUE(event.Events.GetEvents().empty());

    const SchemaRead bone = ReadSchema(R"({"clip": {"schemaVersion": 1, "rootMotion": {"bone": "root\n"}}})");
    EXPECT_NE(bone.Refusal.find("clip.rootMotion.bone holds a control character"), std::string::npos) << bone.Refusal;
}

TEST(GltfImport, ClipSchemaExtrasWithAKeyTwiceInOneObjectAreRefused)
{
    const SchemaRead read = ReadSchema(R"({"clip": {"schemaVersion": 1, "loop": true, "loop": false}})");
    EXPECT_NE(read.Refusal.find(R"(the key "loop" twice in one object)"), std::string::npos) << read.Refusal;
    EXPECT_FALSE(read.Settings.Loop.has_value());
}

TEST(GltfImport, ClipSchemaWholeNumbersWrittenWithADecimalPointAreRead)
{
    const SchemaRead whole = ReadSchema(R"({"clip": {"schemaVersion": 1.0, "events": [{"frame": 3.0, "name": "hit"}]}})");
    ASSERT_TRUE(whole.Refusal.empty()) << whole.Refusal;
    ASSERT_EQ(whole.Events.GetEvents().size(), 1u);
    EXPECT_FLOAT_EQ(whole.Events.GetEvents()[0].Time, 0.3f);

    const SchemaRead fraction = ReadSchema(R"({"clip": {"schemaVersion": 1, "events": [{"frame": 2.5, "name": "hit"}]}})");
    EXPECT_NE(fraction.Refusal.find("clip.events[0].frame is not a whole number of 0 or more"), std::string::npos)
        << fraction.Refusal;
}

TEST(GltfImport, ClipSchemaEventAfterTheClipEndIsRefused)
{
    // Swing lasts one second at 10 frames per second: frame 10 is its last.
    const SchemaRead atEnd = ReadSchema(R"({"clip": {"schemaVersion": 1, "events": [{"frame": 10, "name": "end"}]}})");
    ASSERT_TRUE(atEnd.Refusal.empty()) << atEnd.Refusal;
    ASSERT_EQ(atEnd.Events.GetEvents().size(), 1u);
    EXPECT_FLOAT_EQ(atEnd.Events.GetEvents()[0].Time, 1.0f);

    const SchemaRead pastEnd = ReadSchema(R"({"clip": {"schemaVersion": 1, "events": [{"frame": 11, "name": "late"}]}})");
    EXPECT_NE(pastEnd.Refusal.find(R"(clip.events[0] ("late") is at frame 11, after the clip ends at frame 10)"),
              std::string::npos)
        << pastEnd.Refusal;
    EXPECT_TRUE(pastEnd.Events.GetEvents().empty());
}

TEST(GltfImport, ClipSchemaEventLessThanAFramePastTheEndIsRefused)
{
    // A clip keyed at 10 frames per second that ends at 1.06 s: its end is frame 10.6, so frame 11 (1.1 s) is after it.
    const SchemaRead read = ReadSchema(R"({"clip": {"schemaVersion": 1, "events": [{"frame": 11, "name": "late"}]}})",
                                       {ChannelKeyedAt(11u, 10.0)}, 1.06f);
    EXPECT_NE(read.Refusal.find("is at frame 11, after the clip ends at frame 10.6"), std::string::npos) << read.Refusal;
    EXPECT_TRUE(read.Events.GetEvents().empty());
}

TEST(GltfImport, ClipSchemaEventFramesUseTheRateOfTheChannelWithTheMostKeys)
{
    // The first channel is keyed twice a second, the second ten times: frame 3 is at 0.3 s.
    const SchemaRead read = ReadSchema(R"({"clip": {"schemaVersion": 1, "events": [{"frame": 3, "name": "hit"}]}})",
                                       {ChannelKeyedAt(3u, 2.0), ChannelKeyedAt(11u, 10.0)}, 1.0f);
    ASSERT_TRUE(read.Refusal.empty()) << read.Refusal;
    ASSERT_EQ(read.Events.GetEvents().size(), 1u);
    EXPECT_FLOAT_EQ(read.Events.GetEvents()[0].Time, 0.3f);
}

TEST(GltfImport, ClipSchemaRefusalQuotesAtMost64BytesOfFileText)
{
    const std::string key(1000u, 'k');
    const SchemaRead read = ReadSchema(R"({"clip": {"schemaVersion": 1, ")" + key + R"(": 1}})");
    const std::string quoted = "\"" + std::string(64u, 'k') + "\"... (1000 bytes)";
    EXPECT_NE(read.Refusal.find(quoted), std::string::npos) << read.Refusal;
    EXPECT_LT(read.Refusal.size(), 256u);
}

TEST(GltfImport, ClipSchemaSettingsAndEventsStayWithTheImportedClip)
{
    const ClipLoad source = LoadClip(SwingClipWithExtras(R"({"clip": {"schemaVersion": 1, "loop": false,
        "events": [{"frame": 3, "name": "hit"}]}})"),
                                     "swing.glb");
    ASSERT_TRUE(source.Loaded);
    ASSERT_EQ(source.Clip->GetEventTrack().GetEvents().size(), 1u);

    // The editable copy the animation window makes, saved as .anim, a format that holds neither.
    AnimationClip editable(GUID::Generate(), "swing.anim");
    editable.CopyFrom(*source.Clip);
    EXPECT_EQ(editable.GetChannels().size(), 1u);
    ExpectNoSettingsOrEvents(editable);
    Vector<uint8> saved;
    ASSERT_TRUE(editable.SaveToData(saved));
    AnimationClip reloaded(GUID::Generate(), "swing.anim");
    ASSERT_TRUE(reloaded.LoadFromData(saved));
    ExpectNoSettingsOrEvents(reloaded);

    // Undo restores editable data into whichever clip holds it, a clip read from glTF included.
    ASSERT_TRUE(editable.RestoreFromData(saved));
    ExpectNoSettingsOrEvents(editable);
    ASSERT_TRUE(source.Clip->RestoreFromData(saved));
    ExpectNoSettingsOrEvents(*source.Clip);
}

TEST(GltfImport, ClipSchemaSpeedLargerThanAFloatHoldsIsRefused)
{
    const SchemaRead tooLarge = ReadSchema(R"({"clip": {"schemaVersion": 1, "speed": 1e39}})");
    EXPECT_NE(tooLarge.Refusal.find("is larger than a 32-bit float holds"), std::string::npos) << tooLarge.Refusal;
    EXPECT_FALSE(tooLarge.Settings.Speed.has_value());

    const SchemaRead largest = ReadSchema(R"({"clip": {"schemaVersion": 1, "speed": 3.4e38}})");
    ASSERT_TRUE(largest.Refusal.empty()) << largest.Refusal;
    EXPECT_EQ(largest.Settings.Speed, std::optional<float32>(3.4e38f));
}

TEST(GltfImport, ClipSchemaInExtrasWithObjectsNestedPastSixteenLevelsIsRefused)
{
    const SchemaRead atLimit = ReadSchema(ClipExtrasNestedInObjects(16u));
    ASSERT_TRUE(atLimit.Refusal.empty()) << atLimit.Refusal;
    EXPECT_EQ(atLimit.Settings.Loop, std::optional<bool>(false));

    const SchemaRead pastLimit = ReadSchema(ClipExtrasNestedInObjects(17u));
    EXPECT_NE(pastLimit.Refusal.find("deeper than 16 levels"), std::string::npos) << pastLimit.Refusal;
    EXPECT_FALSE(pastLimit.Settings.Loop.has_value());
}

TEST(GltfImport, ClipSchemaKeysAreCheckedInTheObjectThatHoldsThem)
{
    // "rootMotion" twice in the clip, with the first one's object closed in between.
    const SchemaRead twice = ReadSchema(R"({"clip": {"rootMotion": {"bone": "root"}, "rootMotion": {"bone": "hips"},
        "schemaVersion": 1}})");
    EXPECT_NE(twice.Refusal.find(R"(the key "rootMotion" twice in one object)"), std::string::npos) << twice.Refusal;

    // "bone" in the closed "rootMotion" object and again in the extras object that holds the clip.
    const SchemaRead apart = ReadSchema(R"({"clip": {"rootMotion": {"bone": "root"}, "schemaVersion": 1}, "bone": 2})");
    ASSERT_TRUE(apart.Refusal.empty()) << apart.Refusal;
    ASSERT_TRUE(apart.Settings.RootMotion.has_value());
    EXPECT_EQ(apart.Settings.RootMotion->Bone, "root");
}

TEST(GltfImport, ClipSchemaEventOnTheLastFrameOfAFloatKeyedClipIsRead)
{
    // Key times are 32-bit floats, so the end frame, length x rate, lands a rounding error off a whole frame; an
    // event on the last frame is read and one on the next is refused.
    struct FloatKeyedClip
    {
        size_t LastFrame;
        double FramesPerSecond;
        std::vector<AnimChannel> Channels;
    };
    const FloatKeyedClip clips[] = {
        // Keyed on frames 0 to 29 at 24 frames per second: the end is just under frame 29.
        {29u, 24.0, {ChannelKeyedAt(30u, 24.0)}},
        // The channel with the most keys covers the first half and a two-key channel sets the length: past 1024 s
        // at 25 frames per second and past 2048 s at 30 the end is thousandths of a frame under the last frame.
        {25605u, 25.0, {ChannelKeyedAt(12803u, 25.0), ChannelEndingAt(KeyTime(25605u, 25.0))}},
        {61457u, 30.0, {ChannelKeyedAt(30729u, 30.0), ChannelEndingAt(KeyTime(61457u, 30.0))}},
    };
    for (const FloatKeyedClip& clip : clips)
    {
        const float duration = KeyTime(clip.LastFrame, clip.FramesPerSecond);
        const SchemaRead last = ReadSchema(ClipEventAt(clip.LastFrame), clip.Channels, duration);
        EXPECT_TRUE(last.Refusal.empty()) << clip.LastFrame << " at " << clip.FramesPerSecond << ": " << last.Refusal;
        const SchemaRead next = ReadSchema(ClipEventAt(clip.LastFrame + 1u), clip.Channels, duration);
        EXPECT_NE(next.Refusal.find("after the clip ends"), std::string::npos)
            << clip.LastFrame + 1u << " at " << clip.FramesPerSecond << ": " << next.Refusal;
    }
}

TEST(GltfImport, ClipSchemaRefusalPrintsTheClipEndAsAPlainNumber)
{
    const std::vector<AnimChannel> channels = {ChannelKeyedAt(12346u, 30.0)};
    const SchemaRead read = ReadSchema(ClipEventAt(12400u), channels, KeyTime(12345u, 30.0));
    EXPECT_NE(read.Refusal.find("is at frame 12400, after the clip ends at frame 12345 (30 frames per second)"),
              std::string::npos)
        << read.Refusal;
}

TEST(GltfImport, ClipSchemaExtrasFullOfObjectsAreReadInLinearTime)
{
    // The 256 KiB bound filled with empty objects, the input with the most objects per byte. The reader's passes
    // are linear in the text, so this reads in about a tenth of a second in DebugFast; a parse whose cost grows with
    // the square of the object count takes minutes.
    std::string extras = R"({"clip": {"schemaVersion": 1, "loop": false}, "vendor": [{})";
    constexpr std::string_view kNextObject = ",{}";
    constexpr std::string_view kClose = "]}";
    while (extras.size() + kNextObject.size() + kClose.size() <= ModelExtras::kObjectBoundBytes)
        extras += kNextObject;
    extras += kClose;
    ASSERT_LE(extras.size(), ModelExtras::kObjectBoundBytes);

    std::chrono::milliseconds budget{5000};
#if defined(GE_ENABLE_ASAN) && GE_ENABLE_ASAN
    budget *= 6;
#endif
    const auto start = std::chrono::steady_clock::now();
    const SchemaRead read = ReadSchema(extras);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    ASSERT_TRUE(read.Refusal.empty()) << read.Refusal;
    EXPECT_EQ(read.Settings.Loop, std::optional<bool>(false));
    RecordProperty("readMilliseconds", static_cast<int>(elapsed.count()));
    EXPECT_LT(elapsed, budget) << "read " << extras.size() << " bytes of empty objects in " << elapsed.count() << " ms";
}

TEST(GltfImport, ClipSchemaInExtrasArraysOneAfterAnotherDoNotNestDeeper)
{
    for (const auto& [arrays, inVendorObject] : {std::pair{16u, false}, std::pair{100u, true}})
    {
        const SchemaRead read = ReadSchema(ClipExtrasWithSiblingArrays(arrays, inVendorObject));
        EXPECT_TRUE(read.Refusal.empty()) << arrays << " arrays: " << read.Refusal;
        EXPECT_EQ(read.Settings.Loop, std::optional<bool>(false)) << arrays << " arrays";
    }
}

TEST(GltfImport, ClipSchemaEventAfterTheEndOfAVeryLongClipIsRefused)
{
    // 16 frames per second keeps every time exact in a float: the rate comes from a channel keyed at 0 and 1/16 s
    // (the first of the channels with the most keys), the length from one ending at the last frame. At 2^21 frames
    // four float epsilons of the end are one frame, at 2^22 two; the tolerance stays at half a frame.
    for (const size_t lastFrame : {size_t{1} << 21u, size_t{1} << 22u})
    {
        const float duration = KeyTime(lastFrame, 16.0);
        const std::vector<AnimChannel> channels = {ChannelEndingAt(KeyTime(1u, 16.0)), ChannelEndingAt(duration)};
        const SchemaRead last = ReadSchema(ClipEventAt(lastFrame), channels, duration);
        EXPECT_TRUE(last.Refusal.empty()) << lastFrame << ": " << last.Refusal;
        const SchemaRead next = ReadSchema(ClipEventAt(lastFrame + 1u), channels, duration);
        EXPECT_NE(next.Refusal.find("after the clip ends"), std::string::npos)
            << lastFrame + 1u << ": " << next.Refusal;
    }
}

TEST(GltfImport, ClipSchemaRefusalPrintsTheFileFrameInItsShortestForm)
{
    const SchemaRead read = ReadSchema(R"({"clip": {"schemaVersion": 1, "events": [{"frame": 1e300, "name": "e"}]}})");
    EXPECT_NE(read.Refusal.find("is at frame 1e+300, after the clip ends at frame 10 (10 frames per second)"),
              std::string::npos)
        << read.Refusal;
    EXPECT_LT(read.Refusal.size(), 160u);
}


// One triangle, (1,0,0), (0,1,0), (0,0,1), drawn by two nodes: the first moved 1 along +X, the
// second 2 along +Y and turned 180 degrees about Y.
constexpr std::string_view kTwoNodesShareOneMesh = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0, 1]}],
  "nodes": [{"mesh": 0, "translation": [1, 0, 0]},
            {"mesh": 0, "translation": [0, 2, 0], "rotation": [0, 1, 0, 0]}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";

TEST(GltfImport, EveryNodeThatDrawsAMeshPlacesIt)
{
    ModelAsset asset(GUID::Generate(), "shared.gltf");
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, Bytes(kTwoNodesShareOneMesh), FbxLoaderOptions{}));
    ASSERT_EQ(asset.GetMeshCount(), 1u);
    const Mesh& mesh = asset.GetMesh(0);

    // The default import mirrors X: the first node sits at -1 on X, the turn about Y stays a turn about Y.
    EXPECT_EQ(mesh.SourceNodeIndex, 0);
    EXPECT_FLOAT_EQ(mesh.SourceNodeTransform[12], -1.0f);
    ASSERT_EQ(mesh.ExtraPlacements.size(), 1u);
    const MeshPlacement& second = mesh.ExtraPlacements[0];
    EXPECT_EQ(second.SourceNodeIndex, 1);
    EXPECT_NEAR(second.SourceNodeTransform[0], -1.0f, 1e-6f);
    EXPECT_NEAR(second.SourceNodeTransform[10], -1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(second.SourceNodeTransform[12], 0.0f);
    EXPECT_FLOAT_EQ(second.SourceNodeTransform[13], 2.0f);

    // The model's box holds the triangle at both nodes: [-2,0,0]..[-1,1,1] and [0,2,-1]..[1,3,0].
    float minimum[3];
    float maximum[3];
    asset.GetBoundingBox(minimum, maximum);
    EXPECT_NEAR(minimum[0], -2.0f, 1e-6f);
    EXPECT_NEAR(minimum[1], 0.0f, 1e-6f);
    EXPECT_NEAR(minimum[2], -1.0f, 1e-6f);
    EXPECT_NEAR(maximum[0], 1.0f, 1e-6f);
    EXPECT_NEAR(maximum[1], 3.0f, 1e-6f);
    EXPECT_NEAR(maximum[2], 1.0f, 1e-6f);
}

} // namespace GameEngine
