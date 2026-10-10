// Renderer scale-bench spawner: procedurally fills the primary world with a
// deterministic large scene (static + orbiting-dynamic primitives, point
// lights) so the perf harness (Tools/Benchmarks/Rendering/perf-bench) can capture comparable
// before/after numbers for renderer work. Skinned characters are spawned by
// the harness via the existing spawn_model handler.

#include "DebugServer/BenchSceneHandlers.h"

#include "AssetCore/GUID.h"
#include "Assets/MeshLODGenerator.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

// Orbit parameters for the dynamic subset. Local to this TU: bench entities
// are never serialized, so the type needs no reflection registration.
struct BenchSpin
{
    float BaseX = 0.0f;
    float BaseY = 0.0f;
    float BaseZ = 0.0f;
    float Phase = 0.0f;
    float Speed = 1.0f;
    float Radius = 1.0f;
};

// Moves every BenchSpin entity each frame so their instances stay dirty —
// the workload R1.1's dirty-range upload must handle without re-uploading
// the static 100k.
class BenchSpinSystem : public ECS::ISystem
{
  public:
    const char* GetName() const override { return "BenchSpinSystem"; }

    void Update(ECS::World& world, float32 deltaTime) override
    {
        m_Time += deltaTime;
        const float t = m_Time;
        auto q = world.Query<ECS::Write<Components::Transform>, ECS::Read<BenchSpin>>();
        q.Each(
            [t](ECS::EntityHandle, Components::Transform& xf, const BenchSpin& spin)
            {
                const float a = spin.Phase + t * spin.Speed;
                const Mathematics::Vector3 pos{spin.BaseX + std::cos(a) * spin.Radius, spin.BaseY,
                                               spin.BaseZ + std::sin(a) * spin.Radius};
                xf = Components::Transform::FromTRS(pos, Mathematics::Quaternion{},
                                                    Mathematics::Vector3{1.0f, 1.0f, 1.0f});
            });
    }

  private:
    float m_Time = 0.0f;
};

// The editor's rendering loop runs a wave-based execution plan built at
// startup; systems appended afterwards are outside the plan and never tick.
// Prepend a dedicated wave so the mover runs before TransformHierarchy and
// its writes land in the same frame's extraction.
bool RegisterBenchSpinSystem()
{
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    auto* systems = loop ? loop->GetSystemManager() : nullptr;
    if (!systems)
        return false;

    systems->AddSystem<BenchSpinSystem>();
    const size_t systemIndex = systems->GetSequentialSystemCount() - 1;

    ECS::SystemExecutionPlan plan = systems->GetExecutionPlan();
    if (plan.IsValid())
    {
        ECS::SystemExecutionPlan::Wave wave;
        wave.SystemIndices.push_back(systemIndex);
        plan.Waves.insert(plan.Waves.begin(), wave);
        systems->SetExecutionPlan(std::move(plan));
    }
    return true;
}

// Parents a chain of `depth` child meshes under `root`, each 0.6 m above and
// half the size of the one before: the shape of an imported model (a root
// carrying child parts), so the transform hierarchy has members to walk.
void SpawnChildChain(ECS::World& world, ECS::EntityHandle root, uint32 depth,
                     const Components::MeshRenderer& mesh, const Components::LocalBounds& bounds)
{
    const Mathematics::Vector3 childScale{0.5f, 0.5f, 0.5f};
    ECS::EntityHandle parent = root;
    for (uint32 level = 0; level < depth; ++level)
    {
        ECS::Entity child = world.Create(
            Components::Transform::FromTRS({0.0f, 0.6f, 0.0f}, Mathematics::Quaternion{}, childScale),
            mesh, bounds);
        child.Set(Components::Parent{parent});
        parent = child.GetHandle();
    }
}

