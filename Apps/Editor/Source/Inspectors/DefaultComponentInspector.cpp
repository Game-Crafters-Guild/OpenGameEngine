#include "Inspectors/DefaultComponentInspector.h"

#include "InspectorRegistry.h"           // InspectorContext
#include "EditorChangeNotifications.h"
#include "UndoRedo/UndoRedoService.h"
#include "Inspectors/InspectorDragHelpers.h"  // AddFloatRowWithDrag / AddIntRowWithDrag / AddToggleRow
#include "Inspectors/InspectorUIHelpers.h"    // InspectorUI::AddLine
#include "Inspectors/InspectorComponentRowHelpers.h"  // MakeComponentSnapshotTargetById
#include "Inspectors/PreservedFieldNotice.h"          // AddPreservedFieldNotice
#include "UI/Controls/TextField.h"            // editable String / uint rows
#include "UI/Controls/Dropdown.h"             // enum name dropdown (scanner-detected enum fields)
#include "UI/Controls/Slider.h"               // compact ocean planar reflection strength slider
#include "UI/StyleProperties.h"               // Style::* for the Color swatch drawer
#include "UI/UIManager.h"                      // GetFocusedElementId for the live-refresh focus skip

#include "ECS/ComponentFieldRegistry.h"
#include "ECS/Reflection.h"               // FieldInfo, FieldTypeId, FieldElementSize

