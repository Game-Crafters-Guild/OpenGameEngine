import { execFile, spawn } from "child_process";
import * as fs from "fs";
import * as path from "path";
import { promisify } from "util";
import { repoRoot } from "../paths.js";
import { ToolError } from "../registry.js";

const execFileAsync = promisify(execFile);

/**
 * Orchestration for Radeon GPU Profiler captures of a running editor.
 *
 * UNVERIFIED ON AMD HARDWARE. Every function here was written and reviewed on
 * an NVIDIA machine, where the vendor gate refuses before any of it runs. The
 * refusal path is tested; the AMD-side flow — service handshake, panel attach,
 * trigger, and .rgp output location — has never been executed. Treat the CLI
 * invocations as a starting point to verify, not as known-good.
 *
 * Shape of the flow: RadeonDeveloperServiceCLI is a background broker that the
 * developer-mode driver reports into; RadeonDeveloperPanelCLI connects to that
 * broker to list processes and trigger captures. The service must therefore be
 * running before a capture is possible.
 */

/** Default port RadeonDeveloperServiceCLI listens on for panel connections. */
export const kDefaultServicePort = 27300;

/** Windows install root of the Radeon Developer Tool Suite. */
const kWindowsSuiteRoot = "C:\\Program Files\\AMD\\Radeon Developer Tool Suite";

/** Where to get the suite when it is missing. */
const kRdpDownloadUrl = "https://gpuopen.com/rdp/";

const kServiceCliBase = "RadeonDeveloperServiceCLI";
const kPanelCliBase = "RadeonDeveloperPanelCLI";

function exeName(base: string): string {
  return process.platform === "win32" ? `${base}.exe` : base;
}

/** Resolved locations of the two CLIs the capture flow drives. */
export interface RgpClis {
  serviceCli: string;
  panelCli: string;
  /** Directory both were found in, for error messages and the followup doc. */
  suiteDir: string;
  /** Which resolution step won: "RDP_PATH", "install-dir", or "PATH". */
  source: string;
}

function bothClisIn(dir: string): { serviceCli: string; panelCli: string } | null {
  const serviceCli = path.join(dir, exeName(kServiceCliBase));
  const panelCli = path.join(dir, exeName(kPanelCliBase));
  return fs.existsSync(serviceCli) && fs.existsSync(panelCli) ? { serviceCli, panelCli } : null;
}

/**
 * Order two version-ish directory names newest-first by numeric component.
 *
 * Lexical ordering gets this wrong the moment a component reaches two digits —
 * "1.10" sorts below "1.9" as text — which would prefer an older suite.
 */
function compareVersionsDesc(a: string, b: string): number {
  const componentsOf = (dir: string) =>
    path.basename(dir).split(/[^0-9]+/).filter(part => part.length > 0).map(Number);
  const left = componentsOf(a);
  const right = componentsOf(b);
  for (let i = 0; i < Math.max(left.length, right.length); i++) {
    const difference = (right[i] ?? 0) - (left[i] ?? 0);
    if (difference !== 0) return difference;
  }
  return 0;
}

/** Directories to try under the Windows install root, newest version first. */
function installRootCandidates(): string[] {
  if (!fs.existsSync(kWindowsSuiteRoot)) return [];
  let subdirs: string[] = [];
  try {
    subdirs = fs.readdirSync(kWindowsSuiteRoot, { withFileTypes: true })
      .filter(entry => entry.isDirectory())
      .map(entry => path.join(kWindowsSuiteRoot, entry.name))
      .sort(compareVersionsDesc);
  } catch {
    // An unreadable install root is just a miss — later steps still apply.
  }
  // The root itself comes first, for a flat (unversioned) install.
  return [kWindowsSuiteRoot, ...subdirs];
}

function pathEnvCandidates(): string[] {
  const raw = process.env.PATH ?? "";
  return raw.split(path.delimiter).filter(dir => dir.length > 0);
}

/**
 * Locate both CLIs: RDP_PATH (a directory or either exe) wins, then the Windows
 * install root and its versioned subdirectories, then PATH.
 */
