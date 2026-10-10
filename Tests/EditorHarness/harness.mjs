#!/usr/bin/env node
// Integration test harness — drives the live editor via IPC,
// edits assets, captures screenshots, validates pre/post deltas.
//
// Prereqs: editor must already be running with the IPC server on 127.0.0.1:9999
// (GE_EDITOR_DEBUG_PORT overrides the port for both the editor and this client).
// Run from repo root:
//   node Tests/EditorHarness/harness.mjs            # run all
//   node Tests/EditorHarness/harness.mjs --only=tag1,tag2
//
// Available tags: screenshot-modes, rapid-select, gizmo-visibility, rg-toggle,
//                 texture, texture-scene, search-dialog-scroll
//
// Exit code 0 = all targeted scenarios passed (skipped is allowed).
// Exit code 1 = at least one scenario failed.
// Exit code 2 = harness setup failure (editor unreachable, etc.)

import fs from 'node:fs';
import path from 'node:path';
import { ipc, takeScreenshot, sleep, PORT as IPC_PORT } from './lib/ipc.mjs';
import {
    EDITOR_ASSETS_ROOT, RESULTS_DIR, BACKUPS_DIR,
    ensureDirs, backupFile, restoreBackup,
    shootAndDecode, diffScreenshots, logStep, pingEditor, waitForReload,
    ensureEditorFocused, avgRegion,
} from './lib/helpers.mjs';
import { encodeSolidPNG } from './lib/pngwrite.mjs';

const ARGS = new Set(process.argv.slice(2));
const ONLY = process.argv.find(a => a.startsWith('--only='));
const ONLY_SET = ONLY ? new Set(ONLY.split('=')[1].split(',')) : null;
function shouldRun(name) { return !ONLY_SET || ONLY_SET.has(name); }

const results = [];
function record(name, status, notes, files = {}) {
    results.push({ name, status, notes, files });
    const symbol = status === 'pass' ? 'PASS' : status === 'fail' ? 'FAIL' : 'SKIP';
    console.log(`[${symbol}] ${name} — ${notes}`);
}

const toRestore = [];
function trackForRestore(b) { toRestore.push(b); }
async function restoreAll() {
    for (const b of toRestore) {
        try {
            restoreBackup(b);
            console.log(`Restored: ${path.basename(b.original)}`);
        }
        catch (e) { console.error('Restore failed:', b.original, e.message); }
    }
}

function isVisuallyDifferent(diff, opts = {}) {
    const meanThresh = opts.meanThresh ?? 0.5;
    const fracThresh = opts.fracThresh ?? 0.005;
    if (!diff || !diff.sameSize) return false;
    return diff.meanRGB > meanThresh || diff.diffPixelFrac > fracThresh;
}

// Health-check helper: bail out if editor died.
async function healthCheck() {
    try { await pingEditor(); return true; }
    catch (e) { return false; }
}

// =====================================================================
// SC: Screenshot modes
// =====================================================================
async function scenarioScreenshotModes() {
    const name = 'screenshot-modes';
    const subResults = {};
    const modes = [
        { label: 'window', params: { target: 'window' } },
        { label: 'viewport', params: { target: 'viewport' } },
        { label: 'panel-Inspector', params: { target: 'panel', panelId: 'Inspector' } },
        { label: 'panel-Hierarchy', params: { target: 'panel', panelId: 'Hierarchy' } },
        { label: 'panel-Log', params: { target: 'panel', panelId: 'Log' } },
        { label: 'rect-logical', params: { target: 'rect', x: 100, y: 100, w: 400, h: 300, coords: 'logical' } },
        { label: 'rect-physical', params: { target: 'rect', x: 200, y: 200, w: 400, h: 300, coords: 'physical' } },
    ];
    let passes = 0;
    let failures = [];
    for (const m of modes) {
        try {
            const r = await ipc('take_screenshot', m.params, { timeoutMs: 20000 });
            if (r && r.pngBase64 && r.width > 0 && r.height > 0) {
                const buf = Buffer.from(r.pngBase64, 'base64');
                const out = path.join(RESULTS_DIR, `${name}_${m.label}.png`);
                fs.writeFileSync(out, buf);
                subResults[m.label] = { ok: true, savedTo: out, w: r.width, h: r.height };
                passes++;
            } else {
                subResults[m.label] = { ok: false, error: r && r.error ? r.error : 'no pngBase64' };
                failures.push(m.label);
            }
        } catch (e) {
            subResults[m.label] = { ok: false, error: e.message };
            failures.push(m.label);
        }
    }
    // 'element' mode
    try {
        const ui = await ipc('get_ui_tree', { panelId: 'Hierarchy' }, { timeoutMs: 5000 });
        const targetId = findFirstElementId(ui);
        if (targetId) {
            const r = await ipc('take_screenshot', { target: 'element', elementId: targetId }, { timeoutMs: 20000 });
            if (r && r.pngBase64) {
                const buf = Buffer.from(r.pngBase64, 'base64');
                const out = path.join(RESULTS_DIR, `${name}_element-${targetId.replace(/[^A-Za-z0-9._-]/g, '_')}.png`);
                fs.writeFileSync(out, buf);
                subResults['element'] = { ok: true, savedTo: out, elementId: targetId, w: r.width, h: r.height };
                passes++;
            } else {
                subResults['element'] = { ok: false, error: (r && r.error) || 'no pngBase64' };
                failures.push('element');
            }
        } else {
            subResults['element'] = { ok: false, error: 'no element id found' };
        }
    } catch (e) {
        subResults['element'] = { ok: false, error: e.message };
    }
    const status = failures.length === 0 ? 'pass' : (passes >= 6 ? 'pass' : 'fail');
    const notes = `${passes} modes passed; failures=${failures.join(',') || 'none'}`;
    record(name, status, notes, subResults);
}

