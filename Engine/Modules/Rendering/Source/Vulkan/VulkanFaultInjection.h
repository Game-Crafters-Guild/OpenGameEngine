#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// Device-lost / hung fault injection (Q6 slice 5, design §10). A device-layer
// hook — Vulkan validation layers offer no device-fault injection, so these env
// vars are the only way to drive the recovery machinery deterministically.
//
//   GE_VK_FORCE_DEVICE_LOST=<N>          classify the graphics submit on
//                                        BeginFrame #N as VK_ERROR_DEVICE_LOST
//   GE_VK_FORCE_DEVICE_LOST=<N1,N2,...>  same, but re-arm at each later (increasing)
//                                        frame after recovery — repeat-loss / re-entry
//   GE_VK_FORCE_DEVICE_LOST=submit:<N>   classify the Nth graphics submit as lost
//   GE_VK_FORCE_BEGINFRAME_TIMEOUT=<N>   synthesize one BeginFrame fence timeout
//                                        on frame #N — exercises the Hung path on
//                                        any platform, including Windows where the
//                                        real fence wait is infinite (test-only).
//   GE_VK_FORCE_GPU_HANG=<N>             submit a genuinely non-terminating compute
//                                        dispatch on frame #N — a REAL Windows TDR
//                                        (adapter reset -> genuine VK_ERROR_DEVICE_LOST),
//                                        unlike the fake-submit hooks above which
//                                        classify a still-healthy submit. Exercises
//                                        recovery against a truly reset device.
//
// Zero cost unless configured: Active() gates every query and the injecting
// sites maintain their frame/submit counters only while Active(). The config is
// parsed once at device Initialize and cached; no env read on any hot path.
// Frame and submit ordinals are 1-based (first BeginFrame / first graphics
// submit is #1), so a target of 0 (unset / malformed) never fires.
struct VulkanFaultInjection
{
    enum class LostMode : uint8_t
    {
        None,
        AtFrame,
        AtSubmit
    };

    LostMode lostMode = LostMode::None;
    uint64_t lostTarget = 0;
    // Additional AtFrame targets (GE_VK_FORCE_DEVICE_LOST=N1,N2,...): after the loss
    // at lostTarget fires + recovers, the next queued target arms. Enables repeat-loss
    // (re-entry) testing in one process. Must be strictly increasing — the frame
    // counter climbs monotonically (it is never reset by a rebuild).
    std::vector<uint64_t> lostExtraTargets;
    bool timeoutEnabled = false;
    uint64_t timeoutFrame = 0;
    // Number of consecutive frames to force the BeginFrame timeout (GE_VK_FORCE_BEGINFRAME_TIMEOUT=N:K).
    // K>1 sustains the Hung state across frames so the >250ms "GPU busy" surfacing / editor
    // recovery toast can be observed before it resumes. Default 1 (single-frame Hung).
    uint64_t timeoutFrames = 1;
    bool hangEnabled = false;
    uint64_t hangFrame = 0;

    bool Active() const noexcept
    {
        return lostMode != LostMode::None || timeoutEnabled || hangEnabled;
    }

