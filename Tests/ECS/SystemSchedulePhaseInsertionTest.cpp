// Late-registered systems (package DLLs loaded at project open / player init)
// must run at their declared phase/order/dependencies — not as trailing waves
// after the whole built schedule. Arc 3a seam 1: SystemScheduleBuilder is the
// persistent schedule model; a repeat BuildAndRegisterWithWaves re-solves the
// full graph, instantiates only the new registrations, and places a late
// system exactly where a from-scratch build would.

#include <gtest/gtest.h>

#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "DeathTestChild.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{
class RecordingSystem : public ISystem
{
public:
    RecordingSystem(std::vector<std::string>* log, const char* id) : m_Log(log), m_Id(id) {}
    void Update(World&, float32) override { m_Log->push_back(m_Id); }
    const char* GetName() const override { return m_Id.c_str(); }

private:
    std::vector<std::string>* m_Log;
    std::string m_Id;
};

size_t IndexOf(const std::vector<std::string>& log, const std::string& id)
{
    for (size_t i = 0; i < log.size(); ++i)
        if (log[i] == id)
            return i;
    return static_cast<size_t>(-1);
}

int64_t Count(const std::vector<std::string>& log, const std::string& id)
{
    return std::count(log.begin(), log.end(), id);
}

// Mirrors the real startup schedule's shape: two independent roots (an
// Extraction-phase hierarchy and a Camera-phase camera, both wave 0) and a
// Render-phase consumer depending on both, which lands in a later wave.
void AddBaseSchedule(SystemScheduleBuilder& b, std::vector<std::string>* log)
{
    b.Add<RecordingSystem>("Hierarchy", SystemPhase::Extraction, 1, {}, log, "Hierarchy");
    b.Add<RecordingSystem>("Camera", SystemPhase::Camera, 0, {}, log, "Camera");
    b.Add<RecordingSystem>("RenderGraphBuild", SystemPhase::Render, 0, {"Hierarchy", "Camera"},
                           log, "RenderGraphBuild");
}
} // namespace

// A system registered after the first build must land exactly where a
// from-scratch build with all systems known would place it, and must run
// before the downstream Render-phase consumer (the trailing-wave path would
// run it after — one-frame-late output).
TEST(SystemSchedulePhaseInsertion, LateSystemMatchesFromScratchPlacement)
{
    World world(nullptr);

    SystemManager incremental; // no job system → waves run inline, order is observable
    std::vector<std::string> incLog;
    SystemScheduleBuilder builder;
    AddBaseSchedule(builder, &incLog);
    builder.BuildAndRegisterWithWaves(incremental);

    // Package registers an extraction system after the schedule was built.
    // Dependency-free on purpose: this pins where the levelizer puts a
    // late-registered root, which is the placement question under test.
    builder.Add<RecordingSystem>("PkgExtract", SystemPhase::Extraction, 2, {}, &incLog, "PkgExtract");
    builder.BuildAndRegisterWithWaves(incremental);
    incremental.Update(world, 0.016f);

    SystemManager fromScratch;
    std::vector<std::string> refLog;
    SystemScheduleBuilder reference;
    AddBaseSchedule(reference, &refLog);
    reference.Add<RecordingSystem>("PkgExtract", SystemPhase::Extraction, 2, {}, &refLog, "PkgExtract");
    reference.BuildAndRegisterWithWaves(fromScratch);
    fromScratch.Update(world, 0.016f);

    ASSERT_EQ(incLog.size(), 4u);
    EXPECT_EQ(incLog, refLog) << "late-integrated placement differs from a from-scratch build";
    EXPECT_LT(IndexOf(incLog, "PkgExtract"), IndexOf(incLog, "RenderGraphBuild"))
        << "late extraction system ran after its downstream Render-phase consumer";
}

// A late system's declared dependency on an existing system is honored: it
// runs after that system and before the Render-phase consumer.
TEST(SystemSchedulePhaseInsertion, LateSystemHonorsDependencyOnExistingSystem)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddBaseSchedule(builder, &log);
    builder.BuildAndRegisterWithWaves(sm);

    builder.Add<RecordingSystem>("PkgDeform", SystemPhase::Extraction, 3, {"Hierarchy"}, &log, "PkgDeform");
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    ASSERT_EQ(log.size(), 4u);
    EXPECT_LT(IndexOf(log, "Hierarchy"), IndexOf(log, "PkgDeform"));
    EXPECT_LT(IndexOf(log, "PkgDeform"), IndexOf(log, "RenderGraphBuild"));
}

