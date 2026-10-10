#pragma once

#include "Rendering/Core/Device.h" // DeviceHealth

#include <atomic>
#include <chrono>

namespace GameEngine
{
namespace Rendering
{

// GE_DEVICE_RECOVERY kill switch (design §12). "0" restores today's behavior
// (latch + suppress, no poll-resume / no rebuild machinery); unset or any other
// value leaves recovery enabled. Pure so it is unit-testable without touching the
// process environment; the once-per-process read wraps this in VulkanDevice.
inline bool ParseDeviceRecoveryEnabled(const char* envValue) noexcept
{
    return envValue == nullptr || envValue[0] != '0';
}

// Device-health tri-state machine (Q6 device-lost recovery, design §7 Tier 0).
//
// Owns the atomic DeviceHealth and the legal transitions between Healthy, Hung
// and Lost. Deliberately free of any Vulkan handle or logging dependency so the
// transition rules are unit-testable without a GPU (see DeviceRecoveryTests):
// the VulkanDevice sites classify their VkResult, drive the transition here, and
// perform I/O (logging, marker dump) only on the edge this reports as fired.
//
// Load() is a single relaxed atomic read. The render path reads it once per
// window per frame, so a healthy steady state pays nothing beyond a plain load.
class DeviceHealthState
{
  public:
    using Clock = std::chrono::steady_clock;

    DeviceHealth Load() const noexcept { return m_Health.load(std::memory_order_relaxed); }
    bool IsHealthy() const noexcept { return Load() == DeviceHealth::Healthy; }
    bool IsHung() const noexcept { return Load() == DeviceHealth::Hung; }
    bool IsLost() const noexcept { return Load() == DeviceHealth::Lost; }
    bool IsRebuilding() const noexcept { return Load() == DeviceHealth::Rebuilding; }
    bool IsAwaitingReprovision() const noexcept { return Load() == DeviceHealth::AwaitingReprovision; }
    bool IsFailed() const noexcept { return Load() == DeviceHealth::Failed; }

    // Device is functional for direct GPU ops (buffer create/upload, submit,
    // fence) — Healthy or AwaitingReprovision. Distinct from IsHealthy(), which is
    // "safe to render a frame". Drives shutdown-wait and upload gating: those must
    // still run normally after a rebuild even though rendering stays suppressed
    // until re-provision.
    bool IsDeviceUsable() const noexcept
    {
        const DeviceHealth h = Load();
        return h == DeviceHealth::Healthy || h == DeviceHealth::AwaitingReprovision;
    }

    // Any state except Failed -> Lost. Failed is terminal (a stray loss must not
    // revive it into another rebuild). Returns true only on the first edge into
    // Lost, so the caller logs / dumps markers exactly once.
    //
    // The recover-relose cycle count is updated on that edge: a recovery that
    // survived `stickWindow` held, so the count restarts; a loss sooner than that
    // is another turn of a loop the rebuild is not fixing. RebuildAttempts() cannot
    // measure this — NoteReprovisioned() zeroes it on every successful recovery, so
    // a device that recovers cleanly and re-loses never spends its retry budget.
    bool TransitionToLost(Clock::time_point now, Clock::duration stickWindow) noexcept;

    // Healthy -> Hung on a finite-wait fence timeout, stamping the entry time so
    // the escalation cap can be measured. Returns true on the edge. A device
    // already Lost stays Lost (a timeout cannot un-lose it); an already Hung
    // device stays Hung (its original entry time is kept).
    bool TransitionToHung(Clock::time_point now) noexcept;

    // A fence became signaled while Hung: Hung -> Healthy. Returns true on the
    // edge. No-op (returns false) when already Healthy or Lost.
    bool NoteFenceSignaled() noexcept;

    // Milliseconds spent in Hung as of `now` (0 when not Hung). Drives the
    // "GPU busy" surfacing threshold. Render-thread-only (reads m_HungSince).
    double HungElapsedMs(Clock::time_point now) const noexcept;

    // True when Hung and the time in Hung has reached the escalation cap; the
    // caller then treats the hang as a device loss. Render-thread-only.
    bool ShouldEscalate(Clock::time_point now, Clock::duration cap) const noexcept;

    // Device (re)initialized: force back to Healthy and clear both budgets.
    void Reset() noexcept
    {
        m_Health.store(DeviceHealth::Healthy, std::memory_order_relaxed);
        m_RebuildAttempts = 0;
        m_RecoveryCycles = 0;
        m_LastRecoveryTime = {};
        m_HasRecovered = false;
    }

    // --- Tier-2 rebuild (Q6 slice 2) ----------------------------------------
    // These run only on the render thread (the rebuild is driven from BeginFrame
    // at a frame boundary), so they need no atomicity beyond the m_Health store.

    // Lost -> Rebuilding, incrementing the attempt counter. Returns true on the
    // edge; false if not currently Lost (a rebuild is only entered from a latched
    // loss, and never re-entered while already Rebuilding).
    bool BeginRebuild() noexcept;

    // Rebuilding -> AwaitingReprovision after the device came back functional.
    // Returns true on the edge. The attempt counter is intentionally NOT reset
    // here — it clears only once the device is fully recovered (NoteReprovisioned),
    // so a device that rebuilds but keeps dying still trips the retry cap (M3).
    bool NoteRebuildSucceeded() noexcept;

    // Rebuilding -> Lost after a failed rebuild attempt, so the next BeginFrame
    // retries. Returns true on the edge. The cap is enforced by the caller via
    // RebuildAttempts() + TransitionToFailed().
    bool NoteRebuildFailed() noexcept;

    // AwaitingReprovision -> Healthy once the upper layers re-provisioned (slice
    // 3a). Clears the attempt counter and stamps `now` as the moment this recovery
    // completed; the next loss measures its stick window against that stamp.
    // Returns true on the edge.
    bool NoteReprovisioned(Clock::time_point now) noexcept;

    // Lost | Rebuilding -> Failed (bounded-retry budget exhausted). Returns true
    // on the edge. Terminal: only Reset() (a fresh Initialize) leaves Failed.
    bool TransitionToFailed() noexcept;

    // Rebuild attempts since the last full recovery. Drives the M3 retry cap.
    uint32_t RebuildAttempts() const noexcept { return m_RebuildAttempts; }

    // Recoveries that completed and then lost the device again inside the stick
    // window, since the last recovery that held. Drives the recover-relose cap.
    uint32_t RecoveryCycles() const noexcept { return m_RecoveryCycles; }

  private:
    std::atomic<DeviceHealth> m_Health{DeviceHealth::Healthy};
    // Time the current Hung episode began. Only read/written on the render
    // thread (the cross-thread accessor reads m_Health only), so it needs no
    // atomicity. Meaningful only while m_Health == Hung.
    Clock::time_point m_HungSince{};
    // Consecutive rebuild attempts since the last successful full recovery.
    // Render-thread-only.
    uint32_t m_RebuildAttempts = 0;
    // Successful recoveries that did not hold, since the last one that did.
    // Render-thread-only.
    uint32_t m_RecoveryCycles = 0;
    // When the last successful re-provision completed, and whether there has been
    // one. The flag is not redundant with a sentinel time: Clock::time_point{} is a
    // legitimate stamp. Both render-thread-only.
    Clock::time_point m_LastRecoveryTime{};
    bool m_HasRecovered = false;
};

} // namespace Rendering
} // namespace GameEngine
