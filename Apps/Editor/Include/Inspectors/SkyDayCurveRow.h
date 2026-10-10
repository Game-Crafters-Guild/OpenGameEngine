#pragma once

#include "Components/Rendering/SkyEnvironment.h"
#include "Mathematics/Curve.h"
#include "UI/Controls/CurveField.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
class UIElement;
namespace ECS { class World; struct EntityHandle; }
namespace Editor { class EditorChangeNotifications; class UndoRedoService; }
namespace Platform { class Window; }

// A time of day wrapped into [0, 24] hours, with 24:00 kept as the end of the day.
float WrapSkyTimeOfDayHours(float rawHours);

// What a day curve row offers beyond its keys.
struct SkyDayCurveRowOptions
{
    // The curve's Cubic Bezier alternative, chosen with a Key Curve / Cubic Bezier dropdown; both null
    // for a curve that is keys only.
    Components::SkyScalarCurveShapeMode Components::SkyEnvironment::* ModeMember = nullptr;
    Components::SkyScalarCubicBezier Components::SkyEnvironment::* BezierMember = nullptr;
    // Above 0, the keys are drawn and evaluated in log2 of their value above this floor
    // (Math::EvaluateCurveKeysLogarithmic): a curve whose values span orders of magnitude.
    float LogarithmicFloor = 0.0f;
    // The hours under the graph, the marks on the value axis and the label they and the playhead's
    // value carry (CurveField::Config).
    CurveField::TimeLabels TimeAxisLabels = CurveField::TimeLabels::None;
    std::vector<float> ValueAxisMarks;
    std::function<std::string(float)> ValueLabel;
    // A class on the graph beside sky-day-curve-graph, for a graph laid out differently (inspector.css).
    const char* GraphClass = nullptr;
    // The unit after the selected key's value in the readout, empty for none.
    std::string ValueSuffix;
    // The preset foldout: shapes remapped linearly over the value range.
    bool Presets = true;
    // The preset foldout under the row at the panel's width instead of inside the field.
    bool PresetsFullWidth = false;
};

// One of the sky's day curves as an inspector row: the shared CurveField key editor, or the shared
// CubicBezierField handle editor when `options` gives the curve one, plus presets. Keys live in hours
// [0, 24) and wrap across midnight; Bezier X handles are normalized over the day and Y handles are in
// the scalar's value units. Multi-selection mirrors the primary edit to every entity in `extras`.
// Returns the key graph, or null when the row could not be built.
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
    std::shared_ptr<bool> curvePlaybackScrubActive);

} // namespace GameEngine
