// The view semantics: every read goes to the engine (a value the engine changes is what the
// page sees next), set creates or updates, misuse throws before anything is written, and the
// transform views speak degrees over the engine's matrix.

import assert from 'node:assert/strict';
import test from 'node:test';
import { Camera, DDGIVolume, Light, LocalBounds, MeshRenderer, SkyEnvironment, Transform } from '../dist/src/components.js';
import { composeMatrix, matrixMultiply, quatFromAxisDegrees } from '../dist/src/math.js';
import { loadModel, startEngine } from '../dist/test/harness.js';

const kTolerance = 1e-5;

function near(actual, expected, message) {
    assert.ok(Math.abs(actual - expected) <= kTolerance, `${message}: ${actual} vs ${expected}`);
}

test('a transform read after an engine-side write sees the new value', async () => {
    const { abi, engine } = await startEngine();
    const box = engine.scene.create('box');
    const position = box.transform.position;
    near(position.x, 0, 'before');
    abi.setFloats(box.id, 'Transform', 'matrix', [4, 5, 6], 12);
    near(position.x, 4, 'x after the engine moved it');
    near(box.transform.position.z, 6, 'z after the engine moved it');
});

test('a component view read after an engine-side write sees the new value', async () => {
    const { abi, engine } = await startEngine();
    const lamp = engine.scene.create('lamp');
    const light = lamp.set(Light, { type: 'Point', intensity: 800 });
    assert.equal(light.intensity, 800);
    abi.setFloats(lamp.id, 'Light', 'Intensity', [12]);
    assert.equal(light.intensity, 12);
    assert.equal(lamp.get(Light).type, 'Point');
});

test('set adds a missing component, then updates only the fields it names', async () => {
    const { abi, engine } = await startEngine();
    const lamp = engine.scene.create('lamp');
    assert.equal(lamp.has(Light), false);
    lamp.set(Light, { type: 'Spot', color: [1, 0.5, 0.25] });
    lamp.set(Light, { intensity: 3 });
    const light = lamp.get(Light);
    assert.equal(light.type, 'Spot');
    assert.deepEqual(light.color, [1, 0.5, 0.25]);
    assert.equal(light.intensity, 3);
    assert.throws(() => lamp.add(Light), { code: 'InvalidArgument', message: /already has Light; use entity.set/ });
    lamp.remove(Light);
    assert.equal(lamp.has(Light), false);
    assert.equal(lamp.get(Light), undefined);
    assert.equal(abi.hasComponent(lamp.id, 'Light'), false);
});

test('a misspelled field or an unknown enumerator throws naming the fix and writes nothing', async () => {
    const { abi, engine } = await startEngine();
    const lamp = engine.scene.create('lamp');
    lamp.set(Light, { intensity: 5 });
    const writes = abi.calls.length;
    assert.throws(() => lamp.set(Light, { intensity: 7, brightness: 2 }), { code: 'InvalidArgument', message: /Light has no field 'brightness'. Its fields are: type, color, intensity/ });
    assert.throws(() => lamp.set(Light, { intensity: 7, type: 'Pointy' }), { code: 'InvalidArgument', message: /type takes one of 'Directional', 'Point'/ });
    assert.equal(abi.calls.length, writes);
    assert.equal(lamp.get(Light).intensity, 5);
});

test('an entity field reads and writes an Entity', async () => {
    const { engine } = await startEngine();
    const sky = engine.scene.sky.entity.get(SkyEnvironment);
    assert.equal(sky.sunLight, engine.scene.sun.entity);
    const other = engine.scene.create('other sun');
    sky.sunLight = other;
    assert.equal(engine.scene.sky.entity.get(SkyEnvironment).sunLight, other);
    sky.sunLight = null;
    assert.equal(sky.sunLight, null);
});

