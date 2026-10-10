#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/AutoRegistration.h"
#include "TestComponents.h"

#include "ECS/ECSTemplates.h" // Ensure template implementations are visible

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

TEST(ViewWithoutTest, ExcludesTemporaryComponents) {
    World w(nullptr);
    AutoComponentRegistrar<Temporary>::EnsureRegistered();

    auto a = w.Create(); a.Set(Position{1,2,3}); a.Set(Velocity{1,0,0});
    auto b = w.Create(); b.Set(Position{4,5,6}); b.Set(Velocity{0,1,0}); b.Set(Temporary{0.5f});
    auto c = w.Create(); c.Set(Position{7,8,9});
    w.ProcessCommands();

    size_t count = 0;
    w.Query<Read<Position>, Read<Velocity>>().Without<Temporary>().Each([&](EntityHandle, const Position&, const Velocity&){ ++count; });

    EXPECT_EQ(count, 1u);
}

