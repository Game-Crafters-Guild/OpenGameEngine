// The particle simulation system over an ECS world: budgets, playback controls, seeks, stack assets
// and sub-emitters, with no camera or graphics device. Stack assets come from an asset manager of
// the test's own.

#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECSModules/Rendering/Systems/RegisterRenderingSystems.h"
#include "Particles/Assets/ParticleStackAsset.h"
#include "Particles/ParticleStackAuthoring.h"
#include "Particles/ParticleStackDocument.h"
#include "Particles/Processors/ParticleDragProcessor.h"
#include "Particles/Systems/ParticleSimulationSystem.h"
#include "PathfindingECS/Systems/RegisterPathfindingSystems.h"
#include "PhysicsECS/Systems/RegisterPhysicsSystems.h"
#include "Types/StringId.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Components;
using GameEngine::Particles::ParticleSimulationSystem;

namespace
{
constexpr float kFrame = 1.0f / 30.0f;
// Seven ticks at 30 per second: long enough for the default stack to emit.
constexpr float kLongFrame = 0.25f;

// A stack document around the processors of its first phase, in the JSON authoring form.
std::string StackJson(float lifetime, const std::string& processors)
{
    return R"({"version":1,"entryPhase":1,"lifetime":)" + std::to_string(lifetime) +
           R"(,"phases":[{"id":1,"label":"Spawn","processors":[)" + processors + "]}]}";
}

std::string Burst(uint32 count)
{
    return R"({"type":"emitBurst","id":2,"stage":"emission","parameters":{"count":)" + std::to_string(count) + "}}";
}

std::vector<uint8> Bytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

void WriteText(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string SpawnInChild(uint32 count)
{
    return R"(,{"type":"event","id":3,"stage":"event","parameters":{"trigger":"birth","action":"emit","subEmitter":"subEmitter1","count":)" +
           std::to_string(count) + "}}";
}

const Particles::ParticleEmitterState& Emitter(const ParticleSimulationSystem& system, ECS::EntityHandle entity)
{
    return system.State()->Emitters.at(entity.id);
}

class ParticleSimulationSystemTest : public testing::Test
{
  protected:
    AssetManager Assets;

    // Particle state that resolves stacks through the test's asset manager.
    std::shared_ptr<Particles::ParticleWorldState> NewState()
    {
        auto state = std::make_shared<Particles::ParticleWorldState>();
        state->Assets = &Assets;
        return state;
    }

    // A loaded stack asset read from `json`.
    std::shared_ptr<Particles::ParticleStackAsset> AddStackAsset(const std::string& json)
    {
        const GUID guid = GUID::Generate();
        auto asset = std::make_shared<Particles::ParticleStackAsset>(guid, "test.particlestack");
        EXPECT_TRUE(asset->LoadFromData(Bytes(json)))
            << (asset->Diagnostics().empty() ? json : asset->Diagnostics().front().Message);
        Assets.RegisterLoadedAsset(guid, asset);
        return asset;
    }

    GUID AddStack(const std::string& json) { return AddStackAsset(json)->GetGUID(); }

    // An emitter bursting `count` particles once, with a fixed seed.
    ECS::EntityHandle AddEmitter(ECS::World& world, uint32 count = 12, float lifetime = 1.0f,
                                 const std::string& extraProcessors = {})
    {
        const auto entity = world.CreateEntity();
        ParticleEmitter3D emitter;
        emitter.Stack.Set(AddStack(StackJson(lifetime, Burst(count) + extraProcessors)));
        emitter.Amount = count;
        emitter.Seed = 17;
        world.AddComponentImmediate(entity, emitter);
        world.AddComponentImmediate(entity, WorldTransform{});
        world.AddComponentImmediate(entity, ParticlePlayback{});
        return entity;
    }

    // An emitter that emits only what spawn rules send it, in sub-emitter slot 1 of `parent`.
    ECS::EntityHandle AddChild(ECS::World& world, ECS::EntityHandle parent)
    {
        const auto child = world.CreateEntity();
        ParticleEmitter3D emitter;
        emitter.Stack.Set(AddStack(StackJson(1.0f, "")));
        emitter.Amount = 8;
        emitter.Seed = 5;
        world.AddComponentImmediate(child, emitter);
        world.AddComponentImmediate(child, WorldTransform{});
        world.AddComponentImmediate(child, ParticlePlayback{});
        world.GetComponentForWrite<ParticleEmitter3D>(parent)->SubEmitters[0].Emitter = child;
        return child;
    }
};
} // namespace

TEST_F(ParticleSimulationSystemTest, SimulatesWithoutCameraOrGraphicsDevice)
{
    ECS::World world;
    const auto emitter = AddEmitter(world);
    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);
    EXPECT_EQ(system.State()->LiveParticles, 12u);
    EXPECT_EQ(world.GetComponent<ParticlePlayback>(emitter)->SimulatedSteps, 1u);
    world.AddComponentImmediate(emitter, ECS::Disabled{});
    system.Update(world, kFrame);
    EXPECT_TRUE(system.State()->Emitters.empty());
}

