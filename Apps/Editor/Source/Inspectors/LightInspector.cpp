#include "Inspectors/LightInspector.h"

#include "InspectorRegistry.h"
#include "Platform/SystemMetrics.h"

#include "Components/Rendering/Light.h"
#include "Components/Name.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "EditorChangeNotifications.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/LuxText.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/StyleProperties.h"
#include "Types/ColorUtils.h"

#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/InfoCard.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace GameEngine
{

namespace
{
using Components::LightType;
using Components::AreaLightShape;
using Components::LightUnit;
using Components::LightFalloff;

static constexpr EnumEntry<LightType> kLightTypes[] = {
    {LightType::Directional, "Directional", "dropdown-icon--light-directional"},
    {LightType::Point, "Point", "dropdown-icon--light-point"},
    {LightType::Spot, "Spot", "dropdown-icon--light-spot"},
    {LightType::Ambient, "Ambient", "dropdown-icon--light-ambient"},
    {LightType::Area, "Area", "dropdown-icon--light-area"},
};

static constexpr EnumEntry<AreaLightShape> kAreaLightShapes[] = {
    {AreaLightShape::Rectangle, "Rectangle", "dropdown-icon--shape-rectangle"},
    {AreaLightShape::Disc, "Disc", "dropdown-icon--shape-disc"},
    {AreaLightShape::Sphere, "Sphere", "dropdown-icon--shape-sphere"},
    {AreaLightShape::Cylinder, "Cylinder", "dropdown-icon--shape-cylinder"},
};

static constexpr EnumEntry<LightFalloff> kLightFalloffs[] = {
    {LightFalloff::PhysicalInverseSquare, "Physical / Inverse Square"},
    {LightFalloff::Linear, "Linear"},
    {LightFalloff::SmoothRange, "Smooth Range"},
    {LightFalloff::Custom, "Custom"},
};

static constexpr EnumEntry<LightUnit> kLightUnits[] = {
    {LightUnit::Unitless, "Abstract"},
    {LightUnit::Lux, "Lux"},
    {LightUnit::Lumen, "Lumen"},
    {LightUnit::Candela, "Candela"},
};

// Per-type intensity-unit subsets: only physically-meaningful units are offered for each light
// type (directional illuminance is Lux; punctual emitters are Lumen/Candela; the rest stay
// unitless). Switching type re-anchors the unit + intensity via the converter below.
static constexpr EnumEntry<LightUnit> kLightUnitsDirectional[] = {
    {LightUnit::Unitless, "Abstract"},
    {LightUnit::Lux, "Lux"},
};
static constexpr EnumEntry<LightUnit> kLightUnitsPunctual[] = {
    {LightUnit::Unitless, "Abstract"},
    {LightUnit::Lumen, "Lumen"},
    {LightUnit::Candela, "Candela"},
};
static constexpr EnumEntry<LightUnit> kLightUnitsOther[] = {
    {LightUnit::Unitless, "Abstract"},
};

static const char* FalloffTooltip(LightFalloff falloff)
{
    switch (falloff)
    {
    case LightFalloff::PhysicalInverseSquare:
        return "Physical / Inverse Square: realistic 1 / distance^2 falloff with Range as the cutoff.";
    case LightFalloff::Linear:
        return "Linear: simple straight fade from full brightness to zero at Range.";
    case LightFalloff::SmoothRange:
        return "Smooth Range: artistic soft fade to zero at Range, without physical distance loss.";
    case LightFalloff::Custom:
        return "Custom: uses Custom Decay for manual falloff control. This matches the old Decay behavior.";
    }
    return "How this local light fades with distance. Range remains the cutoff distance.";
}

// The default unit a light should adopt when it becomes `type` (used on type-change conversion).
static LightUnit DefaultUnitForType(LightType type)
{
    switch (type)
    {
    case LightType::Directional: return LightUnit::Lux;
    case LightType::Point:
    case LightType::Spot:        return LightUnit::Candela;
    default:                     return LightUnit::Unitless;
    }
}

// True when `unit` is a valid choice for `type` (matches the per-type subsets above).
static bool IsUnitValidForType(LightUnit unit, LightType type)
{
    if (unit == LightUnit::Unitless)
        return true;
    switch (type)
    {
    case LightType::Directional: return unit == LightUnit::Lux;
    case LightType::Point:
    case LightType::Spot:        return unit == LightUnit::Lumen || unit == LightUnit::Candela;
    default:                     return false;
    }
}

constexpr float kRad2Deg = 57.2957795f;
constexpr float kDeg2Rad = 0.0174532925f;
// Width of the numeric value field paired with an inspector slider — sized to fit the formatted
// value (e.g. "11196.42") plus the input's padding, so the input doesn't overflow past the value
// column (which would push it tight against the panel's right edge, misaligned with other rows).
constexpr float kSliderValueFieldWidthPx = 76.0f;
constexpr float kSliderTrackPaddingPx = InspectorDrag::kInspectorSliderTrackPaddingPx;

// The Kelvin slider's range — shared by its [SetMin,SetMax] and the gradient texture it paints, so
// the slider value and the gradient track one source of truth.
constexpr float kKelvinGradientLoK = 1500.0f;
constexpr float kKelvinGradientHiK = 12000.0f;

// The swatch colour of a light colour. A colour with a channel above 1 (a sky's hue at unit brightness)
// is scaled down by its largest channel, so the swatch keeps its hue instead of clipping it.
static uint32_t LightColorToArgb(const float (&c)[3])
{
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    const float scale = 1.0f / std::max(1.0f, std::max(c[0], std::max(c[1], c[2])));
    uint8_t r = static_cast<uint8_t>(clamp01(c[0] * scale) * 255.0f + 0.5f);
    uint8_t g = static_cast<uint8_t>(clamp01(c[1] * scale) * 255.0f + 0.5f);
    uint8_t b = static_cast<uint8_t>(clamp01(c[2] * scale) * 255.0f + 0.5f);
    return (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static void ArgbToLightColor(uint32_t argb, float (&out)[3])
{
    out[0] = ((argb >> 16) & 0xFF) / 255.0f;
    out[1] = ((argb >> 8) & 0xFF) / 255.0f;
    out[2] = (argb & 0xFF) / 255.0f;
}

static std::string FormatRgb(const float (&c)[3])
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", c[0], c[1], c[2]);
    return buf;
}

// Linear RGB [0,1] -> sRGB-encoded ARGB bytes (0xFFRRGGBB).
static uint32_t LinearToSrgbArgb(const float lin[3])
{
    auto u8 = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        const float s = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint32_t>(s * 255.0f + 0.5f);
    };
    return (0xFFu << 24) | (u8(lin[0]) << 16) | (u8(lin[1]) << 8) | u8(lin[2]);
}

// Kelvin -> chroma-boosted sRGB ARGB for the on-screen gradient ONLY (the light's real tint is the
// unboosted KelvinToLinearRGB). The boost is applied in LINEAR space (Rec.709 luminance, preserving
// brightness) BEFORE the sRGB encode; the gradient texture is stored as RGBA8_SRGB so the GPU
// sampler decodes it back to linear for the UI's linear compositing — no double sRGB encode. True
// blackbody is very desaturated (12000K is only a pale blue), so the boost makes warm/cool legible.
static uint32_t KelvinToBoostedSrgbArgb(float kelvin, float boost)
{
    float lin[3];
    Components::KelvinToLinearRGB(kelvin, lin);
    const float lum = ColorUtils::LinearRec709Luminance(lin);
    for (float& c : lin)
        c = std::clamp(lum + (c - lum) * boost, 0.0f, 1.0f);
    return LinearToSrgbArgb(lin);
}

// Lazily build a 256x1 RGBA8 texture of the Kelvin->color gradient and register it with the UI
// texture set, returning its bindless slot (0 on failure). A single bilinear-sampled textured quad
// is the only seam-free way to draw the gradient: separate gradient/solid rects bleed the track
// through their anti-aliased edges. The gradient spans the fixed [kKelvinGradientLoK,HiK] range, so
// one texture serves every light and every paint; it is cached until the device's GPU objects die
// (shutdown, or an in-place rebuild after device loss), then rebuilt lazily on the next paint.
static uint32_t ResolveKelvinGradientSlot(UI::PrimitiveEmitContext& ctx)
{
    if (!ctx.Textures)
        return 0;
    static Rendering::TextureHandle s_Tex{};
    if (!s_Tex.IsValid())
    {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        Rendering::IDevice* device = rs ? rs->GetDevice() : nullptr;
        if (!device)
            return 0;
        constexpr int kWidth = 256;
        std::array<uint32_t, kWidth> pixels{};
        for (int i = 0; i < kWidth; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(kWidth - 1);
            constexpr float kGradientChromaBoost = 2.0f; // mild; the sRGB-format fix makes this effective
            pixels[static_cast<size_t>(i)] = UI::PackFromARGB(KelvinToBoostedSrgbArgb(
                kKelvinGradientLoK + t * (kKelvinGradientHiK - kKelvinGradientLoK), kGradientChromaBoost));
        }
        Rendering::TextureDesc td{};
        td.width = kWidth;
        td.height = 1;
        // SRGB so the GPU sampler decodes the stored sRGB bytes back to linear for the UI's linear
        // compositing pipeline (UNORM would skip the decode -> double sRGB encode -> washed out).
        td.format = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_SRGB);
        td.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
        td.persistent = true;
        td.debugName = "KelvinGradient";
        s_Tex = device->CreateTexture(td);
        if (!s_Tex.IsValid())
            return 0;
        Rendering::UploadTexture2D(device, s_Tex, pixels.data(), kWidth, 1,
                                   static_cast<size_t>(kWidth) * sizeof(uint32_t), "KelvinGradientStaging");
        // Free the texture whenever the device's GPU objects die — shutdown OR an
        // in-place rebuild after device loss — and clear the handle so the next paint
        // rebuilds the gradient on the live device. A TextureHandle stays IsValid()
        // after its device dies (nothing re-stamps it), so without the reset this
        // cache would hand the UI a handle that resolves to nothing for the rest of
        // the session; without the destroy it would leak for the process lifetime.
        device->RegisterPerDeviceCacheCleanup("Editor.KelvinGradient", [](Rendering::IDevice* dev) {
            if (s_Tex.IsValid())
                dev->DestroyTexture(s_Tex);
            s_Tex = {};
        });
    }
    return ctx.Textures->Register(s_Tex);
}

// Interactive color-temperature picker: a Slider over [1500,12000]K whose track is painted as a
// smooth Kelvin->color gradient (one bilinear-sampled textured quad, seam-free). Subclassing Slider
// gives click-to-set + drag for free; the paint is overridden to draw the gradient plus a downward
// triangle indicator at the value (the inherited ring thumb is hidden via CSS).
class KelvinSlider : public Slider
{
  public:
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& /*style*/,
                              float x, float y, float W, float H) override
    {
        // Parallel-drain thread contract: custom emission may touch shared
        // text/measure state, so it never runs on a JobSystem worker — escalate
        // and let the drain re-emit this element on the UI thread.
        if (ctx.OffThread)
        {
            if (ctx.EscalateFlag)
                *ctx.EscalateFlag = true;
            return;
        }

        if (W <= 0.0f || H <= 0.0f)
            return;
        const float lo = GetMin();
        const float hi = GetMax();
        if (hi <= lo)
            return;

        const float cs = ctx.ContentScale;
        // The inspector always calls SetTrackPaddingPx on this slider, so the track padding is set.
        const float padH = GetTrackPaddingPx() * cs;
        const float trackX = x + padH;
        const float trackW = std::max(0.0f, W - 2.0f * padH);
        if (trackW <= 0.0f)
            return;

        const float barH = std::min(H, 12.0f * cs);
        const float barY = y + (H - barH) * 0.5f;

        // One bilinear-sampled textured quad: the GPU smoothly interpolates the 256-texel Kelvin
        // gradient across the track with no internal seams.
        const uint32_t slot = ResolveKelvinGradientSlot(ctx);
        if (slot != 0)
            ctx.Emit(UI::MakeTexturedQuad(trackX, barY, trackW, barH, slot));

        // Downward triangle indicator at the current value (the inherited ring thumb is hidden via
        // CSS). A dark outline triangle behind the blue one keeps it readable over any gradient hue.
        const float norm = std::clamp((GetValue() - lo) / (hi - lo), 0.0f, 1.0f);
        const float vx = trackX + trackW * norm;
        const float halfW = 6.0f * cs;            // wider than before
        const float triTop = barY - 2.0f * cs;    // base sits just above the bar top (less proud)
        const float triTip = barY + barH * 0.60f; // tip reaches further down into the bar
        const uint32_t kOutline = UI::PackFromARGB(0xE0141414u);
        const uint32_t kIndicator = UI::PackFromARGB(0xFF4C9AF0u);
        ctx.Emit(UI::MakeTriangle(vx - halfW - 1.0f * cs, triTop - 1.0f * cs,
                                  vx + halfW + 1.0f * cs, triTop - 1.0f * cs,
                                  vx, triTip + 1.4f * cs, kOutline, 1.0f));
        ctx.Emit(UI::MakeTriangle(vx - halfW, triTop, vx + halfW, triTop, vx, triTip, kIndicator, 1.0f));
    }
};

