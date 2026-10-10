// Renderer scale-bench capture harness (roadmap R1.7).
//
// Spawns the deterministic bench scene (spawn_bench_scene IPC + N skinned
// foxes via spawn_model), positions the camera, optionally enters play mode,
// then samples renderer metrics for a fixed window and writes a JSON result
// with a printed summary. Diff results across branches to measure R1/R2 work.
//
// Usage (from the repo root that owns the editor build):
//   node Tools/Benchmarks/Rendering/perf-bench/bench.mjs --launch --label main-baseline
//   node Tools/Benchmarks/Rendering/perf-bench/bench.mjs --port 9998 --skip-spawn --label rerun
//
// Key flags:
//   --launch          launch a bench editor (DebugFast) on --port with a
//                     hermetic project under Tools/Benchmarks/Rendering/perf-bench/bench-project
//   --editor PATH     launch this Editor executable instead of the preset's
//                     default path (useful for local before/after builds)
//   --port N          debug-server port (default GE_EDITOR_DEBUG_PORT or 9998)
//   --label NAME      result file name under Tools/Benchmarks/Rendering/perf-bench/results/
//   --static/--dynamic/--lights/--skinned N   scene composition
//   --walls N         occluder walls across the field (interiors/HZB lane)
//   --children N      a chain of N child meshes under every static and dynamic
//                     entity (transform-hierarchy arms; 0 = none)
//   --camera SPEC     vantage "x,y,z@yawDeg,pitchDeg" (default "0,70,-130@0,-24"
//                     sees over walls; the interiors lane wants a low camera
//                     behind a wall, e.g. "0,2.5,-390@0,-1")
//   --samples N --interval MS                  capture window
//   --no-play         stay in edit mode (skinned actors won't animate)
//   --skip-spawn      attach to an editor that already has the bench scene
//   --keep-editor     leave a --launch'd editor running after the run (default:
//                     kill the editor this run launched). No effect without --launch.
//
// NEVER point this at port 9999 — that is the developer's own editor.

import { makeIpc, sleep } from './ipc.mjs';
import { reducePassTimings, groupGpuMs } from './gpu-pass-totals.mjs';
import { spawn, execSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, '..', '..', '..', '..');

