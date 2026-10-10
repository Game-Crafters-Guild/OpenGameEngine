// C12 module-reload protocol — schedule-model semantics.
//
// A reloaded module's plugin replay re-declares its systems into the
// persistent SystemScheduleBuilder under the same owner (plugin id). The
// reconcile must swap SYSTEM BEHAVIOR to the new code: same-named
// registrations replace the live system object in its existing manager slot
// (wave placement stable, no double-tick, old object destroyed exactly once),
// names the replay stopped declaring retire, and the module/generation stamps
// power the load-abort purge + unload quiesce ledger.

#include <gtest/gtest.h>

#include "ECS/ModuleRegistration.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"

#include <algorithm>
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

// Lifecycle probe: counts constructions/destructions per tag so tests can
// assert the swap destroys the OLD object exactly once and constructs the NEW
// one exactly once. The destructor writes to the counters, so declare them
// before the SystemManager that owns the system: locals die in reverse order.
struct LifecycleCounters
{
    int Constructed = 0;
    int Destroyed = 0;
};

class LifecycleSystem : public ISystem
{
public:
    LifecycleSystem(LifecycleCounters* counters, std::vector<std::string>* log, const char* id)
        : m_Counters(counters), m_Log(log), m_Id(id)
    {
        ++m_Counters->Constructed;
    }
    ~LifecycleSystem() override { ++m_Counters->Destroyed; }
    void Update(World&, float32) override { m_Log->push_back(m_Id); }
    const char* GetName() const override { return m_Id.c_str(); }

private:
    LifecycleCounters* m_Counters;
    std::vector<std::string>* m_Log;
    std::string m_Id;
};

int64_t Count(const std::vector<std::string>& log, const std::string& id)
{
    return std::count(log.begin(), log.end(), id);
}

// Wave index of a named system in the manager's execution plan, or -1.
int WaveOf(SystemManager& sm, const std::string& name)
{
    const SystemExecutionPlan& plan = sm.GetExecutionPlan();
    for (size_t wi = 0; wi < plan.Waves.size(); ++wi)
    {
        for (size_t idx : plan.Waves[wi].SystemIndices)
        {
            const char* n = sm.GetSequentialSystemName(idx);
            if (n && name == n)
                return static_cast<int>(wi);
        }
    }
    return -1;
}

// Manager slot of a named system via the plan, or npos.
size_t SlotOf(SystemManager& sm, const std::string& name)
{
    const SystemExecutionPlan& plan = sm.GetExecutionPlan();
    for (const auto& wave : plan.Waves)
    {
        for (size_t idx : wave.SystemIndices)
        {
            const char* n = sm.GetSequentialSystemName(idx);
            if (n && name == n)
                return idx;
        }
    }
    return static_cast<size_t>(-1);
}

struct StampScope
{
    StampScope(std::string_view moduleId, std::uint64_t generation)
    {
        SetActiveRegistrationModule(moduleId, generation);
    }
    ~StampScope() { ClearActiveRegistrationModule(); }
};

void AddBaseSchedule(SystemScheduleBuilder& b, std::vector<std::string>* log)
{
    b.Add<RecordingSystem>("Hierarchy", SystemPhase::Extraction, 1, {}, log, "Hierarchy");
    b.Add<RecordingSystem>("RenderGraphBuild", SystemPhase::Render, 0, {"Hierarchy"}, log,
                           "RenderGraphBuild");
}
} // namespace

// The load-bearing C12 behavior: after a replacement replay, the same-named
// system executes the NEW code — in the same wave slot, exactly once per tick.
TEST(ModuleReloadSchedule, ReplacedOwnersSameNamedSystemRunsNewCode)
{
    World world(nullptr);
    SystemManager sm; // no job system → waves run inline, order observable
    SystemScheduleBuilder builder;
    std::vector<std::string> log;
    AddBaseSchedule(builder, &log);

    builder.BeginOwnedRegistrations("test.pkg");
    builder.Add<RecordingSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &log, "PkgSys/v1");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgSys/v1"), 1);
    const int waveBefore = WaveOf(sm, "PkgSys");
    const size_t slotBefore = SlotOf(sm, "PkgSys");
    log.clear();

    // Module reload: the replacement replay re-declares the same name under
    // the same owner (same phase/order/deps — the common case).
    builder.BeginOwnedRegistrations("test.pkg");
    builder.Add<RecordingSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &log, "PkgSys/v2");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgSys/v2"), 1) << "the replacement's code must tick after the swap";
    EXPECT_EQ(Count(log, "PkgSys/v1"), 0) << "the replaced object must stop ticking";
    EXPECT_EQ(Count(log, "Hierarchy"), 1);
    EXPECT_EQ(Count(log, "RenderGraphBuild"), 1);

    EXPECT_EQ(WaveOf(sm, "PkgSys"), waveBefore) << "wave placement must be identical after the swap";
    EXPECT_EQ(SlotOf(sm, "PkgSys"), slotBefore) << "the swap must reuse the same manager slot";
}

