#include "Animation/AnimationTimeline.h"

#include <cmath>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Animation;

TEST(AnimationTimeline, DiscreteValuesHoldUntilTheNextKey)
{
    Timeline timeline;
    TimelineTrack track;
    track.Type = TimelineTrackType::Property;
    track.UpdateMode = TimelineTrackUpdateMode::Discrete;
    TimelineValueKey first;
    first.Time = 0.0f;
    first.Value[0] = 2.0f;
    TimelineValueKey second;
    second.Time = 1.0f;
    second.Value[0] = 8.0f;
    TimelineValueKey third;
    third.Time = 2.0f;
    third.Value[0] = 14.0f;
    track.ValueKeys = {first, second, third};
    timeline.Tracks.push_back(track);
    for (const auto& [time, expected] : std::vector<std::pair<float, float>>{
             {0.0f, 2.0f}, {0.75f, 2.0f}, {0.999f, 2.0f},
             {1.0f, 8.0f}, {1.75f, 8.0f}, {2.0f, 14.0f}})
    {
        const auto result = EvaluateTimeline(timeline, time, time);
        ASSERT_EQ(result.ValueSamples.size(), 1u);
        EXPECT_FLOAT_EQ(result.ValueSamples.front().Value[0], expected) << time;
    }
    timeline.Tracks.front().UpdateMode = TimelineTrackUpdateMode::Continuous;
    timeline.Tracks.front().Interpolation = TimelineTrackInterpolation::Nearest;
    const auto nearest = EvaluateTimeline(timeline, 0.0f, 0.75f);
    ASSERT_EQ(nearest.ValueSamples.size(), 1u);
    EXPECT_FLOAT_EQ(nearest.ValueSamples.front().Value[0], 8.0f);
}

TEST(AnimationTimeline, InterpolatesPropertyTrack)
{
    const nlohmann::json doc = {
        {"schemaVersion", 1},
        {"assetType", "Timeline"},
        {"tracks", nlohmann::json::array({
            {
                {"type", "position3d"},
                {"name", "Move"},
                {"targetPath", "Root"},
                {"propertyPath", "Transform.position"},
                {"valueKeys", nlohmann::json::array({
                    {{"time", 0.0f}, {"value", nlohmann::json::array({0.0f, 0.0f, 0.0f})}, {"componentCount", 3u}},
                    {{"time", 1.0f}, {"value", nlohmann::json::array({10.0f, 2.0f, -4.0f})}, {"componentCount", 3u}}
                })}
            }
        })}
    };

    Timeline timeline;
    ASSERT_TRUE(LoadTimelineFromJson(doc, timeline));

    const auto result = EvaluateTimeline(timeline, 0.0f, 0.5f);
    ASSERT_EQ(result.ValueSamples.size(), 1u);
    EXPECT_EQ(result.ValueSamples[0].ComponentCount, 3u);
    EXPECT_FLOAT_EQ(result.ValueSamples[0].Value[0], 5.0f);
    EXPECT_FLOAT_EQ(result.ValueSamples[0].Value[1], 1.0f);
    EXPECT_FLOAT_EQ(result.ValueSamples[0].Value[2], -2.0f);
}