TEST_F(ParticleSimulationSystemTest, UnifiedScheduleSharesStateWithExtraction)
{
    ECS::SystemManager manager;
    {
        ECS::SystemScheduleBuilder builder;
        Engine::Renderer::AddRenderingSystemsToSchedule(builder, nullptr);
        PhysicsECS::AddPhysicsSystemsToSchedule(builder);
        PathfindingECS::AddPathfindingSystemsToSchedule(builder);
        builder.BuildAndRegisterWithWaves(manager);
    }
    // Destroy the builder first: factory captures must not count as consumers.
    auto* simulation = manager.GetSystem<ParticleSimulationSystem>();
    ASSERT_NE(simulation, nullptr);
    ASSERT_NE(simulation->State(), nullptr);
    EXPECT_EQ(simulation->State().use_count(), 2) << "Simulation and extraction must own the same particle state";
    simulation->State()->Assets = &Assets;
    ECS::World world;
    AddEmitter(world);
    simulation->Update(world, kFrame);
    EXPECT_EQ(simulation->State()->LiveParticles, 12u);
}

TEST_F(ParticleSimulationSystemTest, WorldClearRestartsSeekForAReusedEntityHandle)
{
    ECS::World world;
    const auto previous = AddEmitter(world, 12, 10.0f);
    const auto emitter = *world.GetComponent<ParticleEmitter3D>(previous);
    ParticleSimulationSystem system(NewState());
    auto* playback = world.GetComponentForWrite<ParticlePlayback>(previous);
    playback->Paused = true;
    playback->SeekTime = 0.5;
    playback->Seek = 1;
    system.Update(world, 0);
    ASSERT_NEAR(world.GetComponent<ParticlePlayback>(previous)->SimulatedTime, 0.5, 1e-6);

    // The counter still owns requests within one scene: changing only the target does not seek.
    world.GetComponentForWrite<ParticlePlayback>(previous)->SeekTime = 1.0;
    system.Update(world, 0);
    ASSERT_NEAR(world.GetComponent<ParticlePlayback>(previous)->SimulatedTime, 0.5, 1e-6);

    const auto worldId = world.GetWorldId();
    world.Clear();
    ASSERT_EQ(world.GetWorldId(), worldId);
    const ECS::EntityHandle reloaded = world.CreateEntity();
    ASSERT_EQ(reloaded, previous) << "The new scene must reuse the complete handle";
    world.AddComponentImmediate(reloaded, emitter);
    world.AddComponentImmediate(reloaded, WorldTransform{});
    ParticlePlayback newPlayback;
    newPlayback.Paused = true;
    newPlayback.SeekTime = 1.0;
    newPlayback.Seek = 1;
    world.AddComponentImmediate(reloaded, newPlayback);

    system.Update(world, 0);
    EXPECT_NEAR(world.GetComponent<ParticlePlayback>(reloaded)->SimulatedTime, 1.0, 1e-6);
    EXPECT_EQ(system.State()->LiveParticles, 12u);
    EXPECT_EQ(system.State()->Assets, &Assets);
}

