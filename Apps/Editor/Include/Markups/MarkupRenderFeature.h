#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Markups/MarkupDrawList.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Types/FlatMap.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace GameEngine::Rendering
{
struct CameraData;
}

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::Editor
{

// The Scene View's glow for world mark-ups (desktop): each mark-up that draws a body
// (CollectMarkupDrawItems) as a translucent body in its color, alpha-over, with a rim in the same
// color: a sphere's follows its silhouette (fresnel), a box's its edges (Shaders/markup_glow.vert,
// .frag), a region's its top edge and its lid's outline (its display's walls and lid,
// Shaders/markup_glow_mesh.vert, read from a vertex buffer: RegionBuffers). It draws into its own target against the pane's scene depth, twice like the gizmo pass
// (the part behind scene geometry dimmed by SceneTools::kGizmoOccludedAlphaScale), back to front,
// and composites premultiplied into the pane's output before the gizmos, so the gizmo's outline
// and the transform tool draw over it. The selected mark-up takes a stronger rim; the hovered one
// (its panel or Hierarchy row under the pointer) a lifted body, kMarkupHoverLift. It declares
// nothing when no mark-up in view draws a body, and does not exist on the compat profile, where
// MarkupGizmo's fill stands in.
class MarkupRenderFeature final : public Engine::Renderer::IRenderFeature
{
  public:
    // The look. The body is alpha-over at kBodyAlpha. The rim term is MarkupGlowRim(facing,
    // kRimPower, gain) in Shaders/Includes/markup_glow_rim.glsl, saturated at 1; facing is n.v on
    // a sphere and MarkupGlowBoxEdgeFacing (the distance to the face's nearest edge) on a box. A
    // sphere's rim adds the color times kRimBudget times the term over the body, so no channel
    // gains more than the color times kRimBudget. A box's rim raises the body's coverage toward
    // kRimBudget instead (alpha-over in the status color), so a box face never brightens past the
    // status color or loses its hue. Where a body meets scene geometry its contact line blends the
    // color over it at up to kRimBudget (markup_glow.frag, MarkupGlowContactBand). The selected
    // mark-up's gain is kSelectedRimGain, which widens its rim; the rest take gain 1. The hovered
    // one draws its body in its color lifted toward white (MarkupHoverRgb) at kHoveredBodyAlpha,
    // the same drawing on a box and a sphere. The part behind scene geometry draws at
    // SceneTools::kGizmoOccludedAlphaScale of all of it, as the gizmo pass dims its occluded draw.
    static constexpr float kBodyAlpha = 0.3f;
    static constexpr float kRimPower = 3.0f;
    static constexpr float kRimBudget = 0.6f;
    static constexpr float kSelectedRimGain = 2.5f;
    static constexpr float kHoveredBodyAlpha = 0.6f;

    // The vertices markup_glow.vert generates for a box (six faces of two triangles) and for a
    // sphere (its kSphereRings x kSphereSegments quads, 24 x 48): restated from markup_glow.vert,
    // change them together.
    static constexpr std::uint32_t kBoxVertexCount = 36;
    static constexpr std::uint32_t kSphereVertexCount = 24 * 48 * 6;

    // One body as the glow draws it: markup_glow's push constants and its vertex count. A region
    // (Rim[3] 2) draws VertexCount vertices of its display's mesh from MeshBuffer at FirstVertex,
    // which DeclareView fills; its Model is the identity (the mesh is in world space).
    struct Draw
    {
        float Model[16]{};  // the unit shape to the world, column-major
        float Color[4]{};   // rgb = the mark-up's color, a = kBodyAlpha
        float Rim[4]{};     // kRimPower, the rim gain, kRimBudget, the shape (0 box, 1 sphere, 2 mesh)
        std::uint32_t VertexCount = 0;
        const MarkupECS::MarkupRegionDisplayCache::Entry* Region = nullptr;
        ECS::EntityHandle RegionEntity{};
        Rendering::BufferHandle MeshBuffer{};
        std::uint32_t FirstVertex = 0;
    };

    // The glow `services` draws, registered in its feature registry on first call; none on the
    // compat profile, whose devices take the gizmo pass's fill instead. Main thread only.
    static MarkupRenderFeature* Ensure(Engine::Renderer::RenderServices& services);

    // The draws for the first `bodies` of `items` (CollectMarkupDrawItems' order) that draw a
    // body, farthest first, into `draws` (cleared first).
    void BuildDraws(std::span<const MarkupDrawItem> items, std::size_t bodies, std::vector<Draw>& draws);

    // Declares the glow of `world`'s mark-ups for one Scene View pane on `frameNumber`, seen by
    // `camera`, drawn against `depth` and composited into `output` through `compositeSampler`.
    // Declares nothing, and returns false, when no mark-up in `camera`'s view draws a body (or the
    // glow's shaders are missing); true when the glow draws the bodies.
    bool DeclareView(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGTexture output,
                     Rendering::RenderGraph::RGTexture depth, Rendering::SamplerHandle compositeSampler,
                     const Rendering::CameraData& camera, ECS::World& world, const MarkupEditorBridge& bridge,
                     std::uint64_t frameNumber);

    // The mark-ups the last DeclareView collected for its pane (CollectMarkupDrawItems' order), and
    // how many lead them as the ones that draw a body: the gizmo pass of the same pane draws their
    // outlines from them (MarkupGizmo::UseCollectedItems).
    const std::vector<MarkupDrawItem>& CollectedItems() const { return m_Items; }
    std::size_t CollectedBodies() const { return m_Bodies; }

    // A device rebuild freed every buffer: the region buffers' handles are dropped, not destroyed,
    // and the next frame writes them again from the displays.
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

  private:
    // The GPU copies of the regions' meshes. A region whose mesh has not changed for two frames
    // gets a buffer of its own, written once into a fresh allocation; one that changed this frame
    // or the last (a knot drag, a height slider) draws from the frame's upload ring instead. A
    // buffer is never written again: a replaced or evicted one is retired and destroyed once the
    // frames in flight that may read it are done. A buffer goes with its region's display (unused
    // for MarkupRegionDisplayCache::kEvictAfterFrames frames); a world reset (a scene closed or
    // opened) retires them all.
    class RegionBuffers
    {
      public:
        // Where `draw`'s region mesh is read on `frameNumber`: fills its MeshBuffer and
        // FirstVertex. False when nothing could be allocated.
        bool Place(Rendering::IDevice& device, Rendering::RenderGraph::RGFrame& frame, std::uint64_t frameNumber,
                   Draw& draw);
        // Retires every buffer when `worldKey` (the drawn world's identity and reset generation)
        // changed, then those unused for kEvictAfterFrames, and destroys the retired ones past the
        // frames in flight.
        void Collect(Rendering::IDevice& device, std::uint64_t frameNumber, std::uint64_t worldKey);
        // Forgets every handle without destroying it (the device that owned them is gone).
        void Drop();

      private:
        struct Owned
        {
            Rendering::BufferHandle Buffer{};
            std::uint64_t MeshRevision = 0;
            std::uint64_t LastUsedFrame = 0;
        };
        struct Retired
        {
            Rendering::BufferHandle Buffer{};
            std::uint64_t Frame = 0;
        };

        FlatMap<std::uint32_t, Owned> m_Owned; // by the region's handle id
        std::vector<Retired> m_Retired;
        std::uint64_t m_WorldKey = 0;
    };


    // The body's two depth-split pipelines for one kind of scene depth.
    struct GlowPipelines
    {
        Rendering::GraphicsPipelineId Visible{};
        Rendering::GraphicsPipelineId Occluded{};
    };

    // Loads the glow's stages (markup_glow, and markup_glow_ms for a multisampled scene depth) and
    // the composite's copy program and interns the five pipelines, on the first frame that draws;
    // false when a package is missing (logged once). Interned ids survive a device rebuild (the
    // device keeps its intern tables), so they are never re-interned.
    bool EnsurePipelines(Rendering::IDevice& device);

    std::vector<MarkupDrawItem> m_Items; // this frame's mark-ups, kept to reuse the allocation
    std::vector<uint32> m_ExcludedScratch; // CollectMarkupDrawItems' scratch, kept to reuse the allocation
    std::size_t m_Bodies = 0;            // how many of m_Items draw a body
    std::vector<Draw> m_Draws;
    std::vector<std::pair<float, std::size_t>> m_Order; // BuildDraws' scratch: distance squared, item
    Rendering::DescriptorSetLayoutDesc m_GlowLayout{};
    Rendering::DescriptorSetLayoutDesc m_CompositeLayout{};
    GlowPipelines m_SingleSamplePipelines;
    GlowPipelines m_MultisamplePipelines;
    // The region meshes' pipelines (markup_glow_mesh, markup_glow_mesh_ms).
    GlowPipelines m_SingleSampleMeshPipelines;
    GlowPipelines m_MultisampleMeshPipelines;
    RegionBuffers m_RegionBuffers;
    Rendering::GraphicsPipelineId m_CompositePipeline{};
    bool m_PipelinesTried = false;
};

} // namespace GameEngine::Editor
