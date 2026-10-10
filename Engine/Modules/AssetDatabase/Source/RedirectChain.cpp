#include "AssetDatabase/RedirectChain.h"

#include "AssetDatabase/AssetRecord.h"
#include "AssetDatabase/IAssetStore.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::AssetDatabase
{

namespace
{

// A cycle is a persistent defect in the stored redirect graph, so every
// resolve that crosses it would otherwise log. One advisory per interval
// carries the same information without drowning the log.
constexpr std::chrono::steady_clock::duration kCycleWarnInterval = std::chrono::seconds(30);

std::atomic<std::chrono::steady_clock::rep> g_NextCycleWarnTick{0};

bool ClaimCycleWarnSlot()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto next = g_NextCycleWarnTick.load(std::memory_order_relaxed);
    if (now < next)
        return false;
    return g_NextCycleWarnTick.compare_exchange_strong(
        next, now + kCycleWarnInterval.count(), std::memory_order_relaxed);
}

void WarnAboutRedirectCycle(const GUID* visited, int visitedCount, const GUID& repeated)
{
    if (!ClaimCycleWarnSlot())
        return;
    std::string members;
    for (int i = 0; i < visitedCount; ++i)
    {
        if (i > 0)
            members += " -> ";
        members += visited[i].ToString();
    }
    members += " -> ";
    members += repeated.ToString();
    Logger::Log::Warning(
        "AssetDatabase: redirect cycle detected ({}); resolving to the queried GUID {}. "
        "References through these GUIDs cannot heal until one of the redirects is removed.",
        members, visited[0].ToString());
}

} // namespace

RedirectChain ChaseRedirectChain(const IAssetStore& store,
                                 const GUID& from,
                                 RedirectTargetCheck check,
                                 const RedirectResidentProbe& residentProbe)
{
    RedirectChain chain{};
    chain.Final = from;
    if (from.IsNull())
        return chain;

    // One slot per hop plus the queried GUID. Linear search over at most nine
    // entries beats a set here: the chase runs on every metadata lookup in a
    // project that has any redirects at all.
    GUID visited[kMaxRedirectChainDepth + 1];
    int visitedCount = 0;
    visited[visitedCount++] = from;

    GUID current = from;
    for (int depth = 0; depth < kMaxRedirectChainDepth; ++depth)
    {
        const std::optional<GUID> next = store.ResolveRedirect(current);
        if (!next || next->IsNull() || *next == current)
            break;
        if (std::find(visited, visited + visitedCount, *next) != visited + visitedCount)
        {
            chain.CycleDetected = true;
            chain.Final = from;
            WarnAboutRedirectCycle(visited, visitedCount, *next);
            return chain;
        }
        current = *next;
        visited[visitedCount++] = current;
    }

    chain.Final = current;
    chain.Redirected = current != from;
    if (!chain.Redirected)
        return chain;

    switch (check)
    {
    case RedirectTargetCheck::None:
        chain.TargetAccepted = true;
        break;
    case RedirectTargetCheck::StoreRecord:
    {
        AssetRecord targetRecord{};
        chain.TargetAccepted = store.TryGetAsset(chain.Final, targetRecord);
        break;
    }
    case RedirectTargetCheck::ResidentRecord:
        chain.TargetAccepted = residentProbe && residentProbe(chain.Final);
        break;
    }
    return chain;
}

size_t RetargetIncomingRedirects(IAssetStore& store, std::span<const GUID> sources)
{
    // Every chain-final target is captured before a single hop is rewritten,
    // so a source whose own hop points at another source in this batch still
    // inherits the target it had on entry.
    std::vector<std::pair<GUID, GUID>> inherited;
    inherited.reserve(sources.size());
    for (const GUID& source : sources)
    {
        const RedirectChain chain =
            ChaseRedirectChain(store, source, RedirectTargetCheck::None, {});
        if (chain.Redirected)
            inherited.emplace_back(source, chain.Final);
    }
    if (inherited.empty())
        return 0;

    size_t retargeted = 0;
    for (const RedirectRecord& record : store.EnumerateRedirects())
    {
        const auto it = std::find_if(inherited.begin(), inherited.end(),
                                     [&](const std::pair<GUID, GUID>& entry)
                                     { return entry.first == record.to; });
        if (it == inherited.end())
            continue;
        // Unreachable for an acyclic graph — an incoming hop sourced at the
        // chain-final target would close a cycle, and a cyclic chase reports
        // no redirection at all. Guarded because AddRedirect would reject the
        // self-redirect and leave the hop pointing at a GUID about to lose its
        // own redirect.
        if (record.from == it->second)
            continue;
        if (store.AddRedirect(record.from, it->second, nullptr))
            ++retargeted;
    }
    return retargeted;
}

} // namespace GameEngine::AssetDatabase
