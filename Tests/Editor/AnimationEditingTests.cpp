#include "Panels/Animation/TimelineTransport.h"
#include "Panels/Animation/CurveDisplayScope.h"
#include "Panels/Animation/CurveReduction.h"

#include <gtest/gtest.h>
#include <limits>

using namespace GameEngine;

TEST(AnimationTransport, RetainsOvershootAndMultipleLoopsInBothDirections)
{
    TimelineState state;
    state.rangeStart = 2.0f; state.rangeEnd = 3.0f;
    state.currentTime = 2.75f; state.playing = true;
    EXPECT_DOUBLE_EQ(AdvanceTimeline(state, 2.5), 2.5);
    EXPECT_FLOAT_EQ(state.currentTime, 2.25f);
    state.reverse = true;
    AdvanceTimeline(state, 3.5);
    EXPECT_FLOAT_EQ(state.currentTime, 2.75f);
    EXPECT_TRUE(state.playing);
}

TEST(AnimationTransport, NonLoopingPlaybackStopsAtEndpointsWithoutExtraScroll)
{
    TimelineState state;
    state.rangeStart = 2; state.rangeEnd = 3; state.currentTime = 2.75f;
    state.playing = true; state.loop = false;
    EXPECT_DOUBLE_EQ(AdvanceTimeline(state, 5), 0.25);
    EXPECT_FLOAT_EQ(state.currentTime, 3);
    EXPECT_FALSE(state.playing);
    state.playing = true; state.reverse = true;
    EXPECT_DOUBLE_EQ(AdvanceTimeline(state, 1), -1);
    EXPECT_FLOAT_EQ(state.currentTime, 2);
    EXPECT_FALSE(state.playing);
}

TEST(AnimationTransport, ExactLoopEndpointsRemainVisibleThenWrap)
{
    TimelineState state;
    state.rangeEnd = 1; state.currentTime = 0.75f; state.playing = true;
    AdvanceTimeline(state, 0.25);
    EXPECT_FLOAT_EQ(state.currentTime, 1);
    AdvanceTimeline(state, 0.25);
    EXPECT_FLOAT_EQ(state.currentTime, 0.25f);
    state.reverse = true;
    AdvanceTimeline(state, 0.25);
    EXPECT_FLOAT_EQ(state.currentTime, 0);
    AdvanceTimeline(state, 0.25);
    EXPECT_FLOAT_EQ(state.currentTime, 0.75f);
}

TEST(AnimationTransport, PauseInvalidDeltaAndEmptyRangeAreSafe)
{
    TimelineState state;
    state.playing = true; state.paused = true; state.currentTime = 4;
    EXPECT_DOUBLE_EQ(AdvanceTimeline(state, 1), 0);
    EXPECT_FLOAT_EQ(state.currentTime, 4);
    state.paused = false;
    AdvanceTimeline(state, std::numeric_limits<double>::quiet_NaN());
    EXPECT_FLOAT_EQ(state.currentTime, 4);
    state.rangeStart = state.rangeEnd = 2;
    AdvanceTimeline(state, 1);
    EXPECT_FLOAT_EQ(state.currentTime, 2);
    EXPECT_FALSE(state.playing);
}

TEST(AnimationCurveDisplay, PinsExtendDrawingWithoutChangingEditableChannels)
{
    const std::vector<int> editable{2};
    const std::vector<PinnedCurve> pins{{0, 1}, {0, 1}, {2, 0}};
    auto drawn = editable;
    drawn.reserve(8);
    const auto* storage = drawn.data();
    AppendPinnedChannels(drawn, pins);
    EXPECT_EQ(drawn, (std::vector<int>{2, 0}));
    EXPECT_EQ(drawn.data(), storage);
    EXPECT_EQ(editable, (std::vector<int>{2}));
    EXPECT_TRUE(IsCurvePinned(pins, 0, 1));
    EXPECT_FALSE(IsCurvePinned(pins, 0, 0));
    AppendPinnedChannels(drawn, pins);
    EXPECT_EQ(drawn, (std::vector<int>{2, 0}));
    EXPECT_EQ(drawn.data(), storage);
    drawn.assign(editable.begin(), editable.end());
    AppendPinnedChannels(drawn, {});
    EXPECT_EQ(drawn, editable);
}

namespace {
struct Key { float time; float value; };
float Sample(const std::vector<Key>& keys, float time)
{
    if (time <= keys.front().time) return keys.front().value;
    for (size_t i = 1; i < keys.size(); ++i)
        if (time <= keys[i].time)
        {
            const float fraction = (time - keys[i - 1].time) / (keys[i].time - keys[i - 1].time);
            return keys[i - 1].value + fraction * (keys[i].value - keys[i - 1].value);
        }
    return keys.back().value;
}
float Error(const std::vector<Key>& a, const std::vector<Key>& b, float time)
{
    return std::abs(Sample(a, time) - Sample(b, time));
}
}

