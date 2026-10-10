#include "Animation/AnimationLibrary.h"

#include <gtest/gtest.h>

#include <string>

using namespace GameEngine;
using namespace GameEngine::Animation;

TEST(AnimationLibrary, LoadsNamedEntriesAndFindsByName)
{
    const std::string text = R"json({
      "schemaVersion": 1,
      "assetType": "AnimationLibrary",
      "animations": [
        {
          "name": "Idle",
          "assetGuid": "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
          "type": "Animation",
          "defaultBlendSeconds": 0.15,
          "loop": true
        },
        {
          "name": "Cinematic",
          "assetGuid": "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb",
          "type": "Timeline",
          "loop": false
        }
      ]
    })json";

    AnimationLibrary library(GUID(), std::filesystem::path("test://library.animlib"));
    Vector<uint8> data(text.begin(), text.end());
    ASSERT_TRUE(library.LoadFromData(data));

    const auto* idle = library.Find("Idle");
    ASSERT_NE(idle, nullptr);
    EXPECT_EQ(idle->AssetGuid, GUID("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"));
    EXPECT_EQ(idle->Type, AssetType::Animation);
    EXPECT_FLOAT_EQ(idle->DefaultBlendSeconds, 0.15f);
    EXPECT_TRUE(idle->Loop);

    const auto* cinematic = library.Find("Cinematic");
    ASSERT_NE(cinematic, nullptr);
    EXPECT_EQ(cinematic->Type, AssetType::Timeline);
    EXPECT_FALSE(cinematic->Loop);
    EXPECT_EQ(library.Find("Missing"), nullptr);
}

TEST(AnimationLibrary, SavesRoundTripData)
{
    AnimationLibrary library(GUID(), std::filesystem::path("test://library.animlib"));
    library.SetEntriesForTest({
        AnimationLibraryEntry{
            "Run",
            GUID("cccccccc-cccc-cccc-cccc-cccccccccccc"),
            AssetType::Animation,
            0.05f,
            true
        }
    });

    Vector<uint8> data;
    ASSERT_TRUE(library.SaveToData(data));

    AnimationLibrary loaded(GUID(), std::filesystem::path("test://loaded.animlib"));
    ASSERT_TRUE(loaded.LoadFromData(data));
    const auto* run = loaded.Find("Run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run->AssetGuid, GUID("cccccccc-cccc-cccc-cccc-cccccccccccc"));
    EXPECT_EQ(run->Type, AssetType::Animation);
    EXPECT_FLOAT_EQ(run->DefaultBlendSeconds, 0.05f);
}