// Old instance destroyed exactly once, new instance constructed exactly once —
// and a later unrelated re-solve must not re-fire either.
TEST(ModuleReloadSchedule, SwapLifecycleFiresExactlyOnceEachSide)
{
    std::vector<std::string> log;
    LifecycleCounters v1;
    LifecycleCounters v2;
    World world(nullptr);
    SystemManager sm;
    SystemScheduleBuilder builder;

    builder.BeginOwnedRegistrations("test.pkg");
    builder.Add<LifecycleSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &v1, &log, "PkgSys/v1");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);
    EXPECT_EQ(v1.Constructed, 1);
    EXPECT_EQ(v1.Destroyed, 0);

    builder.BeginOwnedRegistrations("test.pkg");
    builder.Add<LifecycleSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &v2, &log, "PkgSys/v2");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);

    EXPECT_EQ(v1.Destroyed, 1) << "old system object must be destroyed by the swap";
    EXPECT_EQ(v2.Constructed, 1) << "new system object must be constructed by the swap";
    EXPECT_EQ(v2.Destroyed, 0);

    // An unrelated re-solve (new registration elsewhere) must not touch the
    // swapped slot again.
    builder.Add<RecordingSystem>("Unrelated", SystemPhase::Late, 0, {}, &log, "Unrelated");
    builder.BuildAndRegisterWithWaves(sm);
    EXPECT_EQ(v1.Destroyed, 1);
    EXPECT_EQ(v2.Constructed, 1);
    EXPECT_EQ(v2.Destroyed, 0);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgSys/v2"), 1);
    EXPECT_EQ(Count(log, "PkgSys/v1"), 0);
}

// A replacement replay that stops declaring a system retires it: the object is
// destroyed, its slot goes null (skipped by every execution path), and other
// systems' slots are untouched.
TEST(ModuleReloadSchedule, UndeclaredSystemRetiresOnReload)
{
    std::vector<std::string> log;
    LifecycleCounters dropped;
    World world(nullptr);
    SystemManager sm;
    SystemScheduleBuilder builder;

    builder.BeginOwnedRegistrations("test.pkg");
    builder.Add<RecordingSystem>("PkgKeep", SystemPhase::Extraction, 2, {}, &log, "PkgKeep/v1");
    builder.Add<LifecycleSystem>("PkgDrop", SystemPhase::Extraction, 3, {}, &dropped, &log, "PkgDrop/v1");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgKeep/v1"), 1);
    EXPECT_EQ(Count(log, "PkgDrop/v1"), 1);
    log.clear();

    // v2 keeps PkgKeep but no longer declares PkgDrop.
    builder.BeginOwnedRegistrations("test.pkg");
    builder.Add<RecordingSystem>("PkgKeep", SystemPhase::Extraction, 2, {}, &log, "PkgKeep/v2");
    builder.EndOwnedRegistrations();
    EXPECT_TRUE(builder.HasPendingRetirements());
    builder.BuildAndRegisterWithWaves(sm);
    EXPECT_FALSE(builder.HasPendingRetirements());

    EXPECT_EQ(dropped.Destroyed, 1) << "retired system object must be destroyed";
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgKeep/v2"), 1);
    EXPECT_EQ(Count(log, "PkgDrop/v1"), 0) << "retired system must stop ticking";
    EXPECT_EQ(Count(log, "Unrelated"), 0);
}