// Duplicate names on a re-solve: first registration wins, the late duplicate
// is never instantiated and the schedule keeps running the original.
TEST(SystemSchedulePhaseInsertion, LateDuplicateNameSkippedOnResolve)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddBaseSchedule(builder, &log);
    builder.BuildAndRegisterWithWaves(sm);

    builder.Add<RecordingSystem>("Hierarchy", SystemPhase::Render, 5, {}, &log, "HierarchyDup");
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "Hierarchy"), 1);
    EXPECT_EQ(Count(log, "HierarchyDup"), 0) << "late duplicate name was instantiated";

    // The duplicate stays dropped on subsequent re-solves.
    builder.BuildAndRegisterWithWaves(sm);
    log.clear();
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "Hierarchy"), 1);
    EXPECT_EQ(Count(log, "HierarchyDup"), 0);
}

// Arc-2 interplay: a phase-less direct AddSystem (managed-bridge style) after
// the plan was built runs as a trailing wave; a later schedule re-solve must
// keep it ticking exactly once (SetExecutionPlan's re-append guarantee) while
// the builder-known systems get their solved placement.
TEST(SystemSchedulePhaseInsertion, PhaselessDirectAddSurvivesResolve)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddBaseSchedule(builder, &log);
    builder.BuildAndRegisterWithWaves(sm);

    sm.AddSystem<RecordingSystem>(&log, "Bridge"); // trailing wave, no schedule info

    builder.Add<RecordingSystem>("PkgExtract", SystemPhase::Extraction, 2, {}, &log, "PkgExtract");
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    ASSERT_EQ(log.size(), 5u);
    EXPECT_EQ(Count(log, "Bridge"), 1) << "phase-less late system dropped by schedule re-solve";
    EXPECT_EQ(Count(log, "PkgExtract"), 1);
    EXPECT_LT(IndexOf(log, "PkgExtract"), IndexOf(log, "RenderGraphBuild"));
    EXPECT_EQ(log.back(), "Bridge") << "phase-less system should stay a trailing wave";
}

// An optional dependency that is unresolved at startup (a built-in declaring a
// dep on a package system name) resolves once the package registers: the edge
// is honored on the re-solve.
TEST(SystemSchedulePhaseInsertion, OptionalDependencyResolvesWhenPackageRegisters)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    builder.Add<RecordingSystem>("Consumer", SystemPhase::Extraction, 1,
                                 {OptionalDependency("PkgProducer")}, &log, "Consumer");
    builder.Add<RecordingSystem>("Downstream", SystemPhase::Render, 0, {"Consumer"}, &log, "Downstream");
    builder.BuildAndRegisterWithWaves(sm); // "PkgProducer" absent → edge inactive, no error

    sm.Update(world, 0.016f);
    ASSERT_EQ(log.size(), 2u);
    log.clear();

    builder.Add<RecordingSystem>("PkgProducer", SystemPhase::Extraction, 0, {}, &log, "PkgProducer");
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    ASSERT_EQ(log.size(), 3u);
    EXPECT_LT(IndexOf(log, "PkgProducer"), IndexOf(log, "Consumer"))
        << "dependency edge did not resolve on re-solve";
    EXPECT_LT(IndexOf(log, "Consumer"), IndexOf(log, "Downstream"));
}

// A cycle confined to late registrations must not disturb the running
// schedule: existing systems keep ticking, the cycle members never run.
TEST(SystemSchedulePhaseInsertion, LateCycleKeepsExistingSystemsTicking)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddBaseSchedule(builder, &log);
    builder.BuildAndRegisterWithWaves(sm);

    builder.Add<RecordingSystem>("PkgX", SystemPhase::Extraction, 2, {"PkgY"}, &log, "PkgX");
    builder.Add<RecordingSystem>("PkgY", SystemPhase::Extraction, 3, {"PkgX"}, &log, "PkgY");
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "Hierarchy"), 1);
    EXPECT_EQ(Count(log, "Camera"), 1);
    EXPECT_EQ(Count(log, "RenderGraphBuild"), 1);
    EXPECT_EQ(Count(log, "PkgX"), 0);
    EXPECT_EQ(Count(log, "PkgY"), 0);
}

