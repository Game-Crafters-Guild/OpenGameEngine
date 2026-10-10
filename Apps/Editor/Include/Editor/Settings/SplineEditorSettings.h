#pragma once

#include <cstdint>

#include "Editor/Settings/SettingsStore.h"

namespace GameEngine
{
namespace Editor
{

enum class SplineCurveType : uint8_t
{
    CatmullRom  = 0, // smooth spline through control points, auto-tangent
    Linear      = 1, // straight polyline between control points
    CubicBezier = 2, // explicit tangent handles per control point
};

enum class SplineSelectionShape : uint8_t
{
    Rectangle = 0, // axis-aligned rectangle drag on the click plane
    Lasso     = 1, // free-form polygon traced by the cursor
};

enum class SplineControlRenderShape : uint8_t
{
    Ring   = 0,
    Sphere = 1,
    Cube   = 2,
};

// Acceleration mode for the spline tool's mesh raycast snapping. The picker
// runs Möller–Trumbore over each candidate mesh's CPU index buffer; this
// setting selects whether to brute-force or use a per-mesh BVH built lazily.
enum class SplineMeshSnapAccel : uint8_t
{
    Auto       = 0, // pick BVH for dense meshes, brute force for the rest
    BruteForce = 1, // always loop every triangle (cheap setup, O(n))
    Bvh        = 2, // always traverse the cached BVH (O(log n) after build)
};

// User-level editor settings for the spline tool visuals. Values persisted in
// the Editor preferences (Preferences.json). Controls only what the scene /
// tool gizmos actually render — knot markers, spline line, and curve shape.
class SplineEditorSettings
{
public:
    static SplineEditorSettings& Get()
    {
        static SplineEditorSettings instance;
        return instance;
    }

    void Load()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);

        double defaultRadius = static_cast<double>(m_DefaultRadius);
        double knotSize = static_cast<double>(m_KnotSize);
        double knotThickness = static_cast<double>(m_KnotThickness);
        double splineThickness = static_cast<double>(m_SplineThickness);
        double handleSize = static_cast<double>(m_HandleSize);
        double handleThickness = static_cast<double>(m_HandleThickness);
        prefs.TryGetDouble("spline.defaultRadius", defaultRadius);
        prefs.TryGetDouble("spline.knotSize", knotSize);
        prefs.TryGetDouble("spline.knotThickness", knotThickness);
        prefs.TryGetDouble("spline.splineThickness", splineThickness);
        prefs.TryGetDouble("spline.handleSize", handleSize);
        prefs.TryGetDouble("spline.handleThickness", handleThickness);

        double simplifyTolerance = static_cast<double>(m_SimplifyTolerance);
        double smoothingIterations = static_cast<double>(m_SmoothingIterations);
        prefs.TryGetDouble("spline.simplifyTolerance", simplifyTolerance);
        prefs.TryGetDouble("spline.smoothingIterations", smoothingIterations);

        double autoConnectTolerance = static_cast<double>(m_AutoConnectTolerance);
        double closeLoopTolerance = static_cast<double>(m_CloseLoopTolerance);
        prefs.TryGetDouble("spline.autoConnectTolerance", autoConnectTolerance);
        prefs.TryGetDouble("spline.closeLoopTolerance", closeLoopTolerance);
        prefs.TryGetBool("spline.autoConnect", m_AutoConnect);
        prefs.TryGetBool("spline.autoCloseLoop", m_AutoCloseLoop);
        prefs.TryGetBool("spline.constantScreenSize", m_ConstantScreenSize);
        prefs.TryGetBool("spline.smartDistanceScaling", m_SmartDistanceScaling);
        prefs.TryGetBool("spline.showAllControls", m_ShowAllControls);
        prefs.TryGetBool("spline.showWidthEnvelope", m_ShowWidthEnvelope);
        prefs.TryGetBool("spline.pivotFromSplineCenter", m_PivotFromSplineCenter);
        prefs.TryGetBool("spline.snapToMeshes", m_SnapToMeshes);
        {
            double accelD = static_cast<double>(m_MeshSnapAccel);
            prefs.TryGetDouble("spline.meshSnapAccel", accelD);
            int a = static_cast<int>(accelD);
            if (a >= 0 && a <= 2)
                m_MeshSnapAccel = static_cast<SplineMeshSnapAccel>(a);
        }
        prefs.TryGetBool("spline.conformToSurface", m_ConformToSurface);
        prefs.TryGetBool("spline.drapeToSurface", m_DrapeToSurface);
        {
            double conformStep = static_cast<double>(m_ConformMaxSegmentLength);
            prefs.TryGetDouble("spline.conformMaxSegmentLength", conformStep);
            m_ConformMaxSegmentLength = static_cast<float>(conformStep);
        }
        prefs.TryGetBool("spline.stickToMesh", m_StickToMesh);

