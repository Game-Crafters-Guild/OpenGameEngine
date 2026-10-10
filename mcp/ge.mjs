#!/usr/bin/env node
// Entry point for the GameEngine CLI.
//
//   node mcp/ge.mjs get_editor_state
//   node mcp/ge.mjs take_screenshot --target viewport
//   node mcp/ge.mjs get_log --count 50 --min-level warning
//   node mcp/ge.mjs raw open_scene '{"path":"scenes/main.scene"}'
//   node mcp/ge.mjs --help
//
// The CLI and the MCP server are two front-ends over one tool registry
// (mcp/src/tools/), so every MCP tool is a command here. Methods with no tool
// are reachable through `raw`.
//
// This file keeps mcp/dist in step with mcp/src before handing off, so editing
// a tool and running the CLI can never execute yesterday's definitions — the
// silent failure that makes a documented "remember to rebuild" step worthless.
// GE_CLI_NO_BUILD=1 skips the check (CI builds mcp/ explicitly);
// GE_CLI_FORCE_BUILD=1 forces one.

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const mcpDir = path.dirname(fileURLToPath(import.meta.url));
const cliEntry = path.join(mcpDir, "dist", "cli.js");
const lockDir = path.join(mcpDir, ".build-lock");

function mtimeMs(file) {
  try { return fs.statSync(file).mtimeMs; } catch { return 0; }
}

function newestMtime(dir) {
  let newest = 0;
  let entries;
  try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return 0; }
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    newest = Math.max(newest, entry.isDirectory() ? newestMtime(full) : mtimeMs(full));
  }
  return newest;
}

// Build chatter goes to stderr only: scripted callers parse stdout as JSON, and
// npm prints its own "> pkg@ver build" banner on STDOUT — inheriting it would
// corrupt the first thing a caller parses.
//
// Throws rather than exiting: the caller holds the build lock, and process.exit
// does not unwind through finally, so exiting here would strand the lock and
// stall every later invocation until the stale-lock timeout.
// shell on Windows: npm is npm.cmd, and spawning a .cmd needs a shell.
function run(cmd, args) {
  const result = spawnSync(cmd, args, {
    cwd: mcpDir,
    stdio: ["ignore", process.stderr, "inherit"],
    shell: process.platform === "win32",
  });
  if (result.error) {
    throw new Error(`[ge] ${cmd} ${args.join(" ")} failed to start: ${result.error.message}`);
  }
  if (result.status !== 0) {
    throw new Error(`[ge] ${cmd} ${args.join(" ")} failed (exit ${result.status ?? `signal ${result.signal}`}).`);
  }
}

function build() {
  console.error("[ge] mcp/dist is stale — building…");
  if (!fs.existsSync(path.join(mcpDir, "node_modules"))) run("npm", ["ci", "--no-audit", "--no-fund"]);
  run("npm", ["run", "build"]);
}

// Block the thread without an event-loop turn: the staleness check must finish
// before the CLI module is imported.
function sleepSync(ms) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

// A sweep script spawns one CLI per step; without a lock the first run after an
// edit fires N concurrent tsc runs into the same dist. mkdir is atomic, so the
// winner builds and the losers wait for it.
function buildOnce() {
  for (let attempt = 0; attempt < 600; attempt++) {
    try {
      fs.mkdirSync(lockDir);
    } catch (err) {
      if (err.code !== "EEXIST") throw err;
      // Someone else is building. Wait for them, then re-check staleness.
      const waited = Date.now() - mtimeMs(lockDir);
      if (waited > 300_000) { fs.rmSync(lockDir, { recursive: true, force: true }); continue; }
      sleepSync(250);
      if (!isStale()) return;
      continue;
    }
    try { build(); } finally { fs.rmSync(lockDir, { recursive: true, force: true }); }
    return;
  }
  console.error(`[ge] Timed out waiting for another build to finish. Remove ${lockDir} if it is stale.`);
  process.exit(1);
}

function isStale() {
  const srcMs = Math.max(
    newestMtime(path.join(mcpDir, "src")),
    mtimeMs(path.join(mcpDir, "package.json")),
    mtimeMs(path.join(mcpDir, "tsconfig.json")));
  return srcMs > mtimeMs(cliEntry);
}

if (process.env.GE_CLI_FORCE_BUILD === "1" || (process.env.GE_CLI_NO_BUILD !== "1" && isStale())) {
  try {
    buildOnce();
  } catch (err) {
    console.error(err.message);
    process.exit(1);
  }
}

if (!fs.existsSync(cliEntry)) {
  console.error(`[ge] ${cliEntry} is missing. Build it with: npm --prefix mcp ci && npm --prefix mcp run build`);
  process.exit(1);
}

await import(pathToFileURL(cliEntry).href);
