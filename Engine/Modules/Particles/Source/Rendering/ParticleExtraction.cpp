#include "Particles/Rendering/ParticleExtraction.h"

#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/ParticleRenderer.h"
#include "Components/Rendering/Particles.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/VectorOps.h"
#include "Particles/ParticleFlipbook.h"
#include "Particles/ParticleRuntime.h"
#include "Particles/Rendering/ParticleDrawSort.h"
#include "Particles/Rendering/ParticleMaterials.h"
#include "Particles/Rendering/ParticleRenderData.h"
#include "Particles/Systems/ParticleSimulationSystem.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/MatrixUtils.h"
#include "Rendering/Core/DeviceFrameCounter.h"
#include "Rendering/Core/HashUtils.h"
#include "Types/ScopedSubscription.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

namespace GameEngine::Particles
{
using Engine::Renderer::DrawBindings;
using Engine::Renderer::DrawCommand;
using Engine::Renderer::ExtractedLight;
using Engine::Renderer::ForwardEmitContext;
using Engine::Renderer::Material;
using Engine::Renderer::PrimitiveGenerator;
using Engine::Renderer::RenderServices;

namespace
{
using Math::Lerp;

inline void NormalizeParticleAxis(float* axis)
{
    // Preserve the particle basis fallback for collapsed transforms and trail tangents.
    if (Mathematics::Dot3(axis, axis) <= 1e-12f)
    {
        axis[0] = 0.0f;
        axis[1] = 1.0f;
        axis[2] = 0.0f;
        return;
    }
    Mathematics::Normalize3(axis);
}

float ParticleAxisLength(const Mathematics::Vector4& axis)
{
    return Mathematics::Vector3{axis.x, axis.y, axis.z}.Length();
}

// Half the width of a trail whose particle size does not set it: half a particle of size 1.
constexpr float kTrailHalfWidth = 0.5f;
// Shortest tick a motion vector divides by, so a zero-length tick yields no infinite speed.
constexpr float kMinimumTickSeconds = 1.0e-6f;

float TrailPointWidth(const Particles::ParticleTrailPoint& point, const Components::ParticleRenderer& renderer,
                      float width)
{
    return std::max(0.0f, renderer.TrailSizeAffectsWidth ? point.Size : kTrailHalfWidth) * width;
}

Mathematics::Vector4 TrailTangent(const Particles::ParticleTrailStore& trails, uint32 slot, uint32 index,
                                  const Components::ParticleRenderer& renderer, float width)
{
    const uint32 count = trails.PointCount(slot);
    const auto& before = trails.Point(slot, index ? index - 1 : index);
    const auto& after = trails.Point(slot, std::min(index + 1, count - 1));
    auto tangent = after.Position - before.Position;
    NormalizeParticleAxis(&tangent.x);
    return Mathematics::Vector4(tangent * TrailPointWidth(trails.Point(slot, index), renderer, width), 0.0f);
}

// A direction through the emitter rotation and scale, without its translation.
Mathematics::Vector3 EmitterVector(const Mathematics::Matrix4x4& transform, const Mathematics::Vector3& vector)
{
    const auto rotated = transform.Transform(Mathematics::Vector4(vector, 0.0f));
    return {rotated.x, rotated.y, rotated.z};
}

float TrailAlpha(const Particles::ParticleTrailPoint& point, float normalized, double elapsed, float lifetime,
                 const Components::ParticleRenderer& renderer)
{
    const float fade = renderer.TrailFadeOverLength
                           ? std::max(0.0f, 1.0f - std::abs(2.0f * normalized - 1.0f))
                           : static_cast<float>(std::clamp(1.0 - (elapsed - point.Time) / std::max(0.001, static_cast<double>(lifetime)), 0.0, 1.0));
    return point.Color.w * fade * renderer.TrailAlphaPeak;
}

// What an emitter without a ParticleRenderer draws with.
const Components::ParticleRenderer kDefaultRenderer{};

// How often a material asset an emitter draws from is checked for edits, in extraction frames.
constexpr uint64 kSourceMaterialCheckInterval = 30u;

// Distinguishes the material GUIDs of extractions that share one RenderServices (one per world).
uint64 NextExtractionInstance()
{
    static std::atomic<uint64> next{1u};
    return next.fetch_add(1u, std::memory_order_relaxed);
}

} // namespace

struct ParticleExtraction::Impl
{
    // Deepest pacing any backend reports, plus one for the update phase. Fixed
    // rather than derived so the ring arrays stay plain std::array.
    static constexpr std::size_t kSortedIndirectionSlots =
        GameEngine::Rendering::IDevice::kMaxSupportedFramesInFlight + 1u;

    struct EmitterRenderCache
    {
        // Per-view sorted indirection ring, one element per device frame. Its depth
        // is kSortedIndirectionSlots, which exceeds the device's pacing by the
        // update-phase margin Extract is written in (FrameBufferAllocator::BeginFrame
        // states the rule), so the element being written is never one a frame in
        // flight still reads.
        struct SortedViewBuffer
        {
            std::array<GameEngine::Rendering::BufferHandle, kSortedIndirectionSlots> IndirectionBuffers{};
            std::array<uint32, kSortedIndirectionSlots> Capacities{};

            void Release(GameEngine::Rendering::IDevice& device)
            {
                for (auto& buffer : IndirectionBuffers)
                {
                    if (buffer.IsValid())
                        device.DestroyBuffer(buffer);
                    buffer = {};
                }
                Capacities.fill(0);
            }
        };

