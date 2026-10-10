#pragma once

#include <algorithm>
#include <vector>

#include "Core/CpuProfiler.h"
#include "Rendering/CameraTypes.h"
#include "SceneViewEvents.h"
#include "SceneViewGizmos.h"

namespace GameEngine {
namespace Editor {
namespace SceneTools {

class GizmoCollector;

class ISceneTool
{
public:
    virtual ~ISceneTool() = default;

    virtual const char* GetName() const = 0;

    virtual void OnActivated() { }
    virtual void OnDeactivated() { }

    virtual void OnPointerEvent(const ScenePointerEvent& event) = 0;
    virtual void OnKeyEvent(const SceneKeyEvent& event) = 0;
    // The Delete key while the tool is active, before the view deletes the selected
    // entity: true when the tool took the key (it holds a selection of its own).
    virtual bool DeleteSelection() { return false; }

    virtual void GatherGizmos(GizmoCollector& collector) = 0;
};

class GizmoCollector
{
public:
    void Clear() { m_Gizmos.clear(); }

    void AddGizmo(IGizmo* gizmo)
    {
        if (gizmo)
        {
            m_Gizmos.push_back(gizmo);
        }
    }

    const std::vector<IGizmo*>& GetGizmos() const { return m_Gizmos; }

    bool Empty() const { return m_Gizmos.empty(); }

private:
    std::vector<IGizmo*> m_Gizmos;
};

class SceneToolContext
{
public:
    SceneToolContext() = default;
    ~SceneToolContext() = default;

    void SetView(Rendering::ViewId viewId) { m_ViewId = viewId; }
    Rendering::ViewId GetViewId() const { return m_ViewId; }

    void SetCamera(Rendering::CameraId cameraId) { m_CameraId = cameraId; }
    Rendering::CameraId GetCameraId() const { return m_CameraId; }

    void AddGlobalGizmo(IGizmo* gizmo)
    {
        if (gizmo && std::find(m_GlobalGizmos.begin(), m_GlobalGizmos.end(), gizmo) == m_GlobalGizmos.end())
            m_GlobalGizmos.push_back(gizmo);
    }

	    void SetActiveTool(ISceneTool* tool)
	    {
	        if (m_ActiveTool && m_ActiveTool != tool)
	            m_ActiveTool->OnDeactivated();
	        m_ActiveTool  = tool;
	        if (!m_DispatchingGizmoPointerEvent)
	        {
	            m_ActiveGizmo = nullptr;
	            m_ActiveHit   = GizmoHit{};
	        }
	        if (m_ActiveTool)
	            m_ActiveTool->OnActivated();
	    }
    ISceneTool* GetActiveTool() const { return m_ActiveTool; }