function parseArgs(argv) {
    const a = {
        port: Number(process.env.GE_EDITOR_DEBUG_PORT || 9998),
        label: `run-${new Date().toISOString().replace(/[:.]/g, '-')}`,
        static: 100000, dynamic: 10000, lights: 256, skinned: 50, materials: 32,
        walls: 0, transmissive: 0, children: 0, camera: '0,70,-130@0,-24',
        samples: 24, interval: 500, warmupFrames: 120,
        // lod-content lane (P0.2): generate LOD chains on the bench cube+sphere.
        lods: false,
        // Static-only far-cascade lane (P0.3): no movers → no scene-AABB churn →
        // no empty-cascade-skip bimodality. Forces dynamic + skinned to 0.
        staticWarmup: false,
        play: true, spawn: true, launch: false, keepEditor: false, merge: false, screenshot: null,
        config: 'DebugFast', preset: 'vs2026-x64-local',
        editor: null,
        projectDir: 'bench-project',
        // A/B env flags read once at editor init and passed through to the
        // launched editor process (never live-toggled). --merge is sugar for
        // GE_COLOR_CLASS_MERGE=1; --env NAME[=VALUE] adds any GE_* flag
        // (e.g. --env GE_EXTRACTION_FEED=0 to pin the full lane for A/B).
        envFlags: {},
    };
    for (let i = 2; i < argv.length; ++i) {
        const k = argv[i];
        const next = () => argv[++i];
        if (k === '--port') a.port = Number(next());
        else if (k === '--label') a.label = next();
        else if (k === '--static') a.static = Number(next());
        else if (k === '--dynamic') a.dynamic = Number(next());
        else if (k === '--lights') a.lights = Number(next());
        else if (k === '--skinned') a.skinned = Number(next());
        else if (k === '--materials') a.materials = Number(next());
        else if (k === '--walls') a.walls = Number(next());
        else if (k === '--transmissive') a.transmissive = Number(next());
        else if (k === '--children') a.children = Number(next());
        else if (k === '--merge') { a.merge = true; a.envFlags.GE_COLOR_CLASS_MERGE = '1'; }
        else if (k === '--scatter-stats') { a.envFlags.GE_SCATTER_STATS = '1'; }
        else if (k === '--env') {
            const kv = next();
            const eq = kv.indexOf('=');
            const name = eq < 0 ? kv : kv.slice(0, eq);
            a.envFlags[name] = eq < 0 ? '1' : kv.slice(eq + 1);
        }
        else if (k === '--screenshot') a.screenshot = next();
        else if (k === '--camera') a.camera = next();
        else if (k === '--samples') a.samples = Number(next());
        else if (k === '--interval') a.interval = Number(next());
        else if (k === '--warmup-frames') a.warmupFrames = Number(next());
        else if (k === '--lods') a.lods = true;
        else if (k === '--static-warmup') a.staticWarmup = true;
        else if (k === '--config') a.config = next();
        else if (k === '--preset') a.preset = next();
        else if (k === '--editor') a.editor = path.resolve(next());
        else if (k === '--no-play') a.play = false;
        else if (k === '--skip-spawn') a.spawn = false;
        else if (k === '--launch') a.launch = true;
        else if (k === '--keep-editor') a.keepEditor = true;
        else if (k === '--project-dir') a.projectDir = next();
        else { console.error(`Unknown arg: ${k}`); process.exit(2); }
    }
    if (a.port === 9999) {
        console.error('Refusing port 9999 (developer editor). Use GE_EDITOR_DEBUG_PORT.');
        process.exit(2);
    }
    // The static-only far-cascade lane removes every mover: dynamic orbiters AND
    // animating skinned actors both perturb the scene AABB that drives the
    // empty-cascade-skip flip (methodology F7). Force both to 0.
    if (a.staticWarmup) { a.dynamic = 0; a.skinned = 0; }
    for (const k of ['port', 'static', 'dynamic', 'lights', 'skinned', 'materials', 'walls', 'transmissive', 'children', 'samples', 'interval', 'warmupFrames']) {
        if (!Number.isFinite(a[k]) || a[k] < 0) { console.error(`Invalid --${k}: ${a[k]}`); process.exit(2); }
    }
    if (a.warmupFrames < 1) { console.error('--warmup-frames must be >= 1'); process.exit(2); }
    if (a.samples < 2) { console.error('--samples must be >= 2 (the reduce diffs first vs last)'); process.exit(2); }
    // "x,y,z@yawDeg,pitchDeg" — a malformed vantage makes the capture
    // incomparable, so it is fatal at parse time, not at set_camera time.
    {
        const m = /^(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)@(-?[\d.]+),(-?[\d.]+)$/.exec(a.camera.trim());
        if (!m) { console.error(`Invalid --camera "${a.camera}" (want "x,y,z@yawDeg,pitchDeg")`); process.exit(2); }
        a.cameraPos = [Number(m[1]), Number(m[2]), Number(m[3])];
        a.cameraYaw = Number(m[4]);
        a.cameraPitch = Number(m[5]);
    }
    return a;
}

function percentile(sorted, p) {
    if (!sorted.length) return 0;
    const idx = Math.min(sorted.length - 1, Math.floor(p * (sorted.length - 1)));
    return sorted[idx];
}

function stats(values) {
    if (!values.length) return { mean: 0, min: 0, max: 0, p50: 0, p95: 0 };
    const s = [...values].sort((x, y) => x - y);
    return {
        mean: values.reduce((x, y) => x + y, 0) / values.length,
        min: s[0], max: s[s.length - 1],
        p50: percentile(s, 0.5), p95: percentile(s, 0.95),
    };
}

function gitSha(cwd) {
    // Pin every baseline to the exact worktree commit it was captured on: a
    // before/after pair is only comparable when both share one SHA (P0.3 / F8).
    try {
        return execSync('git rev-parse HEAD', { cwd, encoding: 'utf8' }).trim();
    } catch (e) {
        console.warn(`  git rev-parse failed (${e.message}); gitSha=null`);
        return null;
    }
}

function ensureBenchProject(name) {
    const projDir = path.join(scriptDir, name);
    const foxDst = path.join(projDir, 'Assets', 'Models', 'Fox');
    if (!fs.existsSync(path.join(foxDst, 'Fox.glb'))) {
        const foxSrc = path.join(repoRoot, 'Tests', 'EditorHarness', 'rg2-gate1-content', 'Assets', 'Models', 'Fox');
        fs.mkdirSync(foxDst, { recursive: true });
        fs.cpSync(foxSrc, foxDst, { recursive: true });
        console.log(`bench-project: copied Fox model from ${foxSrc}`);
    }
    return projDir;
}