TEST_F(ParticleSimulationSystemTest, PauseStepRestartAndShrinkingWorldBudget)
{
    ECS::World world;
    const auto first = AddEmitter(world, 12, 10.0f);
    const auto second = AddEmitter(world, 12, 10.0f);
    world.AddComponentImmediate(first, ParticleWorldSettings{15});
    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);
    EXPECT_EQ(system.State()->LiveParticles, 15u);
    auto* playback = world.GetComponentForWrite<ParticlePlayback>(first);
    playback->Paused = true;
    const auto time = playback->SimulatedTime;
    system.Update(world, 0.1f);
    EXPECT_EQ(world.GetComponent<ParticlePlayback>(first)->SimulatedTime, time);
    world.GetComponentForWrite<ParticlePlayback>(first)->SingleStep++;
    world.GetComponentForWrite<ParticleWorldSettings>(first)->MaxParticles = 2;
    system.Update(world, 0.1f);
    EXPECT_EQ(system.State()->LiveParticles, 2u);
    EXPECT_EQ(Emitter(system, second).Simulation.Count(), 0u);
    playback = world.GetComponentForWrite<ParticlePlayback>(first);
    EXPECT_GT(playback->SimulatedTime, time);
    ++playback->Restart;
    system.Update(world, 0.0f);
    EXPECT_EQ(system.State()->LiveParticles, 0u);
    EXPECT_EQ(world.GetComponent<ParticlePlayback>(first)->SimulatedTime, 0.0);
}

TEST_F(ParticleSimulationSystemTest, PendingSeekCannotOverdrawShrinkingWorldBudget)
{
    ECS::World world;
    const auto first = AddEmitter(world, 12, 120.0f);
    const auto second = AddEmitter(world, 12, 120.0f);
    world.AddComponentImmediate(first, ParticleWorldSettings{15});
    auto* playback = world.GetComponentForWrite<ParticlePlayback>(first);
    playback->Paused = true;
    playback->SeekTime = 60;
    ++playback->Seek;

    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);
    ASSERT_TRUE(Emitter(system, first).PreviewSeek.Pending);
    ASSERT_EQ(system.State()->LiveParticles, 15u);

    world.GetComponentForWrite<ParticleWorldSettings>(first)->MaxParticles = 5;
    system.Update(world, kFrame);
    EXPECT_TRUE(Emitter(system, first).PreviewSeek.Pending);
    EXPECT_EQ(Emitter(system, first).Simulation.Count(), 5u);
    EXPECT_EQ(Emitter(system, second).Simulation.Count(), 0u);
    EXPECT_EQ(system.State()->LiveParticles, 5u);
}

TEST_F(ParticleSimulationSystemTest, PreviewSeekYieldsAndLatestRequestWins)
{
    ECS::World world;
    const auto entity = AddEmitter(world, 12, 1.0f, SpawnInChild(1));
    ParticleSimulationSystem system(NewState());
    auto* playback = world.GetComponentForWrite<ParticlePlayback>(entity);
    playback->Paused = true;
    playback->SeekTime = 60;
    ++playback->Seek;
    system.Update(world, 0);
    const auto& entry = Emitter(system, entity);
    EXPECT_TRUE(entry.PreviewSeek.Pending);
    EXPECT_LE(entry.Simulation.StepsLastFrame(), 64u);
    EXPECT_LT(entry.Simulation.Elapsed(), 60);

    playback = world.GetComponentForWrite<ParticlePlayback>(entity);
    playback->SeekTime = 0.217;
    ++playback->Seek;
    for (int frame = 0; frame < 100 && Emitter(system, entity).PreviewSeek.Pending; ++frame)
        system.Update(world, 0);
    EXPECT_FALSE(entry.PreviewSeek.Pending);
    EXPECT_NEAR(entry.Simulation.Elapsed(), 0.2, 1e-6);
    EXPECT_TRUE(entry.Simulation.SpawnEvents().empty());

    playback = world.GetComponentForWrite<ParticlePlayback>(entity);
    playback->SeekTime = 60;
    ++playback->Seek;
    system.Update(world, 0);
    ++world.GetComponentForWrite<ParticlePlayback>(entity)->Restart;
    system.Update(world, 0);
    EXPECT_FALSE(entry.PreviewSeek.Pending);
    EXPECT_DOUBLE_EQ(entry.Simulation.Elapsed(), 0);

    ++world.GetComponentForWrite<ParticlePlayback>(entity)->Seek;
    system.Update(world, 0);
    const auto beforeStep = entry.Simulation.Elapsed();
    ++world.GetComponentForWrite<ParticlePlayback>(entity)->SingleStep;
    system.Update(world, 0);
    EXPECT_FALSE(entry.PreviewSeek.Pending);
    EXPECT_NEAR(entry.Simulation.Elapsed(), beforeStep + 1.0 / 30.0, 1e-6);
}