// Fraction of pixels differing (any channel > 15) within a fractional region
// of two same-size decoded screenshots. For signals that are tiny relative to
// the whole capture (a 16x16 icon in a mostly-empty panel), a whole-image
// fraction drowns the change — crop to where the signal lives.
function regionDiffFrac(a, b, x0f, y0f, x1f, y1f) {
    if (!a || !b || a.width !== b.width || a.height !== b.height) return 0;
    const x0 = Math.floor(a.width * x0f), x1 = Math.floor(a.width * x1f);
    const y0 = Math.floor(a.height * y0f), y1 = Math.floor(a.height * y1f);
    let diff = 0, n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) {
            const o = (y * a.width + x) * a.channels;
            const d = Math.max(Math.abs(a.pixels[o] - b.pixels[o]),
                               Math.abs(a.pixels[o + 1] - b.pixels[o + 1]),
                               Math.abs(a.pixels[o + 2] - b.pixels[o + 2]));
            if (d > 15) diff++;
            n++;
        }
    return n ? diff / n : 0;
}
function findFirstElementId(node) {
    if (!node) return null;
    if (typeof node.id === 'string' && node.id && !node.id.startsWith('_auto_')) return node.id;
    const ch = node.ch || node.children || [];
    for (const c of ch) {
        const r = findFirstElementId(c);
        if (r) return r;
    }
    return null;
}

// =====================================================================
// SC: rapid_select coalescing — verify last selection wins
// =====================================================================
async function scenarioRapidSelect() {
    const name = 'rapid-select-coalesce';
    const hier = await ipc('get_scene_hierarchy', {}, { timeoutMs: 5000 });
    if (!hier || !hier.entities || hier.entities.length < 3) return record(name, 'skip', 'not enough entities');
    const ids = hier.entities.map(e => e.id);
    const lastId = ids[ids.length - 1];

    logStep(name, `rapid-selecting ${ids.length} entities concurrently`);
    await Promise.all(ids.map(id => ipc('select_entity', { entityId: id }, { timeoutMs: 5000 })));
    await sleep(800);

    const state = await ipc('get_editor_state', {}, { timeoutMs: 5000 });
    const sel = state && state.selectedEntity;
    const passed = !!sel && sel.id === lastId;
    const shot = await shootAndDecode(name, 'final', { target: 'panel', panelId: 'Inspector' });
    const expectedName = hier.entities.find(e => e.id === lastId)?.name;
    record(name, passed ? 'pass' : 'fail',
        `selected.id=${sel?.id} expected=${lastId} (${expectedName})`,
        { final: shot.savedTo });
}