test('transform.parent reads back the entity it was set to', async () => {
    const { engine } = await startEngine();
    const parent = engine.scene.create('parent');
    const child = engine.scene.create('child');
    assert.equal(child.transform.parent, null);
    child.transform.parent = parent;
    assert.equal(child.transform.parent, parent);
    child.transform.parent = null;
    assert.equal(child.transform.parent, null);
});

test('transform.children lists the entities parented to it, past the first buffer\'s size', async () => {
    const { engine } = await startEngine();
    const parent = engine.scene.create('parent');
    assert.deepEqual(parent.transform.children, []);
    const children = Array.from({ length: 20 }, (_, i) => engine.scene.create(`child ${i}`));
    for (const child of children) child.transform.parent = parent;
    children[0].transform.parent = children[1];
    assert.deepEqual(parent.transform.children, children.slice(1));
    assert.deepEqual(children[1].transform.children, [children[0]]);
});

test('rotation is in degrees: rotateY(90) turns the entity\'s +Z onto +X, and a position write keeps the rotation', async () => {
    const { abi, engine } = await startEngine();
    const box = engine.scene.create('box');
    box.transform.rotateY(90);
    const m = abi.floats(box.id, 'Transform', 'matrix');
    near(m[8], 1, 'forward x');
    near(m[10], 0, 'forward z');
    near(box.transform.rotation.y, 90, 'rotation.y');
    const rotationBytes = m.slice(0, 12);
    box.transform.position.set(1, 2, 3);
    assert.deepEqual(abi.floats(box.id, 'Transform', 'matrix').slice(0, 12), rotationBytes);
    box.transform.scale.set(2, 2, 2);
    near(box.transform.scale.x, 2, 'scale');
    near(box.transform.rotation.y, 90, 'rotation after a scale write');
});

test('rotateWorldY turns the entity about the world\'s up axis under turned and mirrored parents, and not at all under a degenerate one', async () => {
    const { abi, engine } = await startEngine();
    const turnY = (degrees) => composeMatrix([0, 0, 0], quatFromAxisDegrees([0, 1, 0], degrees), [1, 1, 1]);
    const worldOf = (chain) => chain.reduce((world, entity) => matrixMultiply(world, abi.floats(entity.id, 'Transform', 'matrix')), turnY(0));
    // Each case: the parents' local matrices, root first; the child starts tilted 30 degrees about X.
    const cases = {
        'three turned parents': [
            composeMatrix([1, 2, 3], quatFromAxisDegrees([1, 0, 0], 90), [1, 1, 1]),
            composeMatrix([0, 1, 0], quatFromAxisDegrees([0, 0, 1], 40), [2, 2, 2]),
            composeMatrix([0, 0, 5], quatFromAxisDegrees([0, 1, 0], -70), [1, 1, 1]),
        ],
        'a mirrored parent': [composeMatrix([0, 0, 0], quatFromAxisDegrees([1, 0, 0], 90), [-1, 1, 1])],
    };
    for (const [name, matrices] of Object.entries(cases)) {
        const chain = matrices.map((matrix, index) => {
            const parent = engine.scene.create(`${name} ${index}`);
            abi.setFloats(parent.id, 'Transform', 'matrix', matrix);
            return parent;
        });
        chain.forEach((entity, index) => { if (index > 0) entity.transform.parent = chain[index - 1]; });
        const child = engine.scene.create(`${name} child`);
        child.transform.parent = chain[chain.length - 1];
        child.transform.rotateX(30);
        const before = worldOf([...chain, child]);
        child.transform.rotateWorldY(50);
        const after = worldOf([...chain, child]);
        // The child's world matrix is the old one turned 50 degrees about world +Y through its position.
        const expected = matrixMultiply(turnY(50), before);
        for (let i = 0; i < 12; ++i) near(after[i], expected[i], `${name}: world matrix element ${i}`);
        for (let i = 12; i < 15; ++i) near(after[i], before[i], `${name}: world position ${i - 12}`);
    }    // A parent scaled to zero (a hidden subtree) or flattened has no world up to turn about in
    // its space: the turn does nothing, and does not throw.
    for (const scale of [[0, 0, 0], [1, 0, 1]]) {
        const parent = engine.scene.create(`parent scaled ${scale}`);
        parent.transform.scale.set(...scale);
        const child = engine.scene.create('child of a degenerate parent');
        child.transform.parent = parent;
        child.transform.rotateX(30);
        const local = abi.floats(child.id, 'Transform', 'matrix');
        child.transform.rotateWorldY(50);
        assert.deepEqual(abi.floats(child.id, 'Transform', 'matrix'), local, `under a parent scaled ${scale}`);
    }
});

