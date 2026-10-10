// Zero-pixel-diff sweep for the shadow P2 caster-reduction flip
// (GE_SHADOW_CASTER_REDUCTION). Proves the flip is pixel-neutral in the lit
// frame: culled casters only ever wrote shadow into regions the camera cannot
// see, so the composited scene must be identical with the flag ON vs OFF.
//
// Methodology (mirrors the #352 ship-time discipline):
//   * Same deterministic bench scene (@110k --lods) and camera for every lane.
//   * EDIT mode (no play) freezes every mover, and this render path has no TAA
//     jitter, so a warmed steady-state frame is GPU-deterministic run-to-run.
//   * Three separate editor processes (the flag is read once at process start,
//     never live-toggled): OFF-a, OFF-b, ON.
//   * SELF-DIFF calibration = OFF-a vs OFF-b bounds any residual cross-process /
//     temporal noise. TEST = OFF vs ON. Verdict "identical within noise" iff the
//     test's max channel delta <= the calibration bound.
//
// The lit frame (viewport composite) is the correctness target, NOT the shadow
// map: the shadow atlas legitimately differs where off-screen casters were
// dropped — that difference is the optimization working. capture_resource of
// the shadow layers is grabbed only as a diagnostic to show that contrast.
//
// Usage (from the repo/worktree that owns the DebugFast editor build):
//   node Tools/Benchmarks/Rendering/perf-bench/shadow-p2-diff-sweep.mjs
//   node Tools/Benchmarks/Rendering/perf-bench/shadow-p2-diff-sweep.mjs --port 9998 --warmup-frames 150
//
// NEVER point this at port 9999 (the developer's own editor).

import { makeIpc, sleep } from './ipc.mjs';
import { spawn, execSync } from 'node:child_process';
import zlib from 'node:zlib';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, '..', '..', '..', '..');

function parseArgs(argv) {
    const a = {
        port: Number(process.env.GE_EDITOR_DEBUG_PORT || 9998),
        preset: 'vs2026-x64-local', config: 'DebugFast',
        projectDir: 'bench-project',
        static: 100000, dynamic: 10000, lights: 256, skinned: 50, materials: 32,
        lods: true, camera: '0,70,-130@0,-24',
        warmupFrames: 150,
        outDir: path.join(scriptDir, 'results', 'p2-diff-sweep'),
    };
    a.lane = null;       // capture a single lane ('off-a'|'off-b'|'on') and exit
    a.diffOnly = false;  // skip capture; diff the three PNGs already on disk
    for (let i = 2; i < argv.length; ++i) {
        const k = argv[i]; const next = () => argv[++i];
        if (k === '--port') a.port = Number(next());
        else if (k === '--warmup-frames') a.warmupFrames = Number(next());
        else if (k === '--camera') a.camera = next();
        else if (k === '--out') a.outDir = next();
        else if (k === '--lane') a.lane = next();
        else if (k === '--diff-only') a.diffOnly = true;
        else if (k === '--static') a.static = Number(next());
        else if (k === '--dynamic') a.dynamic = Number(next());
        else if (k === '--skinned') a.skinned = Number(next());
        else { console.error(`Unknown arg: ${k}`); process.exit(2); }
    }
    if (a.lane && !['off-a', 'off-b', 'on'].includes(a.lane)) { console.error(`--lane must be off-a|off-b|on`); process.exit(2); }
    if (a.port === 9999) { console.error('Refusing port 9999 (developer editor).'); process.exit(2); }
    const m = /^(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)@(-?[\d.]+),(-?[\d.]+)$/.exec(a.camera.trim());
    if (!m) { console.error(`Invalid --camera "${a.camera}"`); process.exit(2); }
    a.cameraPos = [Number(m[1]), Number(m[2]), Number(m[3])];
    a.cameraYaw = Number(m[4]); a.cameraPitch = Number(m[5]);
    return a;
}

function editorExe(a) {
    return path.join(repoRoot, 'build', a.preset, 'bin', a.config, 'Apps', 'Editor', 'Editor.exe');
}

function ensureBenchProject(name) {
    const projDir = path.join(scriptDir, name);
    const foxDst = path.join(projDir, 'Assets', 'Models', 'Fox');
    if (!fs.existsSync(path.join(foxDst, 'Fox.glb'))) {
        const foxSrc = path.join(repoRoot, 'Tests', 'EditorHarness', 'rg2-gate1-content', 'Assets', 'Models', 'Fox');
        fs.mkdirSync(foxDst, { recursive: true });
        fs.cpSync(foxSrc, foxDst, { recursive: true });
    }
    return projDir;
}

