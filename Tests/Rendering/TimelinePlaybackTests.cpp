#include <gtest/gtest.h>

#include "Animation/AnimationTimeline.h"
#include "ECS/World.h"
#include "Engine/Rendering/TimelinePlayback.h"

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

TEST(TimelinePlaybackTests, DispatchesRegisteredMethodCallbacks)
{
    ClearTimelineMethodCallbacks();

    bool invoked = false;
    RegisterTimelineMethodCallback("OnTest", [&](ECS::World&, ECS::EntityHandle, const Animation::TimelineMethodEvent& event)
    {
        invoked = true;
        EXPECT_EQ(event.MethodName, "OnTest");
        EXPECT_EQ(event.Arguments, "payload");
    });

    ECS::World world;
    const ECS::EntityHandle root = world.Create().GetHandle();
    Animation::TimelineMethodEvent event;
    event.MethodName = "OnTest";
    event.Arguments = "payload";
    DispatchTimelineMethodEvents(world, root, {event});
    EXPECT_TRUE(invoked);

    ClearTimelineMethodCallbacks();
}