// A late registration that forms a cycle THROUGH an already-instantiated
// system (via a previously unresolved optional dep) must never make that
// system silently stop ticking — it survives as a trailing wave until the
// cycle is fixed.
TEST(SystemSchedulePhaseInsertion, LateCycleThroughInstantiatedSystemKeepsItTicking)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    builder.Add<RecordingSystem>("A", SystemPhase::Extraction, 1,
                                 {OptionalDependency("PkgX")}, &log, "A"); // unresolved at first build
    builder.Add<RecordingSystem>("B", SystemPhase::Render, 0, {}, &log, "B");
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    ASSERT_EQ(log.size(), 2u);
    log.clear();

    builder.Add<RecordingSystem>("PkgX", SystemPhase::Extraction, 0, {"A"}, &log, "PkgX"); // A ↔ PkgX
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "A"), 1) << "instantiated system silently stopped ticking after a late cycle";
    EXPECT_EQ(Count(log, "B"), 1);
    EXPECT_EQ(Count(log, "PkgX"), 0);
}

// ---------------------------------------------------------------------------
// Unresolved dependency names fail closed.
//
// A declared dependency name is the only thing separating "runs after" from
// "runs concurrently on another worker thread", so a name matching no
// registered system is a silent data race, not a scheduling nicety. These pin
// the guard: a required name is fatal in dev builds, the two declared
// exceptions (OptionalDependency, ScheduleCompleteness::Partial) are not, and a
// schedule whose names all resolve is left alone.
// ---------------------------------------------------------------------------

namespace
{
// A schedule with a MISTYPED dependency: "Hierarchy" where the registered
// system is "TransformHierarchy". This is PR #868's defect shape — the
// misspelling left the dependent system free to share a wave with the system
// it had to follow.
void AddScheduleWithTypoedDependency(SystemScheduleBuilder& b, std::vector<std::string>* log)
{
    b.Add<RecordingSystem>("TransformHierarchy", SystemPhase::Extraction, 1, {}, log, "TransformHierarchy");
    b.Add<RecordingSystem>("Extraction", SystemPhase::Extraction, 2, {"Hierarchy"}, log, "Extraction");
}

#if !defined(NDEBUG)
// A death-test child starts with a fresh, uninitialized logger (the effective
// level is Off until Initialize), so the scheduler's error would reach no sink
// at all. Route it to stderr, which is what gtest matches against.
void RouteLogsToStderrAndSuppressDialogs()
{
    test::SuppressCrtDialogsInDeathTestChild();

    Logger::Log::Initialize({Logger::LogLevel::Debug, false});
    auto sink = std::make_unique<Logger::CallbackSink>();
    sink->RegisterCallback([](const Logger::LogMessage& message) {
        std::fputs(message.Message.c_str(), stderr);
        std::fputc('\n', stderr);
    });
    Logger::Log::AddSink(std::move(sink));
}
#endif
} // namespace

#if !defined(NDEBUG)
// Dev builds (Debug, DebugFast): the build asserts. Matching the assert's own
// text proves the assert fired rather than the process dying some other way —
// an error log alone would not abort.
TEST(SystemScheduleUnresolvedDependency, RequiredNameAssertsInDevBuilds)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            RouteLogsToStderrAndSuppressDialogs();
            SystemManager sm;
            std::vector<std::string> log;
            SystemScheduleBuilder builder;
            AddScheduleWithTypoedDependency(builder, &log);
            builder.BuildAndRegisterWithWaves(sm);
        },
        "Unresolved required system dependency");
}

// The error must survive the abort and state the consequence in plain terms:
// the flush before the assert is what gets it to the sinks at all.
TEST(SystemScheduleUnresolvedDependency, ErrorStatesTheConcurrencyConsequence)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            RouteLogsToStderrAndSuppressDialogs();
            SystemManager sm;
            std::vector<std::string> log;
            SystemScheduleBuilder builder;
            AddScheduleWithTypoedDependency(builder, &log);
            builder.BuildAndRegisterWithWaves(sm);
        },
        "may now run CONCURRENTLY");
}

