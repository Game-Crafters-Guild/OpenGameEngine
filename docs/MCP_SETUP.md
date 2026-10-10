# Open Engine MCP Server & CLI Setup

Open Engine ships one set of tools that drive and introspect the running editor —
query the ECS/scene, read logs, take screenshots, inspect the render graph, build/run
tests, launch the editor — reachable two ways: as a [Model Context
Protocol](https://modelcontextprotocol.io) (MCP) server for an AI client (Claude Code,
Cursor, Claude Desktop, …), and as a CLI for a terminal or a script.

## Architecture

```
┌─────────────┐  MCP / JSON-RPC  ┌───────────────────┐
│  AI client  │ ◄──────────────► │  mcp/src/index.ts │ ┐
│ (Claude …)  │     (stdio)      │  (MCP adapter)    │ │
└─────────────┘                  └───────────────────┘ │  ┌──────────────────┐   TCP IPC   ┌─────────────┐
                                                       ├─►│ mcp/src/tools/   │ ◄─────────► │  Editor     │
┌─────────────┐      argv        ┌───────────────────┐ │  │ (tool registry)  │ 127.0.0.1:  │(DebugServer)│
│  terminal / │ ◄──────────────► │  mcp/src/cli.ts   │ ┘  └──────────────────┘        9999 └─────────────┘
│   script    │                  │  (CLI adapter)    │
└─────────────┘                  └───────────────────┘
```

**One registry, two front-ends.** Every tool is defined once in `mcp/src/tools/` and both
adapters build their surface from that array, so a new tool appears in MCP *and* the CLI
with no second list to update. Each tool proxies to the editor's debug server over TCP
`127.0.0.1:9999`; a handful (build, launch, tests, codegen, graph editing) run on the host
instead. The editor starts the listener automatically on launch (look for an
`EditorDebugServer: Listening on port 9999` line in the editor log). The listener binds to
`127.0.0.1` only. If the editor isn't running, calls fail with "Editor is not running or not
responding on port 9999. Launch it with launch_editor first." — launch it (see below) and
retry.

## Prerequisites

- **Node.js ≥ 20** (`mcp/package.json` `engines`). Check with `node --version`.
- A built **Editor** (the MCP server talks to it; it does not build it for you, though it
  exposes `build_engine` / `launch_editor` tools that do).

## Build the server

The committed [`.mcp.json`](../.mcp.json) points the AI client at `mcp/dist/index.js`,
which is a build artifact — `mcp/dist/`, `mcp/node_modules/` are git-ignored and must be
built locally:

Run this after a clone, and after a `git pull` that touched `mcp/`:

```bash
cmake --build --preset <preset> --target McpServer
```

Choose and configure a preset first with `cmake --list-presets` and
`cmake --preset <preset>`. Configure runs no npm commands. When npm is on PATH it
registers the optional `McpServer` target; otherwise it prints one notice and omits
the target. The target is not part of the default build. It installs dependencies
with `npm ci` when the package manifests change and runs `npm run build` when the
dependency stamp, TypeScript configuration, or source files change. An unchanged
second build does no work. After editing `mcp/src`, run the same target and reload
the MCP client; the CLI launcher also rebuilds stale output automatically.

`node mcp/ge.mjs` does not need the target: on its first run it installs the dependencies
and builds `mcp/dist` itself (build output on stderr). Building `McpServer` also stages the
server beside the editor executable, which is the copy the editor's
[AI Assistant](Editor/ai-assistant.html) runs.

## Configuration

### Claude Code

`.mcp.json` at the repo root registers the server at **project scope** (shared, committed):

```json
{
  "mcpServers": {
    "gameengine": {
      "type": "stdio",
      "command": "node",
      "args": ["mcp/dist/index.js"],
      "env": { "PATH": "${PATH};${NVM_SYMLINK:-c:\\nvm4w\\nodejs}" }
    }
  }
}
```

Project-scoped servers require a one-time trust approval. This repo pre-approves it in
the committed [`.claude/settings.json`](../.claude/settings.json) so it connects without a
prompt:

