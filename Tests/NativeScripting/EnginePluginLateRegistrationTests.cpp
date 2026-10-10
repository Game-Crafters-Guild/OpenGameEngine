// Late plugin registration replay + replace-forward/unregister seam.
//
// EngineCore's hook fan-outs (RegisterEngineComponents / RegisterSceneSchemas /
// RegisterRenderPipelineNodes / AddSystemsToSchedule / OnRuntimeInitialized)
// each run exactly once at startup. A plugin registered by a package/user DLL
// loaded at project open arrives after all of them; the registry's
// late-registration handler is the seam that replays the hooks for exactly
// that plugin. A rebuilt module DLL re-registers its plugin under the same id:
// the NEW instance replaces the old for all dispatch (replace-forward; the old
// DLL stays mapped so nothing dangles) and the replay fires once for the
// replacement. These tests pin the registry-side mechanics plus the
// composition with the persistent-schedule re-solve (the full replay body
// lives in EngineCore::EnableRenderingLoop and needs a live runtime).

#include "PluginAPI/EnginePlugin.h"

#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "ECS/World.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

using namespace GameEngine;

class FakePlugin : public Plugins::IEnginePlugin
{
public:
    explicit FakePlugin(const char* id, bool enabledByDefault = true)
        : m_Descriptor{id, "Fake", "1.0", enabledByDefault}
    {
    }

    const Plugins::PluginDescriptor& GetDescriptor() const override { return m_Descriptor; }

    void OnRuntimeInitialized(Plugins::EnginePluginContext&) override { ++RuntimeInitCount; }

    int RuntimeInitCount = 0;

private:
    Plugins::PluginDescriptor m_Descriptor;
};

// Replaced/unregistered fakes leave the registry, but any fake still
// registered when its test ends keeps receiving later tests' fan-outs — so
// fakes must outlive the test: allocate and leak them, unique ids per test.
FakePlugin& MakePlugin(const char* id)
{
    return *new FakePlugin(id);
}

// The registry is a process-global singleton; scope handler + resolver so a
// test never leaks its wiring into the next one.
struct RegistryWiringScope
{
    ~RegistryWiringScope()
    {
        auto& registry = Plugins::EnginePluginRegistry::Get();
        registry.SetLateRegistrationHandler({});
        registry.SetEnabledResolver(nullptr);
    }
};

bool ContainsPlugin(const std::vector<Plugins::IEnginePlugin*>& plugins,
                    const Plugins::IEnginePlugin* plugin)
{
    return std::find(plugins.begin(), plugins.end(), plugin) != plugins.end();
}

} // namespace

TEST(EnginePluginLateRegistration, HandlerFiresOnceForNewEnabledPlugin)
{
    RegistryWiringScope scope;
    auto& registry = Plugins::EnginePluginRegistry::Get();

    std::vector<std::string> replayed;
    registry.SetLateRegistrationHandler(
        [&replayed](Plugins::IEnginePlugin& plugin)
        { replayed.emplace_back(plugin.GetDescriptor().Id); });

    registry.RegisterPlugin(MakePlugin("test.late.enabled"));

    ASSERT_EQ(replayed.size(), 1u);
    EXPECT_EQ(replayed[0], "test.late.enabled");

    // Same-id NEW instance: replace-forward — the replay fires once for the
    // replacement (it missed every startup fan-out just like a late plugin).
    Plugins::IEnginePlugin& replacement = MakePlugin("test.late.enabled");
    registry.RegisterPlugin(replacement);
    ASSERT_EQ(replayed.size(), 2u);
    EXPECT_EQ(replayed[1], "test.late.enabled");

    // Re-registering the ACTIVE instance is an idempotent no-op: nothing to
    // swap, and the replay must not re-fire (it would double
    // OnRuntimeInitialized).
    registry.RegisterPlugin(replacement);
    EXPECT_EQ(replayed.size(), 2u);
}