	    void HandlePointerEvent(const ScenePointerEvent& event)
	    {
	        GE_CPU_PROFILE_SCOPE("SceneToolContext.HandlePointerEvent");

	        if (!m_ActiveTool)
	        {
	            return;
	        }

	        if (m_CapturedTool && !(event.phase == PointerPhase::Down && event.button == PointerButton::Left))
	        {
	            GE_CPU_PROFILE_SCOPE("SceneToolContext.CapturedTool.OnPointerEvent");
	            m_CapturedTool->OnPointerEvent(event);
	            if (event.phase == PointerPhase::Up)
	                m_CapturedTool = nullptr;
	            return;
	        }

	        // Centralised gizmo picking/drag dispatch. On pointer-down we ask the
	        // active tool which gizmos it wants to expose, run hit-testing across
	        // them, and if a gizmo reports a hit we begin a drag session that
	        // receives subsequent pointer events until release.
	        if (event.phase == PointerPhase::Down && event.button == PointerButton::Left)
	        {
	            GE_CPU_PROFILE_SCOPE("SceneToolContext.PointerDown");
	            GizmoCollector collector;
	            {
	                GE_CPU_PROFILE_SCOPE("SceneToolContext.GatherGizmos");
	                m_ActiveTool->GatherGizmos(collector);
	                for (IGizmo* gizmo : m_GlobalGizmos)
	                    collector.AddGizmo(gizmo);
	            }
	
	            m_ActiveGizmo = nullptr;
	            m_ActiveHit   = GizmoHit{};
	
	            if (!collector.Empty())
	            {
	                bool     hasHit    = false;
	                GizmoHit bestHit   = {};
	                IGizmo*  bestGizmo = nullptr;
	
	                {
	                    GE_CPU_PROFILE_SCOPE("SceneToolContext.HitTestAllGizmos");
	                    for (IGizmo* gizmo : collector.GetGizmos())
	                    {
	                        if (!gizmo)
	                            continue;
	
	                        GizmoHitResult hitResult = gizmo->HitTest(event.ray);
	                        if (!hitResult.hit)
	                            continue;
	
	                        if (!hasHit || hitResult.info.distance < bestHit.distance)
	                        {
	                            hasHit    = true;
	                            bestHit   = hitResult.info;
	                            bestGizmo = gizmo;
	                        }
	                    }
	                }
	
	                if (hasHit)
	                {
	                    m_ActiveGizmo = bestGizmo;
	                    m_ActiveHit   = bestHit;
	
	                    GE_CPU_PROFILE_SCOPE("SceneToolContext.ActiveGizmo.HandlePointerEvent");
	                    m_DispatchingGizmoPointerEvent = true;
	                    const bool consumed = m_ActiveGizmo->HandlePointerEvent(event, m_ActiveHit);
	                    m_DispatchingGizmoPointerEvent = false;
	                    if (consumed)
	                    {
	                        // Gizmo consumed the event; do not forward to the
	                        // tool itself.
	                        return;
	                    }
	                }
	            }
	        }
	        else
	        {
	            if (m_ActiveGizmo)
	            {
	                GE_CPU_PROFILE_SCOPE("SceneToolContext.ActiveGizmo.HandlePointerEvent");
	                m_DispatchingGizmoPointerEvent = true;
	                const bool consumed = m_ActiveGizmo->HandlePointerEvent(event, m_ActiveHit);
	                m_DispatchingGizmoPointerEvent = false;
	                if (consumed)
	                {
	                    if (event.phase == PointerPhase::Up)
	                    {
	                        m_ActiveGizmo = nullptr;
	                        m_ActiveHit   = GizmoHit{};
	                    }
	                    return;
	                }
	
	                if (event.phase == PointerPhase::Up)
	                {
	                    m_ActiveGizmo = nullptr;
	                    m_ActiveHit   = GizmoHit{};
	                }
	            }
	        }

	        // Fall back to tool-level handling when no gizmo captured the event.
	        GE_CPU_PROFILE_SCOPE("SceneToolContext.ActiveTool.OnPointerEvent");
	        if (event.phase == PointerPhase::Down && event.button == PointerButton::Left)
	            m_CapturedTool = m_ActiveTool;
	        m_ActiveTool->OnPointerEvent(event);
	    }

    void HandleKeyEvent(const SceneKeyEvent& event)
    {
        if (m_ActiveTool)
        {
            m_ActiveTool->OnKeyEvent(event);
        }
    }

	    void GatherGizmos(GizmoCollector& collector)
	    {
	        if (m_ActiveTool)
	        {
	            m_ActiveTool->GatherGizmos(collector);
	        }
	    }

private:
	    Rendering::ViewId   m_ViewId   { 0 };
	    Rendering::CameraId m_CameraId { 0 };
	    ISceneTool*         m_ActiveTool  = nullptr;
	    ISceneTool*         m_CapturedTool = nullptr;
	    IGizmo*             m_ActiveGizmo = nullptr; // non-owning
	    GizmoHit            m_ActiveHit   = {};
	    bool                m_DispatchingGizmoPointerEvent = false;
	    std::vector<IGizmo*> m_GlobalGizmos;
};

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine
