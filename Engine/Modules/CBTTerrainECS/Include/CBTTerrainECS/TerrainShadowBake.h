#pragma once

#include "CBTTerrain/TerrainShadowGrid.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>
#include <memory>
#include <optional>
namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::CBTTerrainECS
{

// The terrain the CBT path renders this frame, as the shadow reads it (CBTRenderFeature sets it
// with the frame's height source).
struct TerrainShadowSource
{
    // The height texture (one texel per lattice sample) and its bindless index; invalid or 0 when
    // the terrain has no unified height texture (an atlas-backed terrain), which casts no map.
    Rendering::TextureHandle HeightTexture{};
    uint32_t HeightBindlessIndex = 0;
    // Terrain.CastShadows.
    bool CastShadows = true;
};

// What the bake reads of the frame's terrain: the CBT path's bound height source, transform and
// the edit rect it resolved for the frame.
struct TerrainShadowBakeInputs
{
    TerrainShadowSource Source{};
    // The terrain's corner (world X, Z), the world height of normalized 0, its extent in metres
    // and metres per normalized height.
    double TerrainX = 0.0;
    double TerrainZ = 0.0;
    double BaseY = 0.0;
    float SizeX = 0.0f;
    float SizeZ = 0.0f;
    float HeightScale = 0.0f;
    // This frame's edit rect in terrain UV [0, 1] (empty when MaxU <= MinU): the lines crossing it
    // are re-baked.
    float DirtyMinU = 0.0f;
    float DirtyMinV = 0.0f;
    float DirtyMaxU = 0.0f;
    float DirtyMaxV = 0.0f;
    // The pass that uploads the height texture this frame, which the bake orders after
    // (kInvalidId when none was declared in this graph).
    Rendering::RenderGraph::RGPassId HeightUploadPass = Rendering::RenderGraph::kInvalidId;
};

// The terrain's sun-space clearance map (terrain_shadow.glsl): owns the bake kernel and the map,
// declares a bake when the sun or the heights changed, and publishes the map to the shadow feature,
// which every receiver of the directional light reads it through (GE_SampleShadow, the ray-traced
// mask pass).
//
// The bake runs only when its inputs change: the sun's direction (the sky writes it when it leaves
// its hold band, so a static sun never re-bakes and an animated day re-bakes nearly every frame) or
// angular size, the terrain's transform or height texture, a fresh map, or an edit, which re-bakes
// only the lines crossing its rect. It is independent of CBT.Update, which is skipped at rest.
class TerrainShadowBake
{
  public:
    TerrainShadowBake();
    ~TerrainShadowBake();
    TerrainShadowBake(const TerrainShadowBake&) = delete;
    TerrainShadowBake& operator=(const TerrainShadowBake&) = delete;

    // Declares this frame's bake into `frame` (once per graph and frame, however many views call)
    // when its inputs changed, and publishes the map for `viewId`, whose world's primary
    // shadow-casting directional light is the sun. Publishes nothing when the terrain casts no
    // shadow, no sun casts one (none, below the horizon, near the zenith) or bindless textures are
    // off: the receivers then shade without the term.
    void DeclareForView(Engine::Renderer::RenderServices& rs, Rendering::RenderGraph::RGFrame& frame,
                        Rendering::ViewId viewId, uint64_t worldId, const TerrainShadowBakeInputs& inputs);

    // Drops every handle of the dead device; the next declaration re-creates them and bakes in full.
    void OnDeviceRebuilt();

  private:
    // What a full bake depends on; a change re-bakes every line.
    struct BakeKey
    {
        CBTTerrain::TerrainShadowGrid Grid{};
        CBTTerrain::TerrainShadowExtent Extent{};
        float HeightScale = 0.0f;
        uint64_t HeightTexture = 0;
        uint64_t Map = 0;

        bool operator==(const BakeKey&) const = default;
    };

    // This frame's declared map, shared by the views that follow the first.
    struct FrameMap
    {
        const void* Graph = nullptr;
        uint64_t FrameIndex = 0;
        bool Present = false;
        Rendering::RenderGraph::RGTexture Map{};
        CBTTerrain::TerrainShadowGrid Grid{};
    };

    bool EnsurePipeline(Rendering::IDevice& device);
    // Drops the map's bindless registration (a frame without a map).
    void ReleaseMapBindless(Engine::Renderer::RenderServices& rs);
    // Declares the map and the frame's bake into `frame`; false when no map can be published.
    bool DeclareFrameMap(Engine::Renderer::RenderServices& rs, Rendering::RenderGraph::RGFrame& frame,
                         uint64_t worldId, const TerrainShadowBakeInputs& inputs);
    void DeclareBakePass(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGTexture map,
                         const CBTTerrain::TerrainShadowGrid& grid, const TerrainShadowBakeInputs& inputs,
                         CBTTerrain::TerrainShadowLines lines);
    void Publish(Engine::Renderer::RenderServices& rs, Rendering::RenderGraph::RGFrame& frame,
                 Rendering::ViewId viewId, const TerrainShadowBakeInputs& inputs);

    bool m_LoadAttempted = false;
    Rendering::ComputePipelineId m_PipelineId{};
    std::unique_ptr<Rendering::ShaderMeta> m_Meta;
    Rendering::DescriptorSetLayoutDesc m_Set0Layout{};
    Rendering::SamplerHandle m_HeightSampler{};

    // The map's physical texture and its bindless registration (re-registered when the pool hands
    // out another physical).
    Rendering::TextureHandle m_MapTexture{};
    uint32_t m_MapBindlessIndex = 0;
    uint32_t m_MapSide = 0;
    // The inputs of the last declared full bake. A frame that declares a bake and is then abandoned
    // leaves the map one bake behind until the inputs next change.
    std::optional<BakeKey> m_Baked;
    FrameMap m_Frame{};
};

} // namespace GameEngine::CBTTerrainECS
