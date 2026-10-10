// OrbitControls against the editor camera rig's math (SceneViewCameraRig.h), ported here
// independently in double: the camera's position and view basis on three poses, the pan
// basis, the zoom step and frame-rate independent damping, within 1e-6.

import assert from 'node:assert/strict';
import test from 'node:test';
import { OrbitControlsImpl } from '../dist/src/orbit-controls.js';
import { startEngine } from '../dist/test/harness.js';

const kTolerance = 1e-6;
const kDegToRad = 0.017453292519943295769;

// SceneViewCameraRig.h: LookDirection, OrbitCameraPosition, PanBasis.
function lookDirection(yawDeg, pitchDeg) {
    const yaw = yawDeg * kDegToRad;
    const pit = pitchDeg * kDegToRad;
    const cp = Math.cos(pit);
    const f = [cp * Math.cos(yaw), Math.sin(pit), cp * Math.sin(yaw)];
    const len = Math.hypot(...f);
    return f.map((v) => v / len);
}

function orbitCameraPosition(pivot, yawDeg, pitchDeg, distance) {
    const f = lookDirection(yawDeg, pitchDeg);
    return pivot.map((p, i) => p - f[i] * distance);
}

function panBasis(yawDeg, pitchDeg) {
    const look = lookDirection(yawDeg, pitchDeg);
    let flatX = look[0];
    let flatZ = look[2];
    const lenFlatSq = flatX * flatX + flatZ * flatZ;
    if (lenFlatSq > 1e-12) {
        const inv = 1 / Math.sqrt(lenFlatSq);
        flatX *= inv;
        flatZ *= inv;
    } else {
        flatX = 0;
        flatZ = 1;
    }
    const right = [flatZ, 0, -flatX];
    const up = [right[1] * look[2] - right[2] * look[1], right[2] * look[0] - right[0] * look[2], right[0] * look[1] - right[1] * look[0]];
    const upLen = Math.hypot(...up);
    return { right, up: up.map((v) => v / upLen) };
}

function assertVec(actual, expected, label) {
    for (let i = 0; i < 3; ++i) {
        assert.ok(Math.abs(actual[i] - expected[i]) <= kTolerance, `${label}[${i}]: ${actual[i]} vs ${expected[i]}`);
    }
}

function fakeElement() {
    const listeners = new Map();
    return {
        clientHeight: 600,
        addEventListener(type, handler) { listeners.set(type, handler); },
        removeEventListener(type) { listeners.delete(type); },
        setPointerCapture() {},
        dispatch(type, event) { listeners.get(type)?.({ preventDefault() {}, button: 0, shiftKey: false, pointerId: 1, deltaMode: 0, ...event }); },
        listenerCount() { return listeners.size; },
    };
}

/** Controls orbiting `target` at the rig pose, the camera placed there first. */
async function controlsAt(target, yaw, pitch, distance) {
    const { abi, engine } = await startEngine();
    const camera = engine.scene.camera;
    camera.transform.position.set(...orbitCameraPosition([0, 0, 0], yaw, pitch, distance));
    const element = fakeElement();
    const controls = new OrbitControlsImpl(camera, element);
    controls.target.set(...target);
    controls.enableDamping = false;
    controls.update();
    const matrix = () => abi.floats(camera.id, 'Transform', 'matrix');
    return { controls, element, matrix, camera };
}

const kPoses = [
    { target: [0, 0, 0], yaw: 90, pitch: -20, distance: 5 },
    { target: [1.5, 0.75, -2], yaw: 200, pitch: 35, distance: 3.25 },
    { target: [-3, 1, 2], yaw: -45, pitch: -80, distance: 2 },
];

test('the camera sits and looks where the rig puts it, on three poses', async () => {
    for (const { target, yaw, pitch, distance } of kPoses) {
        const { matrix } = await controlsAt(target, yaw, pitch, distance);
        const m = matrix();
        const label = `yaw ${yaw} pitch ${pitch}`;
        assertVec([m[12], m[13], m[14]], orbitCameraPosition(target, yaw, pitch, distance), `${label} position`);
        assertVec([m[8], m[9], m[10]], lookDirection(yaw, pitch), `${label} forward (+Z)`);
        assertVec([m[0], m[1], m[2]], panBasis(yaw, pitch).right, `${label} right (+X)`);
    }
});

test('pitch reads the elevation, sets it on the next update, holds it short of the pole and refuses a non-number', async () => {
    const { target, yaw, pitch, distance } = kPoses[0];
    const { controls, matrix } = await controlsAt(target, yaw, pitch, distance);
    assert.ok(Math.abs(controls.pitch - pitch) <= 1e-4, `starts at ${controls.pitch}`);   // read back from the camera's float32 position
    controls.pitch = -10;
    controls.update();
    const m = matrix();
    assertVec([m[12], m[13], m[14]], orbitCameraPosition(target, yaw, -10, distance), 'position at pitch -10');
    controls.pitch = 120;
    assert.equal(controls.pitch, 89);
    assert.throws(() => { controls.pitch = NaN; }, { code: 'InvalidArgument' });
    assert.equal(controls.pitch, 89);
});