        uint64 LastSeenFrame = 0u;
        // The simulation clock, the interpolation between its ticks and the transform this emitter
        // drew with last frame. Particles move on screen only when one of them changed (a paused
        // emitter or simulation keeps all three).
        double LastElapsed = -1.0;
        float LastInterpolation = -1.0f;
        std::array<float, 16> LastTransform{};
        GUID MaterialGuid = GUID::Null();
        // The effective document the emitter draws with, serialized: its key in m_SharedMaterials.
        std::string MaterialDescription;
        Components::ParticleRenderer MaterialRenderer{};
        bool MaterialInitialized = false;
        GUID RequestedMaterial{};
        std::array<GUID, Components::ParticleMaxMeshes> RequestedMeshes{};
        // The revision of the source material asset the document was built from.
        uint64 SourceRevision = 0;
    };

    // One registered material per distinct effective document, shared by every emitter drawing
    // with it so their particles merge into the same draws. A document no emitter draws with any
    // more gives its GUID back, and the next new document re-registers that GUID in place: the
    // registry holds at most as many particle materials as there were distinct documents at once,
    // however many values an author drags through.
    struct SharedMaterial
    {
        GUID Guid;
        uint32 Users = 0;
    };

    // A material asset emitters draw from: its serialized document and a revision that steps when
    // the document changes. One poll per asset serves every emitter using it.
    struct SourceMaterial
    {
        std::string Description;
        uint64 Revision = 0;
        uint64 NextCheckFrame = 0;
    };

    struct ParticleInstancedPushConstants
    {
        uint64 IndirectionAddress = 0;
        uint64 GpuInstancesAddress = 0;
    };

    std::unordered_map<ECS::EntityId, EmitterRenderCache> m_EmitterCaches;
    std::unordered_map<std::string, SharedMaterial> m_SharedMaterials;
    std::vector<GUID> m_RecycledMaterialGuids;
    uint32 m_IssuedMaterialGuids = 0;
    const uint64 m_Instance = NextExtractionInstance();
    std::unordered_map<GUID, SourceMaterial> m_SourceMaterials;
    std::shared_ptr<ParticleWorldState> m_ParticleState;
    std::unique_ptr<ParticleSimulationSystem> m_FallbackSimulation;
    EmitterRenderCache::SortedViewBuffer m_ParticleBuffers;
    std::unordered_map<GameEngine::Rendering::ViewId, EmitterRenderCache::SortedViewBuffer> m_ViewBuffers;
    struct DrawItem
    {
        const Material* MaterialPtr = nullptr;
        GameEngine::Rendering::MeshGPUHandle Mesh{};
        ECS::EntityId Emitter = 0;
        uint32 SpawnIndex = 0, LayerMask = 1;
        // The emitter's particles moved since the previous frame (its clock or transform changed).
        bool Moving = false;
        Components::ParticleDrawOrder Order = Components::ParticleDrawOrder::ViewDepth;
        float Age = 0, Radius = 1, LodStart = 0, LodEnd = 0;
        Mathematics::Vector3 EmitterPosition{};
    };
    std::vector<ParticleRenderData> m_Particles;
    std::vector<ParticleRenderData> m_CompatViewParticles;
    std::vector<DrawItem> m_DrawItems;
    std::vector<uint32> m_Indices;
    std::vector<ECS::EntityId> m_EmitterOrder;
    std::vector<float> m_Depths;
    std::vector<const Rendering::ViewDesc*> m_MatchingViews;
    // Draw commands hold spans into these until graph declaration, so each is reserved for the
    // whole frame before the first command is built and never reallocates while it is filled.
    std::vector<ParticleInstancedPushConstants> m_ParticlePushConstants;
    std::vector<uint32> m_CompatBaseParticles;
    std::vector<DrawBindings::BufferEntry> m_CompatParticleBindings;
    // Emptied, not erased, every frame so each view's command storage is kept.
    std::unordered_map<GameEngine::Rendering::ViewId, std::vector<DrawCommand>> m_ParticleForwardCommands;
    // RAII: unregisters on destruction, safe if RenderServices died first.
    ScopedSubscription m_ParticleForwardProducer;
    uint64 m_ParticleFrameCounter = 0u;
    // Unwraps the device's frame slot so all kSortedIndirectionSlots elements
    // rotate: the slot's VALUE would cap the rotation at the device's pacing.
    GameEngine::Rendering::DeviceFrameCounter m_SortedIndirectionFrameCounter;

    RenderServices* m_RenderServices = nullptr;
    std::weak_ptr<std::atomic<bool>> m_RenderServicesAlive;

    struct FrameContext
    {
        RenderServices* Services;
        Rendering::MeshGPUHandle SpriteMesh;
        uint64 FrameId;
        uint64 WorldId;
        uint32 RecordBudget;
        uint32& ParticleLights;
    };

    Rendering::BufferHandle EnsureBuffer(Rendering::IDevice* device, uint32 frameSlot, bool useDeviceAddress,
                                         EmitterRenderCache::SortedViewBuffer& buffers, uint32 count,
                                         size_t stride, const char* name)
    {
        auto& buffer = buffers.IndirectionBuffers[frameSlot];
        auto& capacity = buffers.Capacities[frameSlot];
        if (buffer.IsValid() && capacity >= count)
            return buffer;
        if (buffer.IsValid())
            device->DestroyBuffer(buffer);
        capacity = std::bit_ceil(std::max(count, 64u));
        Rendering::BufferDesc desc{};
        desc.size = static_cast<size_t>(capacity) * stride;
        desc.usage = static_cast<uint32>(Rendering::BufferUsage::Storage) |
                     static_cast<uint32>(Rendering::BufferUsage::TransferDst);
        if (useDeviceAddress)
            desc.usage |= static_cast<uint32>(Rendering::BufferUsage::ShaderDeviceAddress);
        desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        desc.persistent = true;
        desc.debugName = name;
        buffer = device->CreateBuffer(desc);
        return buffer;
    }

