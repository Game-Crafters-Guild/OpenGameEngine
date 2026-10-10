#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Terrain/TerrainPlanetRelief.h"

#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainECS/TerrainHandleHygiene.h"
#include "ECS/ECSTemplates.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainService.h"
#include "Terrain/TerrainMaterialRecord.h" // kMaxTerrainMaterials — the slot ID domain
#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "AssetCore/GUID.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <limits>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace GameEngine::Scene
{

// ---------------------------------------------------------------------------
// Pre-volume modifier blocks — the retired FILE FORMAT, not components
//
// The five pre-volume modifier components are gone from the engine. Their
// property blocks are not: scene files on disk still carry them, and a scene
// that silently dropped them would lose authored terrain rather than migrate
// it. So the names stay parseable, and these are what a block parses INTO on
// the way to a volume plus an effect. No scene in this repo carries them; the
// on-disk format is pinned by a fixture that does
// (Engine/Tests/Fixtures/TerrainLegacyModifierScene/LegacyTerrainModifiers.scene,
// loaded by EngineSceneIOTests).
//
// They are ECS components only because the schema interface accumulates a block
// one property at a time, and the entity is the natural place to keep a
// half-parsed block between calls — which is exactly what SceneIO's degraded
// load does when one bad value sends it down the per-property path. Nothing
// reads them: no reflection, no ComponentFactory entry, no gather, no gizmo, no
// inspector section, no save. A leftover from a degraded load is inert.
// ---------------------------------------------------------------------------
// Every block keeps its own Enabled field (ECS::ComponentFlags::KeepsOwnEnabledField): the migration reads it
// onto the effect it becomes, so the loader's generic switch line leaves it to the schema.
namespace LegacyModifierBlocks
{

#define GE_LEGACY_MODIFIER_COMMON_FIELDS                                            Components::TerrainModifierShape Shape = Components::TerrainModifierShape::Circle;     float32 Radius = 50.0f;                                                         float32 RectHalfX = 50.0f;                                                      float32 RectHalfZ = 50.0f;                                                      float32 Falloff = 10.0f;                                                        float32 Priority = 0.0f;                                                        bool Enabled = true; static constexpr bool KeepsOwnEnabledField = true;                                                            uint8 _ModPad[3] = {};

struct TerrainFlattenModifier
{
    GE_LEGACY_MODIFIER_COMMON_FIELDS

    // Ignored when UseEntityHeight is set — the migration maps that to a zero
    // offset from the volume's reference height, never the stale value.
    float32 TargetHeight = 0.0f;
    bool UseEntityHeight = true;
    uint8 _Pad0[3] = {};
};

struct TerrainNoiseModifier
{
    GE_LEGACY_MODIFIER_COMMON_FIELDS

    Components::TerrainModifierBlend Blend = Components::TerrainModifierBlend::Add;
    uint8 _BlendPad[3] = {};
    float32 Frequency = 8.0f;
    float32 Amplitude = 5.0f;
    uint32 Octaves = 4;
    uint32 Seed = 0;
    float32 Lacunarity = 2.0f;
    float32 Persistence = 0.5f;
    float32 BlendSmoothing = Components::kDefaultBlendSmoothingM;
};

struct TerrainStampModifier
{
    GE_LEGACY_MODIFIER_COMMON_FIELDS

    Components::TerrainModifierBlend Blend = Components::TerrainModifierBlend::Add;
    uint8 _BlendPad[3] = {};
    float32 HeightScale = 10.0f;
    float32 Rotation = 0.0f;
    Components::TextureRef StampAssetGuid;
    float32 BlendSmoothing = Components::kDefaultBlendSmoothingM;
};

struct TerrainPaintLayerModifier
{
    GE_LEGACY_MODIFIER_COMMON_FIELDS

    uint32 LayerIndex = 0;
    float32 Strength = 1.0f;
    bool Replace = false;
    uint8 _Pad0[3] = {};
};

// Carries no shape fields: its region has always been the entity's spline.
struct TerrainSplineModifier
{
    Components::TerrainModifierBlend Blend = Components::TerrainModifierBlend::Add;
    uint8 _BlendPad[3] = {};
    float32 Falloff = 5.0f;
    float32 Priority = 0.0f;
    float32 HeightOffset = 0.0f;
    bool Flatten = true;
    bool Enabled = true;
    static constexpr bool KeepsOwnEnabledField = true;
    uint8 _Pad0[2] = {};
    bool PaintLayer = false;
    uint8 _PaintPad[3] = {};
    uint32 PaintLayerIndex = 2;
    float32 PaintStrength = 0.8f;
};

#undef GE_LEGACY_MODIFIER_COMMON_FIELDS

// These are ECS components (the degraded-load path parks one on an entity), so
// they carry the same POD contract they were declared with as public components.
static_assert(std::is_trivially_copyable_v<TerrainFlattenModifier>);
static_assert(std::is_standard_layout_v<TerrainFlattenModifier>);
static_assert(std::is_trivially_copyable_v<TerrainNoiseModifier>);
static_assert(std::is_standard_layout_v<TerrainNoiseModifier>);
static_assert(std::is_trivially_copyable_v<TerrainStampModifier>);
static_assert(std::is_standard_layout_v<TerrainStampModifier>);
static_assert(std::is_trivially_copyable_v<TerrainPaintLayerModifier>);
static_assert(std::is_standard_layout_v<TerrainPaintLayerModifier>);
static_assert(std::is_trivially_copyable_v<TerrainSplineModifier>);
static_assert(std::is_standard_layout_v<TerrainSplineModifier>);

} // namespace LegacyModifierBlocks

} // namespace GameEngine::Scene

namespace GameEngine::ECS
{
// World's component templates are defined out-of-line, so even a type used by a
// single TU needs its instantiation. See ECS/ECSTemplates.h.
GE_INSTANTIATE_ENGINE_COMPONENT(Scene::LegacyModifierBlocks::TerrainFlattenModifier);
GE_INSTANTIATE_ENGINE_COMPONENT(Scene::LegacyModifierBlocks::TerrainNoiseModifier);
GE_INSTANTIATE_ENGINE_COMPONENT(Scene::LegacyModifierBlocks::TerrainStampModifier);
GE_INSTANTIATE_ENGINE_COMPONENT(Scene::LegacyModifierBlocks::TerrainPaintLayerModifier);
GE_INSTANTIATE_ENGINE_COMPONENT(Scene::LegacyModifierBlocks::TerrainSplineModifier);
} // namespace GameEngine::ECS

namespace GameEngine::Scene
{

namespace
{

static std::string FormatFloat(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

static bool ParseF32(std::string_view value, float32& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Float)
    {
        out = static_cast<float32>(v.FloatValue);
        return true;
    }
    if (v.Kind == SceneValueKind::Int)
    {
        out = static_cast<float32>(v.IntValue);
        return true;
    }
    if (outError)
        *outError = "Expected a numeric value";
    return false;
}

// The channel-role -> library-slot binding key, under the current name and the one it
// shipped under. Returns the role index, or 4 when `property` is neither.
//
// Written as one matcher rather than two parse branches so the alias cannot drift from
// the key it aliases: a third name, or a fifth role, is one edit here.
static constexpr uint32 kNoRoleSlotKey = 4u;
static uint32 MatchRoleSlotKey(std::string_view property)
{
    // `property` arrives lowercased.
    constexpr std::string_view kNames[] = {"layerrole", "classifierrole"};
    for (const std::string_view name : kNames)
    {
        if (property.size() != name.size() + 1 || !property.starts_with(name))
            continue;
        const char digit = property[name.size()];
        if (digit >= '0' && digit <= '3')
            return static_cast<uint32>(digit - '0');
    }
    return kNoRoleSlotKey;
}

static bool ParseU32(std::string_view value, uint32& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Int)
    {
        if (v.IntValue < 0 || v.IntValue > 0xFFFFFFFFll)
        {
            if (outError)
                *outError = "Value out of range for uint32";
            return false;
        }
        out = static_cast<uint32>(v.IntValue);
        return true;
    }
    if (v.Kind == SceneValueKind::Float)
    {
        out = static_cast<uint32>(v.FloatValue);
        return true;
    }
    if (outError)
        *outError = "Expected an integer value";
    return false;
}

// Scenes authored before the retired Smooth blend was removed carry blend = 3. Smooth never
// had its own dispatch — every CPU and GPU blend switch let it fall through to Add — so the
// faithful migration is Add, and the baked output is unchanged.
constexpr uint32 kRetiredSmoothBlend = 3;

// Once per scene file rather than once per modifier: a road network can hold dozens of
// legacy modifiers and one report is the whole story. A load with no scene file (a tool or
// test applying a single component block) has no file to key on, so it always reports.
static void WarnRetiredSmoothBlend(const SceneLoadContext& ctx)
{
    if (ctx.SceneFile != nullptr)
    {
        static std::mutex mutex;
        static std::unordered_set<std::string> warned;
        std::lock_guard<std::mutex> lock(mutex);
        if (!warned.emplace(ctx.SceneFile->string()).second)
            return;
    }
    if (ctx.SceneFile != nullptr)
    {
        Logger::Log::Warning(
            "Scene '{}': terrain blend 3 is the retired Smooth mode, which always baked as "
            "Add — loading it as Add. Re-save the scene to drop it.",
            ctx.SceneFile->string());
        return;
    }
    Logger::Log::Warning(
        "Scene: terrain blend 3 is the retired Smooth mode, which always baked as Add — "
        "loading it as Add. Re-save the scene to drop it.");
}

// Which blend modes a given component may author. Average is POOLING — an
// accumulate-across-members-then-apply-once operator — so it is only meaningful
// where a pool accumulator exists: the four HEIGHT effects (flatten, height
// offset, noise, stamp). Every other blend switch in the engine (the sculpt
// zone's, the sphere fill, the GPU kernel) is a per-texel function of
// (current, target) with a fall-through arm, so an Average that reached one
// would silently bake as Add. Refused at this boundary instead, where the scene
// can be told why.
enum class BlendSet
{
    HeightOperators,        // Set / Add / Subtract / Min / Max / SmoothMin / SmoothMax
    HeightOperatorsAndPool, // ... and Average
};

// Parses a terrain modifier/effect `blend` property. Shared by every terrain schema so the
// accepted range and the legacy migration cannot drift apart between them.
static bool ParseBlend(const SceneLoadContext& ctx, std::string_view value, BlendSet set,
                       Components::TerrainModifierBlend& out, std::string* outError)
{
    uint32 v = 0;
    if (!ParseU32(value, v, outError))
        return false;
    if (v == kRetiredSmoothBlend)
    {
        WarnRetiredSmoothBlend(ctx);
        v = static_cast<uint32>(Components::TerrainModifierBlend::Add);
    }
    if (v == static_cast<uint32>(Components::TerrainModifierBlend::Average)
        && set != BlendSet::HeightOperatorsAndPool)
    {
        if (outError)
            *outError = "blend 8 (Average) pools an effect's value with the other members of "
                        "its pool and applies the average once; only the height effects "
                        "(Flatten, Height Offset, Noise, Stamp) have a pool. Use 0-2 or 4-7 here.";
        return false;
    }
    // 3 is migrated above and every other value up to Average is a live mode, so the
    // accepted set is contiguous apart from the tombstone.
    if (v > static_cast<uint32>(Components::TerrainModifierBlend::Average))
    {
        if (outError)
            *outError = "blend must be 0-2, 4-7 or 8 (3 is the retired Smooth)";
        return false;
    }
    out = static_cast<Components::TerrainModifierBlend>(v);
    return true;
}

static bool ParseBool(std::string_view value, bool& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind != SceneValueKind::Bool)
    {
        if (outError)
            *outError = "Expected true/false";
        return false;
    }
    out = v.BoolValue;
    return true;
}

// Parses a Terrain domain from a scene value: the reflected enum NAME (Planar /
// Spherical — how old scanner-serialized TerrainCBT blocks wrote it) or a bare
// integer (0 / 1). Case-insensitive, quote-tolerant.
static bool ParseDomain(std::string_view value, Components::TerrainDomain& out, std::string* outError)
{
    std::string v(value);
    // Trim whitespace and surrounding quotes, then lower-case.
    auto isTrim = [](char ch) { return ch == ' ' || ch == '\t' || ch == '"' || ch == '\''; };
    while (!v.empty() && isTrim(v.front())) v.erase(v.begin());
    while (!v.empty() && isTrim(v.back())) v.pop_back();
    for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    if (v == "spherical" || v == "1") { out = Components::TerrainDomain::Spherical; return true; }
    if (v == "planar" || v == "0") { out = Components::TerrainDomain::Planar; return true; }
    if (outError) *outError = "domain must be planar/spherical or 0/1";
    return false;
}

// Fetches (or default-constructs) the entity's TerrainPlanetRelief, applies one relief
// field, and writes it back. This is the single-entity migration seam: a legacy
// Terrain.planetRelief* / TerrainCBT.PlanetRelief* key folds into the companion
// component here (same-entity, values exact), and the new TerrainPlanetRelief block
// parses through the same path.
static bool ApplyPlanetReliefField(ECS::World& world, ECS::EntityHandle entity,
                                   std::string_view field, std::string_view value,
                                   std::string* outError)
{
    Components::TerrainPlanetRelief relief{};
    if (const auto* existing = world.GetComponent<Components::TerrainPlanetRelief>(entity))
        relief = *existing;

    if (field == "amplitude")
    {
        if (!ParseF32(value, relief.Amplitude, outError)) return false;
    }
    else if (field == "frequency")
    {
        if (!ParseF32(value, relief.Frequency, outError)) return false;
    }
    else if (field == "octaves")
    {
        if (!ParseU32(value, relief.Octaves, outError)) return false;
    }
    else
    {
        if (outError) *outError = "Unknown TerrainPlanetRelief field";
        return false;
    }

    world.AddComponentImmediate(entity, relief);
    return true;
}

// Warns once per process per retired Terrain property. A hand-written schema would
// otherwise hard-fail the whole scene load on the unrecognized key, so these are
// parsed and ignored for one release beat; the warning is the "beat" marker.
static void WarnRetiredTerrainProperty(std::string_view property)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> warned;
    std::lock_guard<std::mutex> lock(mutex);
    if (warned.emplace(std::string(property)).second)
        Logger::Log::Warning(
            "Scene: Terrain.{} is retired (CDLOD-era quadtree tuning is now a service "
            "constant) — ignoring. Re-save the scene to drop it.", property);
}

// Warns once per process per retired legacy Terrain.grass* property. Same one-release-beat
// treatment as the retired quadtree knobs: recognized and dropped so an old scene still loads and
// saves clean. No value migration — a retired knob is not a current one under another name.
//
// The reason is per key rather than one fixed sentence: these aliases retire alongside the
// TerrainGrass properties they mirror, in unrelated groups, and one sentence would misdescribe
// whichever group it was not written for.
static void WarnRetiredGrassProperty(std::string_view property, std::string_view reason)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> warned;
    std::lock_guard<std::mutex> lock(mutex);
    if (warned.emplace(std::string(property)).second)
        Logger::Log::Warning("Scene: Terrain.{} is retired ({}) — ignoring. Re-save the scene to "
                             "drop it.", property, reason);
}