// Inline guide (in a grounded box): quick distinction between physical Lux and engine-authored
// Unitless intensity values.
static void AddIntensityReferenceGuide(UIElement* parent)
{
    UIElement* row = InspectorUI::AddRow(parent);
    row->AddClass("inspector-guide-row");
    InspectorUI::AddLabel(row, "Intensity Reference",
        "Lux uses real-world photometric light values. Arbitrary uses brightness values without real-world units.");
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->AddClass("inspector-guide-field");

    fieldContainer->AddChild(std::make_unique<EditorUI::CollapsibleInfoCard>(
        "Lux: real-world photometric light values.\n"
        "Arbitrary: arbitrary, brightness values."));
}

} // namespace

// The sky as the light inspector names it: "Sky Environment", or Sky Environment "Dusk Sky" when the
// author renamed it.
static std::string DrivingSkyDisplayName(const ECS::World& world, ECS::EntityHandle sky)
{
    constexpr const char* kSkyTypeName = "Sky Environment";
    const auto* name = world.GetComponent<Components::Name>(sky);
    if (!name || name->value[0] == '\0' || name->View() == kSkyTypeName)
        return kSkyTypeName;
    return std::string(kSkyTypeName) + " \"" + std::string(name->View()) + "\"";
}

// What the notice says a sky sets on this light, for the fields it drives
// (SkySunIlluminance::DrivenLightFields).
static std::string SkyDriverNoticeText(const ECS::World& world, ECS::EntityHandle sky, uint8_t fields)
{
    using namespace Components::SkySunIlluminance;
    const std::string who = DrivingSkyDisplayName(world, sky);
    if (fields & kDrivesIntensity)
        return who + ((fields & kDrivesColor) ? " sets this light's direction, colour and intensity from the time of day."
                                              : " sets this light's direction and intensity from the time of day.");
    return who + " sets this light's direction and colour from the time of day. Intensity is the sun's "
                 "illuminance with the sun overhead.";
}

