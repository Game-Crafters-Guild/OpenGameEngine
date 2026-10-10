import { spawn } from "child_process";
import * as fs from "fs";
import * as path from "path";
import { ipc } from "../ipc-singleton.js";
import { repoRoot } from "../paths.js";

/** First directory on PATH holding `name`, joined; null when absent. */
function findOnPath(name: string): string | null {
  for (const dir of (process.env.PATH ?? "").split(path.delimiter)) {
    if (!dir) continue;
    const candidate = path.join(dir, name);
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

/**
 * Where renderdoccmd lives, or null when it cannot be found. RENDERDOC_PATH may
 * name either the executable or the install directory.
 *
 * Resolved to an absolute path rather than left to the OS: the editor is
 * spawned detached with stdio ignored, so an unresolvable command would fail
 * silently and only surface as a wait_for_editor timeout.
 */
export function resolveRenderdoccmd(): string | null {
  const exeName = process.platform === "win32" ? "renderdoccmd.exe" : "renderdoccmd";
  const fromEnv = process.env.RENDERDOC_PATH;
  if (fromEnv) {
    const candidate = /renderdoccmd(\.exe)?$/i.test(fromEnv) ? fromEnv : path.join(fromEnv, exeName);
    return fs.existsSync(candidate) ? candidate : null;
  }
  if (process.platform === "win32") {
    const standard = "C:\\Program Files\\RenderDoc\\renderdoccmd.exe";
    if (fs.existsSync(standard)) return standard;
  }
  return findOnPath(exeName);
}

/** Capture destination when the caller does not name one. */
export function defaultCaptureDir(exe: string): string {
  return path.join(path.dirname(exe), "Captures");
}

export interface RenderDocLaunch {
  /** Directory the .rdc files land in. The template stem is `editor`. */
  captureDir: string;
  /** Path to renderdoccmd, from resolveRenderdoccmd(). */
  renderdoccmd: string;
  /**
   * Set GE_VK_CAPTURE_COMPAT=1 for the session. On by default because captures
   * taken with VK_EXT_descriptor_buffer routinely fail to replay — but it also
   * makes HZB occlusion culling report zero culled draws, so turn it off when
   * the capture is for occlusion or perf work.
   */
  captureCompat: boolean;
}

// Launch the staged Editor detached. Editor startup supplies a safe user-writable
// fallback workspace when no --project argument is present; never point project
// state at the executable directory because that mutates and invalidates bundles.
export function launchEditorProcess(
  exe: string,
  extraArgs: string[] = [],
  renderdoc?: RenderDocLaunch,
): { pid: number | undefined; logFile: string; renderdoccmdLog?: string } {
  const exeDir = path.dirname(exe);
  const logDir = path.join(repoRoot(), "build", "mcp-editor-logs");
  fs.mkdirSync(logDir, { recursive: true });
  const logFile = path.join(logDir, "Editor.log");
  const args = ["-logfile", logFile, ...extraArgs];

  if (renderdoc) {
    fs.mkdirSync(renderdoc.captureDir, { recursive: true });

    // renderdoccmd reports a failed launch/inject only on its own stdio, and it
    // is spawned detached — discarding that leaves a failed injection looking
    // like a successful launch until wait_for_editor eventually times out.
    const renderdoccmdLog = path.join(logDir, "renderdoccmd.log");
    const logFd = fs.openSync(renderdoccmdLog, "w");

    // The editor may rewrite RenderDoc's path template on startup, so it is told
    // the same directory as renderdoccmd — otherwise the two disagree and the
    // .rdc lands somewhere the caller is not looking.
    const env: NodeJS.ProcessEnv = { ...process.env, GE_RENDERDOC_CAPTURE_DIR: renderdoc.captureDir };
    if (renderdoc.captureCompat) env.GE_VK_CAPTURE_COMPAT = "1";

    const child = spawn(
      renderdoc.renderdoccmd,
      ["capture", "--working-dir", exeDir, "--capture-file", path.join(renderdoc.captureDir, "editor"), exe, ...args],
      { cwd: exeDir, detached: true, stdio: ["ignore", logFd, logFd], env },
    );
    child.unref();
    fs.closeSync(logFd);
    return { pid: child.pid, logFile, renderdoccmdLog };
  }

  // macOS .app bundles must be launched via `open` for a stable GUI session.
  const isMacAppBundle = process.platform === "darwin" && exe.includes(".app/Contents/MacOS/");
  let child: ReturnType<typeof spawn>;
  if (isMacAppBundle) {
    const appBundle = path.resolve(exe, "..", "..", "..");
    child = spawn("open", ["-a", appBundle, "--args", ...args], {
      cwd: exeDir,
      detached: true,
      stdio: "ignore",
    });
  } else {
    child = spawn(exe, args, {
      cwd: exeDir,
      detached: true,
      stdio: "ignore",
    });
  }
  child.unref();
  return { pid: child.pid, logFile };
}

// Poll until the editor's debug server answers get_editor_state, or time out.
export async function waitForEditorReady(timeoutMs: number): Promise<{ waitedMs: number; state: unknown } | null> {
  const client = ipc();
  const start = Date.now();
  const pollInterval = 500;
  // Short per-probe timeout so one attempt against a listening-but-not-yet-
  // pumping editor can't consume the caller's whole budget.
  const probeTimeoutMs = 3000;
  while (Date.now() - start < timeoutMs) {
    try {
      // Drop any stale connection first, then try fresh.
      if (client.isConnected) client.disconnect();
      await client.connect();
      const state = await client.request("get_editor_state", {}, probeTimeoutMs);
      return { waitedMs: Date.now() - start, state };
    } catch {
      client.disconnect();
      await new Promise(r => setTimeout(r, pollInterval));
    }
  }
  return null;
}
