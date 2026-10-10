#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Platform
{

/**
 * @brief Whether native code modules can be loaded at runtime
 * (LoadLibrary/dlopen). False on single-static-binary platforms (wasm),
 * where user native code must be linked in at build time instead.
 */
bool SupportsDynamicNativeModules();

/**
 * @brief Whether the platform hosts more than one OS window. False on
 * single-surface platforms (wasm owns one canvas), where Window::Create
 * refuses every window after the first. Callers that would open a tool
 * window or a floating popup present it in-engine instead.
 */
bool SupportsMultipleWindows();

/**
 * @brief Worker-thread budget for the engine JobSystem.
 * Hardware concurrency on desktop; on threaded wasm a smaller budget matched
 * to the pre-spawned pthread pool; 0 on single-threaded wasm, where the
 * JobSystem runs inline.
 */
std::uint32_t RecommendedWorkerCount();

/**
 * @brief The most blocking threads the engine JobSystem creates for its job
 * channels (JobSystem::JobChannel): the threads that run work whose thread is
 * off-CPU for most of the job (builds, compiles, process waits). The pool's
 * ceiling, JobSystem::kMaxBlockingThreadBudget, on desktop; 1 on threaded
 * wasm, whose threads all come from the pre-spawned pthread pool (the compute
 * workers, this thread and the asset reader threads fit it); 0 on
 * single-threaded wasm, where channel jobs run inline.
 */
std::uint32_t BlockingThreadBudget();

/**
 * @brief Whether libraries may create worker pools in addition to the engine's
 * reserved threads. False when a fixed host pool has no spare thread budget;
 * libraries use their synchronous mode or the engine JobSystem there.
 */
bool SupportsAuxiliaryThreadPools();

/**
 * @brief Whether a socket can listen for inbound connections. False on wasm,
 * whose BSD socket layer tunnels through a WebSocket relay and can only dial
 * out; a listener there binds, never accepts, and parks its thread forever.
 */
bool SupportsInboundSockets();

/**
 * @brief Whether a directory tree can be walked synchronously from the UI
 * thread. False on wasm: the project tree lives in OPFS behind WASMFS's proxy,
 * and a walk from the frame thread blocks the thread that services that proxy —
 * a deadlock, not slowness. Callers skip or defer the walk when this is false.
 */
bool SupportsSynchronousDirectoryWalk();

/**
 * @brief Whether a thread may be created for one task and retired when it is
 * done (std::thread, std::async). False on wasm: threads come from a fixed
 * pre-spawned pthread pool, a pthread_create past that pool blocks the caller
 * forever, and a pthread's exit races the runtime's mailbox pump in the
 * recycled worker, which kills whichever pool worker inherits it. Callers run
 * the work inline or on the engine JobSystem when this is false.
 */
bool SupportsTransientThreads();

/**
 * @brief Whether the process can spawn another process (git, cmake, an IDE).
 * False on wasm, where the POSIX spawn calls exist in the sysroot, link, and
 * then fail with an opaque errno.
 */
bool SupportsProcessCreation();

/**
 * @brief Whether something other than this process can change the files it
 * watches. False on wasm: the project tree lives in the origin's private
 * storage, so the only writer is this process and a filesystem watcher would
 * only poll a proxied filesystem for changes that cannot happen.
 */
bool SupportsExternalFileChanges();

/**
 * @brief Whether the project tree lives in storage the OS file dialogs cannot
 * address. True on wasm: a folder picked by the browser is imported into the
 * origin's storage rather than opened in place, so a "choose a folder" flow
 * that expects to name a location inside the project must browse the project
 * tree itself instead.
 */
bool ProjectStorageIsSandboxed();

/**
 * @brief Whether the host owns the frame loop and hands the application one
 * frame at a time, delivering input and presenting only after that frame
 * returns. True on wasm: the browser's requestAnimationFrame is the loop, so a
 * frame that runs long — a cold world tick behind a boot modal — starves the
 * modal of the very input that would dismiss it. False where the application
 * pumps its own message loop and a long frame merely arrives late.
 */
bool HostDrivesFrameLoop();

/**
 * @brief Whether the primary editor-shortcut modifier is Command rather than
 * Control (Undo, Save, Duplicate, ...). On native this is a build-time
 * property of the OS the binary is compiled for. On wasm one binary reaches
 * every host OS, so the same #ifdef that decides it natively (__APPLE__,
 * never defined by Emscripten) cannot decide it there — the browser is asked
 * at runtime which OS it is hosted on instead.
 */
bool PrimaryShortcutModifierIsCommand();

/**
 * @brief Whether reading the clipboard can show the user a permission prompt.
 * True on wasm: every read is a real navigator.clipboard.readText() call, and
 * unlike a copy (fired inside the Ctrl+C keystroke, where the browser allows
 * it without asking) a read reached from building a context menu is not
 * always inside a strong enough user gesture to suppress the dialog. A caller
 * that reads only to preview content — a menu's Paste-item enabled state —
 * skips the read when this is true rather than risk a prompt on every
 * right-click; a caller fulfilling an actual Paste command still reads
 * unconditionally, on every platform, since that read is what the user asked
 * for.
 */
bool ClipboardReadCanPromptUser();

} // namespace Platform
} // namespace GameEngine
