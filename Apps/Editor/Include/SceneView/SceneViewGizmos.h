#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "Rendering/CameraTypes.h"
#include "SceneViewEvents.h"
#include "Types/Color.h"

namespace GameEngine::ECS { class World; }

namespace GameEngine {
namespace Editor {
namespace SceneTools {

enum class GizmoHitKind : std::uint8_t
{
    None,
    Axis,
    Plane,
    Volume,
    Custom
};

struct GizmoHit
{
    float        distance = 0.0f;
    GizmoHitKind kind     = GizmoHitKind::None;
    std::uint32_t handleId = 0;
};

struct GizmoHitResult
{
    bool     hit  = false;
    GizmoHit info = {};
};

// Where a gizmo's geometry sits relative to the scene depth buffer.
//
// The dividing line: occlusion is information for a gizmo that DESCRIBES the
// scene, and interference for one that ACTS on it. A spline curve, a knot
// marker or a bounds box reads as a place in the world, so dimming it behind a
// ridge tells the user where that place is. A translate handle is a control
// surface — it says "grab here", and geometry between it and the camera only
// makes it harder to grab. Unity, UE and Godot all draw manipulation handles
// unoccluded for this reason.
//
// SceneDepth is the default, and the right answer for anything anchored to
// world geometry: the overlay pass draws it twice against scene depth, dimmed
// where occluded and at full strength where visible.
//
// AlwaysOnTop is the opt-in — drawn once, full strength, no depth test. It
// covers the manipulation handles (translate / rotate / scale, and the marquee
// that acts on the selection), plus geometry whose world position is an
// artefact of how it is constructed rather than a place it means to be: a
// marquee rectangle parked on a plane in front of the camera, or a fill that
// hugs the very surface it describes.
enum class GizmoDepthMode : std::uint8_t
{
    SceneDepth,
    AlwaysOnTop
};

// A SceneDepth draw's strength where scene geometry occludes it: the fraction of its alpha the
// overlay pass's occluded draw keeps. World overlays that draw themselves against scene depth
// (the mark-up glow) dim their occluded part by the same fraction.
inline constexpr float kGizmoOccludedAlphaScale = 0.5f;

class GizmoRenderContext
{
public:
    GizmoRenderContext(Rendering::ViewId viewId,
                       Rendering::CameraId cameraId,
                       const Mathematics::Vector3* cameraWorldPos = nullptr,
                       const Mathematics::Vector3* cameraUp = nullptr,
                       ECS::World* world = nullptr)
        : m_ViewId(viewId)
        , m_CameraId(cameraId)
        , m_World(world)
	    {
            if (cameraWorldPos)
            {
                m_CameraWorldPos = *cameraWorldPos;
                m_HasCameraWorldPos = true;
            }
            if (cameraUp)
            {
                m_CameraUp = *cameraUp;
                m_HasCameraUp = true;
            }
	    }

    // Geometry-only drawing contexts have no world; Scene View supplies its bound world.
    ECS::World* GetWorld() const { return m_World; }

    Rendering::ViewId GetViewId() const { return m_ViewId; }
    Rendering::CameraId GetCameraId() const { return m_CameraId; }

    // Optional camera position provided by the host (SceneViewController). Gizmos
    // should prefer this over reaching into global state.
    bool HasCameraWorldPosition() const { return m_HasCameraWorldPos; }
    const Mathematics::Vector3* GetCameraWorldPosition() const
    {
        return m_HasCameraWorldPos ? &m_CameraWorldPos : nullptr;
    }

    bool HasCameraUp() const { return m_HasCameraUp; }
    const Mathematics::Vector3* GetCameraUp() const { return m_HasCameraUp ? &m_CameraUp : nullptr; }

    // Orthographic vertical extent in world units (total height of the visible
    // frustum). Set by the host only when the camera is in orthographic mode.
    // Transform gizmos use this to stay at a constant pixel size in 2D/ortho
    // mode, where the literal camera-to-pivot distance no longer controls the
    // projection scale.
    void SetOrthoHeight(float heightWorld)
    {
        m_OrthoHeight    = heightWorld;
        m_HasOrthoHeight = true;
    }
    bool  HasOrthoHeight() const { return m_HasOrthoHeight; }
    float GetOrthoHeight() const { return m_OrthoHeight; }

