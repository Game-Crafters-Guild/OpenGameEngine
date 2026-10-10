#include "SceneView/SplineToolMenu.h"

#include "Editor/Settings/SplineEditorSettings.h"
#include "UI/ContextMenuLabels.h"
#include "UI/EditorIcons.h"

#include <string>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

using Item = ContextMenuManipulator::Item;
using namespace ContextMenuLabels;

constexpr float kSceneSplineKnotSizePresets[] = {0.05f, 0.075f, 0.1f, 0.2f,
                                                 0.3f,  0.5f,   0.75f, 1.0f, 1.5f};
constexpr float kSceneSplineThicknessPresets[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f,
                                                  2.5f, 3.5f,  5.0f, 7.0f};
constexpr float kSceneSplineHandleSizePresets[] = {0.05f, 0.075f, 0.1f, 0.15f, 0.2f,
                                                   0.3f,  0.5f,   0.75f, 1.0f};
constexpr float kSceneSplineHandleThickPresets[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.5f};
constexpr float kSceneSplineKnotThickPresets[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.5f};
constexpr float kSceneSplineDefaultRadiusPresets[] = {0.25f, 0.5f, 1.0f, 2.0f,  3.0f,
                                                     5.0f,  10.0f, 20.0f, 50.0f};
constexpr float kSceneSplineSimplifyPresets[] = {0.0f, 0.10f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
constexpr int kSceneSplineSmoothPresets[] = {0, 1, 2, 3, 4};

} // namespace

