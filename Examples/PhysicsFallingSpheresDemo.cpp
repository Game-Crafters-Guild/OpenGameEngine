#include "Logger/Logger.h"

#include "Platform/Window.h"

#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"

#include "Input/KeyCodes.h"

#include <GLFW/glfw3.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include <span>

#include "Mathematics/MatrixOps.h"
#include "Rendering/CameraTypes.h"

#include "ECS/World.h"
#include "ECS/ECSTemplates.h"

#include "Components/Transform.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Hierarchy.h"

#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"

#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Systems/PhysicsInitSystem.h"
#include "PhysicsECS/Systems/PhysicsStepSystem.h"
#include "PhysicsECS/Systems/PhysicsWritebackSystem.h"

#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"
#include "PhysicsECS/Components/SphereColliderShape.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

struct AppState
{
    bool quitRequested = false;
    bool resetRequested = false;
};

static uint32_t HashU32(uint32_t x)
{
    // Small avalanching hash (good enough for demo jitter).
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static float RandSigned01(uint32_t seed)
{
    // [-1, +1]
    const uint32_t h = HashU32(seed);
    const float t = static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x00FFFFFFu);
    return (t * 2.0f) - 1.0f;
}

static void LogLowestSphere(const char* tag,
                            GameEngine::ECS::World& world,
                            GameEngine::Physics::PhysicsWorld* pw,
                            const std::vector<GameEngine::ECS::EntityHandle>& spheres)
{
    using namespace GameEngine;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;

    float minLocalY = std::numeric_limits<float>::infinity();
    float minPhysY = std::numeric_limits<float>::infinity();
    ECS::EntityHandle minE{};

    for (auto e : spheres)
    {
        if (!world.IsValid(e))
            continue;
        const auto* t = world.GetComponent<Transform>(e);
        const auto* pb = world.GetComponent<PhysicsBody>(e);
        if (!t || !pb)
            continue;

        const float yLocal = t->GetPosition().y;
        if (yLocal < minLocalY)
        {
            minLocalY = yLocal;
            minE = e;
        }

        if (pw && pb->initialized && pw->IsBodyValid(pb->body))
        {
            const auto pt = pw->GetBodyTransform(pb->body);
            minPhysY = std::min(minPhysY, static_cast<float>(pt.position.y));
        }
    }

    if (minE.IsValid() && std::isfinite(minLocalY))
    {
        Logger::Log::Info("[Demo] {} lowest sphere entity={} localMinY={} physMinY={}", tag, minE.id, minLocalY, minPhysY);
    }
}

struct GroundEntities
{
    GameEngine::ECS::EntityHandle ground{};
    GameEngine::ECS::EntityHandle post{};
};

