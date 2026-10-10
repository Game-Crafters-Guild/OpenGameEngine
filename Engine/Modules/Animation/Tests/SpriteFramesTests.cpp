#include "Animation/SpriteFrames.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Animation;

TEST(SpriteFrames, LoadsAndSamplesFrames)
{
    const std::string text = R"json({
      "schemaVersion": 1,
      "assetType": "SpriteFrames",
      "animations": [{
        "name": "Run",
        "loop": true,
        "frames": [
          {"name": "A", "textureGuid": "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", "durationSeconds": 0.1},
          {"name": "B", "textureGuid": "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb", "durationSeconds": 0.1}
        ]
      }]
    })json";

    SpriteFrames frames(GUID(), std::filesystem::path("test://run.spriteframes"));
    Vector<uint8> data(text.begin(), text.end());
    ASSERT_TRUE(frames.LoadFromData(data));

    const SpriteFrame* frame = frames.Sample("Run", 0.15f);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->Name, "B");

    frame = frames.Sample("Run", 0.25f);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->Name, "A");
}