constexpr std::string_view kRetiredGrassDensityReason =
    "grass density is now blades/m2 at the camera and the fade band is gone";
constexpr std::string_view kRetiredGrassShadowStrengthReason =
    "renamed to TerrainGrass.groundingStrength, which is what it gates - the grounding read, "
    "never cast shadows";
constexpr std::string_view kRetiredGrassCurvatureReason =
    "blade bend is authored by the wind amplitudes it multiplied (windRestingLean, "
    "windStrength, windFlutterAmount)";
constexpr std::string_view kRetiredGrassScaleReason =
    "blade size is authored in metres by bladeHeight and bladeWidth; a second multiplier over them "
    "made the component report a size it did not render";

// ---------------------------------------------------------------------------
// TerrainSchema
// ---------------------------------------------------------------------------

class TerrainSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "Terrain"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::Terrain>(entity);
        if (!c)
            return;

        if (!c->TerrainAssetGuid.IsNull())
        {
            const GUID g = c->TerrainAssetGuid.ToGuid();
            outLines.push_back(std::string("Terrain.terrainAsset = \"") + g.ToString() + "\"");
        }
        outLines.push_back("Terrain.terrainAssetType = " + std::to_string(c->TerrainAssetType));
        outLines.push_back("Terrain.baseSource = " + std::to_string(static_cast<uint32>(c->BaseSource)));
        outLines.push_back("Terrain.sizeX = " + FormatFloat(c->SizeX));
        outLines.push_back("Terrain.sizeZ = " + FormatFloat(c->SizeZ));
        outLines.push_back("Terrain.heightScale = " + FormatFloat(c->HeightScale));
        outLines.push_back("Terrain.samplesPerMeter = " + FormatFloat(c->SamplesPerMeter));
        if (c->StreamingRadius > 0.0f)
            outLines.push_back("Terrain.streamingRadius = " + FormatFloat(c->StreamingRadius));

        // Domain + planet radius: only emit for a spherical terrain so planar scenes stay
        // uncluttered. The base relief lives in its own TerrainPlanetRelief block now
        // (TerrainPlanetReliefSchema), not here.
        outLines.push_back("Terrain.domain = " + std::to_string(static_cast<uint32>(c->Domain)));
        if (c->Domain == Components::TerrainDomain::Spherical)
        {
            outLines.push_back("Terrain.planetRadius = " + FormatFloat(c->PlanetRadius));
            // The sphere sculpt payload ref (.tsculpt sidecar) — the planet analogue of a
            // zone's payload line. The texels live in the sidecar; only the ref serializes.
            if (!c->SphereSculptGuid.IsNull())
                outLines.push_back(std::string("Terrain.sphereSculpt = \"")
                                   + c->SphereSculptGuid.ToGuid().ToString() + "\"");
        }

        // Advanced: the CBT screen-space split target. MaxDepth is auto-derived (not a
        // user field), and MaxDepthOverride and DebugView are debug-only runtime knobs —
        // none of the three serializes (DebugView is still settable, see ApplyProperty).
        outLines.push_back("Terrain.targetPixelError = " + FormatFloat(c->TargetPixelError));
        // The authored water surface height. Only a set level serializes: the unset sentinel is
        // the lowest float, which would round-trip through the 6-digit formatter as a different
        // (equally inert) value and clutter every planar scene.
        if (c->SeaLevel > Components::kNoTerrainSeaLevel)
            outLines.push_back("Terrain.seaLevel = " + FormatFloat(c->SeaLevel));
        outLines.push_back("Terrain.materialTiling = " + FormatFloat(c->MaterialTiling));
        // The material library ref. The materials themselves live in the .terrainmatlib;
        // only the reference belongs in the scene, so two terrains can share one library.
        if (!c->MaterialLibraryGuid.IsNull())
            outLines.push_back(std::string("Terrain.materialLibrary = \"")
                               + c->MaterialLibraryGuid.ToGuid().ToString() + "\"");
        // Which library slot each channel role binds. Only a rebound role serializes: the
        // identity default is what an unmigrated terrain carries, so its block stays as it was.
        for (uint32 i = 0; i < 4; ++i)
        {
            if (c->LayerRoleSlot[i] != static_cast<uint8>(i))
                outLines.push_back("Terrain.layerRole" + std::to_string(i) + " = "
                                   + std::to_string(static_cast<uint32>(c->LayerRoleSlot[i])));
        }
        // Per-layer material authoring (#616 plumbing). Only non-default lines serialize so
        // an untextured terrain's scene block stays exactly as before.
        for (uint32 i = 0; i < 4; ++i)
        {
            const std::string idx = std::to_string(i);
            if (!c->LayerAlbedoTexture[i].IsNull())
                outLines.push_back("Terrain.layerAlbedo" + idx + " = \""
                                   + c->LayerAlbedoTexture[i].ToGuid().ToString() + "\"");
            if (c->LayerTiling[i] != 1.0f)
                outLines.push_back("Terrain.layerTiling" + idx + " = " + FormatFloat(c->LayerTiling[i]));
            if ((c->LayerHexTiling >> i) & 1u)
                outLines.push_back("Terrain.layerHex" + idx + " = true");
        }
        outLines.push_back("Terrain.renderLayerMask = " + std::to_string(c->RenderLayerMask));
        outLines.push_back(std::string("Terrain.castShadows = ") + (c->CastShadows ? "true" : "false"));
        outLines.push_back(std::string("Terrain.receiveShadows = ") + (c->ReceiveShadows ? "true" : "false"));
        // GenerateCollision and CollisionLODBias removed; physics collision is
        // controlled by HeightFieldColliderShape component presence.
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::Terrain c{};
        if (const auto* existing = world.GetComponent<Components::Terrain>(entity))
            c = *existing;

        // Runtime handles: keep the ones that still resolve in the live
        // TerrainService, zero the rest. A resolving handle means this apply is
        // an edit of a provisioned terrain (the debug-server set_component path
        // drives this schema via ApplyComponentViaSchema); preserving it routes
        // the edit through the extraction system's in-place debounced
        // re-provision — the same path an inspector commit walks — instead of
        // orphaning the old TerrainData/TiledTerrainData slot and recreating
        // from scratch.
        TerrainECS::ClearUnresolvedTerrainHandles(c);
        // A load from a scene file names the terrain's baked-terrain cache identity; an
        // edit through this schema outside a load keeps the one it has.
        if (ctx.SceneFile != nullptr && ctx.Resolver != nullptr)
            c.BakeOriginScene = ctx.Resolver->GetOrCreateAssetGuid(*ctx.SceneFile);

        if (property == "terrainasset")
        {
            SceneValue sv{};
            if (!ParseValue(value, sv, outError)) return false;
            if (sv.Kind == SceneValueKind::Int && sv.IntValue == 0)
            {
                c.TerrainAssetGuid.Clear();
                c.TerrainAssetType = 0;
            }
            else if (sv.Kind == SceneValueKind::GuidRef || sv.Kind == SceneValueKind::String)
            {
                // A scene file quotes the GUID (String); set_component hands the schema the bare
                // token (GuidRef). Malformed text must FAIL rather than Set: GUID's string
                // constructor yields a NULL guid for it, which would silently unbind the heightmap.
                if (sv.StringValue.empty())
                {
                    c.TerrainAssetGuid.Clear();
                    c.TerrainAssetType = 0;
                }
                else if (IsGuidText(sv.StringValue))
                    c.TerrainAssetGuid.Set(GUID(sv.StringValue));
                else
                {
                    if (outError) *outError = "terrainAsset takes a GUID as 8-4-4-4-12 hex digits, or \"\" or 0 to clear it";
                    return false;
                }
            }
            else
            {
                if (outError) *outError = "terrainAsset takes a GUID as 8-4-4-4-12 hex digits, or \"\" or 0 to clear it";
                return false;
            }
        }
        else if (property == "terrainassettype")
        {
            if (!ParseU32(value, c.TerrainAssetType, outError)) return false;
        }
        else if (property == "basesource")
        {
            uint32 v = 0;
            if (!ParseU32(value, v, outError)) return false;
            if (v > static_cast<uint32>(Components::TerrainBaseSource::Flat))
            {
                if (outError) *outError = "baseSource must be 0-2";
                return false;
            }
            c.BaseSource = static_cast<Components::TerrainBaseSource>(v);
        }
        else if (property == "sizex")
        {
            if (!ParseF32(value, c.SizeX, outError)) return false;
        }
        else if (property == "sizez")
        {
            if (!ParseF32(value, c.SizeZ, outError)) return false;
        }
        else if (property == "heightscale")
        {
            if (!ParseF32(value, c.HeightScale, outError)) return false;
        }
        else if (property == "samplespermeter")
        {
            if (!ParseF32(value, c.SamplesPerMeter, outError)) return false;
        }
        else if (property == "domain")
        {
            uint32 v = 0;
            if (!ParseU32(value, v, outError)) return false;
            if (v > static_cast<uint32>(Components::TerrainDomain::Spherical))
            {
                if (outError) *outError = "domain must be 0 (planar) or 1 (spherical)";
                return false;
            }
            c.Domain = static_cast<Components::TerrainDomain>(v);
        }
        else if (property == "planetradius")
        {
            if (!ParseF32(value, c.PlanetRadius, outError)) return false;
        }
        else if (property == "spheresculpt")
        {
            SceneValue sv{};
            if (!ParseValue(value, sv, outError)) return false;
            if (sv.Kind == SceneValueKind::Int && sv.IntValue == 0)
            {
                c.SphereSculptGuid.Clear();
            }
            else if (sv.Kind == SceneValueKind::String)
            {
                try
                {
                    GUID g(sv.StringValue);
                    c.SphereSculptGuid.Set(g);
                }
                catch (...)
                {
                    if (outError) *outError = "sphereSculpt must be a GUID string";
                    return false;
                }
            }
            else
            {
                if (outError) *outError = "sphereSculpt must be a GUID string";
                return false;
            }
        }
        // Legacy relief keys (pre relief-unification) migrate into the companion
        // TerrainPlanetRelief component on this same entity — values exact.
        else if (property == "planetreliefamplitude")
        {
            if (!ApplyPlanetReliefField(world, entity, "amplitude", value, outError)) return false;
        }
        else if (property == "planetrelieffrequency")
        {
            if (!ApplyPlanetReliefField(world, entity, "frequency", value, outError)) return false;
        }
        else if (property == "planetreliefoctaves")
        {
            if (!ApplyPlanetReliefField(world, entity, "octaves", value, outError)) return false;
        }
        else if (property == "targetpixelerror")
        {
            if (!ParseF32(value, c.TargetPixelError, outError)) return false;
        }
        else if (property == "sealevel")
        {
            if (!ParseF32(value, c.SeaLevel, outError)) return false;
        }
        // The CBT surface debug visualization. Applied but never serialized: it is a
        // viewing mode, not authored terrain, and a scene that saved it would reopen
        // tinted. Settable here so the debug server can drive it, like the inspector.
        else if (property == "debugview")
        {
            uint32 v = 0;
            if (!ParseU32(value, v, outError)) return false;
            if (v > static_cast<uint32>(Components::TerrainDebugView::AtlasSlots))
            {
                if (outError) *outError = "debugView must be 0 (off), 1 (facets) or 2 (atlas slots)";
                return false;
            }
            c.DebugView = static_cast<Components::TerrainDebugView>(v);
        }
        else if (property == "streamingradius")
        {
            if (!ParseF32(value, c.StreamingRadius, outError)) return false;
        }
        else if (property == "materialtiling")
        {
            if (!ParseF32(value, c.MaterialTiling, outError)) return false;
        }
        else if (property == "materiallibrary")
        {
            SceneValue sv{};
            if (!ParseValue(value, sv, outError)) return false;
            if (sv.Kind == SceneValueKind::Int && sv.IntValue == 0)
            {
                c.MaterialLibraryGuid.Clear();
            }
            else if (sv.Kind == SceneValueKind::GuidRef || sv.Kind == SceneValueKind::String)
            {
                // Malformed text must FAIL loudly rather than Set: GUID's string
                // constructor yields a NULL guid for it, which would silently drop the
                // terrain back to the built-in materials.
                if (sv.StringValue.empty())
                    c.MaterialLibraryGuid.Clear();
                else if (IsGuidText(sv.StringValue))
                    c.MaterialLibraryGuid.Set(GUID(sv.StringValue));
                else
                {
                    if (outError) *outError = "materialLibrary must be a GUID string";
                    return false;
                }
            }
            else
            {
                if (outError) *outError = "materialLibrary must be a GUID string";
                return false;
            }
        }
        // `layerRole<i>` is what saves; `classifierRole<i>` is the name this key shipped
        // under and is READ so scenes already on disk keep loading. A save rewrites the
        // file under the current name, so a scene migrates the first time it is saved.
        //
        // The alias MAPS THE VALUE — it does not accept-and-ignore. Ignoring would leave
        // the identity binding in place and silently repaint the terrain with different
        // materials, which is worse than the load failure it replaces: an unknown property
        // fails the WHOLE scene load, and a failed load leaves the previous world rendering
        // with nothing on screen to say so.
        else if (const uint32 role = MatchRoleSlotKey(property); role < 4u)
        {
            uint32 slot = 0;
            if (!ParseU32(value, slot, outError)) return false;
            // Slot IDs are 8-bit by construction (TerrainMaterialRecord.h). A wider value is
            // malformed, not clampable: clamping would silently bind the role to slot 255 and
            // shade the terrain with a material the author never chose.
            if (slot >= Terrain::kMaxTerrainMaterials)
            {
                if (outError) *outError = "layerRole must be a slot ID in 0-255";
                return false;
            }
            c.LayerRoleSlot[role] = static_cast<uint8>(slot);
        }
        else if (property.size() == 12 && property.starts_with("layeralbedo") &&
                 property[11] >= '0' && property[11] <= '3')
        {
            const uint32 layer = static_cast<uint32>(property[11] - '0');
            SceneValue sv{};
            if (!ParseValue(value, sv, outError)) return false;
            if (sv.Kind == SceneValueKind::Int && sv.IntValue == 0)
            {
                c.LayerAlbedoTexture[layer].Clear();
            }
            else if (sv.Kind == SceneValueKind::GuidRef || sv.Kind == SceneValueKind::String)
            {
                // Quoted scene-file form or the bare GUID token set_component hands the
                // schema. Empty clears (parity with the grass texture refs). Anything else
                // must FAIL loudly: GUID's string constructor yields a NULL guid for
                // malformed text, so an unvalidated Set silently UNBINDS the layer.
                if (sv.StringValue.empty())
                    c.LayerAlbedoTexture[layer].Clear();
                else if (IsGuidText(sv.StringValue))
                    c.LayerAlbedoTexture[layer].Set(GUID(sv.StringValue));
                else
                {
                    if (outError) *outError = "layerAlbedo must be a GUID string (8-4-4-4-12 hex)";
                    return false;
                }
            }
            else
            {
                if (outError) *outError = "layerAlbedo must be a GUID string";
                return false;
            }
        }
        else if (property.size() == 12 && property.starts_with("layertiling") &&
                 property[11] >= '0' && property[11] <= '3')
        {
            const uint32 layer = static_cast<uint32>(property[11] - '0');
            if (!ParseF32(value, c.LayerTiling[layer], outError)) return false;
            c.LayerTiling[layer] = std::max(c.LayerTiling[layer], 0.0f);
        }
        else if (property.size() == 9 && property.starts_with("layerhex") &&
                 property[8] >= '0' && property[8] <= '3')
        {
            const uint32 layer = static_cast<uint32>(property[8] - '0');
            bool hex = false;
            if (!ParseBool(value, hex, outError)) return false;
            if (hex)
                c.LayerHexTiling |= (1u << layer);
            else
                c.LayerHexTiling &= ~(1u << layer);
        }
        else if (property == "grassenabled" || property == "grassbladespersquaremeter" ||
                 property == "grassbladeheight" || property == "grassbladewidth" ||
                 property == "grassbladesegments" ||
                 property == "grassrandomscale" ||
                 property == "grasslayerindex" ||
                 property == "grassmaskthreshold" || property == "grassdensityfalloff" ||
                 property == "grassrange" || property == "grassplacementseed" ||
                 property == "grasswindstrength" ||
                 property == "grassbrightness" || property == "grassrandombrightness" ||
                 property == "grasstranslucency" ||
                 property == "grasstexturegrass" ||
                 property == "grasstexturecardspersquaremeter" || property == "grasstexturesize")
        {
            Components::TerrainGrass grass{};
            if (const auto* existingGrass = world.GetComponent<Components::TerrainGrass>(entity))
                grass = *existingGrass;

            if (property == "grassenabled")
            {
                bool enabled = true;
                if (!ParseBool(value, enabled, outError)) return false;
                ECS::Entity(&world, entity).SetEnabled<Components::TerrainGrass>(enabled);
            }
            else if (property == "grassbladespersquaremeter") { if (!ParseF32(value, grass.BladesPerSquareMeter, outError)) return false; grass.BladesPerSquareMeter = std::max(0.0f, grass.BladesPerSquareMeter); }
            else if (property == "grassbladeheight") { if (!ParseF32(value, grass.BladeHeight, outError)) return false; grass.BladeHeight = std::max(0.0f, grass.BladeHeight); }
            else if (property == "grassbladewidth") { if (!ParseF32(value, grass.BladeWidth, outError)) return false; grass.BladeWidth = std::max(0.0f, grass.BladeWidth); }
            else if (property == "grassbladesegments") { if (!ParseU32(value, grass.BladeSegments, outError)) return false; grass.BladeSegments = std::clamp(grass.BladeSegments, Components::kMinTerrainGrassBladeSegments, Components::kMaxTerrainGrassBladeSegments); }
            else if (property == "grassrandomscale") { if (!ParseF32(value, grass.RandomScale, outError)) return false; grass.RandomScale = std::max(0.0f, grass.RandomScale); }
            else if (property == "grasslayerindex") { if (!ParseU32(value, grass.LayerIndex, outError)) return false; grass.LayerIndex = std::min(grass.LayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u); }
            else if (property == "grassmaskthreshold") { if (!ParseF32(value, grass.MaskThreshold, outError)) return false; grass.MaskThreshold = std::clamp(grass.MaskThreshold, 0.0f, 1.0f); }
            else if (property == "grassdensityfalloff") { if (!ParseF32(value, grass.DensityFalloff, outError)) return false; grass.DensityFalloff = std::max(0.0f, grass.DensityFalloff); }
            else if (property == "grassrange") { if (!ParseF32(value, grass.Range, outError)) return false; grass.Range = std::max(0.0f, grass.Range); }
            else if (property == "grassplacementseed") { if (!ParseF32(value, grass.PlacementSeed, outError)) return false; }
            else if (property == "grasswindstrength") { if (!ParseF32(value, grass.WindStrength, outError)) return false; grass.WindStrength = std::max(0.0f, grass.WindStrength); }
            else if (property == "grassbrightness") { if (!ParseF32(value, grass.Brightness, outError)) return false; grass.Brightness = std::max(0.0f, grass.Brightness); }
            else if (property == "grassrandombrightness") { if (!ParseF32(value, grass.RandomBrightness, outError)) return false; grass.RandomBrightness = std::max(0.0f, grass.RandomBrightness); }
            else if (property == "grasstranslucency") { if (!ParseF32(value, grass.Translucency, outError)) return false; grass.Translucency = std::max(0.0f, grass.Translucency); }
            else if (property == "grasstexturegrass") { if (!ParseBool(value, grass.TextureGrass, outError)) return false; }
            else if (property == "grasstexturecardspersquaremeter") { if (!ParseF32(value, grass.TextureCardsPerSquareMeter, outError)) return false; grass.TextureCardsPerSquareMeter = std::max(0.0f, grass.TextureCardsPerSquareMeter); }
            else if (property == "grasstexturesize") { if (!ParseF32(value, grass.TextureSize, outError)) return false; grass.TextureSize = std::max(0.0f, grass.TextureSize); }

            world.AddComponentImmediate(entity, grass);
        }
        else if (property == "renderlayermask")
        {
            if (!ParseU32(value, c.RenderLayerMask, outError)) return false;
        }
        else if (property == "castshadows")
        {
            if (!ParseBool(value, c.CastShadows, outError)) return false;
        }
        else if (property == "receiveshadows")
        {
            if (!ParseBool(value, c.ReceiveShadows, outError)) return false;
        }
        // "generatecollision" and "collisionlodbias" are deprecated;
        // silently ignore for backwards compatibility with old scene files.
        else if (property == "generatecollision" || property == "collisionlodbias")
        {
            // No-op: physics collision now uses HeightFieldColliderShape component.
        }
        // "patchgridsize" / "lodrangescale" retired from the Terrain component (they only
        // fed the CDLOD-era service quadtree, now service-internal constants). A hand-written
        // schema hard-fails the whole scene load on an unrecognized key, so ignore these
        // one release beat with a warning — old scenes (e.g. Lanscape.scene) still load.
        else if (property == "patchgridsize" || property == "lodrangescale")
        {
            WarnRetiredTerrainProperty(property);
        }
        else if (property == "grassdensity" || property == "grassfadestart" ||
                 property == "grassfadeend" || property == "grassdistancefadeenabled" ||
                 property == "grasstexturedensity")
        {
            WarnRetiredGrassProperty(property, kRetiredGrassDensityReason);
        }
        else if (property == "grassscale")
        {
            WarnRetiredGrassProperty(property, kRetiredGrassScaleReason);
        }
        else if (property == "grasscurvature")
        {
            WarnRetiredGrassProperty(property, kRetiredGrassCurvatureReason);
        }
        else if (property == "grassshadowstrength")
        {
            WarnRetiredGrassProperty(property, kRetiredGrassShadowStrengthReason);
        }
        else
        {
            if (outError)
                *outError = "Unknown Terrain property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    void EnumerateAssetReferences(const ECS::World& world,
                                  ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override
    {
        const auto* c = world.GetComponent<Components::Terrain>(entity);
        if (!c || !visitor)
            return;

        // The sphere sculpt sidecar is a real dependency edge: without it a packaged /
        // copied project would drop the .tsculpt and the planet would load unsculpted.
        const GUID sculpt = c->SphereSculptGuid.ToGuid();
        if (!sculpt.IsNull())
            visitor(sculpt, AssetType::TerrainSphereSculptData, std::string_view{}, "spheresculpt");

        // The material library is a dependency edge: without it a packaged project would drop
        // the .terrainmatlib and every terrain would shade from the built-in materials.
        const GUID library = c->MaterialLibraryGuid.ToGuid();
        if (!library.IsNull())
            visitor(library, AssetType::TerrainMaterialLibrary, std::string_view{},
                    "materiallibrary");

        // Per-layer albedo textures are dependency edges: a packaged project must carry them
        // or the terrain loads back to the untextured tint fallback.
        static constexpr const char* kLayerAlbedoKeys[4] = {"layeralbedo0", "layeralbedo1",
                                                            "layeralbedo2", "layeralbedo3"};
        for (uint32 i = 0; i < 4; ++i)
        {
            const GUID layerGuid = c->LayerAlbedoTexture[i].ToGuid();
            if (!layerGuid.IsNull())
                visitor(layerGuid, AssetType::Texture, std::string_view{}, kLayerAlbedoKeys[i]);
        }

        const GUID g = c->TerrainAssetGuid.ToGuid();
        if (g.IsNull())
            return;

        AssetType type = static_cast<AssetType>(c->TerrainAssetType);
        if (type == AssetType::Unknown && c->BaseSource == Components::TerrainBaseSource::HeightmapAsset)
        {
            // Nothing writes TerrainAssetType today, so a heightmap reference
            // must not be dropped for its 0 type field — resolve the edge's
            // type from the registry, falling back to Texture (the PNG16
            // path) when the registry can't answer.
            type = AssetType::Texture;
            auto& engine = EngineCore::GetInstance();
            if (engine.IsInitialized())
            {
                AssetMetadata meta{};
                if (engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(g, meta)
                    && meta.Type != AssetType::Unknown)
                {
                    type = meta.Type;
                }
            }
        }
        if (type == AssetType::Unknown)
            return;

        visitor(g, type, std::string_view{}, "terrainasset");
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::Terrain>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::Terrain{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        // The component's external resources are released by the OnRemove hook
        // TerrainECS::RegisterTerrainWorldHooks installs, which the remove below
        // fires — the same body World::Clear and World::DestroyEntity reach.
        world.RemoveComponentImmediate<Components::Terrain>(entity);
        return true;
    }
};

GE_REGISTER_SCENE_SCHEMA(TerrainSchema)

// ---------------------------------------------------------------------------
// TerrainCBT migration (legacy) — folds the retired renderer component into Terrain
// ---------------------------------------------------------------------------
// Pre-merge scenes carry a scanner-serialized `TerrainCBT` block (domain, planet
// radius/relief, target pixel error, maxDepth). Post-merge the type is gone, so
// without this the block would load as an unknown component (values never applied)
// and a saved planet would silently flatten to Planar — and the orphan block would
// re-emit on every save. This schema intercepts the name, migrates the fields into
// the merged Terrain, and emits NOTHING on save, consuming the block for good.
// maxDepth is intentionally dropped (it is auto-derived now). Not reachable from the
// Add-Component menu (that is reflection-driven; TerrainCBT is no longer reflected).
class TerrainCBTMigrationSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainCBT"; }

    void Serialize(const ECS::World&, ECS::EntityHandle,
                   [[maybe_unused]] const SceneSaveContext&,
                   std::vector<std::string>&) const override
    {
        // Consume: never re-serialize. The fields live on Terrain now.
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::Terrain c{};
        if (const auto* existing = world.GetComponent<Components::Terrain>(entity))
            c = *existing;

        if (property == "domain")
        {
            if (!ParseDomain(value, c.Domain, outError)) return false;
        }
        else if (property == "planetradius")
        {
            if (!ParseF32(value, c.PlanetRadius, outError)) return false;
        }
        // Legacy relief keys (pre relief-unification) migrate into the companion
        // TerrainPlanetRelief component on this same entity — values exact.
        else if (property == "planetreliefamplitude")
        {
            if (!ApplyPlanetReliefField(world, entity, "amplitude", value, outError)) return false;
        }
        else if (property == "planetrelieffrequency")
        {
            if (!ApplyPlanetReliefField(world, entity, "frequency", value, outError)) return false;
        }
        else if (property == "planetreliefoctaves")
        {
            if (!ApplyPlanetReliefField(world, entity, "octaves", value, outError)) return false;
        }
        else if (property == "targetpixelerror")
        {
            if (!ParseF32(value, c.TargetPixelError, outError)) return false;
        }
        else if (property == "maxdepth")
        {
            // Dropped: MaxDepth is auto-derived from domain + size / radius now.
        }
        // Any other legacy TerrainCBT field: ignore (forward/backward tolerant).

        world.AddComponentImmediate(entity, c);
        return true;
    }

    // A props-less [TerrainCBT] block in an old scene must not fail the load; it also
    // must not create a bare component (the type is gone). No-op success.
    bool AddDefault(ECS::World&, ECS::EntityHandle, std::string*) const override { return true; }
    bool Remove(ECS::World&, ECS::EntityHandle) const override { return true; }
};