// SetEnabled(false) applied by a user/tool survives the module reload swap —
// the replacement instance must not silently resurrect a disabled system.
TEST(ModuleReloadSchedule, EnabledStateSurvivesSwap)
{
    World world(nullptr);
    SystemManager sm;
    SystemScheduleBuilder builder;
    std::vector<std::string> log;

    builder.BeginOwnedRegistrations("test.pkg");
    builder.Add<RecordingSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &log, "PkgSys/v1");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);

    ASSERT_TRUE(sm.SetSystemEnabledByName("PkgSys", false));
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgSys/v1"), 0);

    builder.BeginOwnedRegistrations("test.pkg");
    builder.Add<RecordingSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &log, "PkgSys/v2");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgSys/v2"), 0) << "swap must preserve the disabled state";

    ASSERT_TRUE(sm.SetSystemEnabledByName("PkgSys", true));
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgSys/v2"), 1) << "re-enable must tick the NEW instance";
    EXPECT_EQ(Count(log, "PkgSys/v1"), 0);
}

// A replacement replay may change the replaced system's dependencies — the
// re-solve must move it to the wave a from-scratch build would give it.
TEST(ModuleReloadSchedule, SwapWithChangedDependenciesReplaces)
{
    World world(nullptr);
    SystemManager sm;
    SystemScheduleBuilder builder;
    std::vector<std::string> log;
    AddBaseSchedule(builder, &log);

    builder.BeginOwnedRegistrations("test.pkg");
    builder.Add<RecordingSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &log, "PkgSys/v1");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);
    EXPECT_EQ(WaveOf(sm, "PkgSys"), 0) << "no deps → root wave";

    // v2 depends on Hierarchy → must move to a later wave, still swapping code.
    builder.BeginOwnedRegistrations("test.pkg");
    builder.Add<RecordingSystem>("PkgSys", SystemPhase::Extraction, 2, {"Hierarchy"}, &log, "PkgSys/v2");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);

    EXPECT_GT(WaveOf(sm, "PkgSys"), 0) << "changed dependencies must re-place the system";
    log.clear();
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgSys/v2"), 1);
    EXPECT_EQ(Count(log, "PkgSys/v1"), 0);
    // Dependency order actually honored in the tick log.
    const auto hierarchyAt = std::find(log.begin(), log.end(), "Hierarchy");
    const auto pkgAt = std::find(log.begin(), log.end(), "PkgSys/v2");
    EXPECT_TRUE(hierarchyAt < pkgAt);
}

// A same-named registration from a DIFFERENT owner is a collision, not a
// replacement: first wins, exactly as before C12.
TEST(ModuleReloadSchedule, CrossOwnerCollisionStillFirstWins)
{
    World world(nullptr);
    SystemManager sm;
    SystemScheduleBuilder builder;
    std::vector<std::string> log;

    builder.BeginOwnedRegistrations("test.pkgA");
    builder.Add<RecordingSystem>("SharedName", SystemPhase::Extraction, 2, {}, &log, "SharedName/A");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);

    builder.BeginOwnedRegistrations("test.pkgB");
    builder.Add<RecordingSystem>("SharedName", SystemPhase::Extraction, 2, {}, &log, "SharedName/B");
    builder.EndOwnedRegistrations();
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "SharedName/A"), 1) << "collision must keep the first owner's system";
    EXPECT_EQ(Count(log, "SharedName/B"), 0);
}

// Core (owner-less) registrations are never treated as replacements — the
// legacy duplicate-drop behavior is unchanged for them.
TEST(ModuleReloadSchedule, OwnerlessDuplicateStillDropped)
{
    World world(nullptr);
    SystemManager sm;
    SystemScheduleBuilder builder;
    std::vector<std::string> log;

    builder.Add<RecordingSystem>("CoreSys", SystemPhase::Extraction, 2, {}, &log, "CoreSys/first");
    builder.BuildAndRegisterWithWaves(sm);
    builder.Add<RecordingSystem>("CoreSys", SystemPhase::Extraction, 2, {}, &log, "CoreSys/second");
    builder.BuildAndRegisterWithWaves(sm);

    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "CoreSys/first"), 1);
    EXPECT_EQ(Count(log, "CoreSys/second"), 0);
}

