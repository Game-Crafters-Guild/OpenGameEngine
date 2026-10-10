// rgp_capture must refuse on non-AMD hardware, and say what it actually found.
//
// The tool orchestrates a Radeon GPU Profiler capture. Its AMD-side flow is
// UNVERIFIED — written on an NVIDIA machine, never executed on Radeon — so the
// one behaviour that must not rot is the gate that stops it from being run
// somewhere it cannot possibly work, and the honesty of what it says when it
// does refuse. A gate that silently degraded to "attempt anyway" would spawn
// vendor CLIs on a machine with no Radeon driver and report a confusing
// failure instead of a clear one.
//
// The editor is faked here: a TCP server speaking the debug protocol's
// line-delimited JSON, answering get_gpu_tooling with whatever vendor an arm
// needs. No editor binary, no GPU, no build of the C++ side — which is the
// point, because the real machine can only ever produce one vendor.
//
// Exit 0 = every arm held; 1 = a failure; 2 = the harness itself could not run.
//
//   node Tests/Mcp/rgp-capture-check.mjs

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const geEntry = path.join(repoRoot, 'mcp', 'ge.mjs');
const exeSuffix = process.platform === 'win32' ? '.exe' : '';

// Where the tool logs every CLI it spawns. Its mere existence is the evidence
// that something was spawned, which is what the refusal arms assert against.
const rgpLogDir = path.join(repoRoot, 'build', 'mcp-rgp-logs');

// A capture actually being attempted looks like this in the tool's output. The
// (\.exe)? is load-bearing: the resolver produces RadeonDeveloperPanelCLI.exe
// on Windows, so a pattern demanding a space right after the bare name matches
// nothing there and the assertion silently passes on a real spawn.
const kCaptureAttempted = /RadeonDeveloperPanelCLI(\.exe)?["']? .*--trigger-capture/;

const failures = [];
const tempDirs = [];

function check(label, condition, detail) {
  if (condition) return;
  failures.push(`${label}\n    ${detail}`);
}

function makeTempDir(prefix) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), prefix));
  tempDirs.push(dir);
  return dir;
}

/** A directory holding both CLI names, so resolution succeeds. */
function makeResolvableSuite() {
  const dir = makeTempDir('rgp-suite-');
  fs.writeFileSync(path.join(dir, `RadeonDeveloperServiceCLI${exeSuffix}`), '');
  fs.writeFileSync(path.join(dir, `RadeonDeveloperPanelCLI${exeSuffix}`), '');
  return dir;
}

/**
 * A TCP server answering get_gpu_tooling with `reply`, the response fields the
 * editor sends: { ok: true, result } for an answer, { ok: false, error } for a
 * refusal.
 */
function startFakeEditor(reply) {
  return new Promise((resolve, reject) => {
    const server = net.createServer((socket) => {
      let buffer = '';
      socket.on('data', (chunk) => {
        buffer += chunk.toString();
        const lines = buffer.split('\n');
        buffer = lines.pop() ?? '';
        for (const line of lines) {
          if (!line.trim()) continue;
          let id = '0';
          try { id = JSON.parse(line).id ?? '0'; } catch { /* answer anyway */ }
          socket.write(JSON.stringify({ id, ...reply }) + '\n');
        }
      });
      socket.on('error', () => { /* client hangs up between arms */ });
    });
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => resolve({ server, port: server.address().port }));
  });
}

// One budget for the whole run. The first spawn may install and compile mcp/;
// the rest are fast. Kept below the ctest TIMEOUT so this file is what speaks
// first when something hangs.
const kTotalBudgetMs = 540_000;
const deadline = Date.now() + kTotalBudgetMs;

/** Spawns the CLI and returns stdout/stderr; rgp_capture is expected to fail. */
function spawnCli(args, extraEnv = {}) {
  return new Promise((resolve, reject) => {
    const remaining = deadline - Date.now();
    if (remaining <= 0) {
      reject(new Error(`ran out of the ${kTotalBudgetMs}ms budget before: ge ${args.join(' ')}`));
      return;
    }
    const child = spawn(process.execPath, [geEntry, ...args], {
      cwd: repoRoot,
      env: { ...process.env, ...extraEnv },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => { stdout += d.toString(); });
    child.stderr.on('data', (d) => { stderr += d.toString(); });
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`timed out (${remaining}ms of budget left): ge ${args.join(' ')}`));
    }, remaining);
    child.on('error', (e) => { clearTimeout(timer); reject(e); });
    child.on('close', (code) => {
      clearTimeout(timer);
      resolve({ stdout, stderr, code, combined: `${stdout}\n${stderr}` });
    });
  });
}

/**
 * An empty directory for RDP_PATH: it makes the suite lookup miss
 * deterministically instead of depending on what is installed on this machine.
 */