// =====================================================================
// SC: set_gizmos_visibility — hiding the overlays must change the viewport
// and restoring them must bring it back. Selecting an entity guarantees a
// transform gizmo is drawn, so there is something to hide. Runs before the
// asset-mutating scenarios and restores the state it found.
// =====================================================================
async function scenarioGizmoVisibility() {
    const name = 'gizmo-visibility';

    const initial = await ipc('set_gizmos_visibility', {}, { timeoutMs: 5000 });
    if (!initial || initial.error) return record(name, 'skip', 'handler unavailable: ' + (initial?.error ?? 'no response'));
    if (typeof initial.all !== 'boolean' || typeof initial.light !== 'boolean' || typeof initial.transform !== 'boolean')
        return record(name, 'fail', 'empty request did not report all/light/transform: ' + JSON.stringify(initial));

    // A non-bool is rejected by name — a bare "type must be boolean" would not
    // say which of the three groups was wrong.
    const badType = await ipc('set_gizmos_visibility', { light: 'yes' }, { timeoutMs: 5000 });
    const namedRejection = !!badType?.error && badType.error.includes("'light'");

    const hier = await ipc('get_scene_hierarchy', {}, { timeoutMs: 5000 });
    if (!hier || !hier.entities || !hier.entities.length) return record(name, 'skip', 'no entities to select');
    await ipc('select_entity', { entityId: hier.entities[hier.entities.length - 1].id }, { timeoutMs: 5000 });
    await sleep(500);

    logStep(name, 'capturing viewport with gizmos shown, hidden, restored');
    await ipc('set_gizmos_visibility', { all: true }, { timeoutMs: 5000 });
    await sleep(500);
    const shown = await shootAndDecode(name, 'shown', { target: 'viewport' });

    const hiddenState = await ipc('set_gizmos_visibility', { all: false }, { timeoutMs: 5000 });
    await sleep(500);
    const hidden = await shootAndDecode(name, 'hidden', { target: 'viewport' });

    await ipc('set_gizmos_visibility', { all: true }, { timeoutMs: 5000 });
    await sleep(500);
    const restored = await shootAndDecode(name, 'restored', { target: 'viewport' });

    // Leave the editor as it was found.
    await ipc('set_gizmos_visibility',
        { all: initial.all, light: initial.light, transform: initial.transform },
        { timeoutMs: 5000 });

    const hideDiff = await diffScreenshots(name, shown, hidden);
    const restoreDiff = await diffScreenshots(name, shown, restored);

    // Gizmos cover a small share of the viewport (~0.1% on the smoke scene), well
    // under the whole-image thresholds the hot-reload scenarios use — those compare
    // full-frame repaints. Judge against the shown-vs-restored pair instead: it is
    // the same state captured twice, so it measures this scene's frame-to-frame
    // noise, and the hide must clear it by an order of magnitude.
    const kMinHideFrac = 1e-4;
    const kControlMargin = 10;
    const hideFrac = hideDiff?.sameSize ? hideDiff.diffPixelFrac : 0;
    const noiseFrac = restoreDiff?.sameSize ? restoreDiff.diffPixelFrac : Infinity;
    const hidChanged = hideFrac > Math.max(kMinHideFrac, noiseFrac * kControlMargin);
    const cameBack = noiseFrac * kControlMargin < hideFrac;
    const echoed = hiddenState?.all === false;

    const passed = namedRejection && hidChanged && cameBack && echoed;
    record(name, passed ? 'pass' : 'fail',
        `namedTypeRejection=${namedRejection} hideChangedViewport=${hidChanged} ` +
        `(frac=${hideFrac.toExponential(2)} mean=${hideDiff?.meanRGB?.toFixed(3)}) ` +
        `restoredControl=${cameBack} (frac=${noiseFrac.toExponential(2)} ` +
        `mean=${restoreDiff?.meanRGB?.toFixed(3)}) echoedState=${echoed}`,
        { shown: shown.savedTo, hidden: hidden.savedTo, restored: restored.savedTo });
}

