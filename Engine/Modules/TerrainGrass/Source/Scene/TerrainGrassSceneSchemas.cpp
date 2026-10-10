#include "TerrainGrass/Scene/TerrainGrassSceneSchemas.h"

#include "AssetCore/GUID.h"
#include "Components/Terrain/TerrainGrass.h"
#include "ECS/ECSTemplates.h"
#include "Logger/Logger.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"
#include "TerrainGrass/TerrainGrassClamps.h"

#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

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

static void SerializeGuid(std::vector<std::string>& outLines,
                          std::string_view property,
                          const GUID& guid)
{
    if (guid.IsNull())
        return;
    outLines.push_back(std::string(property) + " = &{" + guid.ToString() + "}");
}

static bool ParseGuid(std::string_view value, GUID& outGuid, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;

    std::string guidText;
    if (v.Kind == SceneValueKind::GuidRef)
        guidText = v.StringValue;
    else if (v.Kind == SceneValueKind::String)
        guidText = v.StringValue;
    else if (v.Kind == SceneValueKind::AssetRef)
        guidText = std::string(AssetRefGuid(v));
    else
    {
        if (outError)
            *outError = "Expected a GUID reference";
        return false;
    }

    outGuid = guidText.empty() ? GUID::Null() : GUID(guidText);
    return true;
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

// Parses a TerrainGrass render mode: the enum NAME (Dither / Blend) or a bare integer (0 / 1).
// Case-insensitive, quote-tolerant — the same tolerance ParseDomain gives Terrain.domain.
static bool ParseRenderMode(std::string_view value, Components::TerrainGrassRenderMode& out,
                            std::string* outError)
{
    std::string v(value);
    auto isTrim = [](char ch) { return ch == ' ' || ch == '\t' || ch == '"' || ch == '\''; };
    while (!v.empty() && isTrim(v.front())) v.erase(v.begin());
    while (!v.empty() && isTrim(v.back())) v.pop_back();
    for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    if (v == "dither" || v == "0") { out = Components::TerrainGrassRenderMode::Dither; return true; }
    if (v == "blend" || v == "1") { out = Components::TerrainGrassRenderMode::Blend; return true; }
    if (outError) *outError = "renderMode must be dither/blend or 0/1";
    return false;
}

// Retired TerrainGrass properties. An unrecognized key does not fail the load — the tolerant
// loader records it in the degradation census and preserves its text across saves — but that
// treats a deliberately deleted knob as damage to report and carries dead text forever.
// Retiring it warns once, drops it, and the next save sheds the key.
//
// Deliberately NOT migrated to a value: a retired knob is not a current one under another name.
// Density was a fraction of a fixed candidate lattice whose size depended on the terrain, so the
// same 0.5 meant 4 blades/m2 on a 512 m terrain and 0.016 on an 8 km one; there is no scale-free
// number to carry across. FadeStart/FadeEnd/DistanceFadeEnabled described a fade band that no
// longer exists — blades now leave by not being spawned. RootAlpha/RootAlphaBlendRange faded the
// blade body out toward its base to hide the seam where it met the ground; the root is embedded
// and contact-shaded now, so there is no seam to hide and a blade body is opaque. The contact pair
// (RootContactAo, RootContactShade) scaled the blade base's indirect diffuse and its albedo, and
// only the blade's — one-sided, so their whole sub-1.0 ranges put a step on the contact line, and
// the contact is structural now with no dial to carry a value to. The hue-variance PAIR became
// one authored amount, and RandomColor's tint had no amount dial at all — its strength was a
// shader literal — so both retire into hueVariation rather than mapping onto it.
// Dropping to the current behaviour is the honest migration; inventing a mapping would silently
// restate a value the author never chose.
struct RetiredGrassProperty
{
    // The key as ApplyProperty receives it: the loader lower-cases before dispatch.
    std::string_view Key;
    // The same key as it is spelled in a scene FILE. Carried explicitly because the notice names
    // it, and a notice that says `rootalphablendrange` sends the author grepping for a string
    // their file does not contain.
    std::string_view Authored;
    // Per-key, because the retired set spans several unrelated changes: one fixed sentence would
    // misdescribe whichever group it was not written for.
    std::string_view Reason;
};

constexpr std::string_view kRetiredDensityReason =
    "density is now blades/m2 at the camera and the fade band is gone";
constexpr std::string_view kRetiredRootFadeReason =
    "the blade body is opaque; the root is embedded and contact-shaded instead of faded out";
constexpr std::string_view kRetiredContactAoReason =
    "a blade-side occlusion cannot darken the ground beside it, so the contact keeps its indirect "
    "diffuse fully open";
constexpr std::string_view kRetiredContactShadeReason =
    "the root adopts the ground's albedo exactly; scaling it put a value step on the contact line";
constexpr std::string_view kRetiredShadowStrengthReason =
    "renamed to groundingStrength, which is what it gates - the grounding read, never cast "
    "shadows";
constexpr std::string_view kRetiredHueSplitReason =
    "clump and blade hue spread are one authored amount now (hueVariation), at the shipped "
    "ratio between the two granularities";
constexpr std::string_view kRetiredRandomColorReason =
    "the per-blade tint had no amount dial - its strength was a shader literal - so colour "
    "variation is authored by hueVariation alone";
constexpr std::string_view kRetiredCurvatureReason =
    "blade bend is authored by the wind amplitudes it multiplied (windRestingLean, "
    "windStrength, windFlutterAmount)";
constexpr std::string_view kRetiredScaleReason =
    "blade size is authored in metres by bladeHeight and bladeWidth; a second multiplier over "
    "them made the component report a size it did not render";

constexpr RetiredGrassProperty kRetiredGrassProperties[] = {
    {"density", "density", kRetiredDensityReason},
    {"fadestart", "fadeStart", kRetiredDensityReason},
    {"fadeend", "fadeEnd", kRetiredDensityReason},
    {"distancefadeenabled", "distanceFadeEnabled", kRetiredDensityReason},
    {"texturedensity", "textureDensity", kRetiredDensityReason},
    {"rootalpha", "rootAlpha", kRetiredRootFadeReason},
    {"rootalphablendrange", "rootAlphaBlendRange", kRetiredRootFadeReason},
    {"rootcontactao", "rootContactAo", kRetiredContactAoReason},
    {"rootcontactshade", "rootContactShade", kRetiredContactShadeReason},
    {"scale", "scale", kRetiredScaleReason},
    {"curvature", "curvature", kRetiredCurvatureReason},
    {"clumphuevariance", "clumpHueVariance", kRetiredHueSplitReason},
    {"bladehuevariance", "bladeHueVariance", kRetiredHueSplitReason},
    {"randomcolor", "randomColor", kRetiredRandomColorReason},
    {"shadowstrength", "shadowStrength", kRetiredShadowStrengthReason},
};

const RetiredGrassProperty* FindRetiredGrassProperty(std::string_view property)
{
    for (const auto& entry : kRetiredGrassProperties)
        if (entry.Key == property)
            return &entry;
    return nullptr;
}

// Warns once per process per retired property.
void WarnRetiredGrassProperty(const RetiredGrassProperty& retired)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> warned;
    std::lock_guard<std::mutex> lock(mutex);
    if (warned.emplace(std::string(retired.Key)).second)
        Logger::Log::Warning("Scene: TerrainGrass.{} is retired ({}) — ignoring. Re-save the "
                             "scene to drop it.", retired.Authored, retired.Reason);
}

class TerrainGrassSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainGrass"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainGrass>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("TerrainGrass.renderMode = ") +
            (c->RenderMode == Components::TerrainGrassRenderMode::Blend ? "Blend" : "Dither"));
        outLines.push_back("TerrainGrass.bladesPerSquareMeter = " + FormatFloat(c->BladesPerSquareMeter));
        outLines.push_back("TerrainGrass.range = " + FormatFloat(c->Range));
        outLines.push_back("TerrainGrass.densityFalloff = " + FormatFloat(c->DensityFalloff));
        outLines.push_back("TerrainGrass.placementSeed = " + FormatFloat(c->PlacementSeed));
        outLines.push_back("TerrainGrass.clumpSize = " + FormatFloat(c->ClumpSize));
        outLines.push_back("TerrainGrass.clumpHeightVariance = " + FormatFloat(c->ClumpHeightVariance));
        outLines.push_back("TerrainGrass.clumpAlignment = " + FormatFloat(c->ClumpAlignment));
        outLines.push_back("TerrainGrass.clumpGather = " + FormatFloat(c->ClumpGather));
        outLines.push_back("TerrainGrass.bladeHeight = " + FormatFloat(c->BladeHeight));
        outLines.push_back("TerrainGrass.bladeWidth = " + FormatFloat(c->BladeWidth));
        outLines.push_back("TerrainGrass.maxWidthRatio = " + FormatFloat(c->MaxWidthRatio));
        outLines.push_back("TerrainGrass.bladeSegments = " + std::to_string(c->BladeSegments));
        outLines.push_back("TerrainGrass.randomScale = " + FormatFloat(c->RandomScale));
        outLines.push_back("TerrainGrass.layerIndex = " + std::to_string(c->LayerIndex));
        outLines.push_back("TerrainGrass.maskThreshold = " + FormatFloat(c->MaskThreshold));
        outLines.push_back("TerrainGrass.windDirection = " + FormatFloat(c->WindDirection));
        outLines.push_back("TerrainGrass.windGustSpeed = " + FormatFloat(c->WindGustSpeed));
        outLines.push_back("TerrainGrass.windGustScale = " + FormatFloat(c->WindGustScale));
        outLines.push_back("TerrainGrass.windStrength = " + FormatFloat(c->WindStrength));
        outLines.push_back("TerrainGrass.windRestingLean = " + FormatFloat(c->WindRestingLean));
        outLines.push_back("TerrainGrass.windFlutterAmount = " + FormatFloat(c->WindFlutterAmount));
        outLines.push_back("TerrainGrass.windFlutterSpeed = " + FormatFloat(c->WindFlutterSpeed));
        outLines.push_back("TerrainGrass.windSeed = " + FormatFloat(c->WindSeed));
        outLines.push_back("TerrainGrass.brightness = " + FormatFloat(c->Brightness));
        outLines.push_back("TerrainGrass.randomBrightness = " + FormatFloat(c->RandomBrightness));
        outLines.push_back("TerrainGrass.hueVariation = " + FormatFloat(c->HueVariation));
        outLines.push_back("TerrainGrass.rootShade = " + FormatFloat(c->RootShade));
        outLines.push_back("TerrainGrass.rootFadeStart = " + FormatFloat(c->RootFadeStart));
        outLines.push_back("TerrainGrass.rootFadeEnd = " + FormatFloat(c->RootFadeEnd));
        outLines.push_back("TerrainGrass.bladeNormalForm = " + FormatFloat(c->BladeNormalForm));
        outLines.push_back("TerrainGrass.bladeScatterGain = " + FormatFloat(c->BladeScatterGain));
        outLines.push_back("TerrainGrass.groundingStrength = " + FormatFloat(c->GroundingStrength));
        outLines.push_back("TerrainGrass.translucency = " + FormatFloat(c->Translucency));
        outLines.push_back(std::string("TerrainGrass.textureGrass = ") + (c->TextureGrass ? "true" : "false"));
        outLines.push_back("TerrainGrass.textureCardsPerSquareMeter = " + FormatFloat(c->TextureCardsPerSquareMeter));
        outLines.push_back("TerrainGrass.textureSize = " + FormatFloat(c->TextureSize));
        outLines.push_back(std::string("TerrainGrass.useSplatRootColor = ") + (c->UseSplatRootColor ? "true" : "false"));
        outLines.push_back("TerrainGrass.rootColor = " + std::to_string(c->RootColor));
        outLines.push_back("TerrainGrass.tipColor = " + std::to_string(c->TipColor));
        outLines.push_back("TerrainGrass.backlightColor = " + std::to_string(c->BacklightColor));
        SerializeGuid(outLines, "TerrainGrass.albedoTexture", c->AlbedoTextureAssetGuid.ToGuid());
        SerializeGuid(outLines, "TerrainGrass.alphaTexture", c->AlphaTextureAssetGuid.ToGuid());
        SerializeGuid(outLines, "TerrainGrass.normalTexture", c->NormalTextureAssetGuid.ToGuid());
        outLines.push_back("TerrainGrass.atlasColumns = " + std::to_string(c->AtlasColumns));
        outLines.push_back("TerrainGrass.atlasRows = " + std::to_string(c->AtlasRows));
        outLines.push_back("TerrainGrass.atlasTileCount = " + std::to_string(c->AtlasTileCount));
        outLines.push_back("TerrainGrass.alphaCutoff = " + FormatFloat(c->AlphaCutoff));
        outLines.push_back("TerrainGrass.normalStrength = " + FormatFloat(c->NormalStrength));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainGrass c{};
        if (const auto* existing = world.GetComponent<Components::TerrainGrass>(entity))
            c = *existing;

        if (property == "rendermode") { if (!ParseRenderMode(value, c.RenderMode, outError)) return false; }
        else if (property == "bladespersquaremeter") { if (!ParseF32(value, c.BladesPerSquareMeter, outError)) return false; }
        else if (property == "range") { if (!ParseF32(value, c.Range, outError)) return false; }
        else if (property == "densityfalloff") { if (!ParseF32(value, c.DensityFalloff, outError)) return false; }
        else if (property == "placementseed") { if (!ParseF32(value, c.PlacementSeed, outError)) return false; }
        else if (property == "clumpsize") { if (!ParseF32(value, c.ClumpSize, outError)) return false; }
        else if (property == "clumpheightvariance") { if (!ParseF32(value, c.ClumpHeightVariance, outError)) return false; }
        else if (property == "clumpalignment") { if (!ParseF32(value, c.ClumpAlignment, outError)) return false; }
        else if (property == "clumpgather") { if (!ParseF32(value, c.ClumpGather, outError)) return false; }
        else if (property == "bladeheight") { if (!ParseF32(value, c.BladeHeight, outError)) return false; }
        else if (property == "bladewidth") { if (!ParseF32(value, c.BladeWidth, outError)) return false; }
        else if (property == "maxwidthratio") { if (!ParseF32(value, c.MaxWidthRatio, outError)) return false; }
        else if (property == "bladesegments") { if (!ParseU32(value, c.BladeSegments, outError)) return false; }
        else if (property == "randomscale") { if (!ParseF32(value, c.RandomScale, outError)) return false; }
        else if (property == "layerindex") { if (!ParseU32(value, c.LayerIndex, outError)) return false; }
        else if (property == "maskthreshold") { if (!ParseF32(value, c.MaskThreshold, outError)) return false; }
        else if (property == "winddirection") { if (!ParseF32(value, c.WindDirection, outError)) return false; }
        else if (property == "windgustspeed") { if (!ParseF32(value, c.WindGustSpeed, outError)) return false; }
        else if (property == "windgustscale") { if (!ParseF32(value, c.WindGustScale, outError)) return false; }
        else if (property == "windstrength") { if (!ParseF32(value, c.WindStrength, outError)) return false; }
        else if (property == "windrestinglean") { if (!ParseF32(value, c.WindRestingLean, outError)) return false; }
        else if (property == "windflutteramount") { if (!ParseF32(value, c.WindFlutterAmount, outError)) return false; }
        else if (property == "windflutterspeed") { if (!ParseF32(value, c.WindFlutterSpeed, outError)) return false; }
        else if (property == "windseed") { if (!ParseF32(value, c.WindSeed, outError)) return false; }
        else if (property == "brightness") { if (!ParseF32(value, c.Brightness, outError)) return false; }
        else if (property == "randombrightness") { if (!ParseF32(value, c.RandomBrightness, outError)) return false; }
        else if (property == "huevariation") { if (!ParseF32(value, c.HueVariation, outError)) return false; }
        else if (property == "rootshade") { if (!ParseF32(value, c.RootShade, outError)) return false; }
        else if (property == "rootfadestart") { if (!ParseF32(value, c.RootFadeStart, outError)) return false; }
        else if (property == "rootfadeend") { if (!ParseF32(value, c.RootFadeEnd, outError)) return false; }
        else if (property == "bladenormalform") { if (!ParseF32(value, c.BladeNormalForm, outError)) return false; }
        else if (property == "bladescattergain") { if (!ParseF32(value, c.BladeScatterGain, outError)) return false; }
        else if (property == "groundingstrength") { if (!ParseF32(value, c.GroundingStrength, outError)) return false; }
        else if (property == "translucency") { if (!ParseF32(value, c.Translucency, outError)) return false; }
        else if (property == "texturegrass") { if (!ParseBool(value, c.TextureGrass, outError)) return false; }
        else if (property == "texturecardspersquaremeter") { if (!ParseF32(value, c.TextureCardsPerSquareMeter, outError)) return false; }
        else if (property == "texturesize") { if (!ParseF32(value, c.TextureSize, outError)) return false; }
        else if (property == "usesplatrootcolor") { if (!ParseBool(value, c.UseSplatRootColor, outError)) return false; }
        else if (property == "rootcolor") { if (!ParseU32(value, c.RootColor, outError)) return false; }
        else if (property == "tipcolor") { if (!ParseU32(value, c.TipColor, outError)) return false; }
        else if (property == "backlightcolor") { if (!ParseU32(value, c.BacklightColor, outError)) return false; }
        else if (property == "albedotexture") { GUID g{}; if (!ParseGuid(value, g, outError)) return false; c.AlbedoTextureAssetGuid.Set(g); }
        else if (property == "alphatexture") { GUID g{}; if (!ParseGuid(value, g, outError)) return false; c.AlphaTextureAssetGuid.Set(g); }
        else if (property == "normaltexture") { GUID g{}; if (!ParseGuid(value, g, outError)) return false; c.NormalTextureAssetGuid.Set(g); }
        else if (property == "atlascolumns") { if (!ParseU32(value, c.AtlasColumns, outError)) return false; }
        else if (property == "atlasrows") { if (!ParseU32(value, c.AtlasRows, outError)) return false; }
        else if (property == "atlastilecount") { if (!ParseU32(value, c.AtlasTileCount, outError)) return false; }
        else if (property == "alphacutoff") { if (!ParseF32(value, c.AlphaCutoff, outError)) return false; }
        else if (property == "normalstrength") { if (!ParseF32(value, c.NormalStrength, outError)) return false; }
        else if (const auto* retired = FindRetiredGrassProperty(property))
        {
            WarnRetiredGrassProperty(*retired);
        }
        else
        {
            if (outError)
                *outError = "Unknown TerrainGrass property";
            return false;
        }

        // The bounds are the component's, not this parser's, so an authored value and the same
        // value set over IPC land identically. Whole-component rather than per-property: it costs
        // one pass over a POD and it repairs a field an earlier writer left out of range.
        TerrainGrass::ClampFields(c);
        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainGrass>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::TerrainGrass{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainGrass>(entity);
        return true;
    }
};

GE_REGISTER_SCENE_SCHEMA(TerrainGrassSchema)

static void RegisterIfMissing(std::unique_ptr<ISceneComponentSchema> schema)
{
    if (SceneSchemaRegistry::HasRegistered(schema->GetComponentName()))
        return;
    SceneSchemaRegistry::Register(std::move(schema));
}

} // namespace

void EnsureTerrainGrassSceneSchemasRegistered()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        RegisterIfMissing(std::make_unique<TerrainGrassSchema>());
    });
}

} // namespace GameEngine::Scene
