#include "Inspectors/ValueCurveInspector.h"

#include "InspectorRegistry.h"

#include "Components/Animation/ValueCurve.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/EditorIcons.h"
#include "UI/UIEvents.h"
#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/CubicBezierField.h"
#include "UI/Controls/CurveTangents.h"
#include "UI/Controls/CurvePresets.h"
#include "UI/Controls/CurvePresetPicker.h"

#include "EditorContextMenu/UIContextMenu.h"
#include "Platform/ContextMenu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

std::vector<Dropdown::Option> BuildTargetOptions()
{
    return {
        {"0", "None"},
        {"1", "Transform · Position · X"},
        {"2", "Transform · Position · Y"},
        {"3", "Transform · Position · Z"},
        {"4", "Transform · Scale · X"},
        {"5", "Transform · Scale · Y"},
        {"6", "Transform · Scale · Z"},
        {"7", "Light · Intensity"},
        {"8", "Post Process · Blend Weight"},
    };
}

std::vector<Dropdown::Option> BuildShapeModeOptions()
{
    return {
        {"0", "Key Curve"},
        {"1", "Cubic Bezier"},
    };
}

std::vector<Math::CurveKey> ShapeKeys(const Components::ValueCurve& c)
{
    return std::vector<Math::CurveKey>(c.Shape.Keys, c.Shape.Keys + c.Shape.KeyCount);
}

CubicBezierShape CubicBezierShapeFromCurve(const Components::ValueCurve& c)
{
    return {
        std::clamp(c.CubicBezierControl1X, 0.0f, 1.0f),
        std::clamp(c.CubicBezierControl1Y, 0.0f, 1.0f),
        std::clamp(c.CubicBezierControl2X, 0.0f, 1.0f),
        std::clamp(c.CubicBezierControl2Y, 0.0f, 1.0f),
        std::clamp(c.CubicBezierAnchorStartY, 0.0f, 1.0f),
        std::clamp(c.CubicBezierAnchorEndY, 0.0f, 1.0f),
    };
}

CubicBezierShape CubicBezierShapeFromPreset(const CurvePresets::CubicBezierPreset& preset)
{
    return {
        std::clamp(preset.Control1X, 0.0f, 1.0f),
        std::clamp(preset.Control1Y, 0.0f, 1.0f),
        std::clamp(preset.Control2X, 0.0f, 1.0f),
        std::clamp(preset.Control2Y, 0.0f, 1.0f),
        std::clamp(preset.AnchorStartY, 0.0f, 1.0f),
        std::clamp(preset.AnchorEndY, 0.0f, 1.0f),
    };
}

CurvePresets::CubicBezierPreset CubicBezierPresetFromCurve(const Components::ValueCurve& c)
{
    return {
        std::clamp(c.CubicBezierControl1X, 0.0f, 1.0f),
        std::clamp(c.CubicBezierControl1Y, 0.0f, 1.0f),
        std::clamp(c.CubicBezierControl2X, 0.0f, 1.0f),
        std::clamp(c.CubicBezierControl2Y, 0.0f, 1.0f),
        std::clamp(c.CubicBezierAnchorStartY, 0.0f, 1.0f),
        std::clamp(c.CubicBezierAnchorEndY, 0.0f, 1.0f),
    };
}

void ApplyCubicBezierShape(Components::ValueCurve& c, const CubicBezierShape& shape)
{
    c.ShapeMode = Components::ValueCurveShapeMode::CubicBezier;
    c.UseBuiltinEasing = false;
    c.CubicBezierControl1X = std::clamp(shape.C1X, 0.0f, 1.0f);
    c.CubicBezierControl1Y = std::clamp(shape.C1Y, 0.0f, 1.0f);
    c.CubicBezierControl2X = std::clamp(shape.C2X, 0.0f, 1.0f);
    c.CubicBezierControl2Y = std::clamp(shape.C2Y, 0.0f, 1.0f);
    c.CubicBezierAnchorStartY = std::clamp(shape.AnchorStartY, 0.0f, 1.0f);
    c.CubicBezierAnchorEndY = std::clamp(shape.AnchorEndY, 0.0f, 1.0f);
    Components::RefreshValueCurveCurrentValueFromEvaluation(c);
}

void ApplyBuiltinEasingShape(Components::ValueCurve& c, Math::TweenEasing easing)
{
    c.ShapeMode = Components::ValueCurveShapeMode::KeyCurve;
    c.UseBuiltinEasing = true;
    c.BuiltinEasing = easing;
    c.Shape = Components::MakeValueCurveShapeFromEasing(easing);
    Components::RefreshValueCurveCurrentValueFromEvaluation(c);
}