TEST(AnimationTimeline, BezierUsesTimeAndValueHandlesAndRoundTrips)
{
    Timeline timeline;
    TimelineTrack track;
    track.Type = TimelineTrackType::Bezier;
    TimelineValueKey left;
    left.Time = 2.0f;
    left.OutHandle[0] = 0.1f;
    left.OutHandle[1] = 2.0f;
    left.HandleMode = TimelineBezierHandleMode::Free;
    TimelineValueKey right;
    right.Time = 3.0f;
    right.Value[0] = 1.0f;
    right.InHandle[0] = -0.4f;
    right.InHandle[1] = 1.0f;
    right.HandleMode = TimelineBezierHandleMode::Free;
    track.ValueKeys = {left, right};
    timeline.Tracks.push_back(track);
    // At curve parameter u=0.5, x=2.3875 and y=1.625.
    // Evaluating y directly at normalized time would produce a different value.
    auto result = EvaluateTimeline(timeline, 2.0f, 2.3875f);
    ASSERT_EQ(result.ValueSamples.size(), 1u);
    EXPECT_NEAR(result.ValueSamples.front().Value[0], 1.625f, 0.00001f);
    const auto document = SaveTimelineToJson(timeline);
    Timeline loaded;
    ASSERT_TRUE(LoadTimelineFromJson(document, loaded));
    result = EvaluateTimeline(loaded, 2.0f, 2.3875f);
    ASSERT_EQ(result.ValueSamples.size(), 1u);
    EXPECT_NEAR(result.ValueSamples.front().Value[0], 1.625f, 0.00001f);

    for (auto& key : timeline.Tracks.front().ValueKeys)
        key.HandleMode = TimelineBezierHandleMode::Linear;
    result = EvaluateTimeline(timeline, 2.0f, 2.25f);
    ASSERT_EQ(result.ValueSamples.size(), 1u);
    EXPECT_NEAR(result.ValueSamples.front().Value[0], 0.25f, 0.00001f);
}

TEST(AnimationTimeline, BezierHandlesReachingOutsideTheSegmentStayTimeMonotonic)
{
    Timeline timeline;
    TimelineTrack track;
    track.Type = TimelineTrackType::Bezier;
    TimelineValueKey left;
    left.OutHandle[0] = 0.0f;
    left.OutHandle[1] = 0.0f;
    left.HandleMode = TimelineBezierHandleMode::Free;
    TimelineValueKey right;
    right.Time = 1.0f;
    right.Value[0] = 1.0f;
    // Reaches a whole second back past the left key. Clamped to the segment the
    // time control points are 0 and 0, so x(u) = u^3 and y(u) = 3u^2 - 2u^3;
    // x = 0.5 at u = 0.5^(1/3), where y = 0.8898816. Left unclamped the time
    // curve doubles back and the solve lands on a different, larger value.
    right.InHandle[0] = -2.0f;
    right.InHandle[1] = 0.0f;
    right.HandleMode = TimelineBezierHandleMode::Free;
    track.ValueKeys = {left, right};
    timeline.Tracks.push_back(track);
    const auto result = EvaluateTimeline(timeline, 0.0f, 0.5f);
    ASSERT_EQ(result.ValueSamples.size(), 1u);
    EXPECT_NEAR(result.ValueSamples.front().Value[0], 0.8898816f, 0.0001f);
}

TEST(AnimationTimeline, FiresMethodEventsAcrossForwardAndLoopRanges)
{
    const nlohmann::json doc = {
        {"schemaVersion", 1},
        {"tracks", nlohmann::json::array({
            {
                {"type", "method"},
                {"name", "Events"},
                {"methodKeys", nlohmann::json::array({
                    {{"time", 0.1f}, {"methodName", "OnLoop"}, {"arguments", "wrap"}},
                    {{"time", 0.4f}, {"methodName", "OnHit"}, {"arguments", "left"}}
                })}
            }
        })}
    };

    Timeline timeline;
    ASSERT_TRUE(LoadTimelineFromJson(doc, timeline));

    auto result = EvaluateTimeline(timeline, 0.0f, 0.35f);
    ASSERT_EQ(result.MethodEvents.size(), 1u);
    EXPECT_EQ(result.MethodEvents[0].MethodName, "OnLoop");

    TimelineEvaluateOptions options;
    options.Loop = true;
    options.DurationOverride = 0.5f;
    result = EvaluateTimeline(timeline, 0.45f, 0.15f, options);
    ASSERT_EQ(result.MethodEvents.size(), 1u);
    EXPECT_EQ(result.MethodEvents[0].MethodName, "OnLoop");
}