async function launchEditor(a, flagVal, label) {
    const exe = editorExe(a);
    if (!fs.existsSync(exe)) throw new Error(`Editor not built: ${exe}`);
    const projDir = ensureBenchProject(a.projectDir);
    fs.mkdirSync(a.outDir, { recursive: true });
    const logPath = path.join(a.outDir, `editor-${label}.log`);
    const logFd = fs.openSync(logPath, 'w');
    // The flag is read once at process init, so each lane is a distinct editor.
    // Force-set the value (0 or 1) so no stray parent-shell value leaks a lane.
    const env = { ...process.env, GE_EDITOR_DEBUG_PORT: String(a.port), GE_SHADOW_CASTER_REDUCTION: String(flagVal) };
    const child = spawn(exe, ['--project', projDir], {
        cwd: path.dirname(exe), env, detached: true, stdio: ['ignore', logFd, logFd],
    });
    child.unref();
    fs.closeSync(logFd);
    console.log(`  [${label}] launched pid ${child.pid}  GE_SHADOW_CASTER_REDUCTION=${flagVal}  log ${logPath}`);
    return child.pid;
}

async function waitForEditor(ipc, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
        try { const st = await ipc('get_editor_state', {}, { timeoutMs: 4000 }); if (st && !st.error) return st; }
        catch { /* not up yet */ }
        if (Date.now() > deadline) throw new Error('Editor did not come up in time');
        await sleep(2000);
    }
}

async function framePumpWarm(ipc) {
    const deadline = Date.now() + 180000;
    let prev = -1;
    for (;;) {
        const rs = await ipc('get_render_stats', {}, { timeoutMs: 8000 }).catch(() => null);
        if (rs && !rs.error) { const fc = rs.frameCount ?? 0; if (fc >= 60 && prev >= 0 && fc > prev) return; prev = fc; }
        if (Date.now() > deadline) throw new Error(`Frame pump not advancing (frameCount=${prev})`);
        await sleep(1000);
    }
}

async function frameCountedWarmup(ipc, warmupFrames) {
    const rs0 = await ipc('get_render_stats', {}, { timeoutMs: 8000 }).catch(() => null);
    const target = (rs0?.frameCount ?? 0) + warmupFrames;
    const deadline = Date.now() + 180000;
    for (;;) {
        const rs = await ipc('get_render_stats', {}, { timeoutMs: 8000 }).catch(() => null);
        if ((rs?.frameCount ?? 0) >= target) return rs.frameCount;
        if (Date.now() > deadline) throw new Error(`Warmup frame ${target} not reached`);
        await sleep(250);
    }
}

async function shutdownEditor(ipc, pid, label) {
    try { await ipc('shutdown', {}, { timeoutMs: 8000 }); } catch { /* may drop the socket as it exits */ }
    // Confirm the process we launched is gone; hard-kill it as a fallback.
    // process.kill(pid, 0) probes liveness without spawning an external command.
    const alive = () => { try { process.kill(pid, 0); return true; } catch { return false; } };
    for (let i = 0; i < 20; ++i) {
        if (!alive()) { console.log(`  [${label}] editor pid ${pid} exited`); return; }
        await sleep(500);
    }
    console.warn(`  [${label}] editor pid ${pid} still alive after shutdown; killing`);
    try { process.kill(pid); } catch (e) { console.warn(`  kill: ${e.message}`); }
}

// ---- minimal PNG decode (8-bit, non-interlaced; color types 2/6/0/4) ----
function decodePng(buf) {
    const sig = [137, 80, 78, 71, 13, 10, 26, 10];
    for (let i = 0; i < 8; ++i) if (buf[i] !== sig[i]) throw new Error('not a PNG');
    let off = 8, width = 0, height = 0, bitDepth = 0, colorType = 0, interlace = 0;
    const idat = [];
    while (off < buf.length) {
        const len = buf.readUInt32BE(off); const type = buf.toString('ascii', off + 4, off + 8);
        const data = buf.subarray(off + 8, off + 8 + len);
        if (type === 'IHDR') {
            width = data.readUInt32BE(0); height = data.readUInt32BE(4);
            bitDepth = data[8]; colorType = data[9]; interlace = data[12];
        } else if (type === 'IDAT') idat.push(data);
        else if (type === 'IEND') break;
        off += 12 + len;
    }
    if (bitDepth !== 8) throw new Error(`unsupported bitDepth ${bitDepth}`);
    if (interlace !== 0) throw new Error('interlaced PNG unsupported');
    const channels = { 0: 1, 2: 3, 4: 2, 6: 4 }[colorType];
    if (!channels) throw new Error(`unsupported colorType ${colorType}`);
    const raw = zlib.inflateSync(Buffer.concat(idat));
    const stride = width * channels;
    const out = new Uint8Array(height * stride);
    const paeth = (a, b, c) => { const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c); return pa <= pb && pa <= pc ? a : pb <= pc ? b : c; };
    let rp = 0;
    for (let y = 0; y < height; ++y) {
        const filter = raw[rp++];
        for (let x = 0; x < stride; ++x) {
            const cur = raw[rp++];
            const a = x >= channels ? out[y * stride + x - channels] : 0;
            const b = y > 0 ? out[(y - 1) * stride + x] : 0;
            const c = (x >= channels && y > 0) ? out[(y - 1) * stride + x - channels] : 0;
            let v;
            switch (filter) {
                case 0: v = cur; break;
                case 1: v = cur + a; break;
                case 2: v = cur + b; break;
                case 3: v = cur + ((a + b) >> 1); break;
                case 4: v = cur + paeth(a, b, c); break;
                default: throw new Error(`bad filter ${filter}`);
            }
            out[y * stride + x] = v & 0xff;
        }
    }
    return { width, height, channels, colorType, data: out };
}

