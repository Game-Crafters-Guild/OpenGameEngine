// A fence's span overrides follow the spline's authored points through the
// edits that renumber them — insert, delete and resample (fence design section
// 5) — and the whole edit undoes as one step with the points.

#include <gtest/gtest.h>

#include "Components/Spline/SplineFence.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Placement/FenceSpanOverrideRemap.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "Spline/SplineUtility.h"
#include "UndoRedo/SplinePointEdits.h"
#include "UndoRedo/UndoRedoService.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace GameEngine;
using Components::SplineFence;
using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;
using Editor::SplinePointRenumbering;

namespace
{

SplineSpanOverride Gate(uint32 point, uint16 ordinal)
{
    return {point, ordinal, SplineSpanOverrideKind::Gate, 0u};
}

} // namespace

// Inserting at k moves every point at or after k up, and the overrides with
// them; the ones before k stay.
TEST(FenceSpanOverrideRemap, AnInsertMovesTheOverridesAtOrAfterItUp)
{
    SplineFence fence;
    fence.Overrides[0] = Gate(0u, 1u);
    fence.Overrides[1] = Gate(2u, 0u);
    fence.Overrides[2] = Gate(3u, 4u);
    const SplinePointRenumbering inserted = SplinePointRenumbering::Inserted(5u, 2u, 1u, false);
    EXPECT_TRUE(Editor::RemapSpanOverrides(fence, inserted, {}));
    EXPECT_EQ(fence.Overrides[0], Gate(0u, 1u));
    EXPECT_EQ(fence.Overrides[1], Gate(3u, 0u));
    EXPECT_EQ(fence.Overrides[2], Gate(4u, 4u));

    // Prepending three points at the start moves them all.
    SplineFence prepended;
    prepended.Overrides[0] = Gate(0u, 2u);
    EXPECT_TRUE(Editor::RemapSpanOverrides(prepended,
                                           SplinePointRenumbering::Inserted(4u, 0u, 3u, false), {}));
    EXPECT_EQ(prepended.Overrides[0], Gate(3u, 2u));

    // Appending at the end renumbers nothing.
    SplineFence appended;
    appended.Overrides[0] = Gate(2u, 1u);
    EXPECT_FALSE(Editor::RemapSpanOverrides(appended,
                                            SplinePointRenumbering::Inserted(4u, 4u, 1u, false), {}));
    EXPECT_EQ(appended.Overrides[0], Gate(2u, 1u));
}

// Deleting an interior point merges its run into the run before it. An
// override on the removed point's run moves there and counts on past the spans
// that stood before it, so it names the same place along the wall; the ones
// before are untouched and the ones after move down.
TEST(FenceSpanOverrideRemap, ADeleteFoldsTheRemovedPointsRunIntoTheOneBeforeIt)
{
    SplineFence fence;
    fence.Overrides[0] = Gate(1u, 0u);
    fence.Overrides[1] = Gate(2u, 3u);
    fence.Overrides[2] = Gate(4u, 1u);
    const uint32 runSpanCounts[5] = {2u, 4u, 5u, 3u, 2u};
    const uint32 removed[1] = {2u};
    EXPECT_TRUE(Editor::RemapSpanOverrides(fence, SplinePointRenumbering::Removed(6u, removed, false),
                                           runSpanCounts));
    EXPECT_EQ(fence.Overrides[0], Gate(1u, 0u));
    EXPECT_EQ(fence.Overrides[1], Gate(1u, 4u + 3u)) << "the gate moved along the wall";
    EXPECT_EQ(fence.Overrides[2], Gate(3u, 1u));

    // Two consecutive points removed: the override counts past both runs before it.
    SplineFence twice;
    twice.Overrides[0] = Gate(3u, 1u);
    const uint32 removedTwo[2] = {2u, 3u};
    EXPECT_TRUE(Editor::RemapSpanOverrides(twice, SplinePointRenumbering::Removed(6u, removedTwo, false),
                                           runSpanCounts));
    EXPECT_EQ(twice.Overrides[0], Gate(1u, 1u + 4u + 5u));
}