static GroundEntities CreateGround(GameEngine::ECS::World& world)
{
    using namespace GameEngine;
    using namespace GameEngine::Components;
    using namespace GameEngine::Engine::Renderer;

    GroundEntities out{};
    auto e = world.CreateEntity();
    out.ground = e;

    // Visible ground: use the cube mesh scaled into a thin slab so it's visible
    // from above (the built-in plane mesh is single-sided and can be culled).
    // NOTE: Physics also uses a box; we align the top face at y = 0.
    constexpr float kThickness = 0.2f;
    Mathematics::Vector3 p{0.0f, -kThickness * 0.5f, 0.0f};
    // Use identity rotation. (Previously we rotated 180° around X, but that
    // flips child transforms in the hierarchy and makes demo authoring confusing.)
    Mathematics::Quaternion r{};
    Mathematics::Vector3 s{50.0f, kThickness, 50.0f};
    Transform t = Transform::FromTRS(p, r, s);
    world.AddComponentImmediate(e, t);

    MeshRenderer mr{};
    mr.meshId = 1u;
    mr.renderLayerMask = 1u;
    world.AddComponentImmediate(e, mr);

    // Physics: static floor. Use a box (more robust than infinite plane for demos).
    PhysicsBody pb{};
    pb.motionType = GameEngine::Physics::MotionType::Static;
    pb.mass = 0.0f;
    pb.startAwake = false;
    pb.material.friction = 1.0f;
    pb.material.restitution = 0.35f;
    world.AddComponentImmediate(e, pb);

    PhysicsCollider pc{};
    pc.layer = GameEngine::Physics::Layers::Static;
    // Inherit material from body defaults.
    world.AddComponentImmediate(e, pc);

    BoxColliderShape box{};
    // Unit cube scaled by Transform scale -> big slab.
    box.halfExtentsX = 0.5f;
    box.halfExtentsY = 0.5f;
    box.halfExtentsZ = 0.5f;
    world.AddComponentImmediate(e, box);

    // Example multi-collider body: add a small vertical post as a second collider
    // on the same rigid body by creating a collider entity that points at `e`.
    {
        auto c = world.CreateEntity();
        out.post = c;
        GameEngine::Components::Parent parent{};
        parent.parent = e;
        world.AddComponentImmediate(c, parent);

        // IMPORTANT:
        // The ground entity is heavily scaled (x/z=50, y=0.2). Since this post is
        // parented under the ground, its local translation & scale are multiplied
        // by the parent's scale. Author the post so its *world* size/position is
        // obvious and stable across camera angles.
        //
        // Desired world transform:
        // - position: (0, 2, 8) (in front of camera looking at origin)
        // - size: (1, 4, 1)
        constexpr float kWorldPostX = 0.0f;
        constexpr float kWorldPostY = 2.0f;
        constexpr float kWorldPostZ = 8.0f;
        constexpr float kWorldPostSX = 1.0f;
        constexpr float kWorldPostSY = 4.0f;
        constexpr float kWorldPostSZ = 1.0f;

        // Convert desired world-space TRS into parent-local TRS (approx; parent rotation is identity).
        const float lx = (kWorldPostX - p.x) / s.x;
        const float ly = (kWorldPostY - p.y) / s.y;
        const float lz = (kWorldPostZ - p.z) / s.z;
        const float lsx = kWorldPostSX / s.x;
        const float lsy = kWorldPostSY / s.y;
        const float lsz = kWorldPostSZ / s.z;

        Transform local = Transform::FromTRS(Mathematics::Vector3{lx, ly, lz}, Mathematics::Quaternion{}, Mathematics::Vector3{lsx, lsy, lsz});
        world.AddComponentImmediate(c, local);

        // Visualize the post (otherwise it's physics-only and you won't see it).
        MeshRenderer postMr{};
        postMr.meshId = 1u; // cube
        postMr.renderLayerMask = 1u;
        world.AddComponentImmediate(c, postMr);

        GameEngine::Components::PhysicsColliderOwner owner{};
        owner.body = e;
        world.AddComponentImmediate(c, owner);

        PhysicsCollider col{};
        col.layer = GameEngine::Physics::Layers::Static;
        // Inherit material from body defaults.
        world.AddComponentImmediate(c, col);

        BoxColliderShape post{};
        post.halfExtentsX = 0.5f;
        post.halfExtentsY = 0.5f;
        post.halfExtentsZ = 0.5f;
        world.AddComponentImmediate(c, post);
    }

    return out;
}

static std::vector<GameEngine::ECS::EntityHandle> CreateSpheres(GameEngine::ECS::World& world, int countX, int countZ)
{
    using namespace GameEngine;
    using namespace GameEngine::Components;
    using namespace GameEngine::Engine::Renderer;

    std::vector<ECS::EntityHandle> out;
    out.reserve(static_cast<size_t>(countX * countZ));

    const float spacing = 1.25f;
    const float startY = 10.0f;
    const float radius = 0.5f;

    for (int z = 0; z < countZ; ++z)
    {
        for (int x = 0; x < countX; ++x)
        {
            auto e = world.CreateEntity();
            out.push_back(e);

            const float fx = (x - (countX - 1) * 0.5f) * spacing;
            const float fz = (z - (countZ - 1) * 0.5f) * spacing;

            Mathematics::Vector3 p{fx, startY + z * 0.25f, fz};
            Mathematics::Quaternion r{};
            Mathematics::Vector3 s{1.0f, 1.0f, 1.0f};
            Transform t = Transform::FromTRS(p, r, s);
            world.AddComponentImmediate(e, t);

            MeshRenderer mr{};
            mr.meshId = 2u;
            mr.renderLayerMask = 1u;
            world.AddComponentImmediate(e, mr);

            PhysicsBody pb{};
            pb.motionType = GameEngine::Physics::MotionType::Dynamic;
            pb.mass = 1.0f;
            pb.startAwake = true;
        pb.material.friction = 0.7f;
        pb.material.restitution = 0.6f; // ~5x bounce vs prior 0.1

            pb.continuousCollision = true;

            // Deterministic "noise": a touch of sideways speed + spin so spheres
            // don't bounce perfectly straight up and will roll in varied directions.
            const uint32_t seed = (static_cast<uint32_t>(x) * 73856093u) ^ (static_cast<uint32_t>(z) * 19349663u) ^ 0x9e3779b9u;
            const float rx = RandSigned01(seed ^ 0xA2C2A2C2u);
            const float rz = RandSigned01(seed ^ 0xC3D3C3D3u);
            pb.linearVelocityX = rx * 0.35f;
            pb.linearVelocityZ = rz * 0.35f;
            pb.angularVelocityX = rz * 18.0f;
            pb.angularVelocityY = rx * 6.0f;
            pb.angularVelocityZ = rx * 18.0f;
            world.AddComponentImmediate(e, pb);

            PhysicsCollider pc{};
            pc.layer = GameEngine::Physics::Layers::Dynamic;
            // Inherit material from body defaults.
            world.AddComponentImmediate(e, pc);

            SphereColliderShape sphere{};
            sphere.radius = radius;
            world.AddComponentImmediate(e, sphere);
        }
    }

    return out;
}

