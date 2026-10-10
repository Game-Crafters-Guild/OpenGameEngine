// Engine.create's choices and the frame loop: the build by cross-origin isolation, the
// WebGPU and release checks, the hidden-tab timer, resize tracking and onFrame.

import assert from 'node:assert/strict';
import test from 'node:test';
import { createEngine, selectBuild } from '../dist/src/engine.js';
import { fingerprint } from '../dist/src/reflection.js';
import { StubAbi } from '../dist/test/stub-abi.js';
import { FakeHost, fakeCanvas, startEngine } from '../dist/test/harness.js';

test("threads: 'auto' takes the threaded build only on a cross-origin isolated page, and says why", async () => {
    assert.deepEqual(selectBuild('auto', true), { build: 'mt', reason: 'the page is cross-origin isolated' });
    assert.equal(selectBuild('auto', false).build, 'st');
    assert.equal(selectBuild('single', true).build, 'st');
    assert.throws(() => selectBuild('multi', false), { code: 'InvalidArgument', message: /coi-serviceworker.js/ });
    const host = new FakeHost();
    host.isolated = true;
    let loaded = null;
    await createEngine({ canvas: fakeCanvas() }, host, async (build) => { loaded = build; return new StubAbi(); }, { version: '0.1.0', fingerprint: null });
    assert.equal(loaded, 'mt');
    assert.match(host.logs[0], /using the threaded build because the page is cross-origin isolated/);
});

test('Engine.create rejects without WebGPU, naming the browsers', async () => {
    const host = new FakeHost();
    host.webGpu = false;
    await assert.rejects(createEngine({ canvas: fakeCanvas() }, host, async () => new StubAbi(), { version: '0.1.0', fingerprint: null }),
        { code: 'NoWebGPU', message: /no WebGPU. Use a current Chrome or Edge/ });
});

test('a library built against another release rejects naming both versions and the fix', async () => {
    const abi = new StubAbi();
    abi.engineVersion = '0.2.0';
    const load = async () => abi;
    await assert.rejects(createEngine({ canvas: fakeCanvas() }, new FakeHost(), load, { version: '0.1.0', fingerprint: '0123456789abcdef' }),
        { code: 'VersionMismatch', message: /engine module is release 0.2.0 and this library is 0.1.0.*Update @openengine\/web to 0.2.0/ });
    const matching = await createEngine({ canvas: fakeCanvas() }, new FakeHost(), async () => new StubAbi(),
        { version: '0.1.0', fingerprint: fingerprint(new StubAbi().reflectionJson()) });
    assert.ok(matching.scene.camera);
});

test('a hidden tab ticks on a 33 ms timer, a visible one on animation frames', async () => {
    const { abi, host, engine } = await startEngine();
    host.hidden = true;
    engine.run();
    assert.equal(host.frames.size, 0);
    assert.deepEqual([...host.timers.values()].map((t) => t.delayMs), [33]);
    host.fireTimers();
    assert.equal(abi.calls.filter((c) => c === 'ge_tick').length, 1);
    host.setHidden(false);
    assert.equal(host.timers.size, 0);
    assert.equal(host.frames.size, 1);
    engine.pause();
    assert.equal(host.frames.size, 0);
});

test('a canvas resize reaches the engine as CSS size and device pixel ratio', async () => {
    const { abi, host, canvas } = await startEngine();
    assert.deepEqual(abi.lastResize, [800, 600, 2]);
    canvas.clientWidth = 1024;
    host.dpr = 1.5;
    host.resizeCanvas();
    assert.deepEqual(abi.lastResize, [1024, 600, 1.5]);
});

test('onFrame callbacks run before the tick with the step in seconds, until unsubscribed', async () => {
    const { abi, host, engine } = await startEngine();
    const steps = [];
    const stop = engine.onFrame((dt) => steps.push([dt, abi.calls.filter((c) => c === 'ge_tick').length]));
    engine.run();
    host.fireFrames(1000);
    host.fireFrames(1016);
    stop();
    host.fireFrames(1032);
    assert.deepEqual(steps.map(([, ticks]) => ticks), [0, 1]);
    assert.equal(steps[0][0], 0);
    assert.ok(Math.abs(steps[1][0] - 0.016) < 1e-12);
    assert.equal(abi.calls.filter((c) => c === 'ge_tick').length, 3);
});
