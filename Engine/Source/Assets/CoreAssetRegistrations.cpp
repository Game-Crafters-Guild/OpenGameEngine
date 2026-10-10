#include "Assets/CoreAssetRegistrations.h"

#include "Animation/HumanoidRig.h"
#include "Animation/AnimationController.h"
#include "Animation/AnimationLibrary.h"
#include "Animation/RetargetMap.h"
#include "Animation/SkeletonProfile.h"
#include "Animation/SpriteFrames.h"
#include "Assets/AudioAsset.h"
#include "Assets/AnimationClip.h"
#include "Assets/BinaryAsset.h"
#include "Assets/ClipSetAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/RenderPipelineAsset.h"
#include "Assets/ScriptAsset.h"
#include "Assets/ShaderProgramAsset.h"
#include "Assets/ShaderSourceAsset.h"
#include "Assets/TextureAsset.h"
#include "Assets/TimelineAsset.h"
#include "Assets/AnimationGraphAsset.h"
#include "Assets/NavGridAsset.h"
#include "Assets/NavMeshAsset.h"
#include "Assets/SceneAsset.h"
#include "Graph/GraphAsset.h"

#include "AssetCore/AssetTypes.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace GameEngine
{

namespace
{
constexpr int32 kCoreAssetTypePriority = 100;
constexpr int32 kScriptAssetTypePriority = 50;

template <typename TAsset>
SharedPtr<Asset> CreateAssetAtPath(const AssetMetadata& metadata)
{
    return std::make_shared<TAsset>(metadata.Guid, metadata.Path);
}

// BinaryAsset keeps the typed category with generic storage/path handling, for
// payloads their owning systems decode themselves.
SharedPtr<Asset> CreateBinaryAsset(const AssetMetadata& metadata)
{
    return std::make_shared<BinaryAsset>(metadata.Guid, metadata.Path, metadata.Type);
}

// A .shader file is a program description; every other shader extension is source text.
SharedPtr<Asset> CreateShaderAsset(const AssetMetadata& metadata)
{
    std::string ext = metadata.Path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });

    if (ext == ".shader")
        return std::make_shared<ShaderProgramAsset>(metadata.Guid, metadata.Path);
    return std::make_shared<ShaderSourceAsset>(metadata.Guid, metadata.Path);
}

// Registers a factory for the type under the type's default extensions. A type
// that is already registered keeps its registration.
void RegisterCoreType(AssetTypeRegistry& typeRegistry, AssetType type, AssetFactory factory,
                      const String& description, int32 priority = kCoreAssetTypePriority)
{
    if (typeRegistry.IsAssetTypeRegistered(type))
        return;

    const AssetTypeRegistration registration(type, GetDefaultExtensionsForAssetType(type),
                                             std::move(factory), description, priority);
    if (typeRegistry.RegisterAssetType(registration))
        Logger::Log::Info("{} type registered successfully", description);
    else
        Logger::Log::Warning("Failed to register {} type", description);
}
} // namespace