Math::Curve SampleCubicBezierAsShape(const Components::ValueCurve& c, int sampleCount = 9)
{
    Math::Curve shape;
    sampleCount = std::clamp(sampleCount, 2, static_cast<int>(Math::Curve::Capacity));
    for (int i = 0; i < sampleCount; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(sampleCount - 1);
        Math::CurveKey key;
        key.Time = t;
        key.Value = Components::EvaluateValueCurveCubicBezier01(c, t);
        key.Interp = Math::CurveInterp::Linear;
        shape.TryInsert(key);
    }
    return shape;
}

void SeedCubicBezierFromShape(Components::ValueCurve& c)
{
    const float y0 = std::clamp(c.Shape.Evaluate(0.0f), 0.0f, 1.0f);
    const float y3 = std::clamp(c.Shape.Evaluate(1.0f), 0.0f, 1.0f);
    constexpr float eps = 0.01f;
    const float m0 = (std::clamp(c.Shape.Evaluate(eps), 0.0f, 1.0f) - y0) / eps;
    const float m1 = (y3 - std::clamp(c.Shape.Evaluate(1.0f - eps), 0.0f, 1.0f)) / eps;
    c.CubicBezierAnchorStartY = y0;
    c.CubicBezierAnchorEndY = y3;
    c.CubicBezierControl1X = 1.0f / 3.0f;
    c.CubicBezierControl1Y = std::clamp(y0 + m0 / 3.0f, 0.0f, 1.0f);
    c.CubicBezierControl2X = 2.0f / 3.0f;
    c.CubicBezierControl2Y = std::clamp(y3 - m1 / 3.0f, 0.0f, 1.0f);
}

void ApplyShapeModeChange(Components::ValueCurve& c, Components::ValueCurveShapeMode mode)
{
    if (c.ShapeMode == mode)
        return;

    if (mode == Components::ValueCurveShapeMode::CubicBezier)
    {
        SeedCubicBezierFromShape(c);
        c.ShapeMode = Components::ValueCurveShapeMode::CubicBezier;
        c.UseBuiltinEasing = false;
    }
    else
    {
        c.Shape = SampleCubicBezierAsShape(c);
        c.ShapeMode = Components::ValueCurveShapeMode::KeyCurve;
        c.UseBuiltinEasing = false;
    }
    Components::RefreshValueCurveCurrentValueFromEvaluation(c);
}

// Write an edited key list back into the component's Shape (capacity-bounded) and refresh the
// cached evaluated value so any target/preview stays consistent.
void ApplyKeysToShape(Components::ValueCurve& c, const std::vector<Math::CurveKey>& keys)
{
    c.ShapeMode = Components::ValueCurveShapeMode::KeyCurve;
    c.UseBuiltinEasing = false;
    c.Shape = Math::Curve{};
    for (const Math::CurveKey& k : keys)
    {
        if (c.Shape.KeyCount >= Math::Curve::Capacity)
            break;
        c.Shape.TryInsert(k);
    }
    Components::RefreshValueCurveCurrentValueFromEvaluation(c);
}

float ElapsedSecondsFromNormalizedTime(const Components::ValueCurve& curve, float normalizedTime)
{
    constexpr float kMinDurationSeconds = 0.0001f;
    return std::clamp(normalizedTime, 0.0f, 1.0f) * std::max(curve.DurationSeconds, kMinDurationSeconds);
}

float FindClosestNormalizedTimeForOutput(const Components::ValueCurve& curve, float targetOutput)
{
    auto valueAt = [&curve](float t) {
        const float eased = Components::EvaluateValueCurve01(curve, std::clamp(t, 0.0f, 1.0f));
        return curve.StartValue + (curve.EndValue - curve.StartValue) * eased;
    };
    auto errorAt = [&](float t) {
        const float d = valueAt(t) - targetOutput;
        return d * d;
    };

    constexpr int kSamples = 512;
    int bestIndex = 0;
    float bestError = errorAt(0.0f);
    for (int i = 1; i <= kSamples; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kSamples);
        const float err = errorAt(t);
        if (err < bestError)
        {
            bestError = err;
            bestIndex = i;
        }
    }

    float lo = static_cast<float>(std::max(0, bestIndex - 1)) / static_cast<float>(kSamples);
    float hi = static_cast<float>(std::min(kSamples, bestIndex + 1)) / static_cast<float>(kSamples);
    for (int i = 0; i < 28; ++i)
    {
        const float m1 = lo + (hi - lo) / 3.0f;
        const float m2 = hi - (hi - lo) / 3.0f;
        if (errorAt(m1) < errorAt(m2))
            hi = m2;
        else
            lo = m1;
    }
    return std::clamp((lo + hi) * 0.5f, 0.0f, 1.0f);
}

