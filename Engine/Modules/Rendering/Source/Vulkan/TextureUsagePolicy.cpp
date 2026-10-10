#include "TextureUsagePolicy.h"

#include "Logger/Logger.h"
#include "VulkanHandleHelpers.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Rendering
{
namespace
{

const char* UsageBitName(TextureUsage usage)
{
    switch (usage)
    {
    case TextureUsage::ShaderResource:
        return "ShaderResource";
    case TextureUsage::RenderTarget:
        return "RenderTarget";
    case TextureUsage::DepthStencil:
        return "DepthStencil";
    case TextureUsage::UnorderedAccess:
        return "UnorderedAccess";
    case TextureUsage::TransferSrc:
        return "TransferSrc";
    case TextureUsage::TransferDst:
        return "TransferDst";
    default:
        return "?";
    }
}

std::string FormatUsage(TextureUsage usage)
{
    if (usage == TextureUsage::None)
    {
        return "None";
    }
    static constexpr TextureUsage kBits[] = {
        TextureUsage::ShaderResource, TextureUsage::RenderTarget, TextureUsage::DepthStencil,
        TextureUsage::UnorderedAccess, TextureUsage::TransferSrc, TextureUsage::TransferDst};

    std::string out;
    for (TextureUsage bit : kBits)
    {
        if (static_cast<uint32_t>(usage & bit) == 0)
        {
            continue;
        }
        if (!out.empty())
        {
            out += '|';
        }
        out += UsageBitName(bit);
    }
    const uint32_t known = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                           static_cast<uint32_t>(TextureUsage::RenderTarget) |
                           static_cast<uint32_t>(TextureUsage::DepthStencil) |
                           static_cast<uint32_t>(TextureUsage::UnorderedAccess) |
                           static_cast<uint32_t>(TextureUsage::TransferSrc) |
                           static_cast<uint32_t>(TextureUsage::TransferDst);
    if (const uint32_t leftover = static_cast<uint32_t>(usage) & ~known)
    {
        out += "|0x" + std::to_string(leftover);
    }
    return out;
}

struct Violation
{
    std::string DebugName;
    TextureUsage Declared = TextureUsage::None;
    TextureUsage Required = TextureUsage::None;
    uint32_t Format = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Depth = 0;
    uint32_t MipLevels = 1;
    uint32_t ArrayLayers = 1;
    uint64_t Count = 0;
    std::vector<std::string> Operations;
};

struct AuditState
{
    std::mutex Mutex;
    std::unordered_map<std::string, Violation> Violations;
    std::unordered_map<std::string, uint64_t> ObservedByOperation;
    uint64_t ObservedTotal = 0;
    uint64_t UnresolvedUses = 0;
    // What the last summary line already said, so a later report that would repeat it
    // stays silent. Guarded by Mutex with the rest of the state.
    size_t SummarizedViolations = 0;
    uint64_t SummarizedObserved = 0;
    bool Summarized = false;
};

// The summary repeats only when the reader would learn something: a violation the last
// summary did not carry, or an observed count an order of magnitude past the one it did.
// Report() runs at every device teardown, so an unconditional repeat is hundreds of
// lines and a strict once is a first-teardown snapshot pinned to whatever that device
// happened to have seen — in a process that builds a device per test, single digits for
// a run that goes on to observe tens of thousands of uses.
constexpr uint64_t kObservedGrowthFactor = 10;

bool ShouldSummarize(const AuditState& state)
{
    if (!state.Summarized)
    {
        return true;
    }
    if (state.Violations.size() > state.SummarizedViolations)
    {
        return true;
    }
    return state.ObservedTotal >= std::max<uint64_t>(state.SummarizedObserved * kObservedGrowthFactor, 1);
}

AuditState& State()
{
    static AuditState state;
    return state;
}

bool EnvIsTrue(const char* name, bool valueWhenUnset)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return valueWhenUnset;
    }
    return std::strcmp(value, "0") != 0;
}

bool ResolveAuditEnabled()
{
    // Developer configs detect by default: the drift is cheap to see the day it
    // is written and expensive to find in a port. Release keeps the hooks but
    // stays quiet unless asked, so a platform the dev configs cannot reach can
    // still be harvested.
#if defined(GE_DEV_DIAG)
    constexpr bool kDefault = true;
#else
    constexpr bool kDefault = false;
#endif
    return EnvIsTrue("GE_TEXTURE_USAGE_AUDIT", kDefault);
}

// The per-operation tally is diagnostic detail, and Report() runs once per device
// teardown over process-global state: a host that builds a device per test reprints
// the same growing tally hundreds of times.
//
// Where Debug leaves it depends on the host. The editor and the player set the global
// minimum to Debug and let the file sink filter, so there it is one GE_LOG_FILE_LEVEL
// away. A test executable has no such control: its global minimum is whatever some test
// last passed to Logger::Log::Initialize and no environment variable moves it, so a test
// that wants the tally sets that level itself, or reads Snapshot() — the same numbers
// without the log round trip, and what the tests here assert on.
void EmitDetail(const std::string& line)
{
    Logger::Log::Debug(line);
}

// The one line a reader who did not ask for the detail still needs.
void EmitSummary(const std::string& line)
{
    Logger::Log::Info(line);
}

// A violation is a defect, not a statistic: it warns the first time it is seen so
// it surfaces in a normal developer run rather than only in the exit summary.
void EmitViolation(const std::string& line)
{
    Logger::Log::Warning(line);
}

} // namespace

bool TextureUsagePolicy::IsAuditEnabled()
{
    static const bool enabled = ResolveAuditEnabled();
    return enabled;
}

bool TextureUsagePolicy::IsStrict()
{
    static const bool strict = EnvIsTrue("GE_STRICT_TEXTURE_USAGE", false);
    return strict;
}

