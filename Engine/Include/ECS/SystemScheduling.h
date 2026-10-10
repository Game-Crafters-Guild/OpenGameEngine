#pragma once

#include "ECS/ModuleRegistration.h"
#include "ECS/Systems.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cctype>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <tuple>

namespace GameEngine { namespace ECS {

namespace Detail
{
// Wrap a system type and override GetName() with the schedule-provided name.
// This allows tooling (and Engine::SetRenderingSystemEnabled) to address systems
// by the stable schedule name rather than the concrete C++ type name.
template <typename T>
class NamedSystem final : public T
{
public:
    template <typename... Args>
    explicit NamedSystem(std::string scheduledName, Args&&... args)
        : T(std::forward<Args>(args)...), m_ScheduledName(std::move(scheduledName))
    {
    }

    const char* GetName() const override { return m_ScheduledName.c_str(); }

private:
    std::string m_ScheduledName;
};
} // namespace Detail

// High-level phases to group systems and give coarse ordering
enum class SystemPhase : int {
    Early = 0,
    Animation = 10,
    Skinning = 20,
    Extraction = 30,
    Camera = 40,
    Visibility = 50,
    Culling = 60,
    Render = 70,
    Late = 100,
};

// One declared ordering edge: the name of a system that must run before the
// declaring system. Implicitly constructible from a name so the common case
// still reads as a plain list of names — {"TransformHierarchy", "Camera"}.
//
// A dependency name is the ONLY thing separating "runs after" from "runs
// concurrently on another worker thread", so a name that matches no registered
// system is a scheduling defect, not a hint: the wave solver would drop the
// edge and silently widen a wave. BuildAndRegisterWithWaves therefore rejects
// unresolved REQUIRED names. Use OptionalDependency for the declared exception.
struct SystemDependency {
    std::string Name;
    // Set only via OptionalDependency: the name belongs to a system that may
    // legitimately not be registered in every configuration, so an unresolved
    // name is expected rather than a defect. The edge is still honored in full
    // whenever the name IS present, including on a later re-solve once a
    // package registers it.
    bool Optional { false };

    SystemDependency(const char* name) : Name(name) {}
    SystemDependency(std::string name) : Name(std::move(name)) {}
};

// Declare a dependency on a system that may legitimately be absent — one a
// package or optional module supplies only when it is loaded. Unresolved
// optional names are reported at Debug level and never fail the build.
//
// This is a deliberate hole in the unresolved-dependency guard: it also
// silences the typo it exists to catch, so only reach for it where the name
// genuinely is conditional, never to quiet a name you cannot place.
inline SystemDependency OptionalDependency(std::string name)
{
    SystemDependency dep{std::move(name)};
    dep.Optional = true;
    return dep;
}

// Whether a given schedule build is expected to contain every system its
// registrations name. Application schedules are Complete: every module has
// contributed by the time the schedule is built, so an unresolved required
// dependency is a defect. Partial exists for module-level tests that build a
// deliberate subset of the application schedule to assert one edge in
// isolation — there, unresolved names are the point of the fixture.
enum class ScheduleCompleteness {
    Complete,
    Partial,
};

struct SystemRegistration {
    std::string Name;                        // unique system name
    SystemPhase Phase { SystemPhase::Early }; // coarse ordering
    int Order { 0 };                         // fine ordering within phase (lower runs first)
    std::vector<SystemDependency> Dependencies; // systems that must run before this
    // Instantiate the system into the manager: append a new slot, or — when the
    // registration was superseded by a module reload — replace the object at
    // its existing slot in place. Returns the slot index.
    std::function<size_t(ECS::SystemManager&, size_t replaceIndex)> AddToManager;