test('a positive turn takes +Y toward +Z about X, +Z toward +X about Y and +X toward +Y about Z', async () => {
    const { abi, engine } = await startEngine();
    // Each call turns a fresh entity 90 degrees; the column it reads is the turned axis.
    const turned = (turn, column) => {
        const box = engine.scene.create('box');
        turn(box.transform);
        return abi.floats(box.id, 'Transform', 'matrix').slice(column * 4, column * 4 + 3);
    };
    const expectAxis = (actual, expected, message) => expected.forEach((value, i) => near(actual[i], value, `${message} ${i}`));
    expectAxis(turned((t) => t.rotateX(90), 1), [0, 0, 1], 'rotateX: +Y onto +Z');
    expectAxis(turned((t) => t.rotateZ(90), 0), [0, 1, 0], 'rotateZ: +X onto +Y');
    expectAxis(turned((t) => t.rotateWorldY(90), 2), [1, 0, 0], 'rotateWorldY: +Z onto +X');
});

test('a loaded model\'s bounds are its LocalBounds box in its parent\'s space', async () => {
    const { engine, host } = await startEngine();
    const model = await loadModel(engine, host, 'robot.glb');
    model.transform.position.set(10, 0, 0);
    model.transform.scale.set(2, 2, 2);
    const bounds = model.bounds;
    near(bounds.center.x, 10, 'center x');
    near(bounds.center.y, 1, 'center y');
    near(bounds.size.y, 2, 'size y');
    near(bounds.min.x, 9, 'min x');
    assert.equal(engine.scene.create('empty').bounds, null);
});

test('entity.animation lists the model\'s clips, plays one at a speed, pauses, and throws the engine\'s message', async () => {
    const { abi, engine, host } = await startEngine();
    const fox = await loadModel(engine, host, 'fox.glb');
    abi.animate(fox.id, ['Survey', 'Walk', 'Run']);
    assert.deepEqual(fox.animation.clips, ['Survey', 'Walk', 'Run']);
    fox.animation.play('Run', { speed: 1.5 });
    assert.deepEqual(abi.animation(fox.id), { clips: ['Survey', 'Walk', 'Run'], clip: 'Run', speed: 1.5, paused: false });
    fox.animation.pause();
    assert.equal(abi.animation(fox.id).paused, true);
    fox.animation.play('Walk');
    assert.equal(abi.animation(fox.id).speed, 1, 'the default speed');
    assert.throws(() => fox.animation.play('Jump'), { code: 'Engine', message: /entity.animation.play\('Jump'\) failed: the model has no clip 'Jump'; its clips are Survey, Walk, Run/ });
    const lamp = engine.scene.create('lamp');
    assert.throws(() => lamp.animation.clips, { code: 'Engine', message: /entity.animation.clips failed: entity \d+ is not an animated model/ });
    lamp.destroy();
    assert.throws(() => lamp.animation.pause(), { code: 'Disposed' });
});

