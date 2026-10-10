#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h" // Ensure template implementations are visible
#include "TestComponents.h"

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

TEST(ViewOptionallyVisibleTest, OptionalArgPointerIsNullWhenMissing) {
    World w(nullptr);
    auto e1 = w.Create(); e1.Set(Position{1,2,3}); e1.Set(Velocity{1,1,1});
    auto e2 = w.Create(); e2.Set(Position{2,3,4}); e2.Set(Velocity{2,2,2}); e2.Set(Health{10,10});
    w.ProcessCommands();

    size_t nullCount = 0, nonNullCount = 0;
    w.Query<Read<Position>, Read<Velocity>, Optional<Health>>().Each([&](EntityHandle, const Position&, const Velocity&, const Health* h){
        if (h) ++nonNullCount; else ++nullCount;
    });
    EXPECT_EQ(nullCount, 1u);
    EXPECT_EQ(nonNullCount, 1u);
}

TEST(ViewOptionallyVisibleTest, OptionalArgPointerPresentWhenAvailable) {
    World w(nullptr);
    auto e1 = w.Create(); e1.Set(Position{1,2,3}); e1.Set(Velocity{1,1,1});
    auto e2 = w.Create(); e2.Set(Position{2,3,4}); e2.Set(Velocity{2,2,2}); e2.Set(Health{10,10});
    w.ProcessCommands();

    size_t withHealthPtr = 0;
    w.Query<Read<Position>, Read<Velocity>, Optional<Health>>().Each([&](EntityHandle, const Position&, const Velocity&, const Health* h){
        if (h) ++withHealthPtr;
    });
    EXPECT_EQ(withHealthPtr, 1u);
}