// On an open spline the first run has nothing before it and the last run has
// nothing after it: deleting either end point takes that run away, and its
// overrides go with the wall they named.
TEST(FenceSpanOverrideRemap, DeletingAnOpenEndTakesItsRunsOverridesAway)
{
    SplineFence first;
    first.Overrides[0] = Gate(0u, 1u);
    first.Overrides[1] = Gate(1u, 0u);
    const uint32 removedFirst[1] = {0u};
    EXPECT_TRUE(Editor::RemapSpanOverrides(first, SplinePointRenumbering::Removed(4u, removedFirst, false), {}));
    EXPECT_EQ(first.Overrides[0], SplineSpanOverride{});
    EXPECT_EQ(first.Overrides[1], Gate(0u, 0u));

    SplineFence last;
    last.Overrides[0] = Gate(2u, 1u);
    last.Overrides[1] = Gate(1u, 0u);
    const uint32 removedLast[1] = {3u};
    EXPECT_TRUE(Editor::RemapSpanOverrides(last, SplinePointRenumbering::Removed(4u, removedLast, false), {}));
    EXPECT_EQ(last.Overrides[0], SplineSpanOverride{});
    EXPECT_EQ(last.Overrides[1], Gate(1u, 0u));
}

// A closed loop has no end: deleting its first point folds that run into the
// run that closes the loop.
TEST(FenceSpanOverrideRemap, DeletingTheFirstPointOfALoopFoldsItsRunIntoTheClosingRun)
{
    SplineFence loop;
    loop.Overrides[0] = Gate(0u, 2u);
    loop.Overrides[1] = Gate(3u, 1u);
    const uint32 runSpanCounts[5] = {3u, 3u, 3u, 3u, 6u};
    const uint32 removed[1] = {0u};
    EXPECT_TRUE(Editor::RemapSpanOverrides(loop, SplinePointRenumbering::Removed(5u, removed, true), runSpanCounts));
    EXPECT_EQ(loop.Overrides[0], Gate(3u, 2u + 6u))
        << "the closing run now opens at the old point 4 and its six spans stand first";
    EXPECT_EQ(loop.Overrides[1], Gate(2u, 1u));
}

// A resample maps every old point to the new point nearest it; each override
// lands on the nearest surviving point, which is the design's survival rule.
TEST(FenceSpanOverrideRemap, AResampleLandsEachOverrideOnTheNearestSurvivingPoint)
{
    SplineFence fence;
    fence.Overrides[0] = Gate(0u, 0u);
    fence.Overrides[1] = Gate(3u, 2u);
    fence.Overrides[2] = {5u, 1u, SplineSpanOverrideKind::ExplicitPiece, 1u};
    SplinePointRenumbering resampled;
    resampled.OldToNew = {0u, 1u, 1u, 2u, 3u, 3u, 4u};
    resampled.PointCountAfter = 5u;
    EXPECT_TRUE(Editor::RemapSpanOverrides(fence, resampled, {}));
    EXPECT_EQ(fence.Overrides[0], Gate(0u, 0u));
    EXPECT_EQ(fence.Overrides[1], Gate(2u, 2u));
    EXPECT_EQ(fence.Overrides[2], (SplineSpanOverride{3u, 1u, SplineSpanOverrideKind::ExplicitPiece, 1u}));
}

// An override that already named a point the spline did not have is not the
// edit's to fix: it is left as it was, and reported by the layout.
TEST(FenceSpanOverrideRemap, AnOverrideOnAPointTheSplineDidNotHaveIsLeftAlone)
{
    SplineFence fence;
    fence.Overrides[0] = Gate(9u, 0u);
    EXPECT_FALSE(Editor::RemapSpanOverrides(fence, SplinePointRenumbering::Inserted(4u, 0u, 1u, false), {}));
    EXPECT_EQ(fence.Overrides[0], Gate(9u, 0u));
}

// The subscriber writes the remapped recipe back through the edit's undo
// service, so one undo of the compound restores the overrides the edit moved.
TEST(FenceSpanOverrideRemap, ThePublishedEditRemapsTheFenceAndUndoesWithIt)
{
    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    SplineFence fence;
    fence.Overrides[0] = Gate(2u, 1u);
    world.AddComponentImmediate(entity, fence);

    Editor::UndoRedoService undo;
    const EventSubscription subscription =
        Editor::SplinePointEdited().Subscribe([](const Editor::SplinePointEdit& edit)
                                              { Editor::RemapFenceOverridesOnPointEdit(edit, {}); });
    const SplinePointRenumbering inserted = SplinePointRenumbering::Inserted(5u, 1u, 1u, false);
    undo.BeginCompound("Insert Spline Point");
    Editor::SplinePointEdited().Invoke({&world, entity, &undo, nullptr, &inserted});
    undo.EndCompound();

    ASSERT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_EQ(world.GetComponent<SplineFence>(entity)->Overrides[0], Gate(3u, 1u));
    undo.Undo();
    EXPECT_EQ(world.GetComponent<SplineFence>(entity)->Overrides[0], Gate(2u, 1u));
    undo.Redo();
    EXPECT_EQ(world.GetComponent<SplineFence>(entity)->Overrides[0], Gate(3u, 1u));
}