// A structural stack edit restarts the runtime. A preview seek still under way when it lands starts
// again on the new stack, so the preview reaches the scrubbed time with the edit in it.
TEST_F(ParticleSimulationSystemTest, AStructuralStackEditDuringASeekSeeksAgainOnTheNewStack)
{
    ECS::World world;
    const auto asset = AddStackAsset(StackJson(10.0f, Burst(12)));
    const auto entity = world.CreateEntity();
    ParticleEmitter3D emitter;
    emitter.Stack.Set(asset->GetGUID());
    emitter.Amount = 64;
    emitter.Seed = 17;
    world.AddComponentImmediate(entity, emitter);
    world.AddComponentImmediate(entity, WorldTransform{});
    world.AddComponentImmediate(entity, ParticlePlayback{});
    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);

    auto* playback = world.GetComponentForWrite<ParticlePlayback>(entity);
    playback->Paused = true;
    playback->SeekTime = 60;
    ++playback->Seek;
    system.Update(world, 0);
    ASSERT_TRUE(Emitter(system, entity).PreviewSeek.Pending);

    auto document = *asset->Document();
    uint32 added = 0;
    std::string error;
    ASSERT_TRUE(Particles::AddProcessor(document, document.Phases[0].Id,
                                        Particles::MakeProcessorInstance(Particles::ParticleDragProcessor()), added,
                                        error))
        << error;
    ASSERT_TRUE(asset->SetDocument(document));
    for (int frame = 0; frame < 1000 && (frame == 0 || Emitter(system, entity).PreviewSeek.Pending); ++frame)
        system.Update(world, 0);
    const auto& entry = Emitter(system, entity);
    EXPECT_EQ(entry.Stack, asset->Compiled());
    EXPECT_FALSE(entry.PreviewSeek.Pending);
    // Within half a tick of the scrubbed time.
    EXPECT_NEAR(entry.Simulation.Elapsed(), 60.0, 0.5 * kFrame);
}

TEST_F(ParticleSimulationSystemTest, SubEmitterUsesWorldPositionAndWaitsForNextUpdate)
{
    ECS::World world;
    const auto parent = AddEmitter(world, 1, 1.0f, SpawnInChild(3));
    world.GetComponentForWrite<WorldTransform>(parent)->matrix[12] = 7.0f;
    const auto child = AddChild(world, parent);
    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);
    EXPECT_EQ(Emitter(system, child).Simulation.Count(), 0u);
    system.Update(world, kFrame);
    const auto& simulation = Emitter(system, child).Simulation;
    ASSERT_EQ(simulation.Count(), 3u);
    for (uint32 index = 0; index < simulation.Count(); ++index)
        EXPECT_FLOAT_EQ(simulation.Channels().Positions()[index].x, 7.0f);
}

TEST_F(ParticleSimulationSystemTest, ReusingSystemWithAnotherWorldDropsPriorState)
{
    ECS::World first;
    ECS::World second;
    AddEmitter(first, 12);
    AddEmitter(second, 3);
    ParticleSimulationSystem system(NewState());
    system.Update(first, kFrame);
    EXPECT_EQ(system.State()->LiveParticles, 12u);
    system.Update(second, kFrame);
    EXPECT_EQ(system.State()->LiveParticles, 3u);
    EXPECT_EQ(system.State()->WorldId, second.GetWorldId());
}

