# Compile Server (GameEngine.CompileServerHost)

This document describes the cross-platform compile server used by the Editor and tools, the wire protocol, transports, and how the native client interacts with the managed host.

---

## 1. Components

- **Managed host:** `GameEngine.CompileServerHost` (`Managed/CompileServerHost`)
	- Built as `GameEngine.CompileServerHost.dll` for .NET 10.
	- Repository build output: `Managed/CompileServerHost/bin/<Config>/net10.0/GameEngine.CompileServerHost.dll`, where `<Config>` is the selected build configuration (for example, `DebugFast`).
	- The editor stages the host under the same `Managed/CompileServerHost/bin/<Config>/net10.0` layout beside the application; macOS app bundles use `Contents/Resources`.
	- Native discovery checks bundle resources, then searches upward from the current working directory and executable directory. At each location it prefers its own configuration, then probes `DebugFast`, `RelWithDebInfo`, `Debug`, `Release`, and `MinSizeRel`. `CompileServerHost.dll` is accepted as a legacy filename. There is no single shared Debug-only output.
- **Native client:** `GameEngine::CompileServerClient` + `PipeTransport` (`Engine/Source/Jobs`).
  - Used by scripting / Editor to offload C# compilation to a long-lived Roslyn process.
- **Smoke tests:** `EngineCompileServerSmokeTests` (GTest target).
  - Verifies end-to-end behavior on Windows and Linux (via Docker).

---

## 2. Transports and endpoints

### 2.1 Windows: named pipes

- Host listens on a **named pipe** with the name passed as the first argument:
  - `dotnet GameEngine.CompileServerHost.dll GE_CompileServer_smoke`
- Common pipe names:
  - `GE_CompileServer` – default engine/editor pipe.
  - `GE_CompileServer_default` – used by some tools/tests.
  - `GE_CompileServer_smoke` – used by smoke tests.
- A small PID file is written to `%TEMP%/GE_CompileServer/<pipe>.pid` so tools can discover running servers.

### 2.2 Linux / macOS: Unix domain sockets

- On non-Windows platforms the same **pipe name** maps to a Unix domain socket.
- Base directory is chosen as:
  - If **`GE_PIPE_PATH`** is set: `<GE_PIPE_PATH>`.
  - Else on **Linux**:
    - If `XDG_RUNTIME_DIR` is set: `<XDG_RUNTIME_DIR>/gameengine-compile-server`.
    - Else: `/tmp/gameengine-<uid>/compile_server`.
  - Else on **macOS**:
    - If `TMPDIR` is set: `<TMPDIR>/gameengine-compile-server`.
    - Else: `/tmp/gameengine-compile-server`.
- Socket path for a given pipe name is:
  - `<baseDir>/<pipeName>.sock` (e.g. `/tmp/gameengine-1000/compile_server/GE_CompileServer.sock`).
- PID hints are also written under the OS temp directory (same semantics as Windows).

---

## 3. Host lifetime and idle shutdown

- On startup the host writes `<Temp>/GE_CompileServer/<pipe>.pid`: line 1 its process ID, line 2 the full path of the `GameEngine.CompileServerHost.dll` it runs from. Once per process, before it first launches a host, the client deletes stale files: files without the path line, files whose process has exited (a host that was killed leaves its file behind), and on Windows files whose process ID now belongs to a process started after the file was written.
- Building the host stops only the host running from that build's own output DLL: that host would keep serving the replaced code, and on Windows it keeps the file it loaded open, so the copy would fail. The build first runs `--shutdown-running-from` with the previous output; on Windows a host that did not answer is then force-stopped through line 2 of the PID files. Hosts running from other worktrees or from an editor's staged copy keep running; a host from an older protocol version is replaced by the client's version check on its next compile.
- The server tracks:
  - `s_StartTimeUtc` – start time.
  - `s_LastActivityUtc` – last successful compile request.
- **Idle timeout:**
  - Controlled by `GE_COMPILE_SERVER_IDLE_SECONDS` (seconds).
  - Default: `120`.
  - `0` disables idle shutdown.
- When the idle timeout elapses with no compile requests, the server exits and deletes its PID/socket.
- **Ephemeral (test-spawned) hosts:**
  - The host is a *resident* service by design: its pipe name is keyed to the workspace, not the spawning process, so it deliberately survives an Editor restart (warm Roslyn caches).
  - Processes that must not leak hosts (test runs) set `GE_COMPILE_SERVER_EPHEMERAL=1` before the first spawn. The native launch path (`CompileServerClient`) then binds every host it starts to the spawner's lifetime:
    - **Windows:** the host is created suspended, assigned to a job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, then resumed — the OS kills it when the spawning process exits for any reason (including crashes).
    - **Linux:** the forked child sets `PR_SET_PDEATHSIG` (`SIGKILL`).
    - **macOS:** no OS-level parent-death hook; fixture teardown reaping is the only cleanup layer.
  - Production (Editor) never sets this variable and keeps resident semantics.