const emptySuiteDir = makeTempDir('rgp-no-suite-');

const fakeEditors = [];
async function fakeEditor(reply) {
  const editor = await startFakeEditor(reply);
  fakeEditors.push(editor);
  return editor;
}

try {
  const nvidia = await fakeEditor({ ok: true, result: {
    vendor: 'NVIDIA', vendorId: 4318, hardware: 'NVIDIA GeForce RTX 4090',
    debugUtilsEnabled: true, validationLayerEnabled: false,
    toolingInfoAvailable: true, attachedTools: [],
  } });
  const intel = await fakeEditor({ ok: true, result: {
    vendor: 'Intel', vendorId: 32902, hardware: 'Intel Arc A770',
    debugUtilsEnabled: true, validationLayerEnabled: false,
    toolingInfoAvailable: true, attachedTools: [],
  } });
  const amd = await fakeEditor({ ok: true, result: {
    vendor: 'AMD', vendorId: 4098, hardware: 'AMD Radeon RX 7900 XTX',
    debugUtilsEnabled: true, validationLayerEnabled: false,
    toolingInfoAvailable: true, attachedTools: [],
  } });
  const noDevice = await fakeEditor({ ok: false, error: 'No rendering device' });

  // Arm 1 — NVIDIA: refuse, and name the hardware actually detected. An error
  // that only said "needs AMD" would leave the reader guessing what they have.
  const a = await spawnCli(
    ['rgp_capture', '--port', String(nvidia.port)], { RDP_PATH: emptySuiteDir });
  check('arm 1: refused', a.code !== 0, `exit ${a.code}\n${a.combined}`);
  check('arm 1: names the detected vendor', /NVIDIA/.test(a.combined), a.combined);
  check('arm 1: names the actual hardware',
    a.combined.includes('GeForce RTX 4090'), a.combined);
  check('arm 1: states the AMD requirement', /AMD Radeon/.test(a.combined), a.combined);
  check('arm 1: carries the UNVERIFIED-ON-AMD marker',
    a.combined.includes('UNVERIFIED-ON-AMD'), a.combined);

  // Arm 2 — Intel: the gate is "not AMD", not "is NVIDIA".
  const b = await spawnCli(
    ['rgp_capture', '--port', String(intel.port)], { RDP_PATH: emptySuiteDir });
  check('arm 2: refused on Intel', b.code !== 0, `exit ${b.code}\n${b.combined}`);
  check('arm 2: names Intel', /Intel/.test(b.combined), b.combined);
  check('arm 2: names the actual hardware', b.combined.includes('Arc A770'), b.combined);

  // Arm 3 — no rendering device: report that, rather than claiming a vendor.
  const c = await spawnCli(
    ['rgp_capture', '--port', String(noDevice.port)], { RDP_PATH: emptySuiteDir });
  check('arm 3: refused', c.code !== 0, `exit ${c.code}\n${c.combined}`);
  check('arm 3: reports the device problem',
    c.combined.includes('No rendering device'), c.combined);

  // Arm 4 — AMD: the vendor gate must PASS, so the run reaches CLI resolution.
  // This is the only way the resolver's failure path is reachable on non-AMD
  // hardware, and it also proves the gate runs before anything is spawned.
  const d = await spawnCli(
    ['rgp_capture', '--port', String(amd.port)], { RDP_PATH: emptySuiteDir });
  check('arm 4: still failed (no suite installed)', d.code !== 0, `exit ${d.code}\n${d.combined}`);
  check('arm 4: passed the vendor gate',
    !/needs an AMD Radeon GPU/.test(d.combined),
    `vendor gate fired on an AMD device:\n${d.combined}`);
  check('arm 4: failed at CLI resolution',
    d.combined.includes('Radeon Developer Tool Suite not found'), d.combined);
  check('arm 4: names the download page',
    d.combined.includes('https://gpuopen.com/rdp/'), d.combined);
  check('arm 4: names both CLIs',
    d.combined.includes('RadeonDeveloperServiceCLI')
    && d.combined.includes('RadeonDeveloperPanelCLI'), d.combined);

  // Arm 5 — the gate alone must stop a capture, with nothing else helping.
  //
  // Arms 1-3 point RDP_PATH at an EMPTY directory, so resolution would fail on
  // its own even if the gate were gone: they cannot tell a working gate from a
  // missing one. This arm hands the tool a RESOLVABLE suite and a non-AMD
  // editor, so the gate is the only thing standing between it and spawning
  // vendor CLIs. The evidence is physical rather than textual: the tool creates
  // build/mcp-rgp-logs/ the moment it spawns anything, so that directory
  // staying absent is proof nothing ran.
  const resolvableSuite = makeResolvableSuite();
  fs.rmSync(rgpLogDir, { recursive: true, force: true });
  const e = await spawnCli(
    ['rgp_capture', '--port', String(nvidia.port)], { RDP_PATH: resolvableSuite });
  check('arm 5: refused even with a resolvable suite', e.code !== 0, `exit ${e.code}\n${e.combined}`);
  check('arm 5: refused at the vendor gate, not at resolution',
    /needs an AMD Radeon GPU/.test(e.combined), e.combined);
  check('arm 5: no CLI was spawned',
    !fs.existsSync(rgpLogDir),
    `${rgpLogDir} exists, so something was spawned before the gate refused`);
  check('arm 5: no capture was attempted', !kCaptureAttempted.test(e.combined), e.combined);

  // Arm 6 — the output scan. A stale .rgp left in the output directory must not
  // be reported as this run's capture: that is the exact false pass an
  // unverified flow must never produce. Identity-based, so it does not depend
  // on filesystem timestamp granularity agreeing with the wall clock.
  // Imported from mcp/dist, which the spawns above have built by now.
  const rgp = await import(
    pathToFileURL(path.join(repoRoot, 'mcp', 'dist', 'profiling', 'rgp.js')).href);

  const outDir = makeTempDir('rgp-out-');
  const stalePath = path.join(outDir, 'stale.rgp');
  fs.writeFileSync(stalePath, 'old');

  // The snapshot stands in for "taken just before the capture is triggered".
  const baseline = rgp.snapshotRgpFiles(outDir);
  check('arm 6: a pre-existing .rgp is not reported as this run',
    rgp.newestRgpSince(outDir, baseline) === null,
    `picked up ${JSON.stringify(rgp.newestRgpSince(outDir, baseline))}`);

  const freshPath = path.join(outDir, 'fresh.rgp');
  fs.writeFileSync(freshPath, 'capture-bytes');
  const found = rgp.newestRgpSince(outDir, baseline);
  check('arm 6: a newly written .rgp is found', found?.filePath === freshPath, JSON.stringify(found));
  check('arm 6: its size is reported', found?.sizeBytes === 'capture-bytes'.length,
    String(found?.sizeBytes));

  fs.writeFileSync(path.join(outDir, 'notes.txt'), 'not a capture');
  check('arm 6: non-.rgp files are ignored',
    rgp.newestRgpSince(outDir, baseline)?.filePath === freshPath,
    JSON.stringify(rgp.newestRgpSince(outDir, baseline)));
  check('arm 6: a missing output directory yields null',
    rgp.newestRgpSince(path.join(outDir, 'absent'), new Map()) === null);

  // A capture that overwrites a name already present still counts as this run's.
  const advancedSeconds = (Date.now() + 5_000) / 1000;
  fs.utimesSync(stalePath, advancedSeconds, advancedSeconds);
  check('arm 6: an overwritten pre-existing .rgp counts as fresh',
    rgp.newestRgpSince(outDir, baseline)?.filePath === stalePath,
    JSON.stringify(rgp.newestRgpSince(outDir, baseline)));

  // Arm 7 — RDP_PATH accepts either the directory or one of the executables.
  const fakeSuite = makeResolvableSuite();
  const savedRdpPath = process.env.RDP_PATH;
  try {
    process.env.RDP_PATH = fakeSuite;
    const viaDir = rgp.resolveRgpClis();
    check('arm 7: RDP_PATH as a directory resolves both CLIs',
      viaDir.serviceCli === path.join(fakeSuite, `RadeonDeveloperServiceCLI${exeSuffix}`)
      && viaDir.panelCli === path.join(fakeSuite, `RadeonDeveloperPanelCLI${exeSuffix}`),
      JSON.stringify(viaDir));

    process.env.RDP_PATH = path.join(fakeSuite, `RadeonDeveloperPanelCLI${exeSuffix}`);
    const viaExe = rgp.resolveRgpClis();
    check('arm 7: RDP_PATH as an executable resolves to its directory',
      viaExe.suiteDir === fakeSuite, JSON.stringify(viaExe));
  } finally {
    if (savedRdpPath === undefined) delete process.env.RDP_PATH;
    else process.env.RDP_PATH = savedRdpPath;
  }
} catch (err) {
  console.error(`rgp-capture-check could not run: ${err.message}`);
  process.exitCode = 2;
} finally {
  for (const editor of fakeEditors) editor.server.close();
  for (const dir of tempDirs) fs.rmSync(dir, { recursive: true, force: true });
}

if (process.exitCode === 2) process.exit(2);

if (failures.length > 0) {
  console.error(`rgp_capture check FAILED (${failures.length}):`);
  for (const f of failures) console.error(`  ${f}`);
  process.exit(1);
}

console.log('rgp_capture check OK: vendor gate refuses non-AMD naming the detected hardware '
  + 'and spawns nothing even with a resolvable suite, passes on AMD through to CLI resolution, '
  + 'and the .rgp scan reports only new or advanced captures.');