TEST(AnimationCurveReduction, DensePeakCannotBypassValueTolerance)
{
    const std::vector<Key> keys{{0, 0}, {0.01f, 10}, {0.02f, 0}};
    const auto result = ReduceCurveKeys(keys, 0.1f, 0.001f, Error, [](auto&) { return true; });
    ASSERT_EQ(result.size(), 3u);
    EXPECT_FLOAT_EQ(result[1].value, 10);
}

TEST(AnimationCurveReduction, RemovesRedundantKeysWithoutRemovingEndpoints)
{
    const std::vector<Key> keys{{0, 0}, {0.25f, 0.25f}, {0.5f, 0.5f}, {1, 1}};
    const auto result = ReduceCurveKeys(keys, 0, 0.0001f, Error, [](auto&) { return true; });
    ASSERT_EQ(result.size(), 2u);
    EXPECT_FLOAT_EQ(result.front().time, 0);
    EXPECT_FLOAT_EQ(result.back().time, 1);
}

TEST(AnimationCurveReduction, HonorsSelectedKeysAndSpacingLimit)
{
    const std::vector<Key> keys{{0, 0}, {0.25f, 0.25f}, {0.5f, 0.5f}, {1, 1}};
    auto result = ReduceCurveKeys(keys, 0, 0.001f, Error, [](auto& key) { return key.time == 0.25f; });
    ASSERT_EQ(result.size(), 3u);
    EXPECT_FLOAT_EQ(result[1].time, 0.5f);
    EXPECT_EQ(ReduceCurveKeys(keys, 0.1f, 0.001f, Error, [](auto&) { return true; }).size(), keys.size());
}

TEST(AnimationCurveReduction, ChecksBetweenKeysNotJustMidpoints)
{
    const std::vector<Key> keys{{0, 0}, {0.5f, 0}, {1, 0}};
    const auto result = ReduceCurveKeys(keys, 0, 0.01f,
        [](const auto&, const auto&, float t) { return std::abs(t * (t - 0.25f) * (t - 0.5f) * 100.0f); },
        [](auto&) { return true; });
    EXPECT_EQ(result.size(), 3u);
}

// A drawn spline can bow between its keys, so error that exists only strictly
// inside an original segment still has to block the removal.
TEST(AnimationCurveReduction, SamplesInsideOriginalSegmentsNotOnlyAtTheirEnds)
{
    const std::vector<Key> keys{{0, 0}, {0.5f, 0}, {1, 0}};
    const auto result = ReduceCurveKeys(keys, 0, 0.01f,
        [](const auto&, const auto&, float time) { return time > 0.02f && time < 0.48f ? 5.0f : 0.0f; },
        [](auto&) { return true; });
    EXPECT_EQ(result.size(), 3u);
}

TEST(AnimationCurveReduction, RejectsNonFiniteErrorAndInvalidTimes)
{
    const std::vector<Key> keys{{0, 0}, {0.5f, 0}, {1, 0}};
    EXPECT_EQ(ReduceCurveKeys(keys, 0, 0.1f,
        [](const auto&, const auto&, float) { return std::numeric_limits<float>::quiet_NaN(); },
        [](auto&) { return true; }).size(), 3u);
    const std::vector<Key> duplicate{{0, 0}, {0, 1}, {1, 0}};
    EXPECT_EQ(ReduceCurveKeys(duplicate, 0, 100, Error, [](auto&) { return true; }).size(), 3u);
}

TEST(AnimationCurveReduction, LimitsErrorSamplingToTheAffectedLateSegments)
{
    std::vector<Key> keys;
    for (int i = 0; i < 200; ++i) keys.push_back({float(i), float(i)});
    int samples = 0;
    const auto result = ReduceCurveKeys(keys, 0, 0.001f,
        [&](const auto& original, const auto& candidate, float time) {
            ++samples;
            EXPECT_GE(time, 188.0f);
            EXPECT_LE(time, 192.0f);
            return Error(original, candidate, time);
        }, [](const Key& key) { return key.time == 190.0f; });
    EXPECT_EQ(result.size(), 199u);
    EXPECT_EQ(keys.size(), 200u);
    EXPECT_GT(samples, 0);
    EXPECT_LE(samples, 72);
    EXPECT_FLOAT_EQ(result[190].time, 191.0f);
}