// =====================================================================
// SC: rendergraph hot-reload — toggle a clearly visible pass.
// We toggle the Sky pass. With Sky disabled the background goes from
// blue gradient to the clear color (typically black), producing a
// massive visual delta. We restore on success.
// =====================================================================
async function scenarioRenderGraphEdit() {
    const name = 'rg-toggle';
    // The ACTIVE pipeline may resolve from the open project (project source
    // wins resolution priority) — editing the staged editor copy would be a
    // silent no-op then. Prefer the project copy when it exists.
    let rgPath = path.join(EDITOR_ASSETS_ROOT, 'RenderPipelines', 'ForwardPlus.rendergraph');
    try {
        const sources = await ipc('get_asset_sources', {}, { timeoutMs: 5000 });
        const proj = (sources.sources || []).find(s => s.alias === 'project');
        if (proj) {
            const projRg = path.join(proj.root, 'RenderPipelines', 'ForwardPlus.rendergraph');
            if (fs.existsSync(projRg)) rgPath = projRg;
        }
    } catch {}
    if (!fs.existsSync(rgPath)) return record(name, 'skip', 'ForwardPlus.rendergraph not found');

    logStep(name, 'baseline screenshot (window)');
    const before = await shootAndDecode(name, 'before', { target: 'window' });

    const orig = fs.readFileSync(rgPath, 'utf8');
    const backup = backupFile(rgPath);
    trackForRestore(backup);
    let modifiedJson;
    try {
        const json = JSON.parse(orig);
        // Disable Sky pass — background becomes constant clear color.
        let touched = 0;
        for (const p of json.passes || []) {
            if (p.id === 'Sky') { p.enabled = false; touched++; }
        }
        if (!touched) return record(name, 'skip', 'no Sky pass found');
        modifiedJson = JSON.stringify(json, null, 2);
    } catch (e) {
        return record(name, 'fail', 'JSON edit failed: ' + e.message);
    }

    logStep(name, 'disabling Sky pass');
    fs.writeFileSync(rgPath, modifiedJson, 'utf8');
    await waitForReload(3500);

    if (!await healthCheck()) {
        return record(name, 'fail', 'editor died during rg hot-reload (Sky disabled). Run was bailed.');
    }

    const after = await shootAndDecode(name, 'after', { target: 'window' });
    fs.writeFileSync(rgPath, orig, 'utf8');
    await waitForReload(2500);

    const diff = await diffScreenshots(name, before, after);
    const passed = isVisuallyDifferent(diff, { meanThresh: 1.0, fracThresh: 0.05 });
    const notes = diff
        ? `meanRGB=${diff.meanRGB.toFixed(3)} maxCh=${diff.maxChannelDiff} fracChanged=${(diff.diffPixelFrac * 100).toFixed(2)}%`
        : 'no diff (size mismatch)';
    record(name, passed ? 'pass' : 'fail', notes, { before: before.savedTo, after: after.savedTo });
}

// =====================================================================
// SC: texture hot-reload — replace a UI icon with a clearly different one.
// Edits BOTH the project source (Debug/...) and editor source (DebugFast/...)
// because the same icon may load from either depending on resolution priority.
// Targets Sphere.png; swaps with Plane.png. Visual delta in Hierarchy proves
// UI texture reload propagated.
// =====================================================================
async function scenarioTextureEdit() {
    const name = 'texture-edit-ui-icon';
    // Swap source: Sphere.png (visible in Hierarchy as the Sphere entity icon).
    // Swap target: Cube.png (visually distinct — solid filled cube glyph).
    // Plane.png used to be the target but it's a near-blank icon; the hot-reload
    // pipeline was technically working but the result looked indistinguishable
    // from "icon disappeared." Cube produces an unambiguous visual delta.
    const sphereAbs = path.join(EDITOR_ASSETS_ROOT, 'Icons', 'Sphere.png');
    const cubeAbs   = path.join(EDITOR_ASSETS_ROOT, 'Icons', 'Cube.png');
    if (!fs.existsSync(sphereAbs) || !fs.existsSync(cubeAbs)) {
        return record(name, 'skip', 'Sphere.png or Cube.png not found in editor Icons/');
    }

    const hier = await ipc('get_scene_hierarchy', {}, { timeoutMs: 5000 });
    const sphereEnt = hier.entities.find(e => e.name === 'Sphere');
    if (sphereEnt) await ipc('select_entity', { entityId: sphereEnt.id }, { timeoutMs: 5000 });
    await sleep(400);

    const before = await shootAndDecode(name, 'before', { target: 'panel', panelId: 'Hierarchy' });

    const backups = [];
    backups.push(backupFile(sphereAbs));
    for (const b of backups) trackForRestore(b);

    fs.copyFileSync(cubeAbs, sphereAbs);
    logStep(name, 'replaced Icons/Sphere.png with Cube.png bytes');
    await waitForReload(3500);

    if (!await healthCheck()) {
        return record(name, 'fail', 'editor died during texture hot-reload. Bailed.');
    }

    const after = await shootAndDecode(name, 'after', { target: 'panel', panelId: 'Hierarchy' });
    for (const b of backups) restoreBackup(b);
    await waitForReload(2000);

    const diff = await diffScreenshots(name, before, after);
    // The signal is two ~16x16 row icons in a mostly-empty panel (~0.06% of
    // all pixels) — measure only the entity-rows band where the icons live.
    const rowsFrac = regionDiffFrac(before.decoded, after.decoded, 0, 0, 0.4, 0.3);
    const passed = rowsFrac > 0.003;
    const notes = diff
        ? `rowsRegionFrac=${(rowsFrac * 100).toFixed(2)}% (whole-panel frac=${(diff.diffPixelFrac * 100).toFixed(2)}%)`
        : 'no diff';
    record(name, passed ? 'pass' : 'fail', notes, { before: before.savedTo, after: after.savedTo });
}