TEST(EnginePluginLateRegistration, HandlerSkipsDisabledPlugin)
{
    RegistryWiringScope scope;
    auto& registry = Plugins::EnginePluginRegistry::Get();

    registry.SetEnabledResolver(
        [](std::string_view pluginId, bool defaultEnabled)
        { return pluginId == "test.late.disabled" ? false : defaultEnabled; });

    int calls = 0;
    registry.SetLateRegistrationHandler([&calls](Plugins::IEnginePlugin&) { ++calls; });

    registry.RegisterPlugin(MakePlugin("test.late.disabled"));
    EXPECT_EQ(calls, 0);

    // Replacing a disabled plugin swaps the instance but must not replay.
    registry.RegisterPlugin(MakePlugin("test.late.disabled"));
    EXPECT_EQ(calls, 0);

    registry.RegisterPlugin(MakePlugin("test.late.other"));
    EXPECT_EQ(calls, 1);
}

TEST(EnginePluginLateRegistration, NoHandlerMeansStartupPathUnchanged)
{
    RegistryWiringScope scope;
    auto& registry = Plugins::EnginePluginRegistry::Get();

    // No handler installed (startup-time registration): must not throw, and
    // installing a handler afterwards must not fire retroactively.
    registry.RegisterPlugin(MakePlugin("test.late.startup"));

    int calls = 0;
    registry.SetLateRegistrationHandler([&calls](Plugins::IEnginePlugin&) { ++calls; });
    EXPECT_EQ(calls, 0);

    // Clearing the handler (rendering loop shutdown) stops replay for
    // registrations that follow.
    registry.SetLateRegistrationHandler({});
    registry.RegisterPlugin(MakePlugin("test.late.after-clear"));
    EXPECT_EQ(calls, 0);
}

// Replace-forward is the hot-reload contract: after a same-id re-registration
// the OLD instance receives no dispatch at all (its DLL stays mapped, so the
// stale code is merely unreachable, never dangling) and the NEW instance
// receives every fan-out.
TEST(EnginePluginReplaceForward, SameIdReplacementSwapsDispatch)
{
    RegistryWiringScope scope;
    auto& registry = Plugins::EnginePluginRegistry::Get();

    FakePlugin& original = MakePlugin("test.replace.dispatch");
    registry.RegisterPlugin(original);
    const std::size_t countAfterFirst = registry.GetPlugins().size();

    Plugins::EnginePluginContext context{};
    registry.OnRuntimeInitialized(context);
    EXPECT_EQ(original.RuntimeInitCount, 1);

    FakePlugin& replacement = MakePlugin("test.replace.dispatch");
    registry.RegisterPlugin(replacement);

    const std::vector<Plugins::IEnginePlugin*> plugins = registry.GetPlugins();
    EXPECT_EQ(plugins.size(), countAfterFirst) << "same-id re-registration must not grow the registry";
    EXPECT_FALSE(ContainsPlugin(plugins, &original)) << "the old instance must stop being dispatched";
    EXPECT_TRUE(ContainsPlugin(plugins, &replacement));

    registry.OnRuntimeInitialized(context);
    EXPECT_EQ(original.RuntimeInitCount, 1) << "replaced instance still receives hooks";
    EXPECT_EQ(replacement.RuntimeInitCount, 1);
}

TEST(EnginePluginUnregister, RemovesDispatchAndRefusesStaleInstance)
{
    RegistryWiringScope scope;
    auto& registry = Plugins::EnginePluginRegistry::Get();

    FakePlugin& original = MakePlugin("test.unregister");
    FakePlugin& replacement = MakePlugin("test.unregister");
    registry.RegisterPlugin(original);
    registry.RegisterPlugin(replacement); // replace-forward: original is out

    // A stale (already-replaced) instance must not knock out its successor.
    EXPECT_FALSE(registry.UnregisterPlugin(original));
    EXPECT_TRUE(ContainsPlugin(registry.GetPlugins(), &replacement));

    // A never-registered instance is refused.
    FakePlugin& stranger = MakePlugin("test.unregister.stranger");
    EXPECT_FALSE(registry.UnregisterPlugin(stranger));

    // Unregistering the active instance detaches it from all dispatch.
    EXPECT_TRUE(registry.UnregisterPlugin(replacement));
    EXPECT_FALSE(ContainsPlugin(registry.GetPlugins(), &replacement));

    Plugins::EnginePluginContext context{};
    registry.OnRuntimeInitialized(context);
    EXPECT_EQ(replacement.RuntimeInitCount, 0) << "unregistered instance still receives hooks";

    // Double-unregister is refused, not a crash or a silent success.
    EXPECT_FALSE(registry.UnregisterPlugin(replacement));
}


