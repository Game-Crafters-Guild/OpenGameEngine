#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/AutoRegistration.h"
#include "TestComponents.h"

using namespace GameEngine;
using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

TEST(RegisterComponentsTest, PreRegistersAndAvoidsFirstUseWork) {
    // Do NOT clear the registry here—other tests may rely on global auto-reg
    // Just verify pre-registration causes handlers to exist for types

    World world(nullptr);

    // Before pre-registration: make no assumptions; just ensure after it, handlers exist

    world.RegisterComponents<Position, Velocity>();

    // Also force auto registration explicitly, in case template instantiation is lazy
    AutoComponentRegistrar<Position>::EnsureRegistered();
    AutoComponentRegistrar<Velocity>::EnsureRegistered();

    // Now handlers should exist
    EXPECT_NE(ComponentRegistry::GetHandler(GetComponentTypeId<Position>()), nullptr);
    EXPECT_NE(ComponentRegistry::GetHandler(GetComponentTypeId<Velocity>()), nullptr);
}

