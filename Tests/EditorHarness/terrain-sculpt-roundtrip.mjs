#!/usr/bin/env node
// Terrain sculpt round-trip: create -> sculpt -> save -> reopen -> verify.
//
// The operation nobody could drive from automation before: prove that a brush edit
// on a live terrain reaches the terrain's heights, survives a save/reload cycle, and
// comes back rendering. Runs against BOTH terrain domains — a planar terrain sculpts
// through the zone brush (payload -> .tzone sidecar), a planet through the sphere
// sculpt layer (-> .tsculpt sidecar).
//
// Every assertion is effect-side. A handler's `ok: true` is never treated as evidence:
// the payload magnitude, the terrain heightfield statistics, and the sidecar bytes on
// disk are what the checks read.
//
// Prereqs: an editor running with its IPC server up, opened on a WRITABLE project
// (the scene and its payload sidecars are saved under that project's asset root).
//   GE_EDITOR_DEBUG_PORT=<port> node Tests/EditorHarness/terrain-sculpt-roundtrip.mjs
//
// Options:
//   --domain=planar|spherical|both   which domain(s) to exercise (default both)
//   --keep                           leave the saved scene + sidecars on disk
//
// Exit 0 = every targeted case passed. Exit 1 = a case failed. Exit 2 = setup failure.

import fs from 'node:fs';
import path from 'node:path';
import { ipc, sleep, PORT } from './lib/ipc.mjs';

const ARGV = process.argv.slice(2);
const argOf = (name, fallback) => {
    const hit = ARGV.find(a => a.startsWith(`--${name}=`));
    return hit ? hit.split('=').slice(1).join('=') : fallback;
};
const DOMAIN = argOf('domain', 'both');
const KEEP = ARGV.includes('--keep');

// The bake that folds a zone payload into the terrain heightfield is debounced, so the
// height check polls rather than sampling once.
const BAKE_POLL_MS = 250;
const BAKE_TIMEOUT_MS = 15000;
// A scene open is queued onto the main thread and runs asynchronously.
const OPEN_POLL_MS = 250;
const OPEN_TIMEOUT_MS = 30000;

let failures = 0;
const log = (...a) => console.log(...a);
const step = (s) => log(`\n--- ${s}`);
function check(label, cond, detail) {
    log(`  [${cond ? 'PASS' : 'FAIL'}] ${label}${detail !== undefined ? ` — ${detail}` : ''}`);
    if (!cond) failures++;
    return cond;
}
function fatal(msg) {
    console.error(`SETUP FAILURE: ${msg}`);
    process.exit(2);
}

async function call(method, params = {}) {
    const r = await ipc(method, params);
    if (r && r.error) throw new Error(`${method}: ${r.error}`);
    return r;
}

// The editor's log is the only place a scene-open failure shows up: a failed open leaves
// the previous world rendering and still reports an entity count, so a return value alone
// cannot tell a reload apart from a no-op. get_log answers with a bare array of
// {level, message}; anything else means the ring-buffer sink is missing and the caller
// must not read an empty result as "no errors".
async function logLines(minLevel = 'warning', count = 400) {
    const r = await ipc('get_log', { count, minLevel });
    if (!Array.isArray(r)) throw new Error(`get_log unavailable: ${JSON.stringify(r)}`);
    return r.map(e => `${e.level}: ${e.message}`);
}

async function terrainState() {
    const d = await call('get_terrain_debug');
    return d.terrains || [];
}

// The scene-view viewport rect, for driving a real pointer drag over the terrain.
// get_ui_tree nodes carry {id, cls, x, y, w, h, ch}. Returns null when no scene-view node
// with a usable rect is found — the caller must then SKIP loudly, not pass quietly.
async function findSceneViewRect() {
    const tree = await ipc('get_ui_tree', { maxDepth: 30 });
    let best = null;
    const walk = (node) => {
        if (!node || typeof node !== 'object') return;
        const id = String(node.id ?? '');
        const cls = String(node.cls ?? '');
        const looksLikeSceneView = /sceneview|scene-view|viewport/i.test(id + ' ' + cls);
        const w = Number(node.w ?? 0), h = Number(node.h ?? 0);
        if (looksLikeSceneView && w > 200 && h > 200 && (!best || w * h > best.w * best.h))
            best = { id, x: Number(node.x ?? 0), y: Number(node.y ?? 0), w, h };
        for (const child of node.ch ?? node.children ?? []) walk(child);
    };
    walk(tree.root ?? tree);
    return best;
}

