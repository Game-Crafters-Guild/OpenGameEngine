#include "Inspectors/ColorGradeEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/ColorGradeEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"

#include "Panels/ColorPicker.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "Types/ColorUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace GameEngine
{

namespace
{
using G = Components::ColorGradeEffect;

// A band's zero-centered RGB chroma offset (0 = neutral). Both the wheel puck and
// the numeric fields author this single value.
struct Offset3
{
    float R = 0.0f;
    float G = 0.0f;
    float B = 0.0f;
};

// The band puck stores a balanced zero-centered log offset (0 = neutral). The
// wheel's hue vector has its channel mean removed and is normalized to a peak
// magnitude of one before saturation is applied. The selected hue therefore
// raises at least one channel while lowering its complements, leaving the
// separate Y/lightness control responsible for the common channel offset.
void OffsetToHueSat(const float32 offset[3], float& hue, float& sat)
{
    const float mean = (offset[0] + offset[1] + offset[2]) / 3.0f;
    const float r = offset[0] - mean;
    const float g = offset[1] - mean;
    const float b = offset[2] - mean;
    const float peak = std::max({std::abs(r), std::abs(g), std::abs(b)});
    sat = std::clamp(peak, 0.0f, 1.0f);
    if (peak <= 1e-6f)
    {
        hue = 0.0f;
        sat = 0.0f;
        return;
    }

    // Hue is invariant to a common channel offset and positive scale. Shift
    // the centered vector into RGB's non-negative domain for the HSV helper.
    const float minimum = std::min({r, g, b});
    const float maximum = std::max({r, g, b});
    const float range = maximum - minimum;
    float ignoredSaturation = 0.0f;
    float ignoredValue = 0.0f;
    ColorUtils::RgbToHsv(
        (r - minimum) / range,
        (g - minimum) / range,
        (b - minimum) / range,
        hue, ignoredSaturation, ignoredValue);
}

void HueSatToOffset(float hue, float sat, float& or_, float& og, float& ob)
{
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ColorUtils::HsvToRgb(hue, 1.0f, 1.0f, r, g, b);

    const float mean = (r + g + b) / 3.0f;
    r -= mean;
    g -= mean;
    b -= mean;
    const float peak = std::max({std::abs(r), std::abs(g), std::abs(b)});
    const float scale = peak > 1e-6f ? std::clamp(sat, 0.0f, 1.0f) / peak : 0.0f;
    or_ = r * scale;
    og = g * scale;
    ob = b * scale;
}

void OffsetToHueSat(const Offset3& o, float& hue, float& sat)
{
    const float32 a[3] = {o.R, o.G, o.B};
    OffsetToHueSat(a, hue, sat);
}

Offset3 HueSatToOffset3(float hue, float sat)
{
    Offset3 o;
    HueSatToOffset(hue, sat, o.R, o.G, o.B);
    return o;
}

float ClampUnit(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

// Hex remains a conventional value-1 tint view of the balanced wheel vector.
std::array<float, 3> OffsetToTint(const Offset3& o)
{
    float hue = 0.0f;
    float sat = 0.0f;
    OffsetToHueSat(o, hue, sat);
    float r = 1.0f, g = 1.0f, b = 1.0f;
    ColorUtils::HsvToRgb(hue, sat, 1.0f, r, g, b);
    return {r, g, b};
}

// A typed tint color -> canonical band offset via the shared hue/sat mapping. Value
// is forced to 1 so a typed color and the puck never disagree on brightness (band
// brightness is the separate lightness slider, mirroring Unity's SMH trackballs).
Offset3 TintToOffset(float r, float g, float b)
{
    float h = 0.0f, s = 0.0f, v = 0.0f;
    ColorUtils::RgbToHsv(ClampUnit(r), ClampUnit(g), ClampUnit(b), h, s, v);
    return HueSatToOffset3(h, s);
}

std::string TintToHex(const std::array<float, 3>& tint)
{
    auto to255 = [](float c) { return static_cast<int>(std::lround(ClampUnit(c) * 255.0f)); };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", to255(tint[0]), to255(tint[1]), to255(tint[2]));
    return buf;
}

bool HexToTint(const std::string& text, float& r, float& g, float& b)
{
    std::string h;
    for (char c : text)
    {
        if (!std::isspace(static_cast<unsigned char>(c)))
            h.push_back(c);
    }
    if (!h.empty() && h[0] == '#')
        h.erase(h.begin());
    if (h.size() != 6)
        return false;

    auto nibble = [](char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return -1;
    };

    int n[6];
    for (int i = 0; i < 6; ++i)
    {
        n[i] = nibble(h[static_cast<std::size_t>(i)]);
        if (n[i] < 0)
            return false;
    }
    r = static_cast<float>(n[0] * 16 + n[1]) / 255.0f;
    g = static_cast<float>(n[2] * 16 + n[3]) / 255.0f;
    b = static_cast<float>(n[4] * 16 + n[5]) / 255.0f;
    return true;
}

// Shared pointers so the wheel, the Y/R/G/B fields and the hex field can drive one
// another. The syncing guard blocks a programmatic control update (which fires no
// value callbacks anyway) from being re-interpreted as a user edit.
struct BandSync
{
    ColorWheelElement* Wheel = nullptr;
    FloatField* Y = nullptr;
    FloatField* R = nullptr;
    FloatField* G = nullptr;
    FloatField* B = nullptr;
    TextField* Hex = nullptr;
    bool Syncing = false;
};

// Fixed square wheel: fits the narrowest 3-up column (< kBandColumnMinWidthPx)
// and stays centered on wider ones. A fixed size avoids reserving an oversized
// layout slot from an AspectRatio derived off the full column width.
constexpr float kWheelSizePx = 84.0f;
// Per-column min width (fields-driven: four YRGB cells + the wheel). Below this
// the band row's flex-wrap drops columns to fewer per line (3 -> 2 -> 1),
// degrading to a vertical stack on a narrow panel. Sized so three columns sit
// side by side at a normal inspector width (~350px+) before wrapping.
constexpr float kBandColumnMinWidthPx = 96.0f;
constexpr float kBandColumnGapPx = 6.0f;
// Fixed caption column for the Value/Hex rows so the slider and hex box left-align.
constexpr float kCaptionColWidthPx = 34.0f;
constexpr float kBandGapPx = 6.0f;
constexpr float kBandSpacingPx = 16.0f;
constexpr float kFieldGapPx = 4.0f;
constexpr float kTitleFontSizePx = 13.0f;
constexpr float kCaptionFontSizePx = 12.0f;
// Compact slider min width so three Value sliders fit their columns before wrap.
constexpr float kValueSliderMinWidthPx = 44.0f;
// Below this inspector-content width, prioritize the independent RGB channels;
// Y remains available through the Lightness slider directly above the row.
constexpr float kYFieldMinInspectorWidthPx = 400.0f;

using ApplyColorFn = std::function<void(G&, float, float, float)>;
using ApplyLightnessFn = std::function<void(G&, float)>;

Label* AddCaption(UIElement* parent, const char* text, float fontSize)
{
    auto label = std::make_unique<Label>();
    Label* raw = label.get();
    raw->SetText(text);
    raw->Overrides().Set(Style::FontSize, StyleLength::Px(fontSize));
    parent->AddChild(std::move(label));
    return raw;
}

struct ChannelControl
{
    UIElement* Container = nullptr;
    Label* Caption = nullptr;
    FloatField* Field = nullptr;
};

class ResponsiveYrgbRow final : public UIElement
{
public:
    explicit ResponsiveYrgbRow(UIElement* widthReference)
        : m_WidthReference(widthReference)
    {
    }

    void SetYContainer(UIElement* container) { m_YContainer = container; }

    void OnPostLayout() override
    {
        if (!m_WidthReference || !m_YContainer)
            return;

        const float availableWidth = m_WidthReference->GetLayoutWidth();
        if (availableWidth <= 0.0f)
            return;

        const bool shouldHide = availableWidth < kYFieldMinInspectorWidthPx;
        if (shouldHide == m_YHidden)
            return;

        m_YHidden = shouldHide;
        m_YContainer->Overrides().Set(
            Style::Display, shouldHide ? DisplayMode::None : DisplayMode::Flex);
        m_YContainer->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

private:
    UIElement* m_WidthReference = nullptr;
    UIElement* m_YContainer = nullptr;
    bool m_YHidden = false;
};

ChannelControl AddChannelField(UIElement* row, const char* letter, float initial, const char* tooltip)
{
    auto col = std::make_unique<UIElement>();
    UIElement* colRaw = col.get();
    col->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::FlexBasis, StyleLength::Px(0.0f))
        .Set(Style::Gap, StyleLength::Px(2.0f));

    Label* caption = AddCaption(col.get(), letter, kCaptionFontSizePx);
    caption->AddClass("color-grade-channel-caption");

    auto field = std::make_unique<FloatField>();
    FloatField* raw = field.get();
    raw->AddClass("inspector-float-field");
    raw->SetFixedDecimalPlaces(2);
    raw->SetValueWithoutNotify(initial);
    if (tooltip && tooltip[0] != '\0')
        raw->SetTooltip(tooltip);
    raw->Overrides().Set(Style::Width, StyleLength::Percent(100.0f));
    col->AddChild(std::move(field));

    row->AddChild(std::move(col));
    return {colRaw, caption, raw};
}

// One Unity SMH / Unreal-style trackball column: title, hue/sat wheel, then a
// compact master lightness slider and numeric YRGB + hex tint input directly
// beneath the wheel. Built as an equal-width flex item so three columns sit
// side by side (parent is the wrapping band row). The wheel, the fields and the
// hex field all read/write the same band offset and stay in sync; a wheel drag
// or a field edit is one coalesced undo entry.
void BuildColorGradeBand(UIElement* parent, const char* title,
                         const float32 offset[3], float lightness,
                         ApplyColorFn applyColor, ApplyLightnessFn applyLightness,
                         ECS::World* w, ECS::EntityHandle e,
                         Editor::EditorChangeNotifications* n, Editor::UndoRedoService* undo,
                         std::string colorEditName, std::string lightnessEditName)
{
    using namespace InspectorDrag;

    auto sync = std::make_shared<BandSync>();

    // One coalesced-undo lifecycle for the band color, shared by the wheel and the
    // numeric fields (only one gesture is ever live at a time). First preview begins
    // the interactive edit; commit ends it as a single undo entry.
    auto colorHandlers = MakeComponentInteractiveHandlers<G, Offset3>(
        w, e, n, undo, colorEditName,
        [applyColor](G& u, Offset3 o) { applyColor(u, o.R, o.G, o.B); }, {});
    std::function<void(Offset3)> colorPreview = std::move(colorHandlers.first);
    std::function<void(Offset3)> colorCommit = std::move(colorHandlers.second);

    // Push helpers write controls WITHOUT firing their callbacks (SetHue/Saturation
    // and SetValueWithoutNotify are silent), so cross-control sync never recurses.
    auto pushPuck = [sync](const Offset3& o)
    {
        float h = 0.0f, s = 0.0f;
        OffsetToHueSat(o, h, s);
        sync->Wheel->SetHue(h);
        sync->Wheel->SetSaturation(s);
    };
    auto pushRgb = [sync](const Offset3& o)
    {
        sync->R->SetValueWithoutNotify(o.R);
        sync->G->SetValueWithoutNotify(o.G);
        sync->B->SetValueWithoutNotify(o.B);
    };
    auto pushHex = [sync](const Offset3& o)
    {
        sync->Hex->SetValueWithoutNotify(TintToHex(OffsetToTint(o)));
    };
    auto syncAll = [sync, pushPuck, pushRgb, pushHex](const Offset3& o)
    {
        sync->Syncing = true;
        pushPuck(o);
        pushRgb(o);
        pushHex(o);
        sync->Syncing = false;
    };

    auto offsetFromFields = [sync]() -> Offset3
    {
        return {
            std::clamp(sync->R->GetValue(), -1.0f, 1.0f),
            std::clamp(sync->G->GetValue(), -1.0f, 1.0f),
            std::clamp(sync->B->GetValue(), -1.0f, 1.0f)};
    };

    // --- Column (equal-width flex item in the wrapping band row) -----------------
    auto card = std::make_unique<UIElement>();
    card->AddClass("color-grade-band");
    card->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::AlignItems, AlignItems::Stretch)
        .Set(Style::Gap, StyleLength::Px(kBandGapPx))
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::FlexShrink, 1.0f)
        .Set(Style::FlexBasis, StyleLength::Px(0.0f))
        .Set(Style::MinWidth, StyleLength::Px(kBandColumnMinWidthPx));
    UIElement* cardRaw = card.get();

    Label* titleLabel = AddCaption(cardRaw, title, kTitleFontSizePx);
    titleLabel->Overrides().Set(Style::AlignSelf, AlignItems::Center);

    // Wheel on top, centered. Grading style: center-neutral disc with a hue ring
    // at the rim, so the puck's distance reads as the strength of the hue push
    // (unlike the full-sat color-picker disc). Fixed square size; the element
    // insets paint and hit-testing to min(w,h), so the puck tracks correctly.
    auto wheel = std::make_unique<ColorWheelElement>();
    wheel->SetGradingStyle(true);
    wheel->Overrides()
        .Set(Style::Width, StyleLength::Px(kWheelSizePx))
        .Set(Style::Height, StyleLength::Px(kWheelSizePx))
        .Set(Style::FlexShrink, 0.0f)
        .Set(Style::AlignSelf, AlignItems::Center);
    {
        float h0 = 0.0f, s0 = 0.0f;
        OffsetToHueSat(offset, h0, s0);
        wheel->SetHue(h0);
        wheel->SetSaturation(s0);
    }
    sync->Wheel = wheel.get();
    cardRaw->AddChild(std::move(wheel));

    // Master lightness/value slider directly beneath the wheel (centered at neutral).
    auto lightHandlers = MakeComponentInteractiveHandlers<G, float>(
        w, e, n, undo, lightnessEditName,
        [applyLightness](G& u, float v) { applyLightness(u, std::clamp(v, -1.0f, 1.0f)); }, {});
    std::function<void(float)> lightPreview = std::move(lightHandlers.first);
    std::function<void(float)> lightCommit = std::move(lightHandlers.second);

    auto valueRow = std::make_unique<UIElement>();
    valueRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::AlignItems, AlignItems::Stretch)
        .Set(Style::Gap, StyleLength::Px(2.0f));
    Label* lightnessCaption = AddCaption(valueRow.get(), "Lightness", kCaptionFontSizePx);
    lightnessCaption->Overrides().Set(Style::AlignSelf, AlignItems::Center);
    lightnessCaption->SetTooltip("Master lightness for this tonal band; double-click to reset.");

    auto slider = std::make_unique<Slider>();
    Slider* sliderRaw = slider.get();
    sliderRaw->AddClass("property-slider");
    sliderRaw->SetMin(-1.0f);
    sliderRaw->SetMax(1.0f);
    sliderRaw->SetStep(0.0f);
    sliderRaw->SetCentered(true);
    sliderRaw->SetShowValueBubble(true);
    sliderRaw->SetValueWithoutNotify(lightness);
    sliderRaw->Overrides()
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::MinWidth, StyleLength::Px(kValueSliderMinWidthPx));
    sliderRaw->SetTooltip("Master lightness for this tonal band (0 = neutral).");
    sliderRaw->SetOnValueChanging([sync, lightPreview](const float& v)
    {
        sync->Y->SetValueWithoutNotify(v);
        lightPreview(v);
    });
    sliderRaw->SetOnValueChanged([sync, lightCommit](const float& v)
    {
        sync->Y->SetValueWithoutNotify(v);
        lightCommit(v);
    });
    SetupLabelDragSlider(lightnessCaption, sliderRaw, nullptr, nullptr, /*defaultValue=*/0.0f);
    valueRow->AddChild(std::move(slider));
    cardRaw->AddChild(std::move(valueRow));

    // Resolve-style YRGB values: Y is the band's master lightness and RGB are
    // the actual additive log-channel offsets. All four are zero at neutral.
    constexpr const char* kYTooltip = "Master lightness offset for this tonal band (-1 to 1; neutral 0).";
    constexpr const char* kRgbTooltip = "Additive log-channel offset (-1 to 1; neutral 0).";
    auto rgbRow = std::make_unique<ResponsiveYrgbRow>(parent);
    ResponsiveYrgbRow* rgbRowRaw = rgbRow.get();
    rgbRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(kFieldGapPx));
    const Offset3 initialOffset{offset[0], offset[1], offset[2]};
    const ChannelControl yControl = AddChannelField(rgbRow.get(), "Y", lightness, kYTooltip);
    const ChannelControl rControl = AddChannelField(rgbRow.get(), "R", initialOffset.R, kRgbTooltip);
    const ChannelControl gControl = AddChannelField(rgbRow.get(), "G", initialOffset.G, kRgbTooltip);
    const ChannelControl bControl = AddChannelField(rgbRow.get(), "B", initialOffset.B, kRgbTooltip);
    sync->Y = yControl.Field;
    sync->R = rControl.Field;
    sync->G = gControl.Field;
    sync->B = bControl.Field;
    rgbRowRaw->SetYContainer(yControl.Container);
    cardRaw->AddChild(std::move(rgbRow));

    // Hex tint field.
    auto hexRow = std::make_unique<UIElement>();
    hexRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(kFieldGapPx));
    Label* hexCaption = AddCaption(hexRow.get(), "Hex", kCaptionFontSizePx);
    hexCaption->Overrides().Set(Style::Width, StyleLength::Px(kCaptionColWidthPx)).Set(Style::FlexShrink, 0.0f);

    auto hexField = std::make_unique<TextField>();
    TextField* hexRaw = hexField.get();
    hexRaw->AddClass("inspector-text-field");
    hexRaw->SetValueWithoutNotify(TintToHex(OffsetToTint(initialOffset)));
    hexRaw->SetTooltip("Tint color as #RRGGBB.");
    hexRaw->Overrides().Set(Style::FlexGrow, 1.0f);
    sync->Hex = hexRaw;
    hexRow->AddChild(std::move(hexField));
    cardRaw->AddChild(std::move(hexRow));

    parent->AddChild(std::move(card));

    // --- Wiring -----------------------------------------------------------------
    // Wheel drag: preview each tick, commit once on release; keep the numeric
    // fields (the non-source controls) in sync live.
    sync->Wheel->SetOnChange([sync, colorPreview, pushRgb, pushHex](float h, float s)
    {
        const Offset3 o = HueSatToOffset3(h, s);
        colorPreview(o);
        if (!sync->Syncing)
        {
            pushRgb(o);
            pushHex(o);
        }
    });
    sync->Wheel->SetOnChangeEnd([sync, colorCommit]()
    {
        const Offset3 o = HueSatToOffset3(sync->Wheel->GetHue(), sync->Wheel->GetSaturation());
        colorCommit(o);
    });

    // RGB field edit: preview while typing (move the puck + hex, leave the edited
    // fields alone), commit + normalize display on Enter/blur.
    auto onRgbChanging = [sync, colorPreview, pushPuck, pushHex, offsetFromFields]()
    {
        if (sync->Syncing)
            return;
        const Offset3 o = offsetFromFields();
        colorPreview(o);
        pushPuck(o);
        pushHex(o);
    };
    auto onRgbCommitted = [sync, colorCommit, syncAll, offsetFromFields]()
    {
        if (sync->Syncing)
            return;
        const Offset3 o = offsetFromFields();
        colorCommit(o);
        syncAll(o);
    };
    for (FloatField* f : {sync->R, sync->G, sync->B})
    {
        f->SetOnValueChanging([onRgbChanging](const float&) { onRgbChanging(); });
        f->SetOnValueChanged([onRgbCommitted](const float&) { onRgbCommitted(); });
    }

    // Use the standard inspector label scrubber for the compact channel
    // captions as well as direct text editing. Double-click resets one channel.
    for (const ChannelControl& control : {rControl, gControl, bControl})
    {
        SetupLabelDragFloat(
            control.Caption, control.Field,
            onRgbChanging, onRgbCommitted,
            /*defaultValue=*/0.0f, /*minValue=*/-1.0f, /*maxValue=*/1.0f);
    }

    // Y is the numeric view of the same master-lightness value as the slider.
    sync->Y->SetOnValueChanging([sync, sliderRaw, lightPreview](const float& raw)
    {
        if (sync->Syncing)
            return;
        const float value = std::clamp(raw, -1.0f, 1.0f);
        sliderRaw->SetValueWithoutNotify(value);
        lightPreview(value);
    });
    sync->Y->SetOnValueChanged([sync, sliderRaw, lightCommit](const float& raw)
    {
        if (sync->Syncing)
            return;
        const float value = std::clamp(raw, -1.0f, 1.0f);
        sync->Y->SetValueWithoutNotify(value);
        sliderRaw->SetValueWithoutNotify(value);
        lightCommit(value);
    });
    SetupLabelDragFloat(
        yControl.Caption, yControl.Field,
        [sync, sliderRaw, lightPreview]()
        {
            const float value = std::clamp(sync->Y->GetValue(), -1.0f, 1.0f);
            sliderRaw->SetValueWithoutNotify(value);
            lightPreview(value);
        },
        [sync, sliderRaw, lightCommit]()
        {
            const float value = std::clamp(sync->Y->GetValue(), -1.0f, 1.0f);
            sync->Y->SetValueWithoutNotify(value);
            sliderRaw->SetValueWithoutNotify(value);
            lightCommit(value);
        },
        /*defaultValue=*/0.0f, /*minValue=*/-1.0f, /*maxValue=*/1.0f);

    // Hex field edit: same lifecycle, driving the puck + RGB fields live.
    sync->Hex->SetOnValueChanging([sync, colorPreview, pushPuck, pushRgb](const std::string& text)
    {
        if (sync->Syncing)
            return;
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (!HexToTint(text, r, g, b))
            return;
        const Offset3 o = TintToOffset(r, g, b);
        colorPreview(o);
        pushPuck(o);
        pushRgb(o);
    });
    sync->Hex->SetOnValueChanged([sync, colorCommit, syncAll, offsetFromFields](const std::string& text)
    {
        if (sync->Syncing)
            return;
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (!HexToTint(text, r, g, b))
        {
            // Invalid hex: revert the field to the current color.
            syncAll(offsetFromFields());
            return;
        }
        const Offset3 o = TintToOffset(r, g, b);
        colorCommit(o);
        syncAll(o);
    });

    // The band title owns the color reset, while the Lightness label above the
    // slider owns the independent master-lightness reset.
    titleLabel->SetTooltip("Double-click to reset this tonal band's color.");
    auto lastTitleClick = std::make_shared<std::chrono::steady_clock::time_point>();
    titleLabel->RegisterEventHandler(
        kEventMouseUp,
        [lastTitleClick, colorCommit, syncAll](UIEvent& event)
        {
            if (event.Button != 0)
                return;

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - *lastTitleClick;
            if (lastTitleClick->time_since_epoch().count() != 0 &&
                elapsed < Platform::GetDoubleClickInterval())
            {
                const Offset3 neutral{};
                colorCommit(neutral);
                syncAll(neutral);
                *lastTitleClick = {};
                event.Stop();
                return;
            }
            *lastTitleClick = now;
        });
}
} // namespace

