// The call contract (design 3.1) against the stubbed ABI: no export runs while ge_create is
// suspended or after dispose; a model load does not suspend, so frames keep running while
// the facade polls it; loads run one at a time; a load requested inside a frame starts after
// that frame's tick.

import assert from 'node:assert/strict';
import test from 'node:test';
import { Bridge } from '../dist/src/bridge.js';
import { kInvalidEntity } from '../dist/src/abi.js';
import { createEngine } from '../dist/src/engine.js';
import { deferred, StubAbi } from '../dist/test/stub-abi.js';
import { FakeHost, fakeCanvas, settle, startEngine, within } from '../dist/test/harness.js';

const count = (abi, call) => abi.calls.filter((c) => c === call).length;
const loads = (abi) => abi.calls.filter((c) => c.startsWith('ge_load_asset'));

test('the contract guard refuses a field write while ge_create is suspended, and the call never reaches the module', async () => {
    const abi = new StubAbi();
    abi.createGate = deferred();
    const bridge = new Bridge(abi);
    const creating = bridge.contract.suspend('ge_create', () => abi.ge_create(bridge.writeString('#c'), 0));
    assert.throws(() => bridge.abi.ge_field_set(1, 0x1000n, 0, 16),
        { code: 'CallOrder', message: /ge_field_set was called while ge_create is suspended: no engine call may run until Engine.create resolves/ });
    assert.equal(abi.calls.filter((c) => c.startsWith('ge_field_set')).length, 0);
    abi.createGate.resolve(0);
    assert.equal(await creating, 0);
    assert.equal(bridge.abi.ge_entity_create() !== kInvalidEntity, true);
});