// ---------------------------------------------------------------------------
// Composition with the persistent schedule model (C12): a replaced plugin's
// replay re-adds its systems to the live builder under its owner bracket. The
// reconcile swaps the same-named system OBJECTS in place — the NEW module's
// code ticks from the next update, nothing double-ticks, dropped names
// retire, and genuinely new names splice in via the re-solve.
// ---------------------------------------------------------------------------

namespace
{

class RecordingSystem : public ECS::ISystem
{
public:
    RecordingSystem(std::vector<std::string>* log, std::string id)
        : m_Log(log), m_Id(std::move(id))
    {
    }
    void Update(ECS::World&, float32) override { m_Log->push_back(m_Id); }
    const char* GetName() const override { return m_Id.c_str(); }

private:
    std::vector<std::string>* m_Log;
    std::string m_Id;
};

// A plugin whose systems tag the tick log with the plugin GENERATION that
// instantiated them, so the log proves which instance's code is running.
class SystemBearingPlugin final : public FakePlugin
{
public:
    SystemBearingPlugin(const char* id, std::vector<std::string>* log, std::string generation,
                        std::vector<std::string> systemNames)
        : FakePlugin(id), m_Log(log), m_Generation(std::move(generation)),
          m_SystemNames(std::move(systemNames))
    {
    }

    void AddSystemsToSchedule(ECS::SystemScheduleBuilder& builder,
                              Plugins::EnginePluginContext&) override
    {
        for (const std::string& name : m_SystemNames)
        {
            builder.Add<RecordingSystem>(name, ECS::SystemPhase::Extraction, 2, {}, m_Log,
                                         name + "/" + m_Generation);
        }
    }

private:
    std::vector<std::string>* m_Log;
    std::string m_Generation;
    std::vector<std::string> m_SystemNames;
};

int64_t Count(const std::vector<std::string>& log, const std::string& id)
{
    return std::count(log.begin(), log.end(), id);
}

// EngineCore's replay shape (EnableRenderingLoop's late-registration handler):
// owner-bracketed contribution to the persistent builder, re-solved when the
// hook added registrations OR marked retirements.
void InstallEngineShapedReplayHandler(Plugins::EnginePluginRegistry& registry,
                                      ECS::SystemScheduleBuilder& builder, ECS::SystemManager& sm)
{
    registry.SetLateRegistrationHandler(
        [&builder, &sm](Plugins::IEnginePlugin& plugin)
        {
            Plugins::EnginePluginContext context{};
            const size_t regsBefore = builder.GetRegistrationCount();
            builder.BeginOwnedRegistrations(plugin.GetDescriptor().Id);
            plugin.AddSystemsToSchedule(builder, context);
            builder.EndOwnedRegistrations();
            if (builder.GetRegistrationCount() != regsBefore || builder.HasPendingRetirements())
                builder.BuildAndRegisterWithWaves(sm);
        });
}

} // namespace

