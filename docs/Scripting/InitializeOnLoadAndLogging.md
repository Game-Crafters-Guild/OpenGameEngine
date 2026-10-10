# InitializeOnLoad (IoL) and Logging

This document describes how InitializeOnLoad (IoL) works in the Editor and other hosts, how `Console.WriteLine` is routed into the native logger, and what contracts / invariants we rely on so this remains stable.

---

## 1. High‑level picture

- IoL allows scripts to mark static methods with `[InitializeOnLoad]`.
- When the Editor boots (and when it hot‑reloads scripts), IoL methods are discovered and invoked by **GameEngine.HotReload** running inside **GameEngine.CoreBridge**.
- IoL methods can call `Console.WriteLine`; at runtime this is redirected through **EngineLogWriter** into native logging (`GE_Log` → `Logger::Log`).
- Engine-owned content is staged with the application; project scripts belong to the open project:
  - On Windows, managed core assemblies are next to `Editor.exe` in `build/<preset>/bin/<Config>/Apps/Editor`.
  - Project script sources: `<project>/Assets`.
  - Project script output: `<project>/ScriptAssemblies/GameEngine.Scripts.dll`.
  - Editor-owned scripts use the installed assets root and a separate `GameEngine.Editor.dll`.

The goal is that IoL `Console.WriteLine` appears in the same logfile as the rest of the Editor’s native logs.

---

## 2. Script and assembly layout (Editor)

**Windows runtime roots (Editor.exe):**

- **Editor binary:**
  - `build/<preset>/bin/<Config>/Apps/Editor/Editor.exe` (for example, `DebugFast`).
- **Project scripts root:**
  - `<project>/Assets`.
- **Generated scripts project + output** (for the open project `<project>`):
  - `<project>/GameEngine.Scripts.csproj`
  - `<project>/ScriptAssemblies/GameEngine.Scripts.dll`

**Core managed assemblies:**

- Built by CMake custom targets (`HotReloadAssembly`, `CoreBridgeAssembly`, `ScriptingAbiAssembly`).
- Output directory: staged next to `Editor.exe` (see above).
- At runtime, `PathResolver` resolves them **relative to `Editor.exe`**:
  - `GameEngine.CoreBridge.dll`
  - `GameEngine.HotReload.dll`
  - `GameEngine.Scripting.ABI.dll`

**Project sources and installed content**

- Put project scripts in the open project's assets folder, not in the engine repository or the staged editor's assets.
- The engine derives the default scripts root from the project's resolved asset root and the output directory from its workspace root. An embedding host can supply explicit `ScriptsConfig` roots.
- The installed assets folder holds editor-owned scripts. On macOS, managed core assemblies are staged in `Contents/Resources/Managed`, and generated editor script outputs use a writable scripts-assembly directory rather than modifying the signed app bundle.

---

## 3. IoL execution and console redirection

1. `ScriptManager` boots the CLR and loads `GameEngine.CoreBridge` and `GameEngine.HotReload` using `CoreCLRHost` + `ScriptingABI`.
2. `HotReloadManager` scans `GameEngine.Scripts.dll` for `[InitializeOnLoad]` methods.
3. Before invoking IoL methods, HotReload ensures that **console redirection** is installed:
   - `EngineLogWriter.Install` (or equivalent) sets `Console.Out` (and often `Console.Error`) to an `EngineLogWriter` instance.
4. Inside IoL code, any `Console.WriteLine` calls go through `EngineLogWriter.WriteLine`.

**EngineLogWriter routing rules (simplified):**

- Preferred path:
  - Use the `EngineInstanceContext` the native host registered with CoreBridge.
  - If the context has a bound `EngineNativeBinding.Log` delegate, forward the message to that delegate.
- Fallback path:
  - If no binding exists, call native `GE_Log` in `GameEngine.Native.dll` via P/Invoke.

Either way, IoL logging ultimately reaches the **native `GE_Log` ABI**.

---

## 4. Native logging bridge and `GE_LOGFILE`

The Editor shares the engine's logger state. At startup it calls
`Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState())` before configuring sinks,
and passes that state to `GameEngine.Native` through `GE_SetHostLoggerState`.
The host and ABI then use the same sinks, levels, and queue.

The Editor configures its logger (console + a file sink) on startup and passes the logfile path through the environment variable **`GE_LOGFILE`**. The file sink is always attached: `-logfile` chooses the path, and without it the Editor writes a per-process default under the user cache directory. Historically, the `GameEngine.Native` logger was left unconfigured, so IoL logs routed via `GE_Log` were dropped.

For standalone hosts that have not shared a configured logger, `Engine/Source/Scripting/ScriptingABI.cpp` retains a **lazy bridge** in `EnsureNativeLoggerConfiguredFromEnv()` that runs on first `GE_Log` call:

- If `Logger::Log` inside `GameEngine.Native` already has sinks:
  - Assume a host (or test harness) configured logging explicitly.
  - Log a one‑shot debug message and **do nothing else**.