    // C12 reconcile identity: the plugin id whose AddSystemsToSchedule made
    // this registration (set by the owner scope; empty = engine-core). A
    // same-named re-registration from the SAME owner is a reload replacement —
    // it swaps this registration's code and re-instantiates into the same
    // slot; from a different owner it is a collision and is dropped
    // (first-wins), as before.
    std::string Owner;
    // Module + load generation active at Add (ECS/ModuleRegistration.h) — the
    // unload quiesce ledger's evidence of which image owns the system code.
    ModuleRegistrationStamp Module;
    // Set by EndOwnedRegistrations when the owner's newest replay stopped
    // declaring this name: the next wave build retires the slot and drops the
    // registration.
    bool Retired = false;
    // Set by the reconcile when a replacement superseded an already-
    // instantiated registration: the next wave build re-instantiates into the
    // recorded slot.
    bool PendingReplace = false;
};

class SystemScheduleBuilder {
public:
    // Templated helper for sequential systems
    template<typename T, typename... CtorArgs>
    SystemScheduleBuilder& Add(const std::string& name, SystemPhase phase, int order = 0,
                               std::vector<SystemDependency> Deps = {},
                               CtorArgs&&... ctorArgs) {
        SystemRegistration r;
        r.Name = name; r.Phase = phase; r.Order = order; r.Dependencies = std::move(Deps);
        r.Owner = m_ActiveOwner;
        r.Module = GetActiveRegistrationModule();
        using ArgsTuple = std::tuple<std::decay_t<CtorArgs>...>;
        ArgsTuple args{std::forward<CtorArgs>(ctorArgs)...};
        std::string scheduledName = name;
        r.AddToManager = [args, scheduledName](ECS::SystemManager& m, size_t replaceIndex) mutable -> size_t {
            return std::apply(
                [&](auto&&... a)
                {
                    return m.InstallSystem(
                        std::make_unique<Detail::NamedSystem<T>>(scheduledName, a...), replaceIndex);
                },
                args);
        };
        if (!m_ActiveOwner.empty())
            m_ScopeAddedNames.insert(name);
        m_Regs.push_back(std::move(r));
        return *this;
    }

    // Orders the named system after every other registered system, so it runs
    // alone in the plan's last wave: for a system that consumes the whole
    // frame's state (the render graph build), whose producers cannot all be
    // listed because packages add systems late. Applied at every wave build, so
    // a late registration still lands before it.
    //
    // One system per schedule holds the place: a second name is refused with an
    // error naming both (and asserts in dev builds), since two "last" systems
    // would share the wave and run concurrently. A dependency on the system
    // ordered last is refused by name and its edge dropped, because it would
    // form a cycle that takes the last system out of the plan. The name must be
    // registered by the time the schedule is built. A system added straight to
    // a SystemManager after its plan exists (not through this builder) still
    // runs in a trailing wave after it.
    SystemScheduleBuilder& RunAfterAllOthers(const std::string& name) {
        if (!m_RunsLast.empty() && m_RunsLast != name)
        {
            Logger::Log::Error("[ECS] Scheduler: '{}' cannot also run after every other system: '{}' "
                               "already does (RunAfterAllOthers holds one system per schedule). Give "
                               "'{}' a dependency on what it reads instead.",
                               name, m_RunsLast, name);
            Logger::Log::FlushForCrash(kScheduleAbortFlushTimeout);
            assert(false && "[ECS] RunAfterAllOthers already holds another system — see the preceding error log");
            return *this;
        }
        m_RunsLast = name;
        return *this;
    }

    // Number of registrations held by this builder (deduplicated lazily on the
    // next wave build). Lets callers detect whether a plugin hook actually
    // contributed systems before paying for a schedule re-solve.
    size_t GetRegistrationCount() const { return m_Regs.size(); }

