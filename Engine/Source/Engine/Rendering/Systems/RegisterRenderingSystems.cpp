#include "ECSModules/Rendering/Systems/RegisterRenderingSystems.h"
#include "ECS/SystemScheduling.h"

#include "ECSModules/Rendering/Systems/GamepadCameraControllerSystem.h"
#include "ECSModules/Rendering/Systems/AnimationEventCollectorSystem.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "ECSModules/Rendering/Systems/CharacterGraphParamSystem.h"
#include "ECSModules/Rendering/Systems/AnimationGraphSystem.h"
#include "ECSModules/Rendering/Systems/TimelinePlaybackSystem.h"
#include "ECSModules/Rendering/Systems/ValueCurveSystem.h"
#include "Core/Engine.h"
#include "ECSModules/Rendering/Systems/HumanoidRetargetSystem.h"
#include "ECSModules/Rendering/Systems/RigidAnimationSystem.h"
#include "ECSModules/Rendering/Systems/SkinningUploadSystem.h"
#include "ECSModules/Rendering/Systems/MorphTargetSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "ECSModules/Rendering/Systems/HLODSelectSystem.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "Particles/Systems/ParticleSimulationSystem.h"
#include "ECSModules/Rendering/Systems/CameraSystem.h"
#include "ECSModules/Rendering/Systems/RenderGraphBuildSystem.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "ECSModules/Rendering/Systems/AmbientLightSystem.h"
#include "ECSModules/Rendering/Systems/DDGIVolumeSystem.h"
#include "Engine/Rendering/DDGIFieldRanges.h"
#include "ECSModules/Rendering/Systems/ReflectionProbeSystem.h"

#include "ECS/DisabledInHierarchySystem.h"
#include "Scripting/NativePostSimulationSystem.h"