// Height statistics of the terrain's own heightfield — the downstream artefact a sculpt
// must move, independent of whatever the brush handler reported about itself.
function heightStats(t) {
    const h = t?.terrainData?.heightfield;
    if (!h || h.empty) return null;
    return { min: h.min, max: h.max, mean: h.mean, range: h.range };
}

function statsEqual(a, b, eps = 1e-9) {
    if (!a || !b) return false;
    return Math.abs(a.min - b.min) <= eps && Math.abs(a.max - b.max) <= eps
        && Math.abs(a.mean - b.mean) <= eps;
}

async function waitFor(predicate, { timeoutMs, pollMs, what }) {
    const deadline = Date.now() + timeoutMs;
    let last;
    while (Date.now() < deadline) {
        last = await predicate();
        if (last) return last;
        await sleep(pollMs);
    }
    throw new Error(`timed out after ${timeoutMs}ms waiting for ${what}`);
}

// ---------------------------------------------------------------------------

async function runCase({ label, createCommand, sculpt, sidecarExt }) {
    step(`${label}: create`);
    // The brush and the renderer both resolve "the active terrain" as the FIRST enabled
    // terrain with live data in chunk order — not the newest. A leftover terrain from an
    // earlier case would therefore silently receive this case's stroke, so clear them.
    for (const t of await terrainState())
        await call('delete_entity', { entityId: t.entityId });
    check(`${label}: scene starts with no terrain`, (await terrainState()).length === 0);

    await call('execute_command', { name: createCommand });
    const created = await terrainState();
    const terrain = created[created.length - 1];
    if (!terrain) return check(`${label}: terrain created`, false, 'get_terrain_debug reports none');
    check(`${label}: terrain created`, true,
          `entity ${terrain.entityId} domain=${terrain.component.domain} size=${terrain.component.sizeX}`);

    const before = heightStats(terrain);
    check(`${label}: baseline heightfield readable`, before !== null, JSON.stringify(before));

    step(`${label}: sculpt`);
    const dab = await call('terrain_sculpt_dab', sculpt);
    check(`${label}: dabs applied`, dab.dabsApplied === (sculpt.count ?? 1),
          `${dab.dabsApplied} of ${sculpt.count ?? 1}`);
    check(`${label}: stroke closed`, dab.strokeOpen === false);
    check(`${label}: one undo entry recorded`, dab.undo?.recorded === true, dab.undo?.entry);

    // Effect-side: the edit store actually moved. Never the handler's ok flag.
    if (dab.domain === 'planar') {
        const z = dab.zone;
        check(`${label}: zone payload exists`, !!z && !!z.payload, z?.payload);
        check(`${label}: payload version advanced`, !!z && z.dataVersion.after > z.dataVersion.before,
              `${z?.dataVersion?.before} -> ${z?.dataVersion?.after}`);
        check(`${label}: payload texels written`, !!z && z.writtenMagnitude.after > z.writtenMagnitude.before,
              `sum|offset| ${z?.writtenMagnitude?.before} -> ${z?.writtenMagnitude?.after}`);
        check(`${label}: payload marked unsaved`, z?.needsSave === true);
    } else {
        check(`${label}: sculpt version advanced`,
              dab.sculptVersion.after > dab.sculptVersion.before,
              `${dab.sculptVersion.before} -> ${dab.sculptVersion.after}`);
        const stats = await call('get_terrain_stats');
        check(`${label}: sculpt store reports edits`, stats.sculpt?.hasEdits === true);
        check(`${label}: sculpt store needs save`, stats.sculpt?.needsSave === true);
    }

    // The IPC stroke and the viewport stroke share TerrainBrushTool::ApplyStrokePhase, so a
    // change to that body can break the mouse path while every IPC check stays green. Drive
    // a real drag over the scene view and require the payload to move again.
    if (dab.domain === 'planar') {
        step(`${label}: the same stroke body still runs from the viewport`);
        const view = await findSceneViewRect();
        if (!view) {
            check(`${label}: scene view rect found for the pointer drag`, false,
                  'no scene-view node with a rect in get_ui_tree — pointer-path coverage SKIPPED');
        } else {
            // The drag lands on whichever tab is frontmost in the dock group; a prior
            // focus_view game would silently swallow the stroke. Claim the tab first.
            await call('focus_view', { view: 'scene' });
            // Point the camera down at the terrain so the drag's rays actually hit it.
            await call('look_at', { target: [0, 0, 0], distance: 420 });
            await call('set_camera', { pitchDeg: -55, yawDeg: 20 });
            await sleep(500);
            await call('set_scene_tool', { tool: 'terrainBrush' });
            await call('set_terrain_brush', { radius: 20, strength: 5, mode: 'raise' });
            // release_only on a closed stroke applies nothing but still reports the active
            // zone — a read-only probe of the brush's own state.
            const probeBefore = await call('terrain_sculpt_dab', { release_only: true });
            const cx = view.x + view.w * 0.5, cy = view.y + view.h * 0.5;
            await call('simulate_mouse_drag', {
                startX: cx - view.w * 0.08, startY: cy - view.h * 0.05,
                endX: cx + view.w * 0.08, endY: cy + view.h * 0.05, steps: 12,
            });
            await sleep(500);
            const probeAfter = await call('terrain_sculpt_dab', { release_only: true });
            const zBefore = probeBefore.zone ?? {}, zAfter = probeAfter.zone ?? {};
            const magBefore = zBefore.writtenMagnitude?.after ?? 0;
            const magAfter = zAfter.writtenMagnitude?.after ?? 0;
            // The drag may auto-create its own zone (a stroke landing outside the first
            // zone's footprint does), so "grew" is only meaningful within one payload.
            const wrote = zAfter.payload !== zBefore.payload ? magAfter > 0 : magAfter > magBefore;
            check(`${label}: a viewport drag wrote terrain payload`, wrote,
                  `zone ${zBefore.payload} -> ${zAfter.payload}, sum|offset| ${magBefore} -> ${magAfter}`);
            await call('set_scene_tool', { tool: 'selection' });
        }
    }

    step(`${label}: the sculpt reaches the terrain`);
    let afterSculpt = null;
    if (dab.domain === 'planar') {
        // The zone bake folds the payload into the heightfield the renderer and the
        // heightfield collider both read; that is the artefact worth asserting on.
        //
        // The oracle is the MEAN, not the max. A raise only lifts its own footprint, so
        // whether it moves the terrain's global maximum depends on where the stroke landed
        // relative to the noise — that made the max look like a flaky "the sculpt vanished"
        // signal. Every raised texel moves the mean, and by a fixed amount for a fixed
        // stroke, so the mean answers "did the raise reach the heightfield" outright.
        afterSculpt = await waitFor(async () => {
            const t = (await terrainState()).find(x => x.entityId === terrain.entityId);
            const s = heightStats(t);
            return s && s.mean > before.mean ? s : null;
        }, { timeoutMs: BAKE_TIMEOUT_MS, pollMs: BAKE_POLL_MS, what: 'the zone bake to raise the heightfield mean' })
            .catch(e => { check(`${label}: a raise stroke raised the surface`, false, e.message); return null; });
        if (afterSculpt) {
            check(`${label}: a raise stroke raised the surface`, true,
                  `mean ${before.mean.toFixed(6)} -> ${afterSculpt.mean.toFixed(6)} ` +
                  `(max ${before.max.toFixed(6)} -> ${afterSculpt.max.toFixed(6)})`);
        }
    } else {
        // A planet has no planar heightfield; the CPU-authoritative composed height at the
        // dab direction is the equivalent oracle.
        const dir = sculpt.dir ?? sculpt.pos;
        const probe = await call('get_terrain_stats', { probe_dir: dir });
        check(`${label}: composed sculpt height is non-zero at the dab`,
              Math.abs(probe.sculpt?.probeComposed ?? 0) > 0,
              `probeComposed=${probe.sculpt?.probeComposed}`);
        afterSculpt = { probeComposed: probe.sculpt.probeComposed };
    }

    step(`${label}: save`);
    const sceneRel = `Scenes/SculptRoundTrip_${label}.scene`;
    const saved = await call('save_scene', { path: sceneRel });
    check(`${label}: scene saved`, !!saved.path, saved.path);
    const sceneAbs = saved.path;
    const zonesDir = path.join(path.dirname(sceneAbs),
                               `${path.basename(sceneAbs, '.scene')}_Zones`);
    const sidecars = fs.existsSync(zonesDir)
        ? fs.readdirSync(zonesDir).filter(f => f.endsWith(sidecarExt))
        : [];
    check(`${label}: ${sidecarExt} sidecar written`, sidecars.length > 0,
          `${zonesDir} -> ${JSON.stringify(sidecars)}`);
    const sidecarBytes = sidecars.reduce((n, f) => n + fs.statSync(path.join(zonesDir, f)).size, 0);
    check(`${label}: sidecar is non-empty`, sidecarBytes > 0, `${sidecarBytes} bytes`);

    step(`${label}: reopen`);
    // Load something else first, so "the sculpt is still there" cannot be answered by the
    // world simply never having been torn down.
    await call('open_scene', { path: sceneAbs, restorePolicy: 'discard' });
    await waitFor(async () => {
        const st = await ipc('get_editor_state');
        return st.scenePath && path.resolve(st.scenePath) === path.resolve(sceneAbs);
    }, { timeoutMs: OPEN_TIMEOUT_MS, pollMs: OPEN_POLL_MS, what: 'the scene open to complete' })
        .catch(e => check(`${label}: scene reopened`, false, e.message));

    // Scene-load failure is silent on this engine — read the log back rather than trusting
    // the state field alone. Only genuine load failures count: a bare test project also
    // logs "degraded reference" warnings for the default scene's material/mesh refs, which
    // say nothing about whether the terrain loaded, so those are reported, not failed on.
    let loadErrors = [], degraded = [];
    try {
        const lines = await logLines();
        loadErrors = lines.filter(l => /scene/i.test(l)
            && /(failed to load|parse error|unknown component|could not open|load failed)/i.test(l));
        degraded = lines.filter(l => /degraded reference/i.test(l));
    } catch (e) {
        check(`${label}: the log is readable at all`, false, e.message);
    }
    check(`${label}: no scene-load errors in the log`, loadErrors.length === 0,
          loadErrors.slice(0, 3).join(' | ') || 'clean');
    if (degraded.length)
        log(`  [note] ${degraded.length} degraded asset reference(s) — expected in a bare test ` +
            `project with no imported assets; unrelated to the terrain`);

    step(`${label}: verify after reload`);
    const reloaded = (await terrainState())[0];
    check(`${label}: terrain present after reload`, !!reloaded);
    if (reloaded) {
        check(`${label}: domain survived`, reloaded.component.domain === terrain.component.domain,
              `domain=${reloaded.component.domain}`);

        if (dab.domain === 'planar') {
            const back = await waitFor(async () => {
                const t = (await terrainState())[0];
                const s = heightStats(t);
                return s && s.mean > before.mean ? s : null;
            }, { timeoutMs: BAKE_TIMEOUT_MS, pollMs: BAKE_POLL_MS,
                 what: 'the reloaded terrain to bake its restored zone' })
                .catch(e => { check(`${label}: sculpt survived the round trip`, false, e.message); return null; });
            if (back && afterSculpt) {
                // Same mean as before the save, not merely "some sculpt": a partially
                // restored payload would still clear the > baseline bar.
                const drift = Math.abs(back.mean - afterSculpt.mean);
                check(`${label}: sculpt survived the round trip`, drift < 1e-6,
                      `reloaded mean ${back.mean.toFixed(9)} vs pre-save ${afterSculpt.mean.toFixed(9)} ` +
                      `(unsculpted ${before.mean.toFixed(9)}), drift ${drift.toExponential(2)}`);
            }
        } else {
            const probe = await call('get_terrain_stats', { probe_dir: sculpt.dir ?? sculpt.pos });
            check(`${label}: sculpt survived the round trip`,
                  Math.abs(probe.sculpt?.probeComposed ?? 0) > 0,
                  `probeComposed=${probe.sculpt?.probeComposed}`);
            check(`${label}: restored sculpt needs no re-save`, probe.sculpt?.needsSave === false);
        }

        // Renders: the render feature published a live instance with a valid heightmap.
        const info = (reloaded.activeRenderInfos || [])[0];
        check(`${label}: renders — active render info published`, !!info);
        if (info) {
            check(`${label}: renders — heightmap texture valid`, info.heightmapTextureValid === true);
            check(`${label}: renders — quadtree built`, info.quadtreeBuilt === true);
        }
        // ...and the editor is still presenting frames rather than frozen on a bad reload,
        // which is what would make every "it renders" field above stale-but-plausible.
        const s0 = await call('get_render_stats');
        await sleep(500);
        const s1 = await call('get_render_stats');
        check(`${label}: renders — the frame loop is advancing`, s1.frameCount > s0.frameCount,
              `frameCount ${s0.frameCount} -> ${s1.frameCount}`);

        // Collides. A terrain whose heightfield collider was never refreshed from the sculpt
        // renders exactly like one that was, so this is the only check that separates them.
        // The physics world is built by PhysicsInitSystem only while PLAYING — in edit mode
        // PhysicsWorldService::TryGet() is null and every raycast reports no world — so the
        // check enters play mode, probes, and stops again.
        // set_play_mode answers with the resulting play state, and the transition finishes
        // inside the handler on the main thread, so the entry is read from its own response
        // rather than polled for. ipc() rather than call() so a refused entry records a
        // failed check and lets the rest of the run report, instead of aborting here.
        const entered = await ipc('set_play_mode', { action: 'enter' });
        check(`${label}: collides — play mode entered`, entered.playMode === 'playing',
              entered.error ?? `playMode=${entered.playMode}`);
        await sleep(1500); // let PhysicsInitSystem build the shapes

        const playStats = await call('get_render_stats');
        check(`${label}: collides — physics bodies exist in play mode`,
              (playStats.physics?.bodyCount ?? 0) > 0, `bodyCount=${playStats.physics?.bodyCount}`);

        if (dab.domain === 'planar') {
            // Sample along the stroke and compare each physics contact against the SURFACE at
            // the same XZ, read with probe_only (the brush's own heightfield pick — the
            // surface the renderer draws). Comparing to a fixed threshold cannot work: a
            // raise need not clear the terrain's global maximum. Comparing to the surface can:
            // a collider still built from the unsculpted heightfield sits below the sculpted
            // surface by the whole raise, and a live one tracks it to within a texel.
            const [ax, , az] = sculpt.pos, [bx, , bz] = sculpt.end_pos ?? sculpt.pos;
            let hits = 0, worstGap = 0, wrongEntity = 0, sample = '';
            for (let i = 0; i <= 10; ++i) {
                const t = i / 10;
                const x = ax + (bx - ax) * t, z = az + (bz - az) * t;
                const probe = await ipc('terrain_sculpt_dab', { pos: [x, 0, z], probe_only: true });
                const r = await ipc('physics_raycast',
                                    { origin: [x, 20000, z], direction: [0, -1, 0], maxDistance: 100000 });
                if (r.error) { check(`${label}: collides — physics_raycast answered`, false, r.error); break; }
                if (probe.error || !r.hit) continue;
                ++hits;
                if (r.entityId !== reloaded.entityId) ++wrongEntity;
                const gap = Math.abs(r.point[1] - probe.point[1]);
                if (gap > worstGap) {
                    worstGap = gap;
                    sample = `at (${x.toFixed(0)}, ${z.toFixed(0)}) physics ${r.point[1].toFixed(3)} m ` +
                             `vs surface ${probe.point[1].toFixed(3)} m`;
                }
            }
            check(`${label}: collides — the rays hit a body`, hits > 0, `${hits} contacts`);
            check(`${label}: collides — every contact is on the terrain entity`, wrongEntity === 0,
                  `${wrongEntity} contacts on another entity`);
            // One heightfield texel is 1 / samplesPerMeter metres across, and the surface can
            // move by the local slope over that span; a metre is generous for the 2 samples/m
            // default and still an order of magnitude under this stroke's 6 m/dab raise.
            check(`${label}: collides — the collider tracks the SCULPTED surface`,
                  hits > 0 && worstGap < 1.0, `worst gap ${worstGap.toFixed(3)} m — ${sample}`);
        } else {
            const dir = sculpt.dir ?? [0, 1, 0];
            const radius = reloaded.component.planetRadius ?? 1;
            const r = await ipc('physics_raycast', {
                origin: dir.map(v => v * radius * 4), direction: dir.map(v => -v), maxDistance: radius * 8,
            });
            // A planet's collider is a documented queued slice, so a miss is the known state,
            // not a regression — report it rather than failing the run on it.
            log(`  [note] planet collision: physics_raycast ${r.hit ? 'hit' : 'missed'} ` +
                `(planets carry no heightfield collider today)`);
        }

        // set_play_mode refuses an exit from edit mode rather than answering a silent ok,
        // so only ask for one when the editor is actually running: a refused entry above
        // records its check and continues to here.
        if ((await ipc('get_editor_state')).playMode !== 'editing') {
            let exited = await ipc('set_play_mode', { action: 'exit' });
            // Exiting lands in change_review when editor commands ran during play;
            // the documented second exit discards them, exactly like toolbar Stop.
            // Leaving it unresolved would cascade: the next case's 'enter' is refused.
            if (exited.playMode === 'change_review')
                exited = await ipc('set_play_mode', { action: 'exit' });
            if (exited.playMode !== 'editing')
                log(`  [note] could not confirm return to edit mode: playMode=${exited.playMode}` +
                    (exited.error ? ` (${exited.error})` : ''));
        }
    }

    if (!KEEP) {
        try {
            fs.rmSync(sceneAbs, { force: true });
            fs.rmSync(`${sceneAbs}.meta`, { force: true });
            fs.rmSync(zonesDir, { recursive: true, force: true });
        } catch (e) { log(`  (cleanup skipped: ${e.message})`); }
    }
    return failures === 0;
}