// =====================================================================
// SC: SearchDialog wheel-scroll virt regression check (1d7a0151).
// Open Add Component dialog (clicks 'inspector-add-component-button' by
// coords from panel tree), drive set_scroll across multiple offsets,
// take a screenshot at each, verify each shot has visual delta vs
// offset 0. A broken virt would render compressed/overlapping rows
// where multiple offsets look near-identical.
// =====================================================================
async function scenarioSearchDialogScroll() {
    const name = 'search-dialog-scroll';
    // Make sure an entity is selected so Inspector shows the Add Component button.
    const hier = await ipc('get_scene_hierarchy', {}, { timeoutMs: 5000 });
    const ent = hier.entities.find(e => e.name === 'Sphere') || hier.entities[0];
    await ipc('select_entity', { entityId: ent.id }, { timeoutMs: 5000 });
    await sleep(400);

    // Find Add Component button via panel tree.
    const tree = await ipc('get_panel_tree', { panelId: 'Inspector' }, { timeoutMs: 5000 });
    let addBtn = null;
    (function walk(n) {
        if (addBtn) return;
        if (typeof n.cls === 'string' && n.cls.includes('inspector-add-component-button')) addBtn = n;
        (n.ch || []).forEach(walk);
    })(tree);
    if (!addBtn) return record(name, 'skip', 'Add Component button not found in Inspector');

    const cx = addBtn.x + addBtn.w / 2;
    const cy = addBtn.y + addBtn.h / 2;
    logStep(name, `clicking Add Component at (${cx}, ${cy})`);
    await ipc('click_element', { x: cx, y: cy }, { timeoutMs: 10000 });
    await sleep(700); // allow dialog open animation / layout

    // Verify the SearchDialog opened: search by 'search-dialog-results' class.
    const ui = await ipc('get_ui_tree', {}, { timeoutMs: 5000 });
    let dialogResults = null;
    (function walk(n) {
        if (dialogResults) return;
        if (typeof n.cls === 'string' && n.cls.split(/\s+/).includes('search-dialog-results')) {
            dialogResults = n;
        }
        (n.ch || []).forEach(walk);
    })(ui);

    if (!dialogResults) {
        record(name, 'skip', 'SearchDialog not detected in UI tree after click (search-dialog-results class missing)');
        return;
    }

    logStep(name, `SearchDialog open at (${dialogResults.x},${dialogResults.y}) ${dialogResults.w}x${dialogResults.h}`);

    // Drive scroll via the results list's stable id.
    // Crop screenshots tightly to the results panel so we measure delta in the
    // scrolled list, not random screen noise (e.g. mouse cursor hover effects).
    const offsets = [0, 60, 150, 300, 600];
    const shots = [];
    for (const y of offsets) {
        try {
            const r = await ipc('set_scroll', { elementId: 'search-dialog-results', y }, { timeoutMs: 5000 });
            if (r && r.error) logStep(name, `set_scroll y=${y} returned: ${r.error}`);
        } catch (e) {
            logStep(name, `set_scroll y=${y} failed: ${e.message}`);
        }
        await sleep(300);
        const shot = await shootAndDecode(name, 'sd_y' + y, {
            target: 'rect',
            rect: { x: dialogResults.x, y: dialogResults.y, w: dialogResults.w, h: dialogResults.h },
            coords: 'logical',
        });
        shots.push({ y, shot });
    }

    // Close the dialog by pressing Escape — click on backdrop also works but is fragile.
    // The simplest path is clicking on the backdrop element if present.
    try {
        const backdropTree = await ipc('get_ui_tree', {}, { timeoutMs: 5000 });
        let backdrop = null;
        (function walk(n) {
            if (backdrop) return;
            if (typeof n.cls === 'string' && n.cls.includes('search-dialog-backdrop')) backdrop = n;
            (n.ch || []).forEach(walk);
        })(backdropTree);
        if (backdrop) {
            await ipc('click_element', { x: backdrop.x + 10, y: backdrop.y + 10 }, { timeoutMs: 5000 });
        }
    } catch {}
    await sleep(300);

    let ok = 0, total = 0;
    for (let i = 1; i < shots.length; i++) {
        const d = await diffScreenshots(name, shots[0].shot, shots[i].shot);
        total++;
        if (d && (d.meanRGB > 0.5 || d.diffPixelFrac > 0.005)) ok++;
    }
    const passed = ok === total && total > 0;
    record(name, passed ? 'pass' : (ok > 0 ? 'fail' : 'skip'),
        `${ok}/${total} scroll offsets produced visual delta from offset 0`,
        Object.fromEntries(shots.map(s => ['y' + s.y, s.shot.savedTo])));
}