void TextureUsagePolicy::RecordTransferUse(TextureHandle handle, TextureUsage requiredUsage, const char* operation)
{
    if (!IsAuditEnabled())
    {
        return;
    }

    AuditState& state = State();
    const VulkanTexture* texture = GetVulkanTextureConst(handle);

    std::lock_guard<std::mutex> lock(state.Mutex);
    ++state.ObservedTotal;
    ++state.ObservedByOperation[operation];

    if (texture == nullptr)
    {
        // Swapchain images carry synthetic handles that the TextureManager does
        // not back, and their usage comes from swapchain creation rather than a
        // TextureDesc — there is no declaration site to audit.
        ++state.UnresolvedUses;
        return;
    }

    if (static_cast<uint32_t>(texture->usage & requiredUsage) != 0)
    {
        return;
    }

    const std::string name = texture->debugName.empty() ? std::string("<unnamed>") : texture->debugName;
    const std::string key = name + '#' + UsageBitName(requiredUsage);

    auto [it, inserted] = state.Violations.try_emplace(key);
    Violation& violation = it->second;
    if (inserted)
    {
        violation.DebugName = name;
        violation.Declared = texture->usage;
        violation.Required = requiredUsage;
        violation.Format = static_cast<uint32_t>(texture->format);
        violation.Width = texture->extent.width;
        violation.Height = texture->extent.height;
        violation.Depth = texture->extent.depth;
        violation.MipLevels = texture->mipLevels;
        violation.ArrayLayers = texture->arrayLayers;
    }
    ++violation.Count;
    if (std::find(violation.Operations.begin(), violation.Operations.end(), operation) == violation.Operations.end())
    {
        violation.Operations.emplace_back(operation);
    }

    if (inserted)
    {
        EmitViolation(std::format(
            "[TextureUsagePolicy] '{}' is used as {} by {} but declares only {} — add it to the TextureDesc ({}x{}x{}, {} mips, {} layers, VkFormat {})",
            violation.DebugName,
            UsageBitName(requiredUsage),
            operation,
            FormatUsage(violation.Declared),
            violation.Width,
            violation.Height,
            violation.Depth,
            violation.MipLevels,
            violation.ArrayLayers,
            violation.Format));
    }
}

TextureUsageSnapshot TextureUsagePolicy::Snapshot()
{
    AuditState& state = State();
    std::lock_guard<std::mutex> lock(state.Mutex);

    TextureUsageSnapshot snapshot;
    snapshot.ObservedUses = state.ObservedTotal;
    snapshot.UnresolvedUses = state.UnresolvedUses;
    snapshot.ViolationKeys.reserve(state.Violations.size());
    for (const auto& [key, violation] : state.Violations)
    {
        snapshot.ViolationKeys.push_back(key);
    }
    std::sort(snapshot.ViolationKeys.begin(), snapshot.ViolationKeys.end());
    return snapshot;
}

void TextureUsagePolicy::Report()
{
    if (!IsAuditEnabled())
    {
        return;
    }

    AuditState& state = State();
    std::lock_guard<std::mutex> lock(state.Mutex);

    // The audit accumulates for the whole process but is reported per device teardown,
    // so every report after the first repeats what the reader already has.
    if (ShouldSummarize(state))
    {
        state.Summarized = true;
        state.SummarizedViolations = state.Violations.size();
        state.SummarizedObserved = state.ObservedTotal;
        EmitSummary(std::format(
            "[TextureUsagePolicy] transfer-usage audit: {} transfer uses observed, {} texture/usage-bit "
            "violations (detail at Debug)",
            state.ObservedTotal,
            state.Violations.size()));
    }

    EmitDetail("[TextureUsagePolicy] ===== transfer-usage harvest =====");
    EmitDetail(std::format("[TextureUsagePolicy] observed transfer uses: {} ({} on textures with no TextureDesc declaration)",
                           state.ObservedTotal,
                           state.UnresolvedUses));

    std::vector<std::pair<std::string, uint64_t>> operations(state.ObservedByOperation.begin(),
                                                             state.ObservedByOperation.end());
    std::sort(operations.begin(), operations.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [operation, count] : operations)
    {
        EmitDetail(std::format("[TextureUsagePolicy] observed  {:<40} {}", operation, count));
    }

    if (state.Violations.empty())
    {
        EmitDetail("[TextureUsagePolicy] violations: none");
        EmitDetail("[TextureUsagePolicy] ===== end harvest =====");
        return;
    }

    std::vector<const Violation*> sorted;
    sorted.reserve(state.Violations.size());
    for (const auto& [key, violation] : state.Violations)
    {
        sorted.push_back(&violation);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const Violation* a, const Violation* b) { return a->Count > b->Count; });

    EmitDetail(std::format("[TextureUsagePolicy] violations: {} (texture x missing bit)", sorted.size()));
    for (const Violation* violation : sorted)
    {
        std::string operationList;
        for (const std::string& operation : violation->Operations)
        {
            if (!operationList.empty())
            {
                operationList += ", ";
            }
            operationList += operation;
        }
        EmitDetail(std::format("[TextureUsagePolicy] violation  name='{}' missing={} declared={} uses={} size={}x{}x{} mips={} layers={} vkFormat={} ops=[{}]",
                         violation->DebugName,
                         UsageBitName(violation->Required),
                         FormatUsage(violation->Declared),
                         violation->Count,
                         violation->Width,
                         violation->Height,
                         violation->Depth,
                         violation->MipLevels,
                         violation->ArrayLayers,
                         violation->Format,
                         operationList));
    }
    EmitDetail("[TextureUsagePolicy] ===== end harvest =====");
}

} // namespace Rendering
} // namespace GameEngine