    // Editor "2D view" (orthographic XY side view). Gizmos that draw flat discs
    // in XZ using world +Y should switch to camera-facing billboards in this mode.
    void SetEditor2DMode(bool is2D) { m_Editor2DMode = is2D; }
    bool IsEditor2DMode() const { return m_Editor2DMode; }

	    // Configure which logical triangle layer subsequent DrawTriangles / solid
	    // primitive calls should use. Layers are consumed by the Scene View
	    // overlay pass to order opaque vs transparent gizmo meshes without
	    // relying purely on submission order. Layer 0 is the default.
	    void SetTriangleLayer(std::int32_t layer) { m_TriangleLayer = layer; }

	    // Depth mode applied to subsequent line and triangle draws. Opt-in: a
	    // gizmo that says nothing gets SceneDepth, so new gizmos depth-test like
	    // the rest without having to know this exists. Prefer GizmoDepthModeScope
	    // over calling this directly — the mode is sticky context state shared by
	    // every gizmo in the frame.
	    void SetDepthMode(GizmoDepthMode mode) { m_DepthMode = mode; }
	    GizmoDepthMode GetDepthMode() const { return m_DepthMode; }

	    // Drawing API implemented against the Scene View gizmo overlay pass.
	    // Thickness is specified in world units; concrete implementations may map
	    // this to screen-space line width as appropriate for the renderer.
	    //
	    // Draw a line with an explicit RGBA color. Alpha is reserved for future
	    // use (e.g., translucency in a more advanced gizmo pipeline).
	    void DrawColoredLine(const Mathematics::Vector3& from,
	                        const Mathematics::Vector3& to,
	                        const Color& color,
	                        float thickness = 1.0f);

	    // Bulk line append for precomputed line lists. vertices holds
	    // lineCount consecutive endpoint pairs: from0, to0, from1, to1, ...
	    void DrawColoredLines(const Mathematics::Vector3* vertices,
	                         std::size_t lineCount,
	                         const Color& color,
	                         float thickness = 1.0f);

	    	// --- Mesh / solid primitive helpers ---------------------------------------
	    	// Low-level helper: append a triangle list to the per-view gizmo mesh
            // groups. vertices holds vertexCount positions (a multiple of 3).
            void DrawTriangles(const Mathematics::Vector3* vertices,
	    	                   std::size_t vertexCount,
                               const Color& color);

    	// Solid primitives, expressed directly in world space.
        void DrawSolidBox(const Mathematics::Vector3& center,
                          const Mathematics::Vector3& halfExtents,
                          const Color& color);
        // Oriented box: axes[i] is the box's local axis i in world space.
        void DrawSolidOrientedBox(const Mathematics::Vector3& center,
                                  const Mathematics::Vector3& halfExtents,
                                  const std::array<Mathematics::Vector3, 3>& axes,
                                  const Color& color);
            void DrawSolidSphere(const Mathematics::Vector3& center,
	    	                     float radius,
                                 const Color& color);
            void DrawSolidCylinder(const Mathematics::Vector3& start,
                                  const Mathematics::Vector3& end,
	    	                      float radius,
                                  const Color& color);
            void DrawSolidCone(const Mathematics::Vector3& apex,
                              const Mathematics::Vector3& baseCenter,
	    	                  float baseRadius,
                              const Color& color);
            void DrawSolidDisc(const Mathematics::Vector3& center,
                              const Mathematics::Vector3& normal,
	    	                  float radius,
                              const Color& color);

	    	// Wireframe helpers built on top of the line API. These are useful for
	    	// selection outlines and non-solid debug visuals.
            void DrawWireBox(const Mathematics::Vector3& center,
                             const Mathematics::Vector3& halfExtents,
                             const Color& color,
	    	                 float thickness = 1.0f);
            void DrawWireSphere(const Mathematics::Vector3& center,
	    	                    float radius,
                                const Color& color,
	    	                    float thickness = 1.0f);
            void DrawWireCircle(const Mathematics::Vector3& center,
                               const Mathematics::Vector3& normal,
	    	                   float radius,
                               const Color& color,
	    	                   float thickness = 1.0f);