    // Frame mode fires on the first graphics submit at or after the target frame
    // ("next submit on frame N"): a frame can legitimately lack a graphics submit
    // (acquire-skipped / compute-only), so an exact match would miss. The loss
    // then latches, so it fires exactly once regardless.
    bool ShouldForceLostAtFrame(uint64_t frameOrdinal) const noexcept
    {
        return lostMode == LostMode::AtFrame && frameOrdinal >= lostTarget;
    }
    bool ShouldForceLostAtSubmit(uint64_t submitOrdinal) const noexcept
    {
        return lostMode == LostMode::AtSubmit && submitOrdinal == lostTarget;
    }
    bool ShouldForceTimeoutAtFrame(uint64_t frameOrdinal) const noexcept
    {
        return timeoutEnabled && frameOrdinal >= timeoutFrame &&
               frameOrdinal < timeoutFrame + (timeoutFrames == 0 ? 1 : timeoutFrames);
    }
    // Fires on the first frame at or after the target (mirrors the AtFrame loss
    // rule: a frame may lack a graphics submit, so an exact match could miss).
    bool ShouldForceGpuHangAtFrame(uint64_t frameOrdinal) const noexcept
    {
        return hangEnabled && frameOrdinal >= hangFrame;
    }
    // Consume the fired AtFrame/AtSubmit target: arm the next queued frame target if
    // any (repeat-loss), else disarm. Called once per fired loss.
    void ConsumeLostTarget() noexcept
    {
        if (lostMode == LostMode::AtFrame && !lostExtraTargets.empty())
        {
            lostTarget = lostExtraTargets.front();
            lostExtraTargets.erase(lostExtraTargets.begin());
        }
        else
        {
            lostMode = LostMode::None;
        }
    }
};

// Pure parse (unit-testable). forceLost / forceTimeout are the raw env strings
// (nullptr when unset). A parsed ordinal of 0 disables that hook.
inline VulkanFaultInjection ParseFaultInjection(const char* forceLost, const char* forceTimeout,
                                                const char* forceHang = nullptr)
{
    VulkanFaultInjection cfg;
    if (forceLost != nullptr && forceLost[0] != '\0')
    {
        static const char* kSubmitPrefix = "submit:";
        const size_t prefixLen = std::strlen(kSubmitPrefix);
        if (std::strncmp(forceLost, kSubmitPrefix, prefixLen) == 0)
        {
            const uint64_t n = std::strtoull(forceLost + prefixLen, nullptr, 10);
            if (n > 0)
            {
                cfg.lostMode = VulkanFaultInjection::LostMode::AtSubmit;
                cfg.lostTarget = n;
            }
        }
        else
        {
            // Comma-separated frame targets (N1,N2,...): first arms the loss, the rest
            // queue for repeat-loss (re-entry) testing. Non-positive / malformed entries
            // are skipped; a strictly-increasing sequence is expected.
            const char* p = forceLost;
            char* end = nullptr;
            bool first = true;
            for (;;)
            {
                const uint64_t n = std::strtoull(p, &end, 10);
                if (end != p && n > 0)
                {
                    if (first)
                    {
                        cfg.lostMode = VulkanFaultInjection::LostMode::AtFrame;
                        cfg.lostTarget = n;
                        first = false;
                    }
                    else
                    {
                        cfg.lostExtraTargets.push_back(n);
                    }
                }
                if (end == nullptr || *end == '\0')
                    break;
                p = end + 1; // skip the comma (or any single separator)
            }
        }
    }
    if (forceTimeout != nullptr && forceTimeout[0] != '\0')
    {
        // "N" or "N:K" — K consecutive frames of forced timeout (sustained Hung).
        char* end = nullptr;
        const uint64_t n = std::strtoull(forceTimeout, &end, 10);
        if (n > 0)
        {
            cfg.timeoutEnabled = true;
            cfg.timeoutFrame = n;
            if (end != nullptr && *end == ':')
            {
                const uint64_t k = std::strtoull(end + 1, nullptr, 10);
                if (k > 0)
                    cfg.timeoutFrames = k;
            }
        }
    }
    if (forceHang != nullptr && forceHang[0] != '\0')
    {
        const uint64_t n = std::strtoull(forceHang, nullptr, 10);
        if (n > 0)
        {
            cfg.hangEnabled = true;
            cfg.hangFrame = n;
        }
    }
    return cfg;
}

inline VulkanFaultInjection FaultInjectionFromEnv()
{
    return ParseFaultInjection(std::getenv("GE_VK_FORCE_DEVICE_LOST"),
                               std::getenv("GE_VK_FORCE_BEGINFRAME_TIMEOUT"),
                               std::getenv("GE_VK_FORCE_GPU_HANG"));
}

} // namespace Rendering
} // namespace GameEngine
