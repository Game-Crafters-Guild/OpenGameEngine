#include "Inspectors/FilmSimulationEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/FilmSimulationEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "UI/Controls/Foldout.h"
#include "UI/StyleProperties.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
using Film = Components::FilmSimulationEffect;

constexpr float kMaxHalationTintIntensity = 4.0f;
constexpr float kSwatchSize = 20.0f;
constexpr uint32_t kSwatchBorderColor = 0xFF555555u;

void TintToPickerState(const float* tint, uint32_t& outArgb, float& outIntensity)
{
    const float maxChannel = std::max(std::max(tint[0], tint[1]), tint[2]);
    outIntensity = std::clamp(maxChannel, 1.0f, kMaxHalationTintIntensity);
    const float inverseIntensity = 1.0f / outIntensity;
    const auto quantize = [inverseIntensity](float value)
    {
        return static_cast<uint8_t>(std::clamp(value * inverseIntensity, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    outArgb = 0xFF000000u |
              (static_cast<uint32_t>(quantize(tint[0])) << 16) |
              (static_cast<uint32_t>(quantize(tint[1])) << 8) |
              static_cast<uint32_t>(quantize(tint[2]));
}

void PickerStateToTint(uint32_t argb, float intensity, float* outTint)
{
    const float resolvedIntensity = std::clamp(intensity, 1.0f, kMaxHalationTintIntensity);
    outTint[0] = (static_cast<float>((argb >> 16) & 0xFFu) / 255.0f) * resolvedIntensity;
    outTint[1] = (static_cast<float>((argb >> 8) & 0xFFu) / 255.0f) * resolvedIntensity;
    outTint[2] = (static_cast<float>(argb & 0xFFu) / 255.0f) * resolvedIntensity;
}

std::string FormatTint(const float* tint)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f)", tint[0], tint[1], tint[2]);
    return buffer;
}

void StyleTintSwatch(UIElement* swatch, uint32_t argb)
{
    if (!swatch)
        return;
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(kSwatchSize))
        .Set(Style::Height, StyleLength::Px(kSwatchSize))
        .Set(Style::MinWidth, StyleLength::Px(kSwatchSize))
        .Set(Style::MinHeight, StyleLength::Px(kSwatchSize))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{3.0f, 3.0f, 3.0f, 3.0f})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{
                                     kSwatchBorderColor, kSwatchBorderColor, kSwatchBorderColor, kSwatchBorderColor})
        .Set(Style::BackgroundColor, argb)
        .Set(Style::Cursor, CursorStyle::Pointer);
}