// The near-miss hint is the whole point for a typo: it turns the error into a
// one-line fix instead of a scheduler read.
TEST(SystemScheduleUnresolvedDependency, ErrorSuggestsTheNearMissName)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            RouteLogsToStderrAndSuppressDialogs();
            SystemManager sm;
            std::vector<std::string> log;
            SystemScheduleBuilder builder;
            AddScheduleWithTypoedDependency(builder, &log);
            builder.BuildAndRegisterWithWaves(sm);
        },
        "Did you mean 'TransformHierarchy'");
}
#else
// Shipping configs compile the assert out: the schedule must still build (never
// take the process down) while the edge is reported and dropped.
TEST(SystemScheduleUnresolvedDependency, RequiredNameSurvivesWithoutAssertInShippingBuilds)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddScheduleWithTypoedDependency(builder, &log);
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "Extraction"), 1);
    EXPECT_EQ(Count(log, "TransformHierarchy"), 1);
}
#endif

// An optional dependency whose name is absent must not fail the build. This
// runs UNGUARDED in dev builds, so a spurious assert would abort the suite —
// which is exactly the regression it is here to catch.
TEST(SystemScheduleUnresolvedDependency, OptionalNameDoesNotFailTheBuild)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    builder.Add<RecordingSystem>("Solo", SystemPhase::Extraction, 1,
                                 {OptionalDependency("NeverRegistered")}, &log, "Solo");
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "Solo"), 1) << "optional unresolved dependency must not drop the system";
}

// An optional edge is a real edge whenever the name IS present — the marker
// must not quietly downgrade the ordering it declares.
TEST(SystemScheduleUnresolvedDependency, OptionalEdgeIsHonoredOnceTheNameResolves)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    builder.Add<RecordingSystem>("Dependent", SystemPhase::Extraction, 1,
                                 {OptionalDependency("LateProvider")}, &log, "Dependent");
    builder.Add<RecordingSystem>("LateProvider", SystemPhase::Extraction, 9, {}, &log, "LateProvider");
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    // Order alone would place Dependent (order 1) before LateProvider (order 9);
    // only the honored edge can invert them.
    EXPECT_LT(IndexOf(log, "LateProvider"), IndexOf(log, "Dependent"))
        << "optional dependency did not order the systems once its name resolved";
}

// A deliberately partial schedule (module-level fixtures asserting one edge in
// isolation) tolerates required names it cannot resolve.
TEST(SystemScheduleUnresolvedDependency, PartialScheduleToleratesUnresolvedRequiredNames)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddScheduleWithTypoedDependency(builder, &log);
    builder.BuildAndRegisterWithWaves(sm, ScheduleCompleteness::Partial);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "Extraction"), 1);
    EXPECT_EQ(Count(log, "TransformHierarchy"), 1);
}

// Positive control: a schedule whose names all resolve must build clean and
// keep its ordering. Without this, every test above would still pass if the
// guard simply fired on everything.
TEST(SystemScheduleUnresolvedDependency, FullyResolvedScheduleIsUnaffected)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddBaseSchedule(builder, &log);
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "RenderGraphBuild"), 1);
    EXPECT_LT(IndexOf(log, "Hierarchy"), IndexOf(log, "RenderGraphBuild"));
    EXPECT_LT(IndexOf(log, "Camera"), IndexOf(log, "RenderGraphBuild"));
}

namespace
{
// The plan wave holding each named system; -1 when absent.
int WaveOf(const SystemManager& sm, const std::string& name)
{
    const auto& plan = sm.GetExecutionPlan();
    for (size_t wave = 0; wave < plan.Waves.size(); ++wave)
        for (size_t index : plan.Waves[wave].SystemIndices)
            if (const char* systemName = sm.GetSequentialSystemName(index); systemName && name == systemName)
                return static_cast<int>(wave);
    return -1;
}

// A schedule whose deepest chain (Hierarchy -> Animate -> PostAnimate) is
// longer than RenderGraphBuild's own dependencies, so without the ordering the
// render graph build would share a wave with the chain's middle.
void AddDeepSchedule(SystemScheduleBuilder& b, std::vector<std::string>* log)
{
    AddBaseSchedule(b, log);
    b.Add<RecordingSystem>("Animate", SystemPhase::Extraction, 2, {"Hierarchy"}, log, "Animate");
    b.Add<RecordingSystem>("PostAnimate", SystemPhase::Extraction, 3, {"Animate"}, log, "PostAnimate");
    b.RunAfterAllOthers("RenderGraphBuild");
}
} // namespace