async function launchEditor(a, projDir) {
    const defaultExe = process.platform === 'darwin'
        ? path.join(repoRoot, 'build', a.preset, 'bin', a.config, 'Apps', 'Editor',
                    'Editor.app', 'Contents', 'MacOS', 'Editor')
        : path.join(repoRoot, 'build', a.preset, 'bin', a.config, 'Apps', 'Editor', 'Editor.exe');
    const exe = a.editor ?? defaultExe;
    if (!fs.existsSync(exe)) throw new Error(`Editor not built: ${exe}`);
    console.log(`Launching ${exe} on port ${a.port} (project: ${projDir})`);
    const logPath = path.join(scriptDir, 'results', `editor-${a.label}.log`);
    fs.mkdirSync(path.dirname(logPath), { recursive: true });
    const logFd = fs.openSync(logPath, 'w');
    // The A/B env flags are read once at editor init, so each lane is a
    // distinct editor process (flag ON vs OFF), never a live toggle. The
    // managed set is force-cleared unless this launch set it, so a stray
    // parent-shell flag can never leak into a lane (byte-identical OFF); any
    // other --env entries pass through as-is. --merge is sugar for
    // GE_COLOR_CLASS_MERGE=1 and keeps working.
    const kManagedFlags = ['GE_COLOR_CLASS_MERGE', 'GE_PARALLEL_EXTRACTION', 'GE_PARALLEL_RECORD',
                           'GE_PARALLEL_HIERARCHY', 'GE_TLAS_FEED', 'GE_HIER_PROFILE',
                           'GE_EXTRACTION_FEED',
                           // Present pacing must never contaminate CPU/GPU frame-time A/B runs.
                           // Opt in explicitly with --env GE_VSYNC=0 (or =1).
                           'GE_VSYNC',
                           // Default-off scatter instrumentation (drawnTriangles atomic).
                           // Managed so a stray shell value can't contaminate the stats-off
                           // baseline lane; --scatter-stats (or --env GE_SCATTER_STATS=1)
                           // opts the shadow-triangle lanes in.
                           'GE_SCATTER_STATS'];
    const env = { ...process.env, GE_EDITOR_DEBUG_PORT: String(a.port) };
    for (const f of kManagedFlags) delete env[f];
    for (const [name, val] of Object.entries(a.envFlags)) env[name] = val;
    const shownNames = [...new Set([...kManagedFlags, ...Object.keys(a.envFlags)])];
    console.log(`  env flags: ${shownNames.map(f => `${f}=${env[f] ?? '(unset)'}`).join('  ')}`);
    const child = spawn(exe, ['--project', projDir], {
        cwd: path.dirname(exe),
        env,
        detached: true, stdio: ['ignore', logFd, logFd],
    });
    child.unref();
    fs.closeSync(logFd);
    console.log(`  editor log: ${logPath}`);
    return child;
}

// Terminate an editor this run launched. The child was spawned detached (its own
// process group), so a Windows tree-kill by PID reaps any helper processes it spawned
// too. taskkill is invoked by absolute path — it is not always on PATH — with a direct
// process.kill fallback (which still frees the debug-server port even if the tree-kill is
// unavailable). Only ever called on the child from launchEditor — never on an editor this
// run merely attached to (--skip-spawn / no --launch).
function killLaunchedEditor(child) {
    if (!child || child.pid == null) return;
    const pid = child.pid;
    if (process.platform === 'win32') {
        const taskkill = process.env.SystemRoot
            ? path.join(process.env.SystemRoot, 'System32', 'taskkill.exe')
            : 'taskkill';
        try {
            execSync(`"${taskkill}" /PID ${pid} /T /F`, { stdio: 'ignore' });
            console.log(`  killed launched editor (pid ${pid})`);
            return;
        } catch { /* taskkill missing or failed — fall back to a direct kill below */ }
    }
    try {
        process.kill(pid);
        console.log(`  killed launched editor (pid ${pid})`);
    } catch (e) {
        console.warn(`  could not kill launched editor pid ${pid}: ${e.message}`);
    }
}

async function waitForEditor(ipc, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
        try {
            const st = await ipc('get_editor_state', {}, { timeoutMs: 4000 });
            if (st && !st.error) return st;
        } catch { /* not up yet */ }
        if (Date.now() > deadline) throw new Error('Editor did not come up in time');
        await sleep(2000);
    }
}