void AddHalationTintRow(const InspectorContext& ctx, UIElement* parent, const Film* effect)
{
    ECS::World* world = ctx.World;
    const ECS::EntityHandle entity = ctx.Entity;
    Editor::EditorChangeNotifications* notifications = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;

    UIElement* row = InspectorUI::AddRow(parent);
    Label* tintLabel = InspectorUI::AddLabel(row, "Halation Tint", "HDR color of the film-base scatter");
    if (tintLabel)
        tintLabel->AddClass("inspector-label-no-drag");
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));

    uint32_t argb = 0;
    float intensity = 1.0f;
    TintToPickerState(effect->HalationTint, argb, intensity);

    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    StyleTintSwatch(swatchRaw, argb);
    fieldContainer->AddChild(std::move(swatch));

    auto valueLabel = std::make_unique<Label>();
    valueLabel->AddClass("inspector-text");
    valueLabel->SetText(FormatTint(effect->HalationTint));
    valueLabel->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
    Label* valueLabelRaw = valueLabel.get();
    fieldContainer->AddChild(std::move(valueLabel));

    // The picker's callbacks outlive an inspector rebuild: resolve the row through weak
    // refs instead of holding the freed widgets.
    auto updateUI = [world, entity, swatchRef = UIElement::MakeWeakRef(swatchRaw),
                     valueLabelRef = UIElement::MakeWeakRef(valueLabelRaw)]()
    {
        const auto* film = world->GetComponent<Film>(entity);
        UIElement* swatch = swatchRef.Get();
        Label* valueLabel = valueLabelRef.Get();
        if (!film || !swatch || !valueLabel)
            return;
        uint32_t nextArgb = 0;
        float nextIntensity = 1.0f;
        TintToPickerState(film->HalationTint, nextArgb, nextIntensity);
        StyleTintSwatch(swatch, nextArgb);
        valueLabel->SetText(FormatTint(film->HalationTint));
    };

    auto clickHandler = [world, entity, notifications, undo, openPicker, updateUI](UIEvent& event)
    {
        if (event.Button != 0)
            return;
        event.Stop();
        if (!openPicker)
            return;
        const auto* current = world->GetComponent<Film>(entity);
        if (!current)
            return;
        const Film original = *current;
        uint32_t initialArgb = 0;
        float initialIntensity = 1.0f;
        TintToPickerState(current->HalationTint, initialArgb, initialIntensity);

        using Edit = Editor::UndoRedoService::InteractiveEdit;
        auto edit = std::make_shared<Edit>();
        if (undo)
        {
            auto target = InspectorDrag::MakeComponentSnapshotTarget<Film>(world, entity, notifications, "Halation Tint");
            *edit = undo->BeginInteractiveEdit("Change Film Simulation Halation Tint", std::move(target));
        }

        auto previewTint = [world, entity, notifications, edit](uint32_t newArgb, float newIntensity)
        {
            float tint[3]{};
            PickerStateToTint(newArgb, newIntensity, tint);
            if (*edit)
            {
                edit->Preview([&]
                              {
                    auto* film = world->GetComponentForWrite<Film>(entity);
                    if (!film)
                        return;
                    std::copy_n(tint, 3, film->HalationTint); });
            }
            else if (const auto* film = world->GetComponent<Film>(entity))
            {
                Film updated = *film;
                std::copy_n(tint, 3, updated.HalationTint);
                Editor::PreviewComponentUpdate(world, entity, notifications, updated);
            }
        };

        ColorPickerCallbacks callbacks;
        callbacks.onValueChanging = [previewTint, updateUI](uint32_t color, float value)
        {
            previewTint(color, value);
            updateUI();
        };
        callbacks.onApply = [world, entity, notifications, edit, previewTint, updateUI](uint32_t color, float value)
        {
            previewTint(color, value);
            if (*edit)
                edit->Commit();
            else if (const auto* film = world->GetComponent<Film>(entity))
                Editor::CommitComponentUpdate(world, entity, notifications, *film);
            updateUI();
        };
        callbacks.onCancel = [world, entity, notifications, edit, original, updateUI]()
        {
            if (*edit)
                edit->Cancel();
            else
                Editor::PreviewComponentUpdate(world, entity, notifications, original);
            updateUI();
        };
        openPicker(initialArgb, initialIntensity, std::move(callbacks));
    };
    swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
    valueLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
}

// Section enable-dots share one ToggleDragPaintGroup so a single press-and-sweep
// paints several sections to the same state. The whole gesture is one
// interactive undo edit; each painted section previews into it and the edit
// commits when the gesture ends.
// State shared by the section dots' gesture callbacks. Owned by those
// callbacks (via the paint group the toggles keep alive); it must not own the
// paint group back, or the pair would leak on every inspector rebuild.
struct SectionDotState
{
    struct Section
    {
        std::string Tooltip;
        std::function<void(Film&, bool)> Set;
    };

    std::vector<Section> Sections;
    std::optional<Editor::UndoRedoService::InteractiveEdit> Edit;
};

struct SectionDotGroup
{
    std::shared_ptr<SectionDotState> State;
    std::shared_ptr<InspectorDrag::ToggleDragPaintGroup> Paint;
};