float NormalizedPlaybackTime(const Components::ValueCurve& curve)
{
    constexpr float kMinDurationSeconds = 0.0001f;
    const float durationSeconds = std::max(curve.DurationSeconds, kMinDurationSeconds);
    if (curve.Loop)
    {
        const float cycleLength = curve.PingPong ? 2.0f : 1.0f;
        float cycleT = std::fmod(curve.ElapsedSeconds / durationSeconds, cycleLength);
        if (cycleT < 0.0f)
            cycleT += cycleLength;
        return (curve.PingPong && cycleT > 1.0f) ? (2.0f - cycleT) : cycleT;
    }

    float cycleT = curve.ElapsedSeconds / durationSeconds;
    if (curve.PingPong)
    {
        cycleT = std::clamp(cycleT, 0.0f, 2.0f);
        return (cycleT > 1.0f) ? (2.0f - cycleT) : cycleT;
    }
    return std::clamp(cycleT, 0.0f, 1.0f);
}

} // namespace

void RegisterValueCurveInspector()
{
    using namespace InspectorDrag;

    InspectorRegistry::Get().RegisterComponentInspector<Components::ValueCurve>(
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
                return;

            auto* curve = ctx.World->GetComponent<Components::ValueCurve>(ctx.Entity);
            if (!curve)
                return;

            auto getWorld = ctx.GetWorld;
            ECS::EntityHandle entity = ctx.Entity;
            Editor::EditorChangeNotifications* notifications = ctx.ChangeNotifications;
            Editor::UndoRedoService* undo = ctx.Undo;

            AddToggleRow(
                ctx.Parent, "Playing", curve->Playing,
                [getWorld, entity, notifications, undo](bool value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Toggle Value Curve Playing",
                        [value](Components::ValueCurve& c) { c.Playing = value; });
                },
                "Advance elapsed time and evaluate the curve each frame");

            AddFloatRowWithDrag(
                ctx.Parent, "Duration (s)", curve->DurationSeconds, [](float) {},
                [getWorld, entity, notifications, undo](float value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Change Value Curve Duration",
                        [value](Components::ValueCurve& c)
                        {
                            constexpr float32 kMinDurationSeconds = 0.0001f;
                            c.DurationSeconds = std::max(value, kMinDurationSeconds);
                            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
                        });
                },
                1.0f, "Seconds for one forward pass of the curve", 0.0001f);

            AddFloatRowWithDrag(
                ctx.Parent, "Start Value", curve->StartValue, [](float) {},
                [getWorld, entity, notifications, undo](float value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Change Value Curve Start",
                        [value](Components::ValueCurve& c)
                        {
                            c.StartValue = value;
                            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
                        });
                },
                0.0f, "Value the shape maps to at t=0");

            AddFloatRowWithDrag(
                ctx.Parent, "End Value", curve->EndValue, [](float) {},
                [getWorld, entity, notifications, undo](float value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Change Value Curve End",
                        [value](Components::ValueCurve& c)
                        {
                            c.EndValue = value;
                            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
                        });
                },
                1.0f, "Value the shape maps to at t=1");

            AddToggleRow(
                ctx.Parent, "Loop", curve->Loop,
                [getWorld, entity, notifications, undo](bool value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Toggle Value Curve Loop",
                        [value](Components::ValueCurve& c)
                        {
                            c.Loop = value;
                            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
                        });
                },
                "Restart from the beginning when duration elapses");

            AddToggleRow(
                ctx.Parent, "Ping Pong", curve->PingPong,
                [getWorld, entity, notifications, undo](bool value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Toggle Value Curve Ping Pong",
                        [value](Components::ValueCurve& c)
                        {
                            c.PingPong = value;
                            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
                        });
                },
                "Play forward then backward");

            static const std::vector<Dropdown::Option> kShapeModeOptions = BuildShapeModeOptions();
            Dropdown* shapeModeDropdown = InspectorUI::AddDropdownRow(
                ctx.Parent, "Shape Mode", kShapeModeOptions, static_cast<int>(curve->ShapeMode));
            const bool initialCubicBezierMode = curve->ShapeMode == Components::ValueCurveShapeMode::CubicBezier;
            auto suppressShapeModeChange = std::make_shared<bool>(false);

            CurveField* curveFieldRaw = nullptr;
            CubicBezierField* bezierFieldRaw = nullptr;

            auto curveField = std::make_unique<CurveField>();
            curveFieldRaw = curveField.get();
            CurveField::Config cfg;
            cfg.TimeMin = 0.0f;
            cfg.TimeMax = 1.0f;
            cfg.ValueMin = 0.0f;
            cfg.ValueMax = 1.0f;
            cfg.AllowTimeDrag = true;
            cfg.AllowAddRemove = true;
            cfg.AllowTangentEditing = true;
            cfg.WrapAround = false;
            cfg.ShowPlaybackIndicator = true;
            cfg.AllowPlaybackScrub = true;
            cfg.MinKeys = 2;
            cfg.MaxKeys = Math::Curve::Capacity;
            curveField->SetConfig(cfg);
            curveField->SetKeys(ShapeKeys(*curve));
            curveField->Overrides()
                .Set(Style::Height, StyleLength::Px(220.0f))
                .Set(Style::MinHeight, StyleLength::Px(220.0f))
                .Set(Style::MarginTop, StyleLength::Px(8.0f));
            if (initialCubicBezierMode)
                curveField->AddClass("hidden");

            auto bezierField = std::make_unique<CubicBezierField>();
            bezierFieldRaw = bezierField.get();
            CubicBezierField::Config bezierCfg;
            bezierCfg.TimeMin = 0.0f;
            bezierCfg.TimeMax = 1.0f;
            bezierCfg.ValueMin = 0.0f;
            bezierCfg.ValueMax = 1.0f;
            bezierCfg.ShowPlaybackIndicator = true;
            bezierCfg.AllowPlaybackScrub = true;
            bezierField->SetConfig(bezierCfg);
            bezierField->SetShape(CubicBezierShapeFromCurve(*curve));
            if (!initialCubicBezierMode)
                bezierField->AddClass("hidden");

            auto setPlaybackIndicator = [curveFieldRaw, bezierFieldRaw](const Components::ValueCurve& vc)
            {
                const float t = NormalizedPlaybackTime(vc);
                const float v = std::clamp(Components::EvaluateValueCurve01(vc, t), 0.0f, 1.0f);
                if (curveFieldRaw)
                    curveFieldRaw->SetPlaybackIndicator(t, v, vc.Enabled);
                if (bezierFieldRaw)
                    bezierFieldRaw->SetPlaybackIndicator(t, v, vc.Enabled);
            };

            if (curveFieldRaw)
            {
                curveFieldRaw->SetOnPlaybackScrub(
                    [getWorld, entity, notifications, setPlaybackIndicator](float normalizedTime)
                    {
                        ECS::World* world = getWorld ? getWorld() : nullptr;
                        if (!world) return;
                        auto* vc = world->GetComponent<Components::ValueCurve>(entity);
                        if (!vc) return;

                        Components::ValueCurve updated = *vc;
                        updated.ElapsedSeconds = ElapsedSecondsFromNormalizedTime(updated, normalizedTime);
                        Components::RefreshValueCurveCurrentValueFromEvaluation(updated);
                        setPlaybackIndicator(updated);
                        Editor::PreviewComponentUpdate(world, entity, notifications, updated);
                    });

                curveFieldRaw->SetOnChanging(
                    [getWorld, entity, notifications, setPlaybackIndicator](const std::vector<Math::CurveKey>& keys)
                    {
                        ECS::World* world = getWorld ? getWorld() : nullptr;
                        if (!world) return;
                        auto* vc = world->GetComponent<Components::ValueCurve>(entity);
                        if (!vc) return;
                        Components::ValueCurve updated = *vc;
                        ApplyKeysToShape(updated, keys);
                        setPlaybackIndicator(updated);
                        Editor::PreviewComponentUpdate(world, entity, notifications, updated);
                    });
                curveFieldRaw->SetOnChanged(
                    [getWorld, entity, notifications, undo, setPlaybackIndicator](const std::vector<Math::CurveKey>& keys)
                    {
                        ECS::World* world = getWorld ? getWorld() : nullptr;
                        if (!world) return;
                        if (auto* vc = world->GetComponent<Components::ValueCurve>(entity))
                        {
                            Components::ValueCurve updated = *vc;
                            ApplyKeysToShape(updated, keys);
                            setPlaybackIndicator(updated);
                        }
                        CommitComponentWithUndo<Components::ValueCurve>(
                            world, entity, notifications, undo, "Edit Value Curve Shape",
                            [keys](Components::ValueCurve& c) { ApplyKeysToShape(c, keys); });
                    });
                curveFieldRaw->SetOnKeyContextMenu(
                    [curveFieldRaw, window = ctx.Window](int keyIndex, float x, float y)
                    {
                        if (!window || !curveFieldRaw)
                            return;

                        static std::shared_ptr<INativeContextMenu> s_Menu;
                        if (!s_Menu)
                        {
                            s_Menu = CreateContextMenu();
                            if (!s_Menu)
                                return;
                        }

                        enum : uint32_t
                        {
                            kCmdLinear = 1,
                            kCmdSmoothAuto = 2,
                            kCmdSmoothFlat = 3,
                            kCmdStep = 4,
                            kCmdDelete = 5,
                        };

                        const auto& keys = curveFieldRaw->GetKeys();
                        if (keyIndex < 0 || keyIndex >= static_cast<int>(keys.size()))
                            return;

                        const bool canDelete = keys.size() > 2;
                        const Math::CurveKey selectedKey = keys[static_cast<size_t>(keyIndex)];
                        // Interpolation mode is exactly-one-of.
        auto checked = [](bool value) -> uint32_t {
            return MenuItemFlag_Radio | (value ? MenuItemFlag_Checked : MenuItemFlag_None);
        };

                        s_Menu->Clear();
                        s_Menu->SetCommandHandler(
                            [curveFieldRaw, keyIndex](uint32_t cmd)
                            {
                                if (!curveFieldRaw)
                                    return;

                                switch (cmd)
                                {
                                case kCmdLinear:
                                    curveFieldRaw->SetKeyMode(keyIndex, Math::CurveInterp::Linear,
                                                              Math::CurveTangentMode::Auto);
                                    break;
                                case kCmdSmoothAuto:
                                    curveFieldRaw->SetKeyMode(keyIndex, Math::CurveInterp::Smooth,
                                                              Math::CurveTangentMode::Auto);
                                    break;
                                case kCmdSmoothFlat:
                                    curveFieldRaw->SetKeyMode(keyIndex, Math::CurveInterp::Smooth,
                                                              Math::CurveTangentMode::Flat);
                                    break;
                                case kCmdStep:
                                    curveFieldRaw->SetKeyMode(keyIndex, Math::CurveInterp::Step,
                                                              Math::CurveTangentMode::Auto);
                                    break;
                                case kCmdDelete:
                                    curveFieldRaw->DeleteKey(keyIndex);
                                    break;
                                default:
                                    break;
                                }
                            });

                        s_Menu->AddItem(0, "Linear", kCmdLinear,
                                        checked(selectedKey.Interp == Math::CurveInterp::Linear));
                        s_Menu->SetItemIcon(kCmdLinear, EditorIcons::kLinearCurve);
                        s_Menu->AddItem(0, "Smooth (Auto)", kCmdSmoothAuto,
                                        checked(selectedKey.Interp == Math::CurveInterp::Smooth &&
                                                selectedKey.TangentMode == Math::CurveTangentMode::Auto));
                        s_Menu->SetItemIcon(kCmdSmoothAuto, EditorIcons::kDrawCurve);
                        s_Menu->AddItem(0, "Smooth (Flat)", kCmdSmoothFlat,
                                        checked(selectedKey.Interp == Math::CurveInterp::Smooth &&
                                                selectedKey.TangentMode == Math::CurveTangentMode::Flat));
                        s_Menu->SetItemIcon(kCmdSmoothFlat, EditorIcons::kLinearCurve);
                        s_Menu->AddItem(0, "Step", kCmdStep,
                                        checked(selectedKey.Interp == Math::CurveInterp::Step));
                        s_Menu->SetItemIcon(kCmdStep, EditorIcons::kSteppedCurve);
                        s_Menu->AddSeparator(0);
                        s_Menu->AddItem(0, "Delete Key", kCmdDelete,
                                        canDelete ? MenuItemFlag_None : MenuItemFlag_Disabled);
                        s_Menu->SetItemIcon(kCmdDelete, EditorIcons::kTrash);
                        s_Menu->Show(window, static_cast<int>(x), static_cast<int>(y));
                    });
            }
            if (bezierFieldRaw)
            {
                bezierFieldRaw->SetOnPlaybackScrub(
                    [getWorld, entity, notifications, setPlaybackIndicator](float normalizedTime)
                    {
                        ECS::World* world = getWorld ? getWorld() : nullptr;
                        if (!world) return;
                        auto* vc = world->GetComponent<Components::ValueCurve>(entity);
                        if (!vc) return;

                        Components::ValueCurve updated = *vc;
                        updated.ElapsedSeconds = ElapsedSecondsFromNormalizedTime(updated, normalizedTime);
                        Components::RefreshValueCurveCurrentValueFromEvaluation(updated);
                        setPlaybackIndicator(updated);
                        Editor::PreviewComponentUpdate(world, entity, notifications, updated);
                    });
                bezierFieldRaw->SetOnChanging(
                    [getWorld, entity, notifications, setPlaybackIndicator](const CubicBezierShape& shape)
                    {
                        ECS::World* world = getWorld ? getWorld() : nullptr;
                        if (!world) return;
                        auto* vc = world->GetComponent<Components::ValueCurve>(entity);
                        if (!vc) return;
                        Components::ValueCurve updated = *vc;
                        ApplyCubicBezierShape(updated, shape);
                        setPlaybackIndicator(updated);
                        Editor::PreviewComponentUpdate(world, entity, notifications, updated);
                    });
                bezierFieldRaw->SetOnChanged(
                    [getWorld, entity, notifications, undo, setPlaybackIndicator](const CubicBezierShape& shape)
                    {
                        ECS::World* world = getWorld ? getWorld() : nullptr;
                        if (!world) return;
                        if (auto* vc = world->GetComponent<Components::ValueCurve>(entity))
                        {
                            Components::ValueCurve updated = *vc;
                            ApplyCubicBezierShape(updated, shape);
                            setPlaybackIndicator(updated);
                        }
                        CommitComponentWithUndo<Components::ValueCurve>(
                            world, entity, notifications, undo, "Edit Value Curve Bezier",
                            [shape](Components::ValueCurve& c) { ApplyCubicBezierShape(c, shape); });
                    });
            }

            ctx.Parent->AddChild(std::move(curveField));
            ctx.Parent->AddChild(std::move(bezierField));

            FloatField* timeFieldRaw = AddFloatRowWithDrag(
                ctx.Parent, "Time (s)", curve->ElapsedSeconds,
                [getWorld, entity, notifications, setPlaybackIndicator](float value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    auto* vc = world->GetComponent<Components::ValueCurve>(entity);
                    if (!vc) return;
                    Components::ValueCurve updated = *vc;
                    updated.ElapsedSeconds = std::max(0.0f, value);
                    Components::RefreshValueCurveCurrentValueFromEvaluation(updated);
                    setPlaybackIndicator(updated);
                    Editor::PreviewComponentUpdate(world, entity, notifications, updated);
                },
                [getWorld, entity, notifications, undo, setPlaybackIndicator](float value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    if (auto* vc = world->GetComponent<Components::ValueCurve>(entity))
                    {
                        Components::ValueCurve updated = *vc;
                        updated.ElapsedSeconds = std::max(0.0f, value);
                        Components::RefreshValueCurveCurrentValueFromEvaluation(updated);
                        setPlaybackIndicator(updated);
                    }
                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Change Value Curve Time",
                        [value](Components::ValueCurve& c)
                        {
                            c.ElapsedSeconds = std::max(0.0f, value);
                            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
                        });
                },
                0.0f, "Elapsed playback time", 0.0f);

            const float evaluatedOutput = Components::EvaluateValueCurveValue(*curve, curve->ElapsedSeconds);
            FloatField* outputFieldRaw = AddFloatRowWithDrag(
                ctx.Parent, "Output", evaluatedOutput,
                [getWorld, entity, notifications, setPlaybackIndicator](float value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    auto* vc = world->GetComponent<Components::ValueCurve>(entity);
                    if (!vc) return;
                    Components::ValueCurve updated = *vc;
                    const float t = FindClosestNormalizedTimeForOutput(updated, value);
                    updated.ElapsedSeconds = ElapsedSecondsFromNormalizedTime(updated, t);
                    Components::RefreshValueCurveCurrentValueFromEvaluation(updated);
                    setPlaybackIndicator(updated);
                    Editor::PreviewComponentUpdate(world, entity, notifications, updated);
                },
                [getWorld, entity, notifications, undo, setPlaybackIndicator](float value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    if (auto* vc = world->GetComponent<Components::ValueCurve>(entity))
                    {
                        Components::ValueCurve updated = *vc;
                        const float t = FindClosestNormalizedTimeForOutput(updated, value);
                        updated.ElapsedSeconds = ElapsedSecondsFromNormalizedTime(updated, t);
                        Components::RefreshValueCurveCurrentValueFromEvaluation(updated);
                        setPlaybackIndicator(updated);
                    }
                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Change Value Curve Output",
                        [value](Components::ValueCurve& c)
                        {
                            const float t = FindClosestNormalizedTimeForOutput(c, value);
                            c.ElapsedSeconds = ElapsedSecondsFromNormalizedTime(c, t);
                            Components::RefreshValueCurveCurrentValueFromEvaluation(c);
                        });
                },
                evaluatedOutput, "Find the closest curve time for this output value");

            auto updatePlaybackReadout = [getWorld, entity, curveFieldRaw, bezierFieldRaw, timeFieldRaw, outputFieldRaw,
                                          setPlaybackIndicator]()
            {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world)
                    return;
                auto* vc = world->GetComponent<Components::ValueCurve>(entity);
                if (!vc)
                {
                    if (curveFieldRaw)
                        curveFieldRaw->SetPlaybackIndicator(0.0f, 0.0f, false);
                    if (bezierFieldRaw)
                        bezierFieldRaw->SetPlaybackIndicator(0.0f, 0.0f, false);
                    return;
                }
                setPlaybackIndicator(*vc);
                if (timeFieldRaw)
                    timeFieldRaw->SetValueWithoutNotify(vc->ElapsedSeconds);
                if (outputFieldRaw)
                    outputFieldRaw->SetValueWithoutNotify(Components::EvaluateValueCurveValue(*vc, vc->ElapsedSeconds));
            };
            updatePlaybackReadout();
            if (ctx.FrameRefreshCallbacks)
                ctx.FrameRefreshCallbacks->push_back(updatePlaybackReadout);
            else if (ctx.SimulationRefreshCallbacks)
                ctx.SimulationRefreshCallbacks->push_back(updatePlaybackReadout);

            // Preset foldout: curated built-ins + the user's saved library. Picking replaces the shape;
            // saving captures the current shape into the shared user library.
            auto presetFoldout = std::make_unique<Foldout>();
            presetFoldout->SetTitle("Presets");
            presetFoldout->AddClass("rp-foldout");
            presetFoldout->AddClass("curve-preset-foldout");
            presetFoldout->SetExpanded(false);
            UIElement* presetContent = presetFoldout->GetContentContainer();

            auto picker = std::make_unique<CurvePresetPicker>();
            CurvePresetPicker* pickerRaw = picker.get();
            picker->SetContextMenuWindow(ctx.Window);
            picker->SetOnPick(
                [getWorld, entity, notifications, undo, curveFieldRaw, bezierFieldRaw, shapeModeDropdown,
                 suppressShapeModeChange, setPlaybackIndicator](const CurvePresets::CurvePreset& preset)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;

                    const bool applyBezier = preset.Kind == CurvePresets::CurvePresetKind::CubicBezier;
                    if (curveFieldRaw)
                    {
                        if (applyBezier)
                            curveFieldRaw->AddClass("hidden");
                        else
                            curveFieldRaw->RemoveClass("hidden");
                    }
                    if (bezierFieldRaw)
                    {
                        if (applyBezier)
                            bezierFieldRaw->RemoveClass("hidden");
                        else
                            bezierFieldRaw->AddClass("hidden");
                    }
                    if (shapeModeDropdown && suppressShapeModeChange)
                    {
                        *suppressShapeModeChange = true;
                        shapeModeDropdown->SetSelectedIndex(applyBezier ? 1 : 0);
                        *suppressShapeModeChange = false;
                    }

                    if (applyBezier)
                    {
                        const CubicBezierShape shape = CubicBezierShapeFromPreset(preset.Bezier);
                        if (bezierFieldRaw)
                            bezierFieldRaw->SetShape(shape);
                        if (auto* vc = world->GetComponent<Components::ValueCurve>(entity))
                        {
                            Components::ValueCurve updated = *vc;
                            ApplyCubicBezierShape(updated, shape);
                            setPlaybackIndicator(updated);
                        }

                        CommitComponentWithUndo<Components::ValueCurve>(
                            world, entity, notifications, undo, "Apply Curve Preset",
                            [shape](Components::ValueCurve& c) { ApplyCubicBezierShape(c, shape); });
                        return;
                    }

                    const std::vector<Math::CurveKey> keys = preset.Curve.Keys;
                    if (curveFieldRaw)
                        curveFieldRaw->SetKeys(keys);
                    if (auto* vc = world->GetComponent<Components::ValueCurve>(entity))
                    {
                        Components::ValueCurve updated = *vc;
                        if (preset.HasBuiltinEasing)
                            ApplyBuiltinEasingShape(updated, preset.BuiltinEasing);
                        else
                            ApplyKeysToShape(updated, keys);
                        setPlaybackIndicator(updated);
                    }

                    if (preset.HasBuiltinEasing)
                    {
                        const Math::TweenEasing easing = preset.BuiltinEasing;
                        CommitComponentWithUndo<Components::ValueCurve>(
                            world, entity, notifications, undo, "Apply Curve Preset",
                            [easing](Components::ValueCurve& c) { ApplyBuiltinEasingShape(c, easing); });
                    }
                    else
                    {
                        CommitComponentWithUndo<Components::ValueCurve>(
                            world, entity, notifications, undo, "Apply Curve Preset",
                            [keys](Components::ValueCurve& c) { ApplyKeysToShape(c, keys); });
                    }
                });
            picker->SetOnRequestSave(
                [getWorld, entity, pickerRaw]()
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    const auto* c = world->GetComponent<Components::ValueCurve>(entity);
                    if (!c) return;
                    if (c->ShapeMode == Components::ValueCurveShapeMode::CubicBezier)
                    {
                        CurvePresets::UserPresetLibrary::Get().Add(
                            CurvePresets::NextUserPresetName(), CubicBezierPresetFromCurve(*c));
                    }
                    else
                    {
                        CurvePresets::CurvePreset preset;
                        preset.Name = CurvePresets::NextUserPresetName();
                        preset.Kind = CurvePresets::CurvePresetKind::KeyCurve;
                        preset.HasBuiltinEasing = c->UseBuiltinEasing;
                        preset.BuiltinEasing = c->BuiltinEasing;
                        for (uint8 i = 0; i < c->Shape.KeyCount; ++i)
                            preset.Curve.Keys.push_back(c->Shape.Keys[i]);
                        CurvePresets::UserPresetLibrary::Get().Add(std::move(preset));
                    }
                    if (pickerRaw)
                        pickerRaw->Refresh();
                });
            presetContent->AddChild(std::move(picker));
            ctx.Parent->AddChild(std::move(presetFoldout));

            shapeModeDropdown->SetOnValueChanged(
                [getWorld, entity, notifications, undo, curveFieldRaw, bezierFieldRaw, suppressShapeModeChange, setPlaybackIndicator](
                    const std::string& value)
                {
                    if (suppressShapeModeChange && *suppressShapeModeChange)
                        return;

                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;

                    int modeValue = 0;
                    try { modeValue = std::stoi(value); }
                    catch (...) { return; }
                    const auto mode = modeValue == static_cast<int>(Components::ValueCurveShapeMode::CubicBezier)
                        ? Components::ValueCurveShapeMode::CubicBezier
                        : Components::ValueCurveShapeMode::KeyCurve;
                    const bool showBezier = mode == Components::ValueCurveShapeMode::CubicBezier;

                    if (curveFieldRaw)
                    {
                        if (showBezier)
                            curveFieldRaw->AddClass("hidden");
                        else
                            curveFieldRaw->RemoveClass("hidden");
                    }
                    if (bezierFieldRaw)
                    {
                        if (showBezier)
                            bezierFieldRaw->RemoveClass("hidden");
                        else
                            bezierFieldRaw->AddClass("hidden");
                    }

                    if (auto* vc = world->GetComponent<Components::ValueCurve>(entity))
                    {
                        Components::ValueCurve updated = *vc;
                        ApplyShapeModeChange(updated, mode);
                        if (showBezier && bezierFieldRaw)
                            bezierFieldRaw->SetShape(CubicBezierShapeFromCurve(updated));
                        else if (!showBezier && curveFieldRaw)
                            curveFieldRaw->SetKeys(ShapeKeys(updated));
                        setPlaybackIndicator(updated);
                    }

                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Change Value Curve Shape Mode",
                        [mode](Components::ValueCurve& c) { ApplyShapeModeChange(c, mode); });
                });

            AddToggleRow(
                ctx.Parent, "Apply To Target", curve->ApplyToTarget,
                [getWorld, entity, notifications, undo](bool value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Toggle Value Curve Target Apply",
                        [value](Components::ValueCurve& c) { c.ApplyToTarget = value; });
                },
                "Write the evaluated value into the selected target property");

            static const std::vector<Dropdown::Option> kTargetOptions = BuildTargetOptions();
            Dropdown* targetDropdown = InspectorUI::AddDropdownRow(
                ctx.Parent, "Target", kTargetOptions, static_cast<int>(curve->Target),
                "When Apply To Target is on, evaluated output drives this scene property "
                "(Start/End Value still scale how much it applies)");
            targetDropdown->SetOnValueChanged(
                [getWorld, entity, notifications, undo](const std::string& value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;

                    int target = 0;
                    try { target = std::stoi(value); }
                    catch (...) { return; }

                    CommitComponentWithUndo<Components::ValueCurve>(
                        world, entity, notifications, undo, "Change Value Curve Target",
                        [target](Components::ValueCurve& c)
                        {
                            constexpr int kMinTarget = static_cast<int>(Components::ValueCurveTarget::None);
                            constexpr int kMaxTarget =
                                static_cast<int>(Components::ValueCurveTarget::PostProcessWeight);
                            if (target < kMinTarget || target > kMaxTarget)
                                return;
                            c.Target = static_cast<Components::ValueCurveTarget>(target);
                        });
                });
        });
}

} // namespace GameEngine