TEST_F(ParticleSimulationSystemTest, SpawnRulesSpawnInTheEntityTheirSlotNames)
{
    ECS::World world;
    const auto parent = AddEmitter(world, 1, 1.0f, SpawnInChild(2));
    const auto child = AddChild(world, parent);
    ParticleSimulationSystem system(NewState());
    const auto spawnedInChild = [&]
    {
        ++world.GetComponentForWrite<ParticlePlayback>(parent)->Restart;
        ++world.GetComponentForWrite<ParticlePlayback>(child)->Restart;
        system.Update(world, kFrame);
        system.Update(world, kFrame);
        const auto found = system.State()->Emitters.find(child.id);
        return found == system.State()->Emitters.end() ? 0u : found->second.Simulation.Count();
    };
    EXPECT_EQ(spawnedInChild(), 2u);
    world.GetComponentForWrite<ParticleEmitter3D>(parent)->SubEmitters[0].Emitter = {};
    EXPECT_EQ(spawnedInChild(), 0u);
    world.GetComponentForWrite<ParticleEmitter3D>(parent)->SubEmitters[0].Emitter = child;
    EXPECT_EQ(spawnedInChild(), 2u);
    world.AddComponentImmediate(child, ECS::Disabled{});
    EXPECT_EQ(spawnedInChild(), 0u);
    EXPECT_TRUE(system.State()->Emitters.find(child.id) == system.State()->Emitters.end());
}

TEST_F(ParticleSimulationSystemTest, AGameEventSentThroughPlaybackRunsTheStacksGameEventRules)
{
    ECS::World world;
    const std::string popRule =
        R"(,{"type":"event","id":3,"stage":"event","parameters":{"trigger":"external","action":"kill","eventName":"Pop"}})";
    const auto entity = AddEmitter(world, 6, 5.0f, popRule);
    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);
    ASSERT_EQ(Emitter(system, entity).Simulation.Count(), 6u);

    auto* playback = world.GetComponentForWrite<ParticlePlayback>(entity);
    ASSERT_TRUE(playback->SendEvent("Other"_sid));
    system.Update(world, kFrame);
    EXPECT_EQ(Emitter(system, entity).Simulation.Count(), 6u);
    EXPECT_EQ(world.GetComponent<ParticlePlayback>(entity)->PendingEventCount, 0u);

    ASSERT_TRUE(world.GetComponentForWrite<ParticlePlayback>(entity)->SendEvent("Pop"_sid));
    system.Update(world, kFrame);
    system.Update(world, kFrame);
    EXPECT_EQ(Emitter(system, entity).Simulation.Count(), 0u);

    playback = world.GetComponentForWrite<ParticlePlayback>(entity);
    for (uint32 index = 0; index < ParticleMaxPendingEvents; ++index)
        EXPECT_TRUE(playback->SendEvent("Pop"_sid));
    EXPECT_FALSE(playback->SendEvent("Pop"_sid));
}

TEST_F(ParticleSimulationSystemTest, ChangingSimulationSpaceRestartsInsteadOfReinterpretingPositions)
{
    ECS::World world;
    const auto entity = AddEmitter(world, 1);
    world.GetComponentForWrite<WorldTransform>(entity)->matrix[12] = 100;
    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);
    ASSERT_EQ(Emitter(system, entity).Simulation.Count(), 1u);
    EXPECT_FLOAT_EQ(Emitter(system, entity).Simulation.Channels().Positions()[0].x, 100);
    world.GetComponentForWrite<ParticleEmitter3D>(entity)->LocalSpace = true;
    system.Update(world, kFrame);
    ASSERT_EQ(Emitter(system, entity).Simulation.Count(), 1u);
    EXPECT_FLOAT_EQ(Emitter(system, entity).Simulation.Channels().Positions()[0].x, 0);
}

