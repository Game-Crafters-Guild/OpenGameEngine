#include <gtest/gtest.h>

#include <vector>

#include <nlohmann/json.hpp>

#include "Editor/Settings/AxisLayoutSerializer.h"
#include "Types/StringId.h"
#include "UI/Controls/AxisModel.h"

using namespace GameEngine;
using namespace GameEngine::Editor;

namespace
{

TrackDef MakeTrack(StringId key, float size, float minSize, bool hidden = false)
{
    TrackDef t;
    t.Key     = key;
    t.Size    = size;
    t.MinSize = minSize;
    t.Hidden  = hidden;
    return t;
}

AxisModel MakeAxis()
{
    AxisModel axis(Axis::Horizontal);
    axis.Set({MakeTrack("name"_sid, 200.0f, 60.0f),
              MakeTrack("type"_sid, 120.0f, 80.0f, /*hidden=*/true)});
    return axis;
}

} // namespace

TEST(AxisLayoutSerializerTests, RoundTripPreservesSizesAndHidden)
{
    const AxisModel source = MakeAxis();
    const nlohmann::json saved = CaptureAxisLayout(source);

    // Apply onto a fresh model with default sizes/visibility.
    AxisModel target(Axis::Horizontal);
    target.Set({MakeTrack("name"_sid, 10.0f, 60.0f),
                MakeTrack("type"_sid, 10.0f, 80.0f, /*hidden=*/false)});
    ApplyAxisLayout(target, saved);

    EXPECT_FLOAT_EQ(target.SizeOf("name"_sid), 200.0f);
    EXPECT_FLOAT_EQ(target.SizeOf("type"_sid), 120.0f);
    EXPECT_FALSE(target.Find("name"_sid)->Hidden);
    EXPECT_TRUE(target.Find("type"_sid)->Hidden);
}

TEST(AxisLayoutSerializerTests, ClampsSizeOnLoad)
{
    AxisModel axis(Axis::Horizontal);
    axis.Set({MakeTrack("small"_sid, 200.0f, 60.0f),
              MakeTrack("huge"_sid, 200.0f, 60.0f)});

    nlohmann::json saved = nlohmann::json::object();
    saved[std::to_string("small"_sid)] = {{"size", 5.0f}, {"hidden", false}};
    saved[std::to_string("huge"_sid)]  = {{"size", 100000.0f}, {"hidden", false}};

    ApplyAxisLayout(axis, saved);

    // Below MinSize snaps to the track min; above the corrupt-prefs guard caps at 8192.
    EXPECT_FLOAT_EQ(axis.SizeOf("small"_sid), 60.0f);
    EXPECT_FLOAT_EQ(axis.SizeOf("huge"_sid), 8192.0f);
}

TEST(AxisLayoutSerializerTests, MissingKeyKeepsDefault)
{
    AxisModel axis(Axis::Horizontal);
    axis.Set({MakeTrack("present"_sid, 200.0f, 60.0f),
              MakeTrack("absent"_sid, 150.0f, 60.0f)});

    nlohmann::json saved = nlohmann::json::object();
    saved[std::to_string("present"_sid)] = {{"size", 300.0f}, {"hidden", true}};

    ApplyAxisLayout(axis, saved);

    EXPECT_FLOAT_EQ(axis.SizeOf("present"_sid), 300.0f);
    EXPECT_TRUE(axis.Find("present"_sid)->Hidden);

    // No entry for "absent" -> its loaded defaults are untouched.
    EXPECT_FLOAT_EQ(axis.SizeOf("absent"_sid), 150.0f);
    EXPECT_FALSE(axis.Find("absent"_sid)->Hidden);
}

TEST(AxisLayoutSerializerTests, SkipsFillAndNonResizableTracks)
{
    // A real resizable column, a non-resizable column, and the trailing fill track.
    TrackDef name = MakeTrack("name"_sid, 200.0f, 60.0f);

    TrackDef fixed = MakeTrack("fixed"_sid, 90.0f, 60.0f);
    fixed.Resizable = false;

    AxisModel axis(Axis::Horizontal);
    axis.Set({name, fixed, TrackDef::MakeFill("spacer"_sid)});

    // Capture only persists the resizable, non-fill column.
    const nlohmann::json saved = CaptureAxisLayout(axis);
    EXPECT_TRUE(saved.contains(std::to_string("name"_sid)));
    EXPECT_FALSE(saved.contains(std::to_string("fixed"_sid)));
    EXPECT_FALSE(saved.contains(std::to_string("spacer"_sid)));

    // Even a stale entry for a skipped track must not be applied back onto it.
    nlohmann::json stale = saved;
    stale[std::to_string("fixed"_sid)]  = {{"size", 400.0f}, {"hidden", true}};
    stale[std::to_string("spacer"_sid)] = {{"size", 400.0f}, {"hidden", true}};

    ApplyAxisLayout(axis, stale);
    EXPECT_FLOAT_EQ(axis.SizeOf("fixed"_sid), 90.0f);
    EXPECT_FALSE(axis.Find("fixed"_sid)->Hidden);
    EXPECT_FALSE(axis.Find("spacer"_sid)->Hidden);
}
