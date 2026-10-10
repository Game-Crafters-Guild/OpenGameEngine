#include "Animation/TweenService.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Animation;

TEST(TweenService, UpdatesValuesAndCompletes)
{
    TweenService service;
    float32 value = 0.0f;
    bool completed = false;

    TweenRequest request;
    request.From = 2.0f;
    request.To = 6.0f;
    request.DurationSeconds = 1.0f;
    request.Easing = Math::TweenEasing::Linear;
    request.OnValue = [&](float32 v) { value = v; };
    request.OnComplete = [&]() { completed = true; };

    const TweenHandle handle = service.Add(std::move(request));
    service.Update(0.5f);
    EXPECT_FLOAT_EQ(value, 4.0f);
    EXPECT_TRUE(service.IsAlive(handle));

    service.Update(0.5f);
    EXPECT_FLOAT_EQ(value, 6.0f);
    EXPECT_TRUE(completed);
    EXPECT_FALSE(service.IsAlive(handle));
}

TEST(TweenService, PauseAndResumeHoldProgress)
{
    TweenService service;
    float32 value = 0.0f;

    TweenRequest request;
    request.To = 10.0f;
    request.DurationSeconds = 1.0f;
    request.Easing = Math::TweenEasing::Linear;
    request.OnValue = [&](float32 v) { value = v; };
    const TweenHandle handle = service.Add(std::move(request));

    service.Pause(handle);
    service.Update(0.5f);
    EXPECT_FLOAT_EQ(value, 0.0f);
    EXPECT_EQ(service.GetStatus(handle), TweenStatus::Paused);

    service.Resume(handle);
    service.Update(0.5f);
    EXPECT_FLOAT_EQ(value, 5.0f);
}