        int64_t color = 0;
        if (prefs.TryGetInt64("spline.knotColor", color))
            m_KnotColor = static_cast<uint32_t>(color);
        if (prefs.TryGetInt64("spline.knotSelectedColor", color))
            m_KnotSelectedColor = static_cast<uint32_t>(color);
        if (prefs.TryGetInt64("spline.splineColor", color))
            m_SplineColor = static_cast<uint32_t>(color);
        if (prefs.TryGetInt64("spline.brushStrokeColor", color))
            m_BrushStrokeColor = static_cast<uint32_t>(color);
        if (prefs.TryGetInt64("spline.handleColor", color))
            m_HandleColor = static_cast<uint32_t>(color);

        {
            double typeD = static_cast<double>(m_CurveType);
            prefs.TryGetDouble("spline.curveType", typeD);
            int t = static_cast<int>(typeD);
            if (t >= 0 && t <= 2)
                m_CurveType = static_cast<SplineCurveType>(t);
        }

        {
            double shapeD = static_cast<double>(m_SelectionShape);
            prefs.TryGetDouble("spline.selectionShape", shapeD);
            int s = static_cast<int>(shapeD);
            if (s >= 0 && s <= 1)
                m_SelectionShape = static_cast<SplineSelectionShape>(s);
        }
        {
            double shapeD = static_cast<double>(m_ControlRenderShape);
            prefs.TryGetDouble("spline.controlRenderShape", shapeD);
            int s = static_cast<int>(shapeD);
            if (s >= 0 && s <= 2)
                m_ControlRenderShape = static_cast<SplineControlRenderShape>(s);
        }

        m_DefaultRadius = static_cast<float>(defaultRadius);
        m_KnotSize = static_cast<float>(knotSize);
        m_KnotThickness = static_cast<float>(knotThickness);
        m_SplineThickness = static_cast<float>(splineThickness);
        m_HandleSize = static_cast<float>(handleSize);
        m_HandleThickness = static_cast<float>(handleThickness);
        m_SimplifyTolerance = static_cast<float>(simplifyTolerance);
        m_SmoothingIterations = static_cast<int>(smoothingIterations);
        m_AutoConnectTolerance = static_cast<float>(autoConnectTolerance);
        m_CloseLoopTolerance = static_cast<float>(closeLoopTolerance);
        Validate();
    }

