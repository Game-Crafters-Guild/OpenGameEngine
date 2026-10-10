# MCP and CLI gates

Node test scripts that guard the agent tool surface — the MCP server and the `ge`
CLI in `mcp/`, and the editor debug-server handlers they talk to. They read
sources as text or drive the CLI against a faked editor, so none of them needs a
built editor, a GPU, or a specific vendor of graphics hardware.

Each is registered as a ctest in the root `CMakeLists.txt` under `BUILD_TESTING`,
and each is runnable on its own with `node Tests/Mcp/<script>` from the repo root.
Exit 0 means the gate held, 1 means it caught something, 2 means the gate itself
could not run.

| Script | ctest name | What it fails on |
|---|---|---|
| `ipc-method-drift.mjs` | `EditorIpcMethodDrift` | A `RegisterHandler()` in `Apps/Editor/Source/DebugServer` that is neither exposed as a tool in `mcp/src/tools` nor listed in `mcp/ipc-method-coverage.json` with a reason for staying raw-only. Also catches stale and redundant entries in that list. |
| `mcp-editor-endpoint-check.mjs` | `McpEditorEndpointReport` | An answer that does not name which editor produced it. Several editors run at once, so a call that lands on the wrong one returns plausible data; every answer, including a refusal, must report the endpoint taken from the live socket. |
| `rgp-capture-check.mjs` | `McpRgpCaptureGate` | The Radeon GPU Profiler flow attempting a capture on non-AMD hardware, refusing without naming what it found, or reporting a stale `.rgp` file as a fresh capture. |

`ipc-method-drift.mjs` also runs in CI before anything is built, and takes
`--list` to print every debug-server method with how it is reached.

The two harness gates spawn the CLI through `mcp/ge.mjs`, which rebuilds
`mcp/dist` when it is stale — that first build is why their ctest timeout is
generous.