- Otherwise (no sinks yet):
  - Initialize the logger with a default config (`GlobalMinLevel = Debug`).
  - Read `GE_LOGFILE` via `std::getenv`.
  - If non‑empty:
    - Attach a `FileSink` pointing at `GE_LOGFILE`.
    - Log an info message:
      - `GameEngine.Native logger attached to GE_LOGFILE: <path>`.
  - If empty / unset:
    - Log a one‑shot warning indicating that no logfile bridge could be established.

`GE_Log` now always calls `EnsureNativeLoggerConfiguredFromEnv()` before writing to `Logger::Log`, so IoL `Console.WriteLine` reliably reaches the Editor logfile when `GE_LOGFILE` is set.

---

## 5. Contracts for hosts embedding GameEngine.Native

If you are writing a new host (other than the Editor):

1. **Decide who owns the logfile.**
   - If your host manages the logfile path, set `GE_LOGFILE` in the process environment before any managed code calls `GE_Log`.
   - If the host configures the logger, share its state with the ABI through `GE_SetHostLoggerState` before managed code logs. Configuring an independent logger copy does not configure the ABI copy.

2. **Understand the bridge behavior:**
   - If your host shares a logger state that already has sinks before any `GE_Log` calls, the bridge will **not** attach a `FileSink` or read `GE_LOGFILE`.
   - If you do not configure sinks and `GE_LOGFILE` is set, the bridge will attach a `FileSink` for you on first `GE_Log`.

3. **Console redirection is opt‑in:**
   - IoL console redirection (via `EngineLogWriter`) is currently wired for the Editor.
   - Other hosts can opt in by:
     - Loading `GameEngine.CoreBridge`.
     - Calling `EngineLogWriter.Install()` (or `CoreBridge.Demo_InitializeManaged`) from managed code.

---

## 6. Tests covering IoL + logging behavior

To guard against regressions, there are both native and managed tests that exercise the IoL logging pipeline and the `GE_LOGFILE` bridge:

- **Native (GTest):** `AbiNativeSmokeTest` target
  - `Tests/Integration/AbiNativeSmokeTest.cpp`
    - Loads the C ABI via `GE_GetInterface` and verifies that basic logging and ECS queries succeed.
  - `Tests/Integration/ScriptingAbiLoggerBridgeTests.cpp`
    - Uses the debug-only hook `GameEngine::ResetNativeLoggerBridgeForTests()` to reset the logger state.
    - Confirms that when no sinks are configured and `GE_LOGFILE` is set, the first `GE_Log` call attaches a `FileSink` and writes to the expected logfile.
    - Confirms that when sinks are preconfigured, the bridge respects them and ignores `GE_LOGFILE`.

- **Managed (NUnit):** `Managed/InteropTests/InteropTests.csproj`
  - `InitializeOnLoadLoggingTests`
	    - Verifies that `[InitializeOnLoad]` methods see `Console.Out` as `EngineLogWriter` both on initial load and after hot reload.
	    - Includes a guardrail that runs multiple sequential swaps (to mimic long-lived Editor sessions) and asserts that each swap runs IoL exactly once and still sees `EngineLogWriter` or its wrapper as the underlying writer.
  - `EngineLogWriterRoutingTests`
    - Asserts that when a project context is registered and has an `EngineNativeBinding`, `EngineLogWriter` routes through the managed `binding.Log` delegate (no DllImport), and the native fallback flag is not set.
    - Asserts that when no project context is available, `EngineLogWriter` falls back to `GE_Log` via P/Invoke and marks the one-time native-fallback flag.

These tests are the primary safety net for IoL logging and the `GE_LOGFILE` bridge. When modifying the logging behavior, update these tests (or add new ones) to document and protect the intended behavior.

---

## 7. Debugging checklist for IoL + logging

When IoL logs do not appear where expected:

1. **Confirm paths and layout:**
   - Editor (or host) is being run from the correct binary directory (`build/<preset>/bin/<Config>/Apps/Editor`).
   - Project scripts live under `<project>/Assets` and `<project>/ScriptAssemblies/GameEngine.Scripts.dll` is up to date. Check editor-owned scripts separately in the installed assets folder.
2. **Check `GE_LOGFILE`:**
   - The Editor exports it on every run, so it should be set whether or not `-logfile` was passed. The Editor logs the path it chose as `Editor logfile: ...`.
   - Inside the process, `std::getenv("GE_LOGFILE")` should return that path.
3. **Look for the bridge diagnostics in the log:**
   - `GameEngine.Native logger attached to GE_LOGFILE: ...` (info).
   - Or `[ScriptingABI] GE_LOGFILE not set; GE_Log will use preconfigured sinks only.` (warning).
4. **Verify console redirection:**
   - IoL trace (when enabled) should show that `EngineLogWriter.Install` ran before IoL.
   - Inside managed tests, `Console.Out` should be an `EngineLogWriter` wrapper.
5. **Use the IoL harness when debugging:**
   - `Tools/RunEditorIoLTest.ps1` runs the Editor from `build/<preset>/bin/Debug/Apps/Editor` with IoL tracing and a dedicated logfile, making it easy to validate IoL behavior end‑to‑end.

---

## 8. Related Scripting ABI docs

- **Scripting ABI spec:** `docs/Scripting/ScriptingABI.md` describes the C ABI surface (including `GE_Log` and ECS helpers) that IoL and logging ultimately route through.