TEST(AnimationTimeline, SkipsVideoTracksWhenLoading)
{
    const nlohmann::json doc = {
        {"schemaVersion", 1},
        {"assetType", "Timeline"},
        {"tracks", nlohmann::json::array({
            {
                {"type", "video"},
                {"name", "Cutscene"},
                {"clips", nlohmann::json::array()}
            },
            {
                {"type", "position3d"},
                {"name", "Move"},
                {"targetPath", "Root"},
                {"valueKeys", nlohmann::json::array({
                    {{"time", 0.0f}, {"value", nlohmann::json::array({0.0f, 0.0f, 0.0f})}, {"componentCount", 3u}},
                    {{"time", 1.0f}, {"value", nlohmann::json::array({1.0f, 0.0f, 0.0f})}, {"componentCount", 3u}}
                })}
            }
        })}
    };

    Timeline timeline;
    ASSERT_TRUE(LoadTimelineFromJson(doc, timeline));
    ASSERT_EQ(timeline.Tracks.size(), 1u);
    EXPECT_EQ(timeline.Tracks[0].Type, TimelineTrackType::Position3D);
}

TEST(AnimationTimeline, SamplesClipLocalTimeAndFadeWeight)
{
    const auto clipGuid = GUID("01234567-89ab-cdef-0123-456789abcdef");
    const nlohmann::json doc = {
        {"schemaVersion", 1},
        {"tracks", nlohmann::json::array({
            {
                {"type", "animation"},
                {"name", "Base"},
                {"clips", nlohmann::json::array({
                    {
                        {"clipGuid", clipGuid.ToString()},
                        {"assetType", "Timeline"},
                        {"name", "Walk"},
                        {"offsetOnTimeline", 2.0f},
                        {"inTime", 0.25f},
                        {"outTime", 1.25f},
                        {"fadeInDuration", 0.5f},
                        {"fadeOutDuration", 0.25f}
                    }
                })}
            }
        })}
    };

    Timeline timeline;
    ASSERT_TRUE(LoadTimelineFromJson(doc, timeline));

    const auto result = EvaluateTimeline(timeline, 2.0f, 2.25f);
    ASSERT_EQ(result.ActiveClips.size(), 1u);
    EXPECT_EQ(result.ActiveClips[0].ClipGuid, clipGuid);
    EXPECT_EQ(result.ActiveClips[0].ClipAssetType, AssetType::Timeline);
    EXPECT_FLOAT_EQ(result.ActiveClips[0].LocalTime, 0.5f);
    EXPECT_FLOAT_EQ(result.ActiveClips[0].Weight, 0.5f);
}

namespace
{

nlohmann::json MakeQuaternionRotationTrack(const char* type)
{
    return {
        {"schemaVersion", 1},
        {"tracks", nlohmann::json::array({
            {
                {"type", type},
                {"name", "Spin"},
                {"targetPath", "Root"},
                {"propertyPath", "Transform.rotation"},
                {"valueKeys", nlohmann::json::array({
                    {{"time", 0.0f}, {"value", nlohmann::json::array({0.0f, 0.0f, 0.0f, 1.0f})}, {"componentCount", 4u}},
                    {{"time", 1.0f}, {"value", nlohmann::json::array({0.0f, 0.8660254f, 0.0f, 0.5f})}, {"componentCount", 4u}}
                })}
            }
        })}
    };
}

void ExpectSlerped120DegreeY(const Timeline& timeline)
{
    const auto result = EvaluateTimeline(timeline, 0.0f, 0.5f);
    ASSERT_EQ(result.ValueSamples.size(), 1u);
    const auto& sample = result.ValueSamples[0];
    ASSERT_EQ(sample.ComponentCount, 4u);

    const float magnitude = std::sqrt(sample.Value[0] * sample.Value[0] + sample.Value[1] * sample.Value[1] +
                                       sample.Value[2] * sample.Value[2] + sample.Value[3] * sample.Value[3]);
    EXPECT_NEAR(magnitude, 1.0f, 1e-5f);

    // Halfway between 0 and 120 degrees about +Y is exactly 60 degrees about +Y.
    EXPECT_NEAR(sample.Value[0], 0.0f, 1e-5f);
    EXPECT_NEAR(sample.Value[1], 0.5f, 1e-4f);
    EXPECT_NEAR(sample.Value[2], 0.0f, 1e-5f);
    EXPECT_NEAR(sample.Value[3], 0.8660254f, 1e-4f);
}

} // namespace

