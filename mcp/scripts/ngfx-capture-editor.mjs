#!/usr/bin/env node
//
// Launch the staged Editor under NVIDIA Nsight Graphics.
//
// The in-app capture SDK cannot inject itself: ngfx-capture.exe has to start the
// process so the capture libraries are loaded before the Vulkan device exists.
// This script resolves both executables, sets the capture-friendly environment,
// and hands the editor's own arguments through.
//
// Usage:
//   node mcp/scripts/ngfx-capture-editor.mjs [options] [-- <editor args>]
//
// Options:
//   --exe <path>          Editor executable (default: the staged build)
//   --preset <name>       Build preset for the default editor path
//   --config <name>       Build config for the default editor path (default DebugFast)
//   --output-dir <path>   Where Nsight writes captures (default: scratch dir)
//   --no-capture-compat   Do not set GE_VK_CAPTURE_COMPAT=1
//   --help
//
// Example:
//   node mcp/scripts/ngfx-capture-editor.mjs -- --debug-port 9905

import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");

const NGFX_RELATIVE_EXE = path.join("host", "windows-desktop-nomad-x64", "ngfx-capture.exe");
const PROGRAM_FILES_NVIDIA = "C:\\Program Files\\NVIDIA Corporation";
const DEFAULT_CONFIG = "DebugFast";
const DEFAULT_PRESET = "vs2022-x64-local";

function fail(message) {
  console.error(`ngfx-capture-editor: ${message}`);
  process.exit(1);
}

function parseArgs(argv) {
  const opts = { captureCompat: true };
  const editorArgs = [];
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === "--") { editorArgs.push(...argv.slice(i + 1)); break; }
    else if (arg === "--exe") opts.exe = argv[++i];
    else if (arg === "--preset") opts.preset = argv[++i];
    else if (arg === "--config") opts.config = argv[++i];
    else if (arg === "--output-dir") opts.outputDir = argv[++i];
    else if (arg === "--no-capture-compat") opts.captureCompat = false;
    else if (arg === "--help" || arg === "-h") opts.help = true;
    else fail(`unknown option '${arg}' (editor arguments go after '--')`);
  }
  return { opts, editorArgs };
}

/**
 * Sort "Nsight Graphics 2026.3.1"-style directory names newest-first, comparing
 * version segments numerically so 2026.10 outranks 2026.3.
 */
function byVersionDesc(a, b) {
  const parse = (name) => (name.match(/(\d+)/g) ?? []).map(Number);
  const va = parse(a);
  const vb = parse(b);
  for (let i = 0; i < Math.max(va.length, vb.length); i++) {
    const diff = (vb[i] ?? -1) - (va[i] ?? -1);
    if (diff !== 0) return diff;
  }
  return b.localeCompare(a);
}

function resolveNgfxCapture() {
  const fromEnv = process.env.NSIGHT_GRAPHICS_PATH;
  if (fromEnv) {
    // Accept either the install root or the executable itself.
    const candidate = fromEnv.toLowerCase().endsWith(".exe") ? fromEnv : path.join(fromEnv, NGFX_RELATIVE_EXE);
    if (!fs.existsSync(candidate)) fail(`NSIGHT_GRAPHICS_PATH is set but ${candidate} does not exist`);
    return candidate;
  }

  let entries = [];
  try {
    entries = fs.readdirSync(PROGRAM_FILES_NVIDIA, { withFileTypes: true })
      .filter((e) => e.isDirectory() && e.name.startsWith("Nsight Graphics"))
      .map((e) => e.name)
      .sort(byVersionDesc);
  } catch {
    fail(`cannot read ${PROGRAM_FILES_NVIDIA} — is Nsight Graphics installed? Set NSIGHT_GRAPHICS_PATH to override.`);
  }

  for (const name of entries) {
    const candidate = path.join(PROGRAM_FILES_NVIDIA, name, NGFX_RELATIVE_EXE);
    if (fs.existsSync(candidate)) return candidate;
  }
  fail(`no ngfx-capture.exe under ${PROGRAM_FILES_NVIDIA}. Set NSIGHT_GRAPHICS_PATH to the install directory.`);
}

/**
 * Default to the staged editor for a preset/config, mirroring how the MCP build
 * helpers locate it (mcp/src/build/preset.ts editorExePath).
 */
