#include <gtest/gtest.h>

#include "Assets/FbxLoaderOptions.h"

using GameEngine::FbxLoaderOptions;
using GameEngine::HashFbxLoaderOptions;

TEST(FbxLoaderOptions, EffectiveBakeZerosDisabledAxes)
{
    FbxLoaderOptions opts{};
    opts.Axis.BakeRotationDeg[0] = 90.0f;
    opts.Axis.BakeRotationDeg[1] = 180.0f;
    opts.Axis.BakeRotationDeg[2] = -45.0f;
    opts.Axis.BakeRotationAxisEnabled[1] = true;

    float bake[3] = {1.0f, 1.0f, 1.0f};
    opts.Axis.EffectiveBakeRotationDeg(bake);
    EXPECT_FLOAT_EQ(bake[0], 0.0f);
    EXPECT_FLOAT_EQ(bake[1], 180.0f);
    EXPECT_FLOAT_EQ(bake[2], 0.0f);
    EXPECT_TRUE(opts.Axis.AnyBakeRotationEnabled());
}

TEST(FbxLoaderOptions, DisabledStoredDegreesDoNotChangeHash)
{
    FbxLoaderOptions identity{};
    FbxLoaderOptions stored{};
    stored.Axis.BakeRotationDeg[1] = 180.0f;
    EXPECT_EQ(HashFbxLoaderOptions(identity), HashFbxLoaderOptions(stored));
}

TEST(FbxLoaderOptions, EnablingAnAxisChangesHash)
{
    FbxLoaderOptions off{};
    off.Axis.BakeRotationDeg[1] = 180.0f;
    FbxLoaderOptions on = off;
    on.Axis.BakeRotationAxisEnabled[1] = true;
    EXPECT_NE(HashFbxLoaderOptions(off), HashFbxLoaderOptions(on));
}

TEST(FbxLoaderOptions, DefaultMirrorIsXOnly)
{
    FbxLoaderOptions opts{};
    EXPECT_TRUE(opts.Axis.MirrorAxis[0]);
    EXPECT_FALSE(opts.Axis.MirrorAxis[1]);
    EXPECT_FALSE(opts.Axis.MirrorAxis[2]);
}

TEST(FbxLoaderOptions, ClearingMirrorXChangesHash)
{
    FbxLoaderOptions def{};
    FbxLoaderOptions off = def;
    off.Axis.MirrorAxis[0] = false;
    EXPECT_NE(HashFbxLoaderOptions(def), HashFbxLoaderOptions(off));
}