void RegisterCoreAssetTypes(AssetTypeRegistry& typeRegistry)
{
    RegisterCoreType(typeRegistry, AssetType::Audio, &CreateAssetAtPath<AudioAsset>, "Audio Asset");

    // Not on wasm: model decode is the desktop import pipeline (see
    // ParserRegistry); wasm loads cooked meshes instead.
#if !defined(__EMSCRIPTEN__)
    RegisterCoreType(typeRegistry, AssetType::Model, &CreateAssetAtPath<ModelAsset>, "Model Asset");
#endif

    RegisterCoreType(typeRegistry, AssetType::Texture, &CreateAssetAtPath<TextureAsset>, "Texture Asset");
    RegisterCoreType(typeRegistry, AssetType::Animation, &CreateAssetAtPath<AnimationClip>, "Animation Asset");

    // Humanoid retargeting. The compound extensions (.profile.json /
    // .humanoidrig.json / .retargetmap.json) are recognized via AssetParser
    // subclasses; these factories construct the right Asset subclass once the
    // type is known.
    RegisterCoreType(typeRegistry, AssetType::SkeletonProfile,
                     &CreateAssetAtPath<Animation::SkeletonProfile>, "Skeleton Profile");

    // Animation libraries provide stable names for clip, timeline, montage, and
    // controller references.
    RegisterCoreType(typeRegistry, AssetType::AnimationLibrary,
                     &CreateAssetAtPath<Animation::AnimationLibrary>, "Animation Library");
    RegisterCoreType(typeRegistry, AssetType::AnimationController,
                     &CreateAssetAtPath<Animation::AnimationController>, "Animation Controller");
    RegisterCoreType(typeRegistry, AssetType::SpriteFrames,
                     &CreateAssetAtPath<Animation::SpriteFrames>, "Sprite Frames");
    RegisterCoreType(typeRegistry, AssetType::Timeline, &CreateAssetAtPath<TimelineAsset>, "Timeline Asset");
    RegisterCoreType(typeRegistry, AssetType::AnimationGraph,
                     &CreateAssetAtPath<AnimationGraphAsset>, "Animation Graph");
    RegisterCoreType(typeRegistry, AssetType::ClipSet, &CreateAssetAtPath<ClipSetAsset>, "Clip Set Asset");
    RegisterCoreType(typeRegistry, AssetType::HumanoidRig,
                     &CreateAssetAtPath<Animation::HumanoidRig>, "Humanoid Rig");
    RegisterCoreType(typeRegistry, AssetType::RetargetMap,
                     &CreateAssetAtPath<Animation::RetargetMap>, "Retarget Map");

    RegisterCoreType(typeRegistry, AssetType::Material, &CreateAssetAtPath<MaterialAsset>, "Material Asset");
    RegisterCoreType(typeRegistry, AssetType::Shader, &CreateShaderAsset, "Shader Asset");

    // Script compilation and hot-reload remain the responsibility of
    // ScriptManager; the asset system just loads script text.
    RegisterCoreType(typeRegistry, AssetType::Script, &CreateAssetAtPath<ScriptAsset>, "Script Asset",
                     kScriptAssetTypePriority);

    // Text content for inspector and tooling (runtime loads via SceneIO).
    RegisterCoreType(typeRegistry, AssetType::Scene, &CreateAssetAtPath<SceneAsset>, "Scene Asset");

    RegisterCoreType(typeRegistry, AssetType::NavigationGrid, &CreateAssetAtPath<NavGridAsset>,
                     "Navigation Grid Asset");
    RegisterCoreType(typeRegistry, AssetType::NavigationMesh, &CreateAssetAtPath<NavMeshAsset>,
                     "Navigation Mesh Asset");

    // Ocean payloads are consumed by the ocean systems directly.
    RegisterCoreType(typeRegistry, AssetType::OceanDepthCache, &CreateBinaryAsset, "Ocean Depth Cache");
    RegisterCoreType(typeRegistry, AssetType::OceanWaveSpectrum, &CreateBinaryAsset, "Ocean Wave Spectrum");
    RegisterCoreType(typeRegistry, AssetType::OceanFFTCollision, &CreateBinaryAsset, "Ocean FFT Collision");
    RegisterCoreType(typeRegistry, AssetType::OceanSettings, &CreateBinaryAsset, "Ocean Settings");
    RegisterCoreType(typeRegistry, AssetType::OceanPreset, &CreateBinaryAsset, "Ocean Preset");

    // Raw terrain heightmaps (.r16/.r32). 16-bit PNG heightmaps import as
    // regular Texture assets; this type covers only the raw formats textures
    // don't own. The HeightfieldData loaders decode them at bake time (square
    // dims inferred from file size).
    RegisterCoreType(typeRegistry, AssetType::TerrainHeightmap, &CreateBinaryAsset, "Terrain Heightmap");

    // One zone's brush-authored R32F offset field or R8 weight mask,
    // GUID-referenced from a TerrainSculptZone / TerrainPaintZone component.
    // TerrainZonePayloadCodec decodes/encodes it in TerrainService at bake and
    // save time.
    RegisterCoreType(typeRegistry, AssetType::TerrainZoneData, &CreateBinaryAsset, "Terrain Zone Data");

    // The sparse dab + modifier page store of a spherical terrain's authored
    // sculpt, GUID-referenced from the Terrain component. The CBTTerrain
    // SphereSculptSerialization codec decodes/encodes it at load and save time.
    RegisterCoreType(typeRegistry, AssetType::TerrainSphereSculptData, &CreateBinaryAsset,
                     "Terrain Sphere Sculpt Data");

    RegisterCoreType(typeRegistry, AssetType::RenderPipeline, &CreateAssetAtPath<RenderPipelineAsset>,
                     "Render Pipeline Asset");

    // Material and game-logic graphs.
    RegisterCoreType(typeRegistry, AssetType::Graph, &CreateAssetAtPath<GraphAsset>, "Graph Asset");
}

} // namespace GameEngine