TEST(AnimationTimeline, SlerpsRotation3DTrackInsteadOfLerping)
{
    // Identity at t=0, a 120-degree rotation about +Y at t=1 (Value is quaternion x,y,z,w).
    // A raw per-component lerp at t=0.5 lands at magnitude ~0.866 (shrunk, not
    // unit-length); slerp lands exactly on the 60-degree rotation about +Y.
    Timeline timeline;
    ASSERT_TRUE(LoadTimelineFromJson(MakeQuaternionRotationTrack("rotation3d"), timeline));
    ExpectSlerped120DegreeY(timeline);
}

TEST(AnimationTimeline, SlerpsPropertyRotationTrackInsteadOfLerping)
{
    Timeline timeline;
    ASSERT_TRUE(LoadTimelineFromJson(MakeQuaternionRotationTrack("property"), timeline));
    ExpectSlerped120DegreeY(timeline);
}

TEST(AnimationTimeline, SlerpsBezierRotationTrackInsteadOfLerping)
{
    Timeline timeline;
    ASSERT_TRUE(LoadTimelineFromJson(MakeQuaternionRotationTrack("bezier"), timeline));
    ExpectSlerped120DegreeY(timeline);
}

TEST(AnimationTimeline, DiscreteQuaternionTracksHoldBeforeNextKey)
{
    for (const char* type : {"rotation3d", "property", "bezier"})
    {
        SCOPED_TRACE(type);
        Timeline timeline;
        ASSERT_TRUE(LoadTimelineFromJson(MakeQuaternionRotationTrack(type), timeline));
        timeline.Tracks[0].UpdateMode = TimelineTrackUpdateMode::Discrete;
        const auto held = EvaluateTimeline(timeline, 0.0f, 0.75f);
        ASSERT_EQ(held.ValueSamples.size(), 1u);
        EXPECT_FLOAT_EQ(held.ValueSamples[0].Value[1], 0.0f);
        EXPECT_FLOAT_EQ(held.ValueSamples[0].Value[3], 1.0f);
        const auto next = EvaluateTimeline(timeline, 0.75f, 1.0f);
        ASSERT_EQ(next.ValueSamples.size(), 1u);
        EXPECT_NEAR(next.ValueSamples[0].Value[1], 0.8660254f, 1e-5f);
        EXPECT_FLOAT_EQ(next.ValueSamples[0].Value[3], 0.5f);
    }
}

TEST(AnimationTimeline, LerpsThreeComponentEulerRotationTracks)
{
    for (const char* type : {"rotation3d", "property", "bezier"})
    {
        SCOPED_TRACE(type);
        auto doc = MakeQuaternionRotationTrack(type);
        auto& keys = doc["tracks"][0]["valueKeys"];
        keys[0]["value"] = nlohmann::json::array({10.0f, 20.0f, -30.0f});
        keys[1]["value"] = nlohmann::json::array({30.0f, 140.0f, 10.0f});
        for (auto& key : keys)
            key["componentCount"] = 3u;

        Timeline timeline;
        ASSERT_TRUE(LoadTimelineFromJson(doc, timeline));
        const auto result = EvaluateTimeline(timeline, 0.0f, 0.5f);
        ASSERT_EQ(result.ValueSamples.size(), 1u);
        const auto& sample = result.ValueSamples[0];
        ASSERT_EQ(sample.ComponentCount, 3u);
        EXPECT_FLOAT_EQ(sample.Value[0], 20.0f);
        EXPECT_FLOAT_EQ(sample.Value[1], 80.0f);
        EXPECT_FLOAT_EQ(sample.Value[2], -10.0f);
    }
}
