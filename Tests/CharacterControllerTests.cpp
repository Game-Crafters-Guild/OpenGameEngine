#include <gtest/gtest.h>

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsEntityEventCallbacks.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Systems/CharacterControllerSystem.h"
#include "PhysicsECS/Systems/CharacterControllerWritebackSystem.h"
#include "PhysicsECS/Systems/PhysicsStepSystem.h"
#include "PhysicsECS/Systems/PhysicsWorldHooks.h"

#include <cmath>
#include <limits>
#include <vector>

namespace
{
using namespace GameEngine::Physics;

static PhysicsWorldSettings SmallDeterministicSettings()
{
    PhysicsWorldSettings settings{};
    settings.gravity = Vector3(0.0f, -9.81f, 0.0f);
    settings.numThreads = 1;
    settings.maxBodies = 1024;
    settings.maxBodyPairs = 1024;
    settings.maxContactConstraints = 1024;
    settings.tempAllocatorBytes = 16u * 1024u * 1024u;
    settings.fixedTimeStep = 1.0f / 60.0f;
    settings.maxSubSteps = 4;
    return settings;
}

struct ScopedPhysicsWorldService
{
    explicit ScopedPhysicsWorldService(const PhysicsWorldSettings& settings)
    {
        using namespace GameEngine::PhysicsECS;
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();
        PhysicsEntityEventCallbacks::ClearAll();
        PhysicsWorldService::Initialize(settings);
        PhysicsEntityEventCallbacks::NotifyWorldStateChanged();
    }

    ~ScopedPhysicsWorldService()
    {
        using namespace GameEngine::PhysicsECS;
        PhysicsEntityEventCallbacks::ClearAll();
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();
    }
};

static BodyHandle AddGroundPlane(PhysicsWorld& world)
{
    PlaneShapeDef plane{};
    plane.normal = Vector3(0.0f, 1.0f, 0.0f);
    plane.d = 0.0f;
    plane.halfExtent = 2000.0f;
    const ShapeHandle shape = world.CreateShape(plane);
    EXPECT_TRUE(shape.IsValid());
    BodySettings ground{};
    ground.shape = shape;
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    return world.CreateBody(ground);
}

static CharacterSettings StandingCapsule(const Vector3& position, CharacterMotor motor = CharacterMotor::Kinematic)
{
    CharacterSettings s{};
    s.motor = motor;
    s.position = position;
    s.radius = 0.4f;
    s.height = 1.8f;
    s.maxSlopeAngleDegrees = 45.0f;
    s.maxStepHeight = 0.4f;
    s.layer = (motor == CharacterMotor::Dynamic) ? Layers::Dynamic : Layers::Kinematic;
    return s;
}

static void StepSeconds(PhysicsWorld& world, float32 seconds)
{
    constexpr float32 dt = 1.0f / 60.0f;
    const int steps = static_cast<int>(seconds / dt);
    for (int i = 0; i < steps; ++i)
        world.Step(dt, 1);
}

} // namespace

TEST(CharacterController, KinematicLandsOnPlaneAndReportsGrounded)
{
    PhysicsWorld world(SmallDeterministicSettings());
    ASSERT_TRUE(AddGroundPlane(world).IsValid());

    const CharacterHandle ch = world.CreateCharacter(StandingCapsule(Vector3(0.0f, 3.0f, 0.0f)));
    ASSERT_TRUE(ch.IsValid());

    StepSeconds(world, 2.0f);

    const Transform t = world.GetCharacterTransform(ch);
    EXPECT_LT(t.position.y, 0.2f);
    EXPECT_GT(t.position.y, -0.05f);
    EXPECT_TRUE(world.IsCharacterGrounded(ch));
}

