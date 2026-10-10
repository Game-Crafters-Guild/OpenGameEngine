import { execFile } from "child_process";
import * as fs from "fs";
import { promisify } from "util";
import { z } from "zod";
import { freeDebugPort } from "../build/debug-port.js";
import { defaultCaptureDir, launchEditorProcess, type RenderDocLaunch, resolveRenderdoccmd, waitForEditorReady } from "../build/editor-process.js";
import { buildArgs, ctestArgs, detectArch, detectPreset, editorExePath, isCustomBuild, normalizePreset } from "../build/preset.js";
import { repoRoot } from "../paths.js";
import { defineTool, type ToolDef, ToolError } from "../registry.js";

const execFileAsync = promisify(execFile);

// Building and launching happen on this host, not in the editor — these are the
// only tools in the registry with no debug-server method behind them.

export const tools: ToolDef[] = [
  defineTool({
    name: "build_engine",
    category: "lifecycle",
    actsOnHost: true,
    description: "Build an engine target using CMake. Auto-detects platform (Windows/macOS/Linux), architecture (x64/ARM64), and Visual Studio version. IMPORTANT: Shut down the editor (shutdown_editor) before building to avoid PDB lock errors.",
    schema: {
      target: z.string().optional().describe("CMake target (default: Editor)"),
      config: z.string().optional().describe("Build config: DebugFast, Debug or Release (default: DebugFast)"),
      jobs: z.coerce.number().optional().describe("Parallel jobs (default: 12)"),
      preset: z.string().optional().describe("CMake preset (auto-detected from platform/arch/VS version if omitted)"),
    },
    run: async ({ target, config, jobs, preset }) => {
      const t = target ?? "Editor";
      const c = config ?? "DebugFast";
      const j = String(jobs ?? 12);
      const p = normalizePreset(preset ?? detectPreset(c));
      const args = buildArgs(p, c, t, j);
      const label = isCustomBuild(p) ? p.replace(/__/g, "") : `preset ${p}`;
      try {
        const { stdout, stderr } = await execFileAsync(
          "cmake", args,
          { cwd: repoRoot(), timeout: 300_000, maxBuffer: 10 * 1024 * 1024 },
        );
        // cmake exit 0 = success. Only show the tail of the output.
        const output = (stdout + "\n" + stderr).trim();
        return {
          data: { ok: true, preset: p, config: c, target: t, arch: detectArch() },
          summary: `BUILD OK (${label}, ${detectArch()}):\n${output.slice(-2000)}`,
        };
      } catch (err: any) {
        // cmake exit non-zero = real failure. Show full output for diagnosis.
        const output = ((err.stdout ?? "") + "\n" + (err.stderr ?? "")).trim();
        // Extract actual compiler errors (MSVC "error C", clang/gcc "error:", linker "error LNK").
        const errorLines = output.split("\n").filter((l: string) => /\berror\s+(C\d|LNK\d|:)/i.test(l));
        const summary = errorLines.length > 0
          ? `${errorLines.length} error(s):\n${errorLines.slice(0, 20).join("\n")}\n\n--- Full output ---\n${output.slice(-3000)}`
          : output;
        throw new ToolError(`BUILD FAILED (${label}, exit ${err.code}):\n${summary}`);
      }
    },
  }),

  defineTool({
    name: "run_tests",
    category: "lifecycle",
    actsOnHost: true,
    description: "Build and run a test target via CTest. Auto-detects platform and preset.",
    schema: {
      testFilter: z.string().optional().describe("CTest regex filter (-R), e.g. 'AnimationSystem'"),
      config: z.string().optional().describe("Build config (default: DebugFast)"),
      preset: z.string().optional().describe("CMake preset (auto-detected if omitted)"),
    },
    run: async ({ testFilter, config, preset }) => {
      const c = config ?? "DebugFast";
      const p = normalizePreset(preset ?? detectPreset(c));
      const args = ctestArgs(p, c, testFilter);
      try {
        const { stdout, stderr } = await execFileAsync(
          "ctest", args,
          { cwd: repoRoot(), timeout: 300_000, maxBuffer: 10 * 1024 * 1024 },
        );
        return {
          data: { ok: true, preset: p, config: c, testFilter: testFilter ?? null },
          summary: `TESTS PASSED:\n${(stdout + "\n" + stderr).trim()}`,
        };
      } catch (err: any) {
        const output = ((err.stdout ?? "") + "\n" + (err.stderr ?? "")).trim();
        throw new ToolError(`TESTS FAILED (exit ${err.code}):\n${output}`);
      }
    },
  }),

  defineTool({
    name: "launch_editor",
    category: "lifecycle",
    actsOnHost: true,
    description: "Launch the Editor process in the background. Frees the debug port first (graceful IPC shutdown of the editor holding the configured debug port — GE_EDITOR_DEBUG_PORT, default 9999 — force-kill only if it hangs); editors on other ports are left running. Returns immediately — use wait_for_editor to confirm it's ready. Set renderdoc=true to launch under RenderDoc so trigger_capture works; captures then land at <captureDir>/editor_frameN.rdc.",
    schema: {
      config: z.string().optional().describe("Build config: DebugFast, Debug or Release (default: DebugFast)"),
      preset: z.string().optional().describe("CMake preset (auto-detected if omitted)"),
      extraArgs: z.array(z.string()).optional().describe("Extra command-line args passed to Editor.exe"),
      renderdoc: z.boolean().optional().describe("Launch under 'renderdoccmd capture' so trigger_capture can capture frames. Defaults captureCompat=true, which drops VK_EXT_descriptor_buffer so the .rdc replays — but that also makes HZB occlusion culling report zero culled draws, so pass captureCompat=false for occlusion or perf captures."),
      captureCompat: z.boolean().optional().describe("renderdoc=true: set GE_VK_CAPTURE_COMPAT=1 (default true). Set false to keep VK_EXT_descriptor_buffer — occlusion/perf numbers stay trustworthy, but the capture may fail to replay."),
      captureDir: z.string().optional().describe("renderdoc=true: directory for .rdc files (default: <editor exe dir>/Captures). The editor is pointed at the same directory via GE_RENDERDOC_CAPTURE_DIR."),
    },
    run: async ({ config, preset, extraArgs, renderdoc, captureCompat, captureDir }) => {
      const c = config ?? "DebugFast";
      const p = normalizePreset(preset ?? detectPreset(c));
      const exe = editorExePath(p, c);

      if (!fs.existsSync(exe)) {
        throw new ToolError(`Editor not found at ${exe}. Build it first with build_engine.`);
      }

      let renderdocLaunch: RenderDocLaunch | undefined;
      if (renderdoc) {
        const renderdoccmd = resolveRenderdoccmd();
        if (!renderdoccmd) {
          throw new ToolError(process.env.RENDERDOC_PATH
            ? `RENDERDOC_PATH is '${process.env.RENDERDOC_PATH}' but no renderdoccmd executable is there. `
              + `Point it at the RenderDoc install directory, or at renderdoccmd itself.`
            : `renderdoccmd not found on PATH or at the default install. Install RenderDoc `
              + `(C:\\Program Files\\RenderDoc\\ on Windows), or set RENDERDOC_PATH to its install directory.`);
        }
        renderdocLaunch = {
          captureDir: captureDir ?? defaultCaptureDir(exe),
          renderdoccmd,
          captureCompat: captureCompat ?? true,
        };
      }

      // Free the debug port if a previous editor holds it. Editors on other
      // ports (e.g. the developer's own session) are not touched.
      const portResult = await freeDebugPort();
      if (portResult.blocked) throw new ToolError(portResult.blocked);

      const { pid, logFile, renderdoccmdLog } = launchEditorProcess(exe, extraArgs ?? [], renderdocLaunch);

      return { data: {
        launched: true,
        pid,
        exe,
        logFile,
        portSteps: portResult.steps,
        ...(renderdocLaunch
          ? {
              renderdoc: renderdocLaunch.renderdoccmd,
              captureDir: renderdocLaunch.captureDir,
              captureCompat: renderdocLaunch.captureCompat
                ? "GE_VK_CAPTURE_COMPAT=1 — descriptor buffer dropped so captures replay; HZB occlusion reports 0 culled, so do not use this session for occlusion or perf analysis"
                : "GE_VK_CAPTURE_COMPAT unset — occlusion/perf numbers are trustworthy, but the capture may fail to replay",
              // renderdoccmd launches and injects into the Editor, so the pid
              // above is the launcher's, not the Editor's.
              pidIsRenderdoccmd: true,
              // A failed launch/inject is reported only here.
              renderdoccmdLog,
            }
          : {}),
        note: "Editor is starting. Use wait_for_editor to confirm it is ready and the debug server is accepting connections.",
      } };
    },
  }),

  defineTool({
    name: "wait_for_editor",
    category: "lifecycle",
    description: "Wait until the editor's debug server is accepting connections on the configured debug port (GE_EDITOR_DEBUG_PORT, default 9999). Use after launch_editor or after manually starting the editor.",
    schema: {
      timeoutSec: z.coerce.number().optional().describe("Max seconds to wait (default: 30)"),
    },
    run: async ({ timeoutSec }) => {
      const timeout = (timeoutSec ?? 30) * 1000;
      const ready = await waitForEditorReady(timeout);
      if (!ready) throw new ToolError(`Timed out after ${timeout / 1000}s waiting for the editor. Is it running?`);
      return { data: { connected: true, waitedMs: ready.waitedMs, editorState: ready.state } };
    },
  }),

  defineTool({
    name: "editor_full_cycle",
    category: "lifecycle",
    actsOnHost: true,
    description: "Full build-launch-verify cycle: frees the debug port (graceful shutdown of the editor on the configured debug port only), builds the Editor target, launches it, and waits for it to be ready. Returns editor state when ready. If an editor on another port holds this build's PDB, the build step reports the lock — stop that editor yourself (shutdown_editor with --port set to its debug port).",
    schema: {
      config: z.string().optional().describe("Build config: DebugFast, Debug or Release (default: DebugFast)"),
      target: z.string().optional().describe("CMake target (default: Editor)"),
      jobs: z.coerce.number().optional().describe("Parallel jobs (default: 12)"),
    },
    run: async ({ config, target, jobs }) => {
      const c = config ?? "DebugFast";
      const t = target ?? "Editor";
      const j = String(jobs ?? 12);
      const p = detectPreset(c);
      const steps: string[] = [];

      // Step 1: Free the debug port (port-scoped, like launch_editor — never
      // sweeps editors on other ports). If a different editor still holds this
      // build's PDB, the build error below reports it rather than us killing
      // a session we don't own.
      const portResult = await freeDebugPort();
      steps.push(...portResult.steps);
      if (portResult.blocked) throw new ToolError(portResult.blocked);

      // Step 2: Build.
      steps.push(`Building ${t} (${c})...`);
      try {
        await execFileAsync(
          "cmake",
          buildArgs(p, c, t, j),
          { cwd: repoRoot(), timeout: 300_000, maxBuffer: 10 * 1024 * 1024 },
        );
        steps.push("Build succeeded.");
      } catch (err: any) {
        const output = ((err.stdout ?? "") + "\n" + (err.stderr ?? "")).trim();
        throw new ToolError(`BUILD FAILED (exit ${err.code}):\n${output}`);
      }

      // Step 3: Launch editor.
      const exe = editorExePath(p, c);
      if (!fs.existsSync(exe)) throw new ToolError(`Editor exe not found at ${exe} after build.`);
      const { pid } = launchEditorProcess(exe);
      steps.push(`Launched editor (PID ${pid}).`);

      // Step 4: Wait for connection.
      const ready = await waitForEditorReady(45_000);
      if (!ready) {
        steps.push("Timed out waiting for editor connection.");
        throw new ToolError(JSON.stringify({ steps }, null, 2));
      }
      steps.push(`Editor ready after ${Math.round(ready.waitedMs / 1000)}s.`);
      return { data: { steps, editorState: ready.state } };
    },
  }),

  defineTool({
    name: "shutdown_editor",
    category: "lifecycle",
    actsOnHost: true,
    // Drives `shutdown` over IPC before force-killing, so the drift check counts
    // that method as covered even though there is no plain proxy for it.
    coversIpcMethods: ["shutdown"],
    description: "Shut down the editor holding the configured debug port (GE_EDITOR_DEBUG_PORT / --port, default 9999) — graceful IPC shutdown first, then force-kill of that port's holder only if it hangs. Editors on other ports (other worktrees, other sessions) are never touched, and a non-Editor port holder is never killed. Use it to release PDB locks before rebuilding; to stop a different editor, target its port with --port.",
    schema: {},
    run: async () => {
      const result = await freeDebugPort();
      if (result.blocked) throw new ToolError(result.blocked);
      return { data: { steps: result.steps } };
    },
  }),
];
