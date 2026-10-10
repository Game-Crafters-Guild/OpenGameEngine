// scene.query: the chunk walk over the engine's memory. A written column changes the entities,
// a read column is a copy, every matching entity is visited once across the chunks, and a
// callback that throws ends the query in the engine.

import assert from 'node:assert/strict';
import test from 'node:test';
import { Light, MeshRenderer, Transform } from '../dist/src/components.js';
import { startEngine } from '../dist/test/harness.js';

test('a query visits every entity with its components once, across chunks', async () => {
    const { abi, engine } = await startEngine();
    const lamps = Array.from({ length: 7 }, (_, i) => engine.scene.create(`lamp ${i}`));
    lamps.forEach((lamp, i) => lamp.set(Light, { intensity: i }));
    engine.scene.create('no light');
    const seen = [];
    let chunks = 0;
    engine.scene.query([Light]).forEachChunk((chunk) => {
        ++chunks;
        const { values, offset, stride } = chunk.floats(Light, 'intensity');
        for (let i = 0; i < chunk.count; ++i) seen.push([chunk.entity(i), values[offset + i * stride]]);
    });
    assert.ok(chunks > 1, `7 lamps in chunks of ${abi.queryChunkSize} took ${chunks} chunk`);
    // The scene's default sun is a Light too.
    const entities = seen.map(([entity]) => entity);
    assert.equal(new Set(entities).size, entities.length, 'an entity was visited twice');
    for (const entity of entities) assert.ok(entity.has(Light), `entity ${entity.id} has no Light`);
    for (const [i, lamp] of lamps.entries()) assert.equal(seen.find(([entity]) => entity === lamp)?.[1], i, `lamp ${i}`);
});

test('a written column changes the entities; a read column is a copy', async () => {
    const { abi, engine } = await startEngine();
    const lamps = Array.from({ length: 4 }, () => engine.scene.create());
    lamps.forEach((lamp) => lamp.set(Light, { intensity: 2 }));
    engine.scene.query([Light], { write: [Transform] }).forEachChunk((chunk) => {
        const matrix = chunk.floats(Transform, 'matrix');
        const intensity = chunk.floats(Light, 'intensity');
        for (let i = 0; i < chunk.count; ++i) {
            matrix.values[matrix.offset + i * matrix.stride + 12] = 10 + chunk.entity(i).id;
            intensity.values[intensity.offset + i * intensity.stride] = 99;
        }
    });
    for (const lamp of lamps) {
        assert.equal(abi.floats(lamp.id, 'Transform', 'matrix')[12], 10 + lamp.id, 'the write did not land');
        assert.equal(lamp.transform.position.x, 10 + lamp.id);
        assert.equal(lamp.get(Light).intensity, 2, 'a read column was written through');
    }
});

test('a callback that throws ends the query in the engine, and misuse names the fix', async () => {
    const { abi, engine } = await startEngine();
    engine.scene.create().set(Light, { intensity: 1 });
    const query = engine.scene.query([], { write: [Light] });
    assert.throws(() => query.forEachChunk(() => { throw new Error('page bug'); }), /page bug/);
    assert.equal(abi.calls.at(-1), 'ge_query_end');
    // The engine is free again: the next walk starts, over the lamp and the scene's default sun.
    let visited = 0;
    query.forEachChunk((chunk) => { visited += chunk.count; });
    assert.equal(visited, 2);

    assert.throws(() => engine.scene.query([Light], { write: [Light] }), { code: 'InvalidArgument', message: /lists Light twice/ });
    assert.throws(() => engine.scene.query([]), { code: 'InvalidArgument', message: /needs at least one component/ });
    query.forEachChunk((chunk) => {
        assert.throws(() => chunk.floats(Light, 'type'), { code: 'InvalidArgument', message: /reads float fields; Light.type is UInt32/ });
        assert.throws(() => chunk.floats(MeshRenderer, 'meshId'), { code: 'InvalidArgument', message: /This query's components are Light; MeshRenderer is not one of them/ });
        assert.throws(() => chunk.entity(chunk.count), { code: 'InvalidArgument', message: /chunk.entity takes 0 to \d+; got \d+/ });
    });
});

test('engine.dispose() inside a walk ends the walk with the engine, and no call follows the shutdown', async () => {
    const { abi, engine } = await startEngine();
    for (let i = 0; i < 4; ++i) engine.scene.create().set(Light, { intensity: i });
    let disposed = null;
    let visits = 0;
    engine.scene.query([Light]).forEachChunk(() => {
        ++visits;
        disposed ??= engine.dispose();
    });
    await disposed;
    assert.equal(visits, 1, 'the walk went on after the engine was disposed');
    assert.equal(abi.calls.at(-1), 'ge_shutdown', `calls after the shutdown: ${abi.calls.slice(abi.calls.lastIndexOf('ge_shutdown') + 1)}`);
    assert.equal(abi.queryRunning, false, 'the module still holds the walk');
    assert.throws(() => engine.scene.query([Light]).forEachChunk(() => {}), { code: 'Disposed' });
});

test('queries share one block in module memory: making a query every frame allocates nothing there', async () => {
    const { abi, engine } = await startEngine();
    engine.scene.create().set(Light, { intensity: 1 });
    engine.scene.query([Light], { write: [Transform] }).forEachChunk(() => {});
    const before = abi.mallocs;
    for (let frame = 0; frame < 50; ++frame) engine.scene.query([Light], { write: [Transform] }).forEachChunk(() => {});
    assert.equal(abi.mallocs - before, 0, 'module allocations over 50 new queries');
    const seventeen = Array.from({ length: 17 }, () => Light);
    assert.throws(() => engine.scene.query(seventeen), { code: 'InvalidArgument', message: /takes at most 16 components; this one lists 17/ });
});