GE_REGISTER_SCENE_SCHEMA(TerrainCBTMigrationSchema)

// ---------------------------------------------------------------------------
// TerrainPlanetRelief — the planet's base procedural noise (relief-unification slice)
// ---------------------------------------------------------------------------
// The relief that used to be three Terrain.planetRelief* fields now lives here. Legacy
// scenes migrate through TerrainSchema / TerrainCBTMigrationSchema (both fold the old
// keys into this component via ApplyPlanetReliefField); post-slice scenes carry a
// TerrainPlanetRelief block written here. Only spherical terrains carry the component,
// so serialization is presence-gated (planar scenes emit nothing).
class TerrainPlanetReliefSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainPlanetRelief"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainPlanetRelief>(entity);
        if (!c)
            return;
        outLines.push_back("TerrainPlanetRelief.amplitude = " + FormatFloat(c->Amplitude));
        outLines.push_back("TerrainPlanetRelief.frequency = " + FormatFloat(c->Frequency));
        outLines.push_back("TerrainPlanetRelief.octaves = " + std::to_string(c->Octaves));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        if (property == "amplitude" || property == "frequency" || property == "octaves")
            return ApplyPlanetReliefField(world, entity, property, value, outError);
        if (outError) *outError = "Unknown TerrainPlanetRelief property";
        return false;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainPlanetRelief>(entity)) return true;
        world.AddComponentImmediate(entity, Components::TerrainPlanetRelief{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainPlanetRelief>(entity);
        return true;
    }
};

GE_REGISTER_SCENE_SCHEMA(TerrainPlanetReliefSchema)

// ---------------------------------------------------------------------------
// Terrain modifier schemas — Flatten / Noise / Stamp / PaintLayer / Spline
// ---------------------------------------------------------------------------

// Writes the shared TerrainModifier base fields (Shape, Radius, RectHalfX/Z,
// Falloff, Priority, Enabled) into outLines under `<prefix>.*`.
template <class TMod>
static void SerializeModifierBase(const TMod& c, const char* prefix, std::vector<std::string>& outLines)
{
    const std::string p = std::string(prefix) + ".";
    outLines.push_back(p + "shape = " + std::to_string(static_cast<uint32>(c.Shape)));
    outLines.push_back(p + "radius = " + FormatFloat(c.Radius));
    outLines.push_back(p + "rectHalfX = " + FormatFloat(c.RectHalfX));
    outLines.push_back(p + "rectHalfZ = " + FormatFloat(c.RectHalfZ));
    outLines.push_back(p + "falloff = " + FormatFloat(c.Falloff));
    outLines.push_back(p + "priority = " + FormatFloat(c.Priority));
    outLines.push_back(p + "enabled = " + (c.Enabled ? "true" : "false"));
}

