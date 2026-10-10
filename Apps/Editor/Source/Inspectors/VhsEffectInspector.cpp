#include "Inspectors/VhsEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/VhsEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TextField.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{
uint32_t ColorToArgb(const float (&color)[3])
{
    const auto channel = [](float value)
    {
        return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return 0xFF000000u
         | (static_cast<uint32_t>(channel(color[0])) << 16)
         | (static_cast<uint32_t>(channel(color[1])) << 8)
         | static_cast<uint32_t>(channel(color[2]));
}

void ArgbToColor(uint32_t argb, float (&color)[3])
{
    color[0] = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    color[1] = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    color[2] = static_cast<float>(argb & 0xFFu) / 255.0f;
}

std::string FormatRgb(const float (&color)[3])
{
    char text[48];
    std::snprintf(text, sizeof(text), "(%.2f, %.2f, %.2f)",
                  color[0], color[1], color[2]);
    return text;
}

} // namespace

void RegisterVhsEffectInspector()
{
    using namespace InspectorDrag;
    using VHS = Components::VhsEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        const auto* effect = ctx.World->GetComponent<VHS>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* world = ctx.World;
        const ECS::EntityHandle entity = ctx.Entity;
        Editor::EditorChangeNotifications* notifications = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;

        const auto addStrength = [=](const char* label, float value, float VHS::* member,
                                     const char* undoName, const char* tooltip,
                                     float resetValue = 1.0f)
        {
            return AddComponentFloatRowWithDrag<VHS>(
                ctx.Parent, label, value, world, entity, notifications, undo, undoName,
                [member](VHS& updated, float v)
                {
                    updated.*member = std::clamp(v, 0.0f, VHS::kStrengthMax);
                    if (member == &VHS::Wobble
                        || member == &VHS::Tracking
                        || member == &VHS::SignalGlitches
                        || member == &VHS::GlitchOffsets)
                    {
                        updated.TimeBasePreset = 0;
                    }
                },
                resetValue, tooltip, {}, 0.0f, VHS::kStrengthMax);
        };

        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Intensity", effect->Intensity, world, entity, notifications, undo,
            "Change VHS Intensity",
            [](VHS& updated, float v)
            {
                updated.Intensity = std::clamp(v, 0.0f, VHS::kIntensityMax);
            },
            1.0f, "Master blend between the untouched scene and the VHS image.",
            {}, 0.0f, VHS::kIntensityMax);

        static const std::vector<Dropdown::Option> kTimeBasePresets = {
            {"0", "Custom"},
            {"1", "Clean Deck"},
            {"2", "Consumer VHS"},
            {"3", "Worn Tape"},
            {"4", "Bad Tracking"},
            {"5", "Damaged Transport"},
        };
        struct TimeBasePresetFields
        {
            FloatField* Wobble = nullptr;
            FloatField* Tracking = nullptr;
            FloatField* SignalGlitches = nullptr;
            FloatField* GlitchOffsets = nullptr;
        };
        auto presetFields = std::make_shared<TimeBasePresetFields>();
        auto* timeBasePreset = InspectorUI::AddDropdownRow(
            ctx.Parent, "Time-Base Preset", kTimeBasePresets,
            static_cast<int>(std::min(
                effect->TimeBasePreset, VHS::kTimeBasePresetMax)),
            "Preset combinations of wobble, tracking, and localized signal glitches.");
        timeBasePreset->SetOnValueChanged(
            [world, entity, notifications, undo, presetFields](
                const std::string& value)
            {
                const uint32 selected = static_cast<uint32>(
                    std::clamp(std::atoi(value.c_str()), 0, 5));
                CommitComponentWithUndo<VHS>(
                    world, entity, notifications, undo,
                    "Change VHS Time-Base Preset",
                    [selected](VHS& updated)
                    {
                        updated.TimeBasePreset = selected;
                        if (selected == 1)
                        {
                            updated.Wobble = 0.08f;
                            updated.Tracking = 0.12f;
                            updated.SignalGlitches = 0.02f;
                            updated.GlitchOffsets = 0.01f;
                        }
                        else if (selected == 2)
                        {
                            updated.Wobble = 0.35f;
                            updated.Tracking = 0.42f;
                            updated.SignalGlitches = 0.12f;
                            updated.GlitchOffsets = 0.08f;
                        }
                        else if (selected == 3)
                        {
                            updated.Wobble = 0.65f;
                            updated.Tracking = 0.85f;
                            updated.SignalGlitches = 0.35f;
                            updated.GlitchOffsets = 0.28f;
                        }
                        else if (selected == 4)
                        {
                            updated.Wobble = 0.85f;
                            updated.Tracking = 1.45f;
                            updated.SignalGlitches = 0.65f;
                            updated.GlitchOffsets = 0.55f;
                        }
                        else if (selected == 5)
                        {
                            updated.Wobble = 1.25f;
                            updated.Tracking = 1.75f;
                            updated.SignalGlitches = 1.15f;
                            updated.GlitchOffsets = 1.05f;
                        }
                    });
                if (const auto* current = world->GetComponent<VHS>(entity))
                {
                    if (presetFields->Wobble)
                        presetFields->Wobble->SetValue(current->Wobble);
                    if (presetFields->Tracking)
                        presetFields->Tracking->SetValue(current->Tracking);
                    if (presetFields->SignalGlitches)
                        presetFields->SignalGlitches->SetValue(
                            current->SignalGlitches);
                    if (presetFields->GlitchOffsets)
                        presetFields->GlitchOffsets->SetValue(
                            current->GlitchOffsets);
                }
            });

        presetFields->Wobble =
            addStrength("Wobble", effect->Wobble, &VHS::Wobble,
                        "Change VHS Wobble", "Horizontal wow, flutter, and line jitter.",
                        0.45f);
        presetFields->Tracking =
            addStrength("Tracking", effect->Tracking, &VHS::Tracking,
                        "Change VHS Tracking", "Time-base drift, narrow line tears, and bottom-edge head switching.",
                        0.45f);
        presetFields->SignalGlitches =
            addStrength("Signal Glitches", effect->SignalGlitches,
                        &VHS::SignalGlitches, "Change VHS Signal Glitches",
                        "Intermittent analog line slips and localized tape distortion before chroma blur.",
                        0.15f);
        presetFields->GlitchOffsets =
            addStrength("Glitch Offsets", effect->GlitchOffsets,
                        &VHS::GlitchOffsets, "Change VHS Glitch Offsets",
                        "Independent moving horizontal slices that stretch and offset local parts of the picture.",
                        0.12f);
        addStrength("Interference / Line Noise", effect->Interference,
                    &VHS::Interference, "Change VHS Interference",
                    "Moving horizontal interference lines plus a slow analog hum bar.",
                    0.10f);
        addStrength("Frame Feedback", effect->FrameFeedback,
                    &VHS::FrameFeedback, "Change VHS Frame Feedback",
                    "Motion-aware previous-frame echo; chroma persists slightly longer than luma.",
                    0.16f);
        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Feedback Decay", effect->FeedbackDecay,
            world, entity, notifications, undo, "Change VHS Feedback Decay",
            [](VHS& updated, float value)
            {
                updated.FeedbackDecay = std::clamp(value, 0.0f, 0.98f);
            },
            0.70f, "Retention of the preceding frame inside detected motion.",
            {}, 0.0f, 0.98f);
        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Motion Threshold", effect->FeedbackMotionThreshold,
            world, entity, notifications, undo,
            "Change VHS Feedback Motion Threshold",
            [](VHS& updated, float value)
            {
                updated.FeedbackMotionThreshold =
                    std::clamp(value, 0.0f, 1.0f);
            },
            0.06f, "Minimum frame difference before feedback trails appear.",
            {}, 0.0f, 1.0f);
        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Trail Length", effect->FeedbackTrailLength,
            world, entity, notifications, undo,
            "Change VHS Feedback Trail Length",
            [](VHS& updated, float value)
            {
                updated.FeedbackTrailLength =
                    std::clamp(value, 0.0f, 24.0f);
            },
            3.0f, "Horizontal offset of the retained analog echo in pixels.",
            {}, 0.0f, 24.0f);

        static const std::vector<Dropdown::Option> kCompositeSignalModes = {
            {"0", "Off"},
            {"1", "NTSC 2-Phase"},
            {"2", "NTSC 3-Phase"},
            {"3", "Old / Soft 3-Phase"},
        };
        auto* compositeSignalMode = InspectorUI::AddDropdownRow(
            ctx.Parent, "Composite Signal", kCompositeSignalModes,
            static_cast<int>(std::min(
                effect->CompositeSignalMode, VHS::kCompositeSignalModeMax)),
            "Composite Y/C phase model used before the VHS chroma filter.");
        compositeSignalMode->SetOnValueChanged(
            [world, entity, notifications, undo](const std::string& value)
            {
                const uint32 selected = static_cast<uint32>(
                    std::clamp(std::atoi(value.c_str()), 0, 3));
                CommitComponentWithUndo<VHS>(
                    world, entity, notifications, undo,
                    "Change VHS Composite Signal Mode",
                    [selected](VHS& updated)
                    {
                        updated.CompositeSignalMode = selected;
                    });
            });
        addStrength("Dot Crawl", effect->DotCrawl, &VHS::DotCrawl,
                    "Change VHS Dot Crawl",
                    "Beaded composite interference along strong color transitions.",
                    0.20f);
        addStrength("Color Bleed", effect->ColorBleed, &VHS::ColorBleed,
                    "Change VHS Color Bleed", "Intensity and symmetric softness of the bandwidth-limited YIQ chroma.");

        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Bleed Shift", effect->ColorBleedOffset, world, entity, notifications, undo,
            "Change VHS Bleed Offset",
            [](VHS& updated, float v)
            {
                updated.ColorBleedOffset = std::clamp(v, 0.0f, VHS::kColorBleedOffsetMax);
            },
            4.0f, "Horizontal displacement of the softened chroma trail in output pixels.",
            {}, 0.0f, VHS::kColorBleedOffsetMax);

        addStrength("Tape Noise", effect->TapeNoise, &VHS::TapeNoise,
                    "Change VHS Tape Noise", "Luma and chroma tape grain.", 0.18f);
        addStrength("Chroma Streaks", effect->ChromaStreaks, &VHS::ChromaStreaks,
                    "Change VHS Chroma Streaks", "Occasional horizontal bands with lost chroma.", 0.08f);
        addStrength("Dropouts", effect->Dropouts, &VHS::Dropouts,
                    "Change VHS Dropouts", "Brief concealed lines plus short RF dots and dashes from oxide loss.", 0.05f);
        addStrength("Scanlines", effect->Scanlines, &VHS::Scanlines,
                    "Change VHS Scanlines",
                    "Optional soft 480-line display response with alternating-field phase weave.",
                    0.08f);

        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Speed", effect->Speed, world, entity, notifications, undo,
            "Change VHS Speed",
            [](VHS& updated, float v)
            {
                updated.Speed = std::clamp(v, 0.0f, VHS::kSpeedMax);
            },
            1.0f, "Animation speed multiplier.",
            {}, 0.0f, VHS::kSpeedMax);

        {
            UIElement* row = InspectorUI::AddRow(ctx.Parent);
            Label* label = InspectorUI::AddLabel(
                row, "Controls",
                "VCR transport shortcuts. Buttons also set the conventional OSD label.");
            if (label)
                label->AddClass("inspector-label-no-drag");
            UIElement* controls = InspectorUI::AddFieldContainer(row);
            controls->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(5.0f));

            const std::string activeText(effect->GetOverlayText());
            const auto addButton =
                [controls, world, entity, notifications, undo,
                 transportModeValue = effect->TransportMode, activeText](
                    const char* iconClass, const char* buttonText,
                    const char* tooltip, uint32_t mode, const char* overlayLabel)
            {
                auto button = std::make_unique<Button>();
                Button* raw = button.get();
                raw->AddClass("small");
                raw->AddClass("secondary");
                if (iconClass && iconClass[0] != '\0')
                {
                    raw->AddClass(iconClass);
                    const std::string icon(iconClass);
                    if (icon == "vhs-rewind-icon"
                        || icon == "vhs-fast-forward-icon")
                    {
                        raw->AddClass("vhs-transport-seek-button");
                        raw->Overrides()
                            .Set(Style::Width, StyleLength::Px(24.0f))
                            .Set(Style::MinWidth, StyleLength::Px(24.0f));
                    }
                    else
                    {
                        raw->AddClass("icon-button");
                    }
                    if (icon == "toolbar-record-icon")
                        raw->AddClass("vhs-record-red");
                }
                else
                {
                    raw->SetText(buttonText ? buttonText : "");
                    raw->AddClass("vhs-transport-seek-button");
                    raw->Overrides()
                        .Set(Style::Width, StyleLength::Px(32.0f))
                        .Set(Style::MinWidth, StyleLength::Px(32.0f));
                }
                raw->SetTooltip(tooltip);
                const bool active = mode > 0
                    ? transportModeValue == mode
                    : transportModeValue == 0 && activeText == overlayLabel;
                if (active)
                    raw->AddClass("active");
                raw->RegisterEventHandler(kEventButtonClick, [world, entity, notifications, undo, mode, overlayLabel](UIEvent&)
                    {
                        CommitComponentWithUndo<VHS>(
                            world, entity, notifications, undo,
                            "Change VHS Transport",
                            [mode, overlayLabel](VHS& updated)
                            {
                                updated.TransportMode = mode;
                                updated.SetOverlayText(overlayLabel);
                            });
                    });
                controls->AddChild(std::move(button));
            };

            addButton("stop-icon", "", "Stop", 0, "STOP");
            addButton("toolbar-record-icon", "", "Record", 0, "REC");
            addButton("pause-icon", "", "Pause", 0, "PAUSE");
            addButton("play-icon", "", "Play", 0, "PLAY");
            addButton("vhs-rewind-icon", "", "Rewind", 2, "REW");
            addButton("vhs-fast-forward-icon", "", "Fast Forward", 1, "FF");
        }

        addStrength("Transport Strength", effect->TransportStrength,
                    &VHS::TransportStrength, "Change VHS Transport Strength",
                    "Strength of fast-forward or rewind tracking bands and local line slips.",
                    0.65f);

        InspectorUI::AddTextBlock(
            ctx.Parent, "Transport Overlay", "inspector-section-subheader");

        AddToggleRow(ctx.Parent, "Transport Overlay", effect->OverlayEnabled,
            [world, entity, notifications, undo](bool value)
            {
                CommitComponentWithUndo<VHS>(
                    world, entity, notifications, undo, "Change VHS Transport Overlay",
                    [value](VHS& updated) { updated.OverlayEnabled = value; });
            },
            "Show the recorded transport status label and running counter.");

        const auto addDateBurnSection = [&]()
        {
            InspectorUI::AddTextBlock(
                ctx.Parent, "Date Burn", "inspector-section-subheader");

            AddToggleRow(ctx.Parent, "Date Burn", effect->DateBurnEnabled,
            [world, entity, notifications, undo](bool value)
            {
                CommitComponentWithUndo<VHS>(
                    world, entity, notifications, undo, "Change VHS Date Burn",
                    [value](VHS& updated) { updated.DateBurnEnabled = value; });
            },
            "Burn a two-line camcorder clock and date into the image with independent color, size, and position.");

        {
            UIElement* row = InspectorUI::AddRow(ctx.Parent);
            Label* label = InspectorUI::AddLabel(
                row, "Date Burn Color",
                "Independent recorded clock/date color (click to pick).");
            if (label)
                label->AddClass("inspector-label-no-drag");
            UIElement* field = InspectorUI::AddFieldContainer(row);
            field->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(6.0f));

            auto swatch = std::make_unique<UIElement>();
            UIElement* swatchRaw = swatch.get();
            InspectorUI::StyleColorSwatch(swatchRaw, ColorToArgb(effect->DateBurnColor));
            field->AddChild(std::move(swatch));

            auto rgb = std::make_unique<Label>();
            Label* rgbRaw = rgb.get();
            rgbRaw->AddClass("inspector-text");
            rgbRaw->SetText(FormatRgb(effect->DateBurnColor));
            rgbRaw->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
            field->AddChild(std::move(rgb));

            auto refresh = [swatchRaw, rgbRaw, world, entity]()
            {
                const auto* current = world->GetComponent<VHS>(entity);
                if (!current)
                    return;
                InspectorUI::StyleColorSwatch(swatchRaw, ColorToArgb(current->DateBurnColor));
                rgbRaw->SetText(FormatRgb(current->DateBurnColor));
            };

            auto click =
                [world, entity, notifications, undo, openPicker, refresh](
                    UIEvent& event)
            {
                if (event.Button != 0 || !openPicker)
                    return;
                event.Stop();
                const auto* component = world->GetComponent<VHS>(entity);
                if (!component)
                    return;

                using Edit = Editor::UndoRedoService::InteractiveEdit;
                auto edit = std::make_shared<Edit>();
                if (undo)
                {
                    auto target = MakeComponentSnapshotTarget<VHS>(
                        world, entity, notifications, "VHS Date Burn Color");
                    *edit = undo->BeginInteractiveEdit(
                        "Change VHS Date Burn Color", std::move(target));
                }

                ColorPickerCallbacks callbacks;
                callbacks.onApply =
                    [world, entity, notifications, edit, refresh](
                        uint32_t argb, float)
                {
                    if (*edit)
                    {
                        edit->Preview([&]
                        {
                            if (auto* writable =
                                    world->GetComponentForWrite<VHS>(entity))
                            {
                                ArgbToColor(argb, writable->DateBurnColor);
                            }
                        });
                        edit->Commit();
                    }
                    else if (const auto* current =
                                 world->GetComponent<VHS>(entity))
                    {
                        VHS updated = *current;
                        ArgbToColor(argb, updated.DateBurnColor);
                        Editor::CommitComponentUpdate(
                            world, entity, notifications, updated);
                    }
                    refresh();
                };
                callbacks.onCancel = [edit, refresh]()
                {
                    if (*edit)
                        edit->Cancel();
                    refresh();
                };
                callbacks.onValueChanging =
                    [world, entity, notifications, edit, refresh](
                        uint32_t argb, float)
                {
                    if (*edit)
                    {
                        edit->Preview([&]
                        {
                            if (auto* writable =
                                    world->GetComponentForWrite<VHS>(entity))
                            {
                                ArgbToColor(argb, writable->DateBurnColor);
                            }
                        });
                    }
                    else if (const auto* current =
                                 world->GetComponent<VHS>(entity))
                    {
                        VHS updated = *current;
                        ArgbToColor(argb, updated.DateBurnColor);
                        Editor::PreviewComponentUpdate(
                            world, entity, notifications, updated);
                    }
                    refresh();
                };
                openPicker(
                    ColorToArgb(component->DateBurnColor), 1.0f,
                    std::move(callbacks));
            };

            swatchRaw->RegisterEventHandler(kEventMouseDown, click);
            rgbRaw->RegisterEventHandler(kEventMouseDown, click);
        }

        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Date Size", effect->DateBurnSize,
            world, entity, notifications, undo, "Change VHS Date Burn Size",
            [](VHS& updated, float value)
            {
                updated.DateBurnSize = std::clamp(
                    value, VHS::kOverlaySizeMin, VHS::kOverlaySizeMax);
            },
            1.5f, "Independent scale for the date and clock burn-in.",
            {}, VHS::kOverlaySizeMin, VHS::kOverlaySizeMax);

        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Date Position X", effect->DateBurnPositionX,
            world, entity, notifications, undo,
            "Move VHS Date Burn Horizontally",
            [](VHS& updated, float value)
            {
                updated.DateBurnPositionX = std::clamp(value, 0.0f, 1.0f);
            },
            0.031875f, "Horizontal date-burn position: 0 is left and 1 is right.",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Date Position Y", effect->DateBurnPositionY,
            world, entity, notifications, undo,
            "Move VHS Date Burn Vertically",
            [](VHS& updated, float value)
            {
                updated.DateBurnPositionY = std::clamp(value, 0.0f, 1.0f);
            },
            0.072773f, "Vertical date-burn position: 0 is bottom and 1 is top.",
            {}, 0.0f, 1.0f);

        const auto addDateValue =
            [=](const char* label, uint32 value, uint32 VHS::* member,
                uint32 minimum, uint32 maximum, uint32 resetValue,
                const char* undoName, const char* tooltip)
        {
            AddComponentFloatRowWithDrag<VHS>(
                ctx.Parent, label, static_cast<float>(value),
                world, entity, notifications, undo, undoName,
                [member, minimum, maximum](VHS& updated, float newValue)
                {
                    const float rounded = std::floor(newValue + 0.5f);
                    updated.*member = static_cast<uint32>(std::clamp(
                        rounded, static_cast<float>(minimum),
                        static_cast<float>(maximum)));
                },
                static_cast<float>(resetValue), tooltip, {},
                static_cast<float>(minimum), static_cast<float>(maximum));
        };
        addDateValue(
            "Date Year", effect->DateBurnYear, &VHS::DateBurnYear,
            VHS::kDateBurnYearMin, VHS::kDateBurnYearMax, 2018,
            "Change VHS Date Burn Year", "Four-digit recorded date year.");
        addDateValue(
            "Date Month", effect->DateBurnMonth, &VHS::DateBurnMonth,
            1, 12, 5, "Change VHS Date Burn Month",
            "Recorded date month (1-12), displayed as a three-letter abbreviation.");
        addDateValue(
            "Date Day", effect->DateBurnDay, &VHS::DateBurnDay,
            1, 31, 16, "Change VHS Date Burn Day",
            "Recorded day of the month.");
        addDateValue(
            "Clock Hour", effect->DateBurnHour, &VHS::DateBurnHour,
            0, 23, 10, "Change VHS Date Burn Hour",
            "Starting hour in 24-hour input; the burn displays a running 12-hour AM/PM clock.");
        addDateValue(
            "Clock Minute", effect->DateBurnMinute, &VHS::DateBurnMinute,
            0, 59, 44, "Change VHS Date Burn Minute",
            "Starting minute for the running date-burn clock.");
        };

        TextField* overlayText = InspectorUI::AddTextRow(
            ctx.Parent, "Transport Text", std::string(effect->GetOverlayText()),
            "Status label recorded into the tape image. Up to 8 characters: A-Z, 0-9, spaces, colon, dash, period, or slash.");
        overlayText->SetFilterFunction(
            [](const std::string& raw, bool)
            {
                VHS sanitized{};
                sanitized.SetOverlayText(raw);
                return std::string(sanitized.GetOverlayText());
            });
        using TextEdit = Editor::UndoRedoService::InteractiveEdit;
        auto overlayTextEdit = std::make_shared<TextEdit>();
        overlayText->SetOnValueChanging(
            [world, entity, notifications, undo, overlayTextEdit](
                const std::string& value)
            {
                if (undo)
                {
                    if (!*overlayTextEdit)
                    {
                        auto target = MakeComponentSnapshotTarget<VHS>(
                            world, entity, notifications, "VHS Transport Text");
                        *overlayTextEdit = undo->BeginInteractiveEdit(
                            "Change VHS Transport Text", std::move(target));
                    }
                    overlayTextEdit->Preview(
                        [&]
                        {
                            if (auto* writable = world->GetComponentForWrite<VHS>(entity))
                                writable->SetOverlayText(value);
                        });
                }
                else if (const auto* current = world->GetComponent<VHS>(entity))
                {
                    VHS updated = *current;
                    updated.SetOverlayText(value);
                    Editor::PreviewComponentUpdate(
                        world, entity, notifications, updated);
                }
            });
        overlayText->SetOnCommit(
            [overlayText, world, entity, notifications, overlayTextEdit]()
            {
                const std::string value = overlayText->GetValue();
                if (*overlayTextEdit)
                {
                    overlayTextEdit->Preview(
                        [&]
                        {
                            if (auto* writable = world->GetComponentForWrite<VHS>(entity))
                                writable->SetOverlayText(value);
                        });
                    overlayTextEdit->Commit();
                }
                else if (const auto* current = world->GetComponent<VHS>(entity))
                {
                    VHS updated = *current;
                    updated.SetOverlayText(value);
                    Editor::CommitComponentUpdate(
                        world, entity, notifications, updated);
                }
            });

        static const std::vector<Dropdown::Option> kOverlayFonts = {
            {"0", "Rounded"},
            {"1", "Block"},
            {"2", "LCD"},
            {"3", "Solid"},
        };
        auto* font = InspectorUI::AddDropdownRow(
            ctx.Parent, "Transport Font", kOverlayFonts,
            static_cast<int>(std::min(effect->OverlayFont, VHS::kOverlayFontMax)),
            "Shader-native OSD font style.");
        font->SetOnValueChanged([world, entity, notifications, undo](const std::string& value)
        {
            const uint32_t selected = static_cast<uint32_t>(
                std::clamp(std::atoi(value.c_str()), 0, 3));
            CommitComponentWithUndo<VHS>(
                world, entity, notifications, undo, "Change VHS Transport Font",
                [selected](VHS& updated) { updated.OverlayFont = selected; });
        });

        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Transport Size", effect->OverlaySize,
            world, entity, notifications, undo, "Change VHS Transport Size",
            [](VHS& updated, float value)
            {
                updated.OverlaySize = std::clamp(
                    value, VHS::kOverlaySizeMin, VHS::kOverlaySizeMax);
            },
            2.0f, "Scale of the status label and time display.",
            {}, VHS::kOverlaySizeMin, VHS::kOverlaySizeMax);

        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Transport Position X", effect->OverlayPositionX,
            world, entity, notifications, undo, "Move VHS Transport Overlay Horizontally",
            [](VHS& updated, float value)
            {
                updated.OverlayPositionX = std::clamp(value, 0.0f, 1.0f);
            },
            0.01957f, "Horizontal screen position: 0 is left, 1 is right.",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Transport Position Y", effect->OverlayPositionY,
            world, entity, notifications, undo, "Move VHS Transport Overlay Vertically",
            [](VHS& updated, float value)
            {
                updated.OverlayPositionY = std::clamp(value, 0.0f, 1.0f);
            },
            0.0f, "Vertical screen position: 0 is top, 1 is bottom.",
            {}, 0.0f, 1.0f);

        {
            UIElement* row = InspectorUI::AddRow(ctx.Parent);
            Label* label = InspectorUI::AddLabel(
                row, "Transport Color",
                "Recorded OSD color (click to pick; green and white are typical).");
            if (label)
                label->AddClass("inspector-label-no-drag");
            UIElement* field = InspectorUI::AddFieldContainer(row);
            field->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(6.0f));

            auto swatch = std::make_unique<UIElement>();
            UIElement* swatchRaw = swatch.get();
            InspectorUI::StyleColorSwatch(swatchRaw, ColorToArgb(effect->OverlayColor));
            field->AddChild(std::move(swatch));

            auto rgb = std::make_unique<Label>();
            Label* rgbRaw = rgb.get();
            rgbRaw->AddClass("inspector-text");
            rgbRaw->SetText(FormatRgb(effect->OverlayColor));
            rgbRaw->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
            field->AddChild(std::move(rgb));

            auto refresh = [swatchRaw, rgbRaw, world, entity]()
            {
                const auto* current = world->GetComponent<VHS>(entity);
                if (!current)
                    return;
                InspectorUI::StyleColorSwatch(swatchRaw, ColorToArgb(current->OverlayColor));
                rgbRaw->SetText(FormatRgb(current->OverlayColor));
            };

            auto click = [world, entity, notifications, undo, openPicker, refresh](UIEvent& event)
            {
                if (event.Button != 0 || !openPicker)
                    return;
                event.Stop();
                const auto* component = world->GetComponent<VHS>(entity);
                if (!component)
                    return;

                using Edit = Editor::UndoRedoService::InteractiveEdit;
                auto edit = std::make_shared<Edit>();
                if (undo)
                {
                    auto target = MakeComponentSnapshotTarget<VHS>(
                        world, entity, notifications, "VHS Transport Color");
                    *edit = undo->BeginInteractiveEdit(
                        "Change VHS Transport Color", std::move(target));
                }

                ColorPickerCallbacks callbacks;
                callbacks.onApply = [world, entity, notifications, edit, refresh](
                    uint32_t argb, float)
                {
                    if (*edit)
                    {
                        edit->Preview([&]
                        {
                            if (auto* writable = world->GetComponentForWrite<VHS>(entity))
                                ArgbToColor(argb, writable->OverlayColor);
                        });
                        edit->Commit();
                    }
                    else if (const auto* current = world->GetComponent<VHS>(entity))
                    {
                        VHS updated = *current;
                        ArgbToColor(argb, updated.OverlayColor);
                        Editor::CommitComponentUpdate(
                            world, entity, notifications, updated);
                    }
                    refresh();
                };
                callbacks.onCancel = [edit, refresh]()
                {
                    if (*edit)
                        edit->Cancel();
                    refresh();
                };
                callbacks.onValueChanging =
                    [world, entity, notifications, edit, refresh](
                        uint32_t argb, float)
                {
                    if (*edit)
                    {
                        edit->Preview([&]
                        {
                            if (auto* writable = world->GetComponentForWrite<VHS>(entity))
                                ArgbToColor(argb, writable->OverlayColor);
                        });
                    }
                    else if (const auto* current = world->GetComponent<VHS>(entity))
                    {
                        VHS updated = *current;
                        ArgbToColor(argb, updated.OverlayColor);
                        Editor::PreviewComponentUpdate(
                            world, entity, notifications, updated);
                    }
                    refresh();
                };
                openPicker(ColorToArgb(component->OverlayColor), 1.0f,
                           std::move(callbacks));
            };

            swatchRaw->RegisterEventHandler(kEventMouseDown, click);
            rgbRaw->RegisterEventHandler(kEventMouseDown, click);
        }

        AddComponentFloatRowWithDrag<VHS>(
            ctx.Parent, "Transport Opacity", effect->OverlayOpacity,
            world, entity, notifications, undo, "Change VHS Transport Opacity",
            [](VHS& updated, float value)
            {
                updated.OverlayOpacity = std::clamp(value, 0.0f, 1.0f);
            },
            0.85f, "Opacity of the recorded camcorder OSD.",
            {}, 0.0f, 1.0f);

        addDateBurnSection();
    };

    InspectorRegistry::Get().RegisterComponentInspector<VHS>(std::move(fn));
}

} // namespace GameEngine
