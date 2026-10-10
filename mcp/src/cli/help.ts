import { ipcConfig } from "../config.js";
import { kCategoryTitles, type ToolCategory, type ToolDef } from "../registry.js";
import { allTools } from "../tools/index.js";
import { argSpecs, type ArgSpec } from "./args.js";

// Help is generated from the registry and the tools' JSON Schemas, so a new
// tool appears here without anyone editing a list.

const kUsage = `Usage: ge <tool> [--flag value ...]
       ge <tool> '{"key":"value"}'    parameters as one JSON object
       ge <tool> --help               parameters for one tool
       ge raw <method> ['{...}']      call a debug-server method directly
       ge batch                       ndjson {"method","params"} per stdin line
       ge list [--json]               every tool name, one per line`;

function globalFlags(): string {
  const cfg = ipcConfig();
  return `Global flags:
  --port <n>        editor debug port (default ${cfg.port})
  --host <h>        editor debug host (default ${cfg.host})
  --timeout <ms>    per-request watchdog (default ${cfg.timeoutMs})
  --json            machine-readable envelope on stdout
  --dry-run         print the resolved parameters and exit without calling
  --quiet           suppress progress notes on stderr

Environment:
  GE_EDITOR_DEBUG_PORT   default port — bench/worktree editors run on their own
  GE_EDITOR_DEBUG_HOST   default host (default 127.0.0.1)
  IPC_TIMEOUT_MS         default watchdog in ms
  GE_CLI_NO_BUILD=1      skip the staleness check in mcp/ge.mjs`;
}

// "e.g." is not the end of a sentence — splitting there truncates a summary
// mid-clause, which is what a naive `split(/(?<=\.)\s/)` does.
const kAbbreviation = /\b(e\.g|i\.e|etc|vs|approx|cf)\.$/i;
const kSummaryWidth = 100;

function summarize(description: string): string {
  const parts = description.split(/(?<=\.)\s+/);
  let out = parts[0];
  for (let i = 1; i < parts.length && kAbbreviation.test(out); i++) out += ` ${parts[i]}`;
  if (out.length <= kSummaryWidth) return out;
  const cut = out.lastIndexOf(" ", kSummaryWidth);
  return `${out.slice(0, cut > 0 ? cut : kSummaryWidth)}…`;
}

function describeKind(spec: ArgSpec): string {
  switch (spec.kind) {
    case "enum": return spec.choices!.join("|");
    case "boolean": return "true|false";
    case "number": return "<number>";
    case "vector": return `<${spec.components!.join(",")}>`;
    case "array": return spec.fixedLength ? `<${"n,".repeat(spec.fixedLength).slice(0, -1)}>` : "<a,b,...>";
    case "objectArray": return `<${spec.itemFields!.join(":")}>`;
    case "map": return "<key=value>";
    case "union": return spec.acceptsList ? "<value|a,b>" : "<value>";
    default: return "<value>";
  }
}

/** The extra line a parameter needs when its syntax isn't obvious from the type. */
function usageHint(spec: ArgSpec): string | null {
  switch (spec.kind) {
    case "vector": {
      const sample = spec.components!.map((_, i) => i + 1).join(",");
      return `${spec.flag} ${sample}   or   ${spec.flag} '{"y":5}'`;
    }
    case "objectArray":
      return `${spec.flag} ${spec.itemFields!.join(":")} (repeatable)   or   ${spec.flag} '[{...}]'`;
    case "map":
      return `${spec.flag} key=value (repeatable)   or   ${spec.flag} '{"key":"value"}'`;
    case "array":
      return spec.fixedLength
        ? `${spec.flag} 1,2,3`
        : `${spec.flag} a --${spec.flag.slice(2)} b   or   ${spec.flag} a,b`;
    case "boolean":
      return `${spec.flag} / --no-${spec.flag.slice(2)} (omit to leave unchanged)`;
    default:
      return null;
  }
}

export function toolHelp(tool: ToolDef): string {
  const specs = argSpecs(tool);
  const lines = [`${tool.name} — ${kCategoryTitles[tool.category]}`, "", tool.description, ""];

  if (specs.length === 0) {
    lines.push("Takes no parameters.");
    return lines.join("\n");
  }

  lines.push("Parameters:");
  const labels = specs.map((s) => `${s.flag} ${describeKind(s)}`);
  const width = Math.max(...labels.map((l) => l.length));
  specs.forEach((spec, i) => {
    const tag = spec.required ? "(required) " : "";
    lines.push(`  ${labels[i].padEnd(width)}  ${tag}${spec.description}`);
    const hint = usageHint(spec);
    if (hint) lines.push(`  ${" ".repeat(width)}  e.g. ${hint}`);
  });

  if (tool.ipcMethod && tool.ipcMethod !== tool.name) {
    lines.push("", `Debug-server method: ${tool.ipcMethod}`);
  }
  return lines.join("\n");
}

export function overviewHelp(): string {
  const byCategory = new Map<ToolCategory, ToolDef[]>();
  for (const tool of allTools) {
    const list = byCategory.get(tool.category) ?? [];
    list.push(tool);
    byCategory.set(tool.category, list);
  }

  const width = Math.max(...allTools.map((t) => t.name.length));
  const lines = [kUsage, "", globalFlags(), "", `Tools (${allTools.length}):`];

  for (const category of Object.keys(kCategoryTitles) as ToolCategory[]) {
    const tools = byCategory.get(category);
    if (!tools?.length) continue;
    lines.push("", `  ${kCategoryTitles[category]}`);
    // First sentence only — the full text is one `ge <tool> --help` away.
    for (const tool of tools) lines.push(`    ${tool.name.padEnd(width)}  ${summarize(tool.description)}`);
  }

  lines.push(
    "",
    "Methods with no tool (open_scene, physics_raycast, get_asset_list, …) are reachable",
    "with `ge raw <method>`; Tests/Mcp/ipc-method-drift.mjs lists them and why.");
  return lines.join("\n");
}