test('a pan drag moves the target along the rig\'s pan basis by the pixel footprint at the target', async () => {
    const { target, yaw, pitch, distance } = kPoses[1];
    const { controls, element } = await controlsAt(target, yaw, pitch, distance);
    element.dispatch('pointerdown', { button: 2, clientX: 100, clientY: 100 });
    element.dispatch('pointermove', { clientX: 130, clientY: 88 });
    controls.update();
    const metersPerPixel = 2 * distance * Math.tan(30 * kDegToRad) / 600;
    const { right, up } = panBasis(yaw, pitch);
    // The rig's up is cross(right, look): screen-down in this left-handed basis, as DOM y is.
    const expected = target.map((t, i) => t + (-right[i] * 30 - up[i] * -12) * metersPerPixel);
    assertVec([controls.target.x, controls.target.y, controls.target.z], expected, 'target');
});

test('the wheel zooms by e per 1000 pixels, and dispose removes every listener', async () => {
    const { controls, element } = await controlsAt(...Object.values(kPoses[0]));
    element.dispatch('wheel', { deltaY: 100 });
    controls.update();
    assert.ok(Math.abs(controls.distance - 5 * Math.exp(0.1)) <= kTolerance, `${controls.distance}`);
    controls.dispose();
    assert.equal(element.listenerCount(), 0);
});

test('damping converges on the undamped pose and does not depend on the frame rate', async () => {
    const pose = kPoses[0];
    const runs = [];
    for (const [frames, dt] of [[4, 1 / 60], [2, 2 / 60], [1, 0]]) {
        const { controls, element, matrix } = await controlsAt(pose.target, pose.yaw, pose.pitch, pose.distance);
        controls.enableDamping = frames !== 1;
        element.dispatch('pointerdown', { clientX: 0, clientY: 0 });
        element.dispatch('pointermove', { clientX: 60, clientY: 20 });
        for (let i = 0; i < frames; ++i) controls.update(dt);
        const after = matrix().slice(12, 15);
        for (let i = 0; i < 2000; ++i) controls.update(1 / 60);
        runs.push({ after, settled: matrix().slice(12, 15) });
    }
    assertVec(runs[0].after, runs[1].after, '4 frames at 60 Hz vs 2 at 30 Hz');
    assertVec(runs[0].settled, runs[2].after, 'damped, settled vs undamped');
});

test('pointer events make no engine call, and a second pointer does not take over the drag', async () => {
    const { controls, element, camera } = await controlsAt(...Object.values(kPoses[0]));
    const abi = camera.owner.bridge.abi;
    const calls = [];
    const spy = new Proxy(abi, { get: (t, k) => (typeof t[k] === 'function' ? (...a) => { calls.push(k); return t[k](...a); } : t[k]) });
    camera.owner.bridge.abi = spy;
    element.dispatch('pointerdown', { button: 2, clientX: 0, clientY: 0, pointerId: 1 });
    element.dispatch('pointerdown', { button: 0, clientX: 0, clientY: 0, pointerId: 2 });
    element.dispatch('pointermove', { clientX: 500, clientY: 0, pointerId: 2 });
    element.dispatch('pointermove', { clientX: 10, clientY: 0, pointerId: 1 });
    element.dispatch('wheel', { deltaY: 1, deltaMode: 2 });
    assert.deepEqual(calls, []);
    camera.owner.bridge.abi = abi;
    const before = controls.target.x;
    controls.update();
    assert.notEqual(controls.target.x, before);
    assert.ok(Math.abs(controls.distance - 5 * Math.exp(600 / 1000)) <= kTolerance, `page-mode wheel: ${controls.distance}`);
});

test('a drag past the pole leaves no motion pushing against it: dragging back moves at once', async () => {
    const { controls, element, matrix } = await controlsAt(...Object.values(kPoses[0]));
    controls.enableDamping = true;
    element.dispatch('pointerdown', { clientX: 0, clientY: 0 });
    element.dispatch('pointermove', { clientX: 0, clientY: -600 });
    // 20 damped frames: the pole is reached after 4, while 360 * 0.9^20 = 44 degrees of drag remain.
    for (let i = 0; i < 20; ++i) controls.update(1 / 60);
    element.dispatch('pointermove', { clientX: 0, clientY: -540 });
    for (let i = 0; i < 400; ++i) controls.update(1 / 60);
    assertVec(matrix().slice(12, 15), orbitCameraPosition([0, 0, 0], 90, 89 - 36, 5), 'pitch 36 degrees below the pole');
});