```json
{ "enabledMcpjsonServers": ["gameengine"] }
```

Verify from a terminal:

```bash
claude mcp get gameengine     # expect: Scope: Project, Status: ✓ Connected
claude mcp list               # gameengine ... ✓ Connected
```

Tools appear in the session as `mcp__gameengine__<tool>` after the client reloads the
config (restart the session / reload the window once after first build).

### Per-OS notes on resolving `node`

The MCP client launches `node mcp/dist/index.js`, so `node` must resolve in the launch
environment — which is **not** your interactive shell's environment.

- **Windows (nvm4w):** the `env.PATH` above appends nvm's current-node symlink
  (`%NVM_SYMLINK%`, default `c:\nvm4w\nodejs`) so the launcher finds `node`. If you
  install Node another way and it's already on the launcher's PATH, the entry is harmless.
- **macOS / Linux:** PATH splits on `:`, so the Windows suffix fuses onto the last entry of
  your PATH (`/opt/homebrew/bin` becomes `/opt/homebrew/bin;c`) and adds one junk segment.
  Bare `node` works as long as your Node install dir (e.g. `/opt/homebrew/bin`) is on the
  client's launch PATH and is not its last entry. When the client is launched from a
  terminal (or a GUI app that inherits the login shell PATH), this is usually the case.
- **If `node` is NOT on the launch PATH** (e.g. a macOS GUI app started from Finder with a
  stripped PATH): don't edit the shared `.mcp.json` — a single PATH string can't be made
  correct for both OSes (macOS splits on `:`, Windows on `;`, and `c:\…` contains a colon,
  so any merge corrupts a segment on one platform). Instead add a **machine-local**
  override with an absolute node path; local scope wins over the project entry and is
  auto-trusted:

  ```bash
  claude mcp add gameengine -s local \
    -- /opt/homebrew/bin/node /ABS/PATH/TO/repo/mcp/dist/index.js
  ```

### Targeting your own editor (multi-session machines)

The CLI takes `--port` per call. The MCP server cannot: it resolves the port **once, at
launch**, from `GE_EDITOR_DEBUG_PORT` (or `GE_IPC_PORT`), defaulting to `9999`. An MCP
server started without one therefore talks to whatever holds the default port — on a
machine running several editors, usually somebody else's.

Pin a session's server to its own editor in the client's server entry:

```json
{
  "mcpServers": {
    "gameengine": {
      "command": "node",
      "args": ["/ABS/PATH/TO/repo/mcp/dist/index.js"],
      "env": { "GE_EDITOR_DEBUG_PORT": "9987" }
    }
  }
}
```

Not in the shared `.mcp.json` — the port is per-session. Use a machine-local or
session-local server entry.

**Launching the editor on that port.** Two mechanisms, and the flag wins:

```bash
Editor.exe --debug-port 9987          # explicit, per-launch — OUTRANKS the env var
GE_EDITOR_DEBUG_PORT=9987 Editor.exe  # ambient, inherited by everything in that shell
```

Precedence is `--debug-port` > `GE_EDITOR_DEBUG_PORT` > `9999`, and the resolved port is
logged with its source:
`MCP debug server started on port 9987 (from --debug-port)`. Read that line rather than
assuming — it is the cheapest way to catch an editor that did not take the port you meant.

Prefer the flag for lane isolation. An exported `GE_EDITOR_DEBUG_PORT` applies to every
editor started from that shell and follows child processes around, so it is easy to
apply to one editor by accident and to another on purpose. `--debug-port` binds to the one
launch you typed it on. A malformed `--debug-port` value is never silently dropped to the
default: the editor logs `unrecognized command-line argument` with the value, because the
default is a port another editor may already own. A malformed `GE_EDITOR_DEBUG_PORT` is
ignored without a warning, and the port line then reads `(from default)`. There is no
`--port` on the editor; that spelling belongs to the CLI.