void RegisterColorGradeEffectInspector()
{
    using namespace InspectorDrag;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<G>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        // Three trackball columns side by side (Unity SMH / Unreal grading panel).
        // flex-wrap lets the row drop columns to fewer per line (3 -> 2 -> 1) when
        // the inspector is too narrow, rather than clipping horizontally.
        auto bandRow = std::make_unique<UIElement>();
        bandRow->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::FlexWrap, true)
            .Set(Style::AlignItems, AlignItems::FlexStart)
            .Set(Style::Gap, StyleLength::Px(kBandColumnGapPx))
            .Set(Style::MarginBottom, StyleLength::Px(kBandSpacingPx));
        UIElement* bandRowRaw = bandRow.get();
        ctx.Parent->AddChild(std::move(bandRow));

        BuildColorGradeBand(bandRowRaw, "Shadows", effect->ShadowsColor, effect->ShadowsLightness,
            [](G& u, float r, float g, float b) { u.ShadowsColor[0] = r; u.ShadowsColor[1] = g; u.ShadowsColor[2] = b; },
            [](G& u, float v) { u.ShadowsLightness = v; },
            w, e, n, undo, "Change Shadows Color", "Change Shadows Lightness");
        BuildColorGradeBand(bandRowRaw, "Midtones", effect->MidtonesColor, effect->MidtonesLightness,
            [](G& u, float r, float g, float b) { u.MidtonesColor[0] = r; u.MidtonesColor[1] = g; u.MidtonesColor[2] = b; },
            [](G& u, float v) { u.MidtonesLightness = v; },
            w, e, n, undo, "Change Midtones Color", "Change Midtones Lightness");
        BuildColorGradeBand(bandRowRaw, "Highlights", effect->HighlightsColor, effect->HighlightsLightness,
            [](G& u, float r, float g, float b) { u.HighlightsColor[0] = r; u.HighlightsColor[1] = g; u.HighlightsColor[2] = b; },
            [](G& u, float v) { u.HighlightsLightness = v; },
            w, e, n, undo, "Change Highlights Color", "Change Highlights Lightness");

        // Global adjustments.
        AddComponentFloatRowWithDrag<G>(ctx.Parent, "Contrast", effect->Contrast,
            w, e, n, undo, "Change Color Grade Contrast",
            [](G& u, float v) { u.Contrast = std::max(v, 0.0f); },
            1.0f, "Global contrast about mid-grey in log space (1 = no change).", {}, 0.0f, 4.0f);
        AddComponentFloatRowWithDrag<G>(ctx.Parent, "Saturation", effect->Saturation,
            w, e, n, undo, "Change Color Grade Saturation",
            [](G& u, float v) { u.Saturation = std::max(v, 0.0f); },
            1.0f, "Global saturation (1 = source, 0 = grayscale).", {}, 0.0f, 4.0f);

        // Zero-centered global sliders (URP ColorAdjustments/WhiteBalance model):
        // hue shift closes the grade chain; temperature/tint resolve to von Kries
        // LMS gains applied before the corrector.
        auto addCenteredSlider = [&](const char* label, float initial, float range,
                                     const char* editName, auto apply, const char* tooltip)
        {
            auto handlers = InspectorDrag::MakeComponentInteractiveHandlers<G, float>(
                w, e, n, undo, editName, std::move(apply), {});
            const SliderWithFloatValueRow row = AddFloatSliderRow(ctx.Parent, label, initial, -range, range,
                                                                  std::move(handlers.first),
                                                                  std::move(handlers.second), tooltip);
            row.Slider->SetCentered(true);
        };
        addCenteredSlider("Hue Shift", effect->HueShift, 180.0f, "Change Color Grade Hue Shift",
            [](G& u, float v) { u.HueShift = std::clamp(v, -180.0f, 180.0f); },
            "Hue rotation in degrees, applied after the corrector (0 = no change).");

        Label* wbTitle = AddCaption(ctx.Parent, "White Balance", kTitleFontSizePx);
        wbTitle->Overrides().Set(Style::MarginTop, StyleLength::Px(kBandGapPx));
        addCenteredSlider("Temperature", effect->Temperature, 100.0f, "Change White Balance Temperature",
            [](G& u, float v) { u.Temperature = std::clamp(v, -100.0f, 100.0f); },
            "Warms (+) or cools (-) the image toward the compensated white point.");
        addCenteredSlider("Tint", effect->Tint, 100.0f, "Change White Balance Tint",
            [](G& u, float v) { u.Tint = std::clamp(v, -100.0f, 100.0f); },
            "Compensates green (-) / magenta (+) along the axis orthogonal to temperature.");

        // Band ranges: where each tonal band lives on the encoded-log axis
        // (mid-grey ~0.41). Start <= end is enforced at write time by clamping
        // the edited limit against its band sibling, so the shader never sees a
        // reversed pair from the UI.
        Label* rangesTitle = AddCaption(ctx.Parent, "Band Ranges", kTitleFontSizePx);
        rangesTitle->Overrides().Set(Style::MarginTop, StyleLength::Px(kBandGapPx));
        auto addBandLimitSlider = [&](const char* label, float initial, const char* editName,
                                      auto apply, const char* tooltip)
        {
            auto handlers = InspectorDrag::MakeComponentInteractiveHandlers<G, float>(
                w, e, n, undo, editName, std::move(apply), {});
            AddFloatSliderRow(ctx.Parent, label, initial, 0.0f, 1.0f,
                              std::move(handlers.first), std::move(handlers.second), tooltip);
        };
        addBandLimitSlider("Shadows Start", effect->ShadowsStart, "Change Shadows Start",
            [](G& u, float v) { u.ShadowsStart = std::min(ClampUnit(v), u.ShadowsEnd); },
            "Encoded-log luminance where shadow weight starts fading (full weight below).");
        addBandLimitSlider("Shadows End", effect->ShadowsEnd, "Change Shadows End",
            [](G& u, float v) { u.ShadowsEnd = std::max(ClampUnit(v), u.ShadowsStart); },
            "Encoded-log luminance where shadow weight reaches zero (midtones above).");
        addBandLimitSlider("Highlights Start", effect->HighlightsStart, "Change Highlights Start",
            [](G& u, float v) { u.HighlightsStart = std::min(ClampUnit(v), u.HighlightsEnd); },
            "Encoded-log luminance where highlight weight starts rising (midtones below).");
        addBandLimitSlider("Highlights End", effect->HighlightsEnd, "Change Highlights End",
            [](G& u, float v) { u.HighlightsEnd = std::max(ClampUnit(v), u.HighlightsStart); },
            "Encoded-log luminance where highlight weight reaches full strength.");
    };

    InspectorRegistry::Get().RegisterComponentInspector<G>(std::move(fn));
}

} // namespace GameEngine