// The renumberings the spline tools publish.
TEST(SplinePointEdits, RemovedAndInsertedRenumberTheSurvivorsInOrder)
{
    const uint32 removed[2] = {4u, 1u};
    const SplinePointRenumbering r = SplinePointRenumbering::Removed(6u, removed, true);
    EXPECT_EQ(r.OldToNew, (std::vector<uint32>{0u, SplinePointRenumbering::kRemoved, 1u, 2u,
                                               SplinePointRenumbering::kRemoved, 3u}));
    EXPECT_EQ(r.PointCountAfter, 4u);
    EXPECT_TRUE(r.ClosedAfter);

    const SplinePointRenumbering i = SplinePointRenumbering::Inserted(3u, 1u, 2u, false);
    EXPECT_EQ(i.OldToNew, (std::vector<uint32>{0u, 3u, 4u}));
    EXPECT_EQ(i.PointCountAfter, 5u);
}

// The design's resample gate (section 5): resample an overridden spline the way the spline
// inspector does -- sample it, simplify the samples, rewrite the point list from the kept frames
// -- then map the old points to the new ones with the inspector's own mapping and re-address the
// overrides. Each override must land on the surviving point nearest the one it named.
TEST(FenceSpanOverrideRemap, AResampledSplineKeepsEachOverrideOnTheNearestSurvivingPoint)
{
    Spline::SplineData data;
    data.Type = Spline::SplineType::CatmullRom;
    const Mathematics::Vector3 authored[7] = {{-20, 0, 0}, {-14, 0, 9}, {-5, 0, 14}, {5, 0, 14},
                                              {14, 0, 9},  {20, 0, 0},  {22, 0, -8}};
    for (const Mathematics::Vector3& p : authored)
        data.AddPoint(p);
    Spline::RebuildSplineCache(data);

    std::vector<Spline::SplineFrame> frames;
    Spline::SampleUniform(data, 256u, frames);
    std::vector<Mathematics::Vector3> positions;
    for (const Spline::SplineFrame& frame : frames)
        positions.push_back(frame.Position);
    std::vector<uint32> kept;
    Spline::SimplifyPolylineIndices(positions, 3.0f, kept);
    std::vector<Spline::SplineFrame> keptFrames;
    for (const uint32 index : kept)
        keptFrames.push_back(frames[index]);
    data.ReplacePointsFromFrames(keptFrames);
    ASSERT_LT(data.Points.size(), 7u) << "the tolerance no longer thins the spline";
    ASSERT_GE(data.Points.size(), 3u);

    std::vector<Mathematics::Vector3> after;
    for (const Spline::SplineControlPoint& point : data.Points)
        after.push_back(point.Position);
    const SplinePointRenumbering renumbering =
        SplinePointRenumbering::Nearest(authored, after, data.IsEffectivelyClosed());
    ASSERT_EQ(renumbering.OldToNew.size(), 7u);
    EXPECT_EQ(renumbering.PointCountAfter, static_cast<uint32>(after.size()));

    SplineFence fence;
    fence.Overrides[0] = Gate(3u, 1u);
    fence.Overrides[1] = Gate(5u, 0u);
    Editor::RemapSpanOverrides(fence, renumbering, {});
    for (const auto& [slot, point] : {std::pair<int, uint32>{0, 3u}, {1, 5u}})
    {
        uint32 nearest = 0;
        float32 best = std::numeric_limits<float32>::max();
        for (uint32 k = 0; k < after.size(); ++k)
        {
            const Mathematics::Vector3 d = after[k] - authored[point];
            if (Mathematics::Vector3::Dot(d, d) < best)
            {
                best = Mathematics::Vector3::Dot(d, d);
                nearest = k;
            }
        }
        EXPECT_EQ(fence.Overrides[slot].PointIndex, nearest) << "override on point " << point;
        EXPECT_EQ(fence.Overrides[slot].Kind, SplineSpanOverrideKind::Gate);
    }
}