Mathematics::Vector3 BenchColor(uint32 index, uint32 count)
{
    // Evenly spaced hues, full saturation — distinct material rows in captures.
    const float h = (count > 0 ? static_cast<float>(index) / static_cast<float>(count) : 0.0f) * 6.0f;
    const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
    switch (static_cast<int>(h) % 6)
    {
        case 0: return {1.0f, x, 0.0f};
        case 1: return {x, 1.0f, 0.0f};
        case 2: return {0.0f, 1.0f, x};
        case 3: return {0.0f, x, 1.0f};
        case 4: return {x, 0.0f, 1.0f};
        default: return {1.0f, 0.0f, x};
    }
}

} // namespace

void RegisterBenchSceneHandlers(EditorDebugServer& server)
{
    server.RegisterHandler(
        "spawn_bench_scene",
        [](const EditorDebugServer::RequestContext& ctx) -> json
        {
            static bool s_Spawned = false;
            if (s_Spawned)
                return Editor::RefuseRequest("Bench scene already spawned this session; relaunch the editor for a fresh run");

            auto* world = EngineCore::GetInstance().GetPrimaryWorld();
            auto* rs = EngineCore::GetInstance().GetRenderServices();
            if (!world || !rs)
                return Editor::RefuseRequest("No world / render services available");

            // Validate through int64 first: nlohmann's unchecked unsigned cast
            // would wrap a negative count to ~4.3 billion entities on the main
            // thread.
            constexpr int64_t kMaxSpawn = 1000000;
            const int64_t staticIn = ctx.params.value("staticCount", int64_t{100000});
            const int64_t dynamicIn = ctx.params.value("dynamicCount", int64_t{10000});
            const int64_t lightIn = ctx.params.value("lightCount", int64_t{256});
            const int64_t materialIn = ctx.params.value("materialCount", int64_t{32});
            const int64_t wallsIn = ctx.params.value("occluderWalls", int64_t{0});
            // Transmissive (refractive glass) overlay. The key name MUST match
            // the harness param exactly: nlohmann's .value() silently returns
            // the default for an unknown key, so a typo spawns nothing.
            const int64_t transmissiveIn = ctx.params.value("transmissiveCount", int64_t{0});
            // Shadow-caster flag on the glass spheres. false spawns transmissive
            // geometry that is visible to the camera but contributes nothing to any
            // shadow cascade — the material-present / zero-caster arm.
            const bool transmissiveCastShadows =
                ctx.params.value("transmissiveCastShadows", true);
            // A chain of this many child meshes under every static and dynamic
            // entity (0: none), so the transform hierarchy has members to walk.
            const int64_t childrenIn = ctx.params.value("childrenPerEntity", int64_t{0});
            if (staticIn < 0 || staticIn > kMaxSpawn || dynamicIn < 0 || dynamicIn > kMaxSpawn ||
                lightIn < 0 || lightIn > 4096 || materialIn < 1 || materialIn > 256 ||
                wallsIn < 0 || wallsIn > 64 || transmissiveIn < 0 || transmissiveIn > 4096 ||
                childrenIn < 0 || childrenIn > 16)
                return Editor::RefuseRequest("Count out of range (static/dynamic <= 1M, lights <= 4096, materials 1..256, occluderWalls <= 64, transmissive <= 4096, childrenPerEntity 0..16)");
            const uint32 staticCount = static_cast<uint32>(staticIn);
            const uint32 dynamicCount = static_cast<uint32>(dynamicIn);
            const uint32 lightCount = static_cast<uint32>(lightIn);
            const uint32 materialCount = static_cast<uint32>(materialIn);
            const uint32 occluderWalls = static_cast<uint32>(wallsIn);
            const uint32 transmissiveCount = static_cast<uint32>(transmissiveIn);
            const uint32 childrenPerEntity = static_cast<uint32>(childrenIn);
            const float spacing = ctx.params.value("spacing", 2.5f);

            const auto start = std::chrono::steady_clock::now();

            // Materials: distinct baseColor/roughness per index, all on the
            // StandardPBR surface so one pipeline serves every row.
            std::vector<GUID> materials(materialCount);
            for (uint32 i = 0; i < materialCount; ++i)
            {
                MaterialDocument doc =
                    MaterialDocument::CreateDefaultPBR("BenchMat" + std::to_string(i));
                const Mathematics::Vector3 c = BenchColor(i, materialCount);
                doc.properties["baseColor"] = std::vector<float>{c.x, c.y, c.z, 1.0f};
                doc.properties["roughness"] = 0.25f + 0.65f * static_cast<float>(i % 8) / 7.0f;
                doc.properties["metallic"] = (i % 4 == 0) ? 1.0f : 0.0f;
                const GUID guid =
                    GUID::Derive(GUID::Null(), ("bench/material/" + std::to_string(i)).c_str());
                if (!rs->RegisterAndPrewarmMaterial(guid, doc))
                    return Editor::RefuseRequest("Failed to register bench material " + std::to_string(i));
                materials[i] = guid;
            }

            // Transmissive glass material (one shared row, opt-in). enableTransmission
            // sets MaterialKeyword::Transmission, which forces Opaque alpha (writes
            // depth -> exercises the depth prepass) and routes the batch through the
            // dedicated post-grab transmissive pass. As a material-dependent caster it
            // keeps its real materialIndex identity, so its (material, mesh) batch
            // coexists in the cascade=None table with the merged opaque color classes —
            // the exact domain the P2 color-class merge must survive.
            GUID transmissiveMaterial;
            if (transmissiveCount > 0)
            {
                MaterialDocument doc = MaterialDocument::CreateDefaultPBR("BenchGlass");
                doc.properties["baseColor"] = std::vector<float>{0.85f, 0.92f, 1.0f, 1.0f};
                doc.properties["roughness"] = 0.05f;
                doc.properties["metallic"] = 0.0f;
                doc.properties["enableTransmission"] = true;
                doc.properties["transmissionWeight"] = 1.0f;
                transmissiveMaterial = GUID::Derive(GUID::Null(), "bench/material/transmissive");
                if (!rs->RegisterAndPrewarmMaterial(transmissiveMaterial, doc))
                    return Editor::RefuseRequest("Failed to register bench transmissive material");
            }

            const GUID meshGuids[2] = {Engine::Renderer::PrimitiveGenerator::CubeGuid(),
                                       Engine::Renderer::PrimitiveGenerator::SphereGuid()};

            // lod-content lane (design P0.2 Option A). Regenerate the two unique
            // bench meshes with simplified LOD chains and re-register them under
            // the same well-known GUIDs BEFORE the prototypes are built, so every
            // instance references a mesh with lodCount > 1 and ge_SelectLOD picks
            // coarser geometry for distant casters (the far cascades). This
            // validates the shadow-BIAS lever in isolation: bench spawns carry NO
            // LODGroup component, so the per-entity LODGroup.Bias term stays
            // pinned 0 — the three-term LOD composition (global + shadowBias[c] +
            // perEntityBias) is NOT exercised here; that belongs to a separate
            // project-content check (design F5 / Q1). RegisterSubmesh re-uploads
            // in place on content-hash change (ExtraLODs added), keeping the
            // handle + GPUScene mesh index stable.
            json lodResult = json::object();
            if (ctx.params.value("lods", false))
            {
                auto& meshReg = rs->GetMeshGPURegistry();
                auto regenerateWithLods = [&](const GUID& guid, Mesh mesh) -> uint32
                {
                    GenerateMeshLODsInto(mesh); // fills mesh.ExtraLODs; LOD0 untouched
                    meshReg.RegisterSubmesh(Rendering::MeshGPUKey{guid, 0u}, mesh);
                    return mesh.LODCount();
                };
                const uint32 cubeLods = regenerateWithLods(
                    meshGuids[0], Engine::Renderer::PrimitiveGenerator::GenerateCube());
                const uint32 sphereLods = regenerateWithLods(
                    meshGuids[1], Engine::Renderer::PrimitiveGenerator::GenerateSphere());
                lodResult = {{"requested", true},
                             {"generationAvailable", IsMeshLODGenerationAvailable()},
                             {"cubeLodCount", cubeLods},
                             {"sphereLodCount", sphereLods}};
            }

            // Prototype components once; the per-entity loop only swaps the
            // material GUID (FindHandle per entity would be 110k registry hits).
            Components::MeshRenderer meshProto[2];
            Components::LocalBounds boundsProto[2];
            for (int m = 0; m < 2; ++m)
            {
                meshProto[m] = Engine::Renderer::PrimitiveGenerator::MakePrimitiveMeshRenderer(
                    rs, meshGuids[m], materials[0]);
                if (meshProto[m].meshGpuHandleId == 0)
                    return Editor::RefuseRequest("Primitive mesh not registered — rendering loop not started?");
                boundsProto[m] = Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(
                    rs, meshGuids[m]);
            }

            const Mathematics::Quaternion identity{};
            const Mathematics::Vector3 unitScale{1.0f, 1.0f, 1.0f};

            // Static field: square grid centered on the origin. The movers share
            // its side, so it is sized for whichever set is larger.
            const uint32 side = static_cast<uint32>(std::ceil(
                std::sqrt(static_cast<double>(std::max({staticCount, dynamicCount, 1u})))));
            const float half = static_cast<float>(side) * 0.5f;
            for (uint32 i = 0; i < staticCount; ++i)
            {
                const float x = (static_cast<float>(i % side) - half) * spacing;
                const float z = (static_cast<float>(i / side) - half) * spacing;
                const int m = static_cast<int>(i & 1u);
                Components::MeshRenderer mr = meshProto[m];
                mr.materialAssetGuid.Set(materials[i % materialCount]);
                Components::LocalBounds lb = boundsProto[m];
                lb.DynamicObject = false;
                ECS::Entity root = world->Create(
                    Components::Transform::FromTRS({x, 0.5f, z}, identity, unitScale), mr, lb);
                SpawnChildChain(*world, root.GetHandle(), childrenPerEntity, meshProto[0], boundsProto[0]);
            }

            // Dynamic subset: orbiting primitives above the field. Phase from
            // the golden angle so orbits decorrelate without an RNG.
            constexpr float kGoldenAngle = 2.399963f;
            for (uint32 i = 0; i < dynamicCount; ++i)
            {
                const float x = (static_cast<float>(i % side) - half) * spacing * 3.0f;
                const float z = (static_cast<float>(i / side) - half) * spacing * 3.0f;
                const int m = static_cast<int>(i & 1u);
                Components::MeshRenderer mr = meshProto[m];
                mr.materialAssetGuid.Set(materials[i % materialCount]);
                BenchSpin spin;
                spin.BaseX = x;
                spin.BaseY = 2.0f + static_cast<float>(i % 5);
                spin.BaseZ = z;
                spin.Phase = kGoldenAngle * static_cast<float>(i);
                spin.Speed = 0.5f + static_cast<float>(i % 16) * 0.1f;
                spin.Radius = 1.0f + static_cast<float>(i % 8) * 0.25f;
                ECS::Entity root = world->Create(
                    Components::Transform::FromTRS({x, spin.BaseY, z}, identity, unitScale), mr,
                    boundsProto[m], spin);
                SpawnChildChain(*world, root.GetHandle(), childrenPerEntity, meshProto[0], boundsProto[0]);
            }

            // Occluder walls (R2.1 P0 — the HZB interiors baseline): tall
            // solid slabs spanning the field's width, evenly spaced along Z,
            // so a low camera behind one sees only the nearest "room" while
            // the rest of the grid is occluded. Opaque casters; static.
            if (occluderWalls > 0)
            {
                const float fieldExtent = half * spacing; // field half-width
                for (uint32 w = 0; w < occluderWalls; ++w)
                {
                    // Distribute strictly inside the field so every wall has
                    // grid content behind it.
                    const float t = (static_cast<float>(w) + 0.5f) /
                                    static_cast<float>(occluderWalls);
                    const float z = (t * 2.0f - 1.0f) * fieldExtent;
                    Components::MeshRenderer mr = meshProto[0]; // cube
                    mr.materialAssetGuid.Set(materials[0]);
                    Components::LocalBounds lb = boundsProto[0];
                    lb.DynamicObject = false;
                    world->Create(
                        Components::Transform::FromTRS(
                            {0.0f, 6.0f, z}, identity,
                            {fieldExtent * 2.0f, 12.0f, 1.0f}),
                        mr, lb);
                }
            }

            // Transmissive glass spheres: a compact grid floating above the
            // field centre so the camera sees them refracting/blending over the
            // dense opaque grid behind (the pixel-equivalence check for the
            // merge). All share one material + the sphere mesh -> one instanced
            // (material, mesh) batch keyed on the real materialIndex.
            if (transmissiveCount > 0)
            {
                constexpr float kGlassHeight = 3.0f;
                constexpr float kGlassSpacing = 5.0f;
                constexpr float kGlassScale = 2.0f;
                const uint32 glassSide = static_cast<uint32>(
                    std::ceil(std::sqrt(static_cast<double>(transmissiveCount))));
                const float glassHalf = static_cast<float>(glassSide) * 0.5f;
                const Mathematics::Vector3 glassScale{kGlassScale, kGlassScale, kGlassScale};
                for (uint32 i = 0; i < transmissiveCount; ++i)
                {
                    const float x = (static_cast<float>(i % glassSide) - glassHalf) * kGlassSpacing;
                    const float z = (static_cast<float>(i / glassSide) - glassHalf) * kGlassSpacing;
                    Components::MeshRenderer mr = meshProto[1]; // sphere
                    mr.materialAssetGuid.Set(transmissiveMaterial);
                    mr.castShadows = transmissiveCastShadows;
                    Components::LocalBounds lb = boundsProto[1];
                    lb.DynamicObject = false;
                    world->Create(
                        Components::Transform::FromTRS({x, kGlassHeight, z}, identity, glassScale),
                        mr, lb);
                }
            }

            // Point lights: coarse grid floating above the static field.
            const uint32 lightSide = static_cast<uint32>(
                std::ceil(std::sqrt(static_cast<double>(std::max(lightCount, 1u)))));
            const float lightSpacing =
                (static_cast<float>(side) * spacing) / static_cast<float>(std::max(lightSide, 1u));
            const float lightHalf = static_cast<float>(lightSide) * 0.5f;
            for (uint32 i = 0; i < lightCount; ++i)
            {
                const float x = (static_cast<float>(i % lightSide) - lightHalf) * lightSpacing;
                const float z = (static_cast<float>(i / lightSide) - lightHalf) * lightSpacing;
                Components::Light light{};
                light.Type = Components::LightType::Point;
                light.Range = 8.0f;
                light.Intensity = 1500.0f;
                light.IntensityUnit = Components::LightUnit::Lumen;
                const Mathematics::Vector3 c = BenchColor(i, lightCount);
                light.Color[0] = c.x;
                light.Color[1] = c.y;
                light.Color[2] = c.z;
                world->Create(
                    Components::Transform::FromTRS({x, 3.0f, z}, identity, unitScale), light);
            }

            world->ProcessCommands();

            bool moverRegistered = true;
            if (dynamicCount > 0)
                moverRegistered = RegisterBenchSpinSystem();

            s_Spawned = true;
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start);
            return json{{"staticSpawned", staticCount},
                        {"dynamicSpawned", dynamicCount},
                        {"childrenPerEntity", childrenPerEntity},
                        {"transmissiveSpawned", transmissiveCount},
                        {"transmissiveCastShadows", transmissiveCastShadows},
                        {"occluderWalls", occluderWalls},
                        {"lightsSpawned", lightCount},
                        {"materialsRegistered", materialCount},
                        {"moverRegistered", moverRegistered},
                        {"lods", lodResult},
                        {"entityCount", static_cast<uint64_t>(world->GetEntityCount())},
                        {"elapsedMs", elapsed.count()}};
        });
}

} // namespace GameEngine