    void Save()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err); // preserve other settings

        prefs.SetDouble("spline.defaultRadius", static_cast<double>(m_DefaultRadius));
        prefs.SetDouble("spline.knotSize", static_cast<double>(m_KnotSize));
        prefs.SetDouble("spline.knotThickness", static_cast<double>(m_KnotThickness));
        prefs.SetDouble("spline.splineThickness", static_cast<double>(m_SplineThickness));
        prefs.SetDouble("spline.handleSize", static_cast<double>(m_HandleSize));
        prefs.SetDouble("spline.handleThickness", static_cast<double>(m_HandleThickness));
        prefs.SetDouble("spline.simplifyTolerance", static_cast<double>(m_SimplifyTolerance));
        prefs.SetDouble("spline.smoothingIterations", static_cast<double>(m_SmoothingIterations));
        prefs.SetDouble("spline.autoConnectTolerance", static_cast<double>(m_AutoConnectTolerance));
        prefs.SetDouble("spline.closeLoopTolerance", static_cast<double>(m_CloseLoopTolerance));
        prefs.SetBool("spline.autoConnect", m_AutoConnect);
        prefs.SetBool("spline.autoCloseLoop", m_AutoCloseLoop);
        prefs.SetBool("spline.constantScreenSize", m_ConstantScreenSize);
        prefs.SetBool("spline.smartDistanceScaling", m_SmartDistanceScaling);
        prefs.SetBool("spline.showAllControls", m_ShowAllControls);
        prefs.SetBool("spline.showWidthEnvelope", m_ShowWidthEnvelope);
        prefs.SetBool("spline.pivotFromSplineCenter", m_PivotFromSplineCenter);
        prefs.SetBool("spline.snapToMeshes", m_SnapToMeshes);
        prefs.SetDouble("spline.meshSnapAccel", static_cast<double>(m_MeshSnapAccel));
        prefs.SetBool("spline.conformToSurface", m_ConformToSurface);
        prefs.SetBool("spline.drapeToSurface", m_DrapeToSurface);
        prefs.SetDouble("spline.conformMaxSegmentLength", static_cast<double>(m_ConformMaxSegmentLength));
        prefs.SetBool("spline.stickToMesh", m_StickToMesh);
        prefs.SetInt64("spline.knotColor", static_cast<int64_t>(m_KnotColor));
        prefs.SetInt64("spline.knotSelectedColor", static_cast<int64_t>(m_KnotSelectedColor));
        prefs.SetInt64("spline.splineColor", static_cast<int64_t>(m_SplineColor));
        prefs.SetInt64("spline.brushStrokeColor", static_cast<int64_t>(m_BrushStrokeColor));
        prefs.SetInt64("spline.handleColor", static_cast<int64_t>(m_HandleColor));
        prefs.SetDouble("spline.curveType", static_cast<double>(m_CurveType));
        prefs.SetDouble("spline.selectionShape", static_cast<double>(m_SelectionShape));
        prefs.SetDouble("spline.controlRenderShape", static_cast<double>(m_ControlRenderShape));

        prefs.Save(&err);
    }

    // Half-width in metres given to every control point a new stroke creates, and
    // seeded into the new entity's SplineComponent::DefaultRadius. It is the
    // authoring default only: an existing point keeps its own Radius, which the
    // spline inspector edits per point.
    float GetDefaultRadius() const { return m_DefaultRadius; }
    float GetKnotSize() const { return m_KnotSize; }
    float GetKnotThickness() const { return m_KnotThickness; }
    float GetSplineThickness() const { return m_SplineThickness; }
    float GetHandleSize() const { return m_HandleSize; }
    float GetHandleThickness() const { return m_HandleThickness; }
    uint32_t GetKnotColor() const { return m_KnotColor; }
    uint32_t GetKnotSelectedColor() const { return m_KnotSelectedColor; }
    uint32_t GetSplineColor() const { return m_SplineColor; }
    uint32_t GetBrushStrokeColor() const { return m_BrushStrokeColor; }
    uint32_t GetHandleColor() const { return m_HandleColor; }
    SplineCurveType GetCurveType() const { return m_CurveType; }
    SplineSelectionShape GetSelectionShape() const { return m_SelectionShape; }
    SplineControlRenderShape GetControlRenderShape() const { return m_ControlRenderShape; }
    float GetSimplifyTolerance() const { return m_SimplifyTolerance; }
    int GetSmoothingIterations() const { return m_SmoothingIterations; }
    bool GetAutoConnect() const { return m_AutoConnect; }
    bool GetAutoCloseLoop() const { return m_AutoCloseLoop; }
    bool GetConstantScreenSize() const { return m_ConstantScreenSize; }
    bool GetSmartDistanceScaling() const { return m_SmartDistanceScaling; }
    bool GetShowAllControls() const { return m_ShowAllControls; }
    // Display-only: the scene gizmo fills and outlines the selected spline's
    // width band, whose half-width is each control point's Radius. The band is
    // opaque enough to hide the scene under a wide spline, so it is switchable.
    bool GetShowWidthEnvelope() const { return m_ShowWidthEnvelope; }
    bool GetPivotFromSplineCenter() const { return m_PivotFromSplineCenter; }
    bool GetSnapToMeshes() const { return m_SnapToMeshes; }
    SplineMeshSnapAccel GetMeshSnapAccel() const { return m_MeshSnapAccel; }
    bool GetConformToSurface() const { return m_ConformToSurface; }
    // Display-only: the scene gizmo drapes spline centerlines onto the scene
    // surface. Authored point data is never modified (unlike ConformToSurface,
    // which snaps the authored knots at stroke finalize).
    bool GetDrapeToSurface() const { return m_DrapeToSurface; }
    float GetConformMaxSegmentLength() const { return m_ConformMaxSegmentLength; }
    bool GetStickToMesh() const { return m_StickToMesh; }
    float GetAutoConnectTolerance() const { return m_AutoConnectTolerance; }
    float GetCloseLoopTolerance() const
    {
        return m_AutoCloseLoop ? m_CloseLoopTolerance : 0.0f;
    }

    void SetAutoConnect(bool value) { m_AutoConnect = value; Save(); }
    void SetAutoCloseLoop(bool value) { m_AutoCloseLoop = value; Save(); }
    void SetConstantScreenSize(bool value)
    {
        m_ConstantScreenSize = value;
        Save();
    }
    void SetSmartDistanceScaling(bool value)
    {
        m_SmartDistanceScaling = value;
        Save();
    }
    void SetShowAllControls(bool value)
    {
        m_ShowAllControls = value;
        Save();
    }
    void SetShowWidthEnvelope(bool value)
    {
        m_ShowWidthEnvelope = value;
        Save();
    }
    void SetPivotFromSplineCenter(bool value)
    {
        m_PivotFromSplineCenter = value;
        Save();
    }
    void SetSnapToMeshes(bool value)
    {
        m_SnapToMeshes = value;
        Save();
    }
    void SetMeshSnapAccel(SplineMeshSnapAccel value)
    {
        m_MeshSnapAccel = value;
        Save();
    }
    void SetConformToSurface(bool value)
    {
        m_ConformToSurface = value;
        Save();
    }
    void SetDrapeToSurface(bool value)
    {
        m_DrapeToSurface = value;
        Save();
    }
    void SetConformMaxSegmentLength(float value)
    {
        m_ConformMaxSegmentLength = value;
        Validate();
        Save();
    }
    void SetStickToMesh(bool value)
    {
        m_StickToMesh = value;
        Save();
    }
    void SetAutoConnectTolerance(float value)
    {
        m_AutoConnectTolerance = value;
        Validate();
        Save();
    }
    void SetCloseLoopTolerance(float value)
    {
        m_CloseLoopTolerance = value;
        Validate();
        Save();
    }

    void SetSimplifyTolerance(float value)
    {
        m_SimplifyTolerance = value;
        Validate();
        Save();
    }

    void SetSmoothingIterations(int value)
    {
        m_SmoothingIterations = value;
        Validate();
        Save();
    }

    void SetDefaultRadius(float value)
    {
        m_DefaultRadius = value;
        Validate();
        Save();
    }

    void SetKnotSize(float value)
    {
        m_KnotSize = value;
        Validate();
        Save();
    }

    void SetKnotThickness(float value)
    {
        m_KnotThickness = value;
        Validate();
        Save();
    }

    void SetSplineThickness(float value)
    {
        m_SplineThickness = value;
        Validate();
        Save();
    }

    void SetHandleSize(float value)
    {
        m_HandleSize = value;
        Validate();
        Save();
    }

    void SetHandleThickness(float value)
    {
        m_HandleThickness = value;
        Validate();
        Save();
    }

    void SetKnotColor(uint32_t argb) { m_KnotColor = argb; Save(); }
    void SetKnotSelectedColor(uint32_t argb) { m_KnotSelectedColor = argb; Save(); }
    void SetSplineColor(uint32_t argb) { m_SplineColor = argb; Save(); }
    void SetBrushStrokeColor(uint32_t argb) { m_BrushStrokeColor = argb; Save(); }
    void SetHandleColor(uint32_t argb) { m_HandleColor = argb; Save(); }

    void SetCurveType(SplineCurveType value) { m_CurveType = value; Save(); }
    void SetSelectionShape(SplineSelectionShape value) { m_SelectionShape = value; Save(); }
    void SetControlRenderShape(SplineControlRenderShape value) { m_ControlRenderShape = value; Save(); }

