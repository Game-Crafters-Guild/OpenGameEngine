#include "Engine/Rendering/Systems/LensFlareExtractionSystem.h"

#include "Engine/Rendering/LensFlareRenderFeature.h"
#include "Engine/Rendering/RenderServices.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LensFlareSource.h"
#include "Components/Transform.h"

#include "Assets/AssetManager.h"
#include "Assets/FlareAtlasAsset.h"
#include "Assets/LensFlareDefinitionAsset.h"
#include "Assets/TextureAsset.h"
#include "Assets/Parsers/ParserExtractionHelpers.h" // LooksLikeGuid
#include "Core/Engine.h"

#include "ECS/World.h"

#include <cmath>
#include <vector>

// Extraction is VIEW-INDEPENDENT: it resolves each LensFlareSource's flare +
// atlas (and atlas texture) and the per-element atlas UVs once per frame, into a
// ResolvedFlare list on the feature. The render node projects + sizes + rotates
// these per view, so multi-view (Scene + Game / quad-split) is correct.
namespace GameEngine::Engine::Renderer
{

namespace
{
void MultiplyColumnMajor4x4(const float32* a, const float32* b, float32* out)
{
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            out[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

// A transform-tool preview updates the edited light's WorldTransform immediately,
// while TransformHierarchySystem updates its children's cached WorldTransforms on
// the next hierarchy pass. Compose an attached flare from the light's current
// world transform and the flare's local transform so it cannot visually detach
// while the light is moving. The local transform remains meaningful as an authored
// offset from the light.
const Components::WorldTransform& ResolveSourceWorldTransform(
    ECS::World& world, ECS::EntityHandle entity,
    const Components::WorldTransform& cachedWorld,
    Components::WorldTransform& attachedWorld)
{
    const auto* parent = world.GetComponent<Components::Parent>(entity);
    if (!parent || !parent->parent.IsValid() || !world.IsValid(parent->parent) ||
        !world.GetComponent<Components::Light>(parent->parent))
    {
        return cachedWorld;
    }

    const auto* parentWorld =
        world.GetComponent<Components::WorldTransform>(parent->parent);
    const auto* local = world.GetComponent<Components::Transform>(entity);
    if (!parentWorld || !local)
        return cachedWorld;

    attachedWorld = cachedWorld;
    MultiplyColumnMajor4x4(parentWorld->matrix, local->matrix, attachedWorld.matrix);
    return attachedWorld;
}

const Components::WorldTransform* FindDirectionalLightWorldTransform(
    ECS::World& world, ECS::EntityHandle entity)
{
    const auto findOnEntity = [&](ECS::EntityHandle candidate)
        -> const Components::WorldTransform*
    {
        const auto* light = world.GetComponent<Components::Light>(candidate);
        if (!light || light->Type != Components::LightType::Directional)
            return nullptr;
        return world.GetComponent<Components::WorldTransform>(candidate);
    };

    if (const auto* lightWorld = findOnEntity(entity))
        return lightWorld;

    const auto* parent = world.GetComponent<Components::Parent>(entity);
    if (!parent || !parent->parent.IsValid() || !world.IsValid(parent->parent))
        return nullptr;
    return findOnEntity(parent->parent);
}

void ApplyDirectionalLightDirection(ResolvedFlare& flare,
                                    const Components::WorldTransform& lightWorld)
{
    // Toward-sun = -shine = -col2. SunMode projects this as an infinite source
    // so the flare sits on the sun disk, not along the shine vector.
    const float fx = -lightWorld.matrix[8];
    const float fy = -lightWorld.matrix[9];
    const float fz = -lightWorld.matrix[10];
    const float length = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (length <= 1e-6f)
        return;

    flare.Forward[0] = fx / length;
    flare.Forward[1] = fy / length;
    flare.Forward[2] = fz / length;
}

// Resolve a guid-or-mount-relative-path reference string to a GUID.
GUID ResolveRef(::GameEngine::AssetManager& assets, const std::string& ref)
{
    if (ref.empty())
        return GUID::Null();
    if (ParserExtraction::LooksLikeGuid(ref))
        return GUID(ref.c_str());
    return assets.ResolveAssetGuid(std::filesystem::path(ref));
}

template <class T>
std::shared_ptr<T> LoadTyped(::GameEngine::AssetManager& assets, const GUID& guid)
{
    if (guid.IsNull())
        return nullptr;
    SharedPtr<Asset> asset = assets.GetAsset(guid);
    if (!asset)
        asset = assets.LoadAssetAsync(guid).get();
    return std::dynamic_pointer_cast<T>(asset);
}

ResolvedFlareElement MakeFallbackElement(float scale, float position, float brightness,
                                         float r, float g, float b, float a,
                                         float anamorphicX = 0.0f,
                                         float anamorphicY = 0.0f)
{
    ResolvedFlareElement element;
    element.Tint[0] = r;
    element.Tint[1] = g;
    element.Tint[2] = b;
    element.Tint[3] = a;
    element.Brightness = brightness;
    element.Scale = scale;
    element.Position = position;
    element.AnamorphicX = anamorphicX;
    element.AnamorphicY = anamorphicY;
    element.RotateToFlare = position != 0.0f;
    return element;
}

void ApplySourceTransform(ResolvedFlare& rf, const Components::LensFlareSource& src,
                          const Components::WorldTransform& wt)
{
    rf.WorldPos[0] = wt.matrix[12];
    rf.WorldPos[1] = wt.matrix[13];
    rf.WorldPos[2] = wt.matrix[14];

    // World forward = Z basis column (LH, Z+ forward), normalized; its length
    // (uniform-scale assumption) doubles as the entity's transform scale.
    const float fx = wt.matrix[8];
    const float fy = wt.matrix[9];
    const float fz = wt.matrix[10];
    const float fLen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fLen > 1e-6f)
    {
        rf.Forward[0] = fx / fLen;
        rf.Forward[1] = fy / fLen;
        rf.Forward[2] = fz / fLen;
    }
    const float sx = wt.matrix[0];
    const float sy = wt.matrix[1];
    const float sz = wt.matrix[2];
    rf.TransformScale = std::sqrt(sx * sx + sy * sy + sz * sz);

    rf.Intensity = src.Intensity;
    rf.Scale = src.Scale;
    rf.Tint[0] = src.Tint.r;
    rf.Tint[1] = src.Tint.g;
    rf.Tint[2] = src.Tint.b;
    rf.Tint[3] = src.Tint.a;
    rf.MaxDistanceOverride = src.MaxDistanceOverride;
    rf.Occlude = src.Occlude;
}

ResolvedFlare MakeFallbackFlare(::GameEngine::Rendering::TextureHandle atlas,
                                const Components::LensFlareSource& src,
                                const Components::WorldTransform& wt)
{
    ResolvedFlare rf;
    rf.Atlas = atlas;
    ApplySourceTransform(rf, src, wt);

    rf.Globals.GlobalScale = 32.0f;
    rf.Globals.GlobalBrightness = 0.85f;
    rf.Globals.GlobalTint = {1.0f, 0.94f, 0.78f, 1.0f};
    rf.Globals.OffScreenFadeDist = 0.45f;
    rf.Globals.UseDynamicEdgeScale = true;
    rf.Globals.DynamicEdgeScale = 0.65f;
    rf.Globals.DynamicEdgeScaleRange = 0.35f;
    rf.Globals.UseDynamicCenterBoost = true;
    rf.Globals.DynamicCenterBrightness = 0.12f;
    rf.Globals.DynamicCenterRange = 0.55f;

    rf.Elements.push_back(MakeFallbackElement(0.55f, 0.0f, 0.85f, 1.0f, 0.88f, 0.55f, 0.55f));
    rf.Elements.push_back(MakeFallbackElement(1.15f, 0.0f, 0.12f, 1.0f, 0.72f, 0.36f, 0.45f,
                                              5.0f, -0.78f));
    rf.Elements.push_back(MakeFallbackElement(0.42f, 0.34f, 0.36f, 0.45f, 0.9f, 1.0f, 0.45f));
    rf.Elements.push_back(MakeFallbackElement(0.28f, -0.52f, 0.30f, 1.0f, 0.48f, 0.38f, 0.42f));
    rf.Elements.push_back(MakeFallbackElement(0.22f, 0.78f, 0.24f, 0.52f, 1.0f, 0.58f, 0.38f));
    return rf;
}

bool TryResolveAssetFlare(::GameEngine::AssetManager& assets, RenderServices& renderServices,
                          const Components::LensFlareSource& src,
                          const Components::WorldTransform& wt, ResolvedFlare& outFlare)
{
    auto flare = LoadTyped<LensFlareDefinitionAsset>(assets, src.Flare.Guid);
    if (!flare || !flare->IsLoaded())
        return false;

    auto atlas = LoadTyped<FlareAtlasAsset>(assets, ResolveRef(assets, flare->GetAtlasRef()));
    if (!atlas || !atlas->IsLoaded())
        return false;

    const GUID textureGuid = ResolveRef(assets, atlas->GetTextureRef());
    auto textureAsset = LoadTyped<TextureAsset>(assets, textureGuid);
    if (!textureAsset || !textureAsset->IsLoaded())
        return false;

    const TextureHandle tex = renderServices.Textures().GetOrUpload(textureGuid);
    if (!tex.IsValid())
        return false;

    ResolvedFlare rf;
    rf.Atlas = tex;
    rf.AtlasHasAuthoredAlpha = textureAsset->GetChannels() >= 4;
    ApplySourceTransform(rf, src, wt);
    rf.Globals = flare->GetGlobals();

    // Authored curves -> evaluable Hermite keys (stored slopes, so the segment
    // reproduces the source tool's spline; Broken keeps in/out independent).
    const auto convertCurve = [](const std::vector<LensFlare::CurveKeyData>& in,
                                 std::vector<Math::CurveKey>& out)
    {
        out.reserve(in.size());
        for (const LensFlare::CurveKeyData& k : in)
        {
            Math::CurveKey key;
            key.Time = k.Time;
            key.Value = k.Value;
            key.InTangent = k.InTangent;
            key.OutTangent = k.OutTangent;
            key.Interp = Math::CurveInterp::Smooth;
            key.TangentMode = Math::CurveTangentMode::Broken;
            out.push_back(key);
        }
    };
    convertCurve(rf.Globals.AngleCurveKeys, rf.AngleCurve);
    convertCurve(rf.Globals.DynamicEdgeCurveKeys, rf.DynamicEdgeCurve);

    for (const LensFlare::FlareElement& e : flare->GetElements())
    {
        if (!e.Visible)
            continue;
        const int32_t spriteIdx = atlas->FindSprite(e.SpriteName);
        if (spriteIdx < 0)
            continue;
        const LensFlare::AtlasSprite& sprite = atlas->GetSprites()[spriteIdx];

        ResolvedFlareElement re;
        re.UVRect[0] = sprite.U;
        re.UVRect[1] = sprite.V;
        re.UVRect[2] = sprite.W;
        re.UVRect[3] = sprite.H;
        re.Tint[0] = e.Tint.R;
        re.Tint[1] = e.Tint.G;
        re.Tint[2] = e.Tint.B;
        re.Tint[3] = e.Tint.A;
        re.Brightness = e.Brightness;
        re.Scale = e.Scale;
        re.SizeX = e.SizeX;
        re.SizeY = e.SizeY;
        re.Position = e.Position;
        re.OffsetX = e.OffsetX;
        re.OffsetY = e.OffsetY;
        re.AnamorphicX = e.AnamorphicX;
        re.AnamorphicY = e.AnamorphicY;
        re.Angle = e.Angle;
        re.RotationSpeed = e.RotationSpeed;
        re.EdgeBrightnessBoost = e.EdgeBrightnessBoost;
        re.CenterBrightnessBoost = e.CenterBrightnessBoost;
        re.EdgeScaleBoost = e.EdgeScaleBoost;
        re.CenterScaleBoost = e.CenterScaleBoost;
        re.UseStarRotation = e.UseStarRotation;
        re.RotateToFlare = e.RotateToFlare;
        rf.Elements.push_back(re);
    }

    if (rf.Elements.empty())
        return false;

    outFlare = std::move(rf);
    return true;
}

} // namespace

LensFlareExtractionSystem::LensFlareExtractionSystem(RenderServices* renderServices)
    : m_RenderServices(renderServices)
{
}

void LensFlareExtractionSystem::Update(ECS::World& world, float32 deltaTime)
{
    m_Time += deltaTime;
    if (!m_RenderServices)
        return;

    // The feature's GPU pipeline is initialized lazily by the render node (it needs
    // the device at declare time). We still build + store CPU frame data here every
    // frame; the node projects + uploads + draws per view once the pipeline exists.
    auto& feature = m_RenderServices->EnsureFeature<LensFlareRenderFeature>();
    feature.SetTime(m_Time);

    auto& assets = EngineCore::GetInstance().GetAssetManager();

    std::vector<ResolvedFlare> flares;

    world.Query<ECS::Read<Components::LensFlareSource>, ECS::Read<Components::WorldTransform>>()
        .Each(
            [&](ECS::EntityHandle entity, const Components::LensFlareSource& src,
                const Components::WorldTransform& wt)
            {
                Components::WorldTransform attachedWorld;
                const Components::WorldTransform& sourceWorld =
                    ResolveSourceWorldTransform(world, entity, wt, attachedWorld);
                const Components::WorldTransform* directionalLightWorld =
                    src.SunMode ? FindDirectionalLightWorldTransform(world, entity) : nullptr;

                ResolvedFlare rf;
                if (!TryResolveAssetFlare(assets, *m_RenderServices, src, sourceWorld, rf))
                {
                    const TextureHandle fallbackAtlas = m_RenderServices->Textures().GetDefaultLensFlareTexture();
                    if (!fallbackAtlas.IsValid())
                        return;
                    rf = MakeFallbackFlare(fallbackAtlas, src, sourceWorld);
                }

                rf.SunMode = directionalLightWorld != nullptr;
                if (directionalLightWorld)
                    ApplyDirectionalLightDirection(rf, *directionalLightWorld);

                flares.push_back(std::move(rf));
            });

    feature.SetFlares(std::move(flares));
}

} // namespace GameEngine::Engine::Renderer
