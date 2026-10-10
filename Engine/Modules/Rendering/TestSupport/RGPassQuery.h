#pragma once

#include "Rendering/Core/RenderGraph/RGGraph.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Test-only pass-name queries over an RGGraph. Pass names are hierarchical and
// gain segments over time, so every name is a prefix of its own future
// children ("GPUDrawStream.Scatter.World" ⊂ "…World.B", "View1" ⊂ "View10")
// and a substring needle silently absorbs siblings ("DebugOverlay" ⊂
// "ClusterDebugOverlay"). These queries make the question explicit and anchor
// the match at segment boundaries:
//   Exact   — pinning ONE pass. The default; reach for it first.
//   Subtree — pinning a FAMILY rooted at the name's start: the root pass
//             and/or its separator-delimited descendants ("GPUCulling",
//             "VolumetricFog", "RenderEntities").
//   Family  — pinning a parameterised family that sits MID-name, where a
//             Pipeline.<bp>.<node>.View<N> prefix the test does not control
//             precedes it ("MsmCascade_Compute_<c>", "Cascade<c>",
//             "HZBBuild.Mip<m>", "Compact").
namespace GameEngine::Testing::RGQuery
{

// The separators the emitters actually use to extend a name:
//   '.'  Pipeline.<bp>.<node>.View<N>.<suffix>, GPUDrawStream.Scatter.World.B
//   '_'  <prefix>.MsmCascade_Compute_<cascade>
//   '['  RenderEntities[<debugName>#<viewId>], VolumetricFog[<viewId>].<stage>
inline constexpr std::string_view kSegmentSeparators = "._[";

// Query objects VIEW their argument — construct them at the call site, never
// store one past the string it was built from.
struct Exact
{
    std::string_view Name;
};

// Root itself, or root immediately followed by a segment separator. "A.B",
// "A_0" and "A[7]" are in the subtree of "A"; "AB" and "A9" are not — which is
// what stops "GPUCulling.View1" from swallowing "View10".
struct Subtree
{
    std::string_view Root;
};

// An occurrence of Base anywhere in the name whose ends both land on segment
// boundaries: the occurrence starts the name or follows a separator, and ends
// the name, meets a separator, or is extended only by a numeric parameter that
// itself ends the segment. So Family{"Cascade"} matches "…View0.Cascade1" and
// Family{"MsmCascade_Compute"} matches "…View0.MsmCascade_Compute_2", while
// "ClusterDebugOverlay" does NOT satisfy Family{"DebugOverlay"} and
// "MsmCascade_Compute_2" does NOT satisfy Family{"Cascade"}.
struct Family
{
    std::string_view Base;
};

inline bool Matches(std::string_view passName, const Exact& q)
{
    return passName == q.Name;
}

inline bool Matches(std::string_view passName, const Subtree& q)
{
    if (passName == q.Root)
        return true;
    return passName.size() > q.Root.size() &&
           passName.compare(0, q.Root.size(), q.Root) == 0 &&
           kSegmentSeparators.find(passName[q.Root.size()]) != std::string_view::npos;
}

inline bool Matches(std::string_view passName, const Family& q)
{
    if (q.Base.empty())
        return false;
    for (std::size_t at = passName.find(q.Base); at != std::string_view::npos;
         at = passName.find(q.Base, at + 1))
    {
        const bool startsSegment =
            at == 0 || kSegmentSeparators.find(passName[at - 1]) != std::string_view::npos;
        if (startsSegment)
        {
            std::size_t end = at + q.Base.size();
            while (end < passName.size() && passName[end] >= '0' && passName[end] <= '9')
                ++end;
            if (end == passName.size() ||
                kSegmentSeparators.find(passName[end]) != std::string_view::npos)
                return true;
        }
    }
    return false;
}

// DECLARED = every pass added to the graph (RGGraph::PassCount — includes
// passes the compile later culls). SCHEDULED = post-cull execution order
// (RGGraph::ScheduledOrder). They are different sets; say which one you mean.

template <class Query>
uint32_t CountDeclared(const Rendering::RenderGraph::RGGraph& g, const Query& q)
{
    uint32_t n = 0;
    for (std::size_t p = 0; p < g.PassCount(); ++p)
    {
        const char* name = g.PassName(static_cast<Rendering::RenderGraph::RGPassId>(p));
        if (name && Matches(name, q))
            ++n;
    }
    return n;
}

// Ids in declaration order, for access/ordering follow-ups on each member.
template <class Query>
std::vector<Rendering::RenderGraph::RGPassId> DeclaredIds(const Rendering::RenderGraph::RGGraph& g,
                                                          const Query& q)
{
    std::vector<Rendering::RenderGraph::RGPassId> ids;
    for (std::size_t p = 0; p < g.PassCount(); ++p)
    {
        const auto id = static_cast<Rendering::RenderGraph::RGPassId>(p);
        const char* name = g.PassName(id);
        if (name && Matches(name, q))
            ids.push_back(id);
    }
    return ids;
}

// First declared match, or kInvalidId. For pinning a pass the caller then
// interrogates (accesses, attachments); assert the id valid before using it.
template <class Query>
Rendering::RenderGraph::RGPassId FindDeclared(const Rendering::RenderGraph::RGGraph& g,
                                              const Query& q)
{
    for (std::size_t p = 0; p < g.PassCount(); ++p)
    {
        const auto id = static_cast<Rendering::RenderGraph::RGPassId>(p);
        const char* name = g.PassName(id);
        if (name && Matches(name, q))
            return id;
    }
    return Rendering::RenderGraph::kInvalidId;
}

// Every declared name matching q, comma-joined — failure diagnostics: a count
// that came out wrong is unreadable without the set it counted over.
template <class Query>
std::string DeclaredNames(const Rendering::RenderGraph::RGGraph& g, const Query& q)
{
    std::string out;
    for (std::size_t p = 0; p < g.PassCount(); ++p)
    {
        const char* name = g.PassName(static_cast<Rendering::RenderGraph::RGPassId>(p));
        if (!name || !Matches(name, q))
            continue;
        if (!out.empty())
            out += ", ";
        out += name;
    }
    return out.empty() ? std::string("<none>") : out;
}

// Index into ScheduledOrder() of the FIRST scheduled match, or nullopt.
// Presence here is evidence a pass survived cull; presence in the declared
// set is only evidence it was added.
template <class Query>
std::optional<std::size_t> ScheduledIndex(const Rendering::RenderGraph::RGGraph& g, const Query& q)
{
    const auto& order = g.ScheduledOrder();
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        const char* name = g.PassName(order[i]);
        if (name && Matches(name, q))
            return i;
    }
    return std::nullopt;
}

// Snapshot overloads, for fixtures that outlive the frame and keep name
// vectors (HeadlessViewFixture::PassNames / ScheduledPassNames).

template <class Query>
uint32_t CountIn(std::span<const std::string> names, const Query& q)
{
    uint32_t n = 0;
    for (const std::string& name : names)
        if (Matches(name, q))
            ++n;
    return n;
}

template <class Query>
std::optional<std::size_t> IndexIn(std::span<const std::string> names, const Query& q)
{
    for (std::size_t i = 0; i < names.size(); ++i)
        if (Matches(names[i], q))
            return i;
    return std::nullopt;
}

} // namespace GameEngine::Testing::RGQuery