#include <limits>                          // quiet_NaN for the clamped-range float row
#include "ECS/Entity.h"                   // World (CaptureComponentBytes / ApplyComponentBytesImmediate)
#include "Components/Transform.h"         // Euler <-> Quaternion helpers for the Quat drawer
#include "Core/Engine.h"                  // EngineCore::GetInstance().GetAssetManager() for the asset picker
#include "Assets/AssetRegistry.h"         // AssetRegistry for AssetGuid field pickers
#include "AssetCore/GUID.h"               // GUID (AssetRef field storage)
#include "Types/StringUtils.h"            // IdentifierToWords (row and option labels)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine {
namespace {

using Snapshot = Editor::UndoRedoService::SnapshotTarget::Snapshot;

template <class T>
bool ReadPod(const Snapshot& bytes, std::size_t offset, T& out)
{
    if (offset + sizeof(T) > bytes.size())
        return false;
    std::memcpy(&out, bytes.data() + offset, sizeof(T));
    return true;
}

// True when this field's bytes differ between the primary and any of the other
// selected entities — i.e. the selection disagrees, so the field renders as
// indeterminate ("mixed"). Empty extras (single selection) is never mixed.
bool SpanDiffers(const Snapshot& primary, const std::vector<Snapshot>& extras,
                 std::size_t offset, std::size_t size)
{
    if (offset + size > primary.size())
        return false;
    for (const Snapshot& other : extras)
    {
        if (offset + size > other.size())
            return true;  // layout/size mismatch counts as a difference
        if (std::memcmp(primary.data() + offset, other.data() + offset, size) != 0)
            return true;
    }
    return false;
}

// Read any integer field element as a signed int for display. 64-bit values are
// truncated to int — acceptable for the starter set (no 64-bit fields); revisit
// when a component reflects an int64/uint64 that needs the full range.
int ReadIntElement(const Snapshot& bytes, std::size_t offset, ECS::FieldTypeId type)
{
    using ECS::FieldTypeId;
    switch (type)
    {
        case FieldTypeId::Int8:   { std::int8_t   v = 0; ReadPod(bytes, offset, v); return static_cast<int>(v); }
        case FieldTypeId::UInt8:  { std::uint8_t  v = 0; ReadPod(bytes, offset, v); return static_cast<int>(v); }
        case FieldTypeId::Int16:  { std::int16_t  v = 0; ReadPod(bytes, offset, v); return static_cast<int>(v); }
        case FieldTypeId::UInt16: { std::uint16_t v = 0; ReadPod(bytes, offset, v); return static_cast<int>(v); }
        case FieldTypeId::Int32:  { std::int32_t  v = 0; ReadPod(bytes, offset, v); return static_cast<int>(v); }
        case FieldTypeId::UInt32: { std::uint32_t v = 0; ReadPod(bytes, offset, v); return static_cast<int>(v); }
        case FieldTypeId::Int64:  { std::int64_t  v = 0; ReadPod(bytes, offset, v); return static_cast<int>(v); }
        case FieldTypeId::UInt64: { std::uint64_t v = 0; ReadPod(bytes, offset, v); return static_cast<int>(v); }
        default: return 0;
    }
}

void WriteIntElement(Snapshot& bytes, std::size_t offset, ECS::FieldTypeId type, int value)
{
    using ECS::FieldTypeId;
    switch (type)
    {
        case FieldTypeId::Int8:   { auto v = static_cast<std::int8_t>(value);   if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt8:  { auto v = static_cast<std::uint8_t>(value);  if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::Int16:  { auto v = static_cast<std::int16_t>(value);  if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt16: { auto v = static_cast<std::uint16_t>(value); if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::Int32:  { auto v = static_cast<std::int32_t>(value);  if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt32: { auto v = static_cast<std::uint32_t>(value); if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::Int64:  { auto v = static_cast<std::int64_t>(value);  if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt64: { auto v = static_cast<std::uint64_t>(value); if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        default: break;
    }
}

// Write an unsigned integer of the field's width (used by the editable uint row,
// which parses the full unsigned range a signed int can't hold).
void WriteUIntElement(Snapshot& bytes, std::size_t offset, ECS::FieldTypeId type, std::uint64_t value)
{
    using ECS::FieldTypeId;
    switch (type)
    {
        case FieldTypeId::UInt8:  { auto v = static_cast<std::uint8_t>(value);  if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt16: { auto v = static_cast<std::uint16_t>(value); if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt32: { auto v = static_cast<std::uint32_t>(value); if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt64: { if (offset + sizeof(value) <= bytes.size()) std::memcpy(bytes.data() + offset, &value, sizeof(value)); break; }
        default: break;
    }
}

// Read an integer field element as a full 64-bit signed value (no truncation),
// honoring the field's signedness/width. Enum matching compares against the
// EnumNameValue table's std::int64_t, so the full range must survive.
std::int64_t ReadIntElement64(const Snapshot& bytes, std::size_t offset, ECS::FieldTypeId type)
{
    using ECS::FieldTypeId;
    switch (type)
    {
        case FieldTypeId::Int8:   { std::int8_t   v = 0; ReadPod(bytes, offset, v); return static_cast<std::int64_t>(v); }
        case FieldTypeId::UInt8:  { std::uint8_t  v = 0; ReadPod(bytes, offset, v); return static_cast<std::int64_t>(v); }
        case FieldTypeId::Int16:  { std::int16_t  v = 0; ReadPod(bytes, offset, v); return static_cast<std::int64_t>(v); }
        case FieldTypeId::UInt16: { std::uint16_t v = 0; ReadPod(bytes, offset, v); return static_cast<std::int64_t>(v); }
        case FieldTypeId::Int32:  { std::int32_t  v = 0; ReadPod(bytes, offset, v); return static_cast<std::int64_t>(v); }
        case FieldTypeId::UInt32: { std::uint32_t v = 0; ReadPod(bytes, offset, v); return static_cast<std::int64_t>(v); }
        case FieldTypeId::Int64:  { std::int64_t  v = 0; ReadPod(bytes, offset, v); return v; }
        case FieldTypeId::UInt64: { std::uint64_t v = 0; ReadPod(bytes, offset, v); return static_cast<std::int64_t>(v); }
        default: return 0;
    }
}

// Write a full 64-bit value into an integer field element of the field's width.
// Enum values come from the table as std::int64_t; truncating to int would corrupt
// large UInt32/UInt64 enumerators, so this writes the native width directly.
void WriteIntElement64(Snapshot& bytes, std::size_t offset, ECS::FieldTypeId type, std::int64_t value)
{
    using ECS::FieldTypeId;
    switch (type)
    {
        case FieldTypeId::Int8:   { auto v = static_cast<std::int8_t>(value);   if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt8:  { auto v = static_cast<std::uint8_t>(value);  if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::Int16:  { auto v = static_cast<std::int16_t>(value);  if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt16: { auto v = static_cast<std::uint16_t>(value); if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::Int32:  { auto v = static_cast<std::int32_t>(value);  if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt32: { auto v = static_cast<std::uint32_t>(value); if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::Int64:  { auto v = static_cast<std::int64_t>(value);  if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        case FieldTypeId::UInt64: { auto v = static_cast<std::uint64_t>(value); if (offset + sizeof(v) <= bytes.size()) std::memcpy(bytes.data() + offset, &v, sizeof(v)); break; }
        default: break;
    }
}

// Index of the EnumNames entry whose Value equals `value`, or -1 when the stored
// integer is outside the table (a value the scanner never saw). Out-of-table
// values fall back to the numeric field so they stay visible and editable.
int EnumIndexForValue(std::span<const ECS::EnumNameValue> names, std::int64_t value)
{
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (names[i].Value == value)
            return static_cast<int>(i);
    }
    return -1;
}

// Holds the interactive-edit state for one widget across a drag sequence.
// onChanging begins the edit lazily (capturing the pre-edit "before" snapshot)
// and previews live; onChanged commits a single undo entry. When there is no
// undo service the writes apply immediately with no history.
struct FieldEditContext
{
    ECS::World* World = nullptr;
    ECS::EntityHandle Entity{};
    ECS::ComponentTypeId TypeId = 0;
    Editor::EditorChangeNotifications* Notify = nullptr;
    Editor::UndoRedoService* Undo = nullptr;
    std::string Label;
    // Additional selected entities that also have this component. An edit is
    // broadcast to all of them and recorded as one undo entry (empty = single edit).
    std::vector<ECS::EntityHandle> Extras;
};

// One undo target for the current edit: a single-entity target when nothing else
// is selected, else a multi-entity target so the whole broadcast is one entry.
inline Editor::UndoRedoService::SnapshotTarget MakeEditTarget(const FieldEditContext& fec)
{
    return fec.Extras.empty()
        ? InspectorDrag::MakeComponentSnapshotTargetById(fec.World, fec.Entity, fec.TypeId, fec.Notify, fec.Label)
        : InspectorDrag::MakeMultiComponentSnapshotTargetById(fec.World, fec.Entity, fec.Extras, fec.TypeId, fec.Notify, fec.Label);
}

// Wire a widget's live/commit callbacks to patch a single field. `patch` writes
// the widget's new value into a freshly captured byte buffer at the right offset.
template <class V, class PatchFn>
void WireEdit(const std::shared_ptr<std::optional<Editor::UndoRedoService::InteractiveEdit>>& editState,
              const FieldEditContext& fec, PatchFn patch,
              std::function<void(V)>& outChanging, std::function<void(V)>& outChanged)
{
    auto applyLive = [fec, patch](V value) {
        const auto applyOne = [&](ECS::EntityHandle e) {
            Snapshot bytes;
            if (!fec.World || !fec.World->CaptureComponentBytes(e, fec.TypeId, bytes))
                return;
            patch(bytes, value);
            fec.World->ApplyComponentBytesImmediate(e, fec.TypeId, bytes);
        };
        applyOne(fec.Entity);
        for (ECS::EntityHandle e : fec.Extras)
            applyOne(e);
    };

    outChanging = [editState, fec, applyLive](V value) {
        if (!fec.Undo)
        {
            applyLive(value);
            return;
        }
        if (!editState->has_value())
            editState->emplace(fec.Undo->BeginInteractiveEdit(fec.Label, MakeEditTarget(fec)));
        editState->value().Preview([&]() { applyLive(value); });
    };

    outChanged = [editState, fec, applyLive](V value) {
        if (!fec.Undo)
        {
            applyLive(value);
            return;
        }
        if (!editState->has_value())
            editState->emplace(fec.Undo->BeginInteractiveEdit(fec.Label, MakeEditTarget(fec)));
        applyLive(value);
        // Move the edit out and clear the shared state before Commit: Commit fires
        // Notify (which fans out across all edited entities), and were that ever to
        // rebuild the inspector synchronously it would destroy this lambda mid-call.
        // Committing a local keeps that safe.
        auto edit = std::move(editState->value());
        editState->reset();
        edit.Commit();
    };
}

// Row label for a reflected field: a curated name where the field's own reads badly, otherwise its
// identifier as words.
std::string FieldDisplayName(std::string_view name)
{
    if (name == "Meshes")
        return "Mesh";
    if (name == "CombineDisplacementCascade")
        return "Combine Cascade";
    if (name == "LodDataResolution")
        return "LOD Data Resolution";
    if (name == "RasterDepthCaptureResolution")
        return "Raster Depth Resolution";
    if (name == "PlanarReflectionStrength")
        return "Planar Strength";
    if (name == "PlanarReflectionScale")
        return "Planar Scale";
    if (name == "FoamDebugMode")
        return "Foam Debug";
    if (name == "ShallowRefractionReflectionSuppression")
        return "Shallow Reflection Suppression";
    if (name == "DirectionalLightColor")
        return "Glitter Color";
    if (name == "SkyIntensity")
        return "Sky Light";
    if (name == "ReflectionIntensity")
        return "Local Reflections";
    if (name == "ProbesLongAxis")
        return "Divisions";
    if (name == "RaysPerProbe")
        return "Rays / Probe";
    return IdentifierToWords(name);
}

// Option label for an enumerator: a curated name where one is kept, otherwise its identifier as
// words.
std::string EnumDisplayName(std::string_view fieldName, std::string_view name)
{
    if (name == "SingleGrid")
        return "single grid";
    if (name == "Cascaded2")
        return "cascaded (2)";
    // Components::DDGIDebugView
    if (name == "CascadeWeight")
        return "cascade weight";
    if (name == "NearestProbeIrradiance")
        return "nearest probe irradiance";
    if (name == "ProbeStateFine")
        return "probe state (fine)";
    if (name == "NearestProbeIrradianceFine")
        return "nearest probe irradiance (fine)";
    if (name == "FineProbeCoord")
        return "fine probe coord";
    if (name == "FineProbeSlot")
        return "fine probe slot";
    if (name == "ProbeState")
        return "probe state";
    if (name == "Coverage")
        return "probe coverage";
    // Components::ReflectionProbeUpdateMode: a realtime probe re-bakes only
    // while its world changes, so the label says so.
    if (fieldName == "UpdateMode" && name == "Realtime")
        return "Realtime (on change)";
    // DDGIGlossyResolveScale (Components/Rendering/DDGIVolume.h). Gated on
    // the field: reflection only hands us value names, and "Full"/"Half"
    // are generic enough to collide with unrelated enums (e.g.
    // HumanoidRetargetLOD::Full).
    if (fieldName == "GlossyResolveScale")
    {
        if (name == "Quarter")
            return "25%";
        if (name == "Half")
            return "50%";
        if (name == "ThreeQuarter")
            return "75%";
        if (name == "Full")
            return "100%";
    }
    return IdentifierToWords(name);
}

// "Mesh 1" for the first of several elements: counted from one, as the rest of the inspector counts slots.
std::string ElementLabel(std::string_view name, std::uint32_t index, std::uint32_t count)
{
    std::string label(name);
    if (count > 1)
        label += " " + std::to_string(index + 1);
    return label;
}

std::string_view SimpleTypeName(std::string_view canonicalName)
{
    const std::size_t pos = canonicalName.rfind("::");
    return (pos == std::string_view::npos) ? canonicalName : canonicalName.substr(pos + 2);
}

const char* OceanSurfaceFieldTooltip(std::string_view fieldName)
{
    struct Entry { std::string_view Field; const char* Tooltip; };
    static constexpr Entry kTooltips[] = {
        {"DeepColor", "Base deep-water colour used when depth fog fully hides the refracted scene."},
        {"FoamColor", "Tint of whitecaps, shoreline foam, and intersection foam."},
        {"ChoppyScale", "Scales horizontal wave displacement. Higher values sharpen crests and wind chop."},
        {"FresnelPower", "Controls how strongly reflection concentrates near grazing viewing angles."},
        {"ReflectionStrength", "Scales sky and planar reflection contribution on the water surface."},
        {"SubsurfaceStrength", "Overall intensity of the teal subsurface glow through wave crests."},
        {"FoamAmount", "Coverage multiplier for wave-generated whitecaps."},
        {"FoamFadeRate", "How quickly simulated foam decays over time. Lower values leave longer trails."},
        {"WaveFoamStrength", "Amount of foam deposited by breaking or pinching wave crests."},
        {"WaveFoamCoverage", "Breaking threshold for wave foam. Higher values create foam more readily."},
        {"FoamScale", "World tiling scale for the bubble/foam texture pattern."},
        {"FoamFeather", "Softness of foam edges and threshold transitions."},
        {"SprayWindThreshold", "Minimum wind speed in meters per second before open-water crest spray begins."},
        {"SprayEmissionThreshold", "Minimum sampled crest strength required to emit wind spray. Higher values restrict spray to stronger crests."},
        {"FoamTexture", "Optional texture sampled for foam bubbles. Empty uses procedural foam."},
        {"FoamDebugMode", "Debug overlay mode for inspecting foam, shoreline, and depth data."},
        {"ShorelineFoamMaxDepth", "Seabed depth range where shoreline foam appears and fades out."},
        {"ShorelineFoamStrength", "Intensity multiplier for foam along shallow shorelines."},
        {"NormalsStrength", "Strength of high-frequency normal detail on the water surface."},
        {"NormalsScale", "World tiling scale of high-frequency water normal detail."},
        {"Diffuse", "Main lit water diffuse/scatter colour."},
        {"DiffuseGrazing", "Diffuse colour used when viewing across the water at grazing angles."},
        {"DiffuseShadow", "Diffuse tint for water faces with less direct sun."},
        {"SubSurfaceShallowCol", "Shallow-water tint blended in where the seabed is near the surface."},
        {"SubSurfaceDepthMax", "Depth at which shallow-water tint fully fades to deep water."},
        {"SubSurfaceDepthPower", "Exponent shaping the shallow-to-deep colour fade."},
        {"SubSurfaceColour", "Ambient subsurface scatter colour pushed through the water."},
        {"SubSurfaceBase", "Baseline subsurface scatter present across the surface."},
        {"SubSurfaceSun", "Forward-scatter lobe intensity toward the sun."},
        {"SubSurfaceSunFallOff", "Falloff of the sun-facing subsurface scatter lobe."},
        {"Specular", "Specular reflection intensity cap."},
        {"Roughness", "Microfacet roughness for water highlights and reflections."},
        {"IorAir", "Index of refraction for air, used with IOR water for Fresnel/refraction."},
        {"IorWater", "Index of refraction for water, used for Fresnel and underwater bending."},
        {"PlanarReflections", "Uses a planar reflection capture when available, with sky as fallback."},
        {"PlanarReflectionStrength", "Blends the planar reflection capture over the procedural reflected sky."},
        {"PlanarReflectionScale", "Resolution scale of the planar reflection capture relative to the main view."},
        {"SkyBase", "Base/zenith colour of the procedural reflected sky."},
        {"SkyTowardsSun", "Reflected sky tint in the sun direction."},
        {"SkyAwayFromSun", "Reflected sky tint away from the sun."},
        {"SkyDirectionality", "Sharpness of the reflected sky blend around the sun direction."},
        {"DirectionalLightColor", "Ocean-only tint for the bright direct sun glitter on the water. White keeps the scene light colour."},
        {"DirectionalLightBoost", "Boost for the sun glitter spike on reflected sky."},
        {"DirectionalLightFallOff", "Falloff/sharpness of the sun glitter spike."},
        {"DepthFogDensity", "Per-channel water extinction for refracted and underwater geometry. Higher values fog faster."},
        {"DepthFogFalloff", "Distance curve used by surface refraction fog and the underwater pass."},
        {"DepthFogStartDistance", "Meters of clear water before depth fog starts to build."},
        {"DepthFogEndDistance", "Distance where Linear/Smooth depth fog reaches its authored range. 0 uses an automatic range from density."},
        {"DepthFogFalloffPower", "Curve shaping for depth fog distance. 1 is neutral; higher values delay fog near the camera."},
        {"RefractionStrength", "Strength of screen-space refraction distortion from surface normals."},
        {"ShallowRefractionReflectionSuppression", "Reduces sky and planar reflection only where shallow refraction is clear, keeping seabed caustics visible. 0 leaves reflection unchanged."},
        {"ShallowClarityDistance", "Meters of view path through the water over which the depth fog ramps up to full strength, keeping the seabed readable in the shallows. Shorter brings the deep color in sooner."},
        {"ShallowClarityFloor", "The fraction of depth fog at the surface (1 = no window), ramping up to full over the clarity distance."},
        {"CausticsScale", "World tiling size of the caustics pattern. Smaller values make tighter caustics."},
        {"CausticsAverage", "Neutral average subtracted from caustics so they can darken as well as brighten."},
        {"CausticsStrength", "Contrast/intensity of the caustics pattern."},
        {"CausticsFocalDepth", "Water depth where caustics appear sharpest."},
        {"CausticsDepthOfField", "Depth range over which caustics stay sharp around the focal depth."},
        {"CausticsDistortionStrength", "Amount of ripple distortion applied to procedural caustics UVs."},
        {"CausticsDistortionScale", "World tiling scale of the caustics distortion sample."},
        {"CausticsTexture", "Optional user caustics texture. Empty uses the procedural caustics web."},
        {"Underwater", "Enables the fullscreen underwater pass when the camera is below the water surface."},
        {"MeniscusWidth", "Width of the bright waterline band when the camera crosses the surface."},
        {"UnderwaterInscattering", "Enables sun light scattering through the underwater view."},
        {"InscatterStrength", "Intensity of underwater in-scattered light."},
        {"InscatterPhaseG", "Anisotropy of underwater scattering. Higher values push light forward toward the sun direction."},
        {"UnderwaterDistortion", "Enables underwater lens and waterline distortion."},
        {"DistortionStrength", "Strength of the underwater screen distortion."},
        {"UnderwaterGodRays", "Enables procedural underwater god rays."},
        {"GodRayStrength", "Brightness of underwater god rays."},
        {"GodRayDensity", "Angular frequency/density of underwater god ray shafts."},
        {"CausticsOnGeometry", "Projects caustics onto submerged scene geometry in the underwater pass."},
        {"LightShafts", "Enables above-water screen-space light shafts from the sun."},
        {"LightShaftStrength", "Intensity of above-water sun shafts."},
        {"Flow", "Enables the ocean flow cascade to advect foam and surface detail."},
        {"DynamicWaves", "Enables interactive dynamic waves from OceanWaveImpulse and water interactions."},
        {"ClipSurface", "Enables clip sources that cut holes or restore regions in the ocean surface."},
        {"DefaultClippingState", "Default clip value outside clip sources. 0 keeps water solid; 1 clips it away."},
        {"Albedo", "Enables albedo sources that paint colour onto the water surface."},
        {"IntersectionFoamDepth", "Screen-space depth range used to form foam where water meets opaque geometry."},
        {"IntersectionFoamStrength", "Intensity multiplier for intersection/contact foam."},
    };
    for (const Entry& entry : kTooltips)
    {
        if (entry.Field == fieldName)
            return entry.Tooltip;
    }
    return nullptr;
}

const char* OceanRendererFieldTooltip(std::string_view fieldName)
{
    struct Entry { std::string_view Field; const char* Tooltip; };
    static constexpr Entry kTooltips[] = {
        {"QualityOverride", "Ocean quality tier. 0 = Low, 1 = Medium, 2 = High."},
        {"LodCount", "Number of camera-snapped ocean LOD cascade layers."},
        {"LodDataResolution", "Texel resolution per axis for ocean simulation cascades."},
        {"MinScale", "World size of the finest ocean cascade. Smaller values concentrate detail near the camera."},
        {"MaxScale", "Maximum world size of the coarsest ocean cascade."},
        {"GeometryUpSampleFactor", "Multiplier for the camera-centered ocean grid density."},
        {"GeometryDownSampleFactor", "Divider for ocean grid density and triangle count."},
        {"GravityMultiplier", "Scales wave dispersion gravity for FFT and dynamic waves."},
        {"TimeScale", "Multiplies ocean animation time. 0 freezes wave and simulation motion."},
        {"TimeOffset", "Offsets ocean animation phase without changing speed."},
        {"UseFixedTime", "Uses Fixed Time as the absolute ocean time for deterministic preview or capture."},
        {"FixedTime", "Absolute ocean time used when Use Fixed Time is enabled."},
        {"MaxActiveDepthCaches", "Runtime budget for active OceanDepthCacheSource components. 0 keeps all eligible caches."},
        {"RasterDepthCapture", "Captures dynamic mesh geometry into the seabed-depth texture from a top-down pass."},
        {"RasterDepthCaptureResolution", "Resolution of the dynamic raster seabed-depth capture."},
        {"RasterDepthCaptureRenderLayerMask", "Render-layer mask used by raster seabed-depth capture."},
        {"RasterDepthCaptureSizeX", "World X size of the top-down raster seabed-depth capture."},
        {"RasterDepthCaptureSizeZ", "World Z size of the top-down raster seabed-depth capture."},
        {"RasterDepthCaptureTopPadding", "Extra height above captured geometry for the raster depth camera."},
        {"RasterDepthCaptureDeepWaterDepth", "Fallback water depth used where raster capture finds no seabed."},
        {"GlobalWindSpeed", "Global wind speed in meters per second for FFT and Gerstner waves."},
        {"GlobalWindDirection", "Global wind direction in degrees. 0 points along world +X."},
        {"GlobalWindTurbulence", "Directional spread of global wind waves. Higher values make a more chaotic sea."},
        {"SpectrumAsset", "Optional asset-backed global wave spectrum."},
        {"CombineDisplacementCascade", "Experimental path that combines FFT displacement cascades once per frame."},
    };
    for (const Entry& entry : kTooltips)
    {
        if (entry.Field == fieldName)
            return entry.Tooltip;
    }
    return nullptr;
}

const char* ReflectionProbeFieldTooltip(std::string_view fieldName)
{
    struct Entry { std::string_view Field; const char* Tooltip; };
    static constexpr Entry kTooltips[] = {
        {"BlendDistance", "Fade width near the influence edge. Visible when the camera is near the probe boundary."},
        {"Priority", "Tie-breaker when more than one probe influences the camera."},
        {"BoxProjection", "Use the transform-scaled local box for probe selection and parallax-corrected specular reflections. When disabled, the transform range is treated as an ellipsoid."},
        {"OriginOffsetX", "Capture/projection origin offset along the probe's local X axis."},
        {"OriginOffsetY", "Capture/projection origin offset along the probe's local Y axis."},
        {"OriginOffsetZ", "Capture/projection origin offset along the probe's local Z axis."},
        {"Intensity", "Runtime IBL multiplier. Updates immediately without rebaking the cubemap."},
        {"ExposureEV", "Exposure applied when baking the probe capture."},
        {"RotationDegrees", "Yaw rotation applied to sky/HDRI probe lighting during bake."},
        {"IblLowerHemisphereDarkness", "Darkens lighting sampled from below the horizon. 0 keeps it bright; 1 makes it black."},
        {"CaptureResolution", "Cubemap face resolution for the scene capture. Higher values sharpen mirror-like reflections but make probe bakes more expensive."},
        {"UpdateMode", "When the probe re-bakes. Once: only when the probe's own settings, the sky or the geometry in its world are added or removed. Realtime (on change): also whenever its world changes: lights, the sky, moved or edited renderables, material edits, and animated or depth-writing content such as skinned characters, terrain, an animated ocean or grass. Shader-time material animation, playing video textures and particles do not re-bake it; set the interval to 0 to capture them every frame."},
        {"RealtimeUpdateInterval", "Realtime (on change): the fastest the probe may re-bake while the world changes, in seconds; 0 = every frame."},
        {"MaxDistance", "Far clip distance for scene capture. 0 uses the probe volume."},
        {"CullMask", "Render-layer mask captured by the probe's hidden scene views."},
    };
    for (const Entry& entry : kTooltips)
    {
        if (entry.Field == fieldName)
            return entry.Tooltip;
    }
    return nullptr;
}

std::string FieldTooltip(ECS::ComponentTypeId typeId, const ECS::FieldInfo& field)
{
    if (!field.Tooltip.empty())
        return std::string(field.Tooltip);

    const std::string_view fieldName = field.Name;
    const std::string_view typeName = SimpleTypeName(ECS::ComponentFieldRegistry::GetCanonicalName(typeId));
    if (typeName == "OceanSurface")
    {
        if (const char* tooltip = OceanSurfaceFieldTooltip(fieldName))
            return tooltip;
    }
    else if (typeName == "OceanRenderer")
    {
        if (const char* tooltip = OceanRendererFieldTooltip(fieldName))
            return tooltip;
    }
    else if (typeName == "ReflectionProbe")
    {
        if (const char* tooltip = ReflectionProbeFieldTooltip(fieldName))
            return tooltip;
    }

    if (typeName.rfind("Ocean", 0) == 0)
        return IdentifierToWords(fieldName) + " for " + std::string(typeName) + ".";

    return {};
}

// A labeled row backed by a TextField (for String and editable-uint fields).
TextField* AddTextFieldRow(UIElement* parent, const std::string& label, const std::string& initial,
                           const char* tooltip = nullptr)
{
    UIElement* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, label, tooltip);
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    auto text = std::make_unique<TextField>();
    text->SetValue(initial);
    text->AddClass("inspector-text-field");
    if (tooltip)
        text->SetTooltip(tooltip);
    TextField* raw = text.get();
    fieldContainer->AddChild(std::move(text));
    return raw;
}

// Apply a discrete byte-patch to the primary + every extra entity as ONE undo
// entry. Used by widgets that commit in a single shot (text fields, the entity
// picker) rather than through a drag gesture.
void CommitFieldPatch(const FieldEditContext& fec, const std::function<void(Snapshot&)>& patch)
{
    const auto applyOne = [&](ECS::EntityHandle e) {
        Snapshot bytes;
        if (fec.World && fec.World->CaptureComponentBytes(e, fec.TypeId, bytes))
        {
            patch(bytes);
            fec.World->ApplyComponentBytesImmediate(e, fec.TypeId, bytes);
        }
    };
    if (!fec.Undo)
    {
        applyOne(fec.Entity);
        for (ECS::EntityHandle e : fec.Extras)
            applyOne(e);
        return;
    }
    auto edit = fec.Undo->BeginInteractiveEdit(fec.Label, MakeEditTarget(fec));
    applyOne(fec.Entity);
    for (ECS::EntityHandle e : fec.Extras)
        applyOne(e);
    edit.Commit();
}

// Commit a TextField edit as a single undo entry: writeFn patches the component
// bytes from the field's current string value.
void WireTextCommit(TextField* tf, const FieldEditContext& fec,
                    std::function<void(Snapshot&, const std::string&)> writeFn)
{
    tf->SetOnCommit([tf, fec, writeFn]() {
        const std::string value = tf->GetValue();
        CommitFieldPatch(fec, [&](Snapshot& bytes) { writeFn(bytes, value); });
    });
}

// ---- Play-mode live-value refresh ------------------------------------------
//
// Widgets emitted below come from a one-shot byte capture. To keep them live
// while gameplay mutates the component, each in-place-refreshable widget also
// records a FieldRefreshEntry: a thunk that re-reads its field from a fresh
// capture and pushes the value back WITHOUT firing edit callbacks. One
// SimulationRefreshCallbacks entry per component section replays the plan at the
// ~10 Hz sim-refresh cadence (registered at the end of the emit loop). The thunk
// is chosen in the same switch that builds the widget, so no per-field dispatch
// runs on the tick. Each thunk early-outs when the widget already displays the
// fresh value BEFORE calling SetValueWithoutNotify: text-backed fields
// (FloatField/IntField/TextField) reformat unconditionally past Field<T>'s base
// equality check (TextFieldBase::SetValueWithoutNotify, e.g. FloatField builds an
// ostringstream every call), so leaving the compare to the widget would cost an
// idle OceanSurface ~90 string formats per tick. The thunk-level compare keeps an
// unchanged component's replay to comparisons only.
struct FieldRefreshEntry
{
    std::function<void(const Snapshot& bytes, const std::string& focusId)> Apply;
};

using RefreshPlan = std::vector<FieldRefreshEntry>;

// Never overwrite a widget the user is editing. Keyboard/text edits hold focus
// (caught by ContainsFocusedElement); a label drag-scrub does NOT focus the field,
// so it is caught via the shared InspectorDrag drag state.
bool ShouldSkipFieldRefresh(const UIElement* widget, const std::string& focusId)
{
    if (!widget)
        return true;
    if (InspectorUI::ContainsFocusedElement(widget, focusId))
        return true;
    const InspectorDrag::DragState& drag = InspectorDrag::GetDragState();
    return widget == drag.draggingFloatField || widget == drag.draggingIntField ||
           widget == drag.draggingSlider || widget == drag.draggingToggle;
}

void RecordToggleRefresh(RefreshPlan* plan, Toggle* toggle, std::size_t offset)
{
    if (!plan || !toggle)
        return;
    plan->push_back({[toggle, offset](const Snapshot& bytes, const std::string& focusId) {
        if (ShouldSkipFieldRefresh(toggle, focusId))
            return;
        std::uint8_t cur = 0;
        if (!ReadPod(bytes, offset, cur))
            return;
        toggle->SetValueWithoutNotify(cur != 0);
    }});
}

void RecordFloatFieldRefresh(RefreshPlan* plan, FloatField* field, std::size_t offset)
{
    if (!plan || !field)
        return;
    plan->push_back({[field, offset](const Snapshot& bytes, const std::string& focusId) {
        if (ShouldSkipFieldRefresh(field, focusId))
            return;
        float cur = 0.0f;
        if (!ReadPod(bytes, offset, cur))
            return;
        if (field->GetValue() == cur)  // exact: we set precisely what we read last tick
            return;
        field->SetValueWithoutNotify(cur);
    }});
}

void RecordSliderRefresh(RefreshPlan* plan, const InspectorDrag::SliderWithFloatValueRow& row, std::size_t offset)
{
    if (!plan || !row.Slider || !row.ValueField)
        return;
    plan->push_back({[slider = row.Slider, field = row.ValueField, offset](const Snapshot& bytes,
                                                                           const std::string& focusId) {
        if (ShouldSkipFieldRefresh(slider, focusId) || ShouldSkipFieldRefresh(field, focusId))
            return;
        float cur = 0.0f;
        if (!ReadPod(bytes, offset, cur))
            return;
        if (slider->GetValue() == cur)
            return;
        slider->SetValueWithoutNotify(cur);
        field->SetValueWithoutNotify(cur);
    }});
}

void RecordDoubleFieldRefresh(RefreshPlan* plan, FloatField* field, std::size_t offset)
{
    if (!plan || !field)
        return;
    plan->push_back({[field, offset](const Snapshot& bytes, const std::string& focusId) {
        if (ShouldSkipFieldRefresh(field, focusId))
            return;
        double cur = 0.0;
        if (!ReadPod(bytes, offset, cur))
            return;
        const float display = static_cast<float>(cur);
        if (field->GetValue() == display)
            return;
        field->SetValueWithoutNotify(display);
    }});
}

void RecordIntFieldRefresh(RefreshPlan* plan, IntField* field, std::size_t offset, ECS::FieldTypeId type)
{
    if (!plan || !field)
        return;
    plan->push_back({[field, offset, type](const Snapshot& bytes, const std::string& focusId) {
        if (ShouldSkipFieldRefresh(field, focusId))
            return;
        if (offset >= bytes.size())
            return;
        const int cur = ReadIntElement(bytes, offset, type);
        if (field->GetValue() == cur)
            return;
        field->SetValueWithoutNotify(cur);
    }});
}

void RecordUIntTextFieldRefresh(RefreshPlan* plan, TextField* field, std::size_t offset, ECS::FieldTypeId type)
{
    if (!plan || !field)
        return;
    // Cache the last raw value so an idle tick compares one uint64 instead of
    // formatting a string (TextField reformats past the base equality check).
    auto lastValue = std::make_shared<std::optional<std::uint64_t>>();
    plan->push_back({[field, offset, type, lastValue](const Snapshot& bytes, const std::string& focusId) {
        if (ShouldSkipFieldRefresh(field, focusId))
            return;
        std::uint64_t value = 0;
        if (type == ECS::FieldTypeId::UInt32)
        {
            std::uint32_t v = 0;
            if (!ReadPod(bytes, offset, v))
                return;
            value = v;
        }
        else if (!ReadPod(bytes, offset, value))
        {
            return;
        }
        if (lastValue->has_value() && **lastValue == value)
            return;
        *lastValue = value;
        field->SetValueWithoutNotify(std::to_string(value));
    }});
}

void RecordStringFieldRefresh(RefreshPlan* plan, TextField* field, std::size_t offset, std::uint32_t capacity)
{
    if (!plan || !field)
        return;
    plan->push_back({[field, offset, capacity](const Snapshot& bytes, const std::string& focusId) {
        if (ShouldSkipFieldRefresh(field, focusId))
            return;
        if (offset >= bytes.size())
            return;
        const char* text = reinterpret_cast<const char*>(bytes.data() + offset);
        const std::size_t bound = std::min<std::size_t>(
            bytes.size() - offset, capacity != 0 ? capacity : (bytes.size() - offset));
        std::size_t len = 0;
        while (len < bound && text[len] != '\0')
            ++len;
        const std::string_view fresh(text, len);
        if (field->GetValue() == fresh)  // avoid the reformat + inner SetValue churn
            return;
        field->SetValueWithoutNotify(std::string(fresh));
    }});
}

// One float drag row editing the float at `offset` (one undo entry per drag/commit).
// Shared by the scalar Float case and the Vec/Color composite rows; multi-entity
// fan-out is inherited from WireEdit. `mixed` renders the field indeterminate.
void EmitFloatRow(UIElement* parent, RefreshPlan* plan, const Snapshot& bytes,
                  const FieldEditContext& fecBase,
                  const std::string& label, std::size_t offset, bool mixed, bool& any,
                  bool hasRange = false, float minValue = 0.0f, float maxValue = 0.0f,
                  bool useSlider = false, const char* tooltip = nullptr)
{
    float cur = 0.0f;
    ReadPod(bytes, offset, cur);

    FieldEditContext fec = fecBase;
    fec.Label = label;
    auto editState = std::make_shared<std::optional<Editor::UndoRedoService::InteractiveEdit>>();
    std::function<void(float)> onChanging, onChanged;
    WireEdit<float>(editState, fec,
        [offset, hasRange, minValue, maxValue](Snapshot& b, float v) {
            if (offset + sizeof(float) > b.size())
                return;
            const float stored = hasRange ? std::clamp(v, minValue, maxValue) : v;
            std::memcpy(b.data() + offset, &stored, sizeof(float));
        },
        onChanging, onChanged);
    // A ranged float that opts into a slider (useSlider — e.g. the per-octave FFT
    // spectrum arrays) renders as a real horizontal slider so the band-by-band shape
    // is visible. Ranged scalars keep the clamped drag-to-adjust field (the value
    // can't leave the source-matched bounds); unranged floats keep the plain drag
    // field. (The slider has no mixed-value display; multi-edit shows the primary.)
    if (hasRange && useSlider)
    {
        const InspectorDrag::SliderWithFloatValueRow row = InspectorDrag::AddFloatSliderRow(
            parent, fec.Label, cur, minValue, maxValue, std::move(onChanging), std::move(onChanged), tooltip);
        RecordSliderRefresh(plan, row, offset);
    }
    else
    {
        FloatField* f = hasRange
            ? InspectorDrag::AddFloatRowWithDrag(parent, fec.Label, cur, std::move(onChanging),
                                                 std::move(onChanged),
                                                 std::numeric_limits<float>::quiet_NaN(), tooltip,
                                                 minValue, maxValue)
            : InspectorDrag::AddFloatRowWithDrag(parent, fec.Label, cur, std::move(onChanging),
                                                 std::move(onChanged),
                                                 std::numeric_limits<float>::quiet_NaN(), tooltip);
        if (mixed && f)
            f->SetMixed();
        RecordFloatFieldRefresh(plan, f, offset);
    }
    any = true;
}

const ECS::FieldInfo* FindField(std::span<const ECS::FieldInfo> fields, std::string_view name)
{
    for (const ECS::FieldInfo& candidate : fields)
    {
        if (candidate.Name == name)
            return &candidate;
    }
    return nullptr;
}

bool IsOceanSurfaceType(ECS::ComponentTypeId typeId)
{
    return typeId == ECS::ComponentFieldRegistry::FindByName("OceanSurface");
}

bool EmitPlanarReflectionControlsRow(UIElement* parent,
                                     const Snapshot& bytes,
                                     const std::vector<Snapshot>& extraBytes,
                                     const FieldEditContext& fecBase,
                                     ECS::ComponentTypeId typeId,
                                     std::span<const ECS::FieldInfo> fields,
                                     const ECS::FieldInfo& field,
                                     const char* tooltip = nullptr)
{
    if (!IsOceanSurfaceType(typeId) || field.Name != "PlanarReflections" ||
        field.Type != ECS::FieldTypeId::Bool)
    {
        return false;
    }

    const ECS::FieldInfo* strengthField = FindField(fields, "PlanarReflectionStrength");
    if (!strengthField || strengthField->Type != ECS::FieldTypeId::Float)
        return false;

    const std::size_t toggleOffset = field.Offset;
    const std::size_t strengthOffset = strengthField->Offset;

    std::uint8_t toggleValue = 0;
    ReadPod(bytes, toggleOffset, toggleValue);
    float strengthValue = 1.0f;
    ReadPod(bytes, strengthOffset, strengthValue);
    strengthValue = std::clamp(strengthValue, 0.0f, 1.0f);

    UIElement* row = InspectorUI::AddRow(parent);
    Label* label = InspectorUI::AddLabel(row, std::string(FieldDisplayName(field.Name)), tooltip);
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(10.0f));

    FieldEditContext toggleFec = fecBase;
    toggleFec.Label = std::string(FieldDisplayName(field.Name));
    auto toggleEditState = std::make_shared<std::optional<Editor::UndoRedoService::InteractiveEdit>>();
    std::function<void(bool)> onToggleChanging;
    std::function<void(bool)> onToggleChanged;
    WireEdit<bool>(toggleEditState, toggleFec,
        [toggleOffset](Snapshot& b, bool v) {
            if (toggleOffset < b.size())
                b[toggleOffset] = v ? 1 : 0;
        },
        onToggleChanging,
        onToggleChanged);

    Toggle* toggle = InspectorUI::AddToggle(fieldContainer, toggleValue != 0);
    if (tooltip)
        toggle->SetTooltip(tooltip);
    auto labelToggleChanged = onToggleChanged;
    toggle->SetOnValueChanged(std::move(onToggleChanged));
    InspectorDrag::SetupLabelDragToggle(label, toggle,
        [toggle, cb = std::move(labelToggleChanged)]() {
            if (cb)
                cb(toggle->GetValue());
        });
    if (SpanDiffers(bytes, extraBytes, toggleOffset, 1))
        toggle->SetMixed();

    FieldEditContext strengthFec = fecBase;
    strengthFec.Label = std::string(FieldDisplayName(strengthField->Name));
    auto strengthEditState = std::make_shared<std::optional<Editor::UndoRedoService::InteractiveEdit>>();
    std::function<void(float)> onStrengthChanging;
    std::function<void(float)> onStrengthChanged;
    WireEdit<float>(strengthEditState, strengthFec,
        [strengthOffset](Snapshot& b, float v) {
            const float stored = std::clamp(v, 0.0f, 1.0f);
            if (strengthOffset + sizeof(float) <= b.size())
                std::memcpy(b.data() + strengthOffset, &stored, sizeof(float));
        },
        onStrengthChanging,
        onStrengthChanged);

    auto sliderOwned = std::make_unique<Slider>();
    Slider* slider = sliderOwned.get();
    slider->AddClass("property-slider");
    slider->SetMin(0.0f);
    slider->SetMax(1.0f);
    slider->SetShowValueBubble(true);
    slider->SetValueWithoutNotify(strengthValue);
    slider->Overrides()
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::MinWidth, StyleLength::Px(80.0f));
    if (const char* strengthTooltip = OceanSurfaceFieldTooltip(strengthField->Name))
        slider->SetTooltip(strengthTooltip);
    slider->SetOnValueChanging(
        [cb = std::move(onStrengthChanging)](const float& v) {
            if (cb)
                cb(v);
        });
    slider->SetOnValueChanged(
        [cb = std::move(onStrengthChanged)](const float& v) {
            if (cb)
                cb(v);
        });
    fieldContainer->AddChild(std::move(sliderOwned));

    return true;
}

// Emit one float drag row per axis of a Vec2/3/4 or Color composite at baseOffset.
// Each axis's mixed state is computed from its own 4-byte span across the selection.
void EmitFloatAxisRows(UIElement* parent, RefreshPlan* plan, const Snapshot& bytes,
                       const std::vector<Snapshot>& extraBytes,
                       const FieldEditContext& fecBase, const std::string& baseLabel, std::size_t baseOffset,
                       std::uint32_t axisCount, bool isColor, bool& any, const char* tooltip = nullptr)
{
    static const char* const kVecAxes[] = {"X", "Y", "Z", "W"};
    static const char* const kColorAxes[] = {"R", "G", "B", "A"};
    const char* const* axes = isColor ? kColorAxes : kVecAxes;

    for (std::uint32_t a = 0; a < axisCount; ++a)
    {
        const std::size_t axisOffset = baseOffset + static_cast<std::size_t>(a) * sizeof(float);
        const bool mixed = SpanDiffers(bytes, extraBytes, axisOffset, sizeof(float));
        EmitFloatRow(parent, plan, bytes, fecBase, baseLabel + "." + axes[a], axisOffset, mixed, any,
                     false, 0.0f, 0.0f, false, tooltip);
    }
}

// Convert RGBA [0,1] floats to 0xAARRGGBB and back, for the Color swatch drawer.
std::uint32_t ColorFloatsToArgb(const float* c)
{
    auto q = [](float v) { return static_cast<std::uint8_t>(std::max(0.0f, std::min(1.0f, v)) * 255.0f + 0.5f); };
    return (0xFFu << 24) | (static_cast<std::uint32_t>(q(c[0])) << 16)
         | (static_cast<std::uint32_t>(q(c[1])) << 8) | static_cast<std::uint32_t>(q(c[2]));
}

void StyleColorSwatch(UIElement* swatch, std::uint32_t argb)
{
    constexpr float kSize = 20.0f;
    constexpr std::uint32_t kBorder = 0xFF555555u;
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(kSize))
        .Set(Style::Height, StyleLength::Px(kSize))
        .Set(Style::MinWidth, StyleLength::Px(kSize))
        .Set(Style::MinHeight, StyleLength::Px(kSize))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{3.0f, 3.0f, 3.0f, 3.0f})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{kBorder, kBorder, kBorder, kBorder})
        .Set(Style::BackgroundColor, argb)
        .Set(Style::Cursor, CursorStyle::Pointer);
}

// Emit a Color (RGBA float4 at baseOffset) as a swatch + RGB label. Clicking the
// swatch opens the shared color picker; dragging previews live and Apply commits
// one undo entry (multi-entity aware).
void EmitColorSwatchRow(UIElement* parent, RefreshPlan* plan, const Snapshot& bytes,
                        const FieldEditContext& fecBase,
                        const std::string& label, std::size_t baseOffset,
                        const OpenColorPickerWindowFn& openPicker, bool& any,
                        const char* tooltip = nullptr)
{
    std::array<float, 4> rgba{0.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 4; ++i)
        ReadPod(bytes, baseOffset + static_cast<std::size_t>(i) * sizeof(float), rgba[i]);

    UIElement* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, label, tooltip ? tooltip : "Click the swatch to open the color picker");
    UIElement* fc = InspectorUI::AddFieldContainer(row);
    fc->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));

    const std::uint32_t argb = ColorFloatsToArgb(rgba.data());
    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    StyleColorSwatch(swatchRaw, argb);
    if (tooltip)
        swatchRaw->SetTooltip(tooltip);
    fc->AddChild(std::move(swatch));

    char buf[48];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", rgba[0], rgba[1], rgba[2]);
    auto rgbLabel = std::make_unique<Label>();
    rgbLabel->AddClass("inspector-text");
    rgbLabel->SetText(buf);
    rgbLabel->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
    if (tooltip)
        rgbLabel->SetTooltip(tooltip);
    Label* rgbLabelRaw = rgbLabel.get();
    fc->AddChild(std::move(rgbLabel));

    FieldEditContext fec = fecBase;
    fec.Label = label;
    // The picker's callbacks outlive an inspector rebuild: resolve the row through weak
    // refs instead of holding the freed widgets.
    auto clickHandler = [fec, baseOffset, openPicker, swatchRef = UIElement::MakeWeakRef(swatchRaw),
                         rgbLabelRef = UIElement::MakeWeakRef(rgbLabelRaw), argb, rgba](UIEvent& ev) {
        if (ev.Button != 0)
            return;
        ev.Stop();
        if (!openPicker)
            return;
        ColorPickerCallbacks cbs;
        auto editState = std::make_shared<std::optional<Editor::UndoRedoService::InteractiveEdit>>();
        auto writeColor = [baseOffset](Snapshot& b, const std::array<float, 4>& nc) {
            for (int i = 0; i < 4; ++i)
            {
                const std::size_t o = baseOffset + static_cast<std::size_t>(i) * sizeof(float);
                if (o + sizeof(float) <= b.size())
                    std::memcpy(b.data() + o, &nc[i], sizeof(float));
            }
        };
        auto applyLive = [fec, writeColor](const std::array<float, 4>& nc) {
            const auto applyOne = [&](ECS::EntityHandle e) {
                Snapshot bytes;
                if (!fec.World || !fec.World->CaptureComponentBytes(e, fec.TypeId, bytes))
                    return;
                writeColor(bytes, nc);
                fec.World->ApplyComponentBytesImmediate(e, fec.TypeId, bytes);
            };
            applyOne(fec.Entity);
            for (ECS::EntityHandle e : fec.Extras)
                applyOne(e);
        };
        auto updateUi = [swatchRef, rgbLabelRef](std::uint32_t newArgb, const std::array<float, 4>& nc) {
            UIElement* swatch = swatchRef.Get();
            Label* rgbLabel = rgbLabelRef.Get();
            if (!swatch || !rgbLabel)
                return;
            StyleColorSwatch(swatch, newArgb | 0xFF000000u);
            char b2[48];
            std::snprintf(b2, sizeof(b2), "(%.2f, %.2f, %.2f)", nc[0], nc[1], nc[2]);
            rgbLabel->SetText(b2);
        };
        auto argbToColor = [](std::uint32_t newArgb, float intensity) {
            const float scale = std::max(1.0f, intensity);
            std::array<float, 4> out{};
            out[0] = (((newArgb >> 16) & 0xFF) / 255.0f) * scale;
            out[1] = (((newArgb >> 8) & 0xFF) / 255.0f) * scale;
            out[2] = ((newArgb & 0xFF) / 255.0f) * scale;
            out[3] = ((newArgb >> 24) & 0xFF) / 255.0f;
            return out;
        };
        cbs.onValueChanging = [fec, editState, applyLive, updateUi, argbToColor](std::uint32_t newArgb, float intensity) {
            const std::array<float, 4> nc = argbToColor(newArgb, intensity);
            if (!fec.Undo)
            {
                applyLive(nc);
            }
            else
            {
                if (!editState->has_value())
                    editState->emplace(fec.Undo->BeginInteractiveEdit(fec.Label, MakeEditTarget(fec)));
                editState->value().Preview([&]() { applyLive(nc); });
            }
            updateUi(newArgb, nc);
        };
        cbs.onApply = [fec, editState, applyLive, updateUi, argbToColor, writeColor](std::uint32_t newArgb, float intensity) {
            const std::array<float, 4> nc = argbToColor(newArgb, intensity);
            if (!fec.Undo)
            {
                applyLive(nc);
            }
            else if (editState->has_value())
            {
                editState->value().Preview([&]() { applyLive(nc); });
                auto edit = std::move(editState->value());
                editState->reset();
                edit.Commit();
            }
            else
            {
                CommitFieldPatch(fec, [&](Snapshot& b) { writeColor(b, nc); });
            }
            updateUi(newArgb, nc);
        };
        cbs.onCancel = [fec, editState, applyLive, updateUi, argb, rgba]() {
            if (editState->has_value())
            {
                auto edit = std::move(editState->value());
                editState->reset();
                edit.Cancel();
            }
            else if (!fec.Undo)
            {
                applyLive(rgba);
            }
            updateUi(argb, rgba);
        };
        openPicker(argb, 1.0f, std::move(cbs));
    };
    swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
    rgbLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);

    // Live refresh: re-tint the swatch and re-label the RGB text from a fresh
    // capture. The swatch/label are not editable widgets (no focus/drag to skip).
    // The cache compares the DISPLAYED form — the quantized 0xAARRGGBB swatch tint
    // plus the %.2f label string — not the raw floats, so a continuously-animated
    // HDR colour whose display is unchanged skips the StyleColorSwatch restyle
    // (which marks the swatch dirty unconditionally). Known HDR divergence: the
    // picker tints the swatch from raw ARGB while this recomputes a clamped tint
    // from the intensity-scaled floats, so a >1.0 intensity colour can show a
    // slightly different swatch here than in the open picker — cosmetic only, the
    // committed bytes are identical.
    if (plan)
    {
        auto lastShown = std::make_shared<std::optional<std::pair<std::uint32_t, std::string>>>();
        plan->push_back({[swatchRaw, rgbLabelRaw, baseOffset, lastShown](const Snapshot& b,
                                                                         const std::string&) {
            std::array<float, 4> nc{0.0f, 0.0f, 0.0f, 1.0f};
            for (int i = 0; i < 4; ++i)
            {
                if (!ReadPod(b, baseOffset + static_cast<std::size_t>(i) * sizeof(float), nc[i]))
                    return;
            }
            const std::uint32_t argb = ColorFloatsToArgb(nc.data());
            char buf[48];
            std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", nc[0], nc[1], nc[2]);
            std::string label(buf);
            if (lastShown->has_value() && lastShown->value().first == argb &&
                lastShown->value().second == label)
                return;
            StyleColorSwatch(swatchRaw, argb);
            rgbLabelRaw->SetText(label);
            *lastShown = std::make_pair(argb, std::move(label));
        }});
    }
    any = true;
}

// Emit a Quaternion composite at baseOffset as three Euler XYZ degree rows
// (Unity/Unreal-style). Display derives Euler from the stored quaternion; each
// commit re-derives Euler from the live quaternion, sets one axis, and writes the
// rebuilt quaternion back — so the stored bytes stay an exact quaternion.
//
// Two known limitations, acceptable while no component carries a quaternion field
// (so this path has no live consumer yet): (1) editing one axis can shift the
// others' displayed degrees until the rows rebuild — the usual Euler-in-inspector
// ambiguity; (2) under multi-selection this sets the same ABSOLUTE angle on every
// entity (WireEdit's broadcast), unlike TransformInspector which applies a relative
// delta. Revisit both when a real quaternion field ships.
void EmitQuatEulerRows(UIElement* parent, RefreshPlan* plan, const Snapshot& bytes,
                       const std::vector<Snapshot>& extraBytes,
                       const FieldEditContext& fecBase, const std::string& baseLabel,
                       std::size_t baseOffset, bool& any, const char* tooltip = nullptr)
{
    using Mathematics::Quaternion;
    static const char* const kEulerAxes[] = {"X", "Y", "Z"};

    Quaternion q;
    ReadPod(bytes, baseOffset, q);
    float euler[3] = {0.0f, 0.0f, 0.0f};
    Components::EulerXYZDegreesFromQuaternion(q, euler[0], euler[1], euler[2]);

    // The Euler axes are derived from the whole quaternion, so all three rows
    // share one mixed state keyed on the 16-byte quaternion span.
    const bool mixed = SpanDiffers(bytes, extraBytes, baseOffset, sizeof(Quaternion));

    FloatField* axisFields[3] = {nullptr, nullptr, nullptr};
    for (std::uint32_t a = 0; a < 3; ++a)
    {
        FieldEditContext fec = fecBase;
        fec.Label = baseLabel + "." + kEulerAxes[a];
        auto editState = std::make_shared<std::optional<Editor::UndoRedoService::InteractiveEdit>>();
        std::function<void(float)> onChanging, onChanged;
        WireEdit<float>(editState, fec,
            [baseOffset, a](Snapshot& b, float deg) {
                if (baseOffset + sizeof(Quaternion) > b.size())
                    return;
                Quaternion cur;
                std::memcpy(&cur, b.data() + baseOffset, sizeof(Quaternion));
                float e[3];
                Components::EulerXYZDegreesFromQuaternion(cur, e[0], e[1], e[2]);
                e[a] = deg;
                const Quaternion next = Components::QuaternionFromEulerXYZDegrees(e[0], e[1], e[2]);
                std::memcpy(b.data() + baseOffset, &next, sizeof(Quaternion));
            },
            onChanging, onChanged);
        FloatField* f = InspectorDrag::AddFloatRowWithDrag(parent, fec.Label, euler[a],
                                                           std::move(onChanging), std::move(onChanged),
                                                           std::numeric_limits<float>::quiet_NaN(),
                                                           tooltip);
        if (mixed && f)
            f->SetMixed();
        axisFields[a] = f;
        any = true;
    }

    // Editing one Euler axis re-derives the whole quaternion (and can shift the
    // other axes' display), so this is a whole-composite refresh: skip all three
    // rows if the user is on any of them, else re-derive Euler exactly like above.
    if (plan && axisFields[0] && axisFields[1] && axisFields[2])
    {
        FloatField* fx = axisFields[0];
        FloatField* fy = axisFields[1];
        FloatField* fz = axisFields[2];
        plan->push_back({[fx, fy, fz, baseOffset](const Snapshot& b, const std::string& focusId) {
            if (ShouldSkipFieldRefresh(fx, focusId) || ShouldSkipFieldRefresh(fy, focusId) ||
                ShouldSkipFieldRefresh(fz, focusId))
                return;
            if (baseOffset + sizeof(Quaternion) > b.size())
                return;
            Quaternion cur;
            std::memcpy(&cur, b.data() + baseOffset, sizeof(Quaternion));
            float e[3] = {0.0f, 0.0f, 0.0f};
            Components::EulerXYZDegreesFromQuaternion(cur, e[0], e[1], e[2]);
            // The quat -> Euler round-trip is lossy (a committed 90 re-derives as
            // 89.999996), so exact equality would visibly rewrite the row one tick
            // after every commit. Same 1e-4 degree epsilon as TransformInspector's
            // rotation rows.
            constexpr float kEulerRefreshEpsilonDeg = 1.0e-4f;
            if (std::fabs(fx->GetValue() - e[0]) > kEulerRefreshEpsilonDeg)
                fx->SetValueWithoutNotify(e[0]);
            if (std::fabs(fy->GetValue() - e[1]) > kEulerRefreshEpsilonDeg)
                fy->SetValueWithoutNotify(e[1]);
            if (std::fabs(fz->GetValue() - e[2]) > kEulerRefreshEpsilonDeg)
                fz->SetValueWithoutNotify(e[2]);
        }});
    }
}

// Sentinel option value for the multi-edit "mixed" state. The Dropdown forces a
// non-empty option list to a real selection (SetOptions clamps index -1 -> 0), so
// "no agreed value" is represented by a prepended sentinel rather than -1. Its value
// string never parses to a table index, so picking a real option always commits.
constexpr const char* kEnumMixedOptionValue = "__mixed__";
constexpr const char* kPresetCustomOptionValue = "__custom__";

struct UIntPreset
{
    std::uint32_t Value = 0;
    const char* Label = "";
};

struct FloatPreset
{
    float Value = 0.0f;
    const char* Label = "";
};

constexpr UIntPreset kOceanCascadeResolutionPresets[] = {
    {0u, "Default"},
    {128u, "128"},
    {256u, "256"},
    {384u, "384"},
    {512u, "512"},
    {1024u, "1024"},
};

constexpr UIntPreset kOceanRasterDepthResolutionPresets[] = {
    {256u, "256"},
    {512u, "512"},
    {1024u, "1024"},
    {2048u, "2048"},
    {4096u, "4096"},
};

constexpr UIntPreset kReflectionProbeResolutionPresets[] = {
    {64u, "64"},
    {128u, "128"},
    {256u, "256"},
    {512u, "512"},
    {1024u, "1024"},
};

constexpr UIntPreset kOceanFoamDebugModePresets[] = {
    {0u, "Normal"},
    {1u, "Combined Foam"},
    {2u, "Wave Foam"},
    {3u, "Shoreline / Contact"},
    {4u, "Freshness / Age"},
    {5u, "Latest Deposit"},
    {6u, "Seabed Depth Ramp"},
    {7u, "Shallow Mask"},
    {8u, "Invalid / Deep Tiles"},
};

constexpr FloatPreset kOceanMinScalePresets[] = {
    {8.0f, "8"},
    {16.0f, "16"},
    {32.0f, "32"},
    {64.0f, "64"},
    {128.0f, "128"},
    {256.0f, "256"},
};

constexpr FloatPreset kOceanMaxScalePresets[] = {
    {64.0f, "64"},
    {128.0f, "128"},
    {256.0f, "256"},
    {512.0f, "512"},
    {1024.0f, "1024"},
    {2048.0f, "2048"},
    {4096.0f, "4096"},
};

constexpr FloatPreset kOceanPlanarReflectionScalePresets[] = {
    {1.0f, "1x"},
    {0.75f, "0.75x"},
    {0.5f, "0.5x"},
    {0.25f, "0.25x"},
    {0.125f, "0.125x"},
};

std::span<const UIntPreset> ComponentUIntPresetsForField(ECS::ComponentTypeId componentTypeId,
                                                         std::string_view fieldName)
{
    const ECS::ComponentTypeId reflectionProbeId =
        ECS::ComponentFieldRegistry::FindByName("ReflectionProbe");
    if (componentTypeId == reflectionProbeId && fieldName == "CaptureResolution")
        return kReflectionProbeResolutionPresets;

    const ECS::ComponentTypeId oceanRendererId =
        ECS::ComponentFieldRegistry::FindByName("OceanRenderer");
    if (componentTypeId == oceanRendererId)
    {
        if (fieldName == "LodDataResolution")
            return kOceanCascadeResolutionPresets;
        if (fieldName == "RasterDepthCaptureResolution")
            return kOceanRasterDepthResolutionPresets;
    }

    const ECS::ComponentTypeId oceanSurfaceId =
        ECS::ComponentFieldRegistry::FindByName("OceanSurface");
    if (componentTypeId == oceanSurfaceId && fieldName == "FoamDebugMode")
        return kOceanFoamDebugModePresets;
    return {};
}

std::span<const FloatPreset> OceanScalePresetsForField(ECS::ComponentTypeId componentTypeId,
                                                       std::string_view fieldName)
{
    const ECS::ComponentTypeId oceanRendererId =
        ECS::ComponentFieldRegistry::FindByName("OceanRenderer");
    if (componentTypeId == oceanRendererId)
    {
        if (fieldName == "MinScale")
            return kOceanMinScalePresets;
        if (fieldName == "MaxScale")
            return kOceanMaxScalePresets;
    }

    const ECS::ComponentTypeId oceanSurfaceId =
        ECS::ComponentFieldRegistry::FindByName("OceanSurface");
    if (componentTypeId == oceanSurfaceId && fieldName == "PlanarReflectionScale")
        return kOceanPlanarReflectionScalePresets;
    return {};
}

std::string FormatPresetFloat(float value)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    std::string text(buffer);
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text.empty() ? "0" : text;
}

// Render a scanner-detected enum field as a name Dropdown instead of a raw number.
// The committed value is the EnumNameValue::Value behind the chosen label; the
// option payload carries the table index so the change callback can map back.
// Commits as ONE undo entry via CommitFieldPatch (a discrete selection, like the
// EntityHandle/text paths — not a drag gesture). Returns false (caller falls back
// to the numeric field) when the stored value is outside the table.
bool EmitEnumDropdownRow(UIElement* parent, RefreshPlan* plan, const Snapshot& bytes,
                         const std::vector<Snapshot>& extraBytes,
                         const FieldEditContext& fecBase, const ECS::FieldInfo& field,
                         const char* tooltip = nullptr)
{
    const std::size_t offset = field.Offset;
    const ECS::FieldTypeId fieldType = field.Type;
    const std::int64_t current = ReadIntElement64(bytes, offset, fieldType);
    const int currentIndex = EnumIndexForValue(field.EnumNames, current);
    const bool mixed = SpanDiffers(bytes, extraBytes, offset, field.Size);

    // An out-of-table value (and not the mixed case) has no label to show — defer to
    // the numeric field so the raw value stays visible and editable.
    if (currentIndex < 0 && !mixed)
        return false;

    std::vector<Dropdown::Option> options;
    options.reserve(field.EnumNames.size() + 1);
    int selectedIndex = 0;
    if (mixed)
    {
        options.push_back({kEnumMixedOptionValue, "\xE2\x80\x94"});  // em dash placeholder
        selectedIndex = 0;
    }
    for (std::size_t i = 0; i < field.EnumNames.size(); ++i)
    {
        options.push_back(
            {std::to_string(i), std::string(EnumDisplayName(field.Name, field.EnumNames[i].Name))});
        if (!mixed && static_cast<int>(i) == currentIndex)
            selectedIndex = static_cast<int>(options.size()) - 1;
    }

    FieldEditContext fec = fecBase;
    fec.Label = std::string(FieldDisplayName(field.Name));
    Dropdown* dd = InspectorUI::AddDropdownRow(parent, fec.Label, options, selectedIndex, tooltip);
    if (!dd)
        return false;

    // Capture the enum table by value into a small vector so the callback owns it
    // (FieldInfo::EnumNames is a view over a constexpr table that outlives the
    // inspector, but copying the small {value} list keeps the closure self-contained).
    std::vector<std::int64_t> values;
    values.reserve(field.EnumNames.size());
    for (const ECS::EnumNameValue& e : field.EnumNames)
        values.push_back(e.Value);

    // Wire AFTER SetOptions (done inside AddDropdownRow): SetSelectedIndex fires
    // NotifyValueChanged on the initial selection, and wiring first would commit a
    // spurious no-op edit on every rebuild.
    dd->SetOnValueChanged([fec, offset, fieldType, values](const std::string& optionValue) {
        if (optionValue == kEnumMixedOptionValue)
            return;  // selecting the sentinel itself is a no-op
        int idx = -1;
        try { idx = std::stoi(optionValue); } catch (...) { return; }
        if (idx < 0 || idx >= static_cast<int>(values.size()))
            return;
        const std::int64_t newValue = values[static_cast<std::size_t>(idx)];
        CommitFieldPatch(fec, [offset, fieldType, newValue](Snapshot& b) {
            WriteIntElement64(b, offset, fieldType, newValue);
        });
    });

    // Play-mode live refresh (single selection only — plan is null for
    // multi-select, so the mixed layout with its em-dash sentinel never reaches
    // here). The non-mixed layout has exactly one option per enum name, so the
    // option index equals the table index; map the fresh value back the same way
    // emit did, via EnumIndexForValue over the same table.
    if (plan && !mixed)
    {
        const std::span<const ECS::EnumNameValue> enumNames = field.EnumNames;  // view over constexpr data; outlives the inspector
        plan->push_back({[dd, offset, fieldType, enumNames](const Snapshot& b, const std::string& focusId) {
            // Leave the widget alone while the user is interacting: focused/dragging
            // (ShouldSkipFieldRefresh) or with the popup open (IsMenuOpen) — the same
            // skip philosophy as a focused text field.
            if (ShouldSkipFieldRefresh(dd, focusId) || dd->IsMenuOpen())
                return;
            const std::int64_t cur = ReadIntElement64(b, offset, fieldType);
            const int tableIndex = EnumIndexForValue(enumNames, cur);
            // Fresh value left the enum table: emit would have fallen back to the
            // numeric field, which we cannot switch to post-build, so keep the last
            // valid label rather than blanking the row.
            if (tableIndex < 0)
                return;
            dd->SetSelectedIndexWithoutNotify(tableIndex);
        }});
    }
    return true;
}

bool EmitUIntPresetDropdownRow(UIElement* parent, RefreshPlan* plan, const Snapshot& bytes,
                               const std::vector<Snapshot>& extraBytes,
                               const FieldEditContext& fecBase,
                               const ECS::FieldInfo& field,
                               std::span<const UIntPreset> presets,
                               const char* tooltip = nullptr)
{
    if (presets.empty() || field.Type != ECS::FieldTypeId::UInt32)
        return false;

    const std::size_t offset = field.Offset;
    std::uint32_t current = 0;
    ReadPod(bytes, offset, current);
    const bool mixed = SpanDiffers(bytes, extraBytes, offset, field.Size);

    std::vector<Dropdown::Option> options;
    options.reserve(presets.size() + 2);
    int selectedIndex = 0;

    if (mixed)
    {
        options.push_back({kEnumMixedOptionValue, "\xE2\x80\x94"});
        selectedIndex = 0;
    }

    bool matchedPreset = false;
    for (const UIntPreset& preset : presets)
    {
        if (!mixed && preset.Value == current)
            matchedPreset = true;
    }
    if (!mixed && !matchedPreset)
    {
        options.push_back({kPresetCustomOptionValue,
                           "Custom (" + std::to_string(current) + ")"});
        selectedIndex = 0;
    }

    for (const UIntPreset& preset : presets)
    {
        options.push_back({std::to_string(preset.Value), preset.Label});
        if (!mixed && matchedPreset && preset.Value == current)
            selectedIndex = static_cast<int>(options.size()) - 1;
    }

    FieldEditContext fec = fecBase;
    fec.Label = std::string(FieldDisplayName(field.Name));
    Dropdown* dd = InspectorUI::AddDropdownRow(parent, fec.Label, options, selectedIndex, tooltip);
    if (!dd)
        return false;

    dd->SetOnValueChanged([fec, offset](const std::string& optionValue) {
        if (optionValue == kEnumMixedOptionValue || optionValue == kPresetCustomOptionValue)
            return;
        std::uint64_t parsed = 0;
        try { parsed = std::stoull(optionValue, nullptr, 10); } catch (...) { return; }
        if (parsed > std::numeric_limits<std::uint32_t>::max())
            return;
        const std::uint32_t newValue = static_cast<std::uint32_t>(parsed);
        CommitFieldPatch(fec, [offset, newValue](Snapshot& b) {
            WriteUIntElement(b, offset, ECS::FieldTypeId::UInt32, newValue);
        });
    });

    // Play-mode live refresh (single selection only). Map the fresh value back to
    // a preset option exactly as emit did. presetOffset accounts for the baked
    // "Custom (x)" slot at index 0, present only when the initial value matched
    // no preset.
    if (plan && !mixed)
    {
        std::vector<std::uint32_t> presetValues;
        presetValues.reserve(presets.size());
        for (const UIntPreset& preset : presets)
            presetValues.push_back(preset.Value);
        const int presetOffset = matchedPreset ? 0 : 1;
        plan->push_back({[dd, offset, presetValues, presetOffset](const Snapshot& b, const std::string& focusId) {
            if (ShouldSkipFieldRefresh(dd, focusId) || dd->IsMenuOpen())
                return;
            std::uint32_t cur = 0;
            if (!ReadPod(b, offset, cur))
                return;
            for (std::size_t i = 0; i < presetValues.size(); ++i)
            {
                if (presetValues[i] == cur)
                {
                    dd->SetSelectedIndexWithoutNotify(presetOffset + static_cast<int>(i));
                    return;
                }
            }
            // Fresh value matches no preset. Its home is the "Custom (x)" slot, whose
            // label is baked at build time; Dropdown has no per-option relabel and
            // rebuilding the option set would re-fire the commit, so leave the row as
            // baked until the next inspector rebuild (same limitation the
            // AssetGuid/EntityHandle pickers carry).
        }});
    }
    return true;
}

bool EmitFloatPresetDropdownRow(UIElement* parent, RefreshPlan* plan, const Snapshot& bytes,
                                const std::vector<Snapshot>& extraBytes,
                                const FieldEditContext& fecBase,
                                const ECS::FieldInfo& field,
                                std::span<const FloatPreset> presets,
                                const char* tooltip = nullptr)
{
    if (presets.empty() || field.Type != ECS::FieldTypeId::Float)
        return false;

    const std::size_t offset = field.Offset;
    float current = 0.0f;
    ReadPod(bytes, offset, current);
    const bool mixed = SpanDiffers(bytes, extraBytes, offset, field.Size);

    std::vector<Dropdown::Option> options;
    options.reserve(presets.size() + 2);
    int selectedIndex = 0;

    if (mixed)
    {
        options.push_back({kEnumMixedOptionValue, "\xE2\x80\x94"});
        selectedIndex = 0;
    }

    bool matchedPreset = false;
    for (const FloatPreset& preset : presets)
    {
        if (!mixed && preset.Value == current)
            matchedPreset = true;
    }
    if (!mixed && !matchedPreset)
    {
        options.push_back({kPresetCustomOptionValue,
                           "Custom (" + FormatPresetFloat(current) + ")"});
        selectedIndex = 0;
    }

    for (const FloatPreset& preset : presets)
    {
        options.push_back({FormatPresetFloat(preset.Value), preset.Label});
        if (!mixed && matchedPreset && preset.Value == current)
            selectedIndex = static_cast<int>(options.size()) - 1;
    }

    FieldEditContext fec = fecBase;
    fec.Label = std::string(FieldDisplayName(field.Name));
    Dropdown* dd = InspectorUI::AddDropdownRow(parent, fec.Label, options, selectedIndex, tooltip);
    if (!dd)
        return false;

    dd->SetOnValueChanged([fec, offset](const std::string& optionValue) {
        if (optionValue == kEnumMixedOptionValue || optionValue == kPresetCustomOptionValue)
            return;
        float newValue = 0.0f;
        try { newValue = std::stof(optionValue); } catch (...) { return; }
        CommitFieldPatch(fec, [offset, newValue](Snapshot& b) {
            if (offset + sizeof(newValue) <= b.size())
                std::memcpy(b.data() + offset, &newValue, sizeof(newValue));
        });
    });

    // Play-mode live refresh (single selection only). Same mapping as the emit
    // path (exact float compare); presetOffset accounts for the baked
    // "Custom (x)" slot at index 0, present only when the initial value matched
    // no preset.
    if (plan && !mixed)
    {
        std::vector<float> presetValues;
        presetValues.reserve(presets.size());
        for (const FloatPreset& preset : presets)
            presetValues.push_back(preset.Value);
        const int presetOffset = matchedPreset ? 0 : 1;
        plan->push_back({[dd, offset, presetValues, presetOffset](const Snapshot& b, const std::string& focusId) {
            if (ShouldSkipFieldRefresh(dd, focusId) || dd->IsMenuOpen())
                return;
            float cur = 0.0f;
            if (!ReadPod(b, offset, cur))
                return;
            for (std::size_t i = 0; i < presetValues.size(); ++i)
            {
                if (presetValues[i] == cur)  // exact match, mirroring the emit path
                {
                    dd->SetSelectedIndexWithoutNotify(presetOffset + static_cast<int>(i));
                    return;
                }
            }
            // No preset matches: the baked "Custom (x)" label can't be refreshed
            // without rebuilding the options (which re-fires the commit), so leave
            // the row until the next inspector rebuild.
        }});
    }
    return true;
}

// Components whose fields are large arrays render one row per element; cap the
// element rows so an accidentally-reflected blob (e.g. a 64-float matrix) does
// not spam the inspector. The starter set stays well under this.
constexpr std::uint32_t kMaxElementRows = 16;

// Asset categories the picker / drop-slot accepts for an AssetGuid field. A typed
// AssetRef<C> field carries its category straight from reflection
// (FieldInfo::AssetCategory, populated generically by GE_REFLECT_FIELD_), so the
// slot filters to exactly that category with no per-component table — every
// component (UIDocument, LensFlareSource, Ocean*, and any future typed ref) is
// handled by the same code path. An open AssetRef<> (category Unknown) accepts the
// common asset set.
std::vector<AssetType> AcceptedAssetTypesForAssetField(const ECS::FieldInfo& field)
{
    const auto category = static_cast<AssetType>(field.AssetCategory);
    if (category != AssetType::Unknown)
        return {category};

    return {AssetType::Texture, AssetType::Model, AssetType::Material,
            AssetType::Audio, AssetType::Animation, AssetType::CubeLut,
            AssetType::Font, AssetType::Scene, AssetType::Shader,
            AssetType::RenderPipeline, AssetType::AnimationLibrary,
            AssetType::AnimationController, AssetType::OceanDepthCache,
            AssetType::OceanWaveSpectrum, AssetType::LensFlareDefinition,
            AssetType::UILayout, AssetType::UIStyle};
}

// Format a single field's value for a non-editable (ReadOnly) display line.
std::string FormatReadOnlyValue(const Snapshot& bytes, const ECS::FieldInfo& field)
{
    using ECS::FieldTypeId;
    const std::size_t off = field.Offset;
    switch (field.Type)
    {
        case FieldTypeId::Bool:   { std::uint8_t v = 0; ReadPod(bytes, off, v); return v ? "true" : "false"; }
        case FieldTypeId::Float:  { float v = 0.0f; ReadPod(bytes, off, v); return std::to_string(v); }
        case FieldTypeId::Double: { double v = 0.0; ReadPod(bytes, off, v); return std::to_string(v); }
        case FieldTypeId::Int8:
        case FieldTypeId::Int16:
        case FieldTypeId::Int32:
        case FieldTypeId::Int64:
        case FieldTypeId::UInt8:
        case FieldTypeId::UInt16: return std::to_string(ReadIntElement(bytes, off, field.Type));
        case FieldTypeId::UInt32: { std::uint32_t v = 0; ReadPod(bytes, off, v); return std::to_string(v); }
        case FieldTypeId::UInt64: { std::uint64_t v = 0; ReadPod(bytes, off, v); return std::to_string(v); }
        case FieldTypeId::String:
        {
            const char* text = reinterpret_cast<const char*>(bytes.data() + off);
            const std::size_t maxLen = (off < bytes.size()) ? (bytes.size() - off) : 0;
            std::size_t len = 0;
            while (len < maxLen && text[len] != '\0') ++len;
            return std::string(text, len);
        }
        case FieldTypeId::EntityHandle: { std::uint32_t id = 0; ReadPod(bytes, off, id); return "Entity #" + std::to_string(id); }
        default: return "(" + std::to_string(field.Size) + " bytes)";
    }
}

} // namespace

// The rows of `typeId`'s reflected fields, or of only the fields `onlyFields` names (declaration
// order either way).
static bool RenderFieldRows(const InspectorContext& ctx, ECS::ComponentTypeId typeId,
                            std::span<const std::string_view> onlyFields)
{
    if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        return false;

    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    if (fields.empty())
        return false;

    Snapshot bytes;
    if (!ctx.World->CaptureComponentBytes(ctx.Entity, typeId, bytes))
        return false;

    using namespace InspectorDrag;
    using ECS::FieldTypeId;

    UIElement* parent = ctx.Parent;
    FieldEditContext fecBase;
    fecBase.World = ctx.World;
    fecBase.Entity = ctx.Entity;
    fecBase.TypeId = typeId;
    fecBase.Notify = ctx.ChangeNotifications;
    fecBase.Undo = ctx.Undo;

    // Multi-selection: broadcast edits to the other selected entities that also
    // have this component (a successful byte capture doubles as the membership
    // test). The captured blobs are kept to drive per-field "mixed" display where
    // the selection disagrees.
    std::vector<Snapshot> extraBytes;
    for (ECS::EntityHandle e : InspectorDrag::GetAdditionalEntities(ctx))
    {
        Snapshot probe;
        if (ctx.World->CaptureComponentBytes(e, typeId, probe))
        {
            fecBase.Extras.push_back(e);
            extraBytes.push_back(std::move(probe));
        }
    }

    // Play-mode live refresh: build one Apply thunk per in-place-refreshable widget
    // and register a single sim-refresh callback after the emit loop. Only single
    // selection participates — multi-select "mixed" display is out of scope and would
    // be clobbered by the primary entity's live value, so `planPtr` stays null then and
    // every RecordX becomes a no-op.
    RefreshPlan refreshPlan;
    RefreshPlan* const planPtr =
        (ctx.SimulationRefreshCallbacks && fecBase.Extras.empty()) ? &refreshPlan : nullptr;

    bool any = false;

    // A component that keeps its own Enabled field (ECS::ComponentFlags::KeepsOwnEnabledField)
    // shows it as the section header's dot, so that field is not also drawn as a row.
    const ECS::ComponentRegistry::ComponentInfo* componentInfo = ECS::ComponentRegistry::GetComponentInfo(typeId);
    const bool keepsEnabledField =
        componentInfo && ECS::HasAnyFlag(componentInfo->Flags, ECS::ComponentFlags::KeepsOwnEnabledField);

    for (const ECS::FieldInfo& field : fields)
    {
        if (!onlyFields.empty() && std::find(onlyFields.begin(), onlyFields.end(), field.Name) == onlyFields.end())
            continue;
        const std::string tooltip = FieldTooltip(typeId, field);
        const char* tooltipPtr = tooltip.empty() ? nullptr : tooltip.c_str();

        // A field whose authored value this build could not read renders the fallback it landed on,
        // which is indistinguishable from a chosen value while a save writes the authored text
        // instead. Say so above the row, and offer the one action that resolves that disagreement
        // toward the live value.
        Editor::AddPreservedFieldNotice(parent, *ctx.World, ctx.Entity, typeId, field.Name,
                                        ctx.RequestInspectorRefresh);

        // Per-field policy: Hidden fields are omitted; ReadOnly fields render as a
        // non-editable value line (and set_component skips them — see DebugHandlers).
        if (ECS::HasAnyFlag(field.Flags, ECS::FieldFlags::Hidden))
            continue;
        if (keepsEnabledField && field.Offset == componentInfo->EnabledFieldOffset)
            continue;
        if (ECS::HasAnyFlag(field.Flags, ECS::FieldFlags::ReadOnly))
        {
            InspectorUI::AddLine(parent, std::string(FieldDisplayName(field.Name)) + ": " + FormatReadOnlyValue(bytes, field));
            any = true;
            continue;
        }

        if (IsOceanSurfaceType(typeId) && field.Name == "PlanarReflectionStrength")
            continue;

        const std::uint32_t elemSize = ECS::FieldElementSize(field.Type);
        const std::uint32_t count = (elemSize != 0) ? (field.Size / elemSize) : 1;

        // Not live-refreshed in v1: this OceanSurface toggle+slider composite is
        // editor-time tuning (no gameplay writes it) and its slider has no plan
        // hook. Stays stale until rebuild (today's behavior).
        if (EmitPlanarReflectionControlsRow(parent, bytes, extraBytes, fecBase, typeId,
                                            fields, field, tooltipPtr))
        {
            any = true;
            continue;
        }

        switch (field.Type)
        {
            case FieldTypeId::Bool:
            {
                for (std::uint32_t i = 0; i < count && i < kMaxElementRows; ++i)
                {
                    const std::size_t offset = field.Offset + static_cast<std::size_t>(i);
                    std::uint8_t cur = 0;
                    ReadPod(bytes, offset, cur);

                    FieldEditContext fec = fecBase;
                    fec.Label = ElementLabel(FieldDisplayName(field.Name), i, count);
                    auto editState = std::make_shared<std::optional<Editor::UndoRedoService::InteractiveEdit>>();
                    std::function<void(bool)> onChanging, onChanged;
                    WireEdit<bool>(editState, fec,
                        [offset](Snapshot& b, bool v) { if (offset < b.size()) b[offset] = v ? 1 : 0; },
                        onChanging, onChanged);
                    Toggle* tg = AddToggleRow(parent, fec.Label, cur != 0, std::move(onChanged),
                                             tooltipPtr);
                    if (tg && SpanDiffers(bytes, extraBytes, offset, 1))
                        tg->SetMixed();
                    RecordToggleRefresh(planPtr, tg, offset);
                    any = true;
                }
                break;
            }
            case FieldTypeId::Float:
            {
                const std::span<const FloatPreset> floatPresets =
                    (count == 1) ? OceanScalePresetsForField(typeId, field.Name)
                                 : std::span<const FloatPreset>{};
                // Preset dropdowns live-refresh via SetSelectedIndexWithoutNotify: a
                // concrete preset value re-selects its option; a value that maps to the
                // baked "Custom (x)" slot leaves the row until the next rebuild (its label
                // is fixed at build time). See EmitFloatPresetDropdownRow.
                if (!floatPresets.empty() &&
                    EmitFloatPresetDropdownRow(parent, planPtr, bytes, extraBytes, fecBase, field,
                                               floatPresets, tooltipPtr))
                {
                    any = true;
                    break;
                }

                for (std::uint32_t i = 0; i < count && i < kMaxElementRows; ++i)
                {
                    const std::size_t offset = field.Offset + static_cast<std::size_t>(i) * sizeof(float);
                    // Only multi-element float arrays (e.g. the per-octave FFT
                    // spectrum) render as sliders; scalar ranged fields stay as
                    // clamped drag fields.
                    EmitFloatRow(parent, planPtr, bytes, fecBase,
                                 ElementLabel(FieldDisplayName(field.Name), i, count), offset,
                                 SpanDiffers(bytes, extraBytes, offset, sizeof(float)), any,
                                 field.HasRange, field.MinValue, field.MaxValue,
                                 /*useSlider*/ count > 1, tooltipPtr);
                }
                break;
            }
            case FieldTypeId::Double:
            {
                for (std::uint32_t i = 0; i < count && i < kMaxElementRows; ++i)
                {
                    const std::size_t offset = field.Offset + static_cast<std::size_t>(i) * sizeof(double);
                    double cur = 0.0;
                    ReadPod(bytes, offset, cur);

                    FieldEditContext fec = fecBase;
                    fec.Label = ElementLabel(FieldDisplayName(field.Name), i, count);
                    auto editState = std::make_shared<std::optional<Editor::UndoRedoService::InteractiveEdit>>();
                    std::function<void(float)> onChanging, onChanged;
                    WireEdit<float>(editState, fec,
                        [offset](Snapshot& b, float v) { double dv = v; if (offset + sizeof(double) <= b.size()) std::memcpy(b.data() + offset, &dv, sizeof(double)); },
                        onChanging, onChanged);
                    FloatField* f = AddFloatRowWithDrag(parent, fec.Label, static_cast<float>(cur),
                                                        std::move(onChanging), std::move(onChanged),
                                                        std::numeric_limits<float>::quiet_NaN(),
                                                        tooltipPtr);
                    if (f && SpanDiffers(bytes, extraBytes, offset, sizeof(double)))
                        f->SetMixed();
                    RecordDoubleFieldRefresh(planPtr, f, offset);
                    any = true;
                }
                break;
            }
            case FieldTypeId::Int8:
            case FieldTypeId::UInt8:
            case FieldTypeId::Int16:
            case FieldTypeId::UInt16:
            case FieldTypeId::Int32:
            case FieldTypeId::UInt32:
            case FieldTypeId::Int64:
            case FieldTypeId::UInt64:
            {
                const std::span<const UIntPreset> uintPresets =
                    (count == 1) ? ComponentUIntPresetsForField(typeId, field.Name)
                                 : std::span<const UIntPreset>{};
                if (!uintPresets.empty() &&
                    EmitUIntPresetDropdownRow(parent, planPtr, bytes, extraBytes, fecBase, field,
                                              uintPresets, tooltipPtr))
                {
                    any = true;
                    break;
                }

                // Scanner-detected enum (single, non-array field): render a NAME dropdown
                // instead of a raw number. Falls through to the numeric widget below when
                // the stored value is outside the enum table (so it stays visible/editable).
                // Live-refreshed via SetSelectedIndexWithoutNotify: a fresh value that maps
                // to a table entry re-selects it; an out-of-table value keeps the last label
                // (emit's numeric fallback isn't reachable post-build). See EmitEnumDropdownRow.
                if (!field.EnumNames.empty() && count == 1 &&
                    EmitEnumDropdownRow(parent, planPtr, bytes, extraBytes, fecBase, field, tooltipPtr))
                {
                    any = true;
                    break;
                }

                for (std::uint32_t i = 0; i < count && i < kMaxElementRows; ++i)
                {
                    const std::size_t offset = field.Offset + static_cast<std::size_t>(i) * elemSize;

                    // Unsigned 32/64-bit values can exceed signed-int range (e.g. a
                    // 0xFFFFFFFF mask), which a signed IntField would show as -1.
                    // Edit them via a text field (decimal display; accepts decimal
                    // or 0x-hex input).
                    if (field.Type == FieldTypeId::UInt32 || field.Type == FieldTypeId::UInt64)
                    {
                        std::uint64_t uv = 0;
                        if (field.Type == FieldTypeId::UInt32) { std::uint32_t v = 0; ReadPod(bytes, offset, v); uv = v; }
                        else { ReadPod(bytes, offset, uv); }

                        FieldEditContext fec = fecBase;
                        fec.Label = ElementLabel(FieldDisplayName(field.Name), i, count);
                        const FieldTypeId fieldType = field.Type;
                        TextField* tf = AddTextFieldRow(parent, fec.Label, std::to_string(uv),
                                                        tooltipPtr);
                        WireTextCommit(tf, fec, [offset, fieldType](Snapshot& b, const std::string& s) {
                            std::uint64_t parsed = 0;
                            try { parsed = std::stoull(s, nullptr, 0); } catch (...) { return; }
                            WriteUIntElement(b, offset, fieldType, parsed);
                        });
                        if (tf && SpanDiffers(bytes, extraBytes, offset, elemSize))
                            tf->SetMixed();
                        RecordUIntTextFieldRefresh(planPtr, tf, offset, fieldType);
                        any = true;
                        continue;
                    }

                    const int cur = ReadIntElement(bytes, offset, field.Type);

                    FieldEditContext fec = fecBase;
                    fec.Label = ElementLabel(FieldDisplayName(field.Name), i, count);
                    const FieldTypeId fieldType = field.Type;
                    auto editState = std::make_shared<std::optional<Editor::UndoRedoService::InteractiveEdit>>();
                    std::function<void(int)> onChanging, onChanged;
                    WireEdit<int>(editState, fec,
                        [offset, fieldType](Snapshot& b, int v) { WriteIntElement(b, offset, fieldType, v); },
                        onChanging, onChanged);
                    IntField* intf = AddIntRowWithDrag(parent, fec.Label, cur,
                                                       std::move(onChanging), std::move(onChanged),
                                                       std::numeric_limits<int>::min(), tooltipPtr);
                    if (intf && SpanDiffers(bytes, extraBytes, offset, elemSize))
                        intf->SetMixed();
                    RecordIntFieldRefresh(planPtr, intf, offset, field.Type);
                    any = true;
                }
                break;
            }
            case FieldTypeId::String:
            {
                const char* text = reinterpret_cast<const char*>(bytes.data() + field.Offset);
                const std::size_t maxLen = (field.Offset < bytes.size()) ? (bytes.size() - field.Offset) : 0;
                std::size_t len = 0;
                while (len < maxLen && text[len] != '\0')
                    ++len;

                FieldEditContext fec = fecBase;
                fec.Label = std::string(FieldDisplayName(field.Name));
                const std::size_t offset = field.Offset;
                const std::uint32_t cap = field.Size;  // char[cap] capacity, incl. null terminator
                TextField* tf = AddTextFieldRow(parent, fec.Label, std::string(text, len),
                                                tooltipPtr);
                WireTextCommit(tf, fec, [offset, cap](Snapshot& b, const std::string& s) {
                    if (cap == 0 || offset + cap > b.size())
                        return;
                    const std::size_t n = std::min<std::size_t>(s.size(), static_cast<std::size_t>(cap) - 1);
                    std::memcpy(b.data() + offset, s.data(), n);
                    for (std::size_t k = n; k < cap; ++k)  // null-terminate + zero the tail
                        b[offset + k] = 0;
                });
                if (tf && SpanDiffers(bytes, extraBytes, field.Offset, field.Size))
                    tf->SetMixed();
                RecordStringFieldRefresh(planPtr, tf, offset, cap);
                any = true;
                break;
            }
            case FieldTypeId::Vec2:
            case FieldTypeId::Vec3:
            case FieldTypeId::Vec4:
            {
                const std::uint32_t axisCount = (field.Type == FieldTypeId::Vec2) ? 2u
                                              : (field.Type == FieldTypeId::Vec3) ? 3u : 4u;
                for (std::uint32_t i = 0; i < count && i < kMaxElementRows; ++i)
                {
                    const std::size_t base = field.Offset + static_cast<std::size_t>(i) * elemSize;
                    EmitFloatAxisRows(parent, planPtr, bytes, extraBytes, fecBase,
                                      ElementLabel(FieldDisplayName(field.Name), i, count),
                                      base, axisCount, /*isColor*/ false, any, tooltipPtr);
                }
                break;
            }
            case FieldTypeId::Color:
            {
                // Color (RGBA float4) renders as a swatch + picker, like Light's Color.
                for (std::uint32_t i = 0; i < count && i < kMaxElementRows; ++i)
                {
                    const std::size_t base = field.Offset + static_cast<std::size_t>(i) * elemSize;
                    EmitColorSwatchRow(parent, planPtr, bytes, fecBase, ElementLabel(FieldDisplayName(field.Name), i, count), base,
                                       ctx.OpenColorPickerWindow, any, tooltipPtr);
                }
                break;
            }
            case FieldTypeId::Quat:
            {
                for (std::uint32_t i = 0; i < count && i < kMaxElementRows; ++i)
                {
                    const std::size_t base = field.Offset + static_cast<std::size_t>(i) * elemSize;
                    EmitQuatEulerRows(parent, planPtr, bytes, extraBytes, fecBase,
                                      ElementLabel(FieldDisplayName(field.Name), i, count),
                                      base, any, tooltipPtr);
                }
                break;
            }
            case FieldTypeId::EntityHandle:
            {
                // Not live-refreshed in v1: the picker resolves its label asynchronously,
                // so in-place refresh would need to re-run that resolve. Stays stale until
                // rebuild (today's behavior).
                std::uint32_t id = 0;
                ReadPod(bytes, field.Offset, id);

                FieldEditContext fec = fecBase;
                fec.Label = std::string(FieldDisplayName(field.Name));
                const std::size_t offset = field.Offset;
                InspectorUI::AddEntityFieldRow(parent, fec.Label, ECS::EntityHandle(id), ctx.World,
                    [fec, offset](ECS::EntityHandle h) {
                        const std::uint32_t newId = h.id;
                        CommitFieldPatch(fec, [offset, newId](Snapshot& b) {
                            if (offset + sizeof(newId) <= b.size())
                                std::memcpy(b.data() + offset, &newId, sizeof(newId));
                        });
                    },
                    {}, tooltipPtr);
                any = true;
                break;
            }
            case FieldTypeId::AssetGuid:
            {
                // Not live-refreshed in v1: the asset slot resolves its label/thumbnail
                // asynchronously, so in-place refresh would need to re-run that resolve.
                // Stays stale until rebuild (today's behavior).
                // 16-byte asset GUID stored inline (AssetRef<T> fields), one picker row per
                // element of an array of them. Render a real asset picker (drag-drop + browse +
                // thumbnail) instead of the raw-bytes fallback, filtered to the field's asset
                // category (AcceptedAssetTypesForAssetField).
                for (std::uint32_t i = 0; i < count && i < kMaxElementRows; ++i)
                {
                    const std::size_t offset = field.Offset + static_cast<std::size_t>(i) * GUID::kSize;
                    GUID current;
                    if (offset + GUID::kSize <= bytes.size())
                    {
                        GUID::Data data{};
                        std::memcpy(data.data(), bytes.data() + offset, GUID::kSize);
                        current = GUID(data);
                    }

                    FieldEditContext fec = fecBase;
                    fec.Label = ElementLabel(FieldDisplayName(field.Name), i, count);
                    InspectorUI::AddAssetFieldRow(parent, fec.Label, current,
                        AcceptedAssetTypesForAssetField(field),
                        &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
                        [fec, offset](const GUID& picked) {
                            const GUID::Data data = picked.GetData();
                            CommitFieldPatch(fec, [offset, data](Snapshot& b) {
                                if (offset + GUID::kSize <= b.size())
                                    std::memcpy(b.data() + offset, data.data(), GUID::kSize);
                            });
                        },
                        ctx.Thumbnails, tooltipPtr);
                    any = true;
                }
                break;
            }
            default:
            {
                InspectorUI::AddLine(parent, std::string(FieldDisplayName(field.Name)) + " (" + std::to_string(field.Size) + " bytes)");
                any = true;
                break;
            }
        }
    }

    // One sim-refresh callback for the whole section: re-capture the component bytes
    // once, fetch the focused-element id once, then replay every field's Apply thunk.
    // Cleared with the section's callback list on the next rebuild of this section
    // (full ShowEntity or single-section), so the raw widget pointers baked into the
    // plan can never outlive their UI. Registered on the ~10 Hz sim lane, not the
    // per-frame lane.
    if (planPtr && !refreshPlan.empty())
    {
        ECS::World* const world = ctx.World;
        const ECS::EntityHandle entity = ctx.Entity;
        UIElement* const focusRoot = parent;
        ctx.SimulationRefreshCallbacks->push_back(
            [world, entity, typeId, focusRoot, plan = std::move(refreshPlan),
             lastSeenVersion = std::uint64_t{0}]() mutable {
                if (!world || !entity.IsValid() || !world->IsValid(entity))
                    return;

                // Change gate (live-values design Part B): skip the capture + replay
                // when nothing wrote this entity's chunk column since the last tick,
                // reducing an idle tick to one uint64 compare. Chunk-granular: a
                // co-located entity's write over-reports — absorbed by each thunk's
                // equality early-out; it can never miss. A vanished component reads
                // v == 0; caching that 0 is safe (the vanished state early-outs while
                // the capture below would fail anyway) and cannot mask a re-add:
                // stamps are monotonic global versions, so re-adding stamps the
                // destination chunk with a version never seen before — the next tick
                // observes v != lastSeenVersion and repaints.
                const std::uint64_t v = world->GetEntityColumnVersion(entity, typeId);
                if (v == lastSeenVersion)
                    return;
                lastSeenVersion = v;

                Snapshot bytes;
                if (!world->CaptureComponentBytes(entity, typeId, bytes))
                    return;  // component removed since build; the signature poll rebuilds

                UIManager* const ui = focusRoot ? focusRoot->GetOwnerManager() : nullptr;
                static const std::string kEmptyFocusId;
                const std::string& focusId = ui ? ui->GetFocusedElementId() : kEmptyFocusId;

                for (const FieldRefreshEntry& entry : plan)
                    entry.Apply(bytes, focusId);
            });
    }

    return any;
}

bool RenderDefaultComponentInspector(const InspectorContext& ctx, ECS::ComponentTypeId typeId)
{
    return RenderFieldRows(ctx, typeId, {});
}

bool RenderReflectedFieldRows(const InspectorContext& ctx, ECS::ComponentTypeId typeId,
                              std::span<const std::string_view> fieldNames)
{
    return !fieldNames.empty() && RenderFieldRows(ctx, typeId, fieldNames);
}

} // namespace GameEngine
