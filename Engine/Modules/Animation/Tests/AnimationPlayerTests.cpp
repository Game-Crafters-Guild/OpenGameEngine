#include "Animation/AnimationLibrary.h"
#include "Animation/AnimationPlayer.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Animation;

TEST(AnimationPlayer, PlaysLibraryEntryAndSeeks)
{
    AnimationLibrary library(GUID(), std::filesystem::path("test://library.animlib"));
    library.SetEntriesForTest({
        AnimationLibraryEntry{"Idle", GUID("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"), AssetType::Animation, 0.0f, true}
    });

    AnimationPlayer player;
    player.SetLibrary(&library);
    ASSERT_TRUE(player.Play("Idle"));
    player.Update(0.25f, 1.0f);
    EXPECT_FLOAT_EQ(player.GetCurrentTime(), 0.25f);
    player.Seek(0.5f);
    EXPECT_FLOAT_EQ(player.GetCurrentTime(), 0.5f);
}

TEST(AnimationPlayer, QueuesNextAnimationAtEnd)
{
    AnimationLibrary library(GUID(), std::filesystem::path("test://library.animlib"));
    library.SetEntriesForTest({
        AnimationLibraryEntry{"A", GUID("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"), AssetType::Animation, 0.0f, false},
        AnimationLibraryEntry{"B", GUID("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb"), AssetType::Animation, 0.0f, true}
    });

    AnimationPlayer player;
    player.SetLibrary(&library);
    ASSERT_TRUE(player.Play("A"));
    ASSERT_TRUE(player.Queue("B"));
    player.Update(1.1f, 1.0f);
    EXPECT_EQ(player.Current().Name, "B");
    EXPECT_TRUE(player.IsPlaying());
}