TEST_F(ParticleSimulationSystemTest, EmittersRunTheStackTheyReferenceAndFailClosedWithout)
{
    ECS::World world;
    const auto emitter = AddEmitter(world);
    auto& stack = world.GetComponentForWrite<ParticleEmitter3D>(emitter)->Stack;
    const GUID burst = stack.ToGuid();
    // No reference runs the default stack.
    stack.Clear();
    ParticleSimulationSystem system(NewState());
    system.Update(world, kLongFrame);
    EXPECT_GT(system.State()->LiveParticles, 0u);
    EXPECT_TRUE(Emitter(system, emitter).StackError.empty());
    // A reference no asset carries runs nothing, and says the asset is missing.
    world.GetComponentForWrite<ParticleEmitter3D>(emitter)->Stack.Set(GUID::Generate());
    system.Update(world, kFrame);
    EXPECT_EQ(system.State()->LiveParticles, 0u);
    EXPECT_EQ(Emitter(system, emitter).StackError, "its stack asset is missing");
    // A loaded asset that holds no valid stack runs nothing, and says why.
    const GUID broken = GUID::Generate();
    auto unusable = std::make_shared<Particles::ParticleStackAsset>(broken, "broken.particlestack");
    EXPECT_FALSE(unusable->LoadFromData(Bytes("{broken}")));
    Assets.RegisterLoadedAsset(broken, unusable);
    world.GetComponentForWrite<ParticleEmitter3D>(emitter)->Stack.Set(broken);
    system.Update(world, kFrame);
    EXPECT_EQ(system.State()->LiveParticles, 0u);
    EXPECT_FALSE(Emitter(system, emitter).StackError.empty());
    world.GetComponentForWrite<ParticleEmitter3D>(emitter)->Stack.Set(burst);
    system.Update(world, kFrame);
    EXPECT_EQ(system.State()->LiveParticles, 12u);
    EXPECT_TRUE(Emitter(system, emitter).StackError.empty());
}

TEST_F(ParticleSimulationSystemTest, EditingAStackAssetUpdatesEveryEmitterRunningIt)
{
    ECS::World world;
    const auto asset = AddStackAsset(StackJson(10.0f, Burst(12)));
    ParticleEmitter3D emitter;
    emitter.Stack.Set(asset->GetGUID());
    emitter.Amount = 64;
    const auto first = world.CreateEntity();
    const auto second = world.CreateEntity();
    for (const auto entity : {first, second})
    {
        world.AddComponentImmediate(entity, emitter);
        world.AddComponentImmediate(entity, WorldTransform{});
    }
    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);
    ASSERT_EQ(system.State()->LiveParticles, 24u);
    EXPECT_EQ(Emitter(system, first).Stack, Emitter(system, second).Stack);

    // A parameter edit keeps the live particles; the next burst would be the edited one.
    auto document = *asset->Document();
    document.Lifetime = 20.0f;
    ASSERT_TRUE(asset->SetDocument(document));
    system.Update(world, kFrame);
    EXPECT_EQ(system.State()->LiveParticles, 24u);
    EXPECT_EQ(Emitter(system, first).Stack, asset->Compiled());
    EXPECT_FLOAT_EQ(Emitter(system, second).Stack->Lifetime, 20.0f);
}

// A parameter edit rebinds the running simulation instead of restarting it: the particles already
// alive keep their clock.
TEST_F(ParticleSimulationSystemTest, AParameterEditKeepsTheParticlesAliveOnTheirClock)
{
    ECS::World world;
    const auto asset = AddStackAsset(StackJson(10.0f, Burst(12)));
    ParticleEmitter3D emitter;
    emitter.Stack.Set(asset->GetGUID());
    emitter.Amount = 64;
    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, emitter);
    world.AddComponentImmediate(entity, WorldTransform{});
    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);
    system.Update(world, kFrame);
    const double elapsed = Emitter(system, entity).Simulation.Elapsed();
    ASSERT_GT(elapsed, 0.0);
    ASSERT_EQ(system.State()->LiveParticles, 12u);

    auto document = *asset->Document();
    document.Lifetime = 20.0f;
    ASSERT_TRUE(asset->SetDocument(document));
    system.Update(world, kFrame);
    const auto& running = Emitter(system, entity).Simulation;
    EXPECT_EQ(running.Stack(), asset->Compiled().get());
    EXPECT_GT(running.Elapsed(), elapsed) << "a restart would put the clock back to its first tick";
    EXPECT_EQ(system.State()->LiveParticles, 12u);
}

// A stack reference that names a loaded asset of another type is an error the emitter reports, not a
// stack it waits for.
TEST_F(ParticleSimulationSystemTest, AReferenceToAnAssetThatIsNotAStackIsReported)
{
    const GUID other = GUID::Generate();
    Assets.RegisterLoadedAsset(other, std::make_shared<BinaryAsset>(other, "other.bin", AssetType::Unknown));
    ECS::World world;
    const auto entity = world.CreateEntity();
    ParticleEmitter3D emitter;
    emitter.Stack.Set(other);
    world.AddComponentImmediate(entity, emitter);
    world.AddComponentImmediate(entity, WorldTransform{});
    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);
    EXPECT_EQ(system.State()->LiveParticles, 0u);
    EXPECT_NE(Emitter(system, entity).StackError.find("not a particle stack"), std::string::npos)
        << Emitter(system, entity).StackError;
}