**Reading back which editor answered.** The MCP text block ends with
`[editor 127.0.0.1:9987]`; the CLI prints the same line on stderr and carries an `editor`
field in its `--json` envelope (`ge batch` prints its endpoint once on connect). The
endpoint is read from the live socket, so it names the editor that actually replied rather
than the port that was configured. A refusal is still an answer and is still attributed;
tools that reach no editor — build, launch, codegen, or an unreachable port — report none.
If that line names a port you did not expect, stop: the call landed on another editor.

### Other clients (Cursor / Claude Desktop)

Use the same stdio command. Minimal entry (absolute paths recommended for GUI clients):

```json
{
  "mcpServers": {
    "gameengine": {
      "command": "node",
      "args": ["/ABS/PATH/TO/repo/mcp/dist/index.js"]
    }
  }
}
```

## Using it

1. **Launch the editor** so the IPC listener is up on port 9999 — either start it yourself,
   or call the `launch_editor` tool, then `wait_for_editor`. Without a `preset` argument
   `launch_editor` looks for `vs2026-<arch>-local` (or its `-unity` variant) on Windows and
   `macos-<arch>-ninja` or `macos-<arch>-local` on macOS, including the `.app` bundle; pass
   `preset` for any other build, for example `ninja-x64-local` or `linux-x64-local`. The
   configuration defaults to `DebugFast`; pass `config` for another one (the macOS and
   Linux steps in [BUILD.md](../BUILD.md) build `Debug`).
2. Call tools. Quick check: `get_editor_state` returns play mode, scene, entity count, and
   window geometry.

The server also publishes three read-only resources, `gameengine://log`,
`gameengine://scene` and `gameengine://stats`: the recent log, the scene hierarchy and the
render statistics, each empty while no editor answers.

For the tool list, ask the code rather than a doc that can drift:

```bash
node mcp/ge.mjs --help          # all tools, grouped by category
node mcp/ge.mjs list            # just the names, one per line
```

## Command-line interface

```bash
node mcp/ge.mjs get_editor_state
node mcp/ge.mjs get_log --count 50 --min-level warning
node mcp/ge.mjs take_screenshot --target viewport
node mcp/ge.mjs set_camera --position 70,9,50 --yaw-deg 300
node mcp/ge.mjs get_render_stats --port 11162     # a non-default editor
```

`ge.mjs` stat-checks `mcp/src` against `mcp/dist` and runs `tsc` when stale, so editing a
tool and running the CLI can never execute yesterday's definitions. Build output goes to
stderr only, so scripts can parse stdout as JSON. `GE_CLI_NO_BUILD=1` skips the check (CI
builds `mcp/` explicitly); `GE_CLI_FORCE_BUILD=1` forces one.

**Arguments.** Flags come from each tool's JSON Schema — the same document MCP publishes
through `tools/list` — so `ge <tool> --help` cannot describe something an agent sees
differently. Schema keys are kebab-cased (`--entity-id`), and the exact camelCase spelling
works too.

| Shape | Syntax |
|---|---|
| vector | `--position 1,2,3` or `--position '{"y":5}'` |
| list | `--extra-args a --extra-args b`, or `--position 70,9,50` |
| object | `--values Intensity=3` (repeatable), or `--values '{"Intensity":3}'` |
| object list | `--fields Current:float --fields Max:float` |
| boolean | `--all` / `--no-all` / `--all=false` — **omitted is a third state**, and tools like `set_gizmos_visibility` rely on it |
| everything | `ge get_log '{"count":50}'` — a lone `{…}` is the whole parameter object, so an MCP tool call pastes in verbatim |

**Other subcommands.** `ge raw <method> '{…}'` forwards any debug-server method
unvalidated, which is how the handlers with no typed tool stay reachable
(`node Tests/Mcp/ipc-method-drift.mjs --list` shows which, and why). `ge batch` reads
ndjson `{"method","params"}` from stdin over one connection — pair responses by `id`, not
by line order, because deferred captures answer frames later and interleave.

**Global flags:** `--port` `--host` `--timeout` `--json` `--dry-run` `--quiet`.
`--dry-run` prints the resolved parameters without calling, which is the cheapest way to
check an invocation with no editor running.

