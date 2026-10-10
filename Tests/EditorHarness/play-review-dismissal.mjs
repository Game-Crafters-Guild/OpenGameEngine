#!/usr/bin/env node
// Run against an isolated Editor with a disposable project, initially in Edit:
//   GE_EDITOR_DEBUG_PORT=<port> node Tests/EditorHarness/play-review-dismissal.mjs
// Creates/deletes temporary entities through the real Play snapshot and Undo paths.
// Asserts the rendered overlay and subsequent pointer input, not just playMode:
// get_editor_state.modal does not currently include the Play review overlay.
import assert from 'node:assert/strict';
import { ipc, sleep, PORT } from './lib/ipc.mjs';

async function call(method, params = {}) {
    const result = await ipc(method, params);
    assert.ok(!result?.error, `${method}: ${JSON.stringify(result)}`);
    return result;
}

async function until(label, read, accept) {
    const deadline = Date.now() + 10000;
    do {
        const value = await read();
        if (accept(value)) return value;
        await sleep(50);
    } while (Date.now() < deadline);
    throw new Error(`Timed out: ${label}`);
}

function find(node, accept) {
    if (accept(node)) return node;
    for (const child of node.ch ?? []) {
        const found = find(child, accept);
        if (found) return found;
    }
    return null;
}

const hasClass = (node, cls) => String(node.cls ?? '').split(' ').includes(cls);
async function visibleTree() {
    const tree = await call('get_ui_tree', { maxDepth: 60, includeHidden: false, includeLayout: true });
    // Hidden nodes are intentionally omitted: this is a visibility assertion.
    // Depth truncation, however, would make a missing overlay inconclusive.
    assert.equal(tree.truncatedAtDepth, undefined, 'visible UI tree must not be depth-truncated');
    return tree.root ?? tree;
}
async function reviewOverlay(visible) {
    return until(`review overlay ${visible ? 'visible' : 'dismissed'}`, visibleTree,
        tree => Boolean(find(tree, node => hasClass(node, 'playmode-change-review-modal'))) === visible);
}
async function state(expected) {
    return until(`playMode ${expected}`, () => call('get_editor_state'), s => s.playMode === expected);
}
async function transition(action, expected) {
    await call('set_play_mode', { action, ...(action === 'enter' ? { activateGameView: true } : {}) });
    await state(expected);
}

async function clickReviewButton(label) {
    const tree = await visibleTree();
    const overlay = find(tree, node => hasClass(node, 'playmode-change-review-modal'));
    assert.ok(overlay, 'review overlay is visible before clicking its button');
    const button = find(overlay, node => hasClass(node, 'button') && find(node, n => n.text === label));
    assert.ok(button?.id && button.w > 0 && button.h > 0, `${label} button has a pointer target`);
    await call('click_element', { elementId: button.id });
}

async function sceneEntity(name) {
    const { entities } = await call('get_scene_hierarchy');
    return entities.find(entity => entity.name === name);
}

async function runCase(label, dismiss) {
    const name = `PlayReviewRegression_${label}_${Date.now()}`;
    await call('create_entity', { name });
    await transition('enter', 'playing');
    const deletion = await call('delete_entity', { entityId: (await sceneEntity(name)).id });
    assert.equal(deletion.undoTracked, true, 'use an actual tracked Editor command');
    await transition('exit', 'change_review');
    await reviewOverlay(true);
    await dismiss();
    await state('editing');
    await reviewOverlay(false);

    await transition('enter', 'playing');
    await reviewOverlay(false);
    // Pointer injection goes through hit testing. A stale blocking backdrop eats
    // these clicks even when set_play_mode itself reports the desired state.
    await call('click_element', { elementId: 'TopToolbarCenter3' });
    await state('paused');
    await call('click_element', { elementId: 'TopToolbarCenter3' });
    await state('playing');
    await transition('exit', 'editing');
    await reviewOverlay(false);
    // Scene deserialization may remap handles; resolve the surviving entity by
    // its unique fixture name instead of reusing its pre-Play runtime handle.
    const restored = await sceneEntity(name);
    if (label !== 'apply') assert.ok(restored, 'Discard restores the temporary entity');
    if (restored) await call('delete_entity', { entityId: restored.id });
    console.log(`PASS ${label}: overlay dismissed; subsequent Play pointer input works`);
}

try {
    assert.equal((await call('get_editor_state')).playMode, 'editing', 'start in Edit on a disposable project');
    console.log(`Play review regression on port ${PORT}`);
    await runCase('ipc-discard', () => transition('exit', 'editing'));
    await runCase('discard-button', () => clickReviewButton('Discard'));
    await runCase('apply', () => clickReviewButton('Apply Selected'));
    console.log('PASS all 3 Play review dismissal paths');
} catch (error) {
    // Leave the failed state available for inspection instead of hiding evidence.
    console.error(error.stack ?? error);
    process.exitCode = 1;
}
