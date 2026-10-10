#include "Inspectors/SkyDayCurveRow.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/World.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Mathematics/Interpolation.h"
#include "UI/Controls/CubicBezierField.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/CurvePresetPicker.h"
#include "UI/Controls/CurvePresets.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Foldout.h"
#include "UI/EditorIcons.h"
#include "UI/StyleProperties.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_map>

namespace GameEngine
{

namespace
{

static std::unordered_map<std::string, bool>& SkyScalarCurveExpandedStates()
{
    static std::unordered_map<std::string, bool> states;
    return states;
}

static bool IsSkyScalarCurveExpanded(const std::string& key)
{
    const auto& states = SkyScalarCurveExpandedStates();
    const auto it = states.find(key);
    return it != states.end() && it->second;
}

static void SetSkyScalarCurveExpanded(const std::string& key, bool expanded)
{
    SkyScalarCurveExpandedStates()[key] = expanded;
}

static std::unordered_map<std::string, bool>& SkyScalarCurvePresetExpandedStates()
{
    static std::unordered_map<std::string, bool> states;
    return states;
}

static bool IsSkyScalarCurvePresetExpanded(const std::string& key)
{
    const auto& states = SkyScalarCurvePresetExpandedStates();
    const auto it = states.find(key);
    return it != states.end() && it->second;
}

static void SetSkyScalarCurvePresetExpanded(const std::string& key, bool expanded)
{
    SkyScalarCurvePresetExpandedStates()[key] = expanded;
}

// A curve surface (the key graph or the Bezier editor) at its compact height, or filling the row's
// width at the expanded aspect (inspector.css, .sky-day-curve-graph-expanded).
static void SetSkyDayCurveSurfaceExpanded(UIElement* curveSurface, bool expanded)
{
    if (!curveSurface)
        return;
    if (expanded)
        curveSurface->AddClass("sky-day-curve-graph-expanded");
    else
        curveSurface->RemoveClass("sky-day-curve-graph-expanded");
}

// With a key selected the Hour and Value fields edit it; with none they read the curve at the playhead
// and take no input.
static void SetKeyFieldsEditable(FloatField* timeField, FloatField* valueField, bool editable)
{
    for (FloatField* field : {timeField, valueField})
    {
        if (field && field->IsEnabled() != editable)
            InspectorUI::SetRowOfControlEnabled(field, editable);
    }
}

static std::vector<Dropdown::Option> BuildSkyScalarCurveShapeModeOptions()
{
    return {
        {"0", "Key Curve"},
        {"1", "Cubic Bezier"},
    };
}

static CubicBezierShape CubicBezierShapeFromSkyBezier(const Components::SkyScalarCubicBezier& bezier)
{
    return {
        std::clamp(bezier.Control1X, 0.0f, 1.0f),
        bezier.Control1Y,
        std::clamp(bezier.Control2X, 0.0f, 1.0f),
        bezier.Control2Y,
        bezier.AnchorStartY,
        bezier.AnchorEndY,
    };
}

static Components::SkyScalarCubicBezier SkyBezierFromCubicBezierShape(const CubicBezierShape& shape)
{
    return {
        std::clamp(shape.C1X, 0.0f, 1.0f),
        shape.C1Y,
        std::clamp(shape.C2X, 0.0f, 1.0f),
        shape.C2Y,
        shape.AnchorStartY,
        shape.AnchorEndY,
    };
}

static CubicBezierShape SkyBezierShapeFromPreset(const CurvePresets::CubicBezierPreset& preset,
                                                 float displayMin,
                                                 float displayMax)
{
    auto remap = [displayMin, displayMax](float v) {
        return Math::Lerp(displayMin, displayMax, Math::Clamp01(v));
    };
    return {
        std::clamp(preset.Control1X, 0.0f, 1.0f),
        remap(preset.Control1Y),
        std::clamp(preset.Control2X, 0.0f, 1.0f),
        remap(preset.Control2Y),
        remap(preset.AnchorStartY),
        remap(preset.AnchorEndY),
    };
}

static CurvePresets::CubicBezierPreset PresetFromSkyBezierShape(const CubicBezierShape& shape,
                                                                float displayMin,
                                                                float displayMax)
{
    auto normalize = [displayMin, displayMax](float v) {
        return Math::Clamp01(Math::InverseLerp(displayMin, displayMax, v));
    };
    return {
        std::clamp(shape.C1X, 0.0f, 1.0f),
        normalize(shape.C1Y),
        std::clamp(shape.C2X, 0.0f, 1.0f),
        normalize(shape.C2Y),
        normalize(shape.AnchorStartY),
        normalize(shape.AnchorEndY),
    };
}

static Components::SkyScalarCubicBezier SeedSkyBezierFromCurve(const Math::Curve& curve)
{
    CubicBezierShape shape;
    shape.C1X = 1.0f / 3.0f;
    shape.C2X = 2.0f / 3.0f;
    shape.AnchorStartY = Components::EvaluateSkyDayCurve(curve, 0.0f);
    shape.AnchorEndY = Components::EvaluateSkyDayCurve(curve, 24.0f - 0.001f);
    shape.C1Y = Components::EvaluateSkyDayCurve(curve, 8.0f);
    shape.C2Y = Components::EvaluateSkyDayCurve(curve, 16.0f);
    return SkyBezierFromCubicBezierShape(shape);
}

static std::vector<Math::CurveKey> SampleSkyBezierKeys(const Components::SkyScalarCubicBezier& bezier,
                                                       const std::function<float(float)>& clampValue,
                                                       int sampleCount = 9)
{
    std::vector<Math::CurveKey> keys;
    sampleCount = std::clamp(sampleCount, 2, static_cast<int>(Math::Curve::Capacity));
    keys.reserve(static_cast<size_t>(sampleCount));
    const CubicBezierShape shape = CubicBezierShapeFromSkyBezier(bezier);
    for (int i = 0; i < sampleCount; ++i)
    {
        const float t01 = static_cast<float>(i) / static_cast<float>(sampleCount - 1);
        Math::CurveKey key;
        key.Time = Math::Lerp(0.0f, 24.0f, t01);
        key.Value = clampValue(CubicBezierField::EvaluateShape(shape, t01));
        key.Interp = Math::CurveInterp::Linear;
        keys.push_back(key);
    }
    return keys;
}

} // namespace

float WrapSkyTimeOfDayHours(float rawHours)
{
    constexpr float kDayHours = 24.0f;
    constexpr float kEndpointEpsilon = 0.0001f;
    const float clamped = std::max(0.0f, rawHours);
    if (clamped <= kDayHours)
        return (clamped >= (kDayHours - kEndpointEpsilon)) ? kDayHours : clamped;
    return std::fmod(clamped, kDayHours);
}

CurveField* AddSkyDayCurveRow(
    UIElement* parent,
    const std::string& label,
    Math::Curve Components::SkyEnvironment::* member,
    const SkyDayCurveRowOptions& options,
    ECS::World* w,
    ECS::EntityHandle e,
    Editor::EditorChangeNotifications* n,
    Editor::UndoRedoService* undo,
    const std::string& changeName,
    std::function<float(float)> clampValue,
    float displayMin,
    float displayMax,
    const char* tooltip,
    const std::vector<ECS::EntityHandle>& extras,
    Platform::Window* window,
    std::vector<std::function<void()>>* frameRefresh,
    std::vector<std::function<void()>>* simRefresh,
    std::function<void()> refreshTimeOfDayControls,
    std::shared_ptr<bool> curvePlaybackScrubActive)
{
    if (!parent || !w)
        return nullptr;

    auto* sky = w->GetComponent<Components::SkyEnvironment>(e);
    if (!sky)
        return nullptr;

    const auto modeMember = options.ModeMember;
    const auto bezierMember = options.BezierMember;
    const bool hasBezier = modeMember && bezierMember;
    const float logarithmicFloor = options.LogarithmicFloor;

    UIElement* row = InspectorUI::AddRow(parent);
    // The field stacks a tall graph + readout + preset foldout; the row's class keeps the label pinned
    // to the top instead of vertically centered against that full height.
    row->AddClass("sky-day-curve-row");
    Label* rowLabel = InspectorUI::AddLabel(row, label, tooltip);
    if (rowLabel)
        rowLabel->AddClass("inspector-label-no-drag");

    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->AddClass("sky-day-curve-field");

    auto extrasShared = std::make_shared<std::vector<ECS::EntityHandle>>(extras);
    auto expandedState = std::make_shared<bool>(IsSkyScalarCurveExpanded(changeName));
    auto presetExpandedState = std::make_shared<bool>(IsSkyScalarCurvePresetExpanded(changeName));
    const bool initialBezierMode =
        hasBezier && (sky->*modeMember) == Components::SkyScalarCurveShapeMode::CubicBezier;

    Dropdown* modeDropdownRaw = nullptr;
    if (hasBezier)
    {
        auto modeDropdown = std::make_unique<Dropdown>();
        static const std::vector<Dropdown::Option> kShapeModeOptions = BuildSkyScalarCurveShapeModeOptions();
        modeDropdown->SetOptions(kShapeModeOptions, initialBezierMode ? 1 : 0);
        modeDropdown->AddClass("inspector-dropdown");
        modeDropdownRaw = modeDropdown.get();
        fieldContainer->AddChild(std::move(modeDropdown));
    }
    auto suppressModeChange = std::make_shared<bool>(false);

    auto bezierModeActive = std::make_shared<bool>(initialBezierMode);

    // The curve graph: keys live in hours [0, 24) and wrap across midnight. Drag a key in
    // time + value; right-click empty graph space to add a key, or right-click a key for its menu.
    auto graph = std::make_unique<CurveField>();
    CurveField* graphRaw = graph.get();
    {
        CurveField::Config cfg;
        cfg.TimeMin = 0.0f;
        cfg.TimeMax = 24.0f;
        cfg.ValueMin = displayMin;
        cfg.ValueMax = displayMax;
        cfg.AllowTimeDrag = true;
        cfg.AllowAddRemove = true;
        cfg.WrapAround = true;
        cfg.ShowPlaybackIndicator = true;
        cfg.AllowPlaybackScrub = true;
        cfg.MinKeys = 2;
        cfg.MaxKeys = Math::Curve::Capacity;
        cfg.LogarithmicValues = logarithmicFloor > 0.0f;
        cfg.LogarithmicFloor = logarithmicFloor;
        cfg.TimeAxisLabels = options.TimeAxisLabels;
        cfg.ValueAxisMarks = options.ValueAxisMarks;
        cfg.ValueLabel = options.ValueLabel;
        graph->SetConfig(cfg);

        const Math::Curve& curve = sky->*member;
        graph->SetKeys(std::vector<Math::CurveKey>(curve.Keys, curve.Keys + curve.KeyCount));
    }
    graph->AddClass("sky-day-curve-graph");
    if (options.GraphClass)
        graph->AddClass(options.GraphClass);
    if (initialBezierMode)
        graph->AddClass("hidden");
    fieldContainer->AddChild(std::move(graph));

    CubicBezierField* bezierRaw = nullptr;
    if (hasBezier)
    {
        auto bezier = std::make_unique<CubicBezierField>();
        bezierRaw = bezier.get();
        CubicBezierField::Config cfg;
        cfg.TimeMin = 0.0f;
        cfg.TimeMax = 24.0f;
        cfg.ValueMin = displayMin;
        cfg.ValueMax = displayMax;
        cfg.ShowPlaybackIndicator = true;
        cfg.AllowPlaybackScrub = true;
        bezier->SetConfig(cfg);
        bezier->SetShape(CubicBezierShapeFromSkyBezier(sky->*bezierMember));
        bezier->AddClass("sky-day-curve-graph");
        if (!initialBezierMode)
            bezier->AddClass("hidden");
        fieldContainer->AddChild(std::move(bezier));
    }

    // The value the curve gives at an hour, through the evaluator the runtime uses for it.
    auto evaluateAtTimeOfDay = [member, modeMember, bezierMember, logarithmicFloor](const Components::SkyEnvironment& env,
                                                                                   float hours) {
        if (modeMember && bezierMember)
            return Components::EvaluateSkyScalarDayCurve(env.*member, env.*modeMember, env.*bezierMember, hours);
        if (logarithmicFloor > 0.0f)
            return Math::EvaluateCurveKeysLogarithmic((env.*member).Keys, (env.*member).KeyCount, hours,
                                                      logarithmicFloor, 24.0f);
        return Components::EvaluateSkyDayCurve(env.*member, hours);
    };
    auto setTimelineIndicator = [graphRaw, bezierRaw, evaluateAtTimeOfDay](const Components::SkyEnvironment& env) {
        const float hours = WrapSkyTimeOfDayHours(env.TimeOfDayHours);
        const float value = evaluateAtTimeOfDay(env, hours);
        graphRaw->SetPlaybackIndicator(hours, value, true);
        if (bezierRaw)
            bezierRaw->SetPlaybackIndicator(hours, value, true);
    };
    setTimelineIndicator(*sky);

    // Write a whole key set into the curve selected by `member`, clamping each value.
    auto applyKeys = [member, modeMember, clampValue](Components::SkyEnvironment& u, const std::vector<Math::CurveKey>& keys) {
        if (modeMember)
            u.*modeMember = Components::SkyScalarCurveShapeMode::KeyCurve;
        Math::Curve& c = u.*member;
        const uint8_t count = static_cast<uint8_t>(std::min<size_t>(keys.size(), Math::Curve::Capacity));
        c.KeyCount = count;
        for (uint8_t i = 0; i < count; ++i)
        {
            c.Keys[i] = keys[i];
            c.Keys[i].Value = clampValue(c.Keys[i].Value);
        }
    };

    auto applyBezier = [member, modeMember, bezierMember, clampValue](
                           Components::SkyEnvironment& u,
                           const Components::SkyScalarCubicBezier& bezierShape) {
        Components::SkyScalarCubicBezier clamped = bezierShape;
        clamped.Control1X = std::clamp(clamped.Control1X, 0.0f, 1.0f);
        clamped.Control2X = std::clamp(clamped.Control2X, 0.0f, 1.0f);
        clamped.Control1Y = clampValue(clamped.Control1Y);
        clamped.Control2Y = clampValue(clamped.Control2Y);
        clamped.AnchorStartY = clampValue(clamped.AnchorStartY);
        clamped.AnchorEndY = clampValue(clamped.AnchorEndY);
        u.*modeMember = Components::SkyScalarCurveShapeMode::CubicBezier;
        u.*bezierMember = clamped;

        Math::Curve& c = u.*member;
        c.KeyCount = 0;
        const auto sampled = SampleSkyBezierKeys(clamped, clampValue);
        for (const Math::CurveKey& key : sampled)
            c.TryInsert(key);
    };

    using OptEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;
    auto editPtr = std::make_shared<OptEdit>();

    using SkyApplyFn = std::function<void(Components::SkyEnvironment&)>;

    auto previewApply = [w, e, n, undo, changeName, editPtr, extrasShared, setTimelineIndicator](SkyApplyFn apply) {
        auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
        if (!comp)
            return;

        if (!editPtr->has_value() && undo)
        {
            auto target = extrasShared->empty()
                ? InspectorDrag::MakeComponentSnapshotTarget<Components::SkyEnvironment>(w, e, n, changeName)
                : InspectorDrag::MakeMultiComponentSnapshotTarget<Components::SkyEnvironment>(w, e, *extrasShared, n, changeName);
            editPtr->emplace(undo->BeginInteractiveEdit(changeName, std::move(target)));
        }

        Components::SkyEnvironment updated = *comp;
        apply(updated);
        setTimelineIndicator(updated);

        if (editPtr->has_value() && editPtr->value())
        {
            Components::SkyEnvironment captured = updated;
            editPtr->value().Preview([w, e, captured, extrasShared, apply] {
                w->AddComponentImmediate(e, captured);
                for (auto& ex : *extrasShared)
                {
                    auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                    if (!c)
                        continue;
                    Components::SkyEnvironment u = *c;
                    apply(u);
                    w->AddComponentImmediate(ex, u);
                }
            });
            if (n)
            {
                n->NotifyComponentChange<Components::SkyEnvironment>(w, e, Editor::EditorChangeNotifications::ChangeKind::Preview);
                for (auto& ex : *extrasShared)
                    n->NotifyComponentChange<Components::SkyEnvironment>(w, ex, Editor::EditorChangeNotifications::ChangeKind::Preview);
            }
        }
        else
        {
            Editor::PreviewComponentUpdate(w, e, n, updated);
            for (auto& ex : *extrasShared)
            {
                auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                if (!c)
                    continue;
                Components::SkyEnvironment u = *c;
                apply(u);
                Editor::PreviewComponentUpdate(w, ex, n, u);
            }
        }
    };

    auto commitApply = [w, e, n, undo, changeName, editPtr, extrasShared, setTimelineIndicator](SkyApplyFn apply) {
        auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
        if (!comp)
            return;

        Components::SkyEnvironment updated = *comp;
        apply(updated);
        setTimelineIndicator(updated);

        if (editPtr->has_value() && editPtr->value())
        {
            w->AddComponentImmediate(e, updated);
            for (auto& ex : *extrasShared)
            {
                auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                if (!c)
                    continue;
                Components::SkyEnvironment u = *c;
                apply(u);
                w->AddComponentImmediate(ex, u);
            }
            editPtr->value().Commit();
            if (n)
            {
                n->NotifyComponentCommit<Components::SkyEnvironment>(w, e);
                for (auto& ex : *extrasShared)
                    n->NotifyComponentCommit<Components::SkyEnvironment>(w, ex);
            }
            editPtr->reset();
        }
        else
        {
            InspectorDrag::CommitComponentWithUndo<Components::SkyEnvironment>(w, e, n, undo, changeName,
                [updated](Components::SkyEnvironment& u) { u = updated; });
            for (auto& ex : *extrasShared)
            {
                auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                if (!c)
                    continue;
                Components::SkyEnvironment u = *c;
                apply(u);
                Editor::CommitComponentUpdate(w, ex, n, u);
            }
        }
    };

    auto preview = [previewApply, applyKeys](const std::vector<Math::CurveKey>& keys) {
        previewApply([applyKeys, keys](Components::SkyEnvironment& u) { applyKeys(u, keys); });
    };

    auto commit = [commitApply, applyKeys](const std::vector<Math::CurveKey>& keys) {
        commitApply([applyKeys, keys](Components::SkyEnvironment& u) { applyKeys(u, keys); });
    };

    auto previewBezier = [previewApply, applyBezier](const CubicBezierShape& shape) {
        const Components::SkyScalarCubicBezier bezierShape = SkyBezierFromCubicBezierShape(shape);
        previewApply([applyBezier, bezierShape](Components::SkyEnvironment& u) { applyBezier(u, bezierShape); });
    };

    auto commitBezier = [commitApply, applyBezier](const CubicBezierShape& shape) {
        const Components::SkyScalarCubicBezier bezierShape = SkyBezierFromCubicBezierShape(shape);
        commitApply([applyBezier, bezierShape](Components::SkyEnvironment& u) { applyBezier(u, bezierShape); });
    };

    // Default inspector rows for the selected key. These intentionally use the shared
    // AddFloatRowWithDrag helper so the labels drag, double-click reset, and style exactly like
    // normal scalar inspector fields.
    auto readout = std::make_unique<UIElement>();
    readout->AddClass("sky-scalar-curve-readout");
    UIElement* readoutRaw = readout.get();
    fieldContainer->AddChild(std::move(readout));

    auto syncReadoutFn = std::make_shared<std::function<void()>>();
    auto previewSelectedKeyTime = [graphRaw, preview, bezierModeActive](float t) {
        if (bezierModeActive && *bezierModeActive)
            return;
        if (graphRaw->GetSelectedKey() < 0)
            return;
        graphRaw->SetSelectedKeyTime(t);
        preview(graphRaw->GetKeys());
    };
    auto commitSelectedKeyTime = [graphRaw, commit, syncReadoutFn, bezierModeActive](float t) {
        if (bezierModeActive && *bezierModeActive)
            return;
        if (graphRaw->GetSelectedKey() < 0)
            return;
        graphRaw->SetSelectedKeyTime(t);
        commit(graphRaw->GetKeys());
        if (syncReadoutFn && *syncReadoutFn)
            (*syncReadoutFn)();
    };
    auto previewSelectedKeyValue = [graphRaw, preview, clampValue, bezierModeActive](float v) {
        if (bezierModeActive && *bezierModeActive)
            return;
        if (graphRaw->GetSelectedKey() < 0)
            return;
        graphRaw->SetSelectedKeyValue(clampValue(v));
        preview(graphRaw->GetKeys());
    };
    auto commitSelectedKeyValue = [graphRaw, commit, clampValue, syncReadoutFn, bezierModeActive](float v) {
        if (bezierModeActive && *bezierModeActive)
            return;
        if (graphRaw->GetSelectedKey() < 0)
            return;
        graphRaw->SetSelectedKeyValue(clampValue(v));
        commit(graphRaw->GetKeys());
        if (syncReadoutFn && *syncReadoutFn)
            (*syncReadoutFn)();
    };

    FloatField* timeField = InspectorDrag::AddFloatRowWithDrag(
        readoutRaw, "Hour", 0.0f, previewSelectedKeyTime, commitSelectedKeyTime,
        0.0f, "Selected key's time of day in hours.", 0.0f, 24.0f);
    FloatField* valueField = InspectorDrag::AddFloatRowWithDrag(
        readoutRaw, "Value", 0.0f, previewSelectedKeyValue, commitSelectedKeyValue,
        displayMin, tooltip ? tooltip : "Selected key's value.");
    if (!options.ValueSuffix.empty())
        valueField->SetSuffix(options.ValueSuffix);

    // Keep the readout fields showing the selected key (silent SetValue: no callback loop).
    auto syncReadout = [graphRaw, timeField, valueField, bezierModeActive]() {
        if (bezierModeActive && *bezierModeActive)
            return;
        const int sel = graphRaw->GetSelectedKey();
        const auto& keys = graphRaw->GetKeys();
        if (sel >= 0 && sel < static_cast<int>(keys.size()))
        {
            SetKeyFieldsEditable(timeField, valueField, true);
            timeField->SetValueWithoutNotify(keys[sel].Time);
            valueField->SetValueWithoutNotify(keys[sel].Value);
        }
    };
    *syncReadoutFn = syncReadout;

    auto syncTimelineReadout = [evaluateAtTimeOfDay, timeField, valueField, graphRaw, bezierModeActive](
                                   const Components::SkyEnvironment& env) {
        if (bezierModeActive && !*bezierModeActive && graphRaw->GetSelectedKey() >= 0)
            return;
        SetKeyFieldsEditable(timeField, valueField, false);
        const float hours = WrapSkyTimeOfDayHours(env.TimeOfDayHours);
        timeField->SetValueWithoutNotify(hours);
        valueField->SetValueWithoutNotify(evaluateAtTimeOfDay(env, hours));
    };
    syncTimelineReadout(*sky);

    // Drive the active timeline indicator and readout fields from the live time of day each
    // frame. While a curve is actively scrubbing time, the edited curve updates itself directly;
    // other curve playheads stay put until the drag ends.
    // InspectorPanel clears refresh callbacks on rebuild, so captured widgets stay valid.
    auto refreshCurveTimeline = [setTimelineIndicator, syncTimelineReadout, w, e, curvePlaybackScrubActive]() {
        if (curvePlaybackScrubActive && *curvePlaybackScrubActive)
            return;
        if (!w || !e.IsValid() || !w->IsValid(e))
            return;
        if (const auto* s = w->GetComponent<Components::SkyEnvironment>(e))
        {
            setTimelineIndicator(*s);
            syncTimelineReadout(*s);
        }
    };
    if (frameRefresh)
        frameRefresh->push_back(refreshCurveTimeline);
    else if (simRefresh)
        simRefresh->push_back(refreshCurveTimeline);

    auto scrubTimeOfDay = [
        w,
        e,
        n,
        extrasShared,
        setTimelineIndicator,
        syncTimelineReadout,
        refreshTimeOfDayControls](float rawHours) {
        if (!w || !e.IsValid() || !w->IsValid(e))
            return;
        auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
        if (!comp)
            return;

        const float hours = WrapSkyTimeOfDayHours(rawHours);
        Components::SkyEnvironment updated = *comp;
        updated.TimeOfDayHours = hours;
        setTimelineIndicator(updated);
        syncTimelineReadout(updated);
        Editor::PreviewComponentUpdate(w, e, n, updated);

        for (auto& ex : *extrasShared)
        {
            auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
            if (!c)
                continue;
            Components::SkyEnvironment u = *c;
            u.TimeOfDayHours = hours;
            Editor::PreviewComponentUpdate(w, ex, n, u);
        }
        if (refreshTimeOfDayControls)
            refreshTimeOfDayControls();
    };
    graphRaw->SetOnPlaybackScrub(scrubTimeOfDay);
    if (bezierRaw)
        bezierRaw->SetOnPlaybackScrub(scrubTimeOfDay);
    auto setCurvePlaybackScrubActive = [curvePlaybackScrubActive](bool active) {
        if (curvePlaybackScrubActive)
            *curvePlaybackScrubActive = active;
    };
    graphRaw->SetOnPlaybackScrubActiveChanged(setCurvePlaybackScrubActive);
    if (bezierRaw)
        bezierRaw->SetOnPlaybackScrubActiveChanged(setCurvePlaybackScrubActive);

    graphRaw->SetOnChanging([preview, syncReadout](const std::vector<Math::CurveKey>& keys) {
        preview(keys);
        syncReadout();
    });
    graphRaw->SetOnChanged([commit, syncReadout](const std::vector<Math::CurveKey>& keys) {
        commit(keys);
        syncReadout();
    });
    graphRaw->SetOnSelectionChanged([syncReadout](int) { syncReadout(); });
    graphRaw->SetOnKeyContextMenu([graphRaw, syncReadout, window](int keyIndex, float x, float y) {
        if (!window || !graphRaw)
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

        const auto& keys = graphRaw->GetKeys();
        if (keyIndex < 0 || keyIndex >= static_cast<int>(keys.size()))
            return;

        const bool canDelete = keys.size() > 2;
        const Math::CurveKey selectedKey = keys[static_cast<size_t>(keyIndex)];
        // Interpolation mode is exactly-one-of.
        auto checked = [](bool value) -> uint32_t {
            return MenuItemFlag_Radio | (value ? MenuItemFlag_Checked : MenuItemFlag_None);
        };

        s_Menu->Clear();
        s_Menu->SetCommandHandler([graphRaw, syncReadout, keyIndex](uint32_t cmd) {
            if (!graphRaw)
                return;

            switch (cmd)
            {
            case kCmdLinear:
                graphRaw->SetKeyMode(keyIndex, Math::CurveInterp::Linear, Math::CurveTangentMode::Auto);
                break;
            case kCmdSmoothAuto:
                graphRaw->SetKeyMode(keyIndex, Math::CurveInterp::Smooth, Math::CurveTangentMode::Auto);
                break;
            case kCmdSmoothFlat:
                graphRaw->SetKeyMode(keyIndex, Math::CurveInterp::Smooth, Math::CurveTangentMode::Flat);
                break;
            case kCmdStep:
                graphRaw->SetKeyMode(keyIndex, Math::CurveInterp::Step, Math::CurveTangentMode::Auto);
                break;
            case kCmdDelete:
                graphRaw->DeleteKey(keyIndex);
                break;
            default:
                break;
            }
            syncReadout();
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

    if (bezierRaw)
    {
        bezierRaw->SetOnChanging([previewBezier](const CubicBezierShape& shape) {
            previewBezier(shape);
        });
        bezierRaw->SetOnChanged([commitBezier](const CubicBezierShape& shape) {
            commitBezier(shape);
        });
    }

    auto setModeVisible = [graphRaw, bezierRaw, bezierModeActive](bool showBezier) {
        if (!bezierRaw)
            return;
        if (bezierModeActive)
            *bezierModeActive = showBezier;
        if (showBezier)
        {
            graphRaw->AddClass("hidden");
            bezierRaw->RemoveClass("hidden");
        }
        else
        {
            graphRaw->RemoveClass("hidden");
            bezierRaw->AddClass("hidden");
        }
    };

    if (modeDropdownRaw)
    {
        modeDropdownRaw->SetOnValueChanged(
            [w, e, member, modeMember, bezierMember, graphRaw, bezierRaw, syncReadout, commit, commitBezier,
             clampValue, setModeVisible, suppressModeChange](const std::string& value) {
                if (suppressModeChange && *suppressModeChange)
                    return;
                if (!w)
                    return;
                int modeValue = 0;
                try { modeValue = std::stoi(value); }
                catch (...) { return; }
                const bool showBezier = modeValue == static_cast<int>(Components::SkyScalarCurveShapeMode::CubicBezier);
                setModeVisible(showBezier);

                auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
                if (!comp)
                    return;

                if (showBezier)
                {
                    const Components::SkyScalarCubicBezier next =
                        (comp->*modeMember == Components::SkyScalarCurveShapeMode::CubicBezier)
                            ? comp->*bezierMember
                            : SeedSkyBezierFromCurve(comp->*member);
                    const CubicBezierShape shape = CubicBezierShapeFromSkyBezier(next);
                    bezierRaw->SetShape(shape);
                    commitBezier(shape);
                }
                else
                {
                    std::vector<Math::CurveKey> keys;
                    if (comp->*modeMember == Components::SkyScalarCurveShapeMode::CubicBezier)
                        keys = SampleSkyBezierKeys(comp->*bezierMember, clampValue);
                    else
                    {
                        const Math::Curve& curve = comp->*member;
                        keys.assign(curve.Keys, curve.Keys + curve.KeyCount);
                    }
                    graphRaw->SetKeys(keys);
                    commit(graphRaw->GetKeys());
                    syncReadout();
                }
            });
    }

    // Preset foldout: apply a curated/saved shape (remapped from normalized [0,1] into the curve's
    // hours x value range) or save the current curve back to the shared library (normalized to
    // [0,1]^2 so the same shape is reusable on any curve -- sky or ValueCurve).
    if (options.Presets)
    {
        auto presetFoldout = std::make_unique<Foldout>();
        presetFoldout->SetTitle("Presets");
        presetFoldout->AddClass("rp-foldout");
        presetFoldout->AddClass("curve-preset-foldout");
        presetFoldout->SetExpanded(*presetExpandedState);
        presetFoldout->SetOnExpandedChanged([presetExpandedState, changeName](Foldout&, bool expanded) {
            *presetExpandedState = expanded;
            SetSkyScalarCurvePresetExpanded(changeName, expanded);
        });
        UIElement* presetContent = presetFoldout->GetContentContainer();

        auto picker = std::make_unique<CurvePresetPicker>();
        CurvePresetPicker* pickerRaw = picker.get();
        picker->SetContextMenuWindow(window);
        picker->SetOnPick([graphRaw, bezierRaw, modeDropdownRaw, commit, commitBezier, syncReadout, setModeVisible,
                           suppressModeChange, displayMin, displayMax](const CurvePresets::CurvePreset& preset) {
            const bool applyBezier = preset.Kind == CurvePresets::CurvePresetKind::CubicBezier;
            setModeVisible(applyBezier);
            if (modeDropdownRaw && suppressModeChange)
            {
                *suppressModeChange = true;
                modeDropdownRaw->SetSelectedIndex(applyBezier ? 1 : 0);
                *suppressModeChange = false;
            }

            if (applyBezier && bezierRaw)
            {
                const CubicBezierShape shape = SkyBezierShapeFromPreset(preset.Bezier, displayMin, displayMax);
                bezierRaw->SetShape(shape);
                commitBezier(shape);
                return;
            }

            const Math::DynamicCurve presetCurve = CurvePresets::PresetAsDynamicCurve(preset);
            std::vector<Math::CurveKey> remapped;
            remapped.reserve(presetCurve.Keys.size());
            for (const Math::CurveKey& k : presetCurve.Keys)
            {
                Math::CurveKey rk;
                rk.Time = Math::Lerp(0.0f, 24.0f, Math::Clamp01(k.Time));
                rk.Value = Math::Lerp(displayMin, displayMax, Math::Clamp01(k.Value));
                rk.Interp = Math::CurveInterp::Linear; // sky day-curves are linear
                remapped.push_back(rk);
            }
            graphRaw->SetKeys(remapped); // sorts its internal copy
            commit(graphRaw->GetKeys()); // commit the sorted copy so stored data matches the display
            syncReadout();
        });
        picker->SetOnRequestSave([w, e, modeMember, bezierMember, graphRaw, pickerRaw, displayMin, displayMax]() {
            if (w && modeMember && bezierMember)
            {
                if (const auto* c = w->GetComponent<Components::SkyEnvironment>(e))
                {
                    if (c->*modeMember == Components::SkyScalarCurveShapeMode::CubicBezier)
                    {
                        const CubicBezierShape shape = CubicBezierShapeFromSkyBezier(c->*bezierMember);
                        CurvePresets::UserPresetLibrary::Get().Add(
                            CurvePresets::NextUserPresetName(),
                            PresetFromSkyBezierShape(shape, displayMin, displayMax));
                        if (pickerRaw)
                            pickerRaw->Refresh();
                        return;
                    }
                }
            }

            // Normalize the curve back to [0, 1]^2 so the saved shape is reusable on any curve.
            Math::DynamicCurve shape;
            for (const Math::CurveKey& k : graphRaw->GetKeys())
            {
                Math::CurveKey nk;
                nk.Time = Math::Clamp01(Math::InverseLerp(0.0f, 24.0f, k.Time));
                nk.Value = Math::Clamp01(Math::InverseLerp(displayMin, displayMax, k.Value));
                nk.Interp = Math::CurveInterp::Linear;
                shape.Keys.push_back(nk);
            }
            CurvePresets::UserPresetLibrary::Get().Add(CurvePresets::NextUserPresetName(), shape);
            if (pickerRaw)
                pickerRaw->Refresh();
        });
        presetContent->AddChild(std::move(picker));
        if (options.PresetsFullWidth)
            parent->AddChild(std::move(presetFoldout));
        else
            fieldContainer->AddChild(std::move(presetFoldout));
    }

    auto applyExpandedState = [
        row,
        fieldContainer,
        graphRaw,
        bezierRaw,
        expandedState,
        changeName](bool expanded) {
        if (!row || !fieldContainer)
            return;

        *expandedState = expanded;
        SetSkyScalarCurveExpanded(changeName, expanded);

        // The row stacks its label above a full-width field while expanded (inspector.css,
        // .sky-scalar-curve-expanded).
        if (expanded)
            row->AddClass("sky-scalar-curve-expanded");
        else
            row->RemoveClass("sky-scalar-curve-expanded");
        SetSkyDayCurveSurfaceExpanded(graphRaw, expanded);
        SetSkyDayCurveSurfaceExpanded(bezierRaw, expanded);
    };
    applyExpandedState(*expandedState);

    auto toggleExpandedState = [expandedState, applyExpandedState]() {
        applyExpandedState(!*expandedState);
    };
    graphRaw->SetOnCurveDoubleClick(toggleExpandedState);
    if (bezierRaw)
        bezierRaw->SetOnCurveDoubleClick(toggleExpandedState);

    // No key starts selected: the readout shows the curve at the playhead and takes no input until a
    // click selects a key.
    return graphRaw;
}

} // namespace GameEngine