TEST(CharacterController, KinematicLandsOnHeightField)
{
    PhysicsWorld world(SmallDeterministicSettings());

    const uint32 N = 8;
    std::vector<float> samples(N * N, 10.0f);
    HeightFieldShapeDef hfDef;
    hfDef.Samples = samples;
    hfDef.SampleCount = N;
    hfDef.Offset = Vector3(-50.0f, 0.0f, -50.0f);
    hfDef.Scale = Vector3(100.0f / (N - 1), 1.0f, 100.0f / (N - 1));
    const ShapeHandle hfShape = world.CreateShape(hfDef);
    ASSERT_TRUE(hfShape.IsValid());

    BodySettings ground{};
    ground.shape = hfShape;
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    ASSERT_TRUE(world.CreateBody(ground).IsValid());
    world.OptimizeBroadphase();

    const CharacterHandle ch = world.CreateCharacter(StandingCapsule(Vector3(0.0f, 20.0f, 0.0f)));
    ASSERT_TRUE(ch.IsValid());
    StepSeconds(world, 3.0f);

    const Transform t = world.GetCharacterTransform(ch);
    EXPECT_GT(t.position.y, 9.5f) << "y=" << t.position.y;
    EXPECT_LT(t.position.y, 11.0f) << "y=" << t.position.y;
    EXPECT_TRUE(world.IsCharacterGrounded(ch));
}

TEST(CharacterController, SteepSlopeIsNotGrounded)
{
    PhysicsWorld world(SmallDeterministicSettings());

    const uint32 N = 8;
    std::vector<float> samples(N * N);
    const float tan60 = 1.7320508f;
    for (uint32 z = 0; z < N; ++z)
        for (uint32 x = 0; x < N; ++x)
            samples[z * N + x] = static_cast<float>(x) * tan60;

    HeightFieldShapeDef hfDef;
    hfDef.Samples = samples;
    hfDef.SampleCount = N;
    hfDef.Offset = Vector3(0.0f, 0.0f, 0.0f);
    hfDef.Scale = Vector3(1.0f, 1.0f, 1.0f);
    const ShapeHandle hfShape = world.CreateShape(hfDef);
    ASSERT_TRUE(hfShape.IsValid());
    BodySettings ground{};
    ground.shape = hfShape;
    ground.motionType = MotionType::Static;
    ground.layer = Layers::Static;
    ASSERT_TRUE(world.CreateBody(ground).IsValid());
    world.OptimizeBroadphase();

    // Surface at x=3 is y ≈ 5.2. Drop onto the ramp with a 45° walk limit.
    const CharacterHandle ch = world.CreateCharacter(StandingCapsule(Vector3(3.0f, 8.0f, 3.0f)));
    ASSERT_TRUE(ch.IsValid());
    StepSeconds(world, 2.0f);

    EXPECT_FALSE(world.IsCharacterGrounded(ch));
}

TEST(CharacterController, StepsUpOntoBox)
{
    PhysicsWorld world(SmallDeterministicSettings());
    ASSERT_TRUE(AddGroundPlane(world).IsValid());

    BoxShapeDef box{};
    box.halfExtents = Vector3(10.0f, 0.1f, 2.0f);
    const ShapeHandle boxShape = world.CreateShape(box);
    ASSERT_TRUE(boxShape.IsValid());
    BodySettings step{};
    step.shape = boxShape;
    step.motionType = MotionType::Static;
    step.layer = Layers::Static;
    // Platform x in [0.8, 20.8], top at y = 0.2 so a 3 s walk stays on it.
    step.position = Vector3(10.8f, 0.1f, 0.0f);
    ASSERT_TRUE(world.CreateBody(step).IsValid());

    CharacterSettings desc = StandingCapsule(Vector3(0.0f, 0.05f, 0.0f));
    desc.radius = 0.3f;
    desc.height = 1.2f;
    desc.maxStepHeight = 0.4f;
    const CharacterHandle ch = world.CreateCharacter(desc);
    ASSERT_TRUE(ch.IsValid());

    constexpr float32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 180; ++i)
    {
        Vector3 v = world.GetCharacterLinearVelocity(ch);
        v.x = 2.0f;
        v.z = 0.0f;
        world.SetCharacterLinearVelocity(ch, v);
        world.Step(dt, 1);
    }

    const Transform t = world.GetCharacterTransform(ch);
    EXPECT_GT(t.position.x, 1.0f) << "x=" << t.position.x;
    EXPECT_GT(t.position.y, 0.15f) << "y=" << t.position.y << " x=" << t.position.x;
}