    ParticleSortKey SortKey(uint32 index) const
    {
        const auto& item = m_DrawItems[index];
        return {.Depth = m_Depths[index], .Emitter = item.Emitter, .Order = item.Order, .Age = item.Age, .SpawnIndex = item.SpawnIndex};
    }

    bool CompareParticleIndices(uint32 a, uint32 b) const
    {
        const ParticleSortKey left = SortKey(a);
        const ParticleSortKey right = SortKey(b);
        if (DrawsBefore(left, right))
            return true;
        if (DrawsBefore(right, left))
            return false;
        return a < b;
    }

    GUID IssueMaterialGuid()
    {
        if (!m_RecycledMaterialGuids.empty())
        {
            const GUID guid = m_RecycledMaterialGuids.back();
            m_RecycledMaterialGuids.pop_back();
            return guid;
        }
        return GUID::Derive(GUID::Null(), "engine/particles/" + std::to_string(m_Instance) + "/" +
                                              std::to_string(m_IssuedMaterialGuids++));
    }

    void ReleaseMaterial(EmitterRenderCache& cache)
    {
        if (cache.MaterialDescription.empty())
            return;
        const auto found = m_SharedMaterials.find(cache.MaterialDescription);
        if (found != m_SharedMaterials.end() && --found->second.Users == 0)
        {
            m_RecycledMaterialGuids.push_back(found->second.Guid);
            m_SharedMaterials.erase(found);
        }
        cache.MaterialDescription.clear();
        cache.MaterialGuid = GUID::Null();
    }

    // The registered material for `doc`, shared with every emitter drawing the same document.
    Material* AcquireMaterial(RenderServices& services, EmitterRenderCache& cache, const MaterialDocument& doc)
    {
        std::string description = SerializeMaterialDocument(doc).dump();
        if (description != cache.MaterialDescription)
        {
            ReleaseMaterial(cache);
            auto [shared, added] = m_SharedMaterials.try_emplace(description);
            if (added)
                shared->second.Guid = IssueMaterialGuid();
            ++shared->second.Users;
            cache.MaterialGuid = shared->second.Guid;
            cache.MaterialDescription = std::move(description);
            if (added)
                return services.Materials().RegisterMaterialFromDocument(cache.MaterialGuid, doc);
        }
        Material* material = services.Materials().Registry().Find(cache.MaterialGuid);
        return material ? material : services.Materials().RegisterMaterialFromDocument(cache.MaterialGuid, doc);
    }

    // Whether the material asset `guid` changed since the emitter last built its document from it.
    bool SourceMaterialChanged(EmitterRenderCache& cache, const GUID& guid, const MaterialAsset& asset,
                               uint64 frameId)
    {
        SourceMaterial& source = m_SourceMaterials[guid];
        if (frameId >= source.NextCheckFrame)
        {
            std::string description = SerializeMaterialDocument(asset.GetDocument()).dump();
            if (description != source.Description)
            {
                source.Description = std::move(description);
                ++source.Revision;
            }
            source.NextCheckFrame = frameId + kSourceMaterialCheckInterval;
        }
        const bool changed = cache.SourceRevision != source.Revision;
        cache.SourceRevision = source.Revision;
        return changed;
    }

    void AppendParticle(const FrameContext& frame, ParticleRenderData data, DrawItem item,
                        uint32 spawnIndex, float age, float radius, Rendering::MeshGPUHandle mesh, bool ribbon)
    {
        if (!mesh.IsValid() || m_Particles.size() >= frame.RecordBudget)
            return;
        data.Metadata[0] = item.MaterialPtr->GetGpuSceneMaterialIndex();
        data.Metadata[2] = 2u; // receive scene shadows
        if (ribbon)
            data.Metadata[3] = 2u;
        item.Mesh = mesh;
        item.SpawnIndex = spawnIndex;
        item.Age = age;
        item.Radius = radius;
        m_Particles.push_back(data);
        m_DrawItems.push_back(item);
    }