TEST_F(ParticleSimulationSystemTest, AReloadTheAssetRefusesKeepsTheLastValidStackRunning)
{
    const auto file = std::filesystem::temp_directory_path() / "ge_particle_reload.particlestack";
    WriteText(file, StackJson(10.0f, Burst(12)));
    const GUID guid = GUID::Generate();
    auto asset = std::make_shared<Particles::ParticleStackAsset>(guid, file);
    ASSERT_TRUE(asset->Load());
    Assets.RegisterLoadedAsset(guid, asset);
    ECS::World world;
    const auto entity = world.CreateEntity();
    ParticleEmitter3D emitter;
    emitter.Stack.Set(guid);
    emitter.Amount = 64;
    world.AddComponentImmediate(entity, emitter);
    world.AddComponentImmediate(entity, WorldTransform{});
    ParticleSimulationSystem system(NewState());
    system.Update(world, kFrame);
    ASSERT_EQ(system.State()->LiveParticles, 12u);
    const auto running = asset->Compiled();

    WriteText(file, "{broken}");
    EXPECT_EQ(asset->Reload(), ReloadOutcome::Failed);
    EXPECT_EQ(asset->Compiled(), running);
    EXPECT_FALSE(asset->Diagnostics().empty());
    system.Update(world, kFrame);
    EXPECT_EQ(system.State()->LiveParticles, 12u);
    EXPECT_EQ(Emitter(system, entity).Stack, running);
    std::filesystem::remove(file);
}