TEST(CharacterController, ModeSwitchRecreates)
{
    PhysicsWorld world(SmallDeterministicSettings());
    ASSERT_TRUE(AddGroundPlane(world).IsValid());

    CharacterHandle ch = world.CreateCharacter(StandingCapsule(Vector3(0.0f, 2.0f, 0.0f), CharacterMotor::Kinematic));
    ASSERT_TRUE(ch.IsValid());
    StepSeconds(world, 0.5f);
    ASSERT_TRUE(world.IsCharacterValid(ch));

    world.DestroyCharacter(ch);
    EXPECT_FALSE(world.IsCharacterValid(ch));

    ch = world.CreateCharacter(StandingCapsule(Vector3(0.0f, 2.0f, 0.0f), CharacterMotor::Dynamic));
    ASSERT_TRUE(ch.IsValid());
    StepSeconds(world, 1.5f);
    EXPECT_TRUE(world.IsCharacterValid(ch));
    const Transform t = world.GetCharacterTransform(ch);
    EXPECT_LT(t.position.y, 2.0f);
}

TEST(CharacterController, InvalidCapsuleRejected)
{
    PhysicsWorld world(SmallDeterministicSettings());
    ASSERT_TRUE(world.CreateCharacter(StandingCapsule(Vector3(0.0f, 1.0f, 0.0f))).IsValid())
        << "a legal capsule must create, or this test cannot distinguish reject from a stub";

    CharacterSettings s = StandingCapsule(Vector3(0.0f, 1.0f, 0.0f));
    s.height = 0.5f;
    s.radius = 0.4f;
    EXPECT_FALSE(world.CreateCharacter(s).IsValid());
}

TEST(CharacterController, EcsWritebackAndPhysicsBodyRefusal)
{
    using GameEngine::Components::CharacterController;
    using GameEngine::Components::PhysicsBody;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = GameEngine::PhysicsECS::PhysicsWorldService::Get();
    ASSERT_TRUE(AddGroundPlane(pw).IsValid());

    GameEngine::ECS::World world;
    GameEngine::PhysicsECS::RegisterPhysicsWorldHooks(world);
    world.EnableComponentDirtyFeed(GameEngine::ECS::GetComponentTypeId<WorldTransform>());

    Transform pose = Transform::FromTRS(
        GameEngine::Mathematics::Vector3(0.0f, 3.0f, 0.0f),
        GameEngine::Mathematics::Quaternion::Identity(),
        GameEngine::Mathematics::Vector3(1.0f, 1.0f, 1.0f));
    WorldTransform wt{};
    for (int i = 0; i < 16; ++i)
        wt.matrix[i] = pose.matrix[i];

    auto e = world.CreateEntity();
    world.AddComponentImmediate<Transform>(e, pose);
    world.AddComponentImmediate<WorldTransform>(e, wt);
    world.AddComponentImmediate<CharacterController>(e, CharacterController{});

    GameEngine::PhysicsECS::CharacterControllerSystem init;
    GameEngine::PhysicsECS::PhysicsStepSystem step;
    GameEngine::PhysicsECS::CharacterControllerWritebackSystem writeback;

    constexpr float32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; ++i)
    {
        init.Update(world, dt);
        step.Update(world, dt);
        writeback.Update(world, dt);
    }

    const auto* cc = world.GetComponent<CharacterController>(e);
    ASSERT_NE(cc, nullptr);
    EXPECT_TRUE(cc->initialized);
    EXPECT_TRUE(pw.IsCharacterValid(cc->character));
    EXPECT_TRUE(cc->grounded);
    const auto* t = world.GetComponent<Transform>(e);
    ASSERT_NE(t, nullptr);
    EXPECT_LT(t->GetPosition().y, 0.2f);

    auto blocked = world.CreateEntity();
    world.AddComponentImmediate<Transform>(blocked, pose);
    world.AddComponentImmediate<WorldTransform>(blocked, wt);
    world.AddComponentImmediate<PhysicsBody>(blocked, PhysicsBody{});
    world.AddComponentImmediate<CharacterController>(blocked, CharacterController{});
    init.Update(world, dt);
    const auto* blockedCc = world.GetComponent<CharacterController>(blocked);
    ASSERT_NE(blockedCc, nullptr);
    EXPECT_FALSE(blockedCc->initialized);
    EXPECT_TRUE(blockedCc->refusedBecausePhysicsBody);
    EXPECT_FALSE(pw.IsCharacterValid(blockedCc->character));
}