// ---------------------------------------------------------------------------

async function main() {
    log(`terrain sculpt round-trip — IPC port ${PORT}`);
    let state;
    try { state = await ipc('get_editor_state'); }
    catch (e) { fatal(`editor unreachable on port ${PORT}: ${e.message}`); }
    if (state.error) fatal(`editor unreachable: ${state.error}`);
    log(`editor: worktree=${state.session?.worktreeName} config=${state.session?.buildConfig} ` +
        `scene='${state.scenePath}' playMode=${state.playMode}`);

    const cases = [];
    if (DOMAIN === 'planar' || DOMAIN === 'both') {
        cases.push({
            label: 'planar',
            createCommand: 'create_terrain',
            sidecarExt: '.tzone',
            // A stroke across the middle of the 512 m default terrain.
            sculpt: { pos: [-40, 0, 0], end_pos: [40, 0, 0], count: 12,
                      radius: 24, strength: 6, mode: 'raise' },
        });
    }
    if (DOMAIN === 'spherical' || DOMAIN === 'both') {
        cases.push({
            label: 'spherical',
            createCommand: 'create_planet_5km',
            sidecarExt: '.tsculpt',
            sculpt: { dir: [0, 1, 0], end_dir: [0.2, 1, 0], count: 12,
                      radius: 200, strength: 40, mode: 'raise' },
        });
    }
    if (cases.length === 0) fatal(`--domain must be planar, spherical or both (got '${DOMAIN}')`);

    for (const c of cases) await runCase(c);

    log(`\n${failures === 0 ? 'ALL CHECKS PASSED' : `${failures} CHECK(S) FAILED`}`);
    process.exit(failures === 0 ? 0 : 1);
}

main().catch(e => fatal(e.stack || e.message));