        /// Draw an ellipse from two world-space radius vectors, including skewed bases.
        void DrawWireEllipse(const Mathematics::Vector3& center,
                             const Mathematics::Vector3& radiusA,
                             const Mathematics::Vector3& radiusB,
                             const Color& color, float thickness = 1.0f);

	    	// Solid torus (thick ring) for gizmo rotation handles. majorRadius is
	    	// the ring radius, tubeRadius is the thickness of the tube. ringSegments
	    	// controls smoothness around the ring, tubeSegments controls tube cross-section.
	    	//
	    	// When cullTowardCameraPos is non-null, only the half of the ring that
	    	// faces that camera position is emitted (front-facing arc). This matches
	    	// the DCC convention of hiding the occluded back arc of rotation rings so
	    	// the near side reads clearly and can't be mis-grabbed. Pass null to draw
	    	// the full ring.
            void DrawSolidTorus(const Mathematics::Vector3& center,
                                const Mathematics::Vector3& normal,
	    	                    float majorRadius,
	    	                    float tubeRadius,
                                const Color& color,
	    	                    int ringSegments = 32,
	    	                    int tubeSegments = 8,
                                const Mathematics::Vector3* cullTowardCameraPos = nullptr);

	    	// Draw a wireframe box transformed by a 4x4 matrix. The box corners are
	    	// defined by localMin/localMax in local space, then transformed by the
	    	// provided matrix before drawing.
            void DrawTransformedWireBox(const Mathematics::Vector3& localMin,
                                        const Mathematics::Vector3& localMax,
                                        const Mathematics::Matrix4x4& transform,
                                        const Color& color,
	    	                            float thickness = 1.0f);

private:
	    Rendering::ViewId   m_ViewId        { 0 };
	    Rendering::CameraId m_CameraId      { 0 };
	    ECS::World* const   m_World;
        Mathematics::Vector3 m_CameraWorldPos {0.0f, 0.0f, 0.0f};
        bool               m_HasCameraWorldPos {false};
        Mathematics::Vector3 m_CameraUp {0.0f, 1.0f, 0.0f};
        bool               m_HasCameraUp {false};
        float              m_OrthoHeight   { 0.0f };
        bool               m_HasOrthoHeight { false };
        bool               m_Editor2DMode  { false };
	    std::int32_t        m_TriangleLayer { 0 };
	    GizmoDepthMode      m_DepthMode     { GizmoDepthMode::SceneDepth };
};

// Scoped depth-mode override. The mode is sticky state on a context shared by
// every gizmo in the frame, so an opt-in that is not restored leaks
// always-on-top into whatever renders next; this makes forgetting impossible.
class GizmoDepthModeScope
{
public:
    GizmoDepthModeScope(GizmoRenderContext& ctx, GizmoDepthMode mode)
        : m_Ctx(ctx)
        , m_Previous(ctx.GetDepthMode())
    {
        ctx.SetDepthMode(mode);
    }

    ~GizmoDepthModeScope() { m_Ctx.SetDepthMode(m_Previous); }