// Module/generation stamps: a reconciled reload leaves no superseded
// registrations; a module whose plugin was NOT replayed (disabled plugin,
// dropped plugin id) keeps stale entries — which the unload ledger must count.
TEST(ModuleReloadSchedule, SupersededRegistrationLedger)
{
    World world(nullptr);
    SystemManager sm;
    SystemScheduleBuilder builder;
    std::vector<std::string> log;

    {
        StampScope stamp("TestPack", 1);
        builder.BeginOwnedRegistrations("test.pkg");
        builder.Add<RecordingSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &log, "PkgSys/v1");
        builder.EndOwnedRegistrations();
    }
    builder.BuildAndRegisterWithWaves(sm);
    EXPECT_EQ(builder.CountSupersededModuleRegistrations("TestPack", 1), 0u);

    // Generation 2 loads but never replays the plugin (e.g. it was disabled):
    // the generation-1 system object keeps running gen-1 code — one stale entry.
    EXPECT_EQ(builder.CountSupersededModuleRegistrations("TestPack", 2), 1u);

    // A proper replacement replay under generation 2 re-owns the registration.
    {
        StampScope stamp("TestPack", 2);
        builder.BeginOwnedRegistrations("test.pkg");
        builder.Add<RecordingSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &log, "PkgSys/v2");
        builder.EndOwnedRegistrations();
    }
    // Reconcile recorded but not yet applied: the OLD object is still
    // installed, so the ledger must still refuse.
    EXPECT_GE(builder.CountSupersededModuleRegistrations("TestPack", 2), 1u);
    builder.BuildAndRegisterWithWaves(sm);
    EXPECT_EQ(builder.CountSupersededModuleRegistrations("TestPack", 2), 0u);
}

// Load-abort purge: registrations stamped with the aborted generation drop out
// of the model (instantiated slots retired); older generations are untouched.
TEST(ModuleReloadSchedule, AbortPurgeDropsExactGeneration)
{
    World world(nullptr);
    SystemManager sm;
    SystemScheduleBuilder builder;
    std::vector<std::string> log;

    {
        StampScope stamp("TestPack", 1);
        builder.BeginOwnedRegistrations("test.pkg");
        builder.Add<RecordingSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &log, "PkgSys/v1");
        builder.EndOwnedRegistrations();
    }
    builder.BuildAndRegisterWithWaves(sm);

    // An aborted generation-2 load managed to register one NEW system before
    // the handshake refused it.
    {
        StampScope stamp("TestPack", 2);
        builder.BeginOwnedRegistrations("test.pkg");
        builder.Add<RecordingSystem>("PkgSys", SystemPhase::Extraction, 2, {}, &log, "PkgSys/v2");
        builder.Add<RecordingSystem>("PkgNew", SystemPhase::Extraction, 3, {}, &log, "PkgNew/v2");
        builder.EndOwnedRegistrations();
    }
    EXPECT_EQ(builder.PurgeModuleRegistrations(sm, "TestPack", 2), 2u);

    builder.BuildAndRegisterWithWaves(sm);
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgSys/v1"), 1) << "the mapped generation-1 system must keep ticking";
    EXPECT_EQ(Count(log, "PkgSys/v2"), 0);
    EXPECT_EQ(Count(log, "PkgNew/v2"), 0);
    EXPECT_EQ(builder.CountSupersededModuleRegistrations("TestPack", 1), 0u);
}

// Retired slots must be inert everywhere: wave path, flat fallback, name
// lookups, plan re-installation.
TEST(ModuleReloadSchedule, RetiredSlotIsInertEverywhere)
{
    World world(nullptr);
    SystemManager sm;
    std::vector<std::string> log;

    sm.AddSystem<RecordingSystem>(&log, "A");
    sm.AddSystem<RecordingSystem>(&log, "B");

    // Flat path (no plan) with a retired slot.
    ASSERT_TRUE(sm.RetireSystem(0));
    EXPECT_FALSE(sm.RetireSystem(0)) << "double-retire reports false";
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "A"), 0);
    EXPECT_EQ(Count(log, "B"), 1);
    log.clear();

    // Wave path referencing the retired slot skips it.
    SystemExecutionPlan plan;
    plan.Waves.push_back({{0, 1}});
    sm.SetExecutionPlan(std::move(plan));
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "A"), 0);
    EXPECT_EQ(Count(log, "B"), 1);

    EXPECT_FALSE(sm.SetSystemEnabledByName("A", true)) << "retired systems are not addressable";
    EXPECT_EQ(sm.GetSequentialSystemName(0), nullptr);
}
