#!/usr/bin/env node
// Run against an isolated Editor with a disposable project, a scene open, in Edit:
//   GE_EDITOR_DEBUG_PORT=<port> node Tests/EditorHarness/inspector-new-stack.mjs
// New Stack on an emitter that runs the default stack writes a stack asset, assigns it, and the
// emitter's inspector rebuilds from a section's refresh callback once the asset loads. That rebuild
// used to run inside the loop that calls those callbacks and freed the one running. Asserts the
// editor still answers, the emitter runs the new stack and the inspector shows its rows; then undoes
// the creation and deletes the temporary entity. Drives the editor only through its debug port.
import assert from 'node:assert/strict';
import { ipc, sleep, PORT } from './lib/ipc.mjs';

async function call(method, params = {}) {
    const result = await ipc(method, params);
    assert.ok(!result?.error, `${method}: ${JSON.stringify(result)}`);
    return result;
}

async function until(label, read, accept, timeoutMs = 15000) {
    const deadline = Date.now() + timeoutMs;
    do {
        const value = await read();
        if (accept(value)) return value;
        await sleep(100);
    } while (Date.now() < deadline);
    throw new Error(`Timed out: ${label}`);
}

function find(node, accept) {
    if (!node) return null;
    if (accept(node)) return node;
    for (const child of node.ch ?? []) {
        const found = find(child, accept);
        if (found) return found;
    }
    return null;
}

async function inspectorTree() {
    return call('get_panel_tree', { panelId: 'Inspector', maxDepth: 40, maxTextLength: 0 });
}

// Scrolls the inspector so the node `accept` names sits near the top of the panel, and returns it at its
// new place: a click outside the window hits nothing.
async function reveal(accept) {
    await call('set_scroll', { elementId: 'Inspector_1', y: 0 });
    await sleep(300);
    const top = await inspectorTree();
    const node = find(top, accept);
    if (!node) return null;
    await call('set_scroll', { elementId: 'Inspector_1', y: Math.max(0, node.y - (top.y ?? 0) - 150) });
    await sleep(400);
    return find(await inspectorTree(), accept);
}

// The emitter's stack GUID, or undefined while it names none (the default stack).
function stackOf(components) {
    const stack = components?.components?.ParticleEmitter3D?.Stack;
    const text = typeof stack === 'string' ? stack : JSON.stringify(stack ?? '');
    return /[1-9a-f]/i.test(text.replace(/[^0-9a-f]/gi, '')) ? stack : undefined;
}

async function main() {
    await call('get_editor_state');
    const created = await call('create_entity', {
        name: 'New Stack Probe', position: { x: 0, y: 0.5, z: 0 }, components: { ParticleEmitter3D: {} },
    });
    const entityId = created.entityId ?? created.id;
    assert.ok(entityId !== undefined, `create_entity returned an id: ${JSON.stringify(created)}`);
    try {
        await call('select_entity', { entityId });
        const button = await until('the New Stack button', () => reveal(n => n.id === 'particle-emitter-new-stack'),
            Boolean);
        assert.ok(button.w > 0 && button.h > 0, 'New Stack has a pointer target');
        await call('click_element', { x: button.x + button.w / 2, y: button.y + button.h / 2 });

        // The stack loads a frame or more later; the refresh callback then asks for the rebuild.
        const components = await until('the emitter runs the new stack',
            () => call('get_entity_components', { entityId }), c => Boolean(stackOf(c)));
        await sleep(2000);
        const state = await call('get_editor_state');
        assert.ok(state, 'the editor still answers after the inspector rebuilt');
        const rows = await until('the new stack in the inspector', inspectorTree,
            tree => Boolean(find(tree, n => (n.text ?? n.t) === 'Add Phase')));
        assert.ok(rows, 'the inspector shows the new stack\'s rows');
        console.log(`PASS inspector-new-stack: emitter ${entityId} runs ${JSON.stringify(stackOf(components))}; ` +
                    `editor answering on port ${PORT}`);
        await call('undo');
    } finally {
        await ipc('delete_entity', { entityId });
    }
}

main().catch((error) => {
    console.error(`FAIL inspector-new-stack: ${error.message}`);
    process.exit(1);
});
