#include "Engine/Rendering/DDGIEmitterAreas.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Core/Engine.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::Engine::Renderer
{

float ComputeDDGIEmitterSurfaceArea(const Mesh& mesh, const Mathematics::Matrix4x4& transform)
{
    if (mesh.PrimitiveTopology != MeshPrimitiveTopology::Triangles)
        return 0.0f;
    const glm::dmat3 linear(transform.GetGLM());
    const bool indexed = !mesh.Indices.empty();
    const size_t count = indexed ? mesh.Indices.size() : mesh.Vertices.size();
    double area = 0.0;
    for (size_t i = 0; i + 2 < count; i += 3)
    {
        const size_t a = indexed ? mesh.Indices[i] : i;
        const size_t b = indexed ? mesh.Indices[i + 1] : i + 1;
        const size_t c = indexed ? mesh.Indices[i + 2] : i + 2;
        if (a >= mesh.Vertices.size() || b >= mesh.Vertices.size() || c >= mesh.Vertices.size())
            continue;
        const auto position = [&](size_t v)
        {
            const auto& p = mesh.Vertices[v].Position;
            return glm::dvec3(p[0], p[1], p[2]);
        };
        const glm::dvec3 ab = linear * (position(b) - position(a));
        const glm::dvec3 ac = linear * (position(c) - position(a));
        const double triangleArea = 0.5 * glm::length(glm::cross(ab, ac));
        if (std::isfinite(triangleArea))
            area += triangleArea;
    }
    return std::isfinite(area) && area <= std::numeric_limits<float>::max() ? static_cast<float>(area) : 0.0f;
}

float ComputeDDGIEmitterProjectedArea(float surfaceArea, bool doubleSided)
{
    // Cauchy: a surface's mean projected area is a quarter of the area that
    // faces outward, and a double-sided triangle faces outward twice.
    constexpr float kOneSidedFraction = 0.25f;
    constexpr float kDoubleSidedFraction = 0.5f;
    return surfaceArea * (doubleSided ? kDoubleSidedFraction : kOneSidedFraction);
}

void DDGIEmitterAreas::Update(const Rendering::MeshGPURegistry& registry,
                              std::span<const Rendering::GPUInstance> instances)
{
    if (std::none_of(instances.begin(), instances.end(), [](const auto& instance)
        { return (instance.flags & Rendering::kInstanceFlagGIEmitter) != 0u; }))
    {
        Clear();
        return;
    }

    // Snapshot identity only after a registry change, never every frame. The
    // outer scope makes the epoch and entries one coherent snapshot. Asset
    // resolution happens below, after releasing the registry lock.
    {
        Rendering::MeshGPURegistry::TableScope scope(registry);
        const auto revision = registry.GetContentRevision();
        if (m_Registry != &registry || m_Revision != revision)
        {
            m_Sources.clear();
            registry.ForEachKeyedEntry([&](const Rendering::MeshGPUKey& key,
                                           const Rendering::MeshGPUEntry& entry)
            {
                if (entry.gpuMeshIndex != ~0u)
                    m_Sources.emplace(entry.gpuMeshIndex,
                        Source{key.assetGuid, key.submeshIndex, entry.contentHash, entry.cpuMesh});
            });
            m_Registry = &registry;
            m_Revision = revision;
        }
    }

    m_NextEntries.clear();
    auto previous = m_Entries.begin();
    for (size_t index = 0; index < instances.size(); ++index)
    {
        const auto& instance = instances[index];
        if ((instance.flags & Rendering::kInstanceFlagGIEmitter) == 0u)
            continue;
        const auto found = m_Sources.find(instance.meshIndex);
        if (found == m_Sources.end())
            continue;
        auto& source = found->second;
        std::shared_ptr<const Mesh> geometry = source.Geometry.lock();
        if (!geometry)
        {
            auto owner = EngineCore::GetInstance().GetAssetManager().GetAsset(source.AssetGuid);
            const auto* model = dynamic_cast<const ModelAsset*>(owner.get());
            if (model && model->IsLoaded() && source.SubmeshIndex < model->GetMeshCount())
            {
                geometry = std::shared_ptr<const Mesh>(owner, &model->GetMesh(source.SubmeshIndex));
                source.Geometry = geometry;
            }
        }
        // No measurable triangle surface is different from a degenerate
        // triangle surface (whose real area is zero). Leave these entries
        // absent so the caller retains its bounding-sphere fallback.
        if (!geometry || geometry->PrimitiveTopology != MeshPrimitiveTopology::Triangles ||
            geometry->Vertices.size() < 3 ||
            (!geometry->Indices.empty() && geometry->Indices.size() < 3))
            continue;
        std::array<float, 9> linear{};
        for (size_t column = 0; column < 3; ++column)
            for (size_t row = 0; row < 3; ++row)
                linear[column * 3 + row] = instance.transform.Data()[column * 4 + row];
        while (previous != m_Entries.end() && previous->InstanceIndex < index)
            ++previous;
        const bool cached = previous != m_Entries.end() && previous->InstanceIndex == index;
        Entry& entry = m_NextEntries.emplace_back(cached ? *previous : Entry{});
        entry.InstanceIndex = index;
        if (!cached || entry.MeshIndex != instance.meshIndex ||
            entry.ContentHash != source.ContentHash || entry.RegistryRevision != m_Revision ||
            entry.LinearTransform != linear)
        {
            entry.MeshIndex = instance.meshIndex;
            entry.ContentHash = source.ContentHash;
            entry.RegistryRevision = m_Revision;
            entry.LinearTransform = linear;
            // CPU geometry is the bind pose for skinned emitters. Pose
            // deformation still needs a GPU area reduction; rigid and
            // non-uniformly scaled emitters use their actual mesh area.
            entry.SurfaceArea = ComputeDDGIEmitterSurfaceArea(*geometry, instance.transform);
        }
    }
    std::swap(m_Entries, m_NextEntries);
}

std::optional<float> DDGIEmitterAreas::GetSurfaceArea(size_t instanceIndex) const
{
    const auto found = std::lower_bound(m_Entries.begin(), m_Entries.end(), instanceIndex,
                                        [](const Entry& entry, size_t index) { return entry.InstanceIndex < index; });
    return found == m_Entries.end() || found->InstanceIndex != instanceIndex ? std::nullopt
                                                                             : std::optional(found->SurfaceArea);
}

}  // namespace GameEngine::Engine::Renderer
