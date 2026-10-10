import * as fs from "fs";
import * as path from "path";
import { repoRoot } from "./paths.js";

/**
 * Read the debug server's own documentation out of its source.
 *
 * The authoritative parameter docs for a debug-server method are the comments
 * above its RegisterHandler call — that is where they have always lived, and
 * docs/MCP_SETUP.md says so. Parsing them beats maintaining a second copy: a
 * transcribed list is exactly what drifted before.
 */

export interface DebugMethodDoc {
  method: string;
  file: string;
  line: number;
  /** The contiguous comment block above the registration, if any. */
  doc: string | null;
}

// `s` flag: BenchSceneHandlers.cpp splits the call across two lines, so a
// line-oriented match silently misses spawn_bench_scene.
const kRegisterHandler = /RegisterHandler\s*\(\s*"([A-Za-z_0-9]+)"/gs;

/**
 * Collect the comment block immediately above `index`, stopping at the first
 * line that is neither a `//` comment nor blank-adjacent to one. Section
 * banners (`// --- Asset Registry Debug Handlers ---`) are dropped: they
 * describe the neighbourhood, not the method.
 */
function commentAbove(text: string, index: number, method: string): string | null {
  const before = text.slice(0, index).split("\n");
  before.pop(); // the (partial) line the registration starts on
  const lines: string[] = [];
  for (let i = before.length - 1; i >= 0; i--) {
    const trimmed = before[i].trim();
    if (trimmed.startsWith("//")) {
      const body = trimmed.replace(/^\/\/\s?/, "");
      if (/^-{2,}.*-{2,}$/.test(body.trim())) break; // section banner
      lines.unshift(body);
      continue;
    }
    break;
  }
  while (lines.length > 0 && lines[0].trim() === "") lines.shift();
  if (lines.length === 0) return null;

  // Strip the hand-kept numbering ("30c.", "29a2.", "17.").
  lines[0] = lines[0].replace(/^\d+[a-z]?\d*\.\s*/, "");
  // The house style opens with the method's own name, either alone on the line
  // or as a "name — description" lede. Neither carries information next to the
  // name we already print, so drop the restatement, not the description.
  const lede = new RegExp(`^${method}\\s*(?:[—–:-]\\s*)?`, "i");
  if (lede.test(lines[0])) {
    lines[0] = lines[0].replace(lede, "").trim();
    if (lines[0] === "") lines.shift();
  }
  while (lines.length > 0 && lines[0].trim() === "") lines.shift();

  return lines.join("\n").trim() || null;
}

export function readDebugMethodDocs(): DebugMethodDoc[] {
  const dir = path.join(repoRoot(), "Apps", "Editor", "Source", "DebugServer");
  const out: DebugMethodDoc[] = [];
  const seen = new Set<string>();

  let entries: string[];
  try {
    entries = fs.readdirSync(dir).filter(f => f.endsWith(".cpp")).sort();
  } catch {
    return out;
  }

  for (const file of entries) {
    const text = fs.readFileSync(path.join(dir, file), "utf-8");
    for (const m of text.matchAll(kRegisterHandler)) {
      const method = m[1];
      if (seen.has(method)) continue;
      seen.add(method);
      out.push({
        method,
        file,
        line: text.slice(0, m.index).split("\n").length,
        doc: commentAbove(text, m.index!, method),
      });
    }
  }
  return out.sort((a, b) => a.method.localeCompare(b.method));
}

/** method -> why it deliberately has no typed tool. */
export function readRawOnlyReasons(): Record<string, string> {
  try {
    const file = path.join(repoRoot(), "mcp", "ipc-method-coverage.json");
    const parsed = JSON.parse(fs.readFileSync(file, "utf-8"));
    return parsed?.rawOnly ?? {};
  } catch {
    return {};
  }
}