// RunAfterAllOthers puts the system alone in the plan's last wave, after
// systems its declared dependencies never mention.
TEST(SystemScheduleRunAfterAllOthers, RunsAloneInTheLastWave)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddDeepSchedule(builder, &log);
    builder.BuildAndRegisterWithWaves(sm);

    const auto& plan = sm.GetExecutionPlan();
    ASSERT_FALSE(plan.Waves.empty());
    EXPECT_EQ(WaveOf(sm, "RenderGraphBuild"), static_cast<int>(plan.Waves.size()) - 1);
    EXPECT_EQ(plan.Waves.back().SystemIndices.size(), 1u) << "the last wave is shared";
    sm.Update(world, 0.016f);
    EXPECT_EQ(log.back(), "RenderGraphBuild");
}

// A system registered after the first build (a package loaded at project
// open) still runs before the system ordered last.
TEST(SystemScheduleRunAfterAllOthers, LateRegistrationRunsBeforeIt)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddDeepSchedule(builder, &log);
    builder.BuildAndRegisterWithWaves(sm);

    builder.Add<RecordingSystem>("PkgLate", SystemPhase::Extraction, 9, {"PostAnimate"}, &log, "PkgLate");
    builder.BuildAndRegisterWithWaves(sm);
    EXPECT_GT(WaveOf(sm, "RenderGraphBuild"), WaveOf(sm, "PkgLate"));
    EXPECT_EQ(sm.GetExecutionPlan().Waves.back().SystemIndices.size(), 1u);
    sm.Update(world, 0.016f);
    EXPECT_LT(IndexOf(log, "PkgLate"), IndexOf(log, "RenderGraphBuild"));
    EXPECT_EQ(log.back(), "RenderGraphBuild");
}

// A dependency on the system ordered last would form a cycle that takes it out
// of the plan; the dependency is refused by name and the edge dropped, so the
// last system still runs, alone and last.
TEST(SystemScheduleRunAfterAllOthers, DependencyOnTheLastSystemIsDropped)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddDeepSchedule(builder, &log);
    builder.Add<RecordingSystem>("ReadsTheGraph", SystemPhase::Render, 1, {"RenderGraphBuild"}, &log,
                                 "ReadsTheGraph");
    builder.BuildAndRegisterWithWaves(sm);

    ASSERT_GE(WaveOf(sm, "RenderGraphBuild"), 0) << "the last system left the plan";
    EXPECT_EQ(WaveOf(sm, "RenderGraphBuild"), static_cast<int>(sm.GetExecutionPlan().Waves.size()) - 1);
    EXPECT_EQ(sm.GetExecutionPlan().Waves.back().SystemIndices.size(), 1u);
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "RenderGraphBuild"), 1);
    EXPECT_LT(IndexOf(log, "ReadsTheGraph"), IndexOf(log, "RenderGraphBuild"));
}

// A partial (test) schedule may order a system it does not register last; the
// ordering is skipped and everything else runs.
TEST(SystemScheduleRunAfterAllOthers, UnregisteredNameInAPartialScheduleIsSkipped)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;
    SystemScheduleBuilder builder;
    AddBaseSchedule(builder, &log);
    builder.RunAfterAllOthers("NotRegistered");
    builder.BuildAndRegisterWithWaves(sm, ScheduleCompleteness::Partial);
    sm.Update(world, 0.016f);
    EXPECT_EQ(log.size(), 3u);
}

#if !defined(NDEBUG)
// One system per schedule holds the last place: a second claimant would share
// the wave and run concurrently, so it is refused, naming both.
TEST(SystemScheduleRunAfterAllOthers, SecondClaimantAssertsNamingBoth)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            RouteLogsToStderrAndSuppressDialogs();
            SystemScheduleBuilder builder;
            builder.RunAfterAllOthers("RenderGraphBuild");
            builder.RunAfterAllOthers("PresentBuild");
        },
        "'PresentBuild' cannot also run after every other system: 'RenderGraphBuild' already does");
}

// In a complete schedule, ordering a name no system has is a defect.
TEST(SystemScheduleRunAfterAllOthers, UnregisteredNameAssertsInACompleteSchedule)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            RouteLogsToStderrAndSuppressDialogs();
            SystemManager sm;
            std::vector<std::string> log;
            SystemScheduleBuilder builder;
            AddBaseSchedule(builder, &log);
            builder.RunAfterAllOthers("NotRegistered");
            builder.BuildAndRegisterWithWaves(sm);
        },
        "'NotRegistered' is ordered after every other system");
}
#endif