test('Engine.create builds a camera, a sun in lux driven by the sky, and the sky; the views read and write them', async () => {
    const { engine } = await startEngine();
    const { camera, sun, sky } = engine.scene;
    assert.equal(camera.name, 'Camera');
    assert.ok(camera.has(Camera));
    const light = sun.entity.get(Light);
    assert.equal(light.type, 'Directional');
    assert.equal(light.intensityUnit, 'Lux');
    assert.equal(sun.intensity, 100000);
    sun.intensity = 50000;
    assert.equal(light.intensity, 50000);
    sky.timeOfDay = 9.5;
    assert.equal(sky.entity.get(SkyEnvironment).timeOfDayHours, 9.5);
    assert.equal(Light.typeId !== null && Transform.typeId !== null, true);
});

test('a destroyed entity\'s handle throws Disposed', async () => {
    const { engine } = await startEngine();
    const lamp = engine.scene.create('lamp');
    assert.equal(lamp.name, 'lamp');
    lamp.destroy();
    assert.throws(() => lamp.transform.position.x, { code: 'Disposed', message: /was destroyed/ });
    assert.throws(() => lamp.get(Light), { code: 'Disposed' });
});

test('Transform cannot be added or removed, from plain JavaScript too', async () => {
    const { abi, engine } = await startEngine();
    const box = engine.scene.create('box');
    assert.throws(() => box.remove(Transform), { code: 'InvalidArgument', message: /Every entity has a Transform, so entity.remove\(Transform\) is refused/ });
    assert.throws(() => box.add(Transform), { code: 'InvalidArgument', message: /entity.add\(Transform\) is refused/ });
    assert.equal(abi.hasComponent(box.id, 'Transform'), true);
});

test('an asset field round-trips { guid }, taking the dashed form too', async () => {
    const { engine } = await startEngine();
    const mesh = engine.scene.create('mesh').set(MeshRenderer, { modelAssetGuid: { guid: '0123456789abcdef0123456789abcdef' } });
    assert.deepEqual(mesh.modelAssetGuid, { guid: '0123456789abcdef0123456789abcdef' });
    mesh.modelAssetGuid = { guid: 'FEDCBA98-7654-3210-FEDC-BA9876543210' };
    assert.deepEqual(mesh.modelAssetGuid, { guid: 'fedcba9876543210fedcba9876543210' });
    assert.throws(() => { mesh.modelAssetGuid = { guid: 'not-a-guid' }; }, { code: 'InvalidArgument', message: /32 hex digits, dashed/ });
});

test('scene.create({ mesh }) draws a built-in mesh with its bounds; a name still works; an unknown mesh leaves no entity', async () => {
    const { abi, engine } = await startEngine();
    const floor = engine.scene.create({ name: 'floor', mesh: 'plane' });
    assert.equal(floor.name, 'floor');
    assert.equal(floor.has(MeshRenderer), true);
    assert.equal(floor.has(LocalBounds), true);
    assert.deepEqual([floor.bounds.size.x, floor.bounds.size.y, floor.bounds.size.z], [1, 0, 1]);
    assert.ok(abi.calls.includes(`ge_entity_set_mesh(${floor.id}, plane)`));
    assert.equal(engine.scene.create('lamp').has(MeshRenderer), false);
    const before = abi.calls.filter((call) => call === 'ge_entity_destroy').length;
    assert.throws(() => engine.scene.create({ mesh: 'teapot' }),
        { code: 'Engine', message: /scene.create failed: .*'teapot' is not a built-in mesh; use 'plane', 'cube', 'sphere' or 'capsule'/ });
    assert.equal(abi.calls.filter((call) => call === 'ge_entity_destroy').length, before + 1, 'the refused entity was not destroyed');
});

test('DDGIVolume is a component a page sets: its enum fields read and write by name', async () => {
    const { engine } = await startEngine();
    const volume = engine.scene.create('gi').set(DDGIVolume, { fit: 'FollowCamera', intensity: 1.5 });
    assert.equal(volume.fit, 'FollowCamera');
    assert.equal(volume.intensity, 1.5);
    assert.throws(() => { volume.fit = 'Sideways'; }, { code: 'InvalidArgument' });
});