export function resolveRgpClis(): RgpClis {
  const tried: string[] = [];

  const envPath = process.env.RDP_PATH;
  if (envPath) {
    const resolved = path.resolve(envPath);
    // Accept either the directory or one of the executables inside it.
    const dir = fs.existsSync(resolved) && fs.statSync(resolved).isDirectory()
      ? resolved
      : path.dirname(resolved);
    const found = bothClisIn(dir);
    if (found) return { ...found, suiteDir: dir, source: "RDP_PATH" };
    tried.push(`RDP_PATH -> ${dir}`);
  }

  for (const dir of installRootCandidates()) {
    const found = bothClisIn(dir);
    if (found) return { ...found, suiteDir: dir, source: "install-dir" };
    tried.push(dir);
  }

  for (const dir of pathEnvCandidates()) {
    const found = bothClisIn(dir);
    if (found) return { ...found, suiteDir: dir, source: "PATH" };
  }

  throw new ToolError(
    `Radeon Developer Tool Suite not found: need both ${exeName(kServiceCliBase)} and ` +
    `${exeName(kPanelCliBase)} in one directory. Install the suite from ${kRdpDownloadUrl}, ` +
    `or point RDP_PATH at the directory holding them.\n` +
    `Looked in:\n${tried.map(t => `  ${t}`).join("\n") || "  (no candidate directories existed)"}\n` +
    `PATH was also scanned (${pathEnvCandidates().length} entries).`);
}

/** Log directory for every process this module spawns. */
function rgpLogDir(): string {
  const dir = path.join(repoRoot(), "build", "mcp-rgp-logs");
  fs.mkdirSync(dir, { recursive: true });
  return dir;
}

/** True when a process whose image name matches `base` is already running. */
async function isProcessRunning(base: string): Promise<boolean> {
  try {
    if (process.platform === "win32") {
      const { stdout } = await execFileAsync(
        "tasklist", ["/FI", `IMAGENAME eq ${exeName(base)}`, "/NH"], { timeout: 15_000 });
      return stdout.toLowerCase().includes(base.toLowerCase());
    }
    const { stdout } = await execFileAsync("pgrep", ["-x", base], { timeout: 15_000 });
    return stdout.trim().length > 0;
  } catch {
    // pgrep exits non-zero when nothing matches, and a missing tasklist is not
    // worth failing the capture over — treat both as "not running" and let the
    // spawn below decide.
    return false;
  }
}

export interface ServiceState {
  alreadyRunning: boolean;
  pid?: number;
  logFile?: string;
}

/**
 * Make sure the service broker is up, spawning it detached if not.
 *
 * Output goes to a log file rather than being discarded: when the AMD-side flow
 * is finally exercised, the service's own diagnostics are the only record of
 * why a handshake failed, and stdio:"ignore" would throw them away.
 */
export async function ensureServiceRunning(clis: RgpClis): Promise<ServiceState> {
  if (await isProcessRunning(kServiceCliBase)) return { alreadyRunning: true };

  const logFile = path.join(rgpLogDir(), "RadeonDeveloperServiceCLI.log");
  const out = fs.openSync(logFile, "a");
  try {
    const child = spawn(clis.serviceCli, [], {
      cwd: clis.suiteDir,
      detached: true,
      stdio: ["ignore", out, out],
    });
    // A spawn failure is reported asynchronously on POSIX, so it has to be
    // awaited here: left unhandled it would surface after this function
    // returned, as an uncaught 'error' event rather than a refusal the caller
    // can act on.
    await new Promise<void>((resolve, reject) => {
      child.once("spawn", () => resolve());
      child.once("error", (err: Error) => reject(new ToolError(
        `Could not start ${exeName(kServiceCliBase)} from ${clis.suiteDir}: ${err.message}. ` +
        `Check that the Radeon Developer Tool Suite there is a real installation.`)));
    });
    child.unref();
    return { alreadyRunning: false, pid: child.pid, logFile };
  } finally {
    // The child holds its own duplicated handle once spawned.
    fs.closeSync(out);
  }
}

/**
 * THE UNVERIFIED INVOCATION — the one place the panel CLI's command line is
 * decided, kept in a single function so a first AMD run has exactly one thing
 * to correct.
 *
 * These flags are UNVERIFIED: they were not run against a real
 * RadeonDeveloperPanelCLI. Check them against the panel's own `--help` output
 * and the docs shipped in the RDP release (the suite's `docs/` directory, and
 * the "Radeon Developer Panel" help in the install root) before trusting a
 * result from this tool. The open questions are the spelling of each flag,
 * where the panel writes the .rgp, and the start-order requirement below.
 *
 * Also unverified, and more likely to bite than the spelling of a flag: RGP
 * captures normally require the target to have been started while the service
 * was already running, because the developer-mode driver reports processes into
 * a broker that must exist first. Attaching to an editor that predates the
 * service may simply not be capturable, in which case the fix is to relaunch
 * the editor after ensureServiceRunning rather than to change these arguments.
 */