SectionDotGroup MakeSectionDotGroup(const InspectorContext& ctx)
{
    auto state = std::make_shared<SectionDotState>();
    ECS::World* world = ctx.World;
    const ECS::EntityHandle entity = ctx.Entity;
    Editor::EditorChangeNotifications* notifications = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;

    auto paint = std::make_shared<InspectorDrag::ToggleDragPaintGroup>(
        [state, world, entity, notifications, undo](bool)
        {
            if (!undo)
                return;
            auto target = InspectorDrag::MakeComponentSnapshotTarget<Film>(
                world, entity, notifications, "Film Simulation Sections");
            state->Edit.emplace(undo->BeginInteractiveEdit(
                "Toggle Film Simulation Sections", std::move(target)));
        },
        [state, world, entity, notifications](size_t id, bool value, ToggleBase& toggle)
        {
            const auto& section = state->Sections[id];
            if (state->Edit && *state->Edit)
            {
                state->Edit->Preview([&]
                                     {
                    auto* film = world->GetComponentForWrite<Film>(entity);
                    if (!film)
                        return;
                    section.Set(*film, value); });
                if (notifications)
                    notifications->NotifyComponentChange<Film>(
                        world, entity, Editor::EditorChangeNotifications::ChangeKind::Preview);
            }
            else if (const auto* film = world->GetComponent<Film>(entity))
            {
                Film updated = *film;
                section.Set(updated, value);
                Editor::CommitComponentUpdate(world, entity, notifications, updated);
            }
            toggle.SetTooltip(
                std::string(value ? "Enabled - click to disable. "
                                  : "Disabled - click to enable. ") +
                section.Tooltip);
        },
        [state, world, entity, notifications]()
        {
            if (!state->Edit || !*state->Edit)
                return;
            state->Edit->Commit();
            state->Edit.reset();
            if (notifications)
                notifications->NotifyComponentCommit<Film>(world, entity);
        });
    return {std::move(state), std::move(paint)};
}