// =====================================================================
// Main

// =====================================================================
// SC: scene-material texture lifecycle — generates two solid-color test
// textures + two materials in the ACTIVE PROJECT (asked from the editor,
// not assumed), assigns them to the Plane via set_component, and walks the
// full texture path the renderer owns (TextureService):
//   assign   -> material registration (thumbnail path) + BindMaterialTextureRef
//   swap     -> texture-file hot-reload (Evict -> repair-to-default -> rebind)
//   restore  -> the same, back to the original bytes
//   switch   -> second material registration + extraction re-resolve
// Asserts channel dominance of the plane region, not just "something changed".
// Known gap (pre-existing, deliberately NOT tested): editing a .material ON
// DISK only invalidates pipelines (RenderingHotReloadBridge) — texture
// bindings are re-read only through in-editor edit paths.
// =====================================================================
async function scenarioSceneMaterialTexture() {
    const name = 'texture-scene-material';
    const dominant = (c, ch) => {
        const others = ['r', 'g', 'b'].filter(k => k !== ch);
        return c[ch] > 120 && others.every(k => c[ch] > 2.0 * c[k]);
    };
    const fmt = c => `(${c.r.toFixed(0)},${c.g.toFixed(0)},${c.b.toFixed(0)})`;

    // Test assets live in the ACTIVE project (source of truth: the editor).
    const sources = await ipc('get_asset_sources', {}, { timeoutMs: 5000 });
    const proj = (sources.sources || []).find(s => s.alias === 'project');
    if (!proj) return record(name, 'skip', 'no project asset source');
    const dir = path.join(proj.root, 'IntegrationTests');
    fs.mkdirSync(dir, { recursive: true });

    const redPath = path.join(dir, 'IT_Red.png');
    const greenPath = path.join(dir, 'IT_Green.png');
    fs.writeFileSync(redPath, encodeSolidPNG(64, 64, [255, 0, 0]));
    fs.writeFileSync(greenPath, encodeSolidPNG(64, 64, [0, 255, 0]));

    // Registry indexes new files via the file watcher; poll for derived GUIDs.
    const findGuid = (list, suffix) =>
        (list.assets || []).find(a => a.path.endsWith(suffix))?.guid;
    let redGuid, greenGuid;
    for (let i = 0; i < 20 && (!redGuid || !greenGuid); i++) {
        await sleep(1000);
        const list = await ipc('get_asset_list', {}, { timeoutMs: 10000 });
        redGuid = redGuid || findGuid(list, 'integrationtests/it_red.png');
        greenGuid = greenGuid || findGuid(list, 'integrationtests/it_green.png');
    }
    if (!redGuid || !greenGuid) return record(name, 'skip', 'test textures never indexed');

    const makeMat = (mname, albedoGuid) => JSON.stringify({
        schemaVersion: 3, materialName: mname, lightingModel: 'StandardPBR',
        alphaMode: 'Opaque', doubleSided: false, textureFilter: 'Trilinear',
        surfaceShader: 'Surfaces/standard_pbr.glsl',
        properties: { baseColor: [1, 1, 1, 1], roughness: 0.9, metallic: 0.0,
                      emissive: [0, 0, 0], emissiveIntensity: 1.0, ao: 1.0, opacity: 1.0 },
        textures: { albedoMap: albedoGuid },
    }, null, 2);
    fs.writeFileSync(path.join(dir, 'IT_TexA.material'), makeMat('IT TexA', redGuid));
    fs.writeFileSync(path.join(dir, 'IT_TexB.material'), makeMat('IT TexB', greenGuid));
    let matA, matB;
    for (let i = 0; i < 20 && (!matA || !matB); i++) {
        await sleep(1000);
        const list = await ipc('get_asset_list', {}, { timeoutMs: 10000 });
        matA = matA || findGuid(list, 'integrationtests/it_texa.material');
        matB = matB || findGuid(list, 'integrationtests/it_texb.material');
    }
    if (!matA || !matB) return record(name, 'skip', 'test materials never indexed');

    const cleanup = () => { try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} };
    try {
        // Focus first: an unfocused editor throttles presentation, which stalls
        // thumbnail renders (registration) and screenshot readbacks alike. And
        // force 3D — a 2D scene view ignores the camera pose set below.
        ensureEditorFocused();
        // Let a freshly-booted editor finish its startup asset indexing before
        // scenarios start editing files: hot-reload events raced against the
        // startup queue get debounced/coalesced and the visual deltas vanish.
        await sleep(3000);
        await ipc('set_scene_view_mode', { is2D: false }, { timeoutMs: 5000 }).catch(() => {});
        await sleep(500);
        // Register both materials with the renderer via the asset-browser
        // selection -> MaterialThumb render (also exercises the thumbnail path).
        await ipc('select_asset', { path: 'IntegrationTests/IT_TexA.material' }, { timeoutMs: 10000 });
        await sleep(2500);
        await ipc('select_asset', { path: 'IntegrationTests/IT_TexB.material' }, { timeoutMs: 10000 });
        await sleep(2500);

        // Point the Plane at material A and frame an empty stretch of it.
        const hier = await ipc('get_scene_hierarchy', {}, { timeoutMs: 5000 });
        const plane = hier.entities.find(e => e.name === 'Plane');
        if (!plane) { cleanup(); return record(name, 'skip', 'no Plane entity in scene'); }
        await ipc('set_component', {
            entityId: plane.id, component: 'MeshRenderer', values: { materialAssetGuid: matA },
        }, { timeoutMs: 5000 });
        // pitch -10 keeps the real horizon ~20% from the frame top: the sky band
        // (2-12%) samples actual sky in ANY scene — at steeper pitches the whole
        // frame drops below eye level and an infinite ocean/floor fills the
        // 'sky' band, tripping the negative control.
        await ipc('set_camera', { position: [30, 6, 0], yawDeg: 0, pitchDeg: -10 }, { timeoutMs: 5000 });
        ensureEditorFocused();
        await sleep(3000);

        const sample = async (label) => {
            for (let attempt = 0; attempt < 3; attempt++) {
                try {
                    // Full-window context shot first: proves the viewport capture is
                    // the Scene View (chrome + panels visible), not some other panel.
                    await shootAndDecode(name, label + '-window', { target: 'window' }).catch(() => {});
                    const shot = await shootAndDecode(name, label, { target: 'viewport' });
                    if (shot.decoded) return { shot, avg: avgRegion(shot.decoded, 0.375, 0.55, 0.625, 0.8),
                                               sky: avgRegion(shot.decoded, 0.375, 0.02, 0.625, 0.12) };
                } catch (e) { logStep(name, `screenshot ${label} attempt ${attempt}: ${e.message.slice(0, 90)}`); }
                ensureEditorFocused();
                await sleep(2000);
            }
            throw new Error('screenshot failed 3x for ' + label);
        };

        const steps = [];
        const shots = {};
        const check = async (label, ch, mutate) => {
            if (mutate) await mutate();
            // Registration + first-variant shaderc compile can take a few
            // seconds; poll until the expected channel dominates or times out.
            let shot, avg, sky, ok = false;
            for (let tries = 0; tries < 6 && !ok; tries++) {
                if (tries) await sleep(2500);
                ({ shot, avg, sky } = await sample(label));
                ok = dominant(avg, ch);
            }
            // Negative control: the sky band above the horizon must NOT follow
            // the expected plane channel — proves the sampled region really is
            // the plane, not a full-frame tint or a different panel.
            const skyClean = !dominant(sky, ch);
            ok = ok && skyClean;
            shots[label] = shot.savedTo;
            steps.push(`${label}=${fmt(avg)} sky=${fmt(sky)} expect ${ch.toUpperCase()} ${ok ? 'OK' : 'MISS'}`);
            return ok;
        };

        const okAssign = await check('assign-red', 'r', null);
        const okSwap = await check('hotreload-blue', 'b', async () => {
            fs.writeFileSync(redPath, encodeSolidPNG(64, 64, [0, 80, 255]));
            await waitForReload(5000);
        });
        const okRestore = await check('restore-red', 'r', async () => {
            fs.writeFileSync(redPath, encodeSolidPNG(64, 64, [255, 0, 0]));
            await waitForReload(5000);
        });
        const okSwitch = await check('switch-green', 'g', async () => {
            await ipc('set_component', {
                entityId: plane.id, component: 'MeshRenderer', values: { materialAssetGuid: matB },
            }, { timeoutMs: 5000 });
            await waitForReload(4000);
        });
        // Edit the ACTIVE material document ON DISK (external-tool / git-pull
        // path): the Material reload invalidator must re-register the fresh
        // document — texture binding included, not just pipeline caches.
        const okDocEdit = await check('material-disk-edit-red', 'r', async () => {
            fs.writeFileSync(path.join(dir, 'IT_TexB.material'), makeMat('IT TexB', redGuid));
            await waitForReload(5000);
        });

        cleanup();
        const passed = okAssign && okSwap && okRestore && okSwitch && okDocEdit;
        record(name, passed ? 'pass' : 'fail', steps.join(' | '), shots);
    } catch (e) {
        cleanup();
        record(name, 'fail', 'exception: ' + e.message);
    }
}