---

## 4. Wire protocol

All communication is **UTF-8 text**, one line per request/response.

### 4.1 Control messages (client → server)

Sent as a single line **without JSON**:

- `__version__` – query server version.
- `__status__` – query runtime status.
- `__shutdown__` – request immediate shutdown.
- `__shutdown_if_idle__` – ask server to shutdown only if currently idle.

### 4.2 Control responses (server → client)

- `__version__` → JSON object:
  - `{ "Version": "<semver>", "ProcessId": <int> }`.
- `__status__` → JSON object:
  - `{ "Pipe": "<name>", "ProcessId": <int>, "UptimeSeconds": <double>, "LastActivityAgoSeconds": <double>, "IdleSeconds": <int>, "Workspaces": <int>, "ImplicitUsings": ["<namespace>", ...] }`.
  - `ImplicitUsings` lists the namespaces the server imports for a request with `ImplicitUsings: true`.
- `__shutdown__` → plain text line: `"ok"` on success.
- `__shutdown_if_idle__` → plain text line:
  - `"ok"` if server was idle and shut down.
  - `"busy"` if there was recent activity and server stayed alive.

### 4.3 Compile request (client → server)

Sent as a single **JSON line** matching `BuildRequest`:

```json
{
  "ProjectRoot": "<absolute path to scripts project root>",
  "ChangedFiles": ["<.cs paths>"],
  "AffectedFiles": ["<.cs paths>"],
  "AllFiles": ["<all visible .cs files>"],
  "PreferredStrategy": "Incremental" | "Full",
  "ForceFull": false,
  "Config": "Debug" | "Release" | null,
  "Tfm": "net10.0" | null,
  "EngineBinDir": "<optional engine bin dir>" | null,
  "AssemblyName": "<assembly identity>" | null,
  "References": ["<extra absolute reference paths>"] | null,
  "Defines": ["<preprocessor symbols>"] | null,
  "AllowUnsafe": true | absent,
  "ImplicitUsings": true | absent,
  "Nullable": true | absent
}
```

Semantics:

- `ChangedFiles` / `AffectedFiles` / `AllFiles` follow the same conventions as the in-process incremental compiler.
- `PreferredStrategy` + `ForceFull`:
  - If `PreferredStrategy == "Incremental"` and `ForceFull == false`, the server reuses cached syntax trees per `ProjectRoot`.
  - Otherwise it reparses all `AllFiles`.
- `EngineBinDir` (optional):
  - Tells the server where to find `GameEngine.Scripting.ABI.dll`, `GameEngine.CoreBridge.dll`, and `GameEngine.HotReload.dll` for metadata references.
  - If omitted, the server tries to infer this from `ProjectRoot` or its own `AppContext.BaseDirectory`.
- `AllowUnsafe`, `ImplicitUsings`, `Nullable` (optional, absent means `false`): the language settings of the generated csproj (`AllowUnsafeBlocks`, `ImplicitUsings`, `Nullable`), so the live compile accepts and warns about exactly what `dotnet build` of that csproj does. The editor sends all three as `true` for project scripts, editor scripts and package C# modules.
  - `AllowUnsafe` permits `unsafe` blocks and pointer types.
  - `ImplicitUsings` adds the global usings of `Microsoft.NET.Sdk`: `System`, `System.Collections.Generic`, `System.IO`, `System.Linq`, `System.Net.Http`, `System.Threading`, `System.Threading.Tasks`.
  - `Nullable` enables the nullable context, so nullable warnings such as `CS8602` are reported. It changes diagnostics only, never whether a compile succeeds.
  - Adding a request field changes the protocol, so `CompileServerVersion.txt` moves with it: the editor then recycles a resident host from before the field instead of sending it a request it would partly ignore.

### Language settings for script authors

Project scripts, editor scripts and the C# modules of packages all compile with the same settings, whether the editor compiles them live, you build the generated `.csproj` with `dotnet build`, or a ship-optimized export compiles them ahead of time:

- `unsafe` code is allowed.
- The implicit usings listed above are in scope, so `List<T>`, `File`, `Task` or LINQ need no `using` directive.
  - A type of your own with the same name as a type in those namespaces (`Path`, `Timer`, `Task`, `Thread`, `Lock`) is ambiguous (`CS0104`) in a file that imports its namespace with a `using` directive. Qualify the name (`MyGame.Nav.Path`) or add an alias in that file (`using Path = MyGame.Nav.Path;`).