static void DestroyEntitiesImmediateAndCleanupPhysics(GameEngine::ECS::World& world,
                                                     GameEngine::Physics::PhysicsWorld* physicsWorld,
                                                     const std::vector<GameEngine::ECS::EntityHandle>& entities)
{
    using GameEngine::Components::PhysicsBody;

    // IMPORTANT:
    // `DestroyEntityImmediate` uses swap-and-pop within archetypes. If we mutate a
    // component (like clearing PhysicsBody handles) and then destroy the entity,
    // that cleared component data may get swapped onto a different entity, which
    // would make us "miss" destroying that other entity's physics body later.
    //
    // So we do three phases:
    // 1) Snapshot body/shape handles
    // 2) Destroy physics objects
    // 3) Destroy ECS entities
    std::vector<GameEngine::Physics::BodyHandle> bodies;
    std::vector<GameEngine::Physics::ShapeHandle> shapes;
    bodies.reserve(entities.size());
    shapes.reserve(entities.size());

    if (physicsWorld)
    {
        for (auto e : entities)
        {
            if (!world.IsValid(e))
                continue;

            if (auto* pb = world.GetComponent<PhysicsBody>(e))
            {
                if (pb->body.IsValid())
                    bodies.push_back(pb->body);
                if (pb->shape.IsValid())
                    shapes.push_back(pb->shape);
            }
        }

        for (auto b : bodies)
            physicsWorld->DestroyBody(b);
        for (auto s : shapes)
            physicsWorld->DestroyShape(s);
    }

    for (auto e : entities)
    {
        if (world.IsValid(e))
            world.DestroyEntityImmediate(e);
    }
}

} // namespace