TEST_F(ParticleSimulationSystemTest, AGeneralStackMakesRisingSmokeThatGrowsFadesAndReplaysUnderSeek)
{
    ECS::World world;
    const auto entity = AddEmitter(world, 400, 5.0f);
    auto* emitter = world.GetComponentForWrite<ParticleEmitter3D>(entity);
    emitter->TicksPerSecond = 60;
    const std::string processors =
        R"({"type":"emitRate","id":2,"stage":"emission","parameters":{"rate":{"mode":"constant","value":80}}},)"
        R"({"type":"shape","id":3,"stage":"birth","parameters":{"shape":"circle","radius":0.55}},)"
        R"({"type":"property","id":4,"stage":"birth","parameters":{"target":"velocity","operation":"set","value":[)"
        R"({"mode":"range","minimum":-0.1,"maximum":0.3},{"mode":"range","minimum":0.525,"maximum":0.875},)"
        R"({"mode":"range","minimum":-0.16,"maximum":0.24}]}},)"
        R"({"type":"property","id":5,"stage":"birth","parameters":{"target":"color","operation":"set","value":[)"
        R"({"mode":"constant","value":0.92},{"mode":"constant","value":0.9},{"mode":"constant","value":0.86},)"
        R"({"mode":"constant","value":1}]}},)"
        R"({"type":"property","id":6,"stage":"birth","parameters":{"target":"lifetime","operation":"set","value":[)"
        R"({"mode":"range","minimum":3.75,"maximum":6.25}]}},)"
        R"({"type":"property","id":7,"stage":"update","parameters":{"target":"size","operation":"set","value":[)"
        R"({"mode":"curve","curve":[{"time":0,"value":1.1},{"time":1,"value":3.2}]}]}},)"
        R"({"type":"property","id":8,"stage":"update","parameters":{"target":"alpha","operation":"set","value":[)"
        R"({"mode":"curve","curve":[{"time":0,"value":0},{"time":0.04,"value":1},{"time":0.83,"value":1},)"
        R"({"time":1,"value":0}]}]}},)"
        R"({"type":"noise","id":9,"stage":"update","parameters":{"strength":{"mode":"constant","value":0.2},)"
        R"("wavelength":{"mode":"constant","value":4},"scroll":{"mode":"constant","value":0.25}}})";
    world.GetComponentForWrite<ParticleEmitter3D>(entity)->Stack.Set(AddStack(StackJson(5.0f, processors)));
    ParticleSimulationSystem system(NewState());
    for (int tick = 0; tick < 240; ++tick)
        system.Update(world, 1.0f / 60.0f);
    const auto& simulation = Emitter(system, entity).Simulation;
    ASSERT_GT(simulation.Count(), 200u);
    EXPECT_LE(simulation.Count(), 400u);
    float maxSize = 0.0f;
    float maxHeight = 0.0f;
    float minAlpha = 1.0f;
    float maxAlpha = 0.0f;
    const auto& channels = simulation.Channels();
    for (uint32 index = 0; index < simulation.Count(); ++index)
    {
        EXPECT_FLOAT_EQ(channels.Colors()[index].x, 0.92f);
        EXPECT_FLOAT_EQ(channels.Colors()[index].y, 0.9f);
        EXPECT_FLOAT_EQ(channels.Colors()[index].z, 0.86f);
        maxSize = std::max(maxSize, channels.Sizes()[index]);
        maxHeight = std::max(maxHeight, channels.Positions()[index].y);
        minAlpha = std::min(minAlpha, channels.Colors()[index].w);
        maxAlpha = std::max(maxAlpha, channels.Colors()[index].w);
    }
    EXPECT_GT(maxSize, 2.0f);
    EXPECT_GT(maxHeight, 1.0f);
    EXPECT_LT(minAlpha, 0.5f);
    EXPECT_GT(maxAlpha, 0.9f);

    std::vector<std::pair<uint32, float>> expected;
    for (uint32 index = 0; index < simulation.Count(); ++index)
        expected.emplace_back(channels.SpawnIndices()[index], channels.Sizes()[index]);
    auto* playback = world.GetComponentForWrite<ParticlePlayback>(entity);
    playback->Paused = true;
    playback->SeekTime = 4;
    ++playback->Seek;
    for (int frame = 0; frame < 1000 && (frame == 0 || Emitter(system, entity).PreviewSeek.Pending); ++frame)
        system.Update(world, 0);
    const auto& preview = Emitter(system, entity);
    EXPECT_FALSE(preview.PreviewSeek.Pending);
    ASSERT_EQ(preview.Simulation.Count(), expected.size());
    for (uint32 index = 0; index < preview.Simulation.Count(); ++index)
    {
        EXPECT_EQ(preview.Simulation.Channels().SpawnIndices()[index], expected[index].first);
        EXPECT_FLOAT_EQ(preview.Simulation.Channels().Sizes()[index], expected[index].second);
    }
    world.GetComponentForWrite<ParticlePlayback>(entity)->Paused = false;
    world.GetComponentForWrite<ParticleEmitter3D>(entity)->Emitting = false;
    for (int tick = 0; tick < 420; ++tick)
        system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(system.State()->LiveParticles, 0u);
}

// A collision events buffer switched off through its ECS::ComponentDisabled tag reads as absent:
// the emitter's collisions are not written into it, and switching it back on collects them again.
TEST_F(ParticleSimulationSystemTest, SwitchedOffCollisionEventsBufferCollectsNothing)
{
    ECS::World world;
    // The plane sits above the emitter, so every particle touches it.
    const auto entity = AddEmitter(
        world, 12, 1.0f, R"(,{"type":"collision","id":3,"stage":"update","parameters":{"planeOffset":5}})");
    world.AddComponentImmediate(entity, ParticleCollisionEventsBuffer{});
    const auto bufferId = ECS::GetComponentTypeId<ParticleCollisionEventsBuffer>();
    ASSERT_TRUE(world.SetComponentEnabledImmediate(entity, bufferId, false));
    ParticleSimulationSystem system(NewState());
    for (int tick = 0; tick < 30; ++tick)
        system.Update(world, kFrame);
    EXPECT_EQ(world.GetComponent<ParticleCollisionEventsBuffer>(entity)->Count, 0u);

    ASSERT_TRUE(world.SetComponentEnabledImmediate(entity, bufferId, true));
    ++world.GetComponentForWrite<ParticlePlayback>(entity)->Restart;
    uint32 collected = 0;
    for (int tick = 0; tick < 30 && collected == 0; ++tick)
    {
        system.Update(world, kFrame);
        collected = world.GetComponent<ParticleCollisionEventsBuffer>(entity)->Count;
    }
    EXPECT_GT(collected, 0u);
}