std::vector<ContextMenuManipulator::Item> BuildSplineToolMenuItems(const OpenColorPickerWindowFn& openColorPicker)
{
    const auto& settings = SplineEditorSettings::Get();
    const bool isBezier = settings.GetCurveType() == SplineCurveType::CubicBezier;

    std::vector<Item> items;

    // Curve type
    items.push_back({.Path = "Curve Type", .IconPath = EditorIcons::kDrawCurve});
    const struct
    {
        const char* Label;
        const char* Icon;
        SplineCurveType Value;
    } kCurveTypes[] = {
        {"Curve Type/Catmull-Rom", EditorIcons::kSplineCurve, SplineCurveType::CatmullRom},
        {"Curve Type/Linear", EditorIcons::kLinearCurve, SplineCurveType::Linear},
        {"Curve Type/Cubic Bezier", EditorIcons::kEase, SplineCurveType::CubicBezier},
    };
    for (const auto& type : kCurveTypes)
    {
        items.push_back({.Path = type.Label,
                         .IconPath = type.Icon,
                         .Flags = CheckedFlag(settings.GetCurveType() == type.Value),
                         .OnActivate = [value = type.Value]
                         { SplineEditorSettings::Get().SetCurveType(value); }});
    }

    // Selection shape while the spline tool is active.
    items.push_back({.Path = "Selection Shape", .IconPath = EditorIcons::kPointer});
    items.push_back({.Path = "Selection Shape/Rectangle",
                     .IconPath = EditorIcons::kPointer,
                     .Flags = CheckedFlag(settings.GetSelectionShape() ==
                                          SplineSelectionShape::Rectangle),
                     .OnActivate = [] {
                         SplineEditorSettings::Get().SetSelectionShape(
                             SplineSelectionShape::Rectangle);
                     }});
    items.push_back({.Path = "Selection Shape/Free-form (Lasso)",
                     .IconPath = EditorIcons::kPointer,
                     .Flags = CheckedFlag(settings.GetSelectionShape() ==
                                          SplineSelectionShape::Lasso),
                     .OnActivate = [] {
                         SplineEditorSettings::Get().SetSelectionShape(
                             SplineSelectionShape::Lasso);
                     }});

    items.push_back({.Path = "Control Render Shape", .IconPath = EditorIcons::kBox});
    const struct
    {
        const char* Label;
        const char* Icon;
        SplineControlRenderShape Value;
    } kRenderShapes[] = {
        {"Control Render Shape/Rings", EditorIcons::kDot, SplineControlRenderShape::Ring},
        {"Control Render Shape/Spheres", EditorIcons::kSphere,
         SplineControlRenderShape::Sphere},
        {"Control Render Shape/Cubes", EditorIcons::kCube, SplineControlRenderShape::Cube},
    };
    for (const auto& shape : kRenderShapes)
    {
        items.push_back({.Path = shape.Label,
                         .IconPath = shape.Icon,
                         .Flags = CheckedFlag(settings.GetControlRenderShape() == shape.Value),
                         .OnActivate = [value = shape.Value]
                         { SplineEditorSettings::Get().SetControlRenderShape(value); }});
    }

    items.push_back({.Separator = true});

    const auto addFloatPresetSubmenu =
        [&items](const char* submenu, const char* icon, const auto& presets, float current,
                 void (*apply)(float))
    {
        items.push_back({.Path = submenu, .IconPath = icon});
        for (const float v : presets)
        {
            items.push_back({.Path = std::string(submenu) + "/" + FloatLabel(v),
                             .Flags = CheckedFlag(NearlyEqual(v, current)),
                             .OnActivate = [apply, v] { apply(v); }});
        }
    };

    addFloatPresetSubmenu("Knot Size", EditorIcons::kSplineCurve, kSceneSplineKnotSizePresets,
                          settings.GetKnotSize(),
                          [](float v) { SplineEditorSettings::Get().SetKnotSize(v); });
    addFloatPresetSubmenu("Knot Outline Thickness", EditorIcons::kSplineCurve,
                          kSceneSplineKnotThickPresets, settings.GetKnotThickness(), [](float v)
                          { SplineEditorSettings::Get().SetKnotThickness(v); });
    addFloatPresetSubmenu("Spline Thickness", EditorIcons::kSplineCurve,
                          kSceneSplineThicknessPresets, settings.GetSplineThickness(), [](float v)
                          { SplineEditorSettings::Get().SetSplineThickness(v); });

    // Handle size / thickness — only meaningful for Bezier. Hidden entirely for Linear and
    // Catmull-Rom since those curve types have no handles.
    if (isBezier)
    {
        addFloatPresetSubmenu("Handle Size", EditorIcons::kSplineCurve,
                              kSceneSplineHandleSizePresets, settings.GetHandleSize(), [](float v)
                              { SplineEditorSettings::Get().SetHandleSize(v); });
        addFloatPresetSubmenu("Handle Thickness", EditorIcons::kSplineCurve,
                              kSceneSplineHandleThickPresets, settings.GetHandleThickness(),
                              [](float v)
                              { SplineEditorSettings::Get().SetHandleThickness(v); });
    }

    items.push_back({.Separator = true});

    // Half-width in metres for the points a new stroke creates. It also drives the
    // width envelope the scene gizmo draws, because that band IS the point radius.
    addFloatPresetSubmenu("Default Radius", EditorIcons::kRuler,
                          kSceneSplineDefaultRadiusPresets, settings.GetDefaultRadius(),
                          [](float v)
                          { SplineEditorSettings::Get().SetDefaultRadius(v); });

    // Post-draw simplification tolerance (Douglas-Peucker) and Chaikin smoothing
    // iterations. Applied when a painting stroke is released.
    addFloatPresetSubmenu("Simplify Tolerance", EditorIcons::kDrawCurve,
                          kSceneSplineSimplifyPresets, settings.GetSimplifyTolerance(), [](float v)
                          { SplineEditorSettings::Get().SetSimplifyTolerance(v); });

    items.push_back({.Path = "Smoothing Passes", .IconPath = EditorIcons::kEase});
    for (const int v : kSceneSplineSmoothPresets)
    {
        items.push_back({.Path = std::string("Smoothing Passes/") +
                                 (v == 0 ? std::string("Off") : std::to_string(v)),
                         .Flags = CheckedFlag(v == settings.GetSmoothingIterations()),
                         .OnActivate = [v]
                         { SplineEditorSettings::Get().SetSmoothingIterations(v); }});
    }

    // Nothing that depends on mesh snapping can act while snapping is off.
    const uint32_t snapDependent =
        settings.GetSnapToMeshes() ? MenuItemFlag_None : MenuItemFlag_Disabled;

    items.push_back({.Path = "Mesh Acceleration", .IconPath = EditorIcons::kCube});
    const struct
    {
        const char* Label;
        const char* Icon;
        SplineMeshSnapAccel Value;
    } kAccelModes[] = {
        {"Mesh Acceleration/Automatic", EditorIcons::kCube, SplineMeshSnapAccel::Auto},
        {"Mesh Acceleration/Brute Force (Triangle Loop)", EditorIcons::kCube,
         SplineMeshSnapAccel::BruteForce},
        {"Mesh Acceleration/BVH (Large Meshes)", EditorIcons::kTree,
         SplineMeshSnapAccel::Bvh},
    };
    for (const auto& accel : kAccelModes)
    {
        items.push_back({.Path = accel.Label,
                         .IconPath = accel.Icon,
                         .Flags = snapDependent |
                                  CheckedFlag(settings.GetMeshSnapAccel() == accel.Value),
                         .OnActivate = [value = accel.Value]
                         { SplineEditorSettings::Get().SetMeshSnapAccel(value); }});
    }

    items.push_back({.Separator = true});

    // Connection behavior toggles.
    const auto toggle = [&items](const char* label, const char* icon, uint32_t flags,
                                 void (*apply)())
    { items.push_back({.Path = label, .IconPath = icon, .Flags = flags, .OnActivate = apply}); };

    toggle("Auto-Connect to Nearby Spline", EditorIcons::kSplineCurve,
           CheckedFlag(settings.GetAutoConnect()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetAutoConnect(!s.GetAutoConnect());
           });
    toggle("Auto-Close Loop When Endpoints Meet", EditorIcons::kLoop,
           CheckedFlag(settings.GetAutoCloseLoop()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetAutoCloseLoop(!s.GetAutoCloseLoop());
           });
    toggle("Snap to Scene Meshes", EditorIcons::kScene, CheckedFlag(settings.GetSnapToMeshes()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetSnapToMeshes(!s.GetSnapToMeshes());
           });
    toggle("Stick Stroke to Starting Mesh", EditorIcons::kMagnet,
           snapDependent | CheckedFlag(settings.GetStickToMesh()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetStickToMesh(!s.GetStickToMesh());
           });
    toggle("Conform Stroke to Surface", EditorIcons::kMagnet,
           CheckedFlag(settings.GetConformToSurface()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetConformToSurface(!s.GetConformToSurface());
           });
    toggle("Drape Splines on Surface (Display)", EditorIcons::kSplineCurve,
           CheckedFlag(settings.GetDrapeToSurface()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetDrapeToSurface(!s.GetDrapeToSurface());
           });
    toggle("Constant Screen Size (Knots, Handles, Lines)", EditorIcons::kRuler,
           CheckedFlag(settings.GetConstantScreenSize()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetConstantScreenSize(!s.GetConstantScreenSize());
           });
    toggle("Smart Scaling by Distance", EditorIcons::kRuler,
           CheckedFlag(settings.GetSmartDistanceScaling()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetSmartDistanceScaling(!s.GetSmartDistanceScaling());
           });
    toggle("Show All Spline Controls", EditorIcons::kEye,
           CheckedFlag(settings.GetShowAllControls()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetShowAllControls(!s.GetShowAllControls());
           });
    toggle("Show Width Envelope", EditorIcons::kEye,
           CheckedFlag(settings.GetShowWidthEnvelope()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetShowWidthEnvelope(!s.GetShowWidthEnvelope());
           });
    toggle("Pivot From Spline Center", EditorIcons::kSplineCurve,
           CheckedFlag(settings.GetPivotFromSplineCenter()),
           []
           {
               auto& s = SplineEditorSettings::Get();
               s.SetPivotFromSplineCenter(!s.GetPivotFromSplineCenter());
           });

    items.push_back({.Separator = true});

    // Colors — open native color picker. Disabled if host didn't wire a picker.
    const uint32_t colorFlags =
        openColorPicker ? MenuItemFlag_None : MenuItemFlag_Disabled;
    const auto colorPicker = [&openColorPicker](const char* label, uint32_t flags, std::string swatch,
                                    uint32_t (*get)(), void (*apply)(uint32_t)) -> Item
    {
        return {.Path = label,
                .IconPath = EditorIcons::kColorPicker,
                .ColorHex = std::move(swatch),
                .Flags = flags,
                .OnActivate =
                    [openColorPicker, get, apply]
                    {
                        if (!openColorPicker)
                            return;
                        ColorPickerCallbacks cbs;
                        cbs.onApply = [apply](uint32_t argb, float /*intensity*/) { apply(argb); };
                        cbs.onValueChanging = [apply](uint32_t argb, float /*intensity*/)
                        { apply(argb); };
                        openColorPicker(get(), 1.0f, std::move(cbs));
                    }};
    };

    items.push_back(colorPicker(
        "Spline Color...", colorFlags, ArgbToHexRGB(settings.GetSplineColor()),
        [] { return SplineEditorSettings::Get().GetSplineColor(); },
        [](uint32_t c) { SplineEditorSettings::Get().SetSplineColor(c); }));
    items.push_back(colorPicker(
        "Knot Color...", colorFlags, ArgbToHexRGB(settings.GetKnotColor()),
        [] { return SplineEditorSettings::Get().GetKnotColor(); },
        [](uint32_t c) { SplineEditorSettings::Get().SetKnotColor(c); }));
    items.push_back(colorPicker(
        "Selected Knot Color...", colorFlags, ArgbToHexRGB(settings.GetKnotSelectedColor()),
        [] { return SplineEditorSettings::Get().GetKnotSelectedColor(); },
        [](uint32_t c) { SplineEditorSettings::Get().SetKnotSelectedColor(c); }));
    items.push_back(colorPicker(
        "Brush Stroke Color...", colorFlags, ArgbToHexRGB(settings.GetBrushStrokeColor()),
        [] { return SplineEditorSettings::Get().GetBrushStrokeColor(); },
        [](uint32_t c) { SplineEditorSettings::Get().SetBrushStrokeColor(c); }));
    if (isBezier)
    {
        items.push_back(colorPicker(
            "Tangent Color...", colorFlags, std::string{},
            [] { return SplineEditorSettings::Get().GetHandleColor(); },
            [](uint32_t c) { SplineEditorSettings::Get().SetHandleColor(c); }));
    }

    items.push_back({.Separator = true});
    items.push_back({.Path = "Reset to Defaults",
                     .IconPath = EditorIcons::kReset,
                     .OnActivate = [] {
                         auto& s = SplineEditorSettings::Get();
                         s.SetCurveType(SplineCurveType::CatmullRom);
                         s.SetControlRenderShape(SplineControlRenderShape::Ring);
                         s.SetKnotSize(0.3f);
                         s.SetKnotThickness(2.0f);
                         s.SetSplineThickness(2.0f);
                         s.SetHandleSize(0.2f);
                         s.SetHandleThickness(1.5f);
                         s.SetKnotColor(0xFF18E632u);
                         s.SetKnotSelectedColor(0xFFFFFF00u);
                         s.SetSplineColor(0xFF1F96F3u);
                         s.SetBrushStrokeColor(0xFFFF9919u);
                         s.SetHandleColor(0xFFFF66CBu);
                         s.SetSimplifyTolerance(0.25f);
                         s.SetSmoothingIterations(0);
                         s.SetAutoConnect(true);
                         s.SetAutoCloseLoop(true);
                         s.SetConstantScreenSize(true);
                         s.SetSmartDistanceScaling(false);
                         s.SetDefaultRadius(5.0f);
                         s.SetShowAllControls(true);
                         s.SetShowWidthEnvelope(true);
                         s.SetPivotFromSplineCenter(false);
                         s.SetSnapToMeshes(true);
                         s.SetMeshSnapAccel(SplineMeshSnapAccel::Auto);
                         s.SetStickToMesh(true);
                         s.SetConformToSurface(false);
                         s.SetDrapeToSurface(true);
                         s.SetAutoConnectTolerance(2.0f);
                         s.SetCloseLoopTolerance(2.0f);
                     }});

    return items;
}

} // namespace GameEngine::Editor