// Per-texel RGB delta between two decoded PNGs of identical geometry.
function diffRgb(imgA, imgB) {
    if (imgA.width !== imgB.width || imgA.height !== imgB.height)
        throw new Error(`size mismatch ${imgA.width}x${imgA.height} vs ${imgB.width}x${imgB.height}`);
    const n = imgA.width * imgA.height;
    let maxDelta = 0, sum = 0, changed = 0;
    for (let i = 0; i < n; ++i) {
        let px = 0;
        for (let ch = 0; ch < 3; ++ch) {
            const va = imgA.data[i * imgA.channels + ch];
            const vb = imgB.data[i * imgB.channels + ch];
            const d = Math.abs(va - vb);
            if (d > px) px = d;
            sum += d;
        }
        if (px > 0) changed++;
        if (px > maxDelta) maxDelta = px;
    }
    return { width: imgA.width, height: imgA.height, maxDelta, meanDelta: sum / (n * 3), changedTexels: changed, totalTexels: n };
}

async function captureLane(a, flagVal, label) {
    const ipc = makeIpc(a.port);
    const pid = await launchEditor(a, flagVal, label);
    try {
        const st = await waitForEditor(ipc, 300000);
        console.log(`  [${label}] editor up. project ${st.projectRoot ?? st.projectPath ?? 'n/a'}`);
        await framePumpWarm(ipc);
        const spawnRes = await ipc('spawn_bench_scene', {
            staticCount: a.static, dynamicCount: a.dynamic, lightCount: a.lights,
            materialCount: a.materials, occluderWalls: 0, transmissiveCount: 0, lods: a.lods,
        }, { timeoutMs: 120000 });
        if (spawnRes.error) throw new Error(`spawn_bench_scene: ${spawnRes.error}`);
        for (let i = 0; i < a.skinned; ++i) {
            const ang = (i / Math.max(a.skinned, 1)) * Math.PI * 2.0; const r = 18 + (i % 5) * 4;
            const pos = { x: Math.cos(ang) * r, y: 0, z: Math.sin(ang) * r };
            let res = await ipc('spawn_model', { path: 'Models/Fox/Fox.glb', name: `BenchFox${i}`, position: pos }, { timeoutMs: 60000 });
            if (res?.error) { await sleep(1500); await ipc('spawn_model', { path: 'Models/Fox/Fox.glb', name: `BenchFox${i}`, position: pos }, { timeoutMs: 60000 }); }
        }
        const cam = await ipc('set_camera', { position: a.cameraPos, yawDeg: a.cameraYaw, pitchDeg: a.cameraPitch });
        if (cam?.error) throw new Error(`set_camera: ${cam.error}`);
        // EDIT mode: no play-mode click. Movers stay frozen at spawn -> deterministic frame.
        const warmFrame = await frameCountedWarmup(ipc, a.warmupFrames);
        // Invariants echoed so the sweep pins the same scene the bench lane measured.
        const rs = await ipc('get_render_stats', {}, { timeoutMs: 8000 });
        const inv = {
            entityCount: rs?.entityCount,
            gpuSceneInstanceCount: rs?.renderServices?.gpuSceneInstanceCount,
            drawStreamSlots: rs?.renderServices?.drawStreamSlots,
            warmFrame,
        };
        console.log(`  [${label}] warm frame ${warmFrame}  entities ${inv.entityCount}  instances ${inv.gpuSceneInstanceCount}  slots ${inv.drawStreamSlots}`);
        const shot = await ipc('take_screenshot', { target: 'viewport' }, { timeoutMs: 30000 });
        if (shot?.error || !shot?.pngBase64) throw new Error(`take_screenshot: ${shot?.error ?? 'no pngBase64'}`);
        const litPath = path.join(a.outDir, `lit-${label}.png`);
        fs.writeFileSync(litPath, Buffer.from(shot.pngBase64, 'base64'));
        const meta = { label, flagVal, litPath, width: shot.width, height: shot.height, inv };
        fs.writeFileSync(path.join(a.outDir, `meta-${label}.json`), JSON.stringify(meta, null, 2));
        console.log(`  [${label}] lit frame ${shot.width}x${shot.height} -> ${litPath}`);
        return meta;
    } finally {
        await shutdownEditor(ipc, pid, label);
        await sleep(1500); // let the port free before the next lane binds it
    }
}

