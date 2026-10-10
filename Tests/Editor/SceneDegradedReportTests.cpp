#include <gtest/gtest.h>

#include "DebugServer/SceneDegradedReport.h"

#include <optional>
#include <string>

using GameEngine::Editor::BuildSceneDegradedReport;
using GameEngine::Editor::SceneLoadDegraded;
using GameEngine::Scene::SceneLoadSkip;

namespace
{

SceneLoadSkip MakeSkip(const char* component, const char* field, bool preserved)
{
    SceneLoadSkip s{};
    s.entityId = "lamp";
    s.component = component;
    s.field = field;
    s.message = "unknown enum value";
    s.line = 5;
    s.preserved = preserved;
    return s;
}

SceneLoadDegraded MakeRecord(std::initializer_list<SceneLoadSkip> skips)
{
    SceneLoadDegraded d{};
    d.DocumentPath = "C:/scenes/degraded.scene";
    for (const SceneLoadSkip& s : skips)
        d.Census.skips.push_back(s);
    return d;
}

} // namespace

// A clean load says nothing at all — a degraded-scene object on a healthy scene would teach every
// reader to ignore the field.
TEST(SceneDegradedReport, CleanLoadReportsNull)
{
    EXPECT_TRUE(BuildSceneDegradedReport(std::nullopt, std::nullopt).is_null());
}

// The ordinary degraded case: both counts agree because nothing has retired yet.
TEST(SceneDegradedReport, FreshlyDegradedLoadReportsBothCountsAndTheRows)
{
    const SceneLoadDegraded atLoad = MakeRecord({MakeSkip("Light", "type", true)});
    const nlohmann::json r = BuildSceneDegradedReport(atLoad, atLoad);

    EXPECT_EQ(r["loadTimeCount"], 1u);
    EXPECT_EQ(r["outstandingCount"], 1u);
    EXPECT_EQ(r["droppedCount"], 0u);
    ASSERT_EQ(r["skips"].size(), 1u);
    EXPECT_EQ(r["skips"][0]["component"], "Light");
    EXPECT_EQ(r["skips"][0]["field"], "type");
    EXPECT_EQ(r["skips"][0]["preserved"], true);
}

// THE RECONCILIATION. Once the user has discarded every override there is nothing outstanding, but
// the load really was partial and the window title still says so. Going null here would have an
// agent read a healthy scene while a human reads a partial one — two readers of one editor, unable
// to agree. Presence follows the load-time record, exactly like the title.
TEST(SceneDegradedReport, StaysPresentWhenNothingIsOutstandingSoItCannotContradictTheTitle)
{
    const SceneLoadDegraded atLoad = MakeRecord({MakeSkip("Light", "type", true)});
    const nlohmann::json r = BuildSceneDegradedReport(atLoad, std::nullopt);

    ASSERT_FALSE(r.is_null())
        << "the load-time record still stands, so this must not call the scene healthy";
    EXPECT_EQ(r["loadTimeCount"], 1u) << "what the load could not apply does not change";
    EXPECT_EQ(r["outstandingCount"], 0u) << "and what a save must answer for is now nothing";
    EXPECT_TRUE(r["skips"].empty())
        << "a discarded override must never be listed as text a save will write back";
}

// The rows come from the OUTSTANDING record, not the load-time one, so a retired override drops out
// of the list even while the object itself stays present.
TEST(SceneDegradedReport, RowsComeFromTheOutstandingRecordNotTheLoadTimeOne)
{
    const SceneLoadDegraded atLoad =
        MakeRecord({MakeSkip("Light", "type", true), MakeSkip("SplineFence", "SpanGrade", true)});
    const SceneLoadDegraded outstanding = MakeRecord({MakeSkip("Light", "type", true)});

    const nlohmann::json r = BuildSceneDegradedReport(atLoad, outstanding);

    EXPECT_EQ(r["loadTimeCount"], 2u);
    EXPECT_EQ(r["outstandingCount"], 1u);
    ASSERT_EQ(r["skips"].size(), 1u);
    EXPECT_EQ(r["skips"][0]["component"], "Light")
        << "the retired row is gone from the list, not merely re-flagged";
}

// droppedCount is the never-preserved subset and is taken from the load-time record, where it is
// invariant: only preserved rows can retire, so a discard can never inflate what a save would lose.
TEST(SceneDegradedReport, DroppedCountTracksTheNeverPreservedRows)
{
    const SceneLoadDegraded atLoad =
        MakeRecord({MakeSkip("Light", "type", true), MakeSkip("Ghost", "field", false)});
    const SceneLoadDegraded outstanding = MakeRecord({MakeSkip("Ghost", "field", false)});

    const nlohmann::json r = BuildSceneDegradedReport(atLoad, outstanding);

    EXPECT_EQ(r["droppedCount"], 1u);
    EXPECT_EQ(r["loadTimeCount"], 2u);
    EXPECT_EQ(r["outstandingCount"], 1u);
    ASSERT_EQ(r["skips"].size(), 1u);
    EXPECT_EQ(r["skips"][0]["preserved"], false);
}