// The character writeback follows the body writeback's rule: a character at rest is not re-composed
// once its pose is written, and an edited Transform still snaps back to the character.
TEST(CharacterController, WritebackSkipsARestingCharacterAndSnapsAnEditedTransformBack)
{
    using GameEngine::Components::CharacterController;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;
    using GameEngine::PhysicsECS::PhysicsWorldService;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = PhysicsWorldService::Get();

    // No step runs, so the character's backend pose never changes.
    CharacterController cc{};
    cc.character = pw.CreateCharacter(StandingCapsule(Vector3(2.0f, 3.0f, -1.0f)));
    ASSERT_TRUE(pw.IsCharacterValid(cc.character));
    cc.initialized = true;

    GameEngine::ECS::World world;
    auto e = world.CreateEntity();
    world.AddComponentImmediate<Transform>(e, Transform::FromTRS(GameEngine::Mathematics::Vector3(2.0f, 3.0f, -1.0f),
                                                                 GameEngine::Mathematics::Quaternion::Identity(),
                                                                 GameEngine::Mathematics::Vector3(1.5f, 2.5f, 0.75f)));
    world.AddComponentImmediate<WorldTransform>(e, WorldTransform{});
    world.AddComponentImmediate<CharacterController>(e, cc);

    GameEngine::PhysicsECS::CharacterControllerWritebackSystem writeback;
    constexpr float32 dt = 1.0f / 60.0f;
    writeback.Update(world, dt);
    EXPECT_EQ(PhysicsWorldService::GetLastCharactersComposed(), 1u);
    for (int frame = 0; frame < 8; ++frame)
    {
        writeback.Update(world, dt);
        EXPECT_EQ(PhysicsWorldService::GetLastCharactersComposed(), 0u)
            << "frame " << frame << ": the resting character was re-composed";
    }

    const float32 characterX = pw.GetCharacterTransform(cc.character).position.x;
    world.GetComponentForWrite<Transform>(e)->matrix[12] += 3.0f;
    writeback.Update(world, dt);
    EXPECT_EQ(PhysicsWorldService::GetLastCharactersComposed(), 1u);
    EXPECT_EQ(world.GetComponent<Transform>(e)->matrix[12], characterX) << "the edited Transform kept its edit";
    writeback.Update(world, dt);
    EXPECT_EQ(PhysicsWorldService::GetLastCharactersComposed(), 0u);
}

// A character controller switched off — its own tag or its entity — destroys its backend character
// on the next frame (GetDisabled<CharacterController>); switched back on, it is created again like a
// new one.
TEST(CharacterController, SwitchedOffControllerDestroysItsCharacterAndIsRecreatedWhenBackOn)
{
    using GameEngine::Components::CharacterController;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = GameEngine::PhysicsECS::PhysicsWorldService::Get();
    ASSERT_TRUE(AddGroundPlane(pw).IsValid());

    GameEngine::ECS::World world;
    GameEngine::PhysicsECS::RegisterPhysicsWorldHooks(world);
    auto e = world.CreateEntity();
    world.AddComponentImmediate<Transform>(e, Transform{});
    world.AddComponentImmediate<WorldTransform>(e, WorldTransform{});
    world.AddComponentImmediate<CharacterController>(e, CharacterController{});

    GameEngine::PhysicsECS::CharacterControllerSystem system;
    constexpr float32 dt = 1.0f / 60.0f;
    world.SwapLifecycleEvents();
    system.Update(world, dt);
    const auto created = world.GetComponent<CharacterController>(e)->character;
    ASSERT_TRUE(pw.IsCharacterValid(created));

    GameEngine::ECS::Entity(&world, e).SetEnabled<CharacterController>(false);
    world.SwapLifecycleEvents();
    system.Update(world, dt);
    EXPECT_FALSE(pw.IsCharacterValid(created)) << "a switched-off controller must leave the simulation";
    EXPECT_FALSE(world.GetComponent<CharacterController>(e)->initialized);

    GameEngine::ECS::Entity(&world, e).SetEnabled<CharacterController>(true);
    world.SwapLifecycleEvents();
    system.Update(world, dt);
    const auto recreated = world.GetComponent<CharacterController>(e)->character;
    EXPECT_TRUE(pw.IsCharacterValid(recreated));

    world.SetEntityEnabledImmediate(e, false);
    world.SwapLifecycleEvents();
    system.Update(world, dt);
    EXPECT_FALSE(pw.IsCharacterValid(recreated)) << "an inactive entity's character must leave the simulation";
}

