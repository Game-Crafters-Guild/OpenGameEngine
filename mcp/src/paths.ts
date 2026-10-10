import { fileURLToPath } from "node:url";
import * as fs from "fs";
import * as path from "path";
import { ToolError } from "./registry.js";

// The repo root is found by MARKER, not by a fixed number of directory hops:
// this module runs from three different places — mcp/dist/ during development
// (the MCP host may start the server from anywhere; Cursor uses $HOME), the
// standalone gameenginecli binary staged under build/<preset>/bin/<config>/,
// and that same binary copied to an arbitrary directory. A hop count is right
// for exactly one of those. Resolution order:
//   1. GAMEENGINE_ROOT (explicit override, always wins)
//   2. walk up from this module / the packaged executable
//   3. walk up from the working directory
// In the packaged binary import.meta.url resolves to the executable path, so
// (2) covers a repo-staged exe and (3) covers a relocated exe run from inside
// a checkout. Tools that need no repo (the IPC proxies) never call this.
function isRepoRoot(dir: string): boolean {
  return fs.existsSync(path.join(dir, "CMakePresets.json"))
    && fs.existsSync(path.join(dir, "Engine"))
    && fs.existsSync(path.join(dir, "Apps"));
}

function findRepoRootFrom(start: string): string | null {
  let dir = start;
  for (;;) {
    if (isRepoRoot(dir)) return dir;
    const parent = path.dirname(dir);
    if (parent === dir) return null;
    dir = parent;
  }
}

const kModuleDir = path.dirname(fileURLToPath(import.meta.url));
const kEnvRoot: string | null = process.env.GAMEENGINE_ROOT
  ? path.resolve(process.env.GAMEENGINE_ROOT)
  : null;
const kRepoRoot: string | null =
  kEnvRoot ?? findRepoRootFrom(kModuleDir) ?? findRepoRootFrom(process.cwd());

export function repoRoot(): string {
  // An explicit override that points at a non-repo errors loudly rather than
  // falling back to discovery or surfacing later as a confusing ENOENT.
  if (kEnvRoot && !isRepoRoot(kEnvRoot)) {
    throw new Error(
      `GAMEENGINE_ROOT is set to '${kEnvRoot}', which is not a GameEngine repository ` +
      "(expected CMakePresets.json, Engine/ and Apps/ there).");
  }
  if (!kRepoRoot) {
    throw new Error(
      "This command needs the GameEngine repository, and none was found above the executable " +
      "or the working directory. Run it from inside the repo, or set GAMEENGINE_ROOT.");
  }
  return kRepoRoot;
}

// The open project's root when the editor's AI Assistant started this server
// (GE_PROJECT_ROOT); the content tools then read and write inside it and nowhere else.
// Set but empty means the editor has no project open, and the content tools refuse.
const kProjectRootSetting: string | undefined = process.env.GE_PROJECT_ROOT;
const kProjectRoot: string | null = kProjectRootSetting ? path.resolve(kProjectRootSetting) : null;

/** The root a content path (a script directory, a graph, generated code) resolves against. */
export function contentRoot(): string {
  if (kProjectRootSetting === "") {
    throw new ToolError(
      "No project is open in the editor, so the assistant has no project files to read or write: open a project, then ask again.",
      "TOOL_FAILED");
  }
  return kProjectRoot ?? repoRoot();
}

/**
 * Resolves a content path argument: relative to contentRoot(), an absolute path as
 * given. Under GE_PROJECT_ROOT a path outside the project is refused, before
 * anything reads or writes it.
 */
export function resolveContentPath(input: string): string {
  const full = path.resolve(contentRoot(), input);
  if (kProjectRoot) {
    const relative = path.relative(kProjectRoot, full);
    if (relative.startsWith("..") || path.isAbsolute(relative)) {
      throw new ToolError(
        `'${input}' is outside the project (${kProjectRoot}): the assistant reads and writes project files only; ` +
        "use a path inside the project, relative to its root.", "BAD_ARGUMENT");
    }
  }
  return full;
}

/** Default scripts directory: <content root>/Assets/Scripts */
export function defaultScriptsDir(): string {
  return path.join(contentRoot(), "Assets", "Scripts");
}

export function ensureDir(dir: string): void {
  fs.mkdirSync(dir, { recursive: true });
}