TEST(EnginePluginReplaceForward, ReplacedPluginSystemsSwapToNewCode)
{
    RegistryWiringScope scope;
    auto& registry = Plugins::EnginePluginRegistry::Get();

    ECS::World world(nullptr);
    ECS::SystemManager sm; // no job system → waves run inline, order observable
    ECS::SystemScheduleBuilder builder;
    std::vector<std::string> log;
    builder.Add<RecordingSystem>("RenderGraphBuild", ECS::SystemPhase::Render, 0, {}, &log,
                                 "RenderGraphBuild");
    builder.BuildAndRegisterWithWaves(sm);

    InstallEngineShapedReplayHandler(registry, builder, sm);

    // v1 loads late (package DLL at project open) and schedules PkgSys.
    static SystemBearingPlugin v1("test.replace.systems", &log, "v1", {"PkgSys"});
    registry.RegisterPlugin(v1);
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgSys/v1"), 1);
    log.clear();

    // Module rebuild: v2 re-registers the same id, re-declares PkgSys, and
    // adds a genuinely new system. The reconcile must swap PkgSys to v2's
    // code — same schedule slot, no double-tick — and splice PkgSysNew in.
    static SystemBearingPlugin v2("test.replace.systems", &log, "v2", {"PkgSys", "PkgSysNew"});
    registry.RegisterPlugin(v2);
    sm.Update(world, 0.016f);

    EXPECT_EQ(Count(log, "PkgSys/v2"), 1)
        << "replaced plugin's same-named system must execute the NEW code";
    EXPECT_EQ(Count(log, "PkgSys/v1"), 0)
        << "the old instance must stop ticking after the swap";
    EXPECT_EQ(Count(log, "PkgSysNew/v2"), 1)
        << "replacement's new system was not integrated into the schedule";
    EXPECT_EQ(Count(log, "RenderGraphBuild"), 1);
    log.clear();

    // Third generation drops PkgSysNew again: it must retire (stop ticking)
    // even though the same-named PkgSys swap keeps the registration count
    // unchanged net of the re-add.
    static SystemBearingPlugin v3("test.replace.systems", &log, "v3", {"PkgSys"});
    registry.RegisterPlugin(v3);
    sm.Update(world, 0.016f);
    EXPECT_EQ(Count(log, "PkgSys/v3"), 1);
    EXPECT_EQ(Count(log, "PkgSys/v2"), 0);
    EXPECT_EQ(Count(log, "PkgSysNew/v2"), 0) << "system dropped by the replacement must retire";

    registry.UnregisterPlugin(v3);
}

// ---------------------------------------------------------------------------
// Registration hold (C12 loader-lock contract): replays queue during the
// LoadLibrary+handshake window and fire on EndRegistrationHold; a purge (load
// abort) scrubs the queue so a refused module never replays.
// ---------------------------------------------------------------------------

TEST(EnginePluginRegistrationHold, ReplayDefersUntilHoldEnds)
{
    RegistryWiringScope scope;
    auto& registry = Plugins::EnginePluginRegistry::Get();

    std::vector<std::string> replayed;
    registry.SetLateRegistrationHandler(
        [&replayed](Plugins::IEnginePlugin& plugin)
        { replayed.emplace_back(plugin.GetDescriptor().Id); });

    registry.BeginRegistrationHold();
    FakePlugin& held = MakePlugin("test.hold.deferred");
    registry.RegisterPlugin(held);
    EXPECT_TRUE(replayed.empty()) << "replay must not fire under the hold";
    // The registration itself is live immediately (dispatch works during the
    // hold; only the replay waits).
    EXPECT_TRUE(ContainsPlugin(registry.GetPlugins(), &held));

    registry.EndRegistrationHold();
    ASSERT_EQ(replayed.size(), 1u);
    EXPECT_EQ(replayed[0], "test.hold.deferred");

    // Idempotent: a second End fires nothing.
    registry.EndRegistrationHold();
    EXPECT_EQ(replayed.size(), 1u);

    registry.UnregisterPlugin(held);
}

