#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h" // Ensure template implementations are visible
#include "TestComponents.h"

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

TEST(ViewAliasSmokeTest, QueryIterates) {
    World w(nullptr);
    auto e = w.Create();
    e.Set(Position{1,2,3});
    e.Set(Velocity{4,5,6});
    w.ProcessCommands();

    size_t count = 0;
    w.Query<Read<Position>, Read<Velocity>>().Each([&](EntityHandle, const Position& p, const Velocity& v){
        ++count;
        EXPECT_EQ(p.x, 1);
        EXPECT_EQ(v.x, 4);
    });
    EXPECT_EQ(count, 1u);
}

