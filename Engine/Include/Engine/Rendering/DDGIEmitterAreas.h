#pragma once

#include "AssetCore/GUID.h"
#include "Mathematics/Matrix4x4.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
struct Mesh;
namespace Rendering
{
class MeshGPURegistry;
struct GPUInstance;
}
}

namespace GameEngine::Engine::Renderer
{

// World-space area A of the mesh's triangles, each counted once. Measure
// transformed triangle edges, so translation, winding, non-uniform scale and
// shear are handled without using the bounding sphere as a surface-area estimate.
float ComputeDDGIEmitterSurfaceArea(const Mesh& mesh, const Mathematics::Matrix4x4& transform);

// Emitting area averaged over all view directions, which the proxy multiplies
// by radiance to carry the emitter's power: A/4 when each triangle emits from
// its front face, A/2 when a double-sided material emits from both faces.
float ComputeDDGIEmitterProjectedArea(float surfaceArea, bool doubleSided);

class DDGIEmitterAreas
{
  public:
    void Update(const Rendering::MeshGPURegistry& registry,
                std::span<const Rendering::GPUInstance> instances);
    // ComputeDDGIEmitterSurfaceArea of the instance's mesh; absent while the
    // instance has no measurable CPU triangle geometry.
    std::optional<float> GetSurfaceArea(size_t instanceIndex) const;
    void Clear()
    {
        m_Entries.clear();
        m_NextEntries.clear();
        m_Sources.clear();
        m_Registry = nullptr;
        m_Revision = 0;
    }

  private:
    struct Entry
    {
        size_t InstanceIndex = 0;
        uint32_t MeshIndex = 0;
        uint64_t ContentHash = 0;
        uint64_t RegistryRevision = 0;
        std::array<float, 9> LinearTransform{};
        float SurfaceArea = 0.0f;
    };
    // Sorted by InstanceIndex. Update walks instances in index order, so it
    // rebuilds the list into m_NextEntries by merging with the previous one.
    std::vector<Entry> m_Entries;
    std::vector<Entry> m_NextEntries;
    struct Source
    {
        GUID AssetGuid;
        uint32_t SubmeshIndex = 0;
        uint64_t ContentHash = 0;
        // Weak: the cache must not keep a model asset alive, and Update is
        // not called while the light list is full, so it cannot rely on
        // releasing references at the next registry change.
        std::weak_ptr<const Mesh> Geometry;
    };
    std::unordered_map<uint32_t, Source> m_Sources;
    const Rendering::MeshGPURegistry* m_Registry = nullptr;
    uint64_t m_Revision = 0;
};

}  // namespace GameEngine::Engine::Renderer