TEST(CharacterController, AnimationDisplacementOverridesWishVelocity)
{
    using GameEngine::Components::CharacterController;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = GameEngine::PhysicsECS::PhysicsWorldService::Get();
    ASSERT_TRUE(AddGroundPlane(pw).IsValid());

    GameEngine::ECS::World world;
    GameEngine::PhysicsECS::RegisterPhysicsWorldHooks(world);
    world.EnableComponentDirtyFeed(GameEngine::ECS::GetComponentTypeId<WorldTransform>());

    Transform pose = Transform::FromTRS(
        GameEngine::Mathematics::Vector3(0.0f, 1.0f, 0.0f),
        GameEngine::Mathematics::Quaternion::Identity(),
        GameEngine::Mathematics::Vector3(1.0f, 1.0f, 1.0f));
    WorldTransform wt{};
    for (int i = 0; i < 16; ++i)
        wt.matrix[i] = pose.matrix[i];

    CharacterController cc{};
    cc.desiredVelocityX = 99.0f;
    cc.desiredVelocityZ = 0.0f;
    cc.hasAnimationDisplacement = true;
    cc.animationDisplacementX = 0.10f;
    cc.animationDisplacementZ = 0.0f;

    auto e = world.CreateEntity();
    world.AddComponentImmediate<Transform>(e, pose);
    world.AddComponentImmediate<WorldTransform>(e, wt);
    world.AddComponentImmediate<CharacterController>(e, cc);

    GameEngine::PhysicsECS::CharacterControllerSystem motor;
    constexpr float32 dt = 0.05f;
    motor.Update(world, dt);

    const auto* after = world.GetComponent<CharacterController>(e);
    ASSERT_NE(after, nullptr);
    ASSERT_TRUE(after->initialized);
    EXPECT_FALSE(after->hasAnimationDisplacement);
    const auto vel = pw.GetCharacterLinearVelocity(after->character);
    EXPECT_NEAR(vel.x, 2.0f, 1e-3f);
    EXPECT_NEAR(vel.z, 0.0f, 1e-3f);
}

TEST(CharacterController, NonFiniteDisplacementDoesNotPoisonVelocity)
{
    using GameEngine::Components::CharacterController;
    using GameEngine::Components::Transform;
    using GameEngine::Components::WorldTransform;

    ScopedPhysicsWorldService svc(SmallDeterministicSettings());
    auto& pw = GameEngine::PhysicsECS::PhysicsWorldService::Get();
    ASSERT_TRUE(AddGroundPlane(pw).IsValid());

    GameEngine::ECS::World world;
    GameEngine::PhysicsECS::RegisterPhysicsWorldHooks(world);
    world.EnableComponentDirtyFeed(GameEngine::ECS::GetComponentTypeId<WorldTransform>());

    Transform pose = Transform::FromTRS(
        GameEngine::Mathematics::Vector3(0.0f, 1.0f, 0.0f),
        GameEngine::Mathematics::Quaternion::Identity(),
        GameEngine::Mathematics::Vector3(1.0f, 1.0f, 1.0f));
    WorldTransform wt{};
    for (int i = 0; i < 16; ++i)
        wt.matrix[i] = pose.matrix[i];

    CharacterController cc{};
    cc.desiredVelocityX = 3.0f;
    cc.hasAnimationDisplacement = true;
    cc.animationDisplacementX = std::numeric_limits<float>::quiet_NaN();
    cc.animationDisplacementZ = 0.0f;

    auto e = world.CreateEntity();
    world.AddComponentImmediate<Transform>(e, pose);
    world.AddComponentImmediate<WorldTransform>(e, wt);
    world.AddComponentImmediate<CharacterController>(e, cc);

    GameEngine::PhysicsECS::CharacterControllerSystem motor;
    motor.Update(world, 0.05f);

    const auto* after = world.GetComponent<CharacterController>(e);
    ASSERT_NE(after, nullptr);
    ASSERT_TRUE(after->initialized);
    const auto vel = pw.GetCharacterLinearVelocity(after->character);
    EXPECT_TRUE(std::isfinite(vel.x));
    EXPECT_NEAR(vel.x, 0.0f, 1e-3f);
}