namespace GameEngine { namespace Engine::Renderer {

using ::GameEngine::Particles::ParticleWorldState;
using ::GameEngine::Particles::ParticleSimulationSystem;

void AddRenderingSystemsToSchedule(ECS::SystemScheduleBuilder& schedule, RenderServices* renderServices)
{
    using namespace ECS;

    // Names used for dependencies (shared with other modules that want to hook in)
    constexpr const char* kGamepadCamera = "GamepadCameraController";
    constexpr const char* kTimelinePlayback = "TimelinePlayback";
    constexpr const char* kCharGraphParams = "CharacterGraphParams";
    constexpr const char* kAnimEvents = "AnimationEventCollector";
    constexpr const char* kAnimGraph = "AnimationGraph";
    constexpr const char* kAnim = "Animation";
    constexpr const char* kCurve = "ValueCurve";
    constexpr const char* kHumanoidRetarget = "HumanoidRetarget";
    constexpr const char* kRigidAnim = "RigidAnimation";
    constexpr const char* kSkin = "SkinningUpload";
    constexpr const char* kMorph = "MorphTarget";
    constexpr const char* kHierarchy = "TransformHierarchy";
    constexpr const char* kHlodSelect = "HLODSelect";
    constexpr const char* kExtract = "RenderExtraction";
    constexpr const char* kNativePostSimulation = "NativePostSimulation";
    constexpr const char* kCamera = "Camera";
    constexpr const char* kSkyEnv = "SkyEnvironment";
    constexpr const char* kAmbientLight = "AmbientLight";
    constexpr const char* kReflectionProbe = "ReflectionProbe";
    constexpr const char* kDDGIVolume = "DDGIVolume";
    constexpr const char* kRG = "RenderGraphBuild";

    // Enable state is derived before anything reads the world. It mutates
    // archetypes, so it declares exclusivity and the wave joins around it;
    // sorting first in the Early phase puts it ahead of every other system that
    // opens the frame. Not a rendering system — it lives here for the same
    // reason the TLAS and native post-simulation systems do, this being where
    // the engine assembles the default world's schedule.
    schedule.Add<DisabledInHierarchySystem>("DisabledInHierarchy", SystemPhase::Early, 0, {});

    schedule.Add<TimelinePlaybackSystem>(kTimelinePlayback, SystemPhase::Animation, 0, {}, EngineCore::GetInstance().GetAudioSystem());
    schedule.Add<CharacterGraphParamSystem>(kCharGraphParams, SystemPhase::Animation, 1, {kTimelinePlayback});
    // Empties each Animator's event collector before any playback in the wave fires into it. It writes Animator,
    // as the two before it do, so it runs after them.
    schedule.Add<AnimationEventCollectorSystem>(kAnimEvents, SystemPhase::Animation, 1, {kTimelinePlayback, kCharGraphParams});
    schedule.Add<AnimationGraphSystem>(kAnimGraph, SystemPhase::Animation, 1, {kTimelinePlayback, kCharGraphParams, kAnimEvents}, renderServices);
    schedule.Add<AnimationSystem>(kAnim, SystemPhase::Animation, 2, {kTimelinePlayback, kAnimGraph}, renderServices);
    schedule.Add<ValueCurveSystem>(kCurve, SystemPhase::Animation, 3, {kTimelinePlayback, kAnim});
    // Cross-rig humanoid retarget runs after same-rig sampling so its
    // CPU-fallback-emitted skin matrices reach SkinningUpload alongside
    // AnimationSystem's. Pre-Skinning ordering is the contract.
    schedule.Add<HumanoidRetargetSystem>(kHumanoidRetarget, SystemPhase::Animation, 4, {kAnim}, renderServices);
    schedule.Add<RigidAnimationSystem>(kRigidAnim, SystemPhase::Animation, 5, {kAnim});
    schedule.Add<SkinningUploadSystem>(kSkin, SystemPhase::Skinning, 0, {kAnim, kHumanoidRetarget, kAnimGraph}, renderServices);

    // Gamepad fly camera writes local Transform before hierarchy propagates to WorldTransform.
    schedule.Add<GamepadCameraControllerSystem>(
        kGamepadCamera, SystemPhase::Extraction, 0, {kSkin}, EngineCore::GetInstance().GetInputSystem());

    // After the sky too: the sky turns its driven sun light, and that rotation reaches this frame's
    // WorldTransform here.
    schedule.Add<TransformHierarchySystem>(
        kHierarchy, SystemPhase::Extraction, 1,
        {kTimelinePlayback, kAnim, kCurve, kRigidAnim, kSkin, kGamepadCamera, kSkyEnv});
    schedule.Add<MorphTargetSystem>(kMorph, SystemPhase::Extraction, 2, {kHierarchy}, renderServices);

    // HLOD residency switch: evicts/re-admits cluster members vs their proxy so
    // extraction (which it forces onto the full lane on a flip) sees the winning
    // set. Ordered after the hierarchy (needs current WorldTransforms) and before
    // extraction (kExtract depends on it).
    schedule.Add<HLODSelectSystem>(kHlodSelect, SystemPhase::Extraction, 5, {kHierarchy}, renderServices);

    // The systems that write WorldTransform after the hierarchy. The wave
    // solver orders by declared edges only (phase and order sort within a
    // wave), so anything that must see this frame's movement names them.
    // Each is optional: a runtime without that module still builds.
    const auto afterMovementWriters = [](std::vector<SystemDependency> deps) {
        deps.push_back(OptionalDependency("NavigationMovementSystem"));
        deps.push_back(OptionalDependency("PhysicsWriteback"));
        deps.push_back(OptionalDependency("CharacterControllerWriteback"));
        deps.push_back(OptionalDependency("OceanBuoyancy"));
        return deps;
    };

    // Game presentation derives from the movement completed in this engine
    // tick. Exclusivity pins arbitrary user-world access to the caller thread
    // and joins the wave around it. Camera/terrain preparation stays earlier:
    // physics consumes that terrain.
    schedule.Add<NativePostSimulationSystem>(kNativePostSimulation, SystemPhase::Extraction, 6,
        afterMovementWriters({kHierarchy, kMorph, kHlodSelect}), renderServices);

    // Extraction reads this frame's WorldTransforms, so it runs after the
    // movement writers and after the native hook that derives presentation
    // from them. Sector ownership for picking is handled inside the Scene
    // TLAS (centroid-based replication in ReconcileFromRecords); no
    // dedicated assignment system is needed.
    // Particle simulation shares the world state with extraction (which
    // consumes the emitted instances), so it is ordered before it and, like
    // extraction, after the movement writers — attached/physics-driven
    // emitters must step from this tick's transforms.
    auto particles = std::make_shared<ParticleWorldState>();
    particles->Assets = EngineCore::GetInstance().TryGetAssetManager();
    schedule.Add<ParticleSimulationSystem>("ParticleSimulation", SystemPhase::Extraction, 6,
        afterMovementWriters({kHierarchy, kNativePostSimulation}), particles);
    schedule.Add<RenderExtractionSystem>(kExtract, SystemPhase::Extraction, 6,
        afterMovementWriters({kHierarchy, kMorph, kHlodSelect, kNativePostSimulation,
                              "ParticleSimulation", kSkyEnv}), renderServices, particles);

    // The scene TLAS has no per-frame system: its consumers bring it current
    // on demand (Scene/SceneTlas.h, RequestCurrent).

    schedule.Add<CameraSystem>(kCamera, SystemPhase::Camera, 0, {}, renderServices);

    // The sky after the camera, and after the other writers of a light's intensity (value curves and
    // timeline tracks), so the intensity a sky curve writes into its sun light is the frame's last.
    schedule.Add<SkyEnvironmentSystem>(kSkyEnv, SystemPhase::Camera, 1, {kCamera, kCurve, kTimelinePlayback},
                                       renderServices);
    schedule.Add<ReflectionProbeSystem>(
        kReflectionProbe, SystemPhase::Camera, 3, {kCamera, kSkyEnv}, renderServices);
    // Opt-in ambient floor, chained AFTER the probe system. SkyEnvironment,
    // ReflectionProbe and this system all call RenderServices::EnsureFeature on
    // first touch; EnsureFeature is internally synchronized, so sharing a wave
    // with them is safe and this edge is a candidate for relaxation back to
    // {kCamera}. It stands because relaxing it is an ordering change, not just
    // a lock removal: all three write the same IBL feature, and nothing else
    // establishes that the probe's writes land before this system's ambient
    // push. Chaining costs nothing: this is a pure CPU push into the IBL
    // feature that UploadEnvData reads later during record.
    schedule.Add<AmbientLightSystem>(kAmbientLight, SystemPhase::Camera, 4, {kReflectionProbe}, renderServices);
    // DDGI volume extraction: needs WorldTransform (post-hierarchy, so
    // {kCamera} suffices — the same dependency SkyEnvironmentSystem uses)
    // but touches no IBL feature state, so it carries no ordering edge to
    // the Sky/ReflectionProbe/AmbientLight chain above.
    static const bool s_DDGIRangesRegisteredScheduled =
        [] { RegisterDDGIFieldRanges(); return true; }();
    (void)s_DDGIRangesRegisteredScheduled;
    schedule.Add<DDGIVolumeSystem>(kDDGIVolume, SystemPhase::Camera, 4, {kCamera}, renderServices);

    // RenderGraph build is responsible for invoking RenderServices::BuildFrameGraph,
    // which schedules per-view GPU culling (ScheduleViewCullingDispatches) before
    // the bucketer. There is no longer a separate CullingSystem ECS pass, and
    // LOD selection is GPU-only (draw_command_scatter.comp ge_SelectLOD) — the
    // former CPU VisibilityAndLODSystem was redundant and semantically wrong.
    // BuildFrameGraph does the frame's heavy host-side housekeeping and reads
    // what every other system produced, so it runs alone in the last wave,
    // after late package systems too, rather than concurrent with any of them.
    schedule.Add<RenderGraphBuildSystem>(kRG, SystemPhase::Render, 0, {kCamera, kExtract}, renderServices);
    schedule.RunAfterAllOthers(kRG);
}

} } // namespace GameEngine::Engine::Renderer