    void ProcessEmitter(const FrameContext& frame, std::unordered_map<ECS::EntityId, EmitterRenderCache>& runtimes, ECS::EntityHandle entity,
                        const Components::WorldTransform& xf, const Components::ParticleEmitter3D& emitter,
                        const Components::ParticleRenderer& renderer)
    {
        auto* rs = frame.Services;
        auto& meshReg = rs->GetMeshGPURegistry();
        const auto meshHandle = frame.SpriteMesh;
        const auto frameId = frame.FrameId;
        const auto recordBudget = frame.RecordBudget;
        const auto worldId = frame.WorldId;
        auto& particleLights = frame.ParticleLights;
        auto& cache = runtimes[entity.id];
        cache.LastSeenFrame = frameId;
        auto found = m_ParticleState->Emitters.find(entity.id);
        if (found == m_ParticleState->Emitters.end())
            return;
        const Particles::ParticleRuntime* simulation = &found->second.Simulation;
        const bool moving = simulation->Elapsed() != cache.LastElapsed ||
                            simulation->Interpolation() != cache.LastInterpolation ||
                            !std::equal(cache.LastTransform.begin(), cache.LastTransform.end(), xf.matrix);
        cache.LastElapsed = simulation->Elapsed();
        cache.LastInterpolation = simulation->Interpolation();
        std::copy_n(xf.matrix, 16, cache.LastTransform.begin());
        // A prewarming emitter shows nothing until its prewarm completes, rather than fast-forwarding.
        if (simulation->IsPrewarming() || (simulation->Count() == 0 && simulation->Trails().ActiveCount() == 0))
            return;
        bool sourceChanged = false;
        std::shared_ptr<MaterialAsset> sourceMaterial;
        if (auto* assets = EngineCore::GetInstance().TryGetAssetManager())
        {
            const auto guid = renderer.Material.ToGuid();
            if (!guid.IsNull())
            {
                sourceMaterial = std::dynamic_pointer_cast<MaterialAsset>(assets->GetAsset(guid));
                if (!sourceMaterial && cache.RequestedMaterial != guid)
                {
                    cache.RequestedMaterial = guid;
                    assets->LoadAsset(guid, [](auto) {});
                }
                if (sourceMaterial)
                    sourceChanged = SourceMaterialChanged(cache, guid, *sourceMaterial, frameId);
            }
        }
        const bool materialChanged = !cache.MaterialInitialized || sourceChanged || !(cache.MaterialRenderer == renderer);
        Material* material = cache.MaterialInitialized ? rs->Materials().Registry().Find(cache.MaterialGuid) : nullptr;
        if (materialChanged || !material)
        {
            const MaterialDocument doc = BuildParticleRenderMaterialDocument(
                renderer, sourceMaterial ? &sourceMaterial->GetDocument() : nullptr);
            material = AcquireMaterial(*rs, cache, doc);
            cache.MaterialRenderer = renderer;
            cache.MaterialInitialized = true;
        }
        if (!material || !material->GetGraphicsPipelineId().IsValid() || material->GetGpuSceneMaterialIndex() == 0xFFFFFFFFu)
            return;

        // Every assigned mesh draws; an emitter with none draws sprites.
        std::array<Rendering::MeshGPUHandle, Components::ParticleMaxMeshes> meshes{};
        uint32 meshCount = 0;
        for (uint32 slot = 0; slot < Components::ParticleMaxMeshes; ++slot)
        {
            const auto guid = renderer.Meshes[slot].ToGuid();
            if (guid.IsNull())
                continue;
            auto handle = meshReg.FindHandle({guid, 0u});
            auto* assets = EngineCore::GetInstance().TryGetAssetManager();
            if (!handle.IsValid() && assets)
            {
                auto asset = assets->GetAsset(guid);
                if (!asset && cache.RequestedMeshes[slot] != guid)
                {
                    cache.RequestedMeshes[slot] = guid;
                    assets->LoadAsset(guid, [](auto) {});
                }
                if (auto model = std::dynamic_pointer_cast<ModelAsset>(asset))
                {
                    auto handles = meshReg.RegisterModelMeshes(guid, *model);
                    if (!handles.empty())
                        handle = handles.front();
                }
            }
            if (handle.IsValid())
                meshes[meshCount++] = handle;
        }
        const bool sprite = meshCount == 0;
        if (sprite)
            meshes[meshCount++] = meshHandle;
        DrawItem item;
        item.MaterialPtr = material;
        item.Emitter = entity.id;
        item.Moving = moving;
        item.Order = renderer.DrawOrder;
        item.LayerMask = renderer.RenderLayerMask;
        item.LodStart = renderer.ThinningStart;
        item.LodEnd = renderer.ThinningEnd;
        std::copy_n(xf.matrix + 12, 3, &item.EmitterPosition.x);
        const Mathematics::Matrix4x4 emitterTransform{glm::make_mat4(xf.matrix)};
        const bool local = emitter.LocalSpace;
        const auto& channels = simulation->Channels();
        const float interpolation = simulation->Interpolation();
        const auto ages = channels.Ages();
        const auto previousAges = channels.Get<float>(Particles::ParticleChannel::PreviousAge);
        const auto lifetimes = channels.Lifetimes();
        const auto positions = channels.Positions();
        const auto previousPositions = channels.Get<Mathematics::Vector3>(Particles::ParticleChannel::PreviousPosition);
        const auto velocities = channels.Velocities();
        const auto sizes = channels.Sizes();
        const auto previousSizes = channels.Get<float>(Particles::ParticleChannel::PreviousSize);
        const auto scales = channels.Scales();
        const auto previousScales = channels.Get<Mathematics::Vector3>(Particles::ParticleChannel::PreviousScale);
        const auto colors = channels.Colors();
        const auto previousColors = channels.Get<Mathematics::Vector4>(Particles::ParticleChannel::PreviousColor);
        const auto rotations = channels.Rotations();
        const auto previousRotations = channels.Get<float>(Particles::ParticleChannel::PreviousRotation);
        const auto animationSpeeds = channels.Get<float>(Particles::ParticleChannel::AnimationSpeed);
        const auto animationOffsets = channels.Get<float>(Particles::ParticleChannel::AnimationOffset);
        const auto spawns = channels.SpawnIndices();
        const bool lights = channels.Has(Particles::ParticleChannel::LightIntensity);
        const auto lightIntensities = lights ? channels.Get<float>(Particles::ParticleChannel::LightIntensity) : std::span<const float>{};
        const auto lightRanges = lights ? channels.Get<float>(Particles::ParticleChannel::LightRange) : std::span<const float>{};
        for (uint32 index = 0; index < channels.Count(); ++index)
        {
            if (m_Particles.size() >= recordBudget)
                break;
            ParticleRenderData data;
            const Mathematics::Vector3 simulated = simulation->InterpolatedPosition(index);
            const Mathematics::Vector3 position = local ? emitterTransform.TransformPoint(simulated) : simulated;
            if (lights && lightIntensities[index] > 0 && lightRanges[index] > 0 && particleLights < 64)
            {
                ExtractedLight light;
                light.type = Components::LightType::Point;
                std::copy_n(&position.x, 3, light.positionWS);
                light.range = lightRanges[index];
                light.intensity = lightIntensities[index];
                for (int channel = 0; channel < 3; ++channel)
                    light.color[channel] = std::max(0.f, colors[index][channel]);
                light.castsShadows = 0;
                light.SortId = entity.id * 16777619u ^ spawns[index];
                rs->SubmitLight(worldId, light);
                ++particleLights;
            }
            const float age = Lerp(previousAges[index], ages[index], interpolation);
            const float t = std::clamp(age / std::max(lifetimes[index], 0.001f), 0.0f, 1.0f);
            const float size = Lerp(previousSizes[index], sizes[index], interpolation);
            data.PositionSize = {position.x, position.y, position.z, size};
            const Mathematics::Vector3 velocity = local ? EmitterVector(emitterTransform, velocities[index]) : velocities[index];
            data.VelocityAngle = {velocity.x, velocity.y, velocity.z, Lerp(previousRotations[index], rotations[index], interpolation)};
            data.Color = Mathematics::Vector4(glm::mix(static_cast<glm::vec4>(previousColors[index]),
                                                       static_cast<glm::vec4>(colors[index]), interpolation));
            data.AxisZ[3] = static_cast<float>(spawns[index] % std::max(1u, emitter.Amount));
            data.Animation[0] = age;
            data.Animation[3] = static_cast<float>((spawns[index] * 747796405u) & 0xFFFFFFu) / 16777216.0f;
            data.Animation[2] = std::max(0.0f, renderer.VelocityStretch);
            data.Animation[1] = Particles::FlipbookFrame(renderer, age, lifetimes[index], animationSpeeds[index],
                                                         animationOffsets[index]);
            data.Metadata[1] = static_cast<uint32>(renderer.Billboard);
            // Local alignment uses the emitter orientation; other billboards take
            // their basis from each draw camera in GLSL.
            for (int a = 0; a < 3; ++a)
            {
                data.AxisX[a] = xf.matrix[a];
                data.AxisY[a] = xf.matrix[4 + a];
                data.AxisZ[a] = xf.matrix[8 + a];
            }
            if (!renderer.InheritScale)
            {
                NormalizeParticleAxis(&data.AxisX.x);
                NormalizeParticleAxis(&data.AxisY.x);
                NormalizeParticleAxis(&data.AxisZ.x);
            }
            const Mathematics::Vector3 scale = previousScales[index] + (scales[index] - previousScales[index]) * interpolation;
            for (int axis = 0; axis < 3; ++axis)
            {
                data.AxisX[axis] *= scale.x;
                data.AxisY[axis] *= scale.y;
                data.AxisZ[axis] *= scale.z;
            }
            // Noise and other position processors move particles without changing physical velocity;
            // velocity-facing meshes follow the actual displacement.
            if (renderer.Billboard == Components::ParticleBillboard::ZAlongVelocity)
            {
                const Mathematics::Vector3 motion = (positions[index] - previousPositions[index]) /
                                                    std::max(simulation->LastTickSeconds(), kMinimumTickSeconds);
                data.VelocityAngle = Mathematics::Vector4(local ? EmitterVector(emitterTransform, motion) : motion,
                                                          data.VelocityAngle.w);
            }
            const float speed = std::sqrt(Mathematics::Dot3(&data.VelocityAngle.x, &data.VelocityAngle.x));
            const float spriteScale = std::max(ParticleAxisLength(data.AxisX), ParticleAxisLength(data.AxisY));
            const float radius = std::max(0.01f, std::abs(size) * spriteScale * 0.8f + speed * data.Animation[2]);
            if (renderer.TrailOnly)
                continue;
            for (uint32 pass = 0; pass < meshCount; ++pass)
            {
                data.Metadata[3] = sprite ? 0u : 1u;
                float boundsRadius = radius;
                if (!sprite)
                    if (const auto* mesh = meshReg.Find(meshes[pass]))
                    {
                        const auto& c = mesh->bounds.center;
                        const float extent = mesh->bounds.Radius() + std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
                        boundsRadius = std::max(boundsRadius, std::abs(size) * extent *
                                                                  (ParticleAxisLength(data.AxisX) + ParticleAxisLength(data.AxisY) + ParticleAxisLength(data.AxisZ)));
                    }
                AppendParticle(frame, data, item, spawns[index], t, boundsRadius, meshes[pass], false);
            }
        }
        const auto& trails = simulation->Trails();
        if (trails.ActiveCount() > 0)
        {
            const float trailLifetime = simulation->Stack()->Trail.Lifetime;
            // Trail points are recorded in world space.
            for (uint32 slot = 0; slot < trails.SlotCount(); ++slot)
            {
                if (!trails.IsActive(slot))
                    continue;
                const uint32 id = trails.ParticleId(slot);
                const uint32 count = trails.PointCount(slot);
                if (count < 2)
                    continue;
                const auto& first = trails.Point(slot, 0);
                const auto& last = trails.Point(slot, count - 1);
                const float span = std::max(last.Distance - first.Distance, 0.0001f);
                const float randomWidth = float((id * 1664525u + 1013904223u) & 0xffffu) / 65535.0f;
                const float width = Lerp(renderer.TrailWidthMin, renderer.TrailWidthMax, randomWidth);
                for (uint32 i = 1; i < count; ++i)
                {
                    if (m_Particles.size() >= recordBudget)
                        break;
                    const auto& from = trails.Point(slot, i - 1);
                    const auto& to = trails.Point(slot, i);
                    ParticleRenderData data;
                    for (int a = 0; a < 3; ++a)
                        data.PositionSize[a] = (from.Position[a] + to.Position[a]) * 0.5f;
                    Mathematics::Sub3(&to.Position.x, &from.Position.x, &data.VelocityAngle.x);
                    data.PositionSize[3] = 1.0f;
                    data.Animation[3] = std::sqrt(Mathematics::Dot3(&data.VelocityAngle.x, &data.VelocityAngle.x));
                    if (data.Animation[3] < 1e-5f)
                        continue;
                    const float t0 = (from.Distance - first.Distance) / span;
                    const float t1 = (to.Distance - first.Distance) / span;
                    data.AxisX = TrailTangent(trails, slot, i - 1, renderer, width);
                    data.AxisY = TrailTangent(trails, slot, i, renderer, width);
                    data.AxisX.w = TrailAlpha(from, t0, simulation->Elapsed(), trailLifetime, renderer);
                    data.AxisY.w = TrailAlpha(to, t1, simulation->Elapsed(), trailLifetime, renderer);
                    const Mathematics::Vector4 white{1, 1, 1, 1};
                    const auto& fromColor = renderer.TrailInheritColor ? from.Color : white;
                    const auto& toColor = renderer.TrailInheritColor ? to.Color : white;
                    for (int a = 0; a < 3; ++a)
                        data.Color[a] = (fromColor[a] + toColor[a]) * 0.5f;
                    data.Color[3] = 1.0f;
                    data.Animation[0] = (renderer.TrailTextureTile ? from.Distance : t0) * renderer.TrailTextureScaleU;
                    data.Animation[1] = (renderer.TrailTextureTile ? to.Distance : t1) * renderer.TrailTextureScaleU;
                    data.Animation[2] = renderer.TrailTextureScaleV;
                    data.Metadata[1] = 3u;
                    const float radius = data.Animation[3] * 0.5f + std::max(
                                                                        TrailPointWidth(from, renderer, width),
                                                                        TrailPointWidth(to, renderer, width));
                    AppendParticle(frame, data, item, id, t1, radius, meshHandle, true);
                }
            }
        }
    }
};

ParticleExtraction::ParticleExtraction() : m_Impl(std::make_unique<Impl>()) {}
void ParticleExtraction::SetSimulationState(std::shared_ptr<ParticleWorldState> state)
{
    m_Impl->m_ParticleState = std::move(state);
    m_Impl->m_FallbackSimulation.reset();
}
ParticleExtractionStats ParticleExtraction::Stats() const
{
    ParticleExtractionStats stats;
    stats.EmitterCount = static_cast<uint32>(m_Impl->m_EmitterCaches.size());
    if (const auto& state = m_Impl->m_ParticleState)
    {
        stats.LiveParticles = state->LiveParticles;
        stats.SimulationMs = state->SimulationMs;
    }
    return stats;
}

ParticleExtraction::~ParticleExtraction()
{
    const auto alive = m_Impl->m_RenderServicesAlive.lock();
    if (!alive || !alive->load(std::memory_order_acquire) || !m_Impl->m_RenderServices)
        return;
    auto* device = m_Impl->m_RenderServices->GetDevice();
    if (!device)
        return;
    m_Impl->m_ParticleBuffers.Release(*device);
    for (auto& [view, buffers] : m_Impl->m_ViewBuffers)
        buffers.Release(*device);
}

void ParticleExtraction::EmitForwardCommands(ForwardEmitContext& context)
{
    if (!context.Services)
        return;
    auto found = m_Impl->m_ParticleForwardCommands.find(context.ViewId);
    if (found == m_Impl->m_ParticleForwardCommands.end())
        return;
    for (const auto& command : found->second)
        context.Services->EmitLateForwardCommand(context.ViewId, command);
}

void ParticleExtraction::Extract(ECS::World& world, uint64 worldId, RenderServices* rs, float32 deltaTime)
{
    // Direct users of extraction (tests and custom tools) receive a local CPU
    // simulation. The engine schedule supplies the shared, independently ticked state.
    if (!m_Impl->m_ParticleState)
    {
        m_Impl->m_FallbackSimulation = std::make_unique<ParticleSimulationSystem>();
        m_Impl->m_ParticleState = m_Impl->m_FallbackSimulation->State();
    }
    if (m_Impl->m_FallbackSimulation)
        m_Impl->m_FallbackSimulation->Update(world, deltaTime);
    for (auto& [view, commands] : m_Impl->m_ParticleForwardCommands)
        commands.clear();
    m_Impl->m_ParticlePushConstants.clear();
    m_Impl->m_CompatBaseParticles.clear();
    m_Impl->m_CompatParticleBindings.clear();
    m_Impl->m_Particles.clear();
    m_Impl->m_DrawItems.clear();
    if (!rs || !rs->GetDevice())
        return;
    m_Impl->m_RenderServices = rs;
    m_Impl->m_RenderServicesAlive = rs->LifetimeToken();
    auto* device = rs->GetDevice();
    if (!m_Impl->m_ParticleForwardProducer)
        m_Impl->m_ParticleForwardProducer = rs->RegisterForwardEmit(
            [this](ForwardEmitContext& context)
            { EmitForwardCommands(context); }, false);
    const auto& views = rs->Views().GetViews();
    auto& matchingViews = m_Impl->m_MatchingViews;
    matchingViews.clear();
    for (const auto& view : views)
        if ((view.worldId == 0 || view.worldId == worldId) && view.ActiveRenderLayerMask() != 0)
            matchingViews.push_back(&view);
    if (matchingViews.empty())
        return;
    auto& meshReg = rs->GetMeshGPURegistry();
    auto meshHandle = meshReg.FindHandle({PrimitiveGenerator::PlaneSpriteUvGuid(), 0u});
    if (!meshHandle.IsValid())
        meshHandle = meshReg.RegisterSubmesh({PrimitiveGenerator::PlaneSpriteUvGuid(), 0u}, PrimitiveGenerator::GeneratePlaneSpriteUv());
    const uint64 frameId = ++m_Impl->m_ParticleFrameCounter;
    const uint32 frameSlot =
        m_Impl->m_SortedIndirectionFrameCounter.Tick(device->GetFrameIndex()) % Impl::kSortedIndirectionSlots;
    const uint32 recordBudget = std::min(m_Impl->m_ParticleState->Budget * 4u, 262144u);
    uint32 particleLights = 0;

    const Impl::FrameContext frame{rs, meshHandle, frameId, worldId, recordBudget, particleLights};
    m_Impl->m_EmitterOrder.clear();
    for (const auto& [id, entry] : m_Impl->m_ParticleState->Emitters)
        m_Impl->m_EmitterOrder.push_back(id);
    std::sort(m_Impl->m_EmitterOrder.begin(), m_Impl->m_EmitterOrder.end());
    for (const auto id : m_Impl->m_EmitterOrder)
    {
        const auto& entry = m_Impl->m_ParticleState->Emitters.at(id);
        // A switched-off renderer draws nothing; an emitter without one draws the default sprites.
        const auto* renderer = world.GetComponent<Components::ParticleRenderer>(entry.Entity);
        if (renderer && !ECS::Entity(&world, entry.Entity).IsEnabled<Components::ParticleRenderer>())
            continue;
        Components::WorldTransform xf{};
        std::copy_n(entry.Transform.Data(), 16, xf.matrix);
        m_Impl->ProcessEmitter(frame, m_Impl->m_EmitterCaches, entry.Entity, xf, entry.Emitter,
                               renderer ? *renderer : kDefaultRenderer);
    }
    for (auto it = m_Impl->m_EmitterCaches.begin(); it != m_Impl->m_EmitterCaches.end();)
    {
        if (it->second.LastSeenFrame == frameId)
        {
            ++it;
            continue;
        }
        m_Impl->ReleaseMaterial(it->second);
        it = m_Impl->m_EmitterCaches.erase(it);
    }
    const auto viewExists = [&views](GameEngine::Rendering::ViewId viewId)
    { return std::any_of(views.begin(), views.end(), [viewId](const auto& view)
                         { return view.id == viewId; }); };
    for (auto it = m_Impl->m_ViewBuffers.begin(); it != m_Impl->m_ViewBuffers.end();)
    {
        if (viewExists(it->first))
        {
            ++it;
            continue;
        }
        it->second.Release(*device);
        it = m_Impl->m_ViewBuffers.erase(it);
    }
    std::erase_if(m_Impl->m_ParticleForwardCommands, [&viewExists](const auto& entry)
                  { return !viewExists(entry.first); });
    if (m_Impl->m_Particles.empty())
        return;
    const bool useDeviceAddress = device->GetCapabilities().supportsBufferDeviceAddress &&
                                  !Rendering::IsCompatShaderProfile();
    uint64 particleAddress = 0;
    if (useDeviceAddress)
    {
        auto particleBuffer = m_Impl->EnsureBuffer(device, frameSlot, useDeviceAddress, m_Impl->m_ParticleBuffers, static_cast<uint32>(m_Impl->m_Particles.size()), sizeof(ParticleRenderData), "ParticleData");
        if (!particleBuffer.IsValid())
            return;
        device->UpdateBuffer(particleBuffer, 0, m_Impl->m_Particles.size() * sizeof(ParticleRenderData), m_Impl->m_Particles.data());
        particleAddress = device->GetBufferDeviceAddress(particleBuffer);
        if (particleAddress == 0)
            return;
    }
    m_Impl->m_Depths.resize(m_Impl->m_Particles.size());
    m_Impl->m_ParticlePushConstants.reserve(matchingViews.size());
    if (!useDeviceAddress)
    {
        m_Impl->m_CompatParticleBindings.reserve(matchingViews.size());
        // A view draws at most one run per particle.
        m_Impl->m_CompatBaseParticles.reserve(m_Impl->m_Particles.size() * matchingViews.size());
    }
    for (const auto* view : matchingViews)
    {
        const auto* camera = rs->Views().FindCameraData(view->cameraId);
        if (!camera)
            continue;
        Rendering::Matrix4x4 viewProjection;
        std::copy_n(camera->viewProj, 16, viewProjection.Data());
        Rendering::Vector4 planes[6];
        Rendering::ExtractFrustumPlanes(viewProjection, planes);
        m_Impl->m_Indices.clear();
        bool viewSeesMotion = false;
        for (uint32 i = 0; i < m_Impl->m_Particles.size(); ++i)
        {
            const auto& item = m_Impl->m_DrawItems[i];
            const auto& particle = m_Impl->m_Particles[i];
            if ((item.LayerMask & view->ActiveRenderLayerMask()) == 0)
                continue;
            const auto& p = particle.PositionSize;
            if (!Rendering::TestSphereFrustum(Rendering::Vector3(p[0], p[1], p[2]), item.Radius, planes))
                continue;
            if (item.LodEnd > item.LodStart)
            {
                const float dx = p[0] - camera->cameraPos[0], dy = p[1] - camera->cameraPos[1], dz = p[2] - camera->cameraPos[2];
                const float fraction = std::clamp((item.LodEnd - std::sqrt(dx * dx + dy * dy + dz * dz)) / (item.LodEnd - item.LodStart), 0.0f, 1.0f);
                uint32 hash = item.SpawnIndex * 747796405u + static_cast<uint32>(item.Emitter) * 2891336453u;
                hash = (hash ^ (hash >> 16)) * 2246822519u;
                if (static_cast<float>(hash & 0xFFFFFFu) / 16777216.0f >= fraction)
                    continue;
            }
            const bool depthOrder = item.Order == Components::ParticleDrawOrder::ViewDepth;
            m_Impl->m_Depths[i] = Particles::ViewDepth(camera->view, depthOrder ? Mathematics::Vector3{p.x, p.y, p.z} : item.EmitterPosition);
            if (std::isfinite(m_Impl->m_Depths[i]))
            {
                m_Impl->m_Indices.push_back(i);
                viewSeesMotion = viewSeesMotion || item.Moving;
            }
        }
        // Moving particles change this view's pixels without moving an instance or advancing a
        // content epoch, so TAA must not certify the view stationary this frame.
        if (viewSeesMotion)
            rs->NotifyUnversionedMotion(view->id);
        std::sort(m_Impl->m_Indices.begin(), m_Impl->m_Indices.end(),
                  [this](uint32 a, uint32 b)
                  { return m_Impl->CompareParticleIndices(a, b); });
        if (m_Impl->m_Indices.empty())
            continue;
        const size_t viewStride = useDeviceAddress ? sizeof(uint32) : sizeof(ParticleRenderData);
        auto buffer = m_Impl->EnsureBuffer(device, frameSlot, useDeviceAddress, m_Impl->m_ViewBuffers[view->id], static_cast<uint32>(m_Impl->m_Indices.size()), viewStride,
                                           useDeviceAddress ? "ParticleViewIndices" : "ParticleViewData");
        if (!buffer.IsValid())
            continue;
        std::span<const std::byte> nativePush;
        const DrawBindings::BufferEntry* compatBinding = nullptr;
        if (useDeviceAddress)
        {
            device->UpdateBuffer(buffer, 0, m_Impl->m_Indices.size() * sizeof(uint32), m_Impl->m_Indices.data());
            const uint64 indexAddress = device->GetBufferDeviceAddress(buffer);
            if (indexAddress == 0)
                continue;
            m_Impl->m_ParticlePushConstants.push_back({indexAddress, particleAddress});
            const auto& push = m_Impl->m_ParticlePushConstants.back();
            nativePush = std::span<const std::byte>(reinterpret_cast<const std::byte*>(&push), sizeof(push));
        }
        else
        {
            // Preserve late-forward blending and sorted batches without BDA.
            // No GPUScene entries or extra shader indirection binding are needed.
            auto& records = m_Impl->m_CompatViewParticles;
            records.clear();
            records.reserve(m_Impl->m_Indices.size());
            for (uint32 index : m_Impl->m_Indices)
                records.push_back(m_Impl->m_Particles[index]);
            const size_t bytes = records.size() * sizeof(ParticleRenderData);
            device->UpdateBuffer(buffer, 0, bytes, records.data());
            m_Impl->m_CompatParticleBindings.push_back({HashStringId("ParticleInstances"), buffer, 0, bytes});
            compatBinding = &m_Impl->m_CompatParticleBindings.back();
        }
        for (uint32 first = 0; first < m_Impl->m_Indices.size();)
        {
            const auto& item = m_Impl->m_DrawItems[m_Impl->m_Indices[first]];
            uint32 end = first + 1;
            while (end < m_Impl->m_Indices.size())
            {
                const auto& next = m_Impl->m_DrawItems[m_Impl->m_Indices[end]];
                if (next.MaterialPtr != item.MaterialPtr || !(next.Mesh == item.Mesh))
                    break;
                ++end;
            }
            if (const auto* mesh = meshReg.Find(item.Mesh))
            {
                DrawCommand command{};
                command.Material = item.MaterialPtr;
                command.Geometry.Mesh = item.Mesh;
                command.VertexFlags = mesh->vertexFlags;
                command.PassKeywords = kParticleDrawKeywords;
                command.IndexCount = mesh->indexCount;
                command.InstanceCount = end - first;
                command.FirstIndex = mesh->firstIndex;
                command.VertexOffset = static_cast<int32>(mesh->vertexOffset);
                command.FirstInstance = useDeviceAddress ? first : 0u;
                command.Bindings.PushConstants = nativePush;
                if (compatBinding)
                {
                    m_Impl->m_CompatBaseParticles.push_back(first);
                    const auto& base = m_Impl->m_CompatBaseParticles.back();
                    command.Bindings.PushConstants = std::span<const std::byte>(reinterpret_cast<const std::byte*>(&base), sizeof(base));
                    command.Bindings.Buffers = std::span<const DrawBindings::BufferEntry>(compatBinding, 1);
                }
                m_Impl->m_ParticleForwardCommands[view->id].push_back(command);
            }
            first = end;
        }
    }
}
} // namespace GameEngine::Particles
