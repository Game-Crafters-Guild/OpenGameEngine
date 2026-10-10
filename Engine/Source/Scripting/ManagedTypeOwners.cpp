#include "Scripting/ManagedTypeOwners.h"

#include "Logger/Logger.h"

#include <random>

namespace GameEngine::Scripting
{

namespace
{
// Seeded once from the platform entropy source. A counter would make every live owner id
// trivially guessable from outside, which is the one attack validation cannot answer.
std::uint64_t NextCandidate()
{
    static std::mt19937_64 s_Rng{std::random_device{}()};
    static std::mutex s_RngMutex;
    std::lock_guard<std::mutex> lock(s_RngMutex);
    return s_Rng();
}

const char* EntryPointName(OwnerEntryPoint entryPoint)
{
    switch (entryPoint)
    {
    case OwnerEntryPoint::ReleaseTypeOwner:
        return "GE_UI_ReleaseTypeOwner";
    case OwnerEntryPoint::RegisterElementType:
        return "GE_UI_RegisterElementType";
    case OwnerEntryPoint::OrphanElementTypes:
        return "GE_UI_OrphanElementTypes";
    case OwnerEntryPoint::NotifyReloadCompleted:
        return "GE_UI_NotifyReloadCompleted";
    }
    return "GE_UI_<unknown>";
}

// (owner, entry point) folded into one key for the dedup set.
std::uint64_t ReportKey(std::uint64_t owner, OwnerEntryPoint entryPoint)
{
    return owner ^ (static_cast<std::uint64_t>(entryPoint) + 0x9E3779B97F4A7C15ull);
}
} // namespace

ManagedTypeOwners& ManagedTypeOwners::Instance()
{
    // Never destroyed, matching ManagedElementTypes: the unload seam can run late, and a
    // registry destroyed during static teardown would be consulted by a context that outlived
    // it.
    static ManagedTypeOwners* s_instance = new ManagedTypeOwners();
    return *s_instance;
}

std::uint64_t ManagedTypeOwners::Acquire()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (;;)
    {
        const std::uint64_t candidate = NextCandidate();
        // 0 is the engine and is refused everywhere downstream, so it must never be minted.
        if (candidate == 0)
            continue;
        if (m_Live.count(candidate) != 0 || m_Retired.count(candidate) != 0)
            continue;
        m_Live.insert(candidate);
        return candidate;
    }
}

bool ManagedTypeOwners::Release(std::uint64_t owner)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Live.erase(owner) == 0)
        return false;
    m_Retired.insert(owner);
    return true;
}

bool ManagedTypeOwners::IsLive(std::uint64_t owner) const
{
    if (owner == 0)
        return false;
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Live.count(owner) != 0;
}

bool ManagedTypeOwners::ValidateOwner(std::uint64_t owner, OwnerEntryPoint entryPoint)
{
    if (IsLive(owner))
        return true;
    ReportRejected(owner, entryPoint);
    return false;
}

void ManagedTypeOwners::ReportRejected(std::uint64_t owner, OwnerEntryPoint entryPoint)
{
    bool report = false;
    bool budgetJustSpent = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        ++m_RejectedCalls;
        report = ShouldReportLocked(owner, entryPoint);
        budgetJustSpent = report && m_Reported.size() == kMaxReportedRejections;
    }

    // Logged outside the lock. A sink is free to do arbitrary work, and this registry is
    // consulted from the ALC unload seam on an arbitrary thread — holding its mutex across a
    // call it does not control is how a diagnostic becomes a deadlock.
    if (report)
    {
        Logger::Log::Warning(
            "UI: {} refused owner id {} — that id is not a live managed load-context owner. It "
            "was never minted, or its context already released it. A load context must pass back "
            "the id GE_UI_AcquireTypeOwner returned to it.",
            EntryPointName(entryPoint), owner);
    }
    if (budgetJustSpent)
    {
        Logger::Log::Warning("UI: further managed element-type owner refusals will be counted but "
                             "no longer logged ({} distinct call sites reported).",
                             kMaxReportedRejections);
    }
}

bool ManagedTypeOwners::ShouldReportLocked(std::uint64_t owner, OwnerEntryPoint entryPoint)
{
    if (m_Reported.size() >= kMaxReportedRejections)
        return false;
    return m_Reported.insert(ReportKey(owner, entryPoint)).second;
}

std::size_t ManagedTypeOwners::RejectedCallCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_RejectedCalls;
}

std::size_t ManagedTypeOwners::LiveCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Live.size();
}

void ManagedTypeOwners::ResetForTests()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Live.clear();
    m_Reported.clear();
    m_RejectedCalls = 0;
    // Retired ids are deliberately NOT cleared: a suite that recycled one would be asserting
    // against a property the process does not have.
}

} // namespace GameEngine::Scripting