    GizmoDepthModeScope(const GizmoDepthModeScope&) = delete;
    GizmoDepthModeScope& operator=(const GizmoDepthModeScope&) = delete;

private:
    GizmoRenderContext& m_Ctx;
    GizmoDepthMode      m_Previous;
};

// RAII accumulator that collects line vertices sharing a single color +
// thickness, then submits them via a single GizmoRenderContext::DrawColoredLines
// call on destruction.
//
// Use this when a gizmo emits multiple lines that share styling (e.g. an
// icon silhouette, a wire sphere, a direction arrow). Calling
// ctx.DrawColoredLine(...) N times performs N linear-scan group lookups
// into the per-view gizmo line groups; batching collapses that to one
// lookup + one bulk insert.
//
// Helpers such as DrawCircleInto take a GizmoLineBatch& so they don't need
// to know how submission works — they just call AddLine().
//
// The batch holds a fixed-size INLINE buffer (no heap allocation). When the
// buffer fills, AddLine() auto-flushes via DrawColoredLines and resets the
// count, so streaming any number of lines is safe — they merge into the same
// per-view (color, thickness) group on the rendering side regardless of how
// many flush boundaries we cross.
//
// Typical use:
//
//     {
//         GizmoLineBatch batch(ctx, color, thickness);
//         DrawCircleInto(batch, center, axisA, rA, axisB, rB, segments);
//         batch.AddLine(p0, p1);
//         // ...
//     } // dtor flushes via DrawColoredLines.
//
// Non-copyable / non-movable: lifetime is intentionally scope-bound.
class GizmoLineBatch
{
public:
    // Inline-buffer size in lines. 128 lines = 256 endpoints = 3 KB stack per
    // batch — fits all current icon helpers (the largest, DrawAmbientIcon,
    // is ~108 lines). Helpers exceeding this stream-flush, no heap involved.
    static constexpr std::size_t kInlineLineCap = 128;

    GizmoLineBatch(GizmoRenderContext& ctx, const Color& color, float thickness)
        : m_Ctx(&ctx), m_Color(color), m_Thickness(thickness)
    {
    }

    GizmoLineBatch(const GizmoLineBatch&) = delete;
    GizmoLineBatch& operator=(const GizmoLineBatch&) = delete;
    GizmoLineBatch(GizmoLineBatch&&) = delete;
    GizmoLineBatch& operator=(GizmoLineBatch&&) = delete;

    ~GizmoLineBatch() { Flush(); }

    // Append a single line a -> b. Streams via Flush() if the inline buffer
    // fills.
    void AddLine(const Mathematics::Vector3& a, const Mathematics::Vector3& b)
    {
        if (m_Count >= kInlineLineCap)
            Flush();
        m_Buffer[m_Count * 2u] = a;
        m_Buffer[m_Count * 2u + 1u] = b;
        ++m_Count;
    }

    // Submit accumulated lines immediately and reset. The dtor calls this
    // automatically; explicit Flush() is rarely needed except during a
    // stream-overflow inside AddLine.
    void Flush()
    {
        if (m_Count == 0u || !m_Ctx)
            return;
        m_Ctx->DrawColoredLines(m_Buffer, m_Count, m_Color, m_Thickness);
        m_Count = 0u;
    }