    // ------------------------------------------------------------------
    // C12 owner scopes: bracket ONE plugin's AddSystemsToSchedule call so its
    // registrations carry the plugin id. On a reload replay, the scope end
    // reconciles the owner's set: same-named re-registrations became
    // replacements in Add (newest code, same slot — resolved at the next wave
    // build), and prior registrations the replay did NOT re-declare are
    // marked retired (slot destroyed at the next wave build). Not reentrant;
    // main thread, like the builder itself.
    // ------------------------------------------------------------------
    void BeginOwnedRegistrations(std::string_view owner) {
        if (!m_ActiveOwner.empty())
        {
            Logger::Log::Error("[ECS] BeginOwnedRegistrations('{}') while scope '{}' is open — closing it",
                               owner, m_ActiveOwner);
            EndOwnedRegistrations();
        }
        m_ActiveOwner.assign(owner);
        m_ScopeAddedNames.clear();
        m_ScopePriorNames.clear();
        if (m_ActiveOwner.empty())
            return;
        for (const SystemRegistration& r : m_Regs)
        {
            if (!r.Retired && r.Owner == m_ActiveOwner)
                m_ScopePriorNames.insert(r.Name);
        }
    }

    void EndOwnedRegistrations() {
        if (m_ActiveOwner.empty())
            return;
        for (const std::string& priorName : m_ScopePriorNames)
        {
            if (m_ScopeAddedNames.count(priorName))
                continue; // re-declared — reconciled as a replacement
            for (SystemRegistration& r : m_Regs)
            {
                if (!r.Retired && r.Owner == m_ActiveOwner && r.Name == priorName)
                {
                    r.Retired = true;
                    m_PendingRetirements = true;
                    Logger::Log::Info("[ECS] System '{}' no longer declared by plugin '{}' — retiring at "
                                      "the next schedule build",
                                      priorName, m_ActiveOwner);
                }
            }
        }
        m_ActiveOwner.clear();
        m_ScopeAddedNames.clear();
        m_ScopePriorNames.clear();
    }

    // True when an owner scope marked registrations for retirement that the
    // next BuildAndRegisterWithWaves must apply — callers re-solve even when
    // the registration COUNT did not change (a replay that only removed
    // systems).
    bool HasPendingRetirements() const { return m_PendingRetirements; }

    // C12 unload quiesce ledger: registrations whose instantiated system
    // object still comes from an OLDER generation of `moduleId`. Non-zero
    // blocks unmapping the superseded image (the object's vtable lives there).
    size_t CountSupersededModuleRegistrations(std::string_view moduleId,
                                              std::uint64_t currentGeneration) const {
        if (moduleId.empty())
            return 0;
        size_t stale = 0;
        for (size_t i = 0; i < m_Regs.size(); ++i)
        {
            const SystemRegistration& r = m_Regs[i];
            if (r.Retired)
                continue; // retirement applies at the next build, before any unload decision
            if (r.Module.ModuleId == moduleId && r.Module.Generation < currentGeneration)
            {
                Logger::Log::Warning("[ECS] System '{}' still owned by superseded generation {} of module "
                                     "'{}' (current {})",
                                     r.Name, r.Module.Generation, moduleId, currentGeneration);
                ++stale;
            }
            else if (r.PendingReplace && r.Module.ModuleId == moduleId)
            {
                // The registration already carries the new code but the swap
                // has not been applied to the manager yet — the OLD object is
                // still installed.
                Logger::Log::Warning("[ECS] System '{}' has an unapplied replacement for module '{}' — "
                                     "re-solve the schedule before unloading",
                                     r.Name, moduleId);
                ++stale;
            }
        }
        return stale;
    }

    // C12 load-abort purge: drop registrations stamped with exactly
    // {moduleId, generation}, retiring any instantiated slot. With the
    // loader's registration hold in place an aborted load normally replays
    // nothing (so this finds nothing); it exists for the belt-and-braces
    // paths where schedule registrations were made outside the held replay.
    size_t PurgeModuleRegistrations(ECS::SystemManager& manager, std::string_view moduleId,
                                    std::uint64_t generation) {
        if (moduleId.empty())
            return 0;
        m_ManagerIndices.resize(m_Regs.size(), kNotInstantiated);
        size_t purged = 0;
        size_t kept = 0;
        for (size_t i = 0; i < m_Regs.size(); ++i)
        {
            if (m_Regs[i].Module.Matches(moduleId, generation))
            {
                if (m_ManagerIndices[i] != kNotInstantiated)
                    manager.RetireSystem(m_ManagerIndices[i]);
                Logger::Log::Warning("[ECS] Schedule registration '{}' purged with aborted module '{}' "
                                     "(generation {})",
                                     m_Regs[i].Name, moduleId, generation);
                ++purged;
                continue;
            }
            if (kept != i)
            {
                m_Regs[kept] = std::move(m_Regs[i]);
                m_ManagerIndices[kept] = m_ManagerIndices[i];
            }
            ++kept;
        }
        if (purged > 0)
        {
            m_Regs.resize(kept);
            m_ManagerIndices.resize(kept);
        }
        return purged;
    }