// =====================================================================
async function main() {
    ensureDirs();
    console.log('=== Editor Integration Test Harness ===');
    console.log(`Results dir: ${RESULTS_DIR}`);
    console.log(`Asset root:  ${EDITOR_ASSETS_ROOT}`);

    try {
        const s = await pingEditor();
        console.log(`Editor up: ${s.entityCount} entities, ${s.windowCount} window(s).\n`);
        // Raise the editor once up front: the scenarios below read rendered
        // frames, and a minimized or hidden window produces none — take_screenshot
        // then fails naming that rather than returning a picture of the window.
        ensureEditorFocused();
    } catch (e) {
        console.error(`Editor not responding on 127.0.0.1:${IPC_PORT}. Launch the editor first.`);
        console.error('Error:', e.message);
        process.exit(2);
    }

    // Order: passive checks first (screenshot modes, rapid-select), then asset-mutating ones.
    const scenarios = [
        ['screenshot-modes', scenarioScreenshotModes],
        ['rapid-select', scenarioRapidSelect],
        ['gizmo-visibility', scenarioGizmoVisibility],
        ['rg-toggle', scenarioRenderGraphEdit],
        ['texture', scenarioTextureEdit],
        ['texture-scene', scenarioSceneMaterialTexture],
        ['search-dialog-scroll', scenarioSearchDialogScroll],
    ];

    for (const [tag, fn] of scenarios) {
        if (!shouldRun(tag)) continue;
        console.log(`\n--- ${tag} ---`);
        if (!await healthCheck()) {
            record(tag, 'skip', 'editor died before scenario started');
            continue;
        }
        try {
            await fn();
        } catch (e) {
            record(tag, 'fail', 'unhandled exception: ' + e.message);
            console.error(e);
        }
    }

    await restoreAll();
    await waitForReload(1000);

    const passes = results.filter(r => r.status === 'pass').length;
    const fails = results.filter(r => r.status === 'fail').length;
    const skips = results.filter(r => r.status === 'skip').length;
    console.log(`\n=== Summary: ${passes} pass, ${fails} fail, ${skips} skip ===`);

    const reportJson = path.join(RESULTS_DIR, 'report.json');
    fs.writeFileSync(reportJson, JSON.stringify({ results, summary: { passes, fails, skips } }, null, 2));
    const reportMd = path.join(RESULTS_DIR, 'report.md');
    let md = `# Editor Integration Test Report\n\n`;
    md += `Generated: ${new Date().toISOString()}\n`;
    // EDITOR_ASSETS_ROOT is <bin>/<Config>/Apps/Editor/Assets; the binary is its sibling.
    const editorBinary = path.join(path.dirname(EDITOR_ASSETS_ROOT), 'Editor.exe');
    md += `Editor binary: \`${editorBinary}\`\n\n`;
    md += `## Summary\n\n${passes} pass, ${fails} fail, ${skips} skip\n\n`;
    md += `## Scenarios\n\n| Scenario | Status | Notes |\n|---|---|---|\n`;
    for (const r of results) md += `| ${r.name} | ${r.status} | ${r.notes.replace(/\|/g, '\\|')} |\n`;
    md += `\n## Screenshots\n\n`;
    for (const r of results) {
        if (!r.files || !Object.keys(r.files).length) continue;
        md += `### ${r.name}\n\n`;
        for (const [k, v] of Object.entries(r.files)) {
            const p = (typeof v === 'string') ? v : (v && v.savedTo);
            if (p) md += `- ${k}: \`${p}\`\n`;
        }
        md += '\n';
    }
    fs.writeFileSync(reportMd, md);
    console.log(`Wrote ${reportJson}\nWrote ${reportMd}`);

    process.exit(fails > 0 ? 1 : 0);
}

main().catch(async (e) => {
    console.error('Fatal:', e);
    await restoreAll();
    process.exit(2);
});
