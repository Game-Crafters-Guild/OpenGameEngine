// Drift guard between the editor's debug-server handlers and the agent tool
// registry. The C++ side (Apps/Editor/Source/DebugServer/*.cpp, RegisterHandler)
// is authoritative; mcp/src/tools/*.ts is what MCP and the CLI expose. Before
// this check the two lists — plus a help catalog and a spec doc — drifted
// independently for months.
//
// Both sides are read as TEXT, never imported or executed, so this runs in CI
// before anything is built. That works only because ToolDef carries ipcMethod
// as a literal data field rather than a value computed inside run().
//
// A handler with no tool is not a bug by itself — some are deliberately
// raw-only. It must be listed in ipc-method-coverage.json WITH A REASON, so
// adding a handler forces a "tool or raw-only?" decision at the moment it is
// written instead of leaving a silent gap.
//
// Exit 0 = registry and handlers agree; 1 = drift; 2 = usage/IO error.
//
//   node Tests/Mcp/ipc-method-drift.mjs           check
//   node Tests/Mcp/ipc-method-drift.mjs --list    print every method + coverage

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const handlerDir = path.join(repoRoot, 'Apps', 'Editor', 'Source', 'DebugServer');
const toolsDir = path.join(repoRoot, 'mcp', 'src', 'tools');
const coveragePath = path.join(repoRoot, 'mcp', 'ipc-method-coverage.json');

const listOnly = process.argv[2] === '--list';
if (process.argv.length > 3 || (process.argv.length === 3 && !listOnly)) {
    console.error('usage: node ipc-method-drift.mjs [--list]');
    process.exit(2);
}

function readAll(dir, suffix) {
    let entries;
    try {
        entries = fs.readdirSync(dir);
    } catch (err) {
        console.error(`cannot read ${dir}: ${err.message}`);
        process.exit(2);
    }
    return entries
        .filter(f => f.endsWith(suffix))
        .map(f => ({ file: f, text: fs.readFileSync(path.join(dir, f), 'utf8') }));
}

// `s` flag: BenchSceneHandlers.cpp splits the call across two lines, so a
// line-oriented match silently misses spawn_bench_scene.
const kRegisterHandler = /RegisterHandler\s*\(\s*"([A-Za-z_0-9]+)"/gs;
// proxyTool defaults ipcMethod to name, so a plain entry needs no ipcMethod
// field; `name:` immediately followed by `category:` identifies a tool.
const kToolName = /\bname:\s*"([a-z_0-9]+)"\s*,\s*\n?\s*category:/g;
const kIpcMethod = /\bipcMethod:\s*"([A-Za-z_0-9]+)"/g;
const kCoversMethods = /\bcoversIpcMethods:\s*\[([^\]]*)\]/g;

const handlers = new Map(); // method -> source file
for (const { file, text } of readAll(handlerDir, '.cpp')) {
    for (const m of text.matchAll(kRegisterHandler)) {
        if (!handlers.has(m[1])) handlers.set(m[1], file);
    }
}
if (handlers.size === 0) {
    console.error(`no RegisterHandler calls found under ${handlerDir} — the check would pass vacuously`);
    process.exit(2);
}

const toolNames = [];
const covered = new Map(); // method -> tool source file
for (const { file, text } of readAll(toolsDir, '.ts')) {
    for (const m of text.matchAll(kToolName)) {
        toolNames.push({ name: m[1], file });
        // A tool whose name is also a handler covers it, unless it names a
        // different ipcMethod — checked against the explicit list below.
        if (handlers.has(m[1]) && !covered.has(m[1])) covered.set(m[1], file);
    }
    for (const m of text.matchAll(kIpcMethod)) covered.set(m[1], file);
    for (const m of text.matchAll(kCoversMethods)) {
        for (const q of m[1].matchAll(/"([A-Za-z_0-9]+)"/g)) covered.set(q[1], file);
    }
}
if (toolNames.length === 0) {
    console.error(`no tool definitions found under ${toolsDir} — the check would pass vacuously`);
    process.exit(2);
}

let coverage;
try {
    coverage = JSON.parse(fs.readFileSync(coveragePath, 'utf8'));
} catch (err) {
    console.error(`cannot read ${path.relative(repoRoot, coveragePath)}: ${err.message}`);
    process.exit(2);
}
const rawOnly = coverage.rawOnly ?? {};
if (typeof rawOnly !== 'object' || Array.isArray(rawOnly)) {
    console.error(`${path.relative(repoRoot, coveragePath)} needs a "rawOnly" object of method -> reason`);
    process.exit(2);
}

if (listOnly) {
    const width = Math.max(...[...handlers.keys()].map(m => m.length));
    for (const method of [...handlers.keys()].sort()) {
        const via = covered.get(method);
        const state = via ? `tool (${via})` : rawOnly[method] ? `raw-only — ${rawOnly[method]}` : 'UNCLASSIFIED';
        console.log(`  ${method.padEnd(width)}  ${state}`);
    }
    process.exit(0);
}

const problems = [];

for (const [method, file] of handlers) {
    if (covered.has(method) || rawOnly[method]) continue;
    problems.push(
        `unclassified handler: ${method} (${file}) — expose it as a tool in mcp/src/tools/, ` +
        `or add it to mcp/ipc-method-coverage.json under "rawOnly" with a one-line reason.`);
}

for (const [method, file] of covered) {
    if (handlers.has(method)) continue;
    problems.push(
        `dead tool method: ${method} (${file}) — no RegisterHandler("${method}") exists in ` +
        `Apps/Editor/Source/DebugServer/. Fix the ipcMethod, or drop the tool.`);
}

for (const method of Object.keys(rawOnly)) {
    if (!handlers.has(method)) {
        problems.push(`stale allow-list entry: ${method} — listed as raw-only but no handler registers it; remove it from ipc-method-coverage.json.`);
    } else if (covered.has(method)) {
        problems.push(`redundant allow-list entry: ${method} — a tool covers it now; remove it from ipc-method-coverage.json.`);
    }
    if (typeof rawOnly[method] !== 'string' || rawOnly[method].trim() === '') {
        problems.push(`allow-list entry without a reason: ${method} — say why it has no tool.`);
    }
}

const seen = new Set();
for (const { name, file } of toolNames) {
    if (seen.has(name)) problems.push(`duplicate tool name: ${name} (${file}) — one would silently shadow the other.`);
    seen.add(name);
}

if (problems.length > 0) {
    console.error(`debug-server / tool-registry drift (${problems.length} problem${problems.length === 1 ? '' : 's'}):`);
    for (const p of problems) console.error(`  ${p}`);
    process.exit(1);
}

console.log(
    `ipc method coverage OK: ${handlers.size} debug-server methods — ` +
    `${covered.size} covered by tools, ${Object.keys(rawOnly).length} raw-only; ` +
    `${toolNames.length} tools in the registry`);