test('Engine.create makes no other engine call until ge_create resolves', async () => {
    const abi = new StubAbi();
    abi.createGate = deferred();
    const creating = createEngine({ canvas: fakeCanvas() }, new FakeHost(), async () => abi, { version: '0.1.0', fingerprint: null });
    await settle();
    assert.equal(abi.calls.length, 1);
    assert.match(abi.calls[0], /^ge_create\(#opengine-canvas-\d+, 0\)$/);
    abi.createGate.resolve(0);
    await creating;
    assert.ok(abi.calls.length > 1);
});

test('frames keep running while a model loads, and the load resolves on the frame its status turns Ready', async () => {
    const { abi, host, engine } = await startEngine();
    abi.holdLoads = true;
    engine.run();
    let model = null;
    engine.scene.load('robot.glb').then((m) => { model = m; });
    await settle();
    const ticks = count(abi, 'ge_tick');
    host.fireFrames(16);
    host.fireFrames(32);
    await settle();
    assert.equal(count(abi, 'ge_tick'), ticks + 2);
    assert.equal(model, null);
    engine.scene.create('placed during the load').transform.position.set(1, 2, 3);
    abi.finishLoad(1, true);
    host.fireFrames(48);
    await settle();
    assert.ok(model?.bounds);
    assert.equal(count(abi, 'ge_update_assets'), 0);
});

test('a load awaited before engine.run() completes by polling on its own frames', async () => {
    const { abi, host, engine } = await startEngine();
    abi.holdLoads = true;
    const loading = engine.scene.load('robot.glb');
    await settle();
    host.fireFrames(16);
    abi.finishLoad(1, true);
    host.fireFrames(32);
    const model = await within(loading, 'the load awaited before engine.run()');
    assert.ok(model.bounds);
    assert.equal(count(abi, 'ge_tick'), 0);
    assert.ok(count(abi, 'ge_update_assets') >= 1);
});

test('a load still completes after engine.pause() mid-load, by polling on its own frames', async () => {
    const { abi, host, engine } = await startEngine();
    abi.holdLoads = true;
    engine.run();
    const loading = engine.scene.load('robot.glb');
    await settle();
    host.fireFrames(16);
    engine.pause();
    abi.finishLoad(1, true);
    host.fireFrames(32);
    const model = await within(loading, 'the load paused mid-way');
    assert.ok(model.bounds);
});

test('a failed load rejects with the engine\'s message, and the next load still runs', async () => {
    const { abi, host, engine } = await startEngine();
    abi.holdLoads = true;
    const failing = engine.scene.load('missing.glb');
    const next = engine.scene.load('robot.glb');
    await settle();
    abi.finishLoad(1, false);
    host.fireFrames(16);
    await assert.rejects(within(failing, 'the failing load'), { code: 'Engine', message: /scene.load\('https:\/\/page.test\/demo\/missing.glb'\) failed: asset 1 failed to load/ });
    await settle();
    abi.finishLoad(2, true);
    host.fireFrames(32);
    assert.ok((await within(next, 'the load after a failed one')).bounds);
});

test("emissiveStrength reaches the load's materials after each instance is made; an invalid one rejects before any fetch", async () => {
    const { abi, host, engine } = await startEngine();
    engine.run();
    const strengths = () => abi.calls.filter((c) => c.startsWith('ge_model_emissive_strength'));
    await assert.rejects(within(engine.scene.load('lamp.glb', { emissiveStrength: -1 }), 'the invalid strength'), { code: 'InvalidArgument', message: /emissiveStrength takes a finite number of 0 or more/ });
    assert.deepEqual(loads(abi), []);
    const lit = engine.scene.load('lamp.glb', { emissiveStrength: 4 });
    for (let frame = 1; frame < 4; ++frame) { await settle(); host.fireFrames(frame * 16); }
    await within(lit, 'the load with an emissive strength');
    assert.deepEqual(strengths(abi), ['ge_model_emissive_strength(1, 4)']);
    assert.equal(abi.calls.indexOf('ge_instantiate_model') < abi.calls.indexOf('ge_model_emissive_strength(1, 4)'), true);
    const plain = engine.scene.load('lamp.glb');
    for (let frame = 4; frame < 8; ++frame) { await settle(); host.fireFrames(frame * 16); }
    await within(plain, 'the load without one');
    assert.deepEqual(strengths(abi), ['ge_model_emissive_strength(1, 4)']);
});

test('a load whose emissive strength the engine refuses rejects with its message and leaves no model behind', async () => {
    const { abi, host, engine } = await startEngine();
    engine.run();
    abi.refuseEmissiveStrength = true;
    const loading = engine.scene.load('lamp.glb', { emissiveStrength: 4 });
    for (let frame = 1; frame < 4; ++frame) { await settle(); host.fireFrames(frame * 16); }
    await assert.rejects(within(loading, 'the refused strength'), { code: 'Engine', message: /ge_model_emissive_strength needs the renderer/ });
    assert.equal(abi.calls.filter((c) => c === 'ge_instantiate_model').length, 1);
    assert.equal(abi.calls.filter((c) => c === 'ge_entity_destroy').length, 1);
});

test('scene.load called while another load is in flight queues behind it', async () => {
    const { abi, host, engine } = await startEngine();
    abi.holdLoads = true;
    const a = engine.scene.load('a.glb');
    await settle();
    assert.deepEqual(loads(abi), ['ge_load_asset(https://page.test/demo/a.glb)']);
    const b = engine.scene.load('b.glb');
    await settle();
    assert.deepEqual(loads(abi), ['ge_load_asset(https://page.test/demo/a.glb)']);
    abi.finishLoad(1, true);
    host.fireFrames(16);
    await within(a, 'the first queued load');
    await settle();
    assert.deepEqual(loads(abi), ['ge_load_asset(https://page.test/demo/a.glb)', 'ge_load_asset(https://page.test/demo/b.glb)']);
    abi.finishLoad(2, true);
    host.fireFrames(32);
    await within(b, 'the second queued load');
});

test('scene.load from an onFrame callback starts after that frame\'s tick', async () => {
    const { abi, host, engine } = await startEngine();
    let loading = null;
    const stop = engine.onFrame(() => {
        loading ??= engine.scene.load('late.glb');
    });
    engine.run();
    host.fireFrames(16);
    stop();
    await settle();
    host.fireFrames(32);
    const model = await within(loading, 'the load from onFrame');
    const tick = abi.calls.indexOf('ge_tick');
    const load = abi.calls.indexOf('ge_load_asset(https://page.test/demo/late.glb)');
    assert.ok(tick >= 0 && load > tick, `expected the load after the tick: ${abi.calls.join(', ')}`);
    assert.ok(model.bounds);
});

test('engine.run and engine.dispose work during a load; the load then rejects as Disposed', async () => {
    const { abi, engine } = await startEngine();
    abi.holdLoads = true;
    const loading = engine.scene.load('robot.glb');
    await settle();
    engine.run();
    engine.dispose();
    assert.ok(abi.calls.includes('ge_shutdown'));
    await assert.rejects(within(loading, 'the load disposed mid-way'), { code: 'Disposed' });
    await assert.rejects(within(engine.scene.load('later.glb'), 'a load after dispose'), { code: 'Disposed' });
});

test('an onFrame callback that disposes or pauses the engine ends the frame before the tick', async () => {
    const { abi, host, engine } = await startEngine();
    engine.onFrame(() => engine.pause());
    engine.run();
    host.fireFrames(16);
    assert.equal(count(abi, 'ge_tick'), 0);
    assert.equal(host.frames.size, 0);
    const second = await startEngine();
    second.engine.onFrame(() => second.engine.dispose());
    second.engine.run();
    assert.doesNotThrow(() => second.host.fireFrames(16));
    assert.equal(count(second.abi, 'ge_tick'), 0);
});

test('a throwing onFrame callback is reported, and the other callbacks and the tick still run', async () => {
    const { abi, host, engine } = await startEngine();
    const ran = [];
    engine.onFrame(() => { throw new Error('page bug'); });
    engine.onFrame(() => ran.push('second'));
    engine.run();
    host.fireFrames(16);
    assert.deepEqual(ran, ['second']);
    assert.equal(count(abi, 'ge_tick'), 1);
    assert.equal(host.errors.length, 1);
    assert.match(host.errors[0].message, /page bug/);
});

test('engine.dispose resolves only when ge_shutdown does, and nothing reaches the module meanwhile', async () => {
    const { abi, engine } = await startEngine();
    abi.shutdownGate = deferred();
    let disposed = false;
    const disposing = engine.dispose().then(() => { disposed = true; });
    await settle();
    assert.equal(disposed, false);
    const before = abi.calls.length;
    assert.throws(() => engine.scene.create('during shutdown'), { code: 'Disposed' });
    assert.equal(abi.calls.length, before);
    abi.shutdownGate.resolve(0);
    await disposing;
    assert.equal(disposed, true);
});

test('every call after engine.dispose throws Disposed', async () => {
    const { abi, engine } = await startEngine();
    const lamp = engine.scene.create('lamp');
    engine.dispose();
    assert.ok(abi.calls.includes('ge_shutdown'));
    assert.throws(() => lamp.transform.position.x, { code: 'Disposed' });
    assert.throws(() => engine.run(), { code: 'Disposed' });
    assert.throws(() => engine.scene.create(), { code: 'Disposed' });
});
