#include "EZTreeECS/Systems/EZTreeExtractionSystem.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "EZTree/EZTreeOptions.h"
#include "EZTreeECS/Components/EZTreeComponent.h"
#include "EZTreeECS/EZTreeDefaultTextures.h"
#include "EZTreeECS/EZTreeRuntimeMaterials.h"
#include "EZTreeECS/EZTreeService.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/WindVolumeResolver.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Rendering/Common/MatrixUtils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/GPUInstanceWorldKey.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>


namespace GameEngine::EZTreeECS
{
namespace
{

GUID RuntimeGuidFromComponent(ECS::EntityHandle entity, uint64 hash)
{
    return GUID::Derive(GUID::Null(),
                        "engine/ez-tree/entity/" + std::to_string(entity.id) + "/" + std::to_string(hash));
}

GUID RuntimeMaterialGuidFromComponent(ECS::EntityHandle entity)
{
    return GUID::Derive(GUID::Null(), "engine/ez-tree/material/entity/" + std::to_string(entity.id) + "/bark");
}

GUID RuntimeFallbackMaterialGuidFromComponent(ECS::EntityHandle entity)
{
    return GUID::Derive(GUID::Null(), "engine/ez-tree/material/entity/" + std::to_string(entity.id) + "/bark-static");
}

GUID RuntimeLeafMaterialGuidFromComponent(ECS::EntityHandle entity)
{
    return GUID::Derive(GUID::Null(), "engine/ez-tree/material/entity/" + std::to_string(entity.id) + "/leaves");
}

GUID RuntimeLeafFallbackMaterialGuidFromComponent(ECS::EntityHandle entity)
{
    return GUID::Derive(GUID::Null(), "engine/ez-tree/material/entity/" + std::to_string(entity.id) + "/leaves-static");
}

GUID RuntimeTrellisMaterialGuidFromComponent(ECS::EntityHandle entity)
{
    return GUID::Derive(GUID::Null(), "engine/ez-tree/material/entity/" + std::to_string(entity.id) + "/trellis");
}

GUID GuidFromArray(const std::array<uint8, 16>& bytes)
{
    GUID::Data data{};
    std::copy(bytes.begin(), bytes.end(), data.begin());
    return GUID(data);
}

void WriteGuidToArray(const GUID& guid, std::array<uint8, 16>& out)
{
    std::copy(guid.GetData().begin(), guid.GetData().end(), out.begin());
}

void WriteGuidToBytes(const GUID& guid, uint8 (&out)[16])
{
    std::memcpy(out, guid.GetData().data(), 16);
}

GUID MaterialOrDefault(const std::array<uint8, 16>& materialBytes)
{
    GUID material = GuidFromArray(materialBytes);
    if (material.IsNull())
        material = Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid();
    return material;
}

std::vector<float> ColorFromHex(uint32 rgb, float alpha = 1.0f)
{
    return {
        static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(rgb & 0xFFu) / 255.0f,
        alpha};
}

uint32 DefaultBarkTint(EZTree::BarkType type)
{
    switch (type)
    {
    case EZTree::BarkType::Birch:  return 0xB8B2A3u;
    case EZTree::BarkType::Pine:   return 0x5A3A24u;
    case EZTree::BarkType::Willow: return 0x6B5A3Au;
    case EZTree::BarkType::Oak:
    default:                       return 0x6E4A2Fu;
    }
}

uint32 DefaultLeafTint(EZTree::LeafType type)
{
    switch (type)
    {
    case EZTree::LeafType::Ash:   return 0x6F8F3Au;
    case EZTree::LeafType::Aspen: return 0x89A84Bu;
    case EZTree::LeafType::Pine:  return 0x254D2Eu;
    case EZTree::LeafType::Oak:
    default:                      return 0x4C7A2Eu;
    }
}

// Default textures ship inside the eztree package mount. Their GUIDs are the
// pre-extraction editor-mount originals, carried by the package's
// stored-identity .assetmanifest.
GUID ResolvePackageTextureGuid(std::filesystem::path relativePath)
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    GUID guid = assets.ResolveAssetGuid(relativePath, EZTree::kPackageAlias);
    if (!guid.IsNull())
        return guid;
    guid = assets.ResolveAssetGuid(relativePath, kAssetSourceAliasEditor);
    if (guid.IsNull())
        assets.ReportAssetNotInBuild(relativePath.generic_string());
    return guid;
}

GUID DefaultLeafTextureGuid(EZTree::LeafType type, const char* suffix = "color")
{
    return ResolvePackageTextureGuid(DefaultLeafTextureRelativePath(type, suffix));
}

GUID DefaultBarkTextureGuid(EZTree::BarkType type, const char* suffix)
{
    return ResolvePackageTextureGuid(DefaultBarkTextureRelativePath(type, suffix));
}

bool TryLoadMaterialDocument(const GUID& materialGuid, MaterialDocument& outDoc)
{
    if (materialGuid.IsNull())
        return false;

    auto& assets = EngineCore::GetInstance().GetAssetManager();
    SharedPtr<Asset> asset = assets.GetAsset(materialGuid);
    if (!asset)
        asset = assets.LoadAssetAsync(materialGuid).get();

    auto* materialAsset = dynamic_cast<MaterialAsset*>(asset.get());
    if (!materialAsset)
        return false;

    outDoc = materialAsset->GetDocument();
    return true;
}

void ApplyRuntimeWindProperties(MaterialDocument& doc,
                                const EZTree::TreeOptions& options,
                                const Components::LocalBounds* bounds,
                                float strengthScale,
                                const Engine::Renderer::ResolvedWind& wind)
{
    doc.vertexModifier = "VertexModifiers/ez_tree_wind.glsl";

    const float windHeight = bounds
        ? std::max(0.25f, bounds->Box.halfExtents.y * 2.0f)
        : 20.0f;
    const float windBaseY = bounds
        ? (bounds->Box.center.y - bounds->Box.halfExtents.y)
        : 0.0f;

    doc.properties["windStrength"] = std::vector<float>{
        wind.VelocityX * strengthScale,
        wind.VelocityY * strengthScale,
        wind.VelocityZ * strengthScale,
        options.wind.enabled ? 1.0f : 0.0f};
    doc.properties["windParams"] = std::vector<float>{
        std::max(wind.GustFrequency, 0.0f),
        std::max(0.001f, wind.GustScale),
        windHeight,
        windBaseY};
}

void SetVectorIfChanged(Engine::Renderer::Material& material,
                        StringId name,
                        const float* values,
                        uint32_t componentCount)
{
    if (!values || componentCount == 0)
        return;

    float current[4]{};
    if (componentCount <= 4 && material.GetVector(name, current, componentCount))
    {
        bool changed = false;
        for (uint32_t i = 0; i < componentCount; ++i)
        {
            if (std::fabs(current[i] - values[i]) > 0.00001f)
            {
                changed = true;
                break;
            }
        }
        if (!changed)
            return;
    }

    material.SetVector(name, values, componentCount);
}

void ApplyRuntimeWindToMaterial(Engine::Renderer::RenderServices& renderServices,
                                const GUID& materialGuid,
                                const EZTree::TreeOptions& options,
                                const Components::LocalBounds* bounds,
                                float strengthScale,
                                const Engine::Renderer::ResolvedWind& wind)
{
    auto* material = renderServices.Materials().Registry().Find(materialGuid);
    if (!material)
        return;

    const float windHeight = bounds
        ? std::max(0.25f, bounds->Box.halfExtents.y * 2.0f)
        : 20.0f;
    const float windBaseY = bounds
        ? (bounds->Box.center.y - bounds->Box.halfExtents.y)
        : 0.0f;

    const float windStrength[4] = {
        wind.VelocityX * strengthScale,
        wind.VelocityY * strengthScale,
        wind.VelocityZ * strengthScale,
        options.wind.enabled ? 1.0f : 0.0f};
    const float windParams[4] = {
        std::max(wind.GustFrequency, 0.0f),
        std::max(0.001f, wind.GustScale),
        windHeight,
        windBaseY};

    SetVectorIfChanged(*material, HashStringId("windStrength"), windStrength, 4);
    SetVectorIfChanged(*material, HashStringId("windParams"), windParams, 4);
}

void PreloadMaterialDocumentTextures(Engine::Renderer::RenderServices& renderServices,
                                     const MaterialDocument& doc)
{
    for (const auto& [_, texGuidStr] : doc.textures)
    {
        if (texGuidStr.empty())
            continue;
        const GUID texGuid(texGuidStr);
        if (!texGuid.IsNull())
            (void)renderServices.Textures().GetOrUpload(texGuid);
    }
}

MaterialDocument BuildRuntimeBarkMaterialDocument(const EZTree::TreeOptions& options,
                                                  const Components::LocalBounds* bounds,
                                                  const Engine::Renderer::ResolvedWind& wind)
{
    MaterialDocument doc{};
    const GUID authoredMaterial = GuidFromArray(options.barkMaterialGuid);
    const bool hasAuthoredMaterial = TryLoadMaterialDocument(authoredMaterial, doc);

    if (!hasAuthoredMaterial)
        doc = MakeBarkRuntimeMaterialShape();
    else
        doc.materialName = doc.materialName.empty() ? "Tree Generator Bark" : (doc.materialName + " (Tree Generator)");

    // The generated tree combines branch tubes and leaf billboards into one
    // renderable today. Keep it double-sided so leaf quads and any imported
    // EZ-Tree winding differences cannot disappear under backface culling.
    doc.doubleSided = true;

    const bool hasExplicitBarkTint = options.bark.tint != 0xFFFFFFu;
    if (!hasAuthoredMaterial || hasExplicitBarkTint)
        doc.properties["baseColor"] = ColorFromHex(hasExplicitBarkTint ? options.bark.tint : DefaultBarkTint(options.bark.type));
    ApplyRuntimeWindProperties(doc, options, bounds, 1.0f, wind);

    // Per-slot override from the component, in kDefaultTextureSlots order.
    const std::array<GUID, 4> barkOverrides{
        GuidFromArray(options.barkColorTextureGuid),
        GuidFromArray(options.barkNormalTextureGuid),
        GuidFromArray(options.barkRoughnessTextureGuid),
        GuidFromArray(options.barkAoTextureGuid)};

    const std::array<float, 8> barkUv{
        std::max(0.0001f, options.bark.textureScale.x),
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        std::max(0.0001f, options.bark.textureScale.y),
        0.0f,
        0.0f};

    for (size_t i = 0; i < std::size(kDefaultTextureSlots); ++i)
    {
        const DefaultTextureSlot& slot = kDefaultTextureSlots[i];
        GUID texture = barkOverrides[i];
        if (texture.IsNull() && options.bark.textured)
            texture = DefaultBarkTextureGuid(options.bark.type, slot.Suffix);

        if (!texture.IsNull())
            doc.textures[slot.MaterialSlot] = texture.ToString();
        else if (!hasAuthoredMaterial)
            doc.textures[slot.MaterialSlot] = std::string{};
        doc.textureTransforms[slot.MaterialSlot] = barkUv;
    }

    return doc;
}

MaterialDocument BuildRuntimeLeafMaterialDocument(const EZTree::TreeOptions& options,
                                                  const Components::LocalBounds* bounds,
                                                  const Engine::Renderer::ResolvedWind& wind)
{
    MaterialDocument doc{};
    const GUID authoredMaterial = GuidFromArray(options.leafMaterialGuid);
    const bool hasAuthoredMaterial = TryLoadMaterialDocument(authoredMaterial, doc);

    if (!hasAuthoredMaterial)
        doc = MakeLeafRuntimeMaterialShape();
    else
        doc.materialName = doc.materialName.empty() ? "Tree Generator Leaves" : (doc.materialName + " (Tree Generator)");

    doc.doubleSided = true;
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.surfaceShader = "Surfaces/ez_tree_leaves.glsl";
    doc.properties["roughness"] = 0.9f;
    doc.properties["metallic"] = 0.0f;
    ApplyRuntimeWindProperties(doc, options, bounds, 1.35f, wind);

    const bool hasExplicitLeafTint = options.leaves.tint != 0xFFFFFFu;
    if (!hasAuthoredMaterial || hasExplicitLeafTint)
        doc.properties["baseColor"] = ColorFromHex(hasExplicitLeafTint ? options.leaves.tint : DefaultLeafTint(options.leaves.type));
    doc.properties["alphaCutoff"] = std::clamp(options.leaves.alphaTest, 0.0f, 1.0f);

    // Per slot, in kDefaultTextureSlots order: the override the component
    // carries (only albedo has one for leaves), and whether a slot with no
    // texture is cleared or left holding the runtime shape's own value —
    // albedo and ambient occlusion clear, normal and roughness do not.
    const std::array<GUID, 4> leafOverrides{
        GuidFromArray(options.leafColorTextureGuid), GUID::Null(), GUID::Null(), GUID::Null()};
    const std::array<bool, 4> leafClearsWhenAbsent{true, false, false, true};

    const std::array<float, 8> leafUv{
        std::max(0.0001f, options.leaves.textureScale.x),
        0.0f,
        options.leaves.textureOffset.x,
        0.0f,
        0.0f,
        std::max(0.0001f, options.leaves.textureScale.y),
        options.leaves.textureOffset.y,
        0.0f};

    for (size_t i = 0; i < std::size(kDefaultTextureSlots); ++i)
    {
        const DefaultTextureSlot& slot = kDefaultTextureSlots[i];
        GUID texture = leafOverrides[i];
        if (texture.IsNull())
            texture = DefaultLeafTextureGuid(options.leaves.type, slot.Suffix);

        if (!texture.IsNull())
            doc.textures[slot.MaterialSlot] = texture.ToString();
        else if (!hasAuthoredMaterial && leafClearsWhenAbsent[i])
            doc.textures[slot.MaterialSlot] = std::string{};
        doc.textureTransforms[slot.MaterialSlot] = leafUv;
    }

    return doc;
}

MaterialDocument BuildRuntimeTrellisMaterialDocument(const EZTree::TreeOptions& options)
{
    MaterialDocument doc{};
    const GUID authoredMaterial = GuidFromArray(options.trellisMaterialGuid);
    const bool hasAuthoredMaterial = TryLoadMaterialDocument(authoredMaterial, doc);

    if (!hasAuthoredMaterial)
        doc = MakeTrellisRuntimeMaterialShape();
    else
        doc.materialName = doc.materialName.empty() ? "Tree Generator Trellis" : (doc.materialName + " (Tree Generator)");

    doc.doubleSided = true;
    if (!hasAuthoredMaterial)
        doc.properties["baseColor"] = ColorFromHex(options.trellis.color);

    const GUID trellisTexture = GuidFromArray(options.trellisTextureGuid);
    if (!trellisTexture.IsNull())
        doc.textures["albedoMap"] = trellisTexture.ToString();
    else if (!hasAuthoredMaterial)
        doc.textures["albedoMap"] = std::string{};

    return doc;
}

bool HasValidMaterial(Engine::Renderer::RenderServices& renderServices, const GUID& guid)
{
    const auto* material = renderServices.Materials().Registry().Find(guid);
    return material && material->GetGraphicsPipelineId().IsValid();
}

GUID SelectBarkMaterial(Engine::Renderer::RenderServices& renderServices, ECS::EntityHandle entity)
{
    const GUID runtimeGuid = RuntimeMaterialGuidFromComponent(entity);
    if (HasValidMaterial(renderServices, runtimeGuid))
        return runtimeGuid;
    return RuntimeFallbackMaterialGuidFromComponent(entity);
}

GUID SelectLeafMaterial(Engine::Renderer::RenderServices& renderServices, ECS::EntityHandle entity)
{
    const GUID runtimeGuid = RuntimeLeafMaterialGuidFromComponent(entity);
    if (HasValidMaterial(renderServices, runtimeGuid))
        return runtimeGuid;
    return RuntimeLeafFallbackMaterialGuidFromComponent(entity);
}

GUID SelectTrellisMaterial(ECS::EntityHandle entity)
{
    return RuntimeTrellisMaterialGuidFromComponent(entity);
}

bool TreeRuntimeMaterialsAvailable(Engine::Renderer::RenderServices& renderServices, ECS::EntityHandle entity)
{
    // Fallback materials deliberately omit the wind vertex modifier. They keep
    // the tree visible while shader services are starting, but must not make
    // the animated runtime materials look ready or a transient startup miss
    // leaves the tree on the static fallback forever.
    const bool hasBark = HasValidMaterial(renderServices, RuntimeMaterialGuidFromComponent(entity));
    const bool hasLeaf = HasValidMaterial(renderServices, RuntimeLeafMaterialGuidFromComponent(entity));
    const bool hasTrellis = HasValidMaterial(renderServices, RuntimeTrellisMaterialGuidFromComponent(entity));
    return hasBark && hasLeaf && hasTrellis;
}

void ResolveBarkMaterial(Engine::Renderer::RenderServices& renderServices,
                         ECS::EntityHandle entity,
                         const EZTree::TreeOptions& options,
                         const Components::LocalBounds* bounds,
                         const Engine::Renderer::ResolvedWind& wind)
{
    const GUID runtimeGuid = RuntimeMaterialGuidFromComponent(entity);
    MaterialDocument doc = BuildRuntimeBarkMaterialDocument(options, bounds, wind);
    PreloadMaterialDocumentTextures(renderServices, doc);
    const Engine::Renderer::Material* material = renderServices.Materials().RegisterMaterialFromDocument(runtimeGuid, doc);
    if (material && material->GetGraphicsPipelineId().IsValid())
        return;

    // A missing/stale vertex modifier should never make the tree vanish. Fall
    // back to the same material without wind so the mesh still renders.
    doc.vertexModifier.clear();
    const GUID fallbackGuid = RuntimeFallbackMaterialGuidFromComponent(entity);
    renderServices.Materials().RegisterMaterialFromDocument(fallbackGuid, doc);
}

void ResolveLeafMaterial(Engine::Renderer::RenderServices& renderServices,
                         ECS::EntityHandle entity,
                         const EZTree::TreeOptions& options,
                         const Components::LocalBounds* bounds,
                         const Engine::Renderer::ResolvedWind& wind)
{
    const GUID runtimeGuid = RuntimeLeafMaterialGuidFromComponent(entity);
    MaterialDocument doc = BuildRuntimeLeafMaterialDocument(options, bounds, wind);
    PreloadMaterialDocumentTextures(renderServices, doc);
    const Engine::Renderer::Material* material = renderServices.Materials().RegisterMaterialFromDocument(runtimeGuid, doc);
    if (material && material->GetGraphicsPipelineId().IsValid())
        return;

    doc.vertexModifier.clear();
    const GUID fallbackGuid = RuntimeLeafFallbackMaterialGuidFromComponent(entity);
    renderServices.Materials().RegisterMaterialFromDocument(fallbackGuid, doc);
}

void ResolveTrellisMaterial(Engine::Renderer::RenderServices& renderServices,
                            ECS::EntityHandle entity,
                            const EZTree::TreeOptions& options)
{
    const GUID guid = RuntimeTrellisMaterialGuidFromComponent(entity);
    MaterialDocument doc = BuildRuntimeTrellisMaterialDocument(options);
    PreloadMaterialDocumentTextures(renderServices, doc);
    renderServices.Materials().RegisterMaterialFromDocument(guid, doc);
}

Engine::Renderer::ResolvedWind BuildBaseWindWorld(const EZTree::TreeOptions& options,
                                                   const Components::WorldTransform& worldTransform)
{
    Engine::Renderer::ResolvedWind wind{};
    wind.GustFrequency = options.wind.frequency;
    wind.GustScale = options.wind.scale;
    if (!options.wind.enabled)
        return wind;

    const float* m = worldTransform.matrix;
    wind.VelocityX = m[0] * options.wind.strength.x + m[4] * options.wind.strength.y + m[8] * options.wind.strength.z;
    wind.VelocityY = m[1] * options.wind.strength.x + m[5] * options.wind.strength.y + m[9] * options.wind.strength.z;
    wind.VelocityZ = m[2] * options.wind.strength.x + m[6] * options.wind.strength.y + m[10] * options.wind.strength.z;
    wind.Weight = 1.0f;
    return wind;
}

Engine::Renderer::ResolvedWind WorldWindToLocal(const Engine::Renderer::ResolvedWind& worldWind,
                                                 const Components::WorldTransform& worldTransform)
{
    const float* m = worldTransform.matrix;
    float axisX[3]{m[0], m[1], m[2]};
    float axisY[3]{m[4], m[5], m[6]};
    float axisZ[3]{m[8], m[9], m[10]};
    auto normalize = [](float (&axis)[3])
    {
        const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (len <= 1e-6f)
            return;
        axis[0] /= len;
        axis[1] /= len;
        axis[2] /= len;
    };
    normalize(axisX);
    normalize(axisY);
    normalize(axisZ);

    Engine::Renderer::ResolvedWind localWind = worldWind;
    localWind.VelocityX = worldWind.VelocityX * axisX[0] + worldWind.VelocityY * axisX[1] + worldWind.VelocityZ * axisX[2];
    localWind.VelocityY = worldWind.VelocityX * axisY[0] + worldWind.VelocityY * axisY[1] + worldWind.VelocityZ * axisY[2];
    localWind.VelocityZ = worldWind.VelocityX * axisZ[0] + worldWind.VelocityY * axisZ[1] + worldWind.VelocityZ * axisZ[2];
    return localWind;
}

float EstimateWindBoundsInflation(const EZTree::TreeOptions& options,
                                  const Engine::Renderer::ResolvedWind& localWind)
{
    if (!options.wind.enabled)
        return 0.0f;

    const float windMagnitude = std::sqrt(
        localWind.VelocityX * localWind.VelocityX +
        localWind.VelocityY * localWind.VelocityY +
        localWind.VelocityZ * localWind.VelocityZ);
    if (windMagnitude <= 0.0001f)
        return 0.0f;

    // Mirrors ez_tree_wind.glsl's worst-case local displacement:
    // trunk sway (~0.32x wind) + branch sway (~0.35x) + leaf flutter (~0.4x),
    // with guard room for the stronger leaf-material wind scale.
    return windMagnitude * 1.65f;
}

void SubmitPart(Engine::Renderer::RenderServices& renderServices,
                ECS::World& world,
                [[maybe_unused]] ECS::EntityHandle entity,
                const Components::WorldTransform& worldTransform,
                const Components::LocalBounds& bounds,
                Rendering::MeshGPUHandle meshHandle,
                const GUID& materialGuid,
                float boundsInflation,
                uint32& instanceIndex)
{
    if (!meshHandle.IsValid())
        return;

    auto* scene = renderServices.GetGPUScene();
    if (!scene)
        return;

    const auto* material = renderServices.Materials().Registry().Find(materialGuid);
    if (!material || !material->GetGraphicsPipelineId().IsValid())
        return;

    const auto* meshEntry = renderServices.GetMeshGPURegistry().Find(meshHandle);
    if (!meshEntry)
        return;

    const uint32 meshIndex = meshEntry->gpuMeshIndex;
    const uint32 materialIndex = material->GetGpuSceneMaterialIndex();
    if (meshIndex == 0xFFFFFFFFu || materialIndex == 0xFFFFFFFFu)
        return;

    Rendering::GPUInstance instance{};
    std::memcpy(instance.transform.Data(), worldTransform.matrix, sizeof(worldTransform.matrix));
    std::memcpy(instance.prevTransform.Data(), worldTransform.matrix, sizeof(worldTransform.matrix));
    bool mirrored = false;
    Rendering::Vector4 normalColumn0;
    Rendering::MatrixUtils::ComputeNormalMatrixColumns(
        worldTransform.matrix,
        normalColumn0,
        instance.normalMatrixCol1,
        instance.normalMatrixCol2,
        &mirrored);
    instance.normalMatrixCol0 = Rendering::Vector3(normalColumn0.x, normalColumn0.y, normalColumn0.z);
    // EZTree has a fixed layer-1 contract, shared by CPU submission and GPU culling.
    constexpr uint32 kRenderLayerMask = 1u;
    instance.renderLayerMask = kRenderLayerMask;
    instance.meshIndex = meshIndex;
    instance.materialIndex = materialIndex;
    const uint64 worldId = world.GetWorldId();
    const uint32 worldKey16 = Rendering::PackWorldKey16(worldId);
    // Keep the custom GPU-scene path in lockstep with the normal render
    // extraction path. Shadow scatter uses these depth-class bits to route
    // material-dependent casters (alpha test / vertex modifiers) and
    // double-sided casters to the matching shadow batch; the mirror bit splits
    // negative-determinant instances so their winding is compensated.
    instance.flags = 1u | 2u
        | Rendering::DepthClassInstanceFlagBits(
            renderServices.Materials().GetMaterialDepthClass(materialIndex))
        | (worldKey16 << 16);
    if (mirrored && Rendering::MirroredWindingEnabled())
        instance.flags |= Rendering::kInstanceFlagMirrored;
    instance.lodBias = 0.0f;
    const float* m = worldTransform.matrix;
    const auto& center = bounds.Box.center;
    instance.boundingCenter = Rendering::Vector3(
        m[0] * center.x + m[4] * center.y + m[8] * center.z + m[12],
        m[1] * center.x + m[5] * center.y + m[9] * center.z + m[13],
        m[2] * center.x + m[6] * center.y + m[10] * center.z + m[14]);
    const float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    instance.boundingRadius = (bounds.Box.Radius() + boundsInflation) * std::max({sx, sy, sz});

    if (instanceIndex == 0xFFFFFFFFu)
        instanceIndex = scene->AddInstance(instance);
    else
        scene->UpdateInstance(instanceIndex, instance);

    const auto& views = renderServices.Views().GetViews();
    if (views.empty())
        return;

    std::vector<Engine::Renderer::WorldSubmissionRecord> submissions;
    submissions.reserve(views.size());
    for (const auto& view : views)
    {
        if (view.worldId != 0 && view.worldId != world.GetWorldId())
            continue;
        if ((kRenderLayerMask & view.renderLayerMask) == 0u)
            continue;

        Engine::Renderer::WorldSubmissionRecord sub{};
        sub.viewId = view.id;
        sub.meshHandle = meshHandle;
        sub.material = material;
        sub.instanceIndex = instanceIndex;
        sub.renderLayerMask = kRenderLayerMask;
        sub.flags = instance.flags;
        submissions.push_back(sub);
    }
    if (!submissions.empty())
        renderServices.SubmitWorldSubmissions(submissions);
}

void ReleasePartInstances(Engine::Renderer::RenderServices& renderServices,
                          Components::EZTree& tree)
{
    auto* scene = renderServices.GetGPUScene();
    if (!scene)
        return;

    auto release = [scene](uint32& index)
    {
        if (index == 0xFFFFFFFFu)
            return;
        scene->RemoveInstance(index);
        index = 0xFFFFFFFFu;
    };

    release(tree.RuntimeBranchInstanceIndex);
    release(tree.RuntimeLeafInstanceIndex);
    release(tree.RuntimeTrellisInstanceIndex);
}

} // namespace

void EZTreeExtractionSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_RenderServices)
        return;

    auto* service = EZTreeService::TryGet();
    if (!service)
        return;

    // This system runs on a job worker: its wave holds several systems, and at
    // least one of them (MorphTarget) registers meshes too. The regen path below
    // registers four submeshes per tree and SubmitPart dereferences a Find()
    // pointer, so the exclusion has to span the whole pass, not each call.
    Rendering::MeshGPURegistry::TableScope registryScope(
        m_RenderServices->GetMeshGPURegistry());

    // Q6 slice 3b: an in-place device rebuild freed every submesh this system
    // registered, but the content-hash gate below still matches (the options and
    // the cached handle ids are unchanged), so it would never regenerate them —
    // a permanent dead handle. Detect the rebuild via the device generation and
    // force regen for every tree this Update, which re-registers the branch/leaf/
    // trellis submeshes from freshly regenerated geometry.
    bool forceRegenAll = false;
    if (auto* device = m_RenderServices->GetDevice())
    {
        const uint64 gen = device->GetDeviceRebuildGeneration();
        if (gen != m_LastDeviceRebuildGeneration)
        {
            m_LastDeviceRebuildGeneration = gen;
            forceRegenAll = (gen != 0u);
        }
    }

    // Every tree is visited whatever its enable state: one that is switched off,
    // or whose entity is, releases the GPU instances this system gave it.
    world.Query<ECS::Write<Components::EZTree>,
                ECS::Optional<ECS::Disabled>,
                ECS::Optional<ECS::DisabledInHierarchy>,
                ECS::Optional<ECS::ComponentDisabled<Components::EZTree>>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle entity, Components::EZTree& tree, const ECS::Disabled* disabled,
                  const ECS::DisabledInHierarchy* inactive, const ECS::ComponentDisabled<Components::EZTree>* treeOff)
        {
            if (world.GetComponent<Components::MeshRenderer>(entity))
                world.RemoveComponentImmediate<Components::MeshRenderer>(entity);
            if (world.GetComponent<Components::MeshGPUData>(entity))
                world.RemoveComponentImmediate<Components::MeshGPUData>(entity);

            if (disabled || inactive || treeOff)
            {
                ReleasePartInstances(*m_RenderServices, tree);
                return;
            }

            const uint64 hash = EZTree::HashOptions(tree.Options) ^ static_cast<uint64>(tree.RandomizeCounter);
            const bool needsRegen = forceRegenAll
                || tree.RuntimeOptionsHash != hash
                || tree.RuntimeMeshHandleId == 0u
                || tree.RuntimeBranchMeshHandleId == 0u;

            if (needsRegen)
            {
                const GUID oldGuid = GuidFromArray(tree.RuntimeMeshGuid);
                if (!oldGuid.IsNull())
                    service->ReleaseMesh(oldGuid);
                ReleasePartInstances(*m_RenderServices, tree);

                const GUID key = RuntimeGuidFromComponent(entity, hash);
                EZTree::GeneratedTree generated;
                const Rendering::MeshGPUHandle combinedHandle = service->RegenerateMesh(key, tree.Options, &generated);
                if (!combinedHandle.IsValid())
                    return;

                Rendering::MeshGPUHandle branchHandle{};
                Rendering::MeshGPUHandle leafHandle{};
                Rendering::MeshGPUHandle trellisHandle{};
                auto& meshRegistry = m_RenderServices->GetMeshGPURegistry();
                if (!generated.branches.Vertices.empty() && !generated.branches.Indices.empty())
                    branchHandle = meshRegistry.RegisterSubmesh(Rendering::MeshGPUKey{key, 1u}, generated.branches);
                if (!generated.leaves.Vertices.empty() && !generated.leaves.Indices.empty())
                    leafHandle = meshRegistry.RegisterSubmesh(Rendering::MeshGPUKey{key, 2u}, generated.leaves);
                if (!generated.trellis.Vertices.empty() && !generated.trellis.Indices.empty())
                    trellisHandle = meshRegistry.RegisterSubmesh(Rendering::MeshGPUKey{key, 3u}, generated.trellis);

                tree.RuntimeOptionsHash = hash;
                tree.RuntimeMaterialHash = 0;
                tree.RuntimeMeshHandleId = static_cast<uint64>(combinedHandle);
                tree.RuntimeBranchMeshHandleId = static_cast<uint64>(branchHandle);
                tree.RuntimeLeafMeshHandleId = static_cast<uint64>(leafHandle);
                tree.RuntimeTrellisMeshHandleId = static_cast<uint64>(trellisHandle);
                WriteGuidToArray(key, tree.RuntimeMeshGuid);
                tree.RuntimeVertexCount = static_cast<uint32>(generated.combined.Vertices.size());
                tree.RuntimeTriangleCount = static_cast<uint32>(generated.combined.Indices.size() / 3u);
                tree.RuntimeBranchCount = generated.stats.generatedBranchCount;

                Components::LocalBounds bounds{};
                bounds.Box = generated.bounds;
                world.AddComponentImmediate(entity, bounds);
            }

            const auto* worldTransform = world.GetComponent<Components::WorldTransform>(entity);
            const auto* bounds = world.GetComponent<Components::LocalBounds>(entity);
            if (!worldTransform || !bounds)
                return;

            Engine::Renderer::ResolvedWind worldWind = BuildBaseWindWorld(tree.Options, *worldTransform);
            if (tree.Options.wind.enabled)
            {
                worldWind = Engine::Renderer::WindVolumeResolver::ResolveAt(world,
                                                                              worldTransform->matrix[12],
                                                                              worldTransform->matrix[13],
                                                                              worldTransform->matrix[14],
                                                                              worldWind);
            }
            const Engine::Renderer::ResolvedWind localWind = WorldWindToLocal(worldWind, *worldTransform);
            const uint64 materialHash = hash;
            if (tree.RuntimeMaterialHash != materialHash || !TreeRuntimeMaterialsAvailable(*m_RenderServices, entity))
            {
                ResolveBarkMaterial(*m_RenderServices, entity, tree.Options, bounds, localWind);
                ResolveLeafMaterial(*m_RenderServices, entity, tree.Options, bounds, localWind);
                ResolveTrellisMaterial(*m_RenderServices, entity, tree.Options);
                tree.RuntimeMaterialHash = materialHash;
            }

            ApplyRuntimeWindToMaterial(*m_RenderServices,
                                       RuntimeMaterialGuidFromComponent(entity),
                                       tree.Options,
                                       bounds,
                                       1.0f,
                                       localWind);
            ApplyRuntimeWindToMaterial(*m_RenderServices,
                                       RuntimeLeafMaterialGuidFromComponent(entity),
                                       tree.Options,
                                       bounds,
                                       1.35f,
                                       localWind);

            const GUID barkMaterial = SelectBarkMaterial(*m_RenderServices, entity);
            const GUID leafMaterial = SelectLeafMaterial(*m_RenderServices, entity);
            const GUID trellisMaterial = SelectTrellisMaterial(entity);
            const float windBoundsInflation = EstimateWindBoundsInflation(tree.Options, localWind);

            SubmitPart(*m_RenderServices,
                       world,
                       entity,
                       *worldTransform,
                       *bounds,
                       Rendering::MeshGPUHandle(tree.RuntimeBranchMeshHandleId),
                       barkMaterial,
                       windBoundsInflation,
                       tree.RuntimeBranchInstanceIndex);
            SubmitPart(*m_RenderServices,
                       world,
                       entity,
                       *worldTransform,
                       *bounds,
                       Rendering::MeshGPUHandle(tree.RuntimeLeafMeshHandleId),
                       leafMaterial,
                       windBoundsInflation,
                       tree.RuntimeLeafInstanceIndex);
            SubmitPart(*m_RenderServices,
                       world,
                       entity,
                       *worldTransform,
                       *bounds,
                       Rendering::MeshGPUHandle(tree.RuntimeTrellisMeshHandleId),
                       trellisMaterial,
                       windBoundsInflation,
                       tree.RuntimeTrellisInstanceIndex);
        });
}

} // namespace GameEngine::EZTreeECS