template <typename Setter>
UIElement* AddFilmSection(const InspectorContext& ctx,
                          const SectionDotGroup& group,
                          const char* title,
                          bool enabled,
                          bool expanded,
                          const char* tooltip,
                          Setter setter)
{
    auto foldout = std::make_unique<Foldout>();
    foldout->SetTitle(title);
    foldout->AddClass("rp-foldout");
    foldout->SetExpanded(expanded);
    UIElement* content = foldout->GetContentContainer();

    if (UIElement* header = foldout->GetHeader())
    {
        header->Overrides()
            .Set(Style::PaddingTop, StyleLength::Px(2.0f))
            .Set(Style::PaddingRight, StyleLength::Px(6.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(2.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(6.0f))
            .Set(Style::Gap, StyleLength::Px(6.0f));

        const size_t id = group.State->Sections.size();
        group.State->Sections.push_back({tooltip, std::function<void(Film&, bool)>(setter)});

        auto dot = group.Paint->CreateToggle(id, enabled);
        dot->AddClass("inspector-foldout-enable-dot");
        dot->SetTooltip(
            std::string(enabled ? "Enabled - click to disable. "
                                : "Disabled - click to enable. ") +
            tooltip);

        auto visual = std::make_unique<UIElement>();
        visual->AddClass("inspector-foldout-enable-dot-visual");
        dot->AddChild(std::move(visual));

        header->InsertChild(0, std::move(dot));
    }

    ctx.Parent->AddChild(std::move(foldout));
    return content;
}
} // namespace

void RegisterFilmSimulationEffectInspector()
{
    using namespace InspectorDrag;
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;
        auto* c = ctx.World->GetComponent<Film>(ctx.Entity);
        if (!c)
            return;

        auto* w = ctx.World;
        const auto e = ctx.Entity;
        auto* n = ctx.ChangeNotifications;
        auto* undo = ctx.Undo;
        const auto toggleRow = [=](UIElement* parent,
                                   const char* label,
                                   bool value,
                                   const char* command,
                                   auto setter,
                                   const char* tooltip)
        {
            AddToggleRow(parent, label, value, [=](bool next)
                         { CommitComponentWithUndo<Film>(
                               w, e, n, undo, command,
                               [=](Film& film)
                               { setter(film, next); }); }, tooltip);
        };

        AddComponentFloatRowWithDrag<Film>(
            ctx.Parent, "Film Frame Rate", c->FilmFrameRate,
            w, e, n, undo, "Change Film Frame Rate",
            [](Film& film, float value)
            {
                film.FilmFrameRate = std::clamp(value, Film::kFrameRateMin, Film::kFrameRateMax);
            },
            24.0f, "Shared frame rate for grain, hairs, scratches, and gate weave",
            {}, 1.0f, 120.0f);

        auto dots = MakeSectionDotGroup(ctx);
        UIElement* halation = AddFilmSection(
            ctx, dots, "Halation", c->HalationEnabled, false,
            "Warm scatter around bright highlights in the film base.",
            [](Film& film, bool value)
            { film.HalationEnabled = value; });
        AddComponentFloatRowWithDrag<Film>(
            halation, "Intensity", c->HalationIntensity,
            w, e, n, undo, "Change Halation Intensity",
            [](Film& film, float value)
            {
                film.HalationIntensity = std::clamp(value, 0.0f, Film::kHalationIntensityMax);
            },
            2.0f, "Strength of warm highlight scatter", {}, 0.0f, 4.0f);
        AddComponentFloatRowWithDrag<Film>(
            halation, "Radius", c->HalationRadius,
            w, e, n, undo, "Change Halation Radius",
            [](Film& film, float value)
            {
                film.HalationRadius = std::clamp(value, 0.0f, Film::kHalationRadiusMax);
            },
            3.0f, "Halo radius in screen pixels", {}, 0.0f, 64.0f);
        AddHalationTintRow(ctx, halation, c);

        UIElement* grain = AddFilmSection(
            ctx, dots, "Grain", c->GrainEnabled, false,
            "Photochemical grain after tone mapping.",
            [](Film& film, bool value)
            { film.GrainEnabled = value; });
        static const std::vector<Dropdown::Option> grainModeOptions = {
            {"0", "FidelityFX Fast"},
            {"1", "Filmic"},
        };
        auto* grainMode = InspectorUI::AddDropdownRow(
            grain, "Mode", grainModeOptions, static_cast<int>(c->GrainMode),
            "FidelityFX Fast uses AMD's lightweight procedural grain. Filmic uses "
            "discrete overlapping grain particles with density and shape controls.");
        AddComponentFloatRowWithDrag<Film>(
            grain, "Intensity", c->GrainIntensity,
            w, e, n, undo, "Change Grain Intensity",
            [](Film& film, float value)
            {
                film.GrainIntensity = std::clamp(value, 0.0f, Film::kGrainIntensityMax);
            },
            1.0f, "Overall grain amount", {}, 0.0f, 4.0f);
        AddComponentFloatRowWithDrag<Film>(
            grain, "Size", c->GrainSize,
            w, e, n, undo, "Change Grain Size",
            [](Film& film, float value)
            {
                film.GrainSize = std::clamp(value, Film::kGrainSizeMin, Film::kGrainSizeMax);
            },
            3.0f,
            c->GrainMode == Components::FilmGrainMode::FidelityFXFast
                ? "Approximate FidelityFX grain scale in screen pixels"
                : "Diameter of each grain in screen pixels",
            {}, 0.25f, 8.0f);

        const bool filmicMode = c->GrainMode == Components::FilmGrainMode::Filmic;
        auto filmicShapeControls = std::make_unique<UIElement>();
        UIElement* filmicShapeControlsRaw = filmicShapeControls.get();
        filmicShapeControlsRaw->Overrides()
            .Set(Style::Display, filmicMode ? DisplayMode::Flex : DisplayMode::None)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::AlignItems, AlignItems::Stretch);
        toggleRow(
            filmicShapeControlsRaw, "Smooth", c->GrainSmooth, "Change Grain Filter Mode",
            [](Film& film, bool value)
            { film.GrainSmooth = value; },
            "Smooth individual grains; off keeps crisp pixel grain.");
        AddComponentFloatRowWithDrag<Film>(
            filmicShapeControlsRaw, "Density", c->GrainDensity,
            w, e, n, undo, "Change Grain Density",
            [](Film& film, float value)
            {
                film.GrainDensity = std::clamp(value, 0.0f, 1.0f);
            },
            1.0f, "Fraction of grain cells that are active", {}, 0.0f, 1.0f);
        grain->AddChild(std::move(filmicShapeControls));

        AddComponentFloatRowWithDrag<Film>(
            grain, "Shadows", c->GrainShadowResponse,
            w, e, n, undo, "Change Grain Shadows",
            [](Film& film, float value)
            {
                film.GrainShadowResponse = std::clamp(value, 0.0f, Film::kGrainResponseMax);
            },
            1.0f, "Grain response in shadows", {}, 0.0f, 2.0f);
        AddComponentFloatRowWithDrag<Film>(
            grain, "Midtones", c->GrainMidtoneResponse,
            w, e, n, undo, "Change Grain Midtones",
            [](Film& film, float value)
            {
                film.GrainMidtoneResponse = std::clamp(value, 0.0f, Film::kGrainResponseMax);
            },
            1.0f, "Grain response in midtones", {}, 0.0f, 2.0f);
        AddComponentFloatRowWithDrag<Film>(
            grain, "Highlights", c->GrainHighlightResponse,
            w, e, n, undo, "Change Grain Highlights",
            [](Film& film, float value)
            {
                film.GrainHighlightResponse = std::clamp(value, 0.0f, Film::kGrainResponseMax);
            },
            0.35f, "Grain response in highlights", {}, 0.0f, 2.0f);

        auto filmicColorControls = std::make_unique<UIElement>();
        UIElement* filmicColorControlsRaw = filmicColorControls.get();
        filmicColorControlsRaw->Overrides()
            .Set(Style::Display, filmicMode ? DisplayMode::Flex : DisplayMode::None)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::AlignItems, AlignItems::Stretch);
        toggleRow(
            filmicColorControlsRaw, "Colored", c->GrainColored, "Change Grain Color Mode",
            [](Film& film, bool value)
            { film.GrainColored = value; },
            "Use independent RGB grain instead of monochrome grain.");
        grain->AddChild(std::move(filmicColorControls));

        grainMode->SetOnValueChanged(
            [=](const std::string& value)
            {
                int mode = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), mode);
                if (parsed.ec != std::errc{})
                    return;
                mode = std::clamp(mode, 0, static_cast<int>(Film::kGrainModeLast));
                const bool showFilmicControls =
                    mode == static_cast<int>(Components::FilmGrainMode::Filmic);
                filmicShapeControlsRaw->Overrides().Set(
                    Style::Display, showFilmicControls ? DisplayMode::Flex : DisplayMode::None);
                filmicColorControlsRaw->Overrides().Set(
                    Style::Display, showFilmicControls ? DisplayMode::Flex : DisplayMode::None);
                CommitComponentWithUndo<Film>(
                    w, e, n, undo, "Change Film Grain Mode",
                    [mode](Film& film)
                    {
                        film.GrainMode = static_cast<Components::FilmGrainMode>(mode);
                    });
            });

        UIElement* hair = AddFilmSection(
            ctx, dots, "Hair", c->HairEnabled, false,
            "Generate random dark fibers caught in the film gate.",
            [](Film& film, bool value)
            { film.HairEnabled = value; });
        AddComponentFloatRowWithDrag<Film>(
            hair, "Amount", c->HairAmount,
            w, e, n, undo, "Change Hair Amount",
            [](Film& film, float value)
            {
                film.HairAmount = std::clamp(value, 0.0f, Film::kHairAmountMax);
            },
            0.082891f, "Average number of visible fibers", {}, 0.0f, Film::kHairAmountMax);
        AddComponentFloatRowWithDrag<Film>(
            hair, "Intensity", c->HairIntensity,
            w, e, n, undo, "Change Hair Intensity",
            [](Film& film, float value)
            {
                film.HairIntensity = std::clamp(value, 0.0f, Film::kArtifactIntensityMax);
            },
            1.2f, "Fiber opacity", {}, 0.0f, Film::kArtifactIntensityMax);
        AddComponentFloatRowWithDrag<Film>(
            hair, "Width", c->HairWidth,
            w, e, n, undo, "Change Hair Width",
            [](Film& film, float value)
            {
                film.HairWidth = std::clamp(value, Film::kHairWidthMin, Film::kHairWidthMax);
            },
            1.25f, "Fiber width in pixels", {}, 0.25f, 8.0f);
        AddComponentFloatRowWithDrag<Film>(
            hair, "Length", c->HairLength,
            w, e, n, undo, "Change Hair Length",
            [](Film& film, float value)
            {
                film.HairLength = std::clamp(value, Film::kHairLengthMin, Film::kHairLengthMax);
            },
            220.0f, "Fiber length in pixels", {}, 8.0f, 1024.0f);
        AddComponentFloatRowWithDrag<Film>(
            hair, "Random Size", c->HairRandomSize,
            w, e, n, undo, "Change Hair Random Size",
            [](Film& film, float value)
            {
                film.HairRandomSize = std::clamp(value, 0.0f, 1.0f);
            },
            1.0f, "Random variation in each fiber's length and width", {}, 0.0f, 1.0f);
        AddComponentFloatRowWithDrag<Film>(
            hair, "Curl", c->HairCurl,
            w, e, n, undo, "Change Hair Curl",
            [](Film& film, float value)
            {
                film.HairCurl = std::clamp(value, -1.0f, 1.0f);
            },
            0.25f, "Signed curvature of generated fibers", {}, -1.0f, 1.0f);
        AddComponentFloatRowWithDrag<Film>(
            hair, "Curl Randomness", c->HairCurlRandomness,
            w, e, n, undo, "Change Hair Curl Randomness",
            [](Film& film, float value)
            {
                film.HairCurlRandomness = std::clamp(value, 0.0f, 1.0f);
            },
            1.0f, "Random variation in each fiber's curl strength", {}, 0.0f, 1.0f);

        UIElement* scratches = AddFilmSection(
            ctx, dots, "Scratches", c->ScratchesEnabled, false,
            "Generate mostly vertical emulsion scratches.",
            [](Film& film, bool value)
            { film.ScratchesEnabled = value; });
        AddComponentFloatRowWithDrag<Film>(
            scratches, "Amount", c->ScratchAmount,
            w, e, n, undo, "Change Scratch Amount",
            [](Film& film, float value)
            {
                film.ScratchAmount = std::clamp(value, 0.0f, Film::kScratchAmountMax);
            },
            0.262656f, "Average number of visible scratches", {}, 0.0f, Film::kScratchAmountMax);
        AddComponentFloatRowWithDrag<Film>(
            scratches, "Intensity", c->ScratchIntensity,
            w, e, n, undo, "Change Scratch Intensity",
            [](Film& film, float value)
            {
                film.ScratchIntensity = std::clamp(value, 0.0f, Film::kArtifactIntensityMax);
            },
            0.327031f, "Scratch opacity", {}, 0.0f, Film::kArtifactIntensityMax);
        AddComponentFloatRowWithDrag<Film>(
            scratches, "Width", c->ScratchWidth,
            w, e, n, undo, "Change Scratch Width",
            [](Film& film, float value)
            {
                film.ScratchWidth = std::clamp(value, Film::kScratchWidthMin, Film::kScratchWidthMax);
            },
            0.8f, "Scratch width in pixels", {}, 0.25f, 8.0f);
        AddComponentFloatRowWithDrag<Film>(
            scratches, "Length", c->ScratchLength,
            w, e, n, undo, "Change Scratch Length",
            [](Film& film, float value)
            {
                film.ScratchLength = std::clamp(value, Film::kScratchLengthMin, Film::kScratchLengthMax);
            },
            0.65f, "Scratch length as a fraction of frame height", {}, 0.05f, 1.0f);

        UIElement* dust = AddFilmSection(
            ctx, dots, "Dust", c->DustEnabled, false,
            "Random dust specks settled in the film gate.",
            [](Film& film, bool value)
            { film.DustEnabled = value; });
        AddComponentFloatRowWithDrag<Film>(
            dust, "Amount", c->DustAmount,
            w, e, n, undo, "Change Dust Amount",
            [](Film& film, float value)
            {
                film.DustAmount = std::clamp(value, 0.0f, Film::kDustAmountMax);
            },
            2.5f, "Average number of visible dust specks", {}, 0.0f, Film::kDustAmountMax);
        AddComponentFloatRowWithDrag<Film>(
            dust, "Intensity", c->DustIntensity,
            w, e, n, undo, "Change Dust Intensity",
            [](Film& film, float value)
            {
                film.DustIntensity = std::clamp(value, 0.0f, Film::kArtifactIntensityMax);
            },
            1.0f, "Dust speck opacity", {}, 0.0f, Film::kArtifactIntensityMax);
        AddComponentFloatRowWithDrag<Film>(
            dust, "Size", c->DustSize,
            w, e, n, undo, "Change Dust Size",
            [](Film& film, float value)
            {
                film.DustSize = std::clamp(value, Film::kDustSizeMin, Film::kDustSizeMax);
            },
            3.24375f, "Speck radius in pixels", {}, Film::kDustSizeMin, Film::kDustSizeMax);
        AddComponentFloatRowWithDrag<Film>(
            dust, "Size Randomness", c->DustRandomSize,
            w, e, n, undo, "Change Dust Size Randomness",
            [](Film& film, float value)
            {
                film.DustRandomSize = std::clamp(value, 0.0f, 1.0f);
            },
            1.0f, "Random variation in each speck's size", {}, 0.0f, 1.0f);

        UIElement* gateWeave = AddFilmSection(
            ctx, dots, "Gate Weave", c->GateWeaveEnabled, false,
            "Mechanical frame registration movement.",
            [](Film& film, bool value)
            { film.GateWeaveEnabled = value; });
        AddComponentFloatRowWithDrag<Film>(
            gateWeave, "Horizontal", c->GateWeaveHorizontal,
            w, e, n, undo, "Change Horizontal Gate Weave",
            [](Film& film, float value)
            {
                film.GateWeaveHorizontal = std::clamp(value, 0.0f, Film::kGateWeaveOffsetMax);
            },
            0.75f, "Horizontal movement in pixels", {}, 0.0f, 32.0f);
        AddComponentFloatRowWithDrag<Film>(
            gateWeave, "Vertical", c->GateWeaveVertical,
            w, e, n, undo, "Change Vertical Gate Weave",
            [](Film& film, float value)
            {
                film.GateWeaveVertical = std::clamp(value, 0.0f, Film::kGateWeaveOffsetMax);
            },
            0.5f, "Vertical movement in pixels", {}, 0.0f, 32.0f);
        AddComponentFloatRowWithDrag<Film>(
            gateWeave, "Rotation", c->GateWeaveRotation,
            w, e, n, undo, "Change Gate Weave Rotation",
            [](Film& film, float value)
            {
                film.GateWeaveRotation = std::clamp(value, 0.0f, Film::kGateWeaveRotationMax);
            },
            0.08f, "Rotational movement in degrees", {}, 0.0f, 2.0f);

        UIElement* gateMask = AddFilmSection(
            ctx, dots, "Gate Mask", c->GateMask != Components::FilmGateMask::None, false,
            "Procedural film gate or projection aperture mask.",
            [](Film& film, bool value)
            {
                if (!value)
                {
                    film.GateMask = Components::FilmGateMask::None;
                }
                else if (film.GateMask == Components::FilmGateMask::None)
                {
                    film.GateMask = Components::FilmGateMask::RoundedGate;
                }
            });

        static const std::vector<Dropdown::Option> maskOptions = {
            {"0", "None"},
            {"1", "Rounded Gate"},
            {"2", "Academy 1.37"},
            {"3", "Widescreen 1.85"},
            {"4", "Anamorphic 2.39"},
        };
        auto* mask = InspectorUI::AddDropdownRow(
            gateMask, "Mask", maskOptions, static_cast<int>(c->GateMask),
            "Procedural film gate or projection aperture mask");
        mask->SetOnValueChanged([=](const std::string& value)
                                {
            int mode = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), mode);
            if (parsed.ec != std::errc{})
                return;
            CommitComponentWithUndo<Film>(
                w, e, n, undo, "Change Film Gate Mask",
                [mode](Film& film)
                {
                    film.GateMask = static_cast<Components::FilmGateMask>(
                        std::clamp(mode, 0, static_cast<int>(Film::kGateMaskLast)));
                }); });
        AddComponentFloatRowWithDrag<Film>(
            gateMask, "Roundness", c->GateMaskRoundness,
            w, e, n, undo, "Change Gate Mask Roundness",
            [](Film& film, float value)
            {
                film.GateMaskRoundness = std::clamp(value, 0.0f, Film::kGateMaskRoundnessMax);
            },
            0.025f, "Corner radius as a fraction of the gate's half extent", {}, 0.0f,
            Film::kGateMaskRoundnessMax);
        AddComponentFloatRowWithDrag<Film>(
            gateMask, "Feather", c->GateMaskFeather,
            w, e, n, undo, "Change Gate Mask Feather",
            [](Film& film, float value)
            {
                film.GateMaskFeather = std::clamp(value, 0.0f, Film::kGateMaskFeatherMax);
            },
            3.0f, "Gate edge softness in pixels", {}, 0.0f, Film::kGateMaskFeatherMax);
    };
    InspectorRegistry::Get().RegisterComponentInspector<Film>(std::move(fn));
}
} // namespace GameEngine
