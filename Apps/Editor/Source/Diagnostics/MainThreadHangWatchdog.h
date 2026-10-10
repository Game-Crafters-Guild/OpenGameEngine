#pragma once

// Samples the main loop's heartbeat from a background thread and, when the
// main thread stops beating for longer than a threshold, captures and logs the
// main thread's call stack. Turns an opaque editor freeze (Windows
// AppHangTransient) into an attributable log entry: which function held the
// message pump, and for how long.
//
// The watchdog arms itself on the first NotifyAlive() after Start(), so editor
// startup (no heartbeat yet) never reports. Native modal dialogs pump their
// own messages but suppress our heartbeat; those show up as stalls whose stack
// names the dialog — informative, not a false alarm.
//
// Teardown is watched too, on its own clock and WITHOUT stack capture: see
// BeginShutdownWatch.
//
// Enabled by default in the editor. GE_HANG_WATCHDOG=0 disables;
// GE_HANG_WATCHDOG_MS overrides the report threshold (default 2000),
// GE_HANG_WATCHDOG_SHUTDOWN_MS the teardown one (default 10000), and
// GE_HANG_WATCHDOG_SHUTDOWN_STACKS=1 arms teardown stack capture for a
// diagnostic run (hazard below).
// Stack capture is Windows-only; other platforms log durations without stacks.
//
// Every frame-window stall report also names what each thread of the engine's
// job system is running (WorkStealingThreadPool::SnapshotOccupancy: lock-free
// and allocation-free, so the read cannot wait on a wedged pool or on the
// suspended main thread). Teardown reports leave it out: the pool is destroyed
// during teardown, so BeginShutdownWatch stops reading it.

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::Editor
{

class MainThreadHangWatchdog
{
  public:
    // Call once on the MAIN thread (captures its thread identity), before the
    // main loop starts. Safe to call when disabled via env — becomes a no-op.
    // `jobSystem`, when not null, is the pool whose occupancy each stall
    // report carries; it must stay alive until BeginShutdownWatch or Stop.
    static void Start(const JobSystem::WorkStealingThreadPool* jobSystem);

    // Switches the watch to the teardown window: the main loop is over, so no
    // heartbeat is coming and the stall clock restarts here against a
    // threshold sized for a whole shutdown. Call on the main thread
    // immediately before Application::Shutdown, so a teardown step that wedges
    // is reported instead of vanishing into a log that simply stops. From here
    // on the job system given to Start is no longer read: teardown destroys it.
    //
    // The report carries duration and phase only, not a stack. Capturing one
    // means suspending the main thread and running dbghelp inside the suspend
    // window, and dbghelp allocates and touches the loader: a main thread
    // frozen while holding the CRT heap or loader lock deadlocks the capture
    // against itself and the process never exits. Teardown — a burst of frees
    // and module unloads — is where the main thread holds those locks most, so
    // an on-by-default capture there could wedge the shutdown it was added to
    // observe. Attach cdb out of process for the stack, or accept the risk for
    // one run with GE_HANG_WATCHDOG_SHUTDOWN_STACKS=1; when it is armed the
    // hazard is named in the log line that says so.
    static void BeginShutdownWatch();

    // Joins the watchdog thread. Call after teardown — everything between
    // Start() and here is watched.
    static void Stop();

    // Heartbeat — call once per main-loop iteration on the main thread.
    static void NotifyAlive();
};

// Optional phase annotation for long main-thread operations: the stall report
// names the phase even when the stack is ambiguous. String literals only —
// the watchdog thread reads the pointer at an arbitrary later time.
class HangWatchdogPhase
{
  public:
    explicit HangWatchdogPhase(const char* phase);
    ~HangWatchdogPhase();

    HangWatchdogPhase(const HangWatchdogPhase&) = delete;
    HangWatchdogPhase& operator=(const HangWatchdogPhase&) = delete;

  private:
    const char* m_Previous;
};

} // namespace GameEngine::Editor