// The Intensity row of a light a sky's illuminance curve drives: the illuminance it delivers with the
// sun overhead, in lux as the sky writes it, kept current, with the caption the driven Color row
// carries, read-only.
static void AddSkyDrivenIntensityRow(const InspectorContext& ctx, ECS::World& world, ECS::EntityHandle light,
                                     ECS::EntityHandle sky)
{
    const std::string tooltip = "Set by " + DrivingSkyDisplayName(world, sky) +
                                " from its custom illuminance curve every frame. Turn the sky's Custom illuminance "
                                "curve off to author the intensity here.";
    UIElement* row = InspectorUI::AddRow(ctx.Parent);
    InspectorUI::AddLabel(row, "Intensity", tooltip.c_str());
    UIElement* field = InspectorUI::AddFieldContainer(row);
    field->AddClass("inspector-field-inline-action");
    auto value = std::make_unique<Label>();
    value->AddClass("inspector-text");
    value->SetTooltip(tooltip);
    Label* valueRaw = value.get();
    field->AddChild(std::move(value));
    auto owner = std::make_unique<Label>();
    owner->AddClass("inspector-text");
    owner->SetText("set by the sky");
    field->AddChild(std::move(owner));
    InspectorUI::DisableRowOfControl(valueRaw);

    const auto show = [valueRaw](const Components::Light& driven) {
        const std::string text =
            FormatLux(Components::SkySunIlluminance::LuxFromLightIntensity(driven.Intensity, driven.IntensityUnit));
        if (valueRaw->GetText() != text)
            valueRaw->SetText(text);
    };
    if (const auto* current = world.GetComponent<Components::Light>(light))
        show(*current);
    if (!ctx.SimulationRefreshCallbacks)
        return;
    ctx.SimulationRefreshCallbacks->push_back([getWorld = ctx.GetWorld, light, show]() {
        ECS::World* w = getWorld ? getWorld() : nullptr;
        const auto* current = w && w->IsValid(light) ? w->GetComponent<Components::Light>(light) : nullptr;
        if (current)
            show(*current);
    });
}

// While a sky drives this light, say so at the top of the component, with the action that selects the
// sky: the fields it drives are disabled below.
static void AddSkyDriverNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle sky, uint8_t fields,
                               const std::function<void(ECS::EntityHandle)>& selectEntity)
{
    if (!sky.IsValid())
        return;
    auto notice = std::make_unique<EditorUI::InspectorNotice>(SkyDriverNoticeText(world, sky, fields),
                                                              EditorUI::InspectorNotice::Kind::Information);
    if (selectEntity)
        notice->SetAction("Select sky", "Select the sky that drives this light, to change what it drives.",
                          [selectEntity, sky]() { selectEntity(sky); });
    parent->AddChild(std::move(notice));
}

void RegisterLightInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        {
            return;
        }

        auto* light = ctx.World->GetComponent<Components::Light>(ctx.Entity);
        if (!light)
        {
            InspectorUI::AddLine(ctx.Parent, "(Light missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        const LightType type = light->Type == LightType::Volume ? LightType::Point : light->Type;
        OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;
        using namespace InspectorDrag;
        auto extras = GetAdditionalEntities(ctx);

        // The sky that drives this light and the fields it drives, found once when the inspector builds.
        const ECS::EntityHandle drivingSky = Components::SkySunIlluminance::SkyDrivingLight(*w, e);
        const uint8_t drivenFields = Components::SkySunIlluminance::DrivenLightFields(*w, e);
        const bool colorDrivenBySky = (drivenFields & Components::SkySunIlluminance::kDrivesColor) != 0;
        const bool intensityDrivenBySky = (drivenFields & Components::SkySunIlluminance::kDrivesIntensity) != 0;
        AddSkyDriverNotice(ctx.Parent, *w, drivingSky, drivenFields, ctx.SelectEntity);

        // Editable: Type (Directional / Point / Spot / Ambient / Area)
        auto* typeField = InspectorUI::AddEnumRow(ctx.Parent, "Type", kLightTypes, type,
            "Light type: Directional (infinite), Point (omnidirectional), Spot (cone), Ambient, or Area");
        // Switching type re-anchors the intensity unit + value: if the current unit isn't valid
        // for the new type, convert to that type's default unit preserving effective brightness
        // (Unitless always stays Unitless). Type + unit + intensity commit as one undo step.
        auto applyTypeChange = [](Components::Light& u, LightType newType) {
            const LightUnit oldUnit = u.IntensityUnit;
            u.Type = newType;
            if (!IsUnitValidForType(oldUnit, newType))
            {
                const float unitless = Components::LightIntensityToUnitless(u.Intensity, oldUnit);
                const LightUnit newUnit = DefaultUnitForType(newType);
                u.IntensityUnit = newUnit;
                u.Intensity = Components::UnitlessToLightIntensity(unitless, newUnit);
            }
        };
        typeField->SetOnValueChanged([w, e, n, undo, extras, applyTypeChange](LightType v) {
            CommitComponentWithUndo<Components::Light>(w, e, n, undo, "Change Light Type",
                [v, applyTypeChange](Components::Light& u) { applyTypeChange(u, v); });
            for (auto& ex : extras)
            {
                auto* c = w->GetComponent<Components::Light>(ex);
                if (!c) continue;
                Components::Light u = *c;
                applyTypeChange(u, v);
                Editor::CommitComponentUpdate(w, ex, n, u);
            }
            // Rebuild so the per-type unit dropdown subset + intensity guide reflect the new type.
            if (n)
            {
                Editor::EditorChangeNotifications::ComponentChangedEvent ev{};
                ev.world = w;
                ev.entity = e;
                ev.componentType = ECS::GetComponentTypeId<Components::Light>();
                ev.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
                n->NotifyComponentChanged(ev);
            }
        });

        // Editable: Intensity (all types). While a sky's illuminance curve writes it, the row reads
        // the value the sky wrote, as the sky's inspector and readout write illuminance, and says who
        // sets it.
        if (intensityDrivenBySky)
            AddSkyDrivenIntensityRow(ctx, *w, e, drivingSky);
        else
            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Intensity", light->Intensity, w, e, n, undo,
                                                            "Change Light Intensity",
                                                            [](Components::Light& u, float v) { u.Intensity = v; },
                                                            1.0f, "Light brightness multiplier", extras);

        // Editable: Intensity unit — how Intensity is interpreted (resolved on the CPU at extraction).
        // Show a unit valid for the type even if a loaded/scripted light stored an invalid one, so the
        // dropdown doesn't fall back to index-0 "Unitless" while the guide shows the stored unit.
        const LightUnit displayUnit =
            IsUnitValidForType(light->IntensityUnit, type) ? light->IntensityUnit : DefaultUnitForType(type);
        {
            const char* unitTip =
                "Lux: real-world photometric light values. Arbitrary: arbitrary brightness values. "
                "Switching units preserves visible brightness. Only units valid for this light type are shown.";
            auto* unitField =
                (type == LightType::Directional)
                    ? InspectorUI::AddEnumRow(ctx.Parent, "Intensity Unit", kLightUnitsDirectional, displayUnit, unitTip)
                : (type == LightType::Point || type == LightType::Spot)
                    ? InspectorUI::AddEnumRow(ctx.Parent, "Intensity Unit", kLightUnitsPunctual, displayUnit, unitTip)
                    : InspectorUI::AddEnumRow(ctx.Parent, "Intensity Unit", kLightUnitsOther, displayUnit, unitTip);
            unitField->SetOnValueChanged([w, e, n, undo, extras](LightUnit v) {
                CommitComponentWithUndo<Components::Light>(w, e, n, undo, "Change Light Intensity Unit",
                    [v](Components::Light& u) {
                        const float unitless = Components::LightIntensityToUnitless(u.Intensity, u.IntensityUnit);
                        u.IntensityUnit = v;
                        u.Intensity = Components::UnitlessToLightIntensity(unitless, v);
                    });
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::Light>(ex);
                    if (!c) continue;
                    Components::Light u = *c;
                    const float unitless = Components::LightIntensityToUnitless(u.Intensity, u.IntensityUnit);
                    u.IntensityUnit = v;
                    u.Intensity = Components::UnitlessToLightIntensity(unitless, v);
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
                if (n)
                {
                    Editor::EditorChangeNotifications::ComponentChangedEvent ev{};
                    ev.world = w;
                    ev.entity = e;
                    ev.componentType = ECS::GetComponentTypeId<Components::Light>();
                    ev.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
                    n->NotifyComponentChanged(ev);
                }
            });
        }
        if (ctx.ShowInfoCards)
            AddIntensityReferenceGuide(ctx.Parent);

        // Editable: Color temperature (Kelvin) — tints the light color when enabled. Read-only while a
        // sky drives the colour: the drive divides the temperature out, so an edit would change nothing.
        const std::string temperatureTooltip = colorDrivenBySky
            ? "Set aside while " + DrivingSkyDisplayName(*w, drivingSky) +
                  " drives this light's colour: the sky's colour already includes it."
            : std::string("Tint the light color by a blackbody color temperature (Kelvin)");
        Toggle* useTemperature = AddToggleRow(ctx.Parent, "Use Color Temperature", light->UseColorTemperature,
            [w, e, n, undo, extras](bool v) {
                CommitComponentWithUndo<Components::Light>(w, e, n, undo, "Change Use Color Temperature",
                    [v](Components::Light& u) { u.UseColorTemperature = v; });
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::Light>(ex);
                    if (!c) continue;
                    Components::Light u = *c;
                    u.UseColorTemperature = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
                if (n)
                {
                    Editor::EditorChangeNotifications::ComponentChangedEvent ev{};
                    ev.world = w;
                    ev.entity = e;
                    ev.componentType = ECS::GetComponentTypeId<Components::Light>();
                    ev.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
                    n->NotifyComponentChanged(ev);
                }
            }, temperatureTooltip.c_str());
        if (colorDrivenBySky && useTemperature)
            InspectorUI::DisableRowOfControl(useTemperature);
        if (light->UseColorTemperature)
        {
            // Interactive gradient slider (drag the thumb to pick) synced with a numeric Kelvin
            // field — drag for visual selection, type for an exact value. Live-previews while
            // dragging; commits one undo step on release.
            UIElement* ctRow = InspectorUI::AddRow(ctx.Parent);
            InspectorUI::AddLabel(ctRow, "Color Temperature",
                "Blackbody color temperature (Kelvin). Drag the gradient thumb or type a value.\n"
                "~1900 candle, 3200 tungsten, 6500 daylight (D65), 10000+ blue sky.");
            UIElement* ctContainer = InspectorUI::AddFieldContainer(ctRow);
            ctContainer->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(6.0f));

            auto ctSlider = std::make_unique<KelvinSlider>();
            ctSlider->AddClass("inspector-kelvin-slider");
            ctSlider->SetMin(kKelvinGradientLoK);
            ctSlider->SetMax(kKelvinGradientHiK);
            ctSlider->SetStep(0.0f);
            ctSlider->SetShowValueBubble(false);
            ctSlider->SetTrackPaddingPx(kSliderTrackPaddingPx);
            ctSlider->SetValueWithoutNotify(light->ColorTemperature);

            auto ctField = std::make_unique<FloatField>();
            ctField->AddClass("inspector-float-field");
            ctField->SetValueWithoutNotify(light->ColorTemperature);
            ctField->Overrides().Set(Style::Width, StyleLength::Px(kSliderValueFieldWidthPx));
            FloatField* ctFieldRaw = ctField.get();
            KelvinSlider* ctSliderRaw = ctSlider.get();

            // Track the drag/edit as ONE interactive undo entry (mirrors AddComponentFloatRowWithDrag):
            // lazily begin on first preview so the snapshot captures the PRE-edit temperature (the
            // earlier preview-then-commit captured the already-changed value, so undo did nothing).
            using OptEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;
            auto editPtr = std::make_shared<OptEdit>();

            // The numeric box accepts free typing (raw text, no snap mid-edit), but every value that
            // reaches the renderer or gets committed/displayed is snapped to the valid Kelvin range, so
            // the live preview never shows an out-of-range temperature and commit lands back in range.
            // Preview and commit MUST snap identically, hence one shared clamp.
            auto clampKelvin = [](float v) {
                return std::round(std::clamp(v, kKelvinGradientLoK, kKelvinGradientHiK));
            };

            // Write a Kelvin value to the primary + every co-selected light (component copy + immediate
            // write). Shared by the preview and commit paths so the per-entity write lives in one place.
            auto applyKelvinToSelection = [w, e, extras](float v) {
                auto setOne = [w, v](ECS::EntityHandle ent) {
                    if (auto* c = w->GetComponent<Components::Light>(ent))
                    {
                        Components::Light u = *c;
                        u.ColorTemperature = v;
                        w->AddComponentImmediate(ent, u);
                    }
                };
                setOne(e);
                for (auto& ex : extras)
                    setOne(ex);
            };
            auto notifyPreview = [w, e, n, extras] {
                if (!n) return;
                n->NotifyComponentChange<Components::Light>(w, e, Editor::EditorChangeNotifications::ChangeKind::Preview);
                for (auto& ex : extras)
                    n->NotifyComponentChange<Components::Light>(w, ex, Editor::EditorChangeNotifications::ChangeKind::Preview);
            };

            // syncField is false when the numeric field itself is the change source: re-writing the
            // field mid-typing reformats partial/clamped input under the caret (the box has no equality
            // guard). The slider has no caret, so it is always synced to the parsed value.
            auto previewKelvin =
                [w, e, n, undo, extras, editPtr, applyKelvinToSelection, notifyPreview, clampKelvin, ctSliderRaw, ctFieldRaw](float v, bool syncField) {
                v = clampKelvin(v);
                ctSliderRaw->SetValueWithoutNotify(v);
                if (syncField)
                    ctFieldRaw->SetValueWithoutNotify(v);
                if (!w->GetComponent<Components::Light>(e))
                    return;
                if (!editPtr->has_value() && undo)
                {
                    auto target = extras.empty()
                        ? MakeComponentSnapshotTarget<Components::Light>(w, e, n, "Change Color Temperature")
                        : MakeMultiComponentSnapshotTarget<Components::Light>(w, e, extras, n, "Change Color Temperature");
                    editPtr->emplace(undo->BeginInteractiveEdit("Change Color Temperature", std::move(target)));
                }
                if (editPtr->has_value() && editPtr->value())
                    editPtr->value().Preview([applyKelvinToSelection, v] { applyKelvinToSelection(v); });
                else
                    applyKelvinToSelection(v);
                notifyPreview();
            };
            auto commitKelvin = [w, e, n, undo, extras, editPtr, applyKelvinToSelection, clampKelvin, ctSliderRaw, ctFieldRaw](float v) {
                v = clampKelvin(v);
                ctSliderRaw->SetValueWithoutNotify(v);
                ctFieldRaw->SetValueWithoutNotify(v); // commit canonicalizes the field (a typed "1" -> "1000")
                if (!w->GetComponent<Components::Light>(e))
                    return;
                if (editPtr->has_value() && editPtr->value())
                {
                    applyKelvinToSelection(v);
                    editPtr->value().Commit();
                    if (n)
                    {
                        n->NotifyComponentCommit<Components::Light>(w, e);
                        for (auto& ex : extras)
                            n->NotifyComponentCommit<Components::Light>(w, ex);
                    }
                    editPtr->reset();
                }
                else
                {
                    CommitComponentWithUndo<Components::Light>(w, e, n, undo, "Change Color Temperature",
                        [v](Components::Light& u) { u.ColorTemperature = v; });
                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::Light>(ex);
                        if (!c) continue;
                        Components::Light ue = *c; ue.ColorTemperature = v;
                        Editor::CommitComponentUpdate(w, ex, n, ue);
                    }
                }
            };

            // The slider syncs the field (no caret); the field must NOT re-write itself mid-typing.
            ctSlider->SetOnValueChanging([previewKelvin](const float& v) { previewKelvin(v, /*syncField=*/true); });
            ctSlider->SetOnValueChanged([commitKelvin](const float& v) { commitKelvin(v); });
            ctField->SetOnValueChanging([previewKelvin](const float& v) { previewKelvin(v, /*syncField=*/false); });
            ctField->SetOnValueChanged([commitKelvin](const float& v) { commitKelvin(v); });

            ctContainer->AddChild(std::move(ctSlider));
            ctContainer->AddChild(std::move(ctField));
            if (colorDrivenBySky)
            {
                InspectorUI::DisableRowOfControl(ctSliderRaw);
                InspectorUI::DisableRowOfControl(ctFieldRaw);
            }
        }

        // Editable: Color — swatch + RGB values, click opens color picker window
        {
            UIElement* row = InspectorUI::AddRow(ctx.Parent);
            const std::string drivenColorMeaning = intensityDrivenBySky
                ? " Under its sun illuminance curve the colour is a hue at unit brightness, so a channel can read "
                  "above 1: the brightness is in Intensity."
                : " From the light the colour also carries the dimming of a low sun and the night, so it darkens "
                  "while Intensity stays the sun overhead.";
            const std::string colorTooltip = colorDrivenBySky
                ? "Set by " + DrivingSkyDisplayName(*w, drivingSky) +
                      " every frame while it drives this light: the sun's color through the atmosphere by day, "
                      "moonlight at night." + drivenColorMeaning +
                      " Turn off its Sky sets the light's colour (under a custom illuminance curve) or its Sky "
                      "drives sun light to author the color here."
                : std::string("Light color (click to open color picker; double-click to reset to white)");
            Label* colorLabel = InspectorUI::AddLabel(row, "Color", colorTooltip.c_str());
            if (colorLabel) colorLabel->AddClass("inspector-label-no-drag");
            UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
            fieldContainer->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(6.0f));

            const uint32_t argb = LightColorToArgb(light->Color);

            auto swatch = std::make_unique<UIElement>();
            UIElement* swatchRaw = swatch.get();
            InspectorUI::StyleColorSwatch(swatchRaw, argb);
            fieldContainer->AddChild(std::move(swatch));

            auto rgbLabel = std::make_unique<Label>();
            rgbLabel->AddClass("inspector-text");
            rgbLabel->SetText(FormatRgb(light->Color));
            if (!colorDrivenBySky)
                rgbLabel->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
            Label* rgbLabelRaw = rgbLabel.get();
            fieldContainer->AddChild(std::move(rgbLabel));

            // Read-only while a sky drives the light: an edit here would be overwritten on the next
            // frame, so the row says who owns it instead of accepting one.
            if (colorDrivenBySky)
            {
                auto owner = std::make_unique<Label>();
                owner->AddClass("inspector-text");
                owner->SetText("set by the sky");
                owner->SetTooltip(colorTooltip);
                fieldContainer->AddChild(std::move(owner));
                InspectorUI::DisableRowOfControl(rgbLabelRaw);
            }
            else
            {

            // The picker's callbacks outlive an inspector rebuild: resolve the row through
            // weak refs instead of holding the freed widgets.
            auto clickHandler = [w, e, n, undo, openPicker,
                                 swatchRef = UIElement::MakeWeakRef(swatchRaw),
                                 rgbLabelRef = UIElement::MakeWeakRef(rgbLabelRaw)](UIEvent& ev) {
                if (ev.Button != 0)
                    return;
                ev.Stop();

                if (!openPicker)
                    return;

                auto* comp = w->GetComponent<Components::Light>(e);
                if (!comp)
                    return;

                uint32_t currentArgb = LightColorToArgb(comp->Color);

                using Edit = Editor::UndoRedoService::InteractiveEdit;
                auto edit = std::make_shared<Edit>();

                if (undo)
                {
                    auto target = MakeComponentSnapshotTarget<Components::Light>(w, e, n, "Light Color");
                    *edit = undo->BeginInteractiveEdit("Change Light Color", std::move(target));
                }

                auto updateUI = [swatchRef, rgbLabelRef, w, e]() {
                    auto* c = w->GetComponent<Components::Light>(e);
                    UIElement* swatch = swatchRef.Get();
                    Label* rgbLabel = rgbLabelRef.Get();
                    if (!c || !swatch || !rgbLabel) return;
                    InspectorUI::StyleColorSwatch(swatch, LightColorToArgb(c->Color));
                    rgbLabel->SetText(FormatRgb(c->Color));
                };

                ColorPickerCallbacks cbs;
                cbs.onApply = [w, e, n, edit, updateUI](uint32_t newArgb, float) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<Components::Light>(e);
                            if (c) ArgbToLightColor(newArgb, c->Color);
                        });
                        edit->Commit();
                    }
                    else
                    {
                        auto* c = w->GetComponent<Components::Light>(e);
                        if (!c) return;
                        Components::Light updated = *c;
                        ArgbToLightColor(newArgb, updated.Color);
                        Editor::CommitComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                cbs.onCancel = [edit, updateUI](){ 
                    if (*edit) edit->Cancel();
                    updateUI();
                };
                cbs.onValueChanging = [w, e, n, edit, updateUI](uint32_t newArgb, float) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<Components::Light>(e);
                            if (c) ArgbToLightColor(newArgb, c->Color);
                        });
                    }
                    else
                    {
                        auto* c = w->GetComponent<Components::Light>(e);
                        if (!c) return;
                        Components::Light updated = *c;
                        ArgbToLightColor(newArgb, updated.Color);
                        Editor::PreviewComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                openPicker(currentArgb, 1.0f, std::move(cbs));
            };

            swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
            rgbLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
            }

            // Double-click on the "Color" label resets to white
            auto lastColorClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
            colorLabel->RegisterEventHandler(kEventMouseDown, [w, e, n, undo, swatchRaw, rgbLabelRaw, lastColorClickTime, colorDrivenBySky](UIEvent& ev) {
                if (ev.Button != 0 || colorDrivenBySky)
                    return;
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastColorClickTime);
                if (elapsed > std::chrono::milliseconds::zero() && elapsed < GameEngine::Platform::GetDoubleClickInterval())
                {
                    ev.Stop();
                    constexpr float kDefaultColor[3] = {1.0f, 1.0f, 1.0f};
                    CommitComponentWithUndo<Components::Light>(w, e, n, undo, "Reset Light Color",
                        [&](Components::Light& u) { u.Color[0] = kDefaultColor[0]; u.Color[1] = kDefaultColor[1]; u.Color[2] = kDefaultColor[2]; });
                    *lastColorClickTime = std::chrono::steady_clock::time_point{};
                    return;
                }
                *lastColorClickTime = now;
            });
        }

        // Editable: Range (Point / Spot / Area)
        if (type == LightType::Point || type == LightType::Spot || type == LightType::Area)
        {
            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Range", light->Range, w, e, n,
                undo, "Change Light Range",
                [](Components::Light& u, float v) { u.Range = std::max(0.0f, v); },
                10.0f, "Maximum distance the light illuminates", extras);
        }

        // Editable: InnerAngle, OuterAngle (Spot only)
        if (type == LightType::Spot)
        {
            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Inner Angle (deg)", light->InnerAngle * kRad2Deg, w, e, n,
                undo, "Change Light Inner Angle",
                [](Components::Light& u, float v) { u.InnerAngle = std::max(0.0f, v * kDeg2Rad); },
                0.5f * kRad2Deg, "Inner cone half-angle in degrees (full-brightness region)", extras);
            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Outer Angle (deg)", light->OuterAngle * kRad2Deg, w, e, n,
                undo, "Change Light Outer Angle",
                [](Components::Light& u, float v) { u.OuterAngle = std::max(0.0f, v * kDeg2Rad); },
                0.8f * kRad2Deg, "Outer cone half-angle in degrees (falloff region)", extras);
        }

        if (type == LightType::Area)
        {
            auto* shapeField = InspectorUI::AddEnumRow(ctx.Parent, "Area Shape", kAreaLightShapes, light->AreaShape,
                "Area emitter shape imported from FBX when available");
            shapeField->SetOnValueChanged([w, e, n, undo, extras](AreaLightShape v) {
                CommitComponentWithUndo<Components::Light>(w, e, n, undo, "Change Area Light Shape",
                    [v](Components::Light& u) { u.AreaShape = v; });
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::Light>(ex);
                    if (!c) continue;
                    Components::Light u = *c;
                    u.AreaShape = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
            });

            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Area Width", light->AreaWidth, w, e, n,
                undo, "Change Area Light Width",
                [](Components::Light& u, float v) { u.AreaWidth = std::max(0.001f, v); },
                1.0f, "Area light width", extras);
            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Area Height", light->AreaHeight, w, e, n,
                undo, "Change Area Light Height",
                [](Components::Light& u, float v) { u.AreaHeight = std::max(0.001f, v); },
                1.0f, "Area light height", extras);
            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Area Radius", light->AreaRadius, w, e, n,
                undo, "Change Area Light Radius",
                [](Components::Light& u, float v) { u.AreaRadius = std::max(0.001f, v); },
                0.5f, "Area light radius for round/spherical emitters", extras);
        }

        if (type == LightType::Point || type == LightType::Spot || type == LightType::Area)
        {
            auto* falloffField = InspectorUI::AddEnumRow(ctx.Parent, "Falloff", kLightFalloffs, light->Falloff,
                "How this local light fades with distance. Range remains the cutoff distance.");
            falloffField->GetDropdown()->SetTooltip(FalloffTooltip(light->Falloff));
            falloffField->SetOnValueChanged([w, e, n, undo, extras](LightFalloff v) {
                CommitComponentWithUndo<Components::Light>(w, e, n, undo, "Change Light Falloff",
                    [v](Components::Light& u) { u.Falloff = v; });
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::Light>(ex);
                    if (!c) continue;
                    Components::Light u = *c;
                    u.Falloff = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
                if (n)
                {
                    Editor::EditorChangeNotifications::ComponentChangedEvent ev{};
                    ev.world = w;
                    ev.entity = e;
                    ev.componentType = ECS::GetComponentTypeId<Components::Light>();
                    ev.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
                    n->NotifyComponentChanged(ev);
                }
            });

            if (light->Falloff == LightFalloff::Custom)
            {
                AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Custom Decay", light->Decay, w, e, n,
                    undo, "Change Light Decay",
                    [](Components::Light& u, float v) { u.Decay = std::max(0.0f, v); },
                    2.0f, "Custom Decay: manual attenuation exponent used only by Custom falloff. This preserves the old Decay behavior.",
                    extras);
            }

            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Fog Contribution", light->FogContribution, w, e, n,
                undo, "Change Light Fog Contribution",
                [](Components::Light& u, float v) { u.FogContribution = std::max(0.0f, v); },
                1.0f, "How strongly this light scatters in volumetric fog", extras);

            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Fog Density Boost", light->FogDensityBoost, w, e, n,
                undo, "Change Light Fog Density Boost",
                [](Components::Light& u, float v) { u.FogDensityBoost = std::max(0.0f, v); },
                0.0f, "Extra local fog lift around this light for stylized shafts", extras);

            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Fog Anisotropy", light->FogAnisotropy, w, e, n,
                undo, "Change Light Fog Anisotropy",
                [](Components::Light& u, float v) { u.FogAnisotropy = std::clamp(v, -0.95f, 0.95f); },
                0.25f, "Per-light fog phase directionality", extras);

            AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Fog Origin Fade", light->FogOriginFade, w, e, n,
                undo, "Change Light Fog Origin Fade",
                [](Components::Light& u, float v) { u.FogOriginFade = std::clamp(v, 0.0f, 1.0f); },
                0.2f, "Fraction of light range used to fade fog near the light source and reduce near-origin noise", extras);
        }

        // Editable: CastsShadows (Directional / Point / Spot — not Ambient)
        if (type != LightType::Ambient)
        {
            AddToggleRow(ctx.Parent, "Casts Shadows", light->CastsShadows,
                [w, e, n, undo, extras](bool v) {
                    CommitComponentWithUndo<Components::Light>(w, e, n, undo, "Change Light Shadows",
                        [v](Components::Light& u) { u.CastsShadows = v; });
                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::Light>(ex);
                        if (!c) continue;
                        Components::Light u = *c;
                        u.CastsShadows = v;
                        Editor::CommitComponentUpdate(w, ex, n, u);
                    }
                    // The shadow rows (softness, cascades, filter quality, PCSS/MSM
                    // controls) are gated on CastsShadows at build time; refresh the
                    // section so they follow the toggle.
                    if (n)
                    {
                        Editor::EditorChangeNotifications::ComponentChangedEvent ev{};
                        ev.world = w;
                        ev.entity = e;
                        ev.componentType = ECS::GetComponentTypeId<Components::Light>();
                        ev.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
                        n->NotifyComponentChanged(ev);
                    }
                }, "Enable shadow casting from this light");
        }

        // Editable: CascadeCount (Directional only)
        if (type == LightType::Directional)
        {
            constexpr int kCascadeMin = 0;
            constexpr int kCascadeMax = static_cast<int>(Engine::Renderer::kMaxShadowCascades);
            IntField* cascadeField = AddComponentIntRowWithDrag<Components::Light>(ctx.Parent, "Cascade Count", static_cast<int>(light->CascadeCount), w, e, n,
                undo, "Change Light Cascade Count",
                [](Components::Light& u, int v) { u.CascadeCount = static_cast<uint32>(std::clamp(v, 0, static_cast<int>(Engine::Renderer::kMaxShadowCascades))); },
                4, "Number of shadow map cascades (0 = disabled)", extras);
            if (cascadeField)
                cascadeField->SetRange(kCascadeMin, kCascadeMax);
        }

        // Shadow softness slider (Directional + castsShadows only)
        if (type == LightType::Directional && light->CastsShadows)
        {
            auto* rs = EngineCore::GetInstance().GetRenderServices();
            auto* feature = rs ? rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>() : nullptr;
            if (feature)
            {
                // Shadow filter quality (replaces the legacy PCSS checkbox).
                // PCSS / MSM4 silently fall back to PoissonPCF when their
                // prerequisites (bindless / a ready moments texture) are
                // not met; the user's selection persists across hot-reload.
                {
                    using Q = Engine::Renderer::ShadowFilterQuality;
                    static constexpr EnumEntry<Q> kQualities[] = {
                        {Q::Grid5x5,    "5x5 Grid PCF"},
                        {Q::Grid3x3,    "3x3 Grid PCF"},
                        {Q::PoissonPCF, "Poisson PCF"},
                        {Q::PCSS,       "PCSS (Contact Hardening)"},
                        {Q::DPCF,       "DPCF (Contact Hardening, cheaper)"},
                        {Q::MSM4,       "MSM4 (Moment Shadow Maps)"},
                    };
                    auto* qField = InspectorUI::AddEnumRow(
                        ctx.Parent,
                        "Filter Quality",
                        kQualities,
                        feature->GetFilterQuality(),
                        "Shadow filter quality:\n"
                        "  Grid PCF (5x5/3x3): cheap, deterministic, hard edges.\n"
                        "  Poisson PCF: variable-tap soft edges, runs without TAA.\n"
                        "  PCSS: contact-hardening soft shadows (penumbra grows with\n"
                        "        blocker distance); needs bindless.\n"
                        "  DPCF: contact hardening from ONE tap set -- no blocker\n"
                        "        search and no dependent read, so it costs less than\n"
                        "        PCSS. Approximates the hardening rather than deriving\n"
                        "        a physical penumbra, so contact edges stay softer than\n"
                        "        PCSS's. Needs bindless.\n"
                        "  MSM4: pre-filtered noise-free soft shadows; falls back to\n"
                        "        PCSS until the moments-write/blur passes are wired.");
                    qField->SetOnValueChanged([w, e, n](Q v) {
                        auto* rs2 = EngineCore::GetInstance().GetRenderServices();
                        if (auto* f = rs2 ? rs2->GetFeature<Engine::Renderer::ShadowMapRenderFeature>() : nullptr)
                            f->SetFilterQuality(v);
                        // Trigger an InspectorRebuild so the conditional rows
                        // (angular diameter, PCSS/DPCF controls, MSM moments,
                        // Poisson taps) re-evaluate their visibility against
                        // the new FilterQuality without requiring the user to
                        // reselect the light.
                        if (n)
                        {
                            Editor::EditorChangeNotifications::ComponentChangedEvent ev{};
                            ev.world = w;
                            ev.entity = e;
                            ev.componentType = ECS::GetComponentTypeId<Components::Light>();
                            ev.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
                            n->NotifyComponentChanged(ev);
                        }
                    });
                }

                using Q = Engine::Renderer::ShadowFilterQuality;
                const Q activeQuality = feature->GetFilterQuality();
                const bool showMsm = (activeQuality == Q::MSM4);
                // Receiver-plane bias is PCSS-only: DPCF has no per-tap reference
                // depth to slope-correct, so the row would be a knob that does
                // nothing under it.
                const bool showPcss = (activeQuality == Q::PCSS);
                // DPCF reads BOTH of these. Its fixed kernel radius comes from
                // Max Penumbra (0 = auto), and its sample count comes from the
                // tap dropdown via GE_PcssTapsForRadius, exactly as PCSS's do.
                const bool showPenumbraCap = (activeQuality == Q::PCSS || activeQuality == Q::DPCF);
                const bool showPoissonTaps = (activeQuality == Q::PCSS || activeQuality == Q::DPCF ||
                                              activeQuality == Q::PoissonPCF);
                // PCSS derives penumbra from angular diameter × blocker distance.
                // DPCF uses the same tan(half angle) to weight contact hardening.
                // Grid / Poisson / MSM4 do not read it.
                const bool showAngularDiameter = (activeQuality == Q::PCSS || activeQuality == Q::DPCF);

                if (showAngularDiameter)
                {
                    AddComponentFloatRowWithDrag<Components::Light>(ctx.Parent, "Angular Diameter (deg)",
                        light->ShadowAngularDiameter, w, e, n,
                        undo, "Change Light Angular Diameter",
                        [](Components::Light& u, float v) { u.ShadowAngularDiameter = std::clamp(v, 0.0f, 20.0f); },
                        0.53f,
                        "Apparent size of the light's disc, in degrees — the only physical\n"
                        "control on penumbra width. An occluder d behind the receiver casts\n"
                        "a penumbra of d x tan(half angle); no divide by distance, the light\n"
                        "is at infinity.\n"
                        "0 = perfectly collimated (hard shadows); 0.53 = the sun; 20 ~ broad overcast.",
                        extras);
                }

                // MSM4 moments resolution.
                if (showMsm)
                {
                    static constexpr EnumEntry<uint32_t> kMomentsRes[] = {
                        {512u,  "512"},
                        {1024u, "1024"},
                        {2048u, "2048"},
                    };
                    auto* momRes = InspectorUI::AddEnumRow(
                        ctx.Parent,
                        "Moments Resolution",
                        kMomentsRes,
                        feature->GetMomentsResolution(),
                        "MSM4 moments-array resolution per cascade.\n"
                        "1024 is the default sweet spot for quality vs memory.\n"
                        "512 trades visible aliasing on contact shadows for 4x\n"
                        "less memory. 2048 yields fine penumbra detail at 4x cost.");
                    momRes->SetOnValueChanged([](uint32_t v) {
                        auto* rs2 = EngineCore::GetInstance().GetRenderServices();
                        if (auto* f = rs2 ? rs2->GetFeature<Engine::Renderer::ShadowMapRenderFeature>() : nullptr)
                            f->SetMomentsResolution(v);
                    });
                }

                // MSM4 blur kernel mode.
                if (showMsm)
                {
                    using BlurMode = Engine::Renderer::MsmBlurMode;
                    static constexpr EnumEntry<BlurMode> kBlurModes[] = {
                        {BlurMode::Linear5Tap,   "Linear (5-tap, fast)"},
                        {BlurMode::Discrete9Tap, "Discrete (9-tap)"},
                    };
                    auto* blurMode = InspectorUI::AddEnumRow(
                        ctx.Parent,
                        "Moments Blur Kernel",
                        kBlurModes,
                        feature->GetMsmBlurMode(),
                        "MSM4 separable Gaussian blur kernel.\n"
                        "Linear5Tap (default) — 5 bilinear samples per pass; same\n"
                        "sigma=2 Gaussian as the 9-tap, ~1.8x fewer texture fetches.\n"
                        "Discrete9Tap — 9 point samples per pass; slightly sharper at\n"
                        "depth discontinuities but ~1.8x more fragment work.");
                    blurMode->SetOnValueChanged([](BlurMode v) {
                        auto* rs2 = EngineCore::GetInstance().GetRenderServices();
                        if (auto* f = rs2 ? rs2->GetFeature<Engine::Renderer::ShadowMapRenderFeature>() : nullptr)
                            f->SetMsmBlurMode(v);
                    });
                }

                // Poisson disk tap count — used by Poisson PCF, PCSS and DPCF.
                if (showPoissonTaps)
                {
                    static constexpr EnumEntry<uint32_t> kTapCounts[] = {
                        {8u,  "8"},
                        {16u, "16"},
                        {32u, "32"},
                        {64u, "64"},
                    };
                    auto* tapField = InspectorUI::AddEnumRow(
                        ctx.Parent,
                        "PCSS Disk Taps",
                        kTapCounts,
                        feature->GetPcssTapCount(),
                        "Disk tap count for PCSS, DPCF and Poisson PCF.\n"
                        "Higher = cleaner penumbra at higher per-fragment cost (linear in N).\n"
                        "16 is the default; 32 noticeably reduces dither; 64 approaches\n"
                        "TAA-quality output but costs ~4x the shadow fetches of 16.\n"
                        "\n"
                        "This is the count at a REFERENCE kernel width, not a fixed budget:\n"
                        "the shader scales it with how many texels the kernel actually spans,\n"
                        "so a fine-texel near cascade gets more taps and a coarse far one\n"
                        "stops paying for taps it cannot resolve.\n"
                        "\n"
                        "DPCF spends its taps on ONE gather that yields both the occluded\n"
                        "percentage and the occluder distance, so a given count costs it less\n"
                        "than the same count costs PCSS.");
                    tapField->SetOnValueChanged([](uint32_t v) {
                        auto* rs2 = EngineCore::GetInstance().GetRenderServices();
                        if (auto* f = rs2 ? rs2->GetFeature<Engine::Renderer::ShadowMapRenderFeature>() : nullptr)
                            f->SetPcssTapCount(v);
                    });
                }

                // Penumbra width in WORLD units. Read by both contact-hardening
                // filters, but it does a different job in each - see the tooltip.
                if (showPenumbraCap)
                {
                    auto mpRow = AddSliderWithFloatValueRow(ctx.Parent, "Max Penumbra (World)", feature->GetPcssMaxPenumbra(), 0.0f,
                        Engine::Renderer::kPcssMaxPenumbraWorld,
                        "Penumbra width in WORLD units, so it stays the same size across\n"
                        "cascades and at any shadow-map resolution.\n"
                        "0 = auto (0.5 m).\n"
                        "\n"
                        "PCSS: an upper CAP. The penumbra is derived from the light's\n"
                        "  angular diameter and the blocker distance; this only bites when\n"
                        "  that derivation would go wider.\n"
                        "\n"
                        "DPCF: the kernel radius ITSELF, because DPCF never learns how far\n"
                        "  the blocker is before filtering - that is what buys it the single\n"
                        "  tap set. So this is DPCF's main softness knob, not a limit.\n"
                        "\n"
                        "Going much wider than the tap budget can sample will band; raise\n"
                        "Tap Count alongside it.");
                    Label* mpLabel = mpRow.Label;
                    Slider* mpSliderRaw = mpRow.Slider;
                    FloatField* mpFieldRaw = mpRow.ValueField;
                    if (!mpLabel || !mpSliderRaw || !mpFieldRaw)
                        return;
                    mpSliderRaw->SetTrackPaddingPx(kSliderTrackPaddingPx);
                    mpSliderRaw->SetStep(0.001f);

                    auto applyMp = [mpSliderRaw, mpFieldRaw](float v) {
                        v = std::clamp(v, 0.0f, Engine::Renderer::kPcssMaxPenumbraWorld);
                        auto* rs2 = EngineCore::GetInstance().GetRenderServices();
                        if (auto* f = rs2 ? rs2->GetFeature<Engine::Renderer::ShadowMapRenderFeature>() : nullptr)
                            f->SetPcssMaxPenumbra(v);
                        mpSliderRaw->SetValueWithoutNotify(v);
                        mpFieldRaw->SetValueWithoutNotify(v);
                    };

                    mpSliderRaw->SetOnValueChanging([applyMp](const float& v) { applyMp(v); });
                    mpSliderRaw->SetOnValueChanged([applyMp](const float& v) { applyMp(v); });
                    mpFieldRaw->SetOnValueChanging([applyMp](const float& v) { applyMp(v); });
                    mpFieldRaw->SetOnValueChanged([applyMp](const float& v) { applyMp(v); });

                    constexpr float kDefaultMaxPenumbra = 0.0f; // auto (tap-budget bound)
                    auto lastMpClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
                    mpLabel->RegisterEventHandler(kEventMouseDown, [applyMp, lastMpClickTime, kDefaultMaxPenumbra](UIEvent& ev) {
                        if (ev.Button != 0) return;
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastMpClickTime);
                        if (lastMpClickTime->time_since_epoch().count() != 0 && elapsed < GameEngine::Platform::GetDoubleClickInterval())
                        {
                            ev.Stop();
                            applyMp(kDefaultMaxPenumbra);
                            *lastMpClickTime = {};
                            return;
                        }
                        *lastMpClickTime = now;
                    });

                    SetupLabelDragSlider(mpLabel, mpSliderRaw);
                }

                // PCSS receiver-plane depth bias (slope-corrected self-shadow fix).
                if (showPcss)
                {
                    UIElement* rpbRow = InspectorUI::AddRow(ctx.Parent);
                    InspectorUI::AddLabel(rpbRow, "PCSS Receiver Plane Bias",
                        "Receiver-plane depth bias: per-pixel slope correction. Fixes acne on grazing surfaces.\nCosts a dFdx/dFdy pair per shadowed fragment.");
                    UIElement* rpbContainer = InspectorUI::AddFieldContainer(rpbRow);
                    auto rpbCheck = std::make_unique<Checkbox>();
                    rpbCheck->SetChecked(feature->IsPcssReceiverPlaneBias());
                    rpbCheck->SetTooltip("Enable receiver-plane slope bias for PCSS shadows.");
                    rpbCheck->SetOnValueChanged([](const bool& v) {
                        auto* rs2 = EngineCore::GetInstance().GetRenderServices();
                        if (auto* f = rs2 ? rs2->GetFeature<Engine::Renderer::ShadowMapRenderFeature>() : nullptr)
                            f->SetPcssReceiverPlaneBias(v);
                    });
                    rpbContainer->AddChild(std::move(rpbCheck));
                }

            }
        }
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::Light>(std::move(fn));
}

} // namespace GameEngine