    // Solve the dependency graph over ALL registrations, instantiate any that
    // are not yet in the manager, and install the resulting wave plan. Systems
    // within a wave have no dependencies on each other and can execute in
    // parallel; waves run sequentially (wave 0 completes before wave 1 starts).
    //
    // The first call is the one-shot startup build. Later calls integrate
    // systems registered after startup (package DLLs loaded at project open /
    // player init): the full graph is re-solved, so a late system lands in
    // exactly the wave a from-scratch build would give it — including
    // OptionalDependency edges that were unresolved at startup and only resolve
    // once the package's system names exist. Already-instantiated systems keep
    // their SystemManager index; only new registrations are instantiated.
    //
    // Every REQUIRED dependency must resolve at each build: an unresolved name
    // logs an error in all configs and asserts in dev builds, because the
    // alternative is dropping the edge and letting the systems share a wave.
    // A name a package supplies only when loaded must say so at the declaration
    // site (OptionalDependency); a deliberately partial schedule must say so
    // here (ScheduleCompleteness::Partial).
    //
    // Not thread-safe: call from the thread that owns the SystemManager,
    // between Update() ticks (module loads and plugin registration are
    // main-thread).
    void BuildAndRegisterWithWaves(ECS::SystemManager& manager,
                                   ScheduleCompleteness completeness = ScheduleCompleteness::Complete) {
        m_ManagerIndices.resize(m_Regs.size(), kNotInstantiated);

        // Retirements (C12): registrations an owner's newest replay stopped
        // declaring. The instantiated slot is destroyed (the object's module
        // stays mapped until the unload ledger clears it) and the registration
        // leaves the model — the re-solve below emits no wave entry for it.
        if (m_PendingRetirements)
        {
            m_PendingRetirements = false;
            size_t kept = 0;
            for (size_t i = 0; i < m_Regs.size(); ++i)
            {
                if (m_Regs[i].Retired)
                {
                    if (m_ManagerIndices[i] != kNotInstantiated)
                        manager.RetireSystem(m_ManagerIndices[i]);
                    continue;
                }
                if (kept != i)
                {
                    m_Regs[kept] = std::move(m_Regs[i]);
                    m_ManagerIndices[kept] = m_ManagerIndices[i];
                }
                ++kept;
            }
            m_Regs.resize(kept);
            m_ManagerIndices.resize(kept);
        }

        // Duplicate names: first registration wins and later ones are dropped —
        // UNLESS the later one comes from the SAME owner (plugin id), which is
        // the module-reload replacement (C12): the first registration adopts
        // the replacement's code/metadata and keeps its manager slot, so the
        // swap happens in place and the NEW module's Update runs from the next
        // tick. Only never-instantiated registrations can be dropped — the
        // first occurrence of a name is always the instantiated one.
        {
            std::unordered_map<std::string, size_t> seen; // name -> kept slot
            seen.reserve(m_Regs.size());
            size_t kept = 0;
            for (size_t i = 0; i < m_Regs.size(); ++i)
            {
                if (auto it = seen.find(m_Regs[i].Name); it != seen.end())
                {
                    SystemRegistration& first = m_Regs[it->second];
                    if (!m_Regs[i].Owner.empty() && m_Regs[i].Owner == first.Owner)
                    {
                        first.Phase = m_Regs[i].Phase;
                        first.Order = m_Regs[i].Order;
                        first.Dependencies = std::move(m_Regs[i].Dependencies);
                        first.AddToManager = std::move(m_Regs[i].AddToManager);
                        first.Module = m_Regs[i].Module;
                        first.PendingReplace = m_ManagerIndices[it->second] != kNotInstantiated;
                        Logger::Log::Info("[ECS] System '{}' re-registered by plugin '{}' (module reload) — "
                                          "replacing the instance in place",
                                          first.Name, first.Owner);
                    }
                    else
                    {
                        Logger::Log::Warning("[ECS] Duplicate system name '{}' in scheduler; skipping duplicate registration", m_Regs[i].Name);
                    }
                    continue;
                }
                seen.emplace(m_Regs[i].Name, kept);
                if (kept != i)
                {
                    m_Regs[kept] = std::move(m_Regs[i]);
                    m_ManagerIndices[kept] = m_ManagerIndices[i];
                }
                ++kept;
            }
            m_Regs.resize(kept);
            m_ManagerIndices.resize(kept);
        }

        const size_t count = m_Regs.size();
        std::unordered_map<std::string, size_t> nameToIndex;
        nameToIndex.reserve(count);
        for (size_t i = 0; i < count; ++i)
            nameToIndex[m_Regs[i].Name] = i;

        // Build adjacency + indegree
        std::vector<std::vector<size_t>> adj(count);
        std::vector<int> indeg(count, 0);
        for (size_t i = 0; i < count; ++i)
        {
            for (const SystemDependency& dep : m_Regs[i].Dependencies)
            {
                if (!m_RunsLast.empty() && dep.Name == m_RunsLast)
                {
                    Logger::Log::Error("[ECS] Scheduler: '{}' depends on '{}', which runs after every "
                                       "other system (RunAfterAllOthers); the dependency is dropped, so "
                                       "'{}' runs before it. Remove the dependency.",
                                       m_Regs[i].Name, m_RunsLast, m_Regs[i].Name);
                    continue;
                }
                auto it = nameToIndex.find(dep.Name);
                if (it == nameToIndex.end())
                {
                    ReportUnresolvedDependency(m_Regs[i].Name, dep, nameToIndex, completeness);
                    continue;
                }
                adj[it->second].push_back(i);
                indeg[i]++;
            }
        }
        AddRunsLastEdges(nameToIndex, adj, indeg, completeness);

        // BFS by waves: all indegree-0 nodes form the current wave.
        // Within each wave, sort by (Phase, Order, Name) for determinism.
        std::vector<std::vector<size_t>> waves;
        std::vector<size_t> currentWave;
        for (size_t i = 0; i < count; ++i)
            if (indeg[i] == 0) currentWave.push_back(i);

        size_t totalWaved = 0;
        while (!currentWave.empty())
        {
            std::sort(currentWave.begin(), currentWave.end(), [&](size_t a, size_t b) {
                const auto& rA = m_Regs[a];
                const auto& rB = m_Regs[b];
                return std::make_tuple((int)rA.Phase, rA.Order, rA.Name) <
                       std::make_tuple((int)rB.Phase, rB.Order, rB.Name);
            });

            waves.push_back(currentWave);
            totalWaved += currentWave.size();

            std::vector<size_t> nextWave;
            for (size_t node : currentWave)
            {
                for (size_t succ : adj[node])
                {
                    if (--indeg[succ] == 0)
                        nextWave.push_back(succ);
                }
            }
            currentWave = std::move(nextWave);
        }

        // Instantiate new registrations in wave order (on the first build this
        // makes the manager's sequential order match the plan order), apply
        // pending reload replacements into their existing slots, and emit the
        // plan from the recorded per-registration manager indices.
        const bool firstBuild = !m_BuiltOnce;
        m_BuiltOnce = true;
        size_t newlyInstantiated = 0;
        size_t replaced = 0;
        std::vector<bool> planned(count, false);
        SystemExecutionPlan plan;
        plan.Waves.reserve(waves.size());
        for (const auto& wave : waves)
        {
            SystemExecutionPlan::Wave w;
            for (size_t i : wave)
            {
                if (m_ManagerIndices[i] == kNotInstantiated && m_Regs[i].AddToManager)
                {
                    m_ManagerIndices[i] = m_Regs[i].AddToManager(manager, SystemManager::kAppendSystem);
                    ++newlyInstantiated;
                }
                else if (m_Regs[i].PendingReplace && m_Regs[i].AddToManager)
                {
                    // Reload replacement: construct the NEW module's system into
                    // the existing slot (the old object is destroyed inside
                    // InstallSystem, while its image is still mapped).
                    m_Regs[i].AddToManager(manager, m_ManagerIndices[i]);
                    m_Regs[i].PendingReplace = false;
                    ++replaced;
                }
                if (m_ManagerIndices[i] == kNotInstantiated)
                    continue; // no AddToManager thunk — nothing to run
                planned[i] = true;
                w.SystemIndices.push_back(m_ManagerIndices[i]);
            }
            if (!w.SystemIndices.empty())
                plan.Waves.push_back(std::move(w));
        }

        if (totalWaved != count)
        {
            Logger::Log::Error("[ECS] Scheduler: Cycle detected or missing nodes (added {} of {})",
                               (uint32)totalWaved, (uint32)count);
            // A registration knocked out of the solve by a cycle never gets a
            // wave. If it was already instantiated (a late registration formed
            // a cycle through an existing system), it must not silently stop
            // ticking — run it in a trailing wave until the cycle is fixed.
            for (size_t i = 0; i < count; ++i)
            {
                if (!planned[i] && m_ManagerIndices[i] != kNotInstantiated)
                {
                    plan.Waves.push_back({{m_ManagerIndices[i]}});
                    Logger::Log::Error("[ECS] Scheduler: system '{}' is in an unresolved cycle — "
                                       "kept ticking as a trailing wave",
                                       m_Regs[i].Name);
                }
            }
        }

        manager.SetExecutionPlan(std::move(plan));

        if (!firstBuild)
        {
            Logger::Log::Info("[ECS] Schedule re-solved for late registrations: {} systems ({} new, {} "
                              "replaced in place) across {} waves",
                              (uint32)count, (uint32)newlyInstantiated, (uint32)replaced, (uint32)waves.size());
        }

        // Log the wave structure
        for (size_t wi = 0; wi < waves.size(); ++wi)
        {
            std::string names;
            for (size_t i : waves[wi])
            {
                if (!names.empty()) names += ", ";
                names += m_Regs[i].Name;
            }
            Logger::Log::Debug("[ECS] Wave {}: {}", (uint32)wi, names);
        }
    }


private:
    static constexpr size_t kNotInstantiated = ~size_t{0};