function resolveEditor(opts) {
  if (opts.exe) {
    if (!fs.existsSync(opts.exe)) fail(`--exe ${opts.exe} does not exist`);
    return path.resolve(opts.exe);
  }

  const config = opts.config ?? DEFAULT_CONFIG;
  const buildRoot = path.join(repoRoot, "build");
  // With several build directories present, directory order is arbitrary and
  // would pick a different editor run to run. Try the daily-driver preset
  // first, then the rest in a stable order. --preset pins one exactly.
  const presets = opts.preset ? [opts.preset] : (() => {
    let dirs = [];
    try {
      dirs = fs.readdirSync(buildRoot, { withFileTypes: true }).filter((e) => e.isDirectory()).map((e) => e.name).sort();
    } catch {
      return [];
    }
    return [...dirs.filter((d) => d === DEFAULT_PRESET), ...dirs.filter((d) => d !== DEFAULT_PRESET)];
  })();

  const tried = [];
  for (const preset of presets) {
    const candidate = path.join(buildRoot, preset, "bin", config, "Apps", "Editor", "Editor.exe");
    tried.push(candidate);
    if (fs.existsSync(candidate)) return candidate;
  }
  fail(`no staged Editor.exe found for config '${config}'. Pass --exe, or --preset/--config.\nTried:\n  ${tried.join("\n  ") || "(no build directories)"}`);
}

const { opts, editorArgs } = parseArgs(process.argv.slice(2));
if (opts.help) {
  // The banner is the comment block at the top of this file, ending at the
  // first line of code. Selecting every "//" line instead would print the
  // implementation comments further down as if they were documentation.
  const lines = fs.readFileSync(fileURLToPath(import.meta.url), "utf8").split(/\r?\n/).slice(1);
  const banner = [];
  for (const line of lines) {
    if (!line.startsWith("//")) break;
    banner.push(line.replace(/^\/\/ ?/, ""));
  }
  console.log(banner.join("\n").trim());
  process.exit(0);
}

if (process.platform !== "win32") fail("Nsight Graphics CLI capture is Windows-only in this engine");

const ngfxCapture = resolveNgfxCapture();
const editorExe = resolveEditor(opts);
const editorDir = path.dirname(editorExe);
const outputDir = path.resolve(opts.outputDir ?? path.join(os.tmpdir(), "ngfx-captures"));
fs.mkdirSync(outputDir, { recursive: true });

// ngfx-capture takes the target's arguments and environment through its own
// --args / --env options; it does not forward trailing positionals.
//
// Each editor argument goes in its own `--args=<value>`. The space-separated
// form silently truncates: ngfx-capture's parser force-consumes one dashed
// token after --args but stops at the next, so `--args --debug-port 9905
// --project X` drops everything from --project on and fails the launch. The
// `=` form forces each token to be read as a value, and repeats accumulate.
const args = ["--exe", editorExe, "--wd", editorDir, "--output-dir", outputDir];
if (opts.captureCompat) {
  // VK_EXT_descriptor_buffer is the extension capture tools struggle with most
  // here; GE_VK_CAPTURE_COMPAT=1 drops it plus acceleration structures for the
  // session (see IsCaptureCompatRequested in VulkanDevice.cpp). Opt out with
  // --no-capture-compat to capture the shipping descriptor path.
  args.push("--env=GE_VK_CAPTURE_COMPAT=1");
}
for (const editorArg of editorArgs) args.push(`--args=${editorArg}`);

console.log(`ngfx-capture : ${ngfxCapture}`);
console.log(`editor       : ${editorExe}`);
console.log(`working dir  : ${editorDir}`);
console.log(`captures     : ${outputDir}`);
console.log(`capture compat: ${opts.captureCompat ? "GE_VK_CAPTURE_COMPAT=1" : "off"}`);
if (editorArgs.length) console.log(`editor args  : ${editorArgs.join(" ")}`);

const child = spawn(ngfxCapture, args, { stdio: "inherit" });
child.on("error", (err) => fail(`failed to start ngfx-capture.exe: ${err.message}`));
child.on("exit", (code, signal) => process.exit(signal ? 1 : (code ?? 0)));