// Parses a single shared base-field property; returns true if handled (even on error).
// Pass `handled = false, outError` empty to indicate the property was unrecognized
// (so callers can try type-specific fields next).
template <class TMod>
static bool ApplyModifierBaseProperty(TMod& c, std::string_view property, std::string_view value,
                                      std::string* outError, bool& handled)
{
    handled = true;
    if (property == "shape")
    {
        uint32 v = 0;
        if (!ParseU32(value, v, outError)) return false;
        if (v > 2) { if (outError) *outError = "shape must be 0-2"; return false; }
        c.Shape = static_cast<Components::TerrainModifierShape>(v);
        return true;
    }
    if (property == "radius")    return ParseF32(value, c.Radius, outError);
    if (property == "recthalfx") return ParseF32(value, c.RectHalfX, outError);
    if (property == "recthalfz") return ParseF32(value, c.RectHalfZ, outError);
    if (property == "falloff")   return ParseF32(value, c.Falloff, outError);
    if (property == "priority")  return ParseF32(value, c.Priority, outError);
    if (property == "enabled")   return ParseBool(value, c.Enabled, outError);
    handled = false;
    return true;
}

// ---------------------------------------------------------------------------
// Pre-volume modifier -> modifier volume + effect (load-time migration)
//
// Each of the five pre-volume modifier components is semantically ONE region
// plus ONE effect, so a scene that names them loads as the volume model and
// re-saves in it: their schemas below parse the authored block exactly as
// before, convert, and drop the pre-volume component. The conversion is exact —
// the fields map one-to-one and the bake is byte-identical
// (TerrainModifierVolumeTests' migration oracles).
//
// The conversion runs from ApplyProperties, the grouped hook the scene loader
// uses, so it sees a whole component block at once and never has to guess at a
// property it has not read yet. A lone ApplyProperty call — SceneIO's
// per-property fallback after a malformed value, or a tool — accumulates into
// the parse block and leaves it on the entity, where nothing reads it.
// ---------------------------------------------------------------------------

// Once per scene file, not once per modifier: a road network can hold dozens of
// pre-volume modifiers and one report is the whole story. Keyed on the scene file
// exactly as WarnRetiredSmoothBlend is; a load with no scene file (a tool or test
// applying a single block) has none to key on, so it always reports.
//
// This is the per-scene migration warning terrain-editing-ux-design 1.2(e) assigns
// to the retirement: the load is silent about a conversion that changes what a
// re-save writes, and a scene nobody re-saves keeps converting on every load.
static void WarnMigratedPreVolumeModifier(const SceneLoadContext& ctx, std::string_view blockName)
{
    if (ctx.SceneFile != nullptr)
    {
        static std::mutex mutex;
        static std::unordered_set<std::string> warned;
        std::lock_guard<std::mutex> lock(mutex);
        if (!warned.emplace(ctx.SceneFile->string()).second)
            return;
        Logger::Log::Warning(
            "Scene '{}': {} is a retired pre-volume terrain modifier — migrating it to a "
            "TerrainModifierVolume plus its effect. Re-save the scene to drop the legacy block.",
            ctx.SceneFile->string(), blockName);
        return;
    }
    Logger::Log::Warning(
        "Scene: {} is a retired pre-volume terrain modifier — migrating it to a "
        "TerrainModifierVolume plus its effect. Re-save the scene to drop the legacy block.",
        blockName);
}

// Shape::Spline resolved through SignedDistanceToSplineXZ, which FILLS a closed
// spline's interior — so the exact migration of a spline-shaped modifier is
// SplineArea. SplinePath (band only, hole kept) is the capability the pre-volume
// model could not express and is never produced by migration.
Components::TerrainVolumeShape MigrateShape(Components::TerrainModifierShape shape)
{
    switch (shape)
    {
    case Components::TerrainModifierShape::Rectangle:
        return Components::TerrainVolumeShape::Rectangle;
    case Components::TerrainModifierShape::Spline:
        return Components::TerrainVolumeShape::SplineArea;
    case Components::TerrainModifierShape::Circle:
        break;
    }
    return Components::TerrainVolumeShape::Circle;
}

// The stack position for the next effect migrated onto this entity: one past the
// highest already there, so several pre-volume modifiers on one entity keep the
// order they were converted in. Shared with the editor's Add Effect so a
// migrated stack and an authored one number their entries the same way.
using TerrainECS::NextEffectStackOrder;

// Write the region half of a pre-volume modifier onto the entity's volume.
// A second pre-volume modifier on the SAME entity would overwrite the first
// one's region: the pre-volume model let each component carry its own shape, the
// volume model deliberately does not. That is a real (if unusual) authoring
// shape, so it is reported rather than silently resolved.
template <class TMod>
void MigrateRegion(ECS::World& world, ECS::EntityHandle entity, const TMod& c)
{
    Components::TerrainModifierVolume vol{};
    const auto* existing = world.GetComponent<Components::TerrainModifierVolume>(entity);
    if (existing)
    {
        vol = *existing;
        const bool sameRegion = vol.Shape == MigrateShape(c.Shape) && vol.Radius == c.Radius
            && vol.RectHalfX == c.RectHalfX && vol.RectHalfZ == c.RectHalfZ
            && vol.Falloff == c.Falloff && vol.Priority == c.Priority;
        if (!sameRegion)
        {
            Logger::Log::Warning(
                "Scene: entity {} carries several terrain modifiers with DIFFERENT shapes or "
                "priorities. A modifier volume owns one region for the whole entity, so the "
                "last one loaded wins — split them onto separate entities to keep both regions.",
                entity.id);
        }
    }
    vol.Shape = MigrateShape(c.Shape);
    vol.Radius = c.Radius;
    vol.RectHalfX = c.RectHalfX;
    vol.RectHalfZ = c.RectHalfZ;
    vol.Falloff = c.Falloff;
    vol.Priority = c.Priority;
    world.AddComponentImmediate(entity, vol);
}

// TerrainSplineModifier carries no shape fields — its region has always been the
// entity's spline, and its falloff/priority are its own. Its Shape::Spline
// resolved through SignedDistanceToSplineXZ, which FILLS a closed spline's
// interior, so the region is SplineArea; on an open spline the two spline shapes
// are identical, because the fill only applies to a closed one.
//
// The SOLE constructor of a spline modifier's region: the schema's AddDefault
// converts a default-valued component through it rather than building a second
// volume, so a bare block and an authored one can never mean different shapes.
void MigrateSplineRegion(ECS::World& world, ECS::EntityHandle entity,
                         const LegacyModifierBlocks::TerrainSplineModifier& c)
{
    Components::TerrainModifierVolume vol{};
    if (const auto* existing = world.GetComponent<Components::TerrainModifierVolume>(entity))
    {
        vol = *existing;
        const bool sameRegion = vol.Shape == Components::TerrainVolumeShape::SplineArea
            && vol.Falloff == c.Falloff && vol.Priority == c.Priority;
        if (!sameRegion)
        {
            Logger::Log::Warning(
                "Scene: entity {} carries several terrain modifiers with DIFFERENT shapes or "
                "priorities. A modifier volume owns one region for the whole entity, so the "
                "last one loaded wins — split them onto separate entities to keep both regions.",
                entity.id);
        }
    }
    vol.Shape = Components::TerrainVolumeShape::SplineArea;
    vol.Falloff = c.Falloff;
    vol.Priority = c.Priority;
    world.AddComponentImmediate(entity, vol);
}

class TerrainFlattenModifierSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainFlattenModifier"; }

    bool ApplyProperties(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                         std::span<const std::pair<std::string_view, std::string_view>> props,
                         std::string* outError, std::size_t* outFailedIndex) const override
    {
        for (std::size_t i = 0; i < props.size(); ++i)
            if (!ApplyProperty(world, entity, ctx, props[i].first, props[i].second, outError))
            {
                if (outFailedIndex) *outFailedIndex = i;
                return false;
            }

        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainFlattenModifier>(entity);
        if (!c) return true;
        const LegacyModifierBlocks::TerrainFlattenModifier legacy = *c;

        WarnMigratedPreVolumeModifier(ctx, "TerrainFlattenModifier");
        MigrateRegion(world, entity, legacy);
        Components::TerrainFlattenEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        fx.Enabled = legacy.Enabled;
        // The pre-volume component IGNORED TargetHeight when UseEntityHeight was
        // set; the effect says the same thing as a zero offset from the volume's
        // reference height.
        fx.UseVolumeHeight = legacy.UseEntityHeight;
        fx.TargetHeight = legacy.UseEntityHeight ? 0.0f : legacy.TargetHeight;
        world.AddComponentImmediate(entity, fx);
        world.RemoveComponentImmediate<LegacyModifierBlocks::TerrainFlattenModifier>(entity);
        return true;
    }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainFlattenModifier>(entity);
        if (!c) return;
        SerializeModifierBase(*c, "TerrainFlattenModifier", outLines);
        outLines.push_back(std::string("TerrainFlattenModifier.targetHeight = ") + FormatFloat(c->TargetHeight));
        outLines.push_back(std::string("TerrainFlattenModifier.useEntityHeight = ") + (c->UseEntityHeight ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        LegacyModifierBlocks::TerrainFlattenModifier c{};
        if (const auto* existing = world.GetComponent<LegacyModifierBlocks::TerrainFlattenModifier>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyModifierBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            if (property == "targetheight")
            {
                if (!ParseF32(value, c.TargetHeight, outError)) return false;
            }
            else if (property == "useentityheight")
            {
                if (!ParseBool(value, c.UseEntityHeight, outError)) return false;
            }
            else
            {
                if (outError) *outError = "Unknown TerrainFlattenModifier property";
                return false;
            }
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    // A component block with no properties means "add defaults" (SceneIO), so a
    // bare block must land the same volume model an authored one does. It
    // converts a default-valued component through the same region constructor
    // the authored path uses, so the two cannot disagree about the region.
    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (!world.GetComponent<Components::TerrainModifierVolume>(entity))
            MigrateRegion(world, entity, LegacyModifierBlocks::TerrainFlattenModifier{});
        if (!world.GetComponent<Components::TerrainFlattenEffect>(entity))
        {
            Components::TerrainFlattenEffect fx{};
            fx.StackOrder = NextEffectStackOrder(world, entity);
            world.AddComponentImmediate(entity, fx);
        }
        return true;
    }

    // Removes what this schema's load produces (the effect), and the pre-volume
    // component if some other path put one there. The volume stays: sibling
    // effects may still be using the region.
    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<LegacyModifierBlocks::TerrainFlattenModifier>(entity);
        world.RemoveComponentImmediate<Components::TerrainFlattenEffect>(entity);
        return true;
    }
};

class TerrainNoiseModifierSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainNoiseModifier"; }

    bool ApplyProperties(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                         std::span<const std::pair<std::string_view, std::string_view>> props,
                         std::string* outError, std::size_t* outFailedIndex) const override
    {
        for (std::size_t i = 0; i < props.size(); ++i)
            if (!ApplyProperty(world, entity, ctx, props[i].first, props[i].second, outError))
            {
                if (outFailedIndex) *outFailedIndex = i;
                return false;
            }

        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainNoiseModifier>(entity);
        if (!c) return true;
        const LegacyModifierBlocks::TerrainNoiseModifier legacy = *c;

        WarnMigratedPreVolumeModifier(ctx, "TerrainNoiseModifier");
        MigrateRegion(world, entity, legacy);
        Components::TerrainNoiseEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        fx.Enabled = legacy.Enabled;
        fx.Blend = legacy.Blend;
        fx.Frequency = legacy.Frequency;
        fx.Amplitude = legacy.Amplitude;
        fx.Octaves = legacy.Octaves;
        fx.Seed = legacy.Seed;
        fx.Lacunarity = legacy.Lacunarity;
        fx.Persistence = legacy.Persistence;
        fx.BlendSmoothing = legacy.BlendSmoothing;
        world.AddComponentImmediate(entity, fx);
        world.RemoveComponentImmediate<LegacyModifierBlocks::TerrainNoiseModifier>(entity);
        return true;
    }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainNoiseModifier>(entity);
        if (!c) return;
        SerializeModifierBase(*c, "TerrainNoiseModifier", outLines);
        outLines.push_back(std::string("TerrainNoiseModifier.blend = ") + std::to_string(static_cast<uint32>(c->Blend)));
        outLines.push_back(std::string("TerrainNoiseModifier.frequency = ") + FormatFloat(c->Frequency));
        outLines.push_back(std::string("TerrainNoiseModifier.amplitude = ") + FormatFloat(c->Amplitude));
        outLines.push_back(std::string("TerrainNoiseModifier.octaves = ") + std::to_string(c->Octaves));
        outLines.push_back(std::string("TerrainNoiseModifier.seed = ") + std::to_string(c->Seed));
        outLines.push_back(std::string("TerrainNoiseModifier.lacunarity = ") + FormatFloat(c->Lacunarity));
        outLines.push_back(std::string("TerrainNoiseModifier.persistence = ") + FormatFloat(c->Persistence));
        outLines.push_back(std::string("TerrainNoiseModifier.blendSmoothing = ") + FormatFloat(c->BlendSmoothing));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        LegacyModifierBlocks::TerrainNoiseModifier c{};
        if (const auto* existing = world.GetComponent<LegacyModifierBlocks::TerrainNoiseModifier>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyModifierBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            if (property == "blend")
            {
                if (!ParseBlend(ctx, value, BlendSet::HeightOperators, c.Blend, outError)) return false;
            }
            else if (property == "frequency")   { if (!ParseF32(value, c.Frequency, outError)) return false; }
            else if (property == "amplitude")   { if (!ParseF32(value, c.Amplitude, outError)) return false; }
            else if (property == "octaves")     { if (!ParseU32(value, c.Octaves, outError)) return false; }
            else if (property == "seed")        { if (!ParseU32(value, c.Seed, outError)) return false; }
            else if (property == "lacunarity")  { if (!ParseF32(value, c.Lacunarity, outError)) return false; }
            else if (property == "persistence") { if (!ParseF32(value, c.Persistence, outError)) return false; }
            else if (property == "blendsmoothing") { if (!ParseF32(value, c.BlendSmoothing, outError)) return false; }
            else
            {
                if (outError) *outError = "Unknown TerrainNoiseModifier property";
                return false;
            }
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (!world.GetComponent<Components::TerrainModifierVolume>(entity))
            MigrateRegion(world, entity, LegacyModifierBlocks::TerrainNoiseModifier{});
        if (!world.GetComponent<Components::TerrainNoiseEffect>(entity))
        {
            Components::TerrainNoiseEffect fx{};
            fx.StackOrder = NextEffectStackOrder(world, entity);
            world.AddComponentImmediate(entity, fx);
        }
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<LegacyModifierBlocks::TerrainNoiseModifier>(entity);
        world.RemoveComponentImmediate<Components::TerrainNoiseEffect>(entity);
        return true;
    }
};

class TerrainStampModifierSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainStampModifier"; }

    bool ApplyProperties(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                         std::span<const std::pair<std::string_view, std::string_view>> props,
                         std::string* outError, std::size_t* outFailedIndex) const override
    {
        for (std::size_t i = 0; i < props.size(); ++i)
            if (!ApplyProperty(world, entity, ctx, props[i].first, props[i].second, outError))
            {
                if (outFailedIndex) *outFailedIndex = i;
                return false;
            }

        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainStampModifier>(entity);
        if (!c) return true;
        const LegacyModifierBlocks::TerrainStampModifier legacy = *c;

        WarnMigratedPreVolumeModifier(ctx, "TerrainStampModifier");
        MigrateRegion(world, entity, legacy);
        Components::TerrainStampEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        fx.Enabled = legacy.Enabled;
        fx.Blend = legacy.Blend;
        fx.HeightScale = legacy.HeightScale;
        fx.Rotation = legacy.Rotation;
        fx.StampAssetGuid = legacy.StampAssetGuid;
        fx.BlendSmoothing = legacy.BlendSmoothing;
        world.AddComponentImmediate(entity, fx);
        world.RemoveComponentImmediate<LegacyModifierBlocks::TerrainStampModifier>(entity);
        return true;
    }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainStampModifier>(entity);
        if (!c) return;
        SerializeModifierBase(*c, "TerrainStampModifier", outLines);
        outLines.push_back(std::string("TerrainStampModifier.blend = ") + std::to_string(static_cast<uint32>(c->Blend)));
        outLines.push_back(std::string("TerrainStampModifier.heightScale = ") + FormatFloat(c->HeightScale));
        outLines.push_back(std::string("TerrainStampModifier.rotation = ") + FormatFloat(c->Rotation));
        outLines.push_back(std::string("TerrainStampModifier.blendSmoothing = ") + FormatFloat(c->BlendSmoothing));

        if (!c->StampAssetGuid.IsNull())
        {
            const GUID g = c->StampAssetGuid.ToGuid();
            outLines.push_back(std::string("TerrainStampModifier.stampAsset = \"") + g.ToString() + "\"");
        }
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        LegacyModifierBlocks::TerrainStampModifier c{};
        if (const auto* existing = world.GetComponent<LegacyModifierBlocks::TerrainStampModifier>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyModifierBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            if (property == "blend")
            {
                if (!ParseBlend(ctx, value, BlendSet::HeightOperators, c.Blend, outError)) return false;
            }
            else if (property == "heightscale") { if (!ParseF32(value, c.HeightScale, outError)) return false; }
            else if (property == "rotation")    { if (!ParseF32(value, c.Rotation, outError)) return false; }
            else if (property == "blendsmoothing") { if (!ParseF32(value, c.BlendSmoothing, outError)) return false; }
            else if (property == "stampasset")
            {
                SceneValue sv{};
                if (!ParseValue(value, sv, outError)) return false;
                if (sv.Kind == SceneValueKind::Int && sv.IntValue == 0)
                {
                    c.StampAssetGuid.Clear();
                }
                else if (sv.Kind == SceneValueKind::String)
                {
                    try
                    {
                        GUID g(sv.StringValue);
                        c.StampAssetGuid.Set(g);
                    }
                    catch (...)
                    {
                        if (outError) *outError = "stampAsset must be a GUID string";
                        return false;
                    }
                }
                else
                {
                    if (outError) *outError = "stampAsset must be a GUID string";
                    return false;
                }
            }
            else
            {
                if (outError) *outError = "Unknown TerrainStampModifier property";
                return false;
            }
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    void EnumerateAssetReferences(const ECS::World& world,
                                  ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override
    {
        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainStampModifier>(entity);
        if (!c || !visitor)
            return;

        const GUID g = c->StampAssetGuid.ToGuid();
        if (g.IsNull())
            return;
        // Masks are Texture assets by contract.
        visitor(g, AssetType::Texture, std::string_view{}, "stampasset");
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (!world.GetComponent<Components::TerrainModifierVolume>(entity))
            MigrateRegion(world, entity, LegacyModifierBlocks::TerrainStampModifier{});
        if (!world.GetComponent<Components::TerrainStampEffect>(entity))
        {
            Components::TerrainStampEffect fx{};
            fx.StackOrder = NextEffectStackOrder(world, entity);
            world.AddComponentImmediate(entity, fx);
        }
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<LegacyModifierBlocks::TerrainStampModifier>(entity);
        world.RemoveComponentImmediate<Components::TerrainStampEffect>(entity);
        return true;
    }
};

class TerrainPaintLayerModifierSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainPaintLayerModifier"; }

    bool ApplyProperties(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                         std::span<const std::pair<std::string_view, std::string_view>> props,
                         std::string* outError, std::size_t* outFailedIndex) const override
    {
        for (std::size_t i = 0; i < props.size(); ++i)
            if (!ApplyProperty(world, entity, ctx, props[i].first, props[i].second, outError))
            {
                if (outFailedIndex) *outFailedIndex = i;
                return false;
            }

        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainPaintLayerModifier>(entity);
        if (!c) return true;
        const LegacyModifierBlocks::TerrainPaintLayerModifier legacy = *c;

        WarnMigratedPreVolumeModifier(ctx, "TerrainPaintLayerModifier");
        MigrateRegion(world, entity, legacy);
        Components::TerrainPaintLayerEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        fx.Enabled = legacy.Enabled;
        fx.LayerIndex = legacy.LayerIndex;
        fx.Strength = legacy.Strength;
        fx.Replace = legacy.Replace;
        world.AddComponentImmediate(entity, fx);
        world.RemoveComponentImmediate<LegacyModifierBlocks::TerrainPaintLayerModifier>(entity);
        return true;
    }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainPaintLayerModifier>(entity);
        if (!c) return;
        SerializeModifierBase(*c, "TerrainPaintLayerModifier", outLines);
        outLines.push_back(std::string("TerrainPaintLayerModifier.layerIndex = ") + std::to_string(c->LayerIndex));
        outLines.push_back(std::string("TerrainPaintLayerModifier.strength = ") + FormatFloat(c->Strength));
        outLines.push_back(std::string("TerrainPaintLayerModifier.replace = ") + (c->Replace ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        LegacyModifierBlocks::TerrainPaintLayerModifier c{};
        if (const auto* existing = world.GetComponent<LegacyModifierBlocks::TerrainPaintLayerModifier>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyModifierBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            if (property == "layerindex") { if (!ParseU32(value, c.LayerIndex, outError)) return false; c.LayerIndex = std::min(c.LayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u); }
            else if (property == "strength") { if (!ParseF32(value, c.Strength, outError)) return false; }
            else if (property == "replace")  { if (!ParseBool(value, c.Replace, outError)) return false; }
            else
            {
                if (outError) *outError = "Unknown TerrainPaintLayerModifier property";
                return false;
            }
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (!world.GetComponent<Components::TerrainModifierVolume>(entity))
            MigrateRegion(world, entity, LegacyModifierBlocks::TerrainPaintLayerModifier{});
        if (!world.GetComponent<Components::TerrainPaintLayerEffect>(entity))
        {
            Components::TerrainPaintLayerEffect fx{};
            fx.StackOrder = NextEffectStackOrder(world, entity);
            world.AddComponentImmediate(entity, fx);
        }
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<LegacyModifierBlocks::TerrainPaintLayerModifier>(entity);
        world.RemoveComponentImmediate<Components::TerrainPaintLayerEffect>(entity);
        return true;
    }
};

// TerrainSplineModifier does NOT share GE_TERRAIN_MODIFIER_COMMON_FIELDS — its
// shape is implicit (the SplineComponent's path) so it has its own reduced
// field set: Blend, Falloff, Priority, HeightOffset, Flatten, Enabled, plus
// PaintLayer/LayerIndex/Strength for optional layer painting.
class TerrainSplineModifierSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainSplineModifier"; }

    bool ApplyProperties(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                         std::span<const std::pair<std::string_view, std::string_view>> props,
                         std::string* outError, std::size_t* outFailedIndex) const override
    {
        for (std::size_t i = 0; i < props.size(); ++i)
            if (!ApplyProperty(world, entity, ctx, props[i].first, props[i].second, outError))
            {
                if (outFailedIndex) *outFailedIndex = i;
                return false;
            }

        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainSplineModifier>(entity);
        if (!c) return true;
        const LegacyModifierBlocks::TerrainSplineModifier legacy = *c;

        WarnMigratedPreVolumeModifier(ctx, "TerrainSplineModifier");
        MigrateSplineRegion(world, entity, legacy);
        if (legacy.Flatten)
        {
            // Flatten to the spline's own Y at the closest point, offset by
            // HeightOffset — exactly what the volume's reference height means.
            Components::TerrainFlattenEffect fx{};
            fx.StackOrder = NextEffectStackOrder(world, entity);
            fx.Enabled = legacy.Enabled;
            fx.UseVolumeHeight = true;
            fx.TargetHeight = legacy.HeightOffset;
            world.AddComponentImmediate(entity, fx);
        }
        else
        {
            // Offset mode always accumulated, whatever Blend said.
            Components::TerrainHeightOffsetEffect fx{};
            fx.StackOrder = NextEffectStackOrder(world, entity);
            fx.Enabled = legacy.Enabled;
            fx.Offset = legacy.HeightOffset;
            fx.Blend = Components::TerrainModifierBlend::Add;
            world.AddComponentImmediate(entity, fx);
        }
        if (legacy.PaintLayer)
        {
            Components::TerrainPaintLayerEffect fx{};
            fx.StackOrder = NextEffectStackOrder(world, entity);
            fx.Enabled = legacy.Enabled;
            fx.LayerIndex = legacy.PaintLayerIndex;
            fx.Strength = legacy.PaintStrength;
            // Spline painting replaced the layer so grass masks could exclude the path.
            fx.Replace = true;
            world.AddComponentImmediate(entity, fx);
        }
        world.RemoveComponentImmediate<LegacyModifierBlocks::TerrainSplineModifier>(entity);
        return true;
    }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<LegacyModifierBlocks::TerrainSplineModifier>(entity);
        if (!c) return;
        outLines.push_back(std::string("TerrainSplineModifier.blend = ") + std::to_string(static_cast<uint32>(c->Blend)));
        outLines.push_back(std::string("TerrainSplineModifier.falloff = ") + FormatFloat(c->Falloff));
        outLines.push_back(std::string("TerrainSplineModifier.priority = ") + FormatFloat(c->Priority));
        outLines.push_back(std::string("TerrainSplineModifier.heightOffset = ") + FormatFloat(c->HeightOffset));
        outLines.push_back(std::string("TerrainSplineModifier.flatten = ") + (c->Flatten ? "true" : "false"));
        outLines.push_back(std::string("TerrainSplineModifier.enabled = ") + (c->Enabled ? "true" : "false"));
        outLines.push_back(std::string("TerrainSplineModifier.paintLayer = ") + (c->PaintLayer ? "true" : "false"));
        outLines.push_back(std::string("TerrainSplineModifier.paintLayerIndex = ") + std::to_string(c->PaintLayerIndex));
        outLines.push_back(std::string("TerrainSplineModifier.paintStrength = ") + FormatFloat(c->PaintStrength));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        LegacyModifierBlocks::TerrainSplineModifier c{};
        if (const auto* existing = world.GetComponent<LegacyModifierBlocks::TerrainSplineModifier>(entity))
            c = *existing;

        if (property == "blend")
        {
            if (!ParseBlend(ctx, value, BlendSet::HeightOperators, c.Blend, outError)) return false;
        }
        else if (property == "falloff")         { if (!ParseF32(value, c.Falloff, outError)) return false; }
        else if (property == "priority")        { if (!ParseF32(value, c.Priority, outError)) return false; }
        else if (property == "heightoffset")    { if (!ParseF32(value, c.HeightOffset, outError)) return false; }
        else if (property == "flatten")         { if (!ParseBool(value, c.Flatten, outError)) return false; }
        else if (property == "enabled")         { if (!ParseBool(value, c.Enabled, outError)) return false; }
        else if (property == "paintlayer")      { if (!ParseBool(value, c.PaintLayer, outError)) return false; }
        else if (property == "paintlayerindex") { if (!ParseU32(value, c.PaintLayerIndex, outError)) return false; c.PaintLayerIndex = std::min(c.PaintLayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u); }
        else if (property == "paintstrength")   { if (!ParseF32(value, c.PaintStrength, outError)) return false; }
        else
        {
            if (outError) *outError = "Unknown TerrainSplineModifier property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    // A bare "TerrainSplineModifier" block adds the volume model, so the editor
    // and blueprint overrides create what a scene load produces. It converts a
    // default-valued component through the same region constructor the authored
    // path uses, so the two cannot disagree about the region's shape.
    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (!world.GetComponent<Components::TerrainModifierVolume>(entity))
            MigrateSplineRegion(world, entity, LegacyModifierBlocks::TerrainSplineModifier{});
        if (!world.GetComponent<Components::TerrainFlattenEffect>(entity))
        {
            Components::TerrainFlattenEffect fx{};
            fx.StackOrder = NextEffectStackOrder(world, entity);
            world.AddComponentImmediate(entity, fx);
        }
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<LegacyModifierBlocks::TerrainSplineModifier>(entity);
        world.RemoveComponentImmediate<Components::TerrainFlattenEffect>(entity);
        world.RemoveComponentImmediate<Components::TerrainHeightOffsetEffect>(entity);
        return true;
    }
};

// TerrainSculptZone / TerrainPaintZone are payload-backed transformable zones
// (edit-pipeline §3.2). They share no fields with the stamp/flatten family
// (extents + payload GUID, not Shape/Radius), so they carry their own schemas.
// The payload's texels live in the .tzone asset (dependency-enumerated below);
// only the reference + zone framing serialize into the scene.
class TerrainSculptZoneSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainSculptZone"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainSculptZone>(entity);
        if (!c) return;
        outLines.push_back(std::string("TerrainSculptZone.extentX = ") + FormatFloat(c->ExtentX));
        outLines.push_back(std::string("TerrainSculptZone.extentZ = ") + FormatFloat(c->ExtentZ));
        outLines.push_back(std::string("TerrainSculptZone.falloff = ") + FormatFloat(c->Falloff));
        outLines.push_back(std::string("TerrainSculptZone.priority = ") + FormatFloat(c->Priority));
        outLines.push_back(std::string("TerrainSculptZone.blend = ") + std::to_string(static_cast<uint32>(c->Blend)));
        if (!c->PayloadRef.IsNull())
            outLines.push_back(std::string("TerrainSculptZone.payload = \"") + c->PayloadRef.ToGuid().ToString() + "\"");
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainSculptZone c{};
        if (const auto* existing = world.GetComponent<Components::TerrainSculptZone>(entity))
            c = *existing;

        if (property == "extentx")       { if (!ParseF32(value, c.ExtentX, outError)) return false; }
        else if (property == "extentz")  { if (!ParseF32(value, c.ExtentZ, outError)) return false; }
        else if (property == "falloff")  { if (!ParseF32(value, c.Falloff, outError)) return false; }
        else if (property == "priority") { if (!ParseF32(value, c.Priority, outError)) return false; }
        else if (property == "blend")
        {
            if (!ParseBlend(ctx, value, BlendSet::HeightOperators, c.Blend, outError)) return false;
        }
        else if (property == "payload")
        {
            SceneValue sv{};
            if (!ParseValue(value, sv, outError)) return false;
            if (sv.Kind == SceneValueKind::Int && sv.IntValue == 0)
                c.PayloadRef.Clear();
            else if (sv.Kind == SceneValueKind::String)
            {
                try { c.PayloadRef.Set(GUID(sv.StringValue)); }
                catch (...) { if (outError) *outError = "payload must be a GUID string"; return false; }
            }
            else { if (outError) *outError = "payload must be a GUID string"; return false; }
        }
        else { if (outError) *outError = "Unknown TerrainSculptZone property"; return false; }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    void EnumerateAssetReferences(const ECS::World& world, ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override
    {
        const auto* c = world.GetComponent<Components::TerrainSculptZone>(entity);
        if (!c || !visitor) return;
        const GUID g = c->PayloadRef.ToGuid();
        if (g.IsNull()) return;
        visitor(g, AssetType::TerrainZoneData, std::string_view{}, "payload");
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainSculptZone>(entity)) return true;
        world.AddComponentImmediate(entity, Components::TerrainSculptZone{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainSculptZone>(entity);
        return true;
    }
};

class TerrainPaintZoneSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainPaintZone"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainPaintZone>(entity);
        if (!c) return;
        outLines.push_back(std::string("TerrainPaintZone.extentX = ") + FormatFloat(c->ExtentX));
        outLines.push_back(std::string("TerrainPaintZone.extentZ = ") + FormatFloat(c->ExtentZ));
        outLines.push_back(std::string("TerrainPaintZone.falloff = ") + FormatFloat(c->Falloff));
        outLines.push_back(std::string("TerrainPaintZone.priority = ") + FormatFloat(c->Priority));
        outLines.push_back(std::string("TerrainPaintZone.layerIndex = ") + std::to_string(c->LayerIndex));
        outLines.push_back(std::string("TerrainPaintZone.strength = ") + FormatFloat(c->Strength));
        if (!c->PayloadRef.IsNull())
            outLines.push_back(std::string("TerrainPaintZone.payload = \"") + c->PayloadRef.ToGuid().ToString() + "\"");
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainPaintZone c{};
        if (const auto* existing = world.GetComponent<Components::TerrainPaintZone>(entity))
            c = *existing;

        if (property == "extentx")         { if (!ParseF32(value, c.ExtentX, outError)) return false; }
        else if (property == "extentz")    { if (!ParseF32(value, c.ExtentZ, outError)) return false; }
        else if (property == "falloff")    { if (!ParseF32(value, c.Falloff, outError)) return false; }
        else if (property == "priority")   { if (!ParseF32(value, c.Priority, outError)) return false; }
        else if (property == "layerindex") { if (!ParseU32(value, c.LayerIndex, outError)) return false; c.LayerIndex = std::min(c.LayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u); }
        else if (property == "strength")   { if (!ParseF32(value, c.Strength, outError)) return false; }
        else if (property == "payload")
        {
            SceneValue sv{};
            if (!ParseValue(value, sv, outError)) return false;
            if (sv.Kind == SceneValueKind::Int && sv.IntValue == 0)
                c.PayloadRef.Clear();
            else if (sv.Kind == SceneValueKind::String)
            {
                try { c.PayloadRef.Set(GUID(sv.StringValue)); }
                catch (...) { if (outError) *outError = "payload must be a GUID string"; return false; }
            }
            else { if (outError) *outError = "payload must be a GUID string"; return false; }
        }
        else { if (outError) *outError = "Unknown TerrainPaintZone property"; return false; }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    void EnumerateAssetReferences(const ECS::World& world, ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override
    {
        const auto* c = world.GetComponent<Components::TerrainPaintZone>(entity);
        if (!c || !visitor) return;
        const GUID g = c->PayloadRef.ToGuid();
        if (g.IsNull()) return;
        visitor(g, AssetType::TerrainZoneData, std::string_view{}, "payload");
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainPaintZone>(entity)) return true;
        world.AddComponentImmediate(entity, Components::TerrainPaintZone{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainPaintZone>(entity);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Modifier volume + effects (design §2a)
// ---------------------------------------------------------------------------

class TerrainModifierVolumeSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainModifierVolume"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainModifierVolume>(entity);
        if (!c) return;
        outLines.push_back(std::string("TerrainModifierVolume.shape = ") + std::to_string(static_cast<uint32>(c->Shape)));
        outLines.push_back(std::string("TerrainModifierVolume.radius = ") + FormatFloat(c->Radius));
        outLines.push_back(std::string("TerrainModifierVolume.rectHalfX = ") + FormatFloat(c->RectHalfX));
        outLines.push_back(std::string("TerrainModifierVolume.rectHalfZ = ") + FormatFloat(c->RectHalfZ));
        outLines.push_back(std::string("TerrainModifierVolume.falloff = ") + FormatFloat(c->Falloff));
        outLines.push_back(std::string("TerrainModifierVolume.falloffInward = ") + FormatFloat(c->FalloffInward));
        outLines.push_back(std::string("TerrainModifierVolume.stationSpacing = ") + FormatFloat(c->StationSpacing));
        outLines.push_back(std::string("TerrainModifierVolume.weight = ") + FormatFloat(c->Weight));
        outLines.push_back(std::string("TerrainModifierVolume.priority = ") + FormatFloat(c->Priority));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainModifierVolume c{};
        if (const auto* existing = world.GetComponent<Components::TerrainModifierVolume>(entity))
            c = *existing;

        if (property == "shape")
        {
            uint32 v = 0;
            if (!ParseU32(value, v, outError)) return false;
            if (v > static_cast<uint32>(Components::TerrainVolumeShape::Global))
            {
                if (outError) *outError = "shape must be 0-4";
                return false;
            }
            c.Shape = static_cast<Components::TerrainVolumeShape>(v);
        }
        else if (property == "radius")        { if (!ParseF32(value, c.Radius, outError)) return false; }
        else if (property == "recthalfx")     { if (!ParseF32(value, c.RectHalfX, outError)) return false; }
        else if (property == "recthalfz")     { if (!ParseF32(value, c.RectHalfZ, outError)) return false; }
        else if (property == "falloff")       { if (!ParseF32(value, c.Falloff, outError)) return false; }
        else if (property == "falloffinward") { if (!ParseF32(value, c.FalloffInward, outError)) return false; }
        else if (property == "stationspacing"){ if (!ParseF32(value, c.StationSpacing, outError)) return false; }
        else if (property == "weight")        { if (!ParseF32(value, c.Weight, outError)) return false; }
        else if (property == "priority")      { if (!ParseF32(value, c.Priority, outError)) return false; }
        else { if (outError) *outError = "Unknown TerrainModifierVolume property"; return false; }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainModifierVolume>(entity)) return true;
        world.AddComponentImmediate(entity, Components::TerrainModifierVolume{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainModifierVolume>(entity);
        return true;
    }
};

// The shared effect fields (StackOrder, Enabled) that every effect block carries.
template <class TEffect>
static void SerializeEffectBase(const TEffect& c, const char* prefix, std::vector<std::string>& outLines)
{
    const std::string p = std::string(prefix) + ".";
    outLines.push_back(p + "stackOrder = " + std::to_string(c.StackOrder));
    outLines.push_back(p + "enabled = " + (c.Enabled ? "true" : "false"));
}

// Parses a shared effect field; `handled` reports whether it was one, so callers
// can try their own fields next.
template <class TEffect>
static bool ApplyEffectBaseProperty(TEffect& c, std::string_view property, std::string_view value,
                                    std::string* outError, bool& handled)
{
    handled = true;
    if (property == "stackorder")
    {
        uint32 v = 0;
        if (!ParseU32(value, v, outError)) return false;
        c.StackOrder = static_cast<int32>(v);
        return true;
    }
    if (property == "enabled") return ParseBool(value, c.Enabled, outError);
    handled = false;
    return true;
}

// Writes a quoted pool-group name into the fixed field. Shared by every poolable
// effect's schema and by the retired corridor schema that folds into the
// flatten, so no authoring route can disagree about the bound.
template <typename TEffect>
static bool ApplyEffectPoolGroup(TEffect& c, std::string_view value,
                                 std::string* outError)
{
    std::string s;
    if (!ParseQuotedString(value, s))
    {
        if (outError) *outError = "poolGroup must be a quoted string";
        return false;
    }
    // A name longer than the field is REFUSED rather than truncated: truncation
    // would silently MERGE two pools whose first 31 characters agree, so it is
    // reported where the scene is loaded. The inspector's row refuses the same
    // edit for the same reason, so the two authoring routes agree.
    if (s.size() >= sizeof(c.PoolGroup))
    {
        if (outError)
            *outError = "poolGroup is longer than kTerrainPoolGroupCapacity - 1 characters";
        return false;
    }
    std::memset(c.PoolGroup, 0, sizeof(c.PoolGroup));
    std::memcpy(c.PoolGroup, s.data(), s.size());
    return true;
}

// The flatten slot a retired corridor's fold owns for the current load. Shared by
// the flatten's own schema and the retired corridor schema so the route-wins rule
// cannot depend on which block the file lists first: the fold claims the slot,
// and the flatten schema drops later blocks for a claimed slot.
static SceneLoadContext::RetiredFoldClaim* FindFlattenFoldClaim(const SceneLoadContext& ctx,
                                                                ECS::EntityHandle entity)
{
    const auto byName = ctx.RetiredFoldClaims.find("TerrainFlattenEffect");
    if (byName == ctx.RetiredFoldClaims.end())
        return nullptr;
    const auto byEntity = byName->second.find(entity.id);
    return byEntity == byName->second.end() ? nullptr : &byEntity->second;
}

static void ClaimFlattenFoldSlot(const SceneLoadContext& ctx, ECS::EntityHandle entity)
{
    ctx.RetiredFoldClaims["TerrainFlattenEffect"][entity.id];
}

class TerrainFlattenEffectSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainFlattenEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainFlattenEffect>(entity);
        if (!c) return;
        SerializeEffectBase(*c, "TerrainFlattenEffect", outLines);
        outLines.push_back(std::string("TerrainFlattenEffect.targetHeight = ") + FormatFloat(c->TargetHeight));
        outLines.push_back(std::string("TerrainFlattenEffect.useVolumeHeight = ") + (c->UseVolumeHeight ? "true" : "false"));
        outLines.push_back(std::string("TerrainFlattenEffect.blend = ") + std::to_string(static_cast<uint32>(c->Blend)));
        outLines.push_back(std::string("TerrainFlattenEffect.blendSmoothing = ") + FormatFloat(c->BlendSmoothing));
        // The pool group is authored and read as TEXT. It resolves to a StringId
        // at gather, but a hash in the scene file would be unreadable in review
        // and unwritable by the content tools that emit these entities.
        outLines.push_back(std::string("TerrainFlattenEffect.poolGroup = ")
                           + FormatQuoted(Components::EffectPoolName(*c)));
        outLines.push_back(std::string("TerrainFlattenEffect.respectClaims = ") + (c->RespectClaims ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        // A retired corridor's fold owns this slot for the rest of the load: the
        // entity carried both components, the route won, and applying this block
        // would hand the slot back. Dropped with one warning, not an error — the
        // scene still loads, exactly as the route-wins warning says it does.
        if (auto* claim = FindFlattenFoldClaim(ctx, entity))
        {
            if (!claim->Warned)
            {
                Logger::Log::Warning(
                    "Scene: entity {} carries both a retired TerrainCorridorEffect and a "
                    "TerrainFlattenEffect. One volume holds one flatten, so the route wins — the "
                    "TerrainFlattenEffect block is dropped. Move one of them to its own volume.",
                    entity.id);
                claim->Warned = true;
            }
            return true;
        }

        Components::TerrainFlattenEffect c{};
        if (const auto* existing = world.GetComponent<Components::TerrainFlattenEffect>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyEffectBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            if (property == "targetheight")         { if (!ParseF32(value, c.TargetHeight, outError)) return false; }
            else if (property == "usevolumeheight") { if (!ParseBool(value, c.UseVolumeHeight, outError)) return false; }
            else if (property == "blend")           { if (!ParseBlend(ctx, value, BlendSet::HeightOperatorsAndPool, c.Blend, outError)) return false; }
            else if (property == "blendsmoothing")  { if (!ParseF32(value, c.BlendSmoothing, outError)) return false; }
            else if (property == "poolgroup")       { if (!ApplyEffectPoolGroup(c, value, outError)) return false; }
            else if (property == "respectclaims")   { if (!ParseBool(value, c.RespectClaims, outError)) return false; }
            else { if (outError) *outError = "Unknown TerrainFlattenEffect property"; return false; }
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainFlattenEffect>(entity)) return true;
        Components::TerrainFlattenEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        world.AddComponentImmediate(entity, fx);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainFlattenEffect>(entity);
        return true;
    }
};

// Once per scene file rather than once per corridor or once per property: a road
// network can hold dozens of routes and one report is the whole story, so the
// entity named is the first one the load reached. Same shape, and same reason, as
// WarnRetiredSmoothBlend above. A load with no scene file (a tool or test
// applying a single component block) has no file to key on, so it always reports.
static void WarnRetiredCorridorEffect(const SceneLoadContext& ctx, ECS::EntityHandle entity)
{
    if (ctx.SceneFile != nullptr)
    {
        static std::mutex mutex;
        static std::unordered_set<std::string> warned;
        std::lock_guard<std::mutex> lock(mutex);
        if (!warned.emplace(ctx.SceneFile->string()).second)
            return;
        Logger::Log::Warning(
            "Scene '{}': TerrainCorridorEffect is retired (entity {}, and any other route in this "
            "scene). It loads as a Flatten effect whose blend is Average — the same pooled grade, "
            "on the volume's own geometry — with its station spacing on the Terrain Modifier "
            "Volume. Saving rewrites the scene in that form.",
            ctx.SceneFile->string(), entity.id);
        return;
    }
    Logger::Log::Warning(
        "Scene: TerrainCorridorEffect is retired (entity {}). It loads as a Flatten effect whose "
        "blend is Average — the same pooled grade, on the volume's own geometry — with its station "
        "spacing on the Terrain Modifier Volume. Saving rewrites the scene in that form.",
        entity.id);
}

// The flatten a retired corridor folds into, at whatever stage of the fold the
// entity has reached.
//
// Seeded with the CORRIDOR's defaults, not the flatten's: text that omits a
// property meant the corridor's default, and the two components disagree about
// two of them — a corridor followed its route's own heights and stopped at
// claimed ground unless told otherwise, while a flatten defaults to neither.
//
// The load's claim on the flatten slot is what says a flatten already on the
// entity is this same fold in progress. An UNCLAIMED existing flatten — any
// blend, Average included — was authored, and the entity carried a corridor AND
// a flatten, which one component of a type per entity cannot express: the route
// wins, loudly. The claim also covers the reverse block order: the flatten's own
// schema drops its block on a claimed slot, so the outcome does not depend on
// which block the file lists first.
static Components::TerrainFlattenEffect FoldedCorridorFlatten(const ECS::World& world,
                                                              ECS::EntityHandle entity,
                                                              bool foldInProgress)
{
    const auto* existing = world.GetComponent<Components::TerrainFlattenEffect>(entity);
    if (existing && foldInProgress)
        return *existing;

    if (existing)
        Logger::Log::Warning(
            "Scene: entity {} carries both a retired TerrainCorridorEffect and a "
            "TerrainFlattenEffect. One volume holds one flatten, so the route replaces it — the "
            "flatten's target height {} is dropped. Move one of them to its own volume.",
            entity.id, existing->TargetHeight);

    Components::TerrainFlattenEffect c{};
    c.Blend = Components::TerrainModifierBlend::Average;
    c.UseVolumeHeight = true;  // the corridor's UseRouteHeight default
    c.RespectClaims = true;    // the corridor's RespectClaims default
    return c;
}

// TerrainCorridorEffect is RETIRED: the corridor dissolved into a Flatten effect
// whose blend is Average, plus the volume's own StationSpacing. The schema keeps
// the retired component NAME so scene text written before the dissolution still
// parses, and translates it on the way in.
//
// It serializes NOTHING. The entity holds a flatten after the fold, and the
// flatten's own schema writes it — which is exactly what makes a load-then-save
// round trip carry the route forward in the new form instead of dropping it.
//
// The same shape as the retired Smooth blend (WarnRetiredSmoothBlend) and the
// retired Terrain properties (WarnRetiredTerrainProperty): a loader translation
// of old text, not a compatibility path in the engine. No TerrainCorridorEffect
// component type exists, nothing in the bake branches on one, and a re-saved
// scene contains no trace of it.
class TerrainCorridorEffectSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainCorridorEffect"; }

    void Serialize(const ECS::World& /*world*/, ECS::EntityHandle /*entity*/,
                   const SceneSaveContext& /*ctx*/,
                   std::vector<std::string>& /*outLines*/) const override
    {
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        // stationSpacing is GEOMETRY and belongs to the volume, so it lands there
        // rather than on the effect. Fetch-or-default because a saved scene writes
        // component blocks in name order and TerrainCorridorEffect sorts BEFORE
        // TerrainModifierVolume: the volume block applies on top of this one and
        // leaves the spacing alone, so the value survives either arrival order.
        if (property == "stationspacing")
        {
            Components::TerrainModifierVolume vol{};
            if (const auto* existing = world.GetComponent<Components::TerrainModifierVolume>(entity))
                vol = *existing;
            if (!ParseF32(value, vol.StationSpacing, outError)) return false;
            WarnRetiredCorridorEffect(ctx, entity);
            world.AddComponentImmediate(entity, vol);
            return true;
        }

        Components::TerrainFlattenEffect c =
            FoldedCorridorFlatten(world, entity, FindFlattenFoldClaim(ctx, entity) != nullptr);

        bool handled = false;
        if (!ApplyEffectBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            // The corridor's field names, mapped onto the flatten's. Its blend was
            // never authored — every corridor pooled — so the fold pins Average.
            if (property == "group")               { if (!ApplyEffectPoolGroup(c, value, outError)) return false; }
            else if (property == "targetheight")   { if (!ParseF32(value, c.TargetHeight, outError)) return false; }
            else if (property == "userouteheight") { if (!ParseBool(value, c.UseVolumeHeight, outError)) return false; }
            else if (property == "respectclaims")  { if (!ParseBool(value, c.RespectClaims, outError)) return false; }
            else { if (outError) *outError = "Unknown TerrainCorridorEffect property"; return false; }
        }

        WarnRetiredCorridorEffect(ctx, entity);
        ClaimFlattenFoldSlot(ctx, entity);
        world.AddComponentImmediate(entity, c);
        return true;
    }

    // A retired component is never ADDED — the picker offers the flatten instead —
    // and there is nothing on the entity for a Remove to take off.
    bool AddDefault(ECS::World& /*world*/, ECS::EntityHandle /*entity*/,
                    std::string* outError) const override
    {
        if (outError)
            *outError = "TerrainCorridorEffect is retired. Add a Flatten effect and set its blend "
                        "to Average to pool it with the other routes.";
        return false;
    }

    bool Remove(ECS::World& /*world*/, ECS::EntityHandle /*entity*/) const override
    {
        return true;
    }
};

class TerrainGroundClaimEffectSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainGroundClaimEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainGroundClaimEffect>(entity);
        if (!c) return;
        SerializeEffectBase(*c, "TerrainGroundClaimEffect", outLines);
        outLines.push_back(std::string("TerrainGroundClaimEffect.strength = ") + FormatFloat(c->Strength));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainGroundClaimEffect c{};
        if (const auto* existing = world.GetComponent<Components::TerrainGroundClaimEffect>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyEffectBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            if (property == "strength") { if (!ParseF32(value, c.Strength, outError)) return false; }
            else { if (outError) *outError = "Unknown TerrainGroundClaimEffect property"; return false; }
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainGroundClaimEffect>(entity)) return true;
        Components::TerrainGroundClaimEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        world.AddComponentImmediate(entity, fx);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainGroundClaimEffect>(entity);
        return true;
    }
};

class TerrainGrassEffectSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainGrassEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   const SceneSaveContext&, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainGrassEffect>(entity);
        if (!c) return;
        SerializeEffectBase(*c, "TerrainGrassEffect", outLines);
        outLines.push_back(std::string("TerrainGrassEffect.heightScale = ") + FormatFloat(c->HeightScale));
        outLines.push_back(std::string("TerrainGrassEffect.densityScale = ") + FormatFloat(c->DensityScale));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity,
                       const SceneLoadContext&, std::string_view property,
                       std::string_view value, std::string* outError) const override
    {
        Components::TerrainGrassEffect c{};
        if (const auto* existing = world.GetComponent<Components::TerrainGrassEffect>(entity)) c = *existing;
        bool handled = false;
        if (!ApplyEffectBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            float32* field = property == "heightscale" ? &c.HeightScale :
                             property == "densityscale" ? &c.DensityScale : nullptr;
            if (!field) { if (outError) *outError = "Unknown TerrainGrassEffect property"; return false; }
            if (!ParseF32(value, *field, outError)) return false;
            if (!std::isfinite(*field)) { if (outError) *outError = "Grass scale must be finite"; return false; }
            *field = Components::ClampTerrainGrassEffectScale(*field);
        }
        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string*) const override
    {
        if (world.GetComponent<Components::TerrainGrassEffect>(entity)) return true;
        Components::TerrainGrassEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        world.AddComponentImmediate(entity, fx);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainGrassEffect>(entity);
        return true;
    }
};

class TerrainHeightOffsetEffectSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainHeightOffsetEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainHeightOffsetEffect>(entity);
        if (!c) return;
        SerializeEffectBase(*c, "TerrainHeightOffsetEffect", outLines);
        outLines.push_back(std::string("TerrainHeightOffsetEffect.offset = ") + FormatFloat(c->Offset));
        outLines.push_back(std::string("TerrainHeightOffsetEffect.blend = ") + std::to_string(static_cast<uint32>(c->Blend)));
        outLines.push_back(std::string("TerrainHeightOffsetEffect.blendSmoothing = ") + FormatFloat(c->BlendSmoothing));
        outLines.push_back(std::string("TerrainHeightOffsetEffect.poolGroup = ")
                           + FormatQuoted(Components::EffectPoolName(*c)));
        outLines.push_back(std::string("TerrainHeightOffsetEffect.respectClaims = ") + (c->RespectClaims ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainHeightOffsetEffect c{};
        if (const auto* existing = world.GetComponent<Components::TerrainHeightOffsetEffect>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyEffectBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            if (property == "offset") { if (!ParseF32(value, c.Offset, outError)) return false; }
            else if (property == "blend")
            {
                if (!ParseBlend(ctx, value, BlendSet::HeightOperatorsAndPool, c.Blend, outError)) return false;
            }
            else if (property == "blendsmoothing") { if (!ParseF32(value, c.BlendSmoothing, outError)) return false; }
            else if (property == "poolgroup") { if (!ApplyEffectPoolGroup(c, value, outError)) return false; }
            else if (property == "respectclaims") { if (!ParseBool(value, c.RespectClaims, outError)) return false; }
            else { if (outError) *outError = "Unknown TerrainHeightOffsetEffect property"; return false; }
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainHeightOffsetEffect>(entity)) return true;
        Components::TerrainHeightOffsetEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        world.AddComponentImmediate(entity, fx);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainHeightOffsetEffect>(entity);
        return true;
    }
};

class TerrainNoiseEffectSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainNoiseEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainNoiseEffect>(entity);
        if (!c) return;
        SerializeEffectBase(*c, "TerrainNoiseEffect", outLines);
        outLines.push_back(std::string("TerrainNoiseEffect.blend = ") + std::to_string(static_cast<uint32>(c->Blend)));
        outLines.push_back(std::string("TerrainNoiseEffect.frequency = ") + FormatFloat(c->Frequency));
        outLines.push_back(std::string("TerrainNoiseEffect.amplitude = ") + FormatFloat(c->Amplitude));
        outLines.push_back(std::string("TerrainNoiseEffect.octaves = ") + std::to_string(c->Octaves));
        outLines.push_back(std::string("TerrainNoiseEffect.seed = ") + std::to_string(c->Seed));
        outLines.push_back(std::string("TerrainNoiseEffect.lacunarity = ") + FormatFloat(c->Lacunarity));
        outLines.push_back(std::string("TerrainNoiseEffect.persistence = ") + FormatFloat(c->Persistence));
        outLines.push_back(std::string("TerrainNoiseEffect.blendSmoothing = ") + FormatFloat(c->BlendSmoothing));
        outLines.push_back(std::string("TerrainNoiseEffect.erosionStrength = ") + FormatFloat(c->ErosionStrength));
        outLines.push_back(std::string("TerrainNoiseEffect.erosionOctaves = ") + std::to_string(c->ErosionOctaves));
        outLines.push_back(std::string("TerrainNoiseEffect.erosionFrequency = ") + FormatFloat(c->ErosionFrequency));
        outLines.push_back(std::string("TerrainNoiseEffect.erosionDetail = ") + FormatFloat(c->ErosionDetail));
        outLines.push_back(std::string("TerrainNoiseEffect.erosionGullyWeight = ") + FormatFloat(c->ErosionGullyWeight));
        outLines.push_back(std::string("TerrainNoiseEffect.erosionEdgeRounding = ") + FormatFloat(c->ErosionEdgeRounding));
        outLines.push_back(std::string("TerrainNoiseEffect.erosionFade = ") + FormatFloat(c->ErosionFade));
        outLines.push_back(std::string("TerrainNoiseEffect.poolGroup = ")
                           + FormatQuoted(Components::EffectPoolName(*c)));
        outLines.push_back(std::string("TerrainNoiseEffect.respectClaims = ") + (c->RespectClaims ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainNoiseEffect c{};
        if (const auto* existing = world.GetComponent<Components::TerrainNoiseEffect>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyEffectBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            if (property == "blend")
            {
                if (!ParseBlend(ctx, value, BlendSet::HeightOperatorsAndPool, c.Blend, outError)) return false;
            }
            else if (property == "frequency")   { if (!ParseF32(value, c.Frequency, outError)) return false; }
            else if (property == "amplitude")   { if (!ParseF32(value, c.Amplitude, outError)) return false; }
            else if (property == "octaves")     { if (!ParseU32(value, c.Octaves, outError)) return false; }
            else if (property == "seed")        { if (!ParseU32(value, c.Seed, outError)) return false; }
            else if (property == "lacunarity")  { if (!ParseF32(value, c.Lacunarity, outError)) return false; }
            else if (property == "persistence") { if (!ParseF32(value, c.Persistence, outError)) return false; }
            else if (property == "blendsmoothing") { if (!ParseF32(value, c.BlendSmoothing, outError)) return false; }
            else if (property == "erosionstrength")     { if (!ParseF32(value, c.ErosionStrength, outError)) return false; }
            else if (property == "erosionoctaves")      { if (!ParseU32(value, c.ErosionOctaves, outError)) return false; }
            else if (property == "erosionfrequency")    { if (!ParseF32(value, c.ErosionFrequency, outError)) return false; }
            else if (property == "erosiondetail")       { if (!ParseF32(value, c.ErosionDetail, outError)) return false; }
            else if (property == "erosiongullyweight")  { if (!ParseF32(value, c.ErosionGullyWeight, outError)) return false; }
            else if (property == "erosionedgerounding") { if (!ParseF32(value, c.ErosionEdgeRounding, outError)) return false; }
            else if (property == "erosionfade")         { if (!ParseF32(value, c.ErosionFade, outError)) return false; }
            else if (property == "poolgroup")     { if (!ApplyEffectPoolGroup(c, value, outError)) return false; }
            else if (property == "respectclaims") { if (!ParseBool(value, c.RespectClaims, outError)) return false; }
            else { if (outError) *outError = "Unknown TerrainNoiseEffect property"; return false; }
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainNoiseEffect>(entity)) return true;
        Components::TerrainNoiseEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        world.AddComponentImmediate(entity, fx);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainNoiseEffect>(entity);
        return true;
    }
};

class TerrainStampEffectSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainStampEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainStampEffect>(entity);
        if (!c) return;
        SerializeEffectBase(*c, "TerrainStampEffect", outLines);
        outLines.push_back(std::string("TerrainStampEffect.blend = ") + std::to_string(static_cast<uint32>(c->Blend)));
        outLines.push_back(std::string("TerrainStampEffect.heightScale = ") + FormatFloat(c->HeightScale));
        outLines.push_back(std::string("TerrainStampEffect.rotation = ") + FormatFloat(c->Rotation));
        outLines.push_back(std::string("TerrainStampEffect.blendSmoothing = ") + FormatFloat(c->BlendSmoothing));
        outLines.push_back(std::string("TerrainStampEffect.poolGroup = ")
                           + FormatQuoted(Components::EffectPoolName(*c)));
        outLines.push_back(std::string("TerrainStampEffect.respectClaims = ") + (c->RespectClaims ? "true" : "false"));
        if (!c->StampAssetGuid.IsNull())
            outLines.push_back(std::string("TerrainStampEffect.stampAsset = \"") + c->StampAssetGuid.ToGuid().ToString() + "\"");
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainStampEffect c{};
        if (const auto* existing = world.GetComponent<Components::TerrainStampEffect>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyEffectBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            if (property == "blend")
            {
                if (!ParseBlend(ctx, value, BlendSet::HeightOperatorsAndPool, c.Blend, outError)) return false;
            }
            else if (property == "heightscale") { if (!ParseF32(value, c.HeightScale, outError)) return false; }
            else if (property == "rotation")    { if (!ParseF32(value, c.Rotation, outError)) return false; }
            else if (property == "blendsmoothing") { if (!ParseF32(value, c.BlendSmoothing, outError)) return false; }
            else if (property == "stampasset")
            {
                SceneValue sv{};
                if (!ParseValue(value, sv, outError)) return false;
                if (sv.Kind == SceneValueKind::Int && sv.IntValue == 0)
                    c.StampAssetGuid.Clear();
                else if (sv.Kind == SceneValueKind::String)
                {
                    try { c.StampAssetGuid.Set(GUID(sv.StringValue)); }
                    catch (...) { if (outError) *outError = "stampAsset must be a GUID string"; return false; }
                }
                else { if (outError) *outError = "stampAsset must be a GUID string"; return false; }
            }
            else if (property == "poolgroup")     { if (!ApplyEffectPoolGroup(c, value, outError)) return false; }
            else if (property == "respectclaims") { if (!ParseBool(value, c.RespectClaims, outError)) return false; }
            else { if (outError) *outError = "Unknown TerrainStampEffect property"; return false; }
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    void EnumerateAssetReferences(const ECS::World& world, ECS::EntityHandle entity,
                                  const AssetRefVisitor& visitor) const override
    {
        const auto* c = world.GetComponent<Components::TerrainStampEffect>(entity);
        if (!c || !visitor) return;
        const GUID g = c->StampAssetGuid.ToGuid();
        if (g.IsNull()) return;
        // Masks are Texture assets by contract.
        visitor(g, AssetType::Texture, std::string_view{}, "stampasset");
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainStampEffect>(entity)) return true;
        Components::TerrainStampEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        world.AddComponentImmediate(entity, fx);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainStampEffect>(entity);
        return true;
    }
};

class TerrainPaintLayerEffectSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainPaintLayerEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainPaintLayerEffect>(entity);
        if (!c) return;
        SerializeEffectBase(*c, "TerrainPaintLayerEffect", outLines);
        outLines.push_back(std::string("TerrainPaintLayerEffect.layerIndex = ") + std::to_string(c->LayerIndex));
        outLines.push_back(std::string("TerrainPaintLayerEffect.strength = ") + FormatFloat(c->Strength));
        outLines.push_back(std::string("TerrainPaintLayerEffect.replace = ") + (c->Replace ? "true" : "false"));
        outLines.push_back(std::string("TerrainPaintLayerEffect.respectClaims = ") + (c->RespectClaims ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainPaintLayerEffect c{};
        if (const auto* existing = world.GetComponent<Components::TerrainPaintLayerEffect>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyEffectBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled)
        {
            if (property == "layerindex")    { if (!ParseU32(value, c.LayerIndex, outError)) return false; c.LayerIndex = std::min(c.LayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u); }
            else if (property == "strength") { if (!ParseF32(value, c.Strength, outError)) return false; }
            else if (property == "replace")  { if (!ParseBool(value, c.Replace, outError)) return false; }
            else if (property == "respectclaims") { if (!ParseBool(value, c.RespectClaims, outError)) return false; }
            else { if (outError) *outError = "Unknown TerrainPaintLayerEffect property"; return false; }
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainPaintLayerEffect>(entity)) return true;
        Components::TerrainPaintLayerEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        world.AddComponentImmediate(entity, fx);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainPaintLayerEffect>(entity);
        return true;
    }
};

// Peel one `<name><index>.` segment off the front of a property path. Returns
// false when the segment is not `name` followed by digits and a dot; on success
// `outIndex` is the digits and `property` advances past the dot.
static bool PeelIndexedSegment(std::string_view& property, std::string_view name, uint32& outIndex)
{
    if (property.size() <= name.size() || property.compare(0, name.size(), name) != 0)
        return false;
    std::size_t i = name.size();
    if (!std::isdigit(static_cast<unsigned char>(property[i])))
        return false;
    // Accumulated wide and refused on overflow rather than wrapped: a uint32
    // that wraps addresses a DIFFERENT row than the file names (rule4294967296
    // becomes rule0), which writes authored values into the wrong rule instead
    // of reporting the key as bad.
    uint64 index = 0;
    while (i < property.size() && std::isdigit(static_cast<unsigned char>(property[i])))
    {
        index = index * 10u + static_cast<uint64>(property[i] - '0');
        if (index > std::numeric_limits<uint32>::max())
            return false;
        ++i;
    }
    if (i >= property.size() || property[i] != '.')
        return false;
    outIndex = static_cast<uint32>(index);
    property.remove_prefix(i + 1);
    return true;
}

// Rule rows serialize as indexed keys (rule0.condition1.min), the same shape the
// spline schema uses for its points. That reintroduces the numbered-field style
// the authoring canonical rejected as agent-hostile, and does so knowingly: the
// alternative — one child entity per row — turns a four-row rule set into four
// entities, which is worse for the inspector, the gather and the scene diff.
class TerrainSurfaceRulesEffectSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainSurfaceRulesEffect"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainSurfaceRulesEffect>(entity);
        if (!c) return;
        SerializeEffectBase(*c, "TerrainSurfaceRulesEffect", outLines);

        // Only LIVE rows and conditions are written: the fixed-capacity tail is
        // storage, not authored content, and writing it would put eight rules in
        // every scene diff that touched one.
        const uint32 ruleCount = std::min(c->RuleCount, Components::kMaxTerrainSurfaceRules);
        outLines.push_back("TerrainSurfaceRulesEffect.ruleCount = " + std::to_string(ruleCount));
        outLines.push_back(std::string("TerrainSurfaceRulesEffect.respectClaims = ")
                           + (c->RespectClaims ? "true" : "false"));

        for (uint32 r = 0; r < ruleCount; ++r)
        {
            const Components::TerrainSurfaceRule& rule = c->Rules[r];
            const std::string p = "TerrainSurfaceRulesEffect.rule" + std::to_string(r) + ".";
            outLines.push_back(p + "material = " + std::to_string(rule.MaterialSlot));
            outLines.push_back(p + "strength = " + FormatFloat(rule.Strength));
            outLines.push_back(p + "replace = " + (rule.Replace ? "true" : "false"));

            const uint32 conditionCount = std::min<uint32>(rule.ConditionCount,
                                                           Components::kMaxTerrainRuleConditions);
            outLines.push_back(p + "conditionCount = " + std::to_string(conditionCount));
            for (uint32 i = 0; i < conditionCount; ++i)
            {
                const Components::TerrainRuleCondition& cond = rule.Conditions[i];
                const std::string cp = p + "condition" + std::to_string(i) + ".";
                outLines.push_back(cp + "kind = " + std::to_string(static_cast<uint32>(cond.Kind)));
                outLines.push_back(cp + "curve = "
                                   + std::to_string(static_cast<uint32>(cond.FalloffCurve)));
                outLines.push_back(cp + "min = " + FormatFloat(cond.Min));
                outLines.push_back(cp + "max = " + FormatFloat(cond.Max));
                outLines.push_back(cp + "feather = " + FormatFloat(cond.Feather));
                // Noise parameters only where they mean something, so a slope
                // band's scene block does not carry a frequency nobody reads.
                if (cond.Kind == Components::TerrainRuleConditionKind::Noise)
                {
                    outLines.push_back(cp + "noiseFrequency = " + FormatFloat(cond.NoiseFrequency));
                    outLines.push_back(cp + "noiseSeed = " + std::to_string(cond.NoiseSeed));
                }
            }
        }
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainSurfaceRulesEffect c{};
        if (const auto* existing = world.GetComponent<Components::TerrainSurfaceRulesEffect>(entity))
            c = *existing;

        bool handled = false;
        if (!ApplyEffectBaseProperty(c, property, value, outError, handled)) return false;
        if (!handled && !ApplyRuleProperty(c, property, value, outError))
            return false;

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainSurfaceRulesEffect>(entity)) return true;
        // No rows: an empty rule list is the honest starting point, and the row
        // the author adds first is unconditional, so it is visible immediately.
        Components::TerrainSurfaceRulesEffect fx{};
        fx.StackOrder = NextEffectStackOrder(world, entity);
        world.AddComponentImmediate(entity, fx);
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainSurfaceRulesEffect>(entity);
        return true;
    }

private:
    // Overflow is a hard load error, not a clamp. A rule set that silently lost
    // its last rows would bake a terrain that looks nearly right, which is the
    // expensive way to find out; the message names the cap and the offender.
    static bool ApplyRuleProperty(Components::TerrainSurfaceRulesEffect& c,
                                  std::string_view property, std::string_view value,
                                  std::string* outError)
    {
        if (property == "rulecount")
        {
            uint32 v = 0;
            if (!ParseU32(value, v, outError)) return false;
            if (v > Components::kMaxTerrainSurfaceRules)
            {
                if (outError)
                    *outError = "TerrainSurfaceRulesEffect.ruleCount " + std::to_string(v)
                              + " exceeds the cap of "
                              + std::to_string(Components::kMaxTerrainSurfaceRules)
                              + "; split the rule set across a second rules modifier";
                return false;
            }
            c.RuleCount = v;
            return true;
        }
        if (property == "respectclaims")
            return ParseBool(value, c.RespectClaims, outError);

        uint32 ruleIndex = 0;
        if (!PeelIndexedSegment(property, "rule", ruleIndex))
        {
            if (outError) *outError = "Unknown TerrainSurfaceRulesEffect property";
            return false;
        }
        if (ruleIndex >= Components::kMaxTerrainSurfaceRules)
        {
            if (outError)
                *outError = "TerrainSurfaceRulesEffect rule index " + std::to_string(ruleIndex)
                          + " exceeds the cap of "
                          + std::to_string(Components::kMaxTerrainSurfaceRules) + " rules";
            return false;
        }
        Components::TerrainSurfaceRule& rule = c.Rules[ruleIndex];

        uint32 conditionIndex = 0;
        if (!PeelIndexedSegment(property, "condition", conditionIndex))
        {
            // Clamped, matching the sibling paint schemas: a slot past the
            // channel count is a bad value, not a bad file, and the bake clamps
            // it the same way.
            if (property == "material")
            {
                if (!ParseU32(value, rule.MaterialSlot, outError)) return false;
                rule.MaterialSlot =
                    std::min(rule.MaterialSlot, Terrain::kMaxTerrainMaterialLayers - 1u);
                return true;
            }
            if (property == "strength")  return ParseF32(value, rule.Strength, outError);
            if (property == "replace")   return ParseBool(value, rule.Replace, outError);
            if (property == "conditioncount")
            {
                uint32 v = 0;
                if (!ParseU32(value, v, outError)) return false;
                if (v > Components::kMaxTerrainRuleConditions)
                {
                    if (outError)
                        *outError = "TerrainSurfaceRulesEffect rule " + std::to_string(ruleIndex)
                                  + " conditionCount " + std::to_string(v) + " exceeds the cap of "
                                  + std::to_string(Components::kMaxTerrainRuleConditions);
                    return false;
                }
                rule.ConditionCount = static_cast<uint8>(v);
                return true;
            }
            if (outError) *outError = "Unknown TerrainSurfaceRulesEffect rule property";
            return false;
        }
        if (conditionIndex >= Components::kMaxTerrainRuleConditions)
        {
            if (outError)
                *outError = "TerrainSurfaceRulesEffect rule " + std::to_string(ruleIndex)
                          + " condition index " + std::to_string(conditionIndex)
                          + " exceeds the cap of "
                          + std::to_string(Components::kMaxTerrainRuleConditions);
            return false;
        }
        Components::TerrainRuleCondition& cond = rule.Conditions[conditionIndex];

        if (property == "kind")
        {
            uint32 v = 0;
            if (!ParseU32(value, v, outError)) return false;
            if (v > static_cast<uint32>(Components::TerrainRuleConditionKind::Noise))
            {
                if (outError) *outError = "Unknown TerrainRuleConditionKind " + std::to_string(v);
                return false;
            }
            cond.Kind = static_cast<Components::TerrainRuleConditionKind>(v);
            return true;
        }
        if (property == "curve")
        {
            uint32 v = 0;
            if (!ParseU32(value, v, outError)) return false;
            if (v > static_cast<uint32>(Components::TerrainRuleFalloffCurve::Smoothstep))
            {
                if (outError) *outError = "Unknown TerrainRuleFalloffCurve " + std::to_string(v);
                return false;
            }
            cond.FalloffCurve = static_cast<Components::TerrainRuleFalloffCurve>(v);
            return true;
        }
        if (property == "min")            return ParseF32(value, cond.Min, outError);
        if (property == "max")            return ParseF32(value, cond.Max, outError);
        if (property == "feather")        return ParseF32(value, cond.Feather, outError);
        if (property == "noisefrequency") return ParseF32(value, cond.NoiseFrequency, outError);
        if (property == "noiseseed")      return ParseU32(value, cond.NoiseSeed, outError);

        if (outError) *outError = "Unknown TerrainSurfaceRulesEffect condition property";
        return false;
    }
};

GE_REGISTER_SCENE_SCHEMA(TerrainModifierVolumeSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainFlattenEffectSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainHeightOffsetEffectSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainNoiseEffectSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainStampEffectSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainPaintLayerEffectSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainSurfaceRulesEffectSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainCorridorEffectSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainGroundClaimEffectSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainGrassEffectSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainFlattenModifierSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainNoiseModifierSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainStampModifierSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainPaintLayerModifierSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainSplineModifierSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainSculptZoneSchema)
GE_REGISTER_SCENE_SCHEMA(TerrainPaintZoneSchema)

} // namespace

namespace
{
static void RegisterIfMissing(std::unique_ptr<ISceneComponentSchema> schema)
{
    if (SceneSchemaRegistry::HasRegistered(schema->GetComponentName()))
        return;
    SceneSchemaRegistry::Register(std::move(schema));
}
} // namespace

void EnsureTerrainSceneSchemasRegistered()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        RegisterIfMissing(std::make_unique<TerrainSchema>());
        RegisterIfMissing(std::make_unique<TerrainCBTMigrationSchema>());
        RegisterIfMissing(std::make_unique<TerrainPlanetReliefSchema>());
        RegisterIfMissing(std::make_unique<TerrainModifierVolumeSchema>());
        RegisterIfMissing(std::make_unique<TerrainFlattenEffectSchema>());
        RegisterIfMissing(std::make_unique<TerrainHeightOffsetEffectSchema>());
        RegisterIfMissing(std::make_unique<TerrainNoiseEffectSchema>());
        RegisterIfMissing(std::make_unique<TerrainStampEffectSchema>());
        RegisterIfMissing(std::make_unique<TerrainPaintLayerEffectSchema>());
        RegisterIfMissing(std::make_unique<TerrainSurfaceRulesEffectSchema>());
        RegisterIfMissing(std::make_unique<TerrainCorridorEffectSchema>());
        RegisterIfMissing(std::make_unique<TerrainGroundClaimEffectSchema>());
        RegisterIfMissing(std::make_unique<TerrainGrassEffectSchema>());
        RegisterIfMissing(std::make_unique<TerrainFlattenModifierSchema>());
        RegisterIfMissing(std::make_unique<TerrainNoiseModifierSchema>());
        RegisterIfMissing(std::make_unique<TerrainStampModifierSchema>());
        RegisterIfMissing(std::make_unique<TerrainPaintLayerModifierSchema>());
        RegisterIfMissing(std::make_unique<TerrainSplineModifierSchema>());
        RegisterIfMissing(std::make_unique<TerrainSculptZoneSchema>());
        RegisterIfMissing(std::make_unique<TerrainPaintZoneSchema>());
    });
}

} // namespace GameEngine::Scene