- The nullable context is enabled: `string?` marks a reference that may be null, and dereferencing one without a check produces a warning, not an error.

### 4.4 Compile response (server → client)

JSON object matching `BuildResponse`:

```json
{
  "Success": true,
  "Warnings": [ { "Severity": "Warning", "Code": "CS0168", "FileUtf8": "...", "Line": 10, "Column": 5, "MessageUtf8": "..." } ],
  "Errors": [ { "Severity": "Error", "Code": "CS1002", "FileUtf8": "...", "Line": 42, "Column": 13, "MessageUtf8": "; expected" } ],
  "AssemblyBytes": "<base64 PE bytes>" or null,
  "PdbBytes": "<base64 PDB bytes>" or null
}
```

Notes:

- `Success` indicates whether Roslyn compilation succeeded.
- `Warnings` / `Errors` include file/line/column and the original diagnostic message (all UTF-8, paths in UTF-8).
- `AssemblyBytes` / `PdbBytes` are present only when `Success == true`.

---

## 5. Native client behavior (CompileServerClient)

High-level flow for `CompileServerClient::Compile`:

1. Ensure a `PipeTransport` exists for the configured pipe name.
2. Attempt to connect; if it fails, call `StartServerIfNeeded()`:
   - Locates `GameEngine.CompileServerHost.dll` using the configuration and location search described above.
   - Starts `dotnet GameEngine.CompileServerHost.dll <pipe>` (via `CreateProcessW` on Windows, `fork/execvp` on Unix).
   - Waits for the server to accept connections (up to ~10 seconds).
3. On each attempt, perform a **version handshake**:
   - Send `__version__`.
   - Parse `Version` and compare to `kExpectedCompileServerVersion` (generated from the host build).
   - If mismatched, send `__shutdown__`, restart the server, and retry.
4. Open a fresh connection and send the compile JSON request.
5. Parse the `BuildResponse` JSON and base64-decode `AssemblyBytes` / `PdbBytes`.
6. If `Success == false`, log a summarized diagnostic block and return `false`.

The client retries a small, fixed number of times with backoff before giving up and falling back to in-process compilation (if configured to do so).

---

## 6. Testing and workflows

- **Native smoke tests:** `EngineCompileServerSmokeTests` (GTest target)
  - Exercises a full round-trip compile using `CompileServerClient` and asserts that:
    - The server can be started automatically.
    - Version handshakes and responses are valid.
    - A simple scripts project compiles successfully.
- **Linux Docker script:** `Tools/Scripts/run-linux-scripting-smoke-tests.ps1`
  - Builds and runs:
    - `EngineCoreClrSmokeTests`.
    - `EngineCompileServerSmokeTests`.
    - Managed IoL tests (`Managed/InteropTests.IoL`).
  - Intended as the primary cross-platform sanity check for compile-server + scripting.

---

## 7. Useful CLI commands

From the repository root, use the configuration that was built; the examples below use `DebugFast`. For a staged application, use its managed host path:

- Query status for a specific pipe:
  - `dotnet Managed/CompileServerHost/bin/DebugFast/net10.0/GameEngine.CompileServerHost.dll --status GE_CompileServer_smoke`
- Shutdown a specific server instance:
  - `dotnet .../GameEngine.CompileServerHost.dll --shutdown GE_CompileServer_smoke`
- Shut down the servers running from one copy of the host (each server whose PID file names the path is asked, and exits only if it runs from that path):
  - `dotnet .../GameEngine.CompileServerHost.dll --shutdown-running-from <path to GameEngine.CompileServerHost.dll>`
- Request shutdown only if idle (no recent work):
  - `dotnet .../GameEngine.CompileServerHost.dll --shutdown-if-idle GE_CompileServer`

	These commands use the same transports and control messages as the native client and are safe to use during development to inspect or recycle compile-server instances.

	On Windows hosts that also run Linux tests inside Docker (for example via `Tools/Scripts/run-linux-editor-iol-test.ps1` or `Tools/Scripts/run-linux-scripting-smoke-tests.ps1`), it is important to avoid leaving background `GameEngine.CompileServerHost.dll` processes running while a container build is rebuilding the host into a shared `Managed/CompileServerHost/bin/<Config>/net10.0` output. The Linux harnesses perform a **pre-flight shutdown** of any such Windows-hosted compile-server processes before invoking `dotnet build` to avoid MSB3021 "Access to the path ... is denied" errors when copying the host DLL on a shared volume.