**Exit codes:** 0 ok · 1 tool failed · 2 usage · 3 editor unreachable · 4 timeout. With
`--json`, the same distinction arrives as `{"ok":false,"code":…,"error":…}`.

## Standalone binary (no node required)

For machines without node — end users, build agents, a bare CI runner — the same CLI
packages as a native executable:

```bash
cmake --build build/<preset> --config <config> --target GameEngineCli
build/<preset>/bin/<config>/gameenginecli get_editor_state      # .exe on Windows
```

The binary embeds the JS runtime (Node SEA, ~90 MB) and talks TCP to the editor's debug
server directly — node/npm are build-time requirements only, and the packaging step
smoke-tests the binary it produced. It is frozen at build time: there is no
rebuild-from-source check inside (that is `ge.mjs`'s job, for the dev loop).

What changes vs `ge.mjs`, and what doesn't:

- Every IPC-backed tool, `raw`, `batch`, `--help`, exit codes: identical — same registry,
  same code.
- Host-side tools (`build_engine`, `run_tests`, `launch_editor`, codegen) still spawn
  cmake/ctest/Editor.exe, so those need to exist on the machine — but never node.
- Tools that read the repo (`list_debug_methods`, the ECS/game-graph authoring tools,
  `build_engine`) locate it by walking up from the executable, then from the working
  directory; `GAMEENGINE_ROOT` overrides both. Outside any checkout they fail with that
  instruction rather than guessing. Pure IPC tools work anywhere.

Platform notes: SEA does not cross-compile — each platform's build produces its own
binary, like every other target. macOS is ad-hoc re-signed during packaging (unsigned
binaries do not run on arm64). On Windows the copied node.exe's Authenticode signature is
invalidated by injection — cosmetic for a local tool; re-sign before distributing.

## Troubleshooting

- **"Editor is not running … on port 9999"** — the server is fine; the editor isn't up (or
  its IPC listener didn't start). Launch the editor (or `launch_editor`) and retry.
- **"Cannot find module …/mcp/dist/index.js"** — not built. Run
  `cmake --build --preset <preset> --target McpServer`.
- **`bind failed on port 9999` in the editor log** — another process holds the port, and
  this editor runs without a debug server. Start it on a free port with `--debug-port <n>`
  and point the client at that port (see *Targeting your own editor*).
- **Client shows the server failing to start / `node` not found** — `node` isn't on the
  client's launch PATH. See *Per-OS notes* above; on macOS use a local-scope override with
  an absolute node path.
- **Stale tools after editing `mcp/src`** — rebuild (`cmake --build --preset <preset> --target McpServer`) and reload the
  client. The CLI handles this itself; only the MCP client needs the manual step.

## Development

Add a tool as an entry in the right `mcp/src/tools/*.ts` module — MCP and the CLI both pick
it up, and neither adapter is edited:

```ts
proxyTool({
  name: "get_thing",          // also the debug-server method, unless ipcMethod says otherwise
  category: "profiling",
  description: "…",           // what the agent reads, and what `ge get_thing --help` prints
  schema: { id: z.coerce.number().describe("…") },
})
```

Use `defineTool({... run})` instead when the handler runs on the host or reshapes the
response (see `tools/lifecycle.ts`, `tools/capture.ts`). A handler returns
`{data, summary?, image?}` and throws `ToolError` to fail; the adapters render both.

Implement the matching IPC method in `Apps/Editor/Source/DebugServer/`. If a handler
should stay reachable only through `ge raw`, list it in
`mcp/ipc-method-coverage.json` with a reason — the `EditorIpcMethodDrift` ctest and
CI step fail on any handler that is neither exposed nor explained, and on any tool naming
a method that no longer exists.

Rebuild with `cmake --build --preset <preset> --target McpServer` (the CLI launcher also handles stale output).

## Security

The server has full access to the editor and, via build/test/launch tools, the local
toolchain and file system. Only enable it for trusted projects and AI clients.

The editor's debug server listens on `127.0.0.1` only and asks for no credentials: any
program on this machine can connect to its port and call its methods, including the ones
reachable only through `ge raw`.