TEST(EnginePluginRegistrationHold, AbortPurgeScrubsQueuedReplay)
{
    RegistryWiringScope scope;
    auto& registry = Plugins::EnginePluginRegistry::Get();

    int replays = 0;
    registry.SetLateRegistrationHandler([&replays](Plugins::IEnginePlugin&) { ++replays; });

    // Simulate NSM's LoadModule bracket: stamp active while the module's
    // registrars run under the hold, then the handshake fails and the purge
    // runs before the image would be unmapped.
    registry.BeginRegistrationHold();
    ECS::SetActiveRegistrationModule("AbortPack", 7);
    FakePlugin& doomed = MakePlugin("test.hold.aborted");
    registry.RegisterPlugin(doomed);
    ECS::ClearActiveRegistrationModule();

    EXPECT_EQ(registry.PurgeModulePlugins("AbortPack", 7), 1u);
    registry.EndRegistrationHold();

    EXPECT_EQ(replays, 0) << "a purged (aborted) module must never replay";
    EXPECT_FALSE(ContainsPlugin(registry.GetPlugins(), &doomed));
}

// ---------------------------------------------------------------------------
// Module attribution (C12): registrations carry the loader's active
// {module, generation} stamp — the load-abort path unregisters by module, and
// the unload ledger counts superseded survivors.
// ---------------------------------------------------------------------------

TEST(EnginePluginModuleAttribution, PurgeByModuleUnregistersExactGeneration)
{
    RegistryWiringScope scope;
    auto& registry = Plugins::EnginePluginRegistry::Get();

    ECS::SetActiveRegistrationModule("PackA", 1);
    FakePlugin& packA = MakePlugin("test.attr.packA");
    registry.RegisterPlugin(packA);
    ECS::SetActiveRegistrationModule("PackB", 3);
    FakePlugin& packB = MakePlugin("test.attr.packB");
    registry.RegisterPlugin(packB);
    ECS::ClearActiveRegistrationModule();
    FakePlugin& startup = MakePlugin("test.attr.static");
    registry.RegisterPlugin(startup);

    const std::vector<std::string> packAIds = registry.GetModulePluginIds("PackA");
    ASSERT_EQ(packAIds.size(), 1u);
    EXPECT_EQ(packAIds[0], "test.attr.packA");

    // Wrong generation purges nothing; the exact stamp purges exactly PackA's.
    EXPECT_EQ(registry.PurgeModulePlugins("PackA", 2), 0u);
    EXPECT_EQ(registry.PurgeModulePlugins("PackA", 1), 1u);
    EXPECT_FALSE(ContainsPlugin(registry.GetPlugins(), &packA));
    EXPECT_TRUE(ContainsPlugin(registry.GetPlugins(), &packB));
    EXPECT_TRUE(ContainsPlugin(registry.GetPlugins(), &startup));

    // Empty module id must never purge (static registrations are not module-owned).
    EXPECT_EQ(registry.PurgeModulePlugins("", 0), 0u);
    EXPECT_TRUE(ContainsPlugin(registry.GetPlugins(), &startup));

    registry.UnregisterPlugin(packB);
    registry.UnregisterPlugin(startup);
}

TEST(EnginePluginModuleAttribution, SupersededLedgerCountsStaleGenerations)
{
    RegistryWiringScope scope;
    auto& registry = Plugins::EnginePluginRegistry::Get();

    ECS::SetActiveRegistrationModule("LedgerPack", 1);
    FakePlugin& v1 = MakePlugin("test.attr.ledger");
    registry.RegisterPlugin(v1);
    ECS::ClearActiveRegistrationModule();

    // Generation 2 loaded but did NOT re-register this plugin id (dropped or
    // disabled): the v1 instance survives with the old stamp — one stale entry.
    EXPECT_EQ(registry.CountSupersededModulePlugins("LedgerPack", 2), 1u);

    // A replacement under generation 2 re-owns the entry.
    ECS::SetActiveRegistrationModule("LedgerPack", 2);
    FakePlugin& v2 = MakePlugin("test.attr.ledger");
    registry.RegisterPlugin(v2);
    ECS::ClearActiveRegistrationModule();
    EXPECT_EQ(registry.CountSupersededModulePlugins("LedgerPack", 2), 0u);

    registry.UnregisterPlugin(v2);
}