function computeAndReport(a) {
    const load = (label) => {
        const metaPath = path.join(a.outDir, `meta-${label}.json`);
        if (!fs.existsSync(metaPath)) throw new Error(`missing ${metaPath} (capture lane ${label} first)`);
        return JSON.parse(fs.readFileSync(metaPath, 'utf8'));
    };
    const metas = { 'off-a': load('off-a'), 'off-b': load('off-b'), 'on': load('on') };
    const imgs = Object.fromEntries(Object.entries(metas).map(([label, m]) => [label, decodePng(fs.readFileSync(m.litPath))]));
    const calib = diffRgb(imgs['off-a'], imgs['off-b']);          // noise floor
    const testA = diffRgb(imgs['off-a'], imgs['on']);             // flip vs baseline
    const testB = diffRgb(imgs['off-b'], imgs['on']);             // flip vs baseline (2nd ref)
    const testMax = Math.max(testA.maxDelta, testB.maxDelta);
    const verdict = testMax <= calib.maxDelta ? 'IDENTICAL WITHIN NOISE' : 'DIFFERENCE EXCEEDS CALIBRATION';

    const report = {
        capturedAt: new Date().toISOString(),
        gitSha: (() => { try { return execSync('git rev-parse HEAD', { cwd: repoRoot, encoding: 'utf8' }).trim(); } catch { return null; } })(),
        scene: { static: a.static, dynamic: a.dynamic, lights: a.lights, skinned: a.skinned, materials: a.materials, lods: a.lods, camera: a.camera, mode: 'edit', warmupFrames: a.warmupFrames },
        invariants: Object.values(metas).map(m => ({ label: m.label, ...m.inv })),
        calibration_off_vs_off: calib,
        test_offA_vs_on: testA,
        test_offB_vs_on: testB,
        verdict,
    };
    fs.writeFileSync(path.join(a.outDir, 'sweep-report.json'), JSON.stringify(report, null, 2));

    console.log('\n===== SHADOW P2 ZERO-PIXEL DIFF SWEEP =====');
    console.log(`resolution        ${calib.width}x${calib.height}  (RGB channels compared)`);
    console.log(`calibration OFF/OFF   maxDelta ${calib.maxDelta}  meanDelta ${calib.meanDelta.toFixed(6)}  changedTexels ${calib.changedTexels}/${calib.totalTexels}`);
    console.log(`test OFF-a/ON         maxDelta ${testA.maxDelta}  meanDelta ${testA.meanDelta.toFixed(6)}  changedTexels ${testA.changedTexels}/${testA.totalTexels}`);
    console.log(`test OFF-b/ON         maxDelta ${testB.maxDelta}  meanDelta ${testB.meanDelta.toFixed(6)}  changedTexels ${testB.changedTexels}/${testB.totalTexels}`);
    console.log(`VERDICT: ${verdict}  (test maxDelta ${testMax} vs calibration bound ${calib.maxDelta})`);
    console.log(`report: ${path.join(a.outDir, 'sweep-report.json')}`);
    if (verdict !== 'IDENTICAL WITHIN NOISE') process.exit(3);
}

async function main() {
    const a = parseArgs(process.argv);
    fs.mkdirSync(a.outDir, { recursive: true });
    if (a.diffOnly) { computeAndReport(a); return; }
    if (a.lane) {
        console.log(`Capturing single lane '${a.lane}' on port ${a.port} (edit mode, warmup ${a.warmupFrames}f).`);
        await captureLane(a, a.lane === 'on' ? 1 : 0, a.lane);
        return;
    }
    console.log(`Shadow P2 diff sweep on port ${a.port}. Scene: ${a.static} static / ${a.dynamic} dynamic / ${a.lights} lights / lods=${a.lods}. Camera ${a.camera}. Edit mode, warmup ${a.warmupFrames}f.`);
    await captureLane(a, 0, 'off-a');
    await captureLane(a, 0, 'off-b');
    await captureLane(a, 1, 'on');
    computeAndReport(a);
}

main().catch(e => { console.error(e); process.exit(1); });