    // The logger is asynchronous: a scheduler error that precedes an assert is
    // drained first, or the abort would discard the line that explains it.
    static constexpr std::chrono::milliseconds kScheduleAbortFlushTimeout{2000};

    // RunAfterAllOthers: an edge from every other registration into the system
    // ordered last.
    void AddRunsLastEdges(const std::unordered_map<std::string, size_t>& nameToIndex,
                          std::vector<std::vector<size_t>>& adj, std::vector<int>& indeg,
                          ScheduleCompleteness completeness) const {
        if (m_RunsLast.empty())
            return;
        const auto last = nameToIndex.find(m_RunsLast);
        if (last == nameToIndex.end())
        {
            if (completeness == ScheduleCompleteness::Complete)
            {
                Logger::Log::Error("[ECS] Scheduler: '{}' is ordered after every other system "
                                   "(RunAfterAllOthers) but no system has that name; register it "
                                   "or remove the ordering",
                                   m_RunsLast);
                Logger::Log::FlushForCrash(kScheduleAbortFlushTimeout);
                assert(false && "[ECS] RunAfterAllOthers names an unregistered system — see the preceding error log");
            }
            return;
        }
        for (size_t i = 0; i < m_Regs.size(); ++i)
        {
            if (i == last->second)
                continue;
            adj[i].push_back(last->second);
            indeg[last->second]++;
        }
    }

    // Case-insensitive containment, both directions: the failure this catches is
    // a name that drifted from the registered one ("Hierarchy" for
    // "TransformHierarchy"), and naming the near-miss turns the error into a
    // one-line fix instead of a scheduler read.
    static std::string SuggestRegisteredNames(
        const std::string& missing, const std::unordered_map<std::string, size_t>& nameToIndex)
    {
        auto lower = [](std::string s) {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string needle = lower(missing);
        std::vector<std::string> hits;
        for (const auto& [name, index] : nameToIndex)
        {
            const std::string candidate = lower(name);
            if (candidate.find(needle) != std::string::npos || needle.find(candidate) != std::string::npos)
                hits.push_back(name);
        }
        if (hits.empty() || hits.size() > 3)
            return {};
        std::sort(hits.begin(), hits.end());
        std::string list;
        for (const std::string& hit : hits)
        {
            if (!list.empty())
                list += "', '";
            list += hit;
        }
        return " Did you mean '" + list + "'?";
    }

    // An unresolved dependency name means the wave solver has no edge to add:
    // the declaring system is free to land in the same wave as the system it
    // was meant to follow, and waves dispatch onto the JobSystem in parallel.
    // Required names therefore fail the build loudly; optional names and
    // deliberately partial schedules are the two declared exceptions.
    static void ReportUnresolvedDependency(const std::string& systemName, const SystemDependency& dep,
                                           const std::unordered_map<std::string, size_t>& nameToIndex,
                                           ScheduleCompleteness completeness)
    {
        if (dep.Optional)
        {
            Logger::Log::Debug("[ECS] Scheduler: optional dependency '{}' of system '{}' is not registered; "
                               "ordering edge inactive in this configuration",
                               dep.Name, systemName);
            return;
        }
        if (completeness == ScheduleCompleteness::Partial)
        {
            Logger::Log::Debug("[ECS] Scheduler: dependency '{}' of system '{}' is not registered in this "
                               "partial schedule; ordering edge inactive",
                               dep.Name, systemName);
            return;
        }
        Logger::Log::Error(
            "[ECS] Scheduler: system '{}' declares a dependency on '{}', but no registered system has that "
            "name. The ordering edge is DROPPED, so '{}' may now run CONCURRENTLY with the system it is "
            "meant to run after — on another worker thread, sharing whatever state the edge was protecting."
            "{} Fix: correct the name to match the registered system exactly, or — only if the system is "
            "genuinely supplied by a package that may not be loaded — declare it as "
            "ECS::OptionalDependency(\"{}\").",
            systemName, dep.Name, systemName, SuggestRegisteredNames(dep.Name, nameToIndex), dep.Name);
        // The logger is asynchronous: without an explicit drain the assert below
        // aborts the process before the sinks receive the very message that
        // explains the abort. Bounded, because an unbounded flush on a path that
        // ends in abort() hangs the process instead whenever the drain thread is
        // blocked behind the sink mutex.
        Logger::Log::FlushForCrash(kScheduleAbortFlushTimeout);
        assert(false && "[ECS] Unresolved required system dependency — see the preceding error log");
    }

    std::vector<SystemRegistration> m_Regs;
    // The system ordered after every other one (RunAfterAllOthers); empty
    // when none is.
    std::string m_RunsLast;
    // Parallel to m_Regs: index of the instantiated system in the
    // SystemManager's sequential list, or kNotInstantiated. Lets wave
    // re-solves emit plans for already-registered systems without
    // re-instantiating them.
    std::vector<size_t> m_ManagerIndices;
    bool m_BuiltOnce = false;

    // C12 owner-scope state (BeginOwnedRegistrations / EndOwnedRegistrations).
    std::string m_ActiveOwner;
    std::unordered_set<std::string> m_ScopePriorNames;
    std::unordered_set<std::string> m_ScopeAddedNames;
    bool m_PendingRetirements = false;
};

} } // namespace GameEngine::ECS

