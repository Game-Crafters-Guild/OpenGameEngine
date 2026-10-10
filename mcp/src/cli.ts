#!/usr/bin/env node

import { overrideIpcConfig } from "./config.js";
import { editorCallCount, lastEditorEndpoint } from "./editor-call.js";
import { ipc } from "./ipc-singleton.js";
import { ToolError } from "./registry.js";
import { allTools, toolByName } from "./tools/index.js";
import { parseToolArgs } from "./cli/args.js";
import { overviewHelp, toolHelp } from "./cli/help.js";
import { kExitOk, kExitUsage, renderError, renderOutput, type RenderOptions } from "./cli/output.js";
import { runBatch, runRaw } from "./cli/raw.js";

// CLI front-end over the shared tool registry (src/tools/). Same definitions
// the MCP server publishes — this file only adapts them to argv and stdout.

interface GlobalOptions extends RenderOptions {
  dryRun: boolean;
  help: boolean;
}

/**
 * Pull global flags out of argv wherever they appear, leaving the tool's own
 * arguments behind. Connection overrides are applied immediately: the IPC
 * client reads the config when it connects, which has not happened yet.
 */
function takeGlobals(argv: string[]): { rest: string[]; opts: GlobalOptions } {
  const opts: GlobalOptions = { json: false, quiet: false, dryRun: false, help: false };
  const rest: string[] = [];

  const numeric = (flag: string, raw: string | undefined): number => {
    const n = Number(raw);
    if (!Number.isInteger(n) || n <= 0) throw new ToolError(`${flag} expects a positive integer, got '${raw}'.`, "BAD_ARGUMENT");
    return n;
  };

  for (let i = 0; i < argv.length; i++) {
    const eq = argv[i].indexOf("=");
    const name = eq >= 0 ? argv[i].slice(0, eq) : argv[i];
    const inline = eq >= 0 ? argv[i].slice(eq + 1) : undefined;
    const value = () => inline ?? argv[++i];

    switch (name) {
      case "--json": opts.json = true; break;
      case "--quiet": opts.quiet = true; break;
      case "--dry-run": opts.dryRun = true; break;
      case "--help": case "-h": opts.help = true; break;
      case "--port": overrideIpcConfig({ port: numeric("--port", value()) }); break;
      case "--timeout": overrideIpcConfig({ timeoutMs: numeric("--timeout", value()) }); break;
      case "--host": overrideIpcConfig({ host: value() }); break;
      default: rest.push(argv[i]);
    }
  }
  return { rest, opts };
}

async function main(): Promise<number> {
  let rest: string[], opts: GlobalOptions;
  try {
    ({ rest, opts } = takeGlobals(process.argv.slice(2)));
  } catch (err) {
    // A malformed global flag is a usage error (exit 2), same as a malformed
    // tool flag — not a tool failure.
    return renderError("ge", err, { json: false, quiet: false });
  }
  const command = rest[0];

  if (!command) {
    console.log(overviewHelp());
    return opts.help ? kExitOk : kExitUsage;
  }
  if (command === "help") {
    const topic = rest[1] ? toolByName(rest[1].replace(/-/g, "_")) : undefined;
    console.log(topic ? toolHelp(topic) : overviewHelp());
    return kExitOk;
  }
  if (command === "list") {
    if (opts.json) console.log(JSON.stringify(allTools.map(t => ({ name: t.name, category: t.category, ipcMethod: t.ipcMethod ?? null }))));
    else for (const tool of allTools) console.log(tool.name);
    return kExitOk;
  }
  if (command === "raw") return runRaw(rest.slice(1), opts);
  if (command === "batch") return runBatch(opts);

  if (command.startsWith("-")) {
    console.error(`Unknown option '${command}'.`);
    console.error(overviewHelp());
    return kExitUsage;
  }

  // Tool names are snake_case; kebab is accepted so either spelling works.
  const tool = toolByName(command.replace(/-/g, "_"));
  if (!tool) {
    console.error(`Unknown tool '${command}'. Run 'ge list' for the full set, or 'ge raw ${command}' to call it as a debug-server method.`);
    return kExitUsage;
  }

  if (opts.help) {
    console.log(toolHelp(tool));
    return kExitOk;
  }

  // Outside the try so the catch can separate "the editor refused" — an answer,
  // and attributable — from "nothing answered", which must claim nobody.
  const callsBefore = editorCallCount();
  const answeredEndpoint = () => (editorCallCount() > callsBefore ? lastEditorEndpoint() : null);
  try {
    const { params } = parseToolArgs(tool, rest.slice(1));
    if (opts.dryRun) {
      console.log(JSON.stringify({ tool: tool.name, ipcMethod: tool.ipcMethod ?? null, params }, null, 2));
      return kExitOk;
    }
    const out = await tool.run(params);
    // Attributed only when this invocation reached an editor — build and
    // codegen tools answer without one.
    renderOutput(tool.name, out, opts, answeredEndpoint());
    return kExitOk;
  } catch (err) {
    return renderError(tool.name, err, opts, answeredEndpoint());
  }
}

main()
  .then((code) => { process.exitCode = code; })
  .catch((e) => {
    console.error(e?.message ?? String(e));
    process.exitCode = 1;
  })
  // A connected socket is an open handle: without this the process prints its
  // result and then hangs forever instead of exiting. Setting exitCode rather
  // than calling process.exit() still lets buffered stdout drain to a pipe.
  .finally(() => { ipc().disconnect(); });