int main()
{
    using namespace GameEngine;
    using namespace GameEngine::Engine::Renderer;

    Logger::Log::Config logCfg;
    logCfg.GlobalMinLevel = Logger::LogLevel::Info;
    Logger::Log::Initialize(logCfg);

    glfwSetErrorCallback([](int code, const char* desc)
                         { Logger::Log::Error("[GLFW] {}: {}", code, (desc ? desc : "")); });
    if (glfwInit() != GLFW_TRUE)
    {
        Logger::Log::Error("glfwInit() failed");
        return 1;
    }

    Platform::Window window;
    if (!window.Create({.Title = "Physics Falling Spheres Demo (R = Reset, Esc = Quit)", .Width = 1280, .Height = 720}))
    {
        Logger::Log::Error("Failed to create window");
        glfwTerminate();
        return 1;
    }
    window.Show();

    AppState state{};
    // Bound straight to the window rather than through WindowInputRouter: this
    // demo hosts no UIManager and no InputSystem, so the router's chain has no
    // stage to offer a key to. Two hotkeys into local state are the whole input
    // surface here.
    window.SetKeyHandler([&](int key, int action, int /*mods*/)
                         {
                             // GLFW action: 1 = press, 2 = repeat, 0 = release
                             const bool pressed = (action == 1);
                             if (!pressed)
                                 return;
                             if (key == GameEngine::Input::kKeyCode_Escape)
                                 state.quitRequested = true;
                             if (key == GameEngine::Input::kKeyCode_R)
                                 state.resetRequested = true;
                         });

    RenderDeviceContext renderCtx;
    RenderDeviceContext::InitParams rp{};
    rp.deviceDesc.enableSwapchain = true;
    rp.windowHandle = window.GetNativeHandle();
    {
        int fbW = 0, fbH = 0;
        window.GetFramebufferSize(fbW, fbH);
        rp.width = static_cast<uint32>(std::max(1, fbW));
        rp.height = static_cast<uint32>(std::max(1, fbH));
    }
    rp.createRenderServices = true;

    if (!renderCtx.Initialize(rp))
    {
        Logger::Log::Error("Failed to initialize RenderDeviceContext");
        return 2;
    }

    auto* device = renderCtx.GetDevice();
    auto* rs = renderCtx.GetRenderServices();
    if (!device || !rs)
    {
        Logger::Log::Error("Render context incomplete");
        return 3;
    }

    // Immediate-mode render graph: pools + one reused frame (mirrors the Player
    // driver, PlayerApplication::RenderFrameRG2). View color/depth/resolve are
    // pool imports declared every frame; the full ForwardPlus spine runs through
    // them and the terminal sRGB encode lands FinalColor on the swapchain.
    Rendering::RenderGraph::RGResourcePool rgPersistent(device);
    Rendering::RenderGraph::RGTransientPool rgTransient(device);
    Rendering::RenderGraph::RGUploadRing rgRing(
        device, std::max(2u, device->GetFramesInFlight()), 1u << 20);
    Rendering::RenderGraph::RGFrame frame(device, &rgPersistent, &rgTransient, &rgRing);
    uint64_t rgFrameIndex = 0;

    // ECS world + systems
    ECS::World world;

    Engine::Renderer::TransformHierarchySystem xfSystem;
    Engine::Renderer::RenderExtractionSystem extraction(rs);
    PhysicsECS::PhysicsInitSystem physicsInit;
    PhysicsECS::PhysicsStepSystem physicsStep;
    PhysicsECS::PhysicsWritebackSystem physicsWriteback;

    // Physics world instance
    Physics::PhysicsWorldSettings physCfg{};
    physCfg.gravity = Physics::Vector3(0.0f, -9.81f, 0.0f);
    PhysicsECS::PhysicsWorldService::Initialize(physCfg);

    // Scene content
    GroundEntities groundEnts = CreateGround(world);
    std::vector<ECS::EntityHandle> spheres = CreateSpheres(world, /*countX*/ 50, /*countZ*/ 50);

    // Camera + view setup (owned by RenderServices, independent of ECS).
    const Rendering::CameraId camId = rs->Views().AllocateCamera("DemoCamera");
    const Rendering::ViewId viewId = rs->Views().AllocateView("DemoView", camId);
    rs->Views().SetViewRenderLayerMask(viewId, 1u);
    rs->Views().SetViewWorldId(viewId, world.GetWorldId());

    // Fixed timestep
    constexpr float fixedDt = 1.0f / 60.0f;
    float accumulator = 0.0f;
    auto last = Clock::now();


    // Prime transforms / physics bodies so we have something to render immediately.
    xfSystem.Update(world, 0.0f);
    physicsInit.Update(world, 0.0f);

    bool loggedGroundOnce = false;
    bool loggedLowestSphereOnce = false;
    bool loggedPostOnce = false;

    // Log immediately after first init to catch any "spawned in ground" cases.
    if (auto* pw = PhysicsECS::PhysicsWorldService::TryGet())
    {
        LogLowestSphere("AfterInit", world, pw, spheres);
        loggedLowestSphereOnce = true;
    }

    while (!window.ShouldClose() && !state.quitRequested)
    {
        Platform::Window::PollEvents();

        // Reset requested: destroy spheres (including physics bodies) + respawn.
        if (state.resetRequested)
        {
            state.resetRequested = false;

            auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
            DestroyEntitiesImmediateAndCleanupPhysics(world, pw, spheres);

            spheres = CreateSpheres(world, /*countX*/ 50, /*countZ*/ 50);

            // Ensure transforms + physics bodies are recreated before we step.
            xfSystem.Update(world, 0.0f);
            physicsInit.Update(world, 0.0f);

            if (auto* pw2 = PhysicsECS::PhysicsWorldService::TryGet())
            {
                LogLowestSphere("AfterResetInit", world, pw2, spheres);
            }

            // Avoid carrying over accumulated time from before the reset.
            accumulator = 0.0f;
            loggedGroundOnce = false;
        }

        // Frame timing
        const auto now = Clock::now();
        const std::chrono::duration<float> dt = now - last;
        last = now;
        accumulator += std::min(dt.count(), 0.25f);

        // Begin frame (swapchain)
        if (!device->BeginFrame())
        {
            int fbW = 0, fbH = 0;
            window.GetFramebufferSize(fbW, fbH);
            renderCtx.RecreateWindowTargetSwapchain(static_cast<uint32>(std::max(1, fbW)), static_cast<uint32>(std::max(1, fbH)));
            continue;
        }

        frame.BeginFrame(rgFrameIndex++);

        // Clear config consumed at world-pass declaration (reverse-Z far depth).
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        clear.clearColorValue[0] = 0.08f;
        clear.clearColorValue[1] = 0.08f;
        clear.clearColorValue[2] = 0.10f;
        clear.clearColorValue[3] = 1.0f;
        clear.clearDepthValue = 0.0f;
        rs->Views().SetViewTargets(viewId, 0, 0, 0, clear);

        // Update camera matrices.
        uint32 w = 0, h = 0;
        float aspect = 16.0f / 9.0f;
        if (device->GetSwapchainSize(w, h) && h != 0)
        {
            aspect = static_cast<float>(w) / static_cast<float>(h);
        }
        const Mathematics::Vector3 eye(0.0f, 10.0f, 20.0f);
        const Mathematics::Vector3 at(0.0f, 0.0f, 0.0f);
        const Mathematics::Vector3 up(0.0f, 1.0f, 0.0f);
        const Mathematics::Matrix4x4 view = Mathematics::MakeLookAtLH(eye, at, up);
        const Mathematics::Matrix4x4 proj = Mathematics::MakePerspectiveLH_ZO_ReverseZ(60.0f * (3.14159265358979323846f / 180.0f), aspect, 0.1f, 200.0f);
        const Mathematics::Matrix4x4 viewProj = proj * view;

        Rendering::CameraData cd{};
        std::memcpy(cd.view, view.Data(), sizeof(cd.view));
        std::memcpy(cd.proj, proj.Data(), sizeof(cd.proj));
        std::memcpy(cd.viewProj, viewProj.Data(), sizeof(cd.viewProj));
        rs->Views().SetCameraData(camId, cd);
        rs->Views().SetViewCamera(viewId, camId);

        // Run fixed-step simulation + extraction.
        // Ensure WorldTransform exists for renderables even on frames where we don't step.
        xfSystem.Update(world, 0.0f);
        physicsInit.Update(world, 0.0f);

        if (!loggedLowestSphereOnce)
        {
            if (auto* pw = PhysicsECS::PhysicsWorldService::TryGet())
            {
                LogLowestSphere("FirstFrame", world, pw, spheres);
            }
            loggedLowestSphereOnce = true;
        }

        // One-shot diagnostics: confirm we have a static ground body after init.
        if (!loggedGroundOnce)
        {
            auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
            if (pw)
            {
                world.Query<ECS::Read<GameEngine::Components::PhysicsBody>>().Each(
                    [&](ECS::EntityHandle /*e*/, const GameEngine::Components::PhysicsBody& pb)
                    {
                        if (loggedGroundOnce)
                            return;
                        if (!pb.initialized || pb.motionType != GameEngine::Physics::MotionType::Static)
                            return;
                        if (!pw->IsBodyValid(pb.body))
                            return;

                        const auto tPhys = pw->GetBodyTransform(pb.body);
                        Logger::Log::Info("[Demo] Ground body static at y={}", tPhys.position.y);
                        loggedGroundOnce = true;
                    });
            }
        }

        while (accumulator >= fixedDt)
        {
            physicsStep.Update(world, fixedDt);
            physicsWriteback.Update(world, fixedDt);
            accumulator -= fixedDt;
        }

        // Extract renderables for current transforms and build RG passes.
        extraction.Update(world, fixedDt);

        // One-shot diagnostic: confirm the post entity has a sane world transform
        // and that it got a GPU instance index assigned.
        if (!loggedPostOnce && groundEnts.post.IsValid() && world.IsValid(groundEnts.post))
        {
            loggedPostOnce = true;
            if (const auto* wt = world.GetComponent<GameEngine::Components::WorldTransform>(groundEnts.post))
            {
                const float wx = wt->matrix[12];
                const float wy = wt->matrix[13];
                const float wz = wt->matrix[14];

                const auto* mg = world.GetComponent<GameEngine::Components::MeshGPUData>(groundEnts.post);
                const uint32 inst = mg ? mg->instanceIndex : 0xFFFFFFFFu;
                Logger::Log::Info("[Demo] Post entity={} worldPos=({}, {}, {}) instanceIndex={}",
                                  groundEnts.post.id,
                                  wx,
                                  wy,
                                  wz,
                                  inst);
            }
            else
            {
                Logger::Log::Warning("[Demo] Post entity={} missing WorldTransform", groundEnts.post.id);
            }
        }

        // Build per-view batch keys, then declare the full ForwardPlus spine on
        // the immediate-mode frame (mirrors PlayerApplication::RenderFrameRG2).
        rs->BuildWorldBatchKeys();
        rs->WriteViewLightBuffer(viewId);

        // Per-frame view targets sized to the swapchain (handles resize): HDR
        // color + depth + single-sample resolve, all pool imports.
        const uint32_t samples = rs->GetDefaultMSAASampleCount();
        Rendering::TextureDesc colorDesc{};
        colorDesc.width = w;
        colorDesc.height = h;
        colorDesc.depth = 1;
        colorDesc.mipLevels = 1;
        colorDesc.arrayLayers = 1;
        colorDesc.sampleCount = samples;
        colorDesc.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
        colorDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                          static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
        colorDesc.debugName = "PhysicsDemo.Color";
        Rendering::TextureDesc depthDesc = colorDesc;
        depthDesc.format =
            static_cast<uint32_t>(device->GetCapabilities().preferredDepthAndStencilFormat);
        depthDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil) |
                          static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
        depthDesc.debugName = "PhysicsDemo.Depth";
        const Rendering::RenderGraph::RGTexture color =
            frame.ImportPersistentTexture("PhysicsDemo.Color", colorDesc);
        const Rendering::RenderGraph::RGTexture depth =
            frame.ImportPersistentTexture("PhysicsDemo.Depth", depthDesc);
        Rendering::RenderGraph::RGTexture resolve{};
        if (samples > 1)
        {
            Rendering::TextureDesc resolveDesc = colorDesc;
            resolveDesc.sampleCount = 1;
            resolveDesc.debugName = "PhysicsDemo.Resolve";
            resolve = frame.ImportPersistentTexture("PhysicsDemo.Resolve", resolveDesc);
        }

        const Rendering::RenderGraph::RGTexture backbuffer = frame.ImportBackbuffer();

        const Engine::Renderer::Pipeline::ViewTargetsRG vt{viewId, color, depth, resolve};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets =
            std::span<const Engine::Renderer::Pipeline::ViewTargetsRG>(&vt, 1);
        params.BuildWorldDrawListsIfReady = false; // BuildWorldBatchKeys above built the keys
        rs->Spine().BuildFrameGraph(frame, params);

        // Terminal encode: the pipeline's FinalColor -> backbuffer (the single
        // OETF owner for every swapchain config).
        const auto out = rs->GetPipelineOutputRG(frame, viewId);
        if (out.Out.IsValid() && backbuffer.IsValid())
            Rendering::Passes::AddSRGBEncodePassRG(
                frame, out.Out, backbuffer,
                {.InputSpace = Rendering::Passes::FinalizeInputSpace::Linear,
                 .Quantizer = Rendering::Passes::FinalizeQuantizer::Destination});

        frame.Execute();
        rs->Spine().OnFrameSubmittedRG(frame, frame.SubmissionToken());
        device->Present();
    }

    PhysicsECS::PhysicsWorldService::Shutdown();
    renderCtx.Shutdown();
    window.Destroy();
    glfwTerminate();
    return 0;
}