async function main() {
    const a = parseArgs(process.argv);
    const ipc = makeIpc(a.port);

    let editorPid = null;
    let editorChild = null;
    if (a.launch) {
        editorChild = await launchEditor(a, ensureBenchProject(a.projectDir));
        editorPid = editorChild.pid;
    }
    try {
    const editorState = await waitForEditor(ipc, a.launch ? 300000 : 10000);
    console.log(`Editor up on ${a.port}. Project: ${editorState.projectRoot ?? editorState.projectPath ?? 'n/a'}`);

    // The debug server answers before the editor finishes its (very long)
    // first frames — spawning then made frame 1 take minutes. Wait until the
    // frame counter is past startup AND still advancing.
    {
        const deadline = Date.now() + 180000;
        let prev = -1;
        for (;;) {
            const rs = await ipc('get_render_stats', {}, { timeoutMs: 8000 }).catch(() => null);
            if (rs && !rs.error) {
                const fc = rs.frameCount ?? 0;
                if (fc >= 60 && prev >= 0 && fc > prev) break;
                prev = fc; // only trust successful polls: two ADVANCING readings required
            }
            if (Date.now() > deadline) throw new Error(`Editor frame pump not advancing (frameCount=${prev})`);
            await sleep(1000);
        }
        console.log('Editor frame pump warm.');
    }

    let spawnResult = null;
    const skinnedResults = [];
    if (a.spawn) {
        console.log(`Spawning bench scene: ${a.static} static / ${a.dynamic} dynamic / ${a.lights} lights / ${a.materials} materials ...`);
        spawnResult = await ipc('spawn_bench_scene', {
            staticCount: a.static, dynamicCount: a.dynamic,
            lightCount: a.lights, materialCount: a.materials,
            occluderWalls: a.walls, transmissiveCount: a.transmissive,
            childrenPerEntity: a.children, lods: a.lods,
        }, { timeoutMs: 120000 });
        if (spawnResult.error) throw new Error(`spawn_bench_scene: ${spawnResult.error}`);
        console.log(`  spawned in ${spawnResult.elapsedMs?.toFixed(0)} ms, entityCount=${spawnResult.entityCount}, mover=${spawnResult.moverRegistered}, transmissive=${spawnResult.transmissiveSpawned}`);
        if (spawnResult.lods?.requested)
            console.log(`  lods: available=${spawnResult.lods.generationAvailable} cube=${spawnResult.lods.cubeLodCount} sphere=${spawnResult.lods.sphereLodCount}`);

        // Skinned actors in a ring near the camera vantage; sequential to
        // keep the deferred loader happy. First spawn may take longest.
        for (let i = 0; i < a.skinned; ++i) {
            const ang = (i / Math.max(a.skinned, 1)) * Math.PI * 2.0;
            const r = 18 + (i % 5) * 4;
            const pos = { x: Math.cos(ang) * r, y: 0, z: Math.sin(ang) * r };
            let res = await ipc('spawn_model', { path: 'Models/Fox/Fox.glb', name: `BenchFox${i}`, position: pos }, { timeoutMs: 60000 });
            if (res?.error) {
                // First-load can exceed the deferred window; retry once.
                await sleep(1500);
                res = await ipc('spawn_model', { path: 'Models/Fox/Fox.glb', name: `BenchFox${i}`, position: pos }, { timeoutMs: 60000 });
            }
            skinnedResults.push(res?.error ? { i, error: res.error } : { i, ok: true });
        }
        const failed = skinnedResults.filter(r => r.error);
        console.log(`  skinned: ${a.skinned - failed.length}/${a.skinned} spawned${failed.length ? ` (${failed.length} FAILED)` : ''}`);
    }

    // Fixed vantage (default: high over the field so a large fraction of the
    // grid is in frustum; --camera overrides for the interiors/HZB lane).
    // Deterministic across runs — a failed set_camera would make the capture
    // incomparable, so it is fatal.
    const cam = await ipc('set_camera',
                          { position: a.cameraPos, yawDeg: a.cameraYaw, pitchDeg: a.cameraPitch });
    if (cam?.error) throw new Error(`set_camera failed: ${cam.error}`);

    if (a.play) {
        console.log('Entering play mode ...');
        const r = await ipc('set_play_mode', { action: 'enter' }, { timeoutMs: 120000 });
        if (r?.error || !['playing', 'entering_play'].includes(r?.playMode))
            throw new Error(`Play mode did not start: ${r?.error ?? r?.playMode ?? 'missing playMode'}`);
        // Large scenes can finish their pre-play snapshot asynchronously. Do
        // not capture an edit-mode or transitioning lane as an animation run.
        let state = r.playMode;
        const deadline = Date.now() + 180000;
        while (state !== 'playing') {
            if (Date.now() >= deadline) throw new Error(`Play mode timed out in ${state}`);
            await sleep(250);
            const current = await ipc('get_editor_state', {}, { timeoutMs: 8000 });
            state = current?.playMode;
            if (current?.error || !['playing', 'entering_play'].includes(state))
                throw new Error(`Play mode did not reach playing: ${current?.error ?? state ?? 'missing playMode'}`);
        }
    }

    const arm = await ipc('get_gpu_profiler', { enabled: true });
    if (arm?.error) throw new Error(`GPU profiler arm failed: ${arm.error}`);

    // Frame-counted warmup (P0.3 / methodology F7). A wall-clock sleep leaves a
    // frame-rate-dependent fraction of the SDSM split-EMA transient in the
    // capture (fast-expand/slow-contract, ~1 time constant per several frames).
    // Poll frameCount and wait +warmupFrames past the current (post-spawn,
    // post-play, post-arm) "ready" frame so every lane converges the same amount
    // regardless of fps.
    {
        const rs0 = await ipc('get_render_stats', {}, { timeoutMs: 8000 }).catch(() => null);
        const readyFrame = rs0?.frameCount ?? 0;
        const target = readyFrame + a.warmupFrames;
        console.log(`Frame-counted warmup: waiting for frame ${target} (ready ${readyFrame} + ${a.warmupFrames}), then sampling ${a.samples} x ${a.interval} ms ...`);
        const deadline = Date.now() + 180000;
        for (;;) {
            const rs = await ipc('get_render_stats', {}, { timeoutMs: 8000 }).catch(() => null);
            const fc = rs?.frameCount ?? 0;
            if (fc >= target) { console.log(`  warm at frame ${fc}.`); break; }
            if (Date.now() > deadline) throw new Error(`Warmup frame ${target} not reached (frameCount=${fc})`);
            await sleep(250);
        }
    }

    // Deterministic-vantage viewport capture for the A/B pixel-equivalence
    // check (scene only, no UI chrome). Written from the returned base64 so
    // OFF/ON runs land in distinct files.
    if (a.screenshot) {
        const shot = await ipc('take_screenshot', { target: 'viewport' }, { timeoutMs: 30000 });
        if (shot?.error || !shot?.pngBase64) {
            console.warn(`  screenshot failed: ${shot?.error ?? 'no pngBase64'}`);
        } else {
            fs.mkdirSync(path.dirname(a.screenshot), { recursive: true });
            fs.writeFileSync(a.screenshot, Buffer.from(shot.pngBase64, 'base64'));
            console.log(`  screenshot: ${a.screenshot} (${shot.width}x${shot.height})`);
        }
    }

    const samples = [];
    let sampleFailures = 0;
    const t0 = Date.now();
    for (let i = 0; i < a.samples; ++i) {
        try {
            const [rs, mon, gpu] = await Promise.all([
                ipc('get_render_stats'),
                ipc('get_monitors'),
                ipc('get_gpu_profiler'),
            ]);
            if (rs?.error || !rs?.renderServices) throw new Error(rs?.error ?? 'render stats missing');
            samples.push({ t: (Date.now() - t0) / 1000, rs, mon, gpu });
        } catch (e) {
            // One bad sample must not discard a multi-minute run; three in a
            // row means the editor is gone.
            console.warn(`  sample ${i} failed: ${e.message}`);
            if (++sampleFailures >= 3) throw new Error('3 consecutive sample failures — aborting capture');
            await sleep(a.interval);
            continue;
        }
        sampleFailures = 0;
        await sleep(a.interval);
    }
    if (samples.length < 2)
        throw new Error(`Only ${samples.length} valid samples — nothing to reduce`);

    // ---- reduce ----
    // App-loop timing comes from get_render_stats (Application::GetFps());
    // the DebugMetrics 'Time/*' monitors are unreliable in this context.
    const fpsVals = samples.map(s => s.rs?.fps).filter(v => v != null && v > 0);
    const frameMsVals = samples.map(s => s.rs?.frameMsMean).filter(v => v != null && v > 0);

    const first = samples[0], last = samples[samples.length - 1];
    const wall = last.t - first.t;
    const frameDelta = (last.rs?.frameCount ?? 0) - (first.rs?.frameCount ?? 0);
    const uploadDelta = (last.rs?.renderServices?.instanceUploadBytesTotal ?? 0)
                      - (first.rs?.renderServices?.instanceUploadBytesTotal ?? 0);
    const packDelta = (last.rs?.renderServices?.materialSSBOPackCount ?? 0)
                    - (first.rs?.renderServices?.materialSSBOPackCount ?? 0);
    const fps = stats(fpsVals);
    const frameMs = stats(frameMsVals);
    const uploadBytesPerSec = wall > 0 ? uploadDelta / wall : 0;
    // Exact per-frame average over the window via the app frame counter.
    const uploadBytesPerFrame = frameDelta > 0 ? uploadDelta / frameDelta : 0;

    // GPU pass means by name across samples.
    // Σ over distinct GPU measurements (see get_gpu_profiler timingSemantics);
    // equal to the per-pass sum on pipeline-point backends.
    const totalGpuVals = samples
        .filter(s => s.gpu?.lastFrame)
        .map(s => s.gpu?.resolveStats?.distinctSpanGpuMs ?? 0);
    // Group totals count a shared encoder span once (gpu-pass-totals.mjs).
    const passMeans = reducePassTimings(samples);

    // P0 shadow-arc: stitch the per-cascade passed-caster + drawn-triangle counts
    // (get_render_stats.shadowArc, last steady-state sample) with the per-cascade
    // GPU ms (profiler pass "…CascadedShadowMap.View{id}.Cascade{c}", windowed
    // mean). The two come from different sources by design — the counts are NOT
    // duplicated into the profiler and the timing is NOT duplicated into
    // get_render_stats; the join lives here.
    const cascadeMsByKey = new Map();
    for (const p of passMeans) {
        const m = /CascadedShadowMap\.View(\d+)\.Cascade(\d+)$/.exec(p.name);
        if (m) cascadeMsByKey.set(`${m[1]}/${m[2]}`, p.gpuSpanMs);
    }
    const arcBlock = last.rs?.renderServices?.shadowArc;
    const shadowArcCascades = (arcBlock?.slices ?? [])
        .filter(s => s.cascadeIndex <= 3) // directional cascades only
        .map(s => ({
            viewId: s.viewId,
            cascade: s.cascadeIndex,
            passedCasters: s.passedCasters,
            drawnTriangles: s.drawnTriangles,
            gpuMs: cascadeMsByKey.get(`${s.viewId}/${s.cascadeIndex}`) ?? null,
        }))
        .sort((x, y) => (x.viewId - y.viewId) || (x.cascade - y.cascade));
    // Main/color view slice (cascadeIndex 0xFF): the cascade filter above keeps only
    // 0..3, so this primary-camera emitted-record (passedCaster) count is otherwise
    // dropped. It is the HLOD v1 main-view win metric — surfaced distinctly, the existing
    // cascade schema untouched. drawnTriangles is gated behind GE_SCATTER_STATS like the
    // cascade slices; passedCasters is valid regardless.
    const mainColorSlice = (arcBlock?.slices ?? []).find(s => s.cascadeIndex === 0xFF) ?? null;
    const shadowArc = {
        cascades: shadowArcCascades,
        mainColor: mainColorSlice
            ? { passedCasters: mainColorSlice.passedCasters, drawnTriangles: mainColorSlice.drawnTriangles }
            : null,
        tableMisses: arcBlock?.tableMisses ?? null,
        overflows: arcBlock?.overflows ?? null,
        // False on the shipping default (GE_SCATTER_STATS off): drawnTriangles is
        // compiled out and reads 0 — not a real triangle count. --scatter-stats
        // opts in. Tripwires/passedCasters are valid regardless.
        statsEnabled: arcBlock?.statsEnabled ?? null,
    };

    const culling = last.rs?.renderServices?.gpuCulling;
    const mainRange = culling?.ranges?.[0];

    // A2 STEP-0 render-thread CPU decomposition — means over the sampling window
    // (each field is last-frame per sample). extraction is the ECS-update
    // segment; sort/bucketer + RG phases + submit/barrier-emit are the
    // extraction-return→present render-thread wall; recordWalk is the summed
    // per-pass cpuMs from get_gpu_profiler (populated only when profiling is
    // armed). bucketSum vs frameMsMean quantifies the instrumentation residual.
    const meanOf = (pick) => {
        const vals = samples.map(pick).filter(v => v != null && Number.isFinite(v));
        return vals.length ? vals.reduce((x, v) => x + v, 0) / vals.length : 0;
    };
    const tlOf = (s) => s.rs?.renderServices?.renderThreadCpu ?? {};
    const rgOf = (s) => tlOf(s).renderGraph ?? {};
    const exOf = (s) => s.rs?.renderServices?.extraction ?? {};
    const cpu = {
        extractionTotalMs: meanOf(s => exOf(s).totalMs),
        extractionGatherMs: meanOf(s => exOf(s).gatherMs),
        extractionProcessMs: meanOf(s => exOf(s).processMs),
        // A2.1 parallel-lane sub-splits (0 in the serial OFF lane).
        extractionPrepareMs: meanOf(s => exOf(s).prepareMs),
        extractionApplyMs: meanOf(s => exOf(s).applyMs),
        sortMs: meanOf(s => tlOf(s).sortMs),
        sortViewCount: last.rs?.renderServices?.renderThreadCpu?.sortViewCount ?? null,
        bucketerScheduleMs: meanOf(s => tlOf(s).bucketerScheduleMs),
        rgCompileMs: meanOf(s => rgOf(s).compileMs),
        rgScheduleMs: meanOf(s => rgOf(s).scheduleMs),
        rgGenerateBarriersMs: meanOf(s => rgOf(s).generateBarriersMs),
        rgBuildSubmissionPlanMs: meanOf(s => rgOf(s).buildSubmissionPlanMs),
        queueSubmitMs: meanOf(s => rgOf(s).queueSubmitMs),
        barrierEmitMs: meanOf(s => rgOf(s).barrierEmitMs),
        recordWalkCpuMs: meanOf(s => s.gpu?.lastFrame?.totalCpuMs),
    };
    cpu.bucketSumMs = cpu.extractionTotalMs + cpu.sortMs + cpu.bucketerScheduleMs
        + cpu.rgCompileMs + cpu.rgScheduleMs + cpu.rgGenerateBarriersMs
        + cpu.rgBuildSubmissionPlanMs + cpu.queueSubmitMs + cpu.barrierEmitMs
        + cpu.recordWalkCpuMs;
    cpu.frameMsMean = frameMs.mean;
    cpu.residualMs = frameMs.mean - cpu.bucketSumMs;
    cpu.residualPct = frameMs.mean > 0 ? (cpu.residualMs / frameMs.mean) * 100 : 0;

    const summary = {
        fps: { mean: fps.mean, min: fps.min, p50: fps.p50 },
        frameMs: { mean: frameMs.mean, p95: frameMs.p95, max: frameMs.max },
        instanceUpload: {
            bytesPerSec: uploadBytesPerSec,
            bytesPerFrame: uploadBytesPerFrame,
            mbPerFrame: uploadBytesPerFrame / (1024 * 1024),
        },
        materialPackDeltaOverWindow: packDelta,
        framesOverWindow: frameDelta,
        effectiveFps: wall > 0 ? frameDelta / wall : 0,
        gpuSceneInstanceCount: last.rs?.renderServices?.gpuSceneInstanceCount ?? null,
        // Live (non-tombstoned) GPUScene instance slots. get_render_stats already exposes
        // it; surfaced here as an HLOD residency signal (proxy admit/evict moves it).
        gpuSceneLiveInstanceCount: last.rs?.renderServices?.gpuSceneLiveInstanceCount ?? null,
        entityCount: last.rs?.entityCount ?? null,
        drawStreamSlots: last.rs?.renderServices?.drawStreamSlots ?? null,
        // A2.1 determinism cross-check (must match OFF vs ON, last frame).
        rebuildCount: last.rs?.renderServices?.extraction?.rebuildCount ?? null,
        skippedCount: last.rs?.renderServices?.extraction?.skippedCount ?? null,
        // Candidate count only — per-instance visibility is GPU-side (no CPU
        // readback since D6); occlusion efficacy metrics arrive with HZB stats.
        mainView: mainRange ? { candidates: mainRange.visibilityCount } : null,
        cpu,
        gpu: {
            totalGpuMs: stats(totalGpuVals).mean,
            bucketerMs: groupGpuMs(passMeans, 'GPUDrawStream.'),
            cullingMs: groupGpuMs(passMeans, 'GPUCulling.'),
            topPasses: passMeans.slice(0, 14).map(({ name, gpuSpanMs }) => ({ name, gpuSpanMs })),
        },
        shadowArc,
    };

    const result = {
        label: a.label, capturedAt: new Date().toISOString(),
        gitSha: gitSha(repoRoot),
        config: { ...a }, editorPid, editorState,
        spawnResult, skinnedResults, summary, samples,
    };
    const outDir = path.join(scriptDir, 'results');
    fs.mkdirSync(outDir, { recursive: true });
    const outPath = path.join(outDir, `${a.label}.json`);
    fs.writeFileSync(outPath, JSON.stringify(result, null, 2));

    console.log('\n===== BENCH SUMMARY =====');
    console.log(`fps            mean ${fps.mean.toFixed(1)}  min ${fps.min.toFixed(1)}  p50 ${fps.p50.toFixed(1)}  (effective ${summary.effectiveFps.toFixed(1)} over ${frameDelta} frames)`);
    console.log(`frameMs        mean ${frameMs.mean.toFixed(2)}  p95 ${frameMs.p95.toFixed(2)}  max ${frameMs.max.toFixed(2)}`);
    console.log(`instanceUpload ${summary.instanceUpload.mbPerFrame.toFixed(2)} MB/frame  (${(uploadBytesPerSec / (1024 * 1024)).toFixed(1)} MB/s)`);
    console.log(`gpuScene       instances ${summary.gpuSceneInstanceCount}  live ${summary.gpuSceneLiveInstanceCount}  entities ${summary.entityCount}  slots ${summary.drawStreamSlots}`);
    if (summary.mainView)
        console.log(`mainView       candidates ${summary.mainView.candidates}`);
    if (shadowArc.mainColor)
        console.log(`mainViewEmit   passedCasters ${shadowArc.mainColor.passedCasters}  drawnTriangles ${shadowArc.mainColor.drawnTriangles}`);
    console.log(`materialPacks  +${packDelta} over ${wall.toFixed(1)} s window`);
    console.log(`cpu timeline   extract ${cpu.extractionTotalMs.toFixed(2)}  sort ${cpu.sortMs.toFixed(3)} (${cpu.sortViewCount ?? '?'} views)  bucketer ${cpu.bucketerScheduleMs.toFixed(3)}`);
    console.log(`  extraction   process ${cpu.extractionProcessMs.toFixed(3)}  prepare ${cpu.extractionPrepareMs.toFixed(3)}  apply ${cpu.extractionApplyMs.toFixed(3)}  (rebuild ${summary.rebuildCount} skipped ${summary.skippedCount})`);
    console.log(`  rg           compile ${cpu.rgCompileMs.toFixed(3)}  schedule ${cpu.rgScheduleMs.toFixed(3)}  genBarriers ${cpu.rgGenerateBarriersMs.toFixed(3)}  buildPlan ${cpu.rgBuildSubmissionPlanMs.toFixed(3)}`);
    console.log(`  record       walk(cpuMs) ${cpu.recordWalkCpuMs.toFixed(3)}  submit ${cpu.queueSubmitMs.toFixed(3)}  barrierEmit ${cpu.barrierEmitMs.toFixed(3)}`);
    console.log(`  reconcile    bucketSum ${cpu.bucketSumMs.toFixed(2)} vs frameMs ${cpu.frameMsMean.toFixed(2)}  residual ${cpu.residualMs.toFixed(2)} (${cpu.residualPct.toFixed(1)}%)`);
    console.log(`gpu totals     frame ${summary.gpu.totalGpuMs.toFixed(2)} ms  bucketer ${summary.gpu.bucketerMs.toFixed(3)} ms  culling ${summary.gpu.cullingMs.toFixed(3)} ms`);
    for (const p of summary.gpu.topPasses.slice(0, 10))
        console.log(`  ${p.gpuSpanMs.toFixed(3).padStart(8)} ms  ${p.name}`);
    if (shadowArc.cascades.length) {
        const statsNote = shadowArc.statsEnabled === false
            ? '  drawnTriangles GATED OFF (0s) — pass --scatter-stats to measure'
            : '';
        console.log(`shadowArc      tableMisses ${shadowArc.tableMisses}  overflows ${shadowArc.overflows}  (git ${result.gitSha?.slice(0, 12) ?? 'n/a'})${statsNote}`);
        console.log(`  view/casc   passedCasters      drawnTriangles     gpuMs`);
        for (const c of shadowArc.cascades)
            console.log(`  ${String(c.viewId).padStart(4)}/${c.cascade}    ${String(c.passedCasters).padStart(12)}   ${String(c.drawnTriangles).padStart(16)}   ${c.gpuMs != null ? c.gpuMs.toFixed(3).padStart(8) : '     n/a'}`);
    }
    console.log(`\nResult written: ${outPath}`);
    } finally {
        // Reap the editor this run launched so back-to-back --launch runs don't collide
        // on the port. --keep-editor opts out (and attach/--skip-spawn never launched one).
        if (a.launch && !a.keepEditor)
            killLaunchedEditor(editorChild);
        else if (a.launch && editorPid != null)
            console.log(`  --keep-editor: leaving editor pid ${editorPid} running on port ${a.port}`);
    }
}

main().catch(e => { console.error(e); process.exit(1); });