export function panelCaptureArgs(options: {
  processName: string;
  outputDir: string;
  durationSec: number;
  servicePort: number;
}): string[] {
  return [
    "--connect", `localhost:${options.servicePort}`,
    "--process", options.processName,
    "--capture-type", "rgp",
    "--capture-duration", String(options.durationSec),
    "--output-dir", options.outputDir,
    "--trigger-capture",
  ];
}

export interface PanelRunResult {
  args: string[];
  exitCode: number | null;
  logFile: string;
  stdout: string;
  stderr: string;
}

/** Run the panel CLI to completion, tee-ing its output to a log file. */
export async function runPanelCapture(clis: RgpClis, args: string[], timeoutMs: number): Promise<PanelRunResult> {
  const logFile = path.join(rgpLogDir(), "RadeonDeveloperPanelCLI.log");
  let stdout = "";
  let stderr = "";
  let exitCode: number | null = null;
  try {
    const result = await execFileAsync(clis.panelCli, args, {
      cwd: clis.suiteDir,
      timeout: timeoutMs,
      maxBuffer: 8 * 1024 * 1024,
    });
    stdout = result.stdout;
    stderr = result.stderr;
    exitCode = 0;
  } catch (err: any) {
    stdout = err?.stdout ?? "";
    stderr = err?.stderr ?? String(err?.message ?? err);
    exitCode = typeof err?.code === "number" ? err.code : null;
  }
  fs.appendFileSync(
    logFile,
    `\n=== ${new Date().toISOString()} ${clis.panelCli} ${args.join(" ")} (exit ${exitCode}) ===\n` +
    `${stdout}\n${stderr}\n`);
  return { args, exitCode, logFile, stdout, stderr };
}

export interface RgpFile {
  filePath: string;
  sizeBytes: number;
  modifiedMs: number;
}

function listRgpFiles(dir: string): RgpFile[] {
  let entries: string[];
  try {
    entries = fs.readdirSync(dir);
  } catch {
    return [];
  }
  const files: RgpFile[] = [];
  for (const entry of entries) {
    if (!entry.toLowerCase().endsWith(".rgp")) continue;
    const filePath = path.join(dir, entry);
    try {
      const stat = fs.statSync(filePath);
      if (stat.isFile()) files.push({ filePath, sizeBytes: stat.size, modifiedMs: stat.mtimeMs });
    } catch {
      // Vanished between readdir and stat.
    }
  }
  return files;
}

/** Every .rgp already in `dir`, keyed by path, taken before a capture runs. */
export function snapshotRgpFiles(dir: string): Map<string, number> {
  const snapshot = new Map<string, number>();
  for (const file of listRgpFiles(dir)) snapshot.set(file.filePath, file.modifiedMs);
  return snapshot;
}

/**
 * Newest .rgp that is absent from `baseline`, or whose mtime advanced past it.
 *
 * Identity, not a wall-clock floor: comparing mtimes against `Date.now()` would
 * assume the filesystem's timestamp granularity and the system clock agree
 * closely enough to separate "written just now" from "already there", which
 * coarse-granularity filesystems and clock adjustments break. Comparing a file
 * against its own earlier state needs neither to hold. Without this, a stale
 * capture sitting in the output directory could be reported as this run's
 * result — the exact false pass an unverified flow must not produce.
 */
export function newestRgpSince(dir: string, baseline: Map<string, number>): RgpFile | null {
  let best: RgpFile | null = null;
  for (const file of listRgpFiles(dir)) {
    const previous = baseline.get(file.filePath);
    if (previous !== undefined && file.modifiedMs <= previous) continue;
    if (!best || file.modifiedMs > best.modifiedMs) best = file;
  }
  return best;
}

/** Default capture output directory when the caller names none. */
export function defaultOutputDir(): string {
  return path.join(repoRoot(), "build", "rgp-captures");
}