    std::size_t LineCount() const { return m_Count; }
    bool        Empty()     const { return m_Count == 0u; }

private:
    GizmoRenderContext* m_Ctx;
    Color               m_Color;
    float               m_Thickness;
    std::size_t         m_Count {0};
    Mathematics::Vector3 m_Buffer[kInlineLineCap * 2];
};

// Orthographic views have no camera distance that sets the projected scale, so
// screen-size-stable gizmos use orthoHeight * this factor instead: the distance at
// which a 45-degree vertical FOV sees the same height, 1 / (2 tan 22.5°).
inline constexpr float kOrthoEffectiveDistanceFactor = 1.2071f;

// Emit `segments` lines tracing an ellipse at `center` in the plane of (axisA,
// axisB), with semi-axes rA along axisA and rB along axisB.
void DrawCircleInto(GizmoLineBatch& batch,
                    const Mathematics::Vector3& center,
                    const Mathematics::Vector3& axisA, float rA,
                    const Mathematics::Vector3& axisB, float rB,
                    int segments = 32);

// Wire ellipsoid as three rings of `segments` lines, in the planes of (axisX,
// axisY), (axisY, axisZ) and (axisZ, axisX), with semi-axes radii.x, radii.y and
// radii.z along the matching axes. Pass unit axes; equal radii draw a sphere, as
// GizmoRenderContext::DrawWireSphere does with the world axes.
void DrawWireEllipsoid(GizmoRenderContext& context,
                       const Mathematics::Vector3& center,
                       const Mathematics::Vector3& axisX,
                       const Mathematics::Vector3& axisY,
                       const Mathematics::Vector3& axisZ,
                       const Mathematics::Vector3& radii,
                       const Color& color,
                       float thickness,
                       int segments = 32);

// Orthonormal camera-facing (right, up) basis at `pos`, for billboarded icons.
// World-X stands in for world-up when the view is near-vertical, so the basis
// stays well-conditioned above and below the point.
void BuildBillboardBasis(const Mathematics::Vector3& pos,
                         const Mathematics::Vector3& camPos,
                         Mathematics::Vector3& right,
                         Mathematics::Vector3& up);

// World-space radius of a camera-facing gizmo icon (light, reflection probe, DDGI
// volume) at `worldPos`. With `orthoHeight > 0` the radius is a fixed fraction of the
// visible height, so the icon keeps its on-screen size at every zoom. Otherwise it
// scales with camera distance, clamped so far icons do not vanish and near ones do not
// balloon. Picking ray-tests against the same radius.
float ComputeGizmoIconWorldRadius(const Mathematics::Vector3& worldPos,
                                  const Mathematics::Vector3& camPos,
                                  float orthoHeight);

// A camera-facing gizmo icon as picking sees it: a sphere around the icon centre
// whose radius is the icon's world radius padded so its outer details stay
// clickable.
struct GizmoIconPickSphere
{
    Mathematics::Vector3 Center;
    float PickRadius = 0.0f;
};

// Index of the icon sphere the ray reaches first, at or in front of the ray
// origin (entry point, or exit point when the origin is inside); empty when the
// ray reaches none. The ray direction must be unit length. Equal distances go to
// the earlier icon.
std::optional<std::size_t> PickNearestGizmoIcon(const GizmoRay& ray,
                                                std::span<const GizmoIconPickSphere> icons);

// Unit tangent and bitangent spanning the plane whose normal is `unitNormal`:
// tangent = seed x normal, bitangent = normal x tangent. The seed is world up, or
// world X when |dot(normal, up)| exceeds 0.99 (within about 8 degrees of
// vertical). The normal must be unit length; a zero normal gives zero axes.
void BuildPlaneBasis(const Mathematics::Vector3& unitNormal,
                     Mathematics::Vector3& tangent,
                     Mathematics::Vector3& bitangent);

// --- Thick lines ------------------------------------------------------------
// Wide lines are not available on every backend (MoltenVK has none), so a gizmo
// line thicker than kThinLineThickness is drawn as a camera-facing quad.
//
// Thickness: with constantScreenSpaceWidth, thickness tracks an approximate
// pixel width (the world width scales with camera distance, or with the
// orthographic height). Otherwise it is a world-space half-width in hundredths
// of a unit (2.0 -> 0.02). smartDistanceScaling scales with
// sqrt(distance * kGizmoSmartDistanceReference) instead of distance.

// At or below this thickness DrawThickLine stays on the line pipeline instead of
// emitting a quad.
inline constexpr float kThinLineThickness = 1.05f;

// Smart distance scaling equals linear distance scaling at this camera distance
// (world units): gizmos drawn nearer are larger than linear, farther smaller.
inline constexpr float kGizmoSmartDistanceReference = 6.0f;

// Camera-facing quad covering a -> b at `thickness`: two triangles as a
// six-vertex triangle list in outVertices. Returns false and writes nothing
// when the context has no camera position or the segment is degenerate or seen
// end-on.
bool BuildThickLineQuad(const GizmoRenderContext& context,
                        const Mathematics::Vector3& a,
                        const Mathematics::Vector3& b,
                        float thickness,
                        bool constantScreenSpaceWidth,
                        bool smartDistanceScaling,
                        std::array<Mathematics::Vector3, 6>& outVertices);

// Line a -> b at `thickness`: a quad from BuildThickLineQuad above
// kThinLineThickness, otherwise a plain line. Above kThinLineThickness a
// degenerate segment draws nothing, and a segment with no quad (no camera
// position, seen end-on) falls back to a plain line.
void DrawThickLine(GizmoRenderContext& context,
                   const Mathematics::Vector3& a,
                   const Mathematics::Vector3& b,
                   const Color& color,
                   float thickness,
                   bool constantScreenSpaceWidth = true,
                   bool smartDistanceScaling = false);

// Append the BuildThickLineQuad triangles for a -> b to outVertices, for callers
// that submit many thick lines of one colour in one DrawTriangles call. Appends
// nothing at or below kThinLineThickness or when no quad can be built.
void AppendThickLineTriangles(const GizmoRenderContext& context,
                              const Mathematics::Vector3& a,
                              const Mathematics::Vector3& b,
                              float thickness,
                              bool constantScreenSpaceWidth,
                              bool smartDistanceScaling,
                              std::vector<Mathematics::Vector3>& outVertices);

// Wire circle of `segments` DrawThickLine segments in the plane of `normal`.
// Draws nothing for a non-positive radius, fewer than 3 segments or a
// degenerate normal.
void DrawThickWireCircle(GizmoRenderContext& context,
                         const Mathematics::Vector3& center,
                         const Mathematics::Vector3& normal,
                         float radius,
                         const Color& color,
                         float thickness,
                         bool constantScreenSpaceLineWidth,
                         bool smartDistanceScaling = false,
                         int segments = 32);

// The DrawThickWireCircle segments as AppendThickLineTriangles triangles.
void AppendThickWireCircleTriangles(const GizmoRenderContext& context,
                                    const Mathematics::Vector3& center,
                                    const Mathematics::Vector3& normal,
                                    float radius,
                                    float thickness,
                                    bool constantScreenSpaceLineWidth,
                                    bool smartDistanceScaling,
                                    std::vector<Mathematics::Vector3>& outVertices,
                                    int segments = 32);