private:
    SplineEditorSettings() { Load(); }

    void Validate()
    {
        if (!(m_DefaultRadius > 0.0f)) m_DefaultRadius = 0.25f;
        if (m_DefaultRadius < 0.1f)    m_DefaultRadius = 0.1f;
        if (m_DefaultRadius > 500.0f)  m_DefaultRadius = 500.0f;

        if (!(m_KnotSize > 0.0f)) m_KnotSize = 0.1f;
        if (m_KnotSize < 0.05f)   m_KnotSize = 0.05f;
        if (m_KnotSize > 10.0f)   m_KnotSize = 10.0f;

        if (!(m_KnotThickness > 0.0f)) m_KnotThickness = 2.0f;
        if (m_KnotThickness < 0.5f)    m_KnotThickness = 0.5f;
        if (m_KnotThickness > 20.0f)   m_KnotThickness = 20.0f;

        if (!(m_SplineThickness > 0.0f)) m_SplineThickness = 2.0f;
        if (m_SplineThickness < 0.5f)    m_SplineThickness = 0.5f;
        if (m_SplineThickness > 20.0f)   m_SplineThickness = 20.0f;

        if (!(m_HandleSize > 0.0f)) m_HandleSize = 0.2f;
        if (m_HandleSize < 0.05f)   m_HandleSize = 0.05f;
        if (m_HandleSize > 10.0f)   m_HandleSize = 10.0f;

        if (!(m_HandleThickness > 0.0f)) m_HandleThickness = 1.5f;
        if (m_HandleThickness < 0.5f)    m_HandleThickness = 0.5f;
        if (m_HandleThickness > 20.0f)   m_HandleThickness = 20.0f;

        if (m_SimplifyTolerance < 0.0f)    m_SimplifyTolerance = 0.0f;
        if (m_SimplifyTolerance > 50.0f)   m_SimplifyTolerance = 50.0f;

        if (m_SmoothingIterations < 0) m_SmoothingIterations = 0;
        if (m_SmoothingIterations > 8) m_SmoothingIterations = 8;

        if (!(m_AutoConnectTolerance > 0.0f)) m_AutoConnectTolerance = 2.0f;
        if (m_AutoConnectTolerance < 0.05f)   m_AutoConnectTolerance = 0.05f;
        if (m_AutoConnectTolerance > 50.0f)   m_AutoConnectTolerance = 50.0f;

        if (!(m_CloseLoopTolerance > 0.0f)) m_CloseLoopTolerance = 2.0f;
        if (m_CloseLoopTolerance < 0.05f)   m_CloseLoopTolerance = 0.05f;
        if (m_CloseLoopTolerance > 50.0f)   m_CloseLoopTolerance = 50.0f;

        if (!(m_ConformMaxSegmentLength > 0.0f)) m_ConformMaxSegmentLength = 0.5f;
        if (m_ConformMaxSegmentLength < 0.05f)   m_ConformMaxSegmentLength = 0.05f;
        if (m_ConformMaxSegmentLength > 20.0f)   m_ConformMaxSegmentLength = 20.0f;
    }

    float    m_DefaultRadius     = 0.25f;
    float    m_KnotSize          = 0.1f;
    float    m_KnotThickness     = 2.0f;
    float    m_SplineThickness   = 2.0f;
    float    m_HandleSize        = 0.2f;
    float    m_HandleThickness   = 1.5f;
    // ARGB (0xAARRGGBB)
    uint32_t m_KnotColor         = 0xFF18E632u;
    uint32_t m_KnotSelectedColor = 0xFFFFFF00u;
    uint32_t m_SplineColor       = 0xFF1F96F3u;
    uint32_t m_BrushStrokeColor  = 0xFFFF9919u;
    uint32_t m_HandleColor       = 0xFFFF66CBu;
    SplineCurveType m_CurveType  = SplineCurveType::CatmullRom;
    SplineSelectionShape m_SelectionShape = SplineSelectionShape::Rectangle;
    SplineControlRenderShape m_ControlRenderShape = SplineControlRenderShape::Cube;
    float    m_SimplifyTolerance = 0.25f;
    int      m_SmoothingIterations = 0;
    bool     m_AutoConnect = true;
    bool     m_AutoCloseLoop = true;
    bool     m_ConstantScreenSize = true;
    bool     m_SmartDistanceScaling = false;
    bool     m_ShowAllControls = true;
    bool     m_ShowWidthEnvelope = true;
    bool     m_PivotFromSplineCenter = false;
    bool                m_SnapToMeshes = true;
    SplineMeshSnapAccel m_MeshSnapAccel = SplineMeshSnapAccel::Auto;
    bool     m_ConformToSurface = false;
    bool     m_DrapeToSurface = true;
    float    m_ConformMaxSegmentLength = 0.5f;
    bool     m_StickToMesh = true;
    float    m_AutoConnectTolerance = 2.0f;
    float    m_CloseLoopTolerance = 2.0f;
};

} // namespace Editor
} // namespace GameEngine