		// Internal representation of a batch of gizmo lines that share the same
		// color and thickness. Vertices are stored as tightly packed xyz triplets.
		struct GizmoLineGroup
		{
		    Color color       {1.0f, 1.0f, 1.0f, 1.0f};
		    float thickness   {1.0f};
		    GizmoDepthMode depthMode {GizmoDepthMode::SceneDepth};
		    std::vector<float> vertices; // xyz triplets, 2 vertices per line
		};
		
		// Internal representation of a batch of gizmo triangles that share the same
		// color. Vertices are stored as tightly packed xyz triplets forming a
		// triangle list.
		struct GizmoTriangleGroup
		{
			    Color       color    {1.0f, 1.0f, 1.0f, 1.0f};
			    std::int32_t layer    {0};
			    GizmoDepthMode depthMode {GizmoDepthMode::SceneDepth};
		    std::vector<float> vertices; // xyz triplets, 3 vertices per triangle
		};
		
			// Per-view line/triangle groups accumulated by GizmoRenderContext. The Scene
			// View gizmo overlay pass consumes these groups to build vertex buffers and
			// issue draws. Call the Reset* helpers once per frame per view before
			// rendering gizmos for that view.
			void ResetGizmoLineGroups(Rendering::ViewId viewId);
			const std::vector<GizmoLineGroup>* GetGizmoLineGroups(Rendering::ViewId viewId);
			void ResetGizmoTriangleGroups(Rendering::ViewId viewId);
			const std::vector<GizmoTriangleGroup>* GetGizmoTriangleGroups(Rendering::ViewId viewId);

			// Utility used by interactive gizmos for picking/dragging. Projects a
			// world-space ray onto an infinite line defined by an origin and
			// direction. Returns false if the ray and line are nearly parallel.
			bool ProjectRayOntoLine(const GizmoRay& ray,
			                        const Mathematics::Vector3& lineOrigin,
			                        const Mathematics::Vector3& lineDirection,
			                        float& outRayT,
			                        float& outLineT,
			                        Mathematics::Vector3& outClosestPoint);

		struct ScenePointerEvent;

class IGizmo
{
public:
    virtual ~IGizmo() = default;

    virtual void Render(GizmoRenderContext& context) = 0;

    virtual GizmoHitResult HitTest(const GizmoRay& ray)
    {
        (void)ray;
        return {};
    }

    virtual bool HandlePointerEvent(const ScenePointerEvent& /*event*/, const GizmoHit& /*hit*/)
    {
        return false;
    }
};

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine
