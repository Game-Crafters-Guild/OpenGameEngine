// The WebLibrary module's ABI under node: the exports Apps/WebLibrary/ts/src/abi.ts specifies,
// called on the real module (WebLibraryAbiSmoke, the page module's own objects with a headless
// boot in place of ge_create's window and device), and the call contract's cases from the
// design (workbench designs/web/web-api.md 3.1).
//
//   node --test-reporter=spec --test-reporter-destination=stdout \
//        --test-reporter=junit --test-reporter-destination=C:/path/results.xml \
//        Tests/Wasm/WebLibraryAbiSmoke.mjs --module <build>/bin/WebLibraryAbiSmoke.mjs \
//        --scanner-json <build>/generated/components.json --model <file.glb> [--report <file.json>]
//
// The facade case needs Apps/WebLibrary/ts/dist (node Apps/WebLibrary/ts/test/run-tests.mjs
// builds it).

import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { after, before, test } from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { adaptModule } from '../../Apps/WebLibrary/opengine-core-binding.js';

const kOk = 0;
const kFailed = -1;
const kInvalidEntity = 0xffffffff;
const AssetStatus = { Loading: 0, Ready: 1, Failed: 2 };
// One triangle whose buffer names a file the server does not have beside it.
const kGltfWithoutItsBuffer = JSON.stringify({
    asset: { version: '2.0' },
    scenes: [{ nodes: [0] }],
    nodes: [{ mesh: 0 }],
    meshes: [{ primitives: [{ attributes: { POSITION: 0 } }] }],
    buffers: [{ byteLength: 36, uri: 'triangle.bin' }],
    bufferViews: [{ buffer: 0, byteLength: 36 }],
    accessors: [{ bufferView: 0, componentType: 5126, count: 3, type: 'VEC3', min: [0, 0, 0], max: [1, 1, 0] }],
});

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

function argument(name, fallback) {
    const index = process.argv.indexOf(`--${name}`);
    if (index >= 0 && index + 1 < process.argv.length) return process.argv[index + 1];
    if (fallback !== undefined) return fallback;
    throw new Error(`WebLibraryAbiSmoke: pass --${name} <path>.`);
}

const modulePath = path.resolve(argument('module'));
const scannerJsonPath = path.resolve(argument('scanner-json'));
const modelPath = path.resolve(argument('model'));
const reportPath = argument('report', '');
const report = {};

let core = null;
let abi = null;
let server = null;
let serverUrl = '';
// Requests the server answered, by path.
const served = new Map();
let booted = false;

// ---- the module ------------------------------------------------------------------------

function text(ptr) {
    const heap = core.HEAPU8;
    let end = ptr;
    while (heap[end] !== 0) ++end;
    return new TextDecoder().decode(heap.slice(ptr, end));
}

function lastError() {
    return text(abi.ge_last_error());
}

// `value` as NUL-terminated UTF-8 in module memory; the caller frees it.
function cString(value) {
    const bytes = new TextEncoder().encode(value);
    const ptr = abi._malloc(bytes.length + 1);
    core.HEAPU8.set(bytes, ptr);
    core.HEAPU8[ptr + bytes.length] = 0;
    return ptr;
}

function withString(value, call) {
    const bytes = new TextEncoder().encode(value);
    const ptr = core._malloc(bytes.length + 1);
    core.HEAPU8.set(bytes, ptr);
    core.HEAPU8[ptr + bytes.length] = 0;
    try {
        return call(ptr);
    } finally {
        core._free(ptr);
    }
}

function withBytes(bytes, call) {
    const ptr = core._malloc(Math.max(bytes.length, 1));
    core.HEAPU8.set(bytes, ptr);
    try {
        return call(ptr);
    } finally {
        core._free(ptr);
    }
}

function readField(entity, typeId, fieldId, size) {
    const ptr = core._malloc(size);
    try {
        const code = abi.ge_field_get(entity, typeId, fieldId, ptr);
        return { code, bytes: core.HEAPU8.slice(ptr, ptr + size) };
    } finally {
        core._free(ptr);
    }
}

function boot() {
    if (booted) return;
    assert.equal(core._smoke_boot(), 0, 'the headless boot failed');
    booted = true;
}

function reflection() {
    const ptr = abi.ge_reflection_json();
    assert.notEqual(ptr, 0, `ge_reflection_json failed: ${lastError()}`);
    return JSON.parse(text(ptr));
}

function componentNamed(document, name) {
    const component = document.components.find((c) => c.name === name);
    assert.ok(component, `the registry reflects no ${name}`);
    return { ...component, typeId: BigInt(component.typeId) };
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// ---- the browser stand-ins for the suspended-create case --------------------------------

function deferred() {
    let resolve;
    const promise = new Promise((r) => { resolve = r; });
    return { promise, resolve };
}

before(async () => {
    const { default: createOpenEngine } = await import(pathToFileURL(modulePath).href);
    core = await createOpenEngine();
    abi = adaptModule(core);
    const model = fs.readFileSync(modelPath);
    server = http.createServer((request, response) => {
        served.set(request.url, (served.get(request.url) ?? 0) + 1);
        if (request.url === '/model.glb' || request.url === '/repeat/model.glb' || request.url === '/after-gltf/model.glb' ||
            request.url === '/after-wav/model.glb') {
            response.writeHead(200, { 'Content-Type': 'model/gltf-binary' });
            response.end(model);
            return;
        }
        if (request.url === '/corrupt.wav') {
            // A RIFF/WAVE header with no fmt chunk.
            response.writeHead(200, { 'Content-Type': 'audio/wav' });
            response.end(Buffer.concat([Buffer.from('RIFF'), Buffer.from([0xff, 0xff, 0xff, 0xff]), Buffer.from('WAVE')]));
            return;
        }
        if (request.url === '/triangle.gltf') {
            response.writeHead(200, { 'Content-Type': 'model/gltf+json' });
            response.end(kGltfWithoutItsBuffer);
            return;
        }
        response.writeHead(404);
        response.end();
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    serverUrl = `http://127.0.0.1:${server.address().port}`;
});

after(() => {
    server?.close();
    if (reportPath) fs.writeFileSync(reportPath, `${JSON.stringify(report, null, 2)}\n`);
});

// ---- ge_create --------------------------------------------------------------------------

test('ge_create without a document is refused with the message, and the module stays usable', async () => {
    const selector = cString('#canvas');
    const code = await abi.ge_create(selector, 0);
    core._free(selector);
    assert.equal(code, kFailed);
    assert.match(lastError(), /ge_create needs a browser page: this environment has no document/);
});

test('a call during a suspended ge_create is refused with the message, and ge_create then finishes', async () => {
    const pack = deferred();
    const saved = { document: globalThis.document, navigator: Object.getOwnPropertyDescriptor(globalThis, 'navigator'), fetch: globalThis.fetch };
    globalThis.document = { querySelector: () => ({ getContext() { return null; } }) };
    Object.defineProperty(globalThis, 'navigator', { value: { gpu: {} }, configurable: true });
    globalThis.fetch = () => pack.promise;
    const selector = cString('#canvas');
    const creating = abi.ge_create(selector, 0);
    try {
        await sleep(10);
        const value = core._malloc(4);
        assert.equal(abi.ge_field_set(1, 1n, 0, value), kFailed);
        assert.match(lastError(), /^ge_field_set was called while ge_create is suspended: no engine call may run until ge_create returns/);
        assert.equal(abi.ge_entity_create(), kInvalidEntity);
        assert.match(lastError(), /^ge_entity_create was called while ge_create is suspended/);
        core._free(value);
    } finally {
        // ge_create finishes whatever the assertions found, so the cases after this one run on
        // a module with no suspended call.
        pack.resolve({ ok: false, status: 404 });
        assert.equal(await creating, kFailed);
        core._free(selector);
        globalThis.document = saved.document;
        if (saved.navigator) Object.defineProperty(globalThis, 'navigator', saved.navigator);
        globalThis.fetch = saved.fetch;
    }
    assert.match(lastError(), /ge_create could not fetch opengine-core\.gepak from beside the engine module: HTTP 404/);
});

test('any suspended export refuses every other call until it returns (ge_shutdown waits on the GPU this way)', async () => {
    boot();
    const gate = deferred();
    core.smokeGate = gate.promise;
    const suspended = core.ccall('smoke_suspend', 'number', [], [], { async: true });
    try {
        await sleep(10);
        assert.equal(abi.ge_entity_create(), kInvalidEntity);
        assert.match(lastError(), /^ge_entity_create was called while smoke_suspend is suspended: no engine call may run until smoke_suspend returns/);
        assert.equal(abi.ge_update_assets(), kFailed);
        assert.match(lastError(), /^ge_update_assets was called while smoke_suspend is suspended/);
    } finally {
        gate.resolve();
        assert.equal(await suspended, 0);
    }
    const entity = abi.ge_entity_create();
    assert.notEqual(entity, kInvalidEntity, lastError());
    assert.equal(abi.ge_entity_destroy(entity), kOk);
});

// ---- reflection -------------------------------------------------------------------------

test('ge_reflection_json lists what the scanner emits for the same build', () => {
    boot();
    const live = reflection();
    const scanned = JSON.parse(fs.readFileSync(scannerJsonPath, 'utf8'));
    const enums = new Map(scanned.enums.map((e) => [e.name, e.values]));
    const liveByName = new Map(live.components.map((c) => [c.name, c]));
    const scannedByName = new Map(scanned.components.map((c) => [c.name, c]));

    assert.equal(live.engineVersion, fs.readFileSync(path.join(repoRoot, 'VERSION'), 'utf8').trim());
    // Reflected by hand-written registrations outside the scanner's headers, or scanned but
    // registered elsewhere: counted so the list cannot grow unnoticed.
    const onlyLive = [...liveByName.keys()].filter((n) => !scannedByName.has(n)).sort();
    const onlyScanned = [...scannedByName.keys()].filter((n) => !liveByName.has(n)).sort();
    report.reflection = { liveComponents: live.components.length, scannedComponents: scanned.components.length, onlyLive, onlyScanned };
    assert.deepEqual(onlyScanned, [], 'components the scanner emits but the module does not reflect');
    assert.deepEqual(onlyLive, ['CubeLutEffect', 'LensFlareSource', 'UIDocument'], 'hand-registered components outside the scanner');

    // The scanner writes an extent it cannot evaluate (`kMaxWeights`) as its text; the module
    // reports the number. Those fields compare on everything else.
    const symbolicCounts = [];
    const projectScanned = (field, liveField) => ({
        name: field.name,
        tsName: field.tsName,
        kind: field.kind,
        count: typeof field.count === 'string' ? (symbolicCounts.push(`${field.name}[${field.count}]`), liveField?.count) : field.count ?? 1,
        readOnly: field.readonly === true,
        enum: field.enum ? enums.get(field.enum) : undefined,
    });
    const projectLive = (field) => ({
        name: field.name,
        tsName: field.tsName,
        kind: field.kind,
        count: field.count,
        readOnly: field.readOnly === true,
        enum: field.enum,
    });
    // A field the scanner cannot type (its `omitted` list) is left out of the web types; the
    // registry still knows its kind, so the module lists it.
    const omitted = new Set(scanned.omitted.map((o) => `${o.component}.${o.field}`));
    const typedOnlyLive = [];
    for (const [name, scannedComponent] of scannedByName) {
        const liveComponent = liveByName.get(name);
        const liveFields = liveComponent.fields.filter((field) => {
            if (!omitted.has(`${name}.${field.name}`)) return true;
            typedOnlyLive.push(`${name}.${field.name}: ${field.kind}`);
            return false;
        });
        assert.deepEqual(liveFields.map(projectLive),
            scannedComponent.fields.map((field, i) => projectScanned(field, liveFields[i])), `${name}'s fields`);
        assert.match(liveComponent.typeId, /^\d+$/);
        for (const field of liveComponent.fields) assert.equal(field.size % field.count, 0, `${name}.${field.name} size`);
    }
    report.reflection.symbolicCounts = symbolicCounts;
    report.reflection.typedOnlyLive = typedOnlyLive;
});

// ---- entities ---------------------------------------------------------------------------

test('entities: create, parent, refuse a cycle, destroy with descendants, and the errors', () => {
    boot();
    const document = reflection();
    const parentComponent = componentNamed(document, 'Parent');
    const transform = componentNamed(document, 'Transform');

    const root = abi.ge_entity_create();
    const child = abi.ge_entity_create();
    const grandchild = abi.ge_entity_create();
    for (const e of [root, child, grandchild]) assert.notEqual(e, kInvalidEntity);
    assert.equal(abi.ge_component_has(root, transform.typeId), 1);

    assert.equal(abi.ge_entity_parent(child, root), kOk);
    assert.equal(abi.ge_entity_parent(grandchild, child), kOk);
    assert.equal(abi.ge_component_has(child, parentComponent.typeId), 1);

    assert.equal(abi.ge_entity_parent(root, grandchild), kFailed);
    assert.match(lastError(), /is a descendant of entity .*would make a cycle/);
    assert.equal(abi.ge_entity_parent(root, root), kFailed);
    assert.match(lastError(), /cannot be its own parent/);

    assert.equal(abi.ge_entity_parent(grandchild, kInvalidEntity), kOk);
    assert.equal(abi.ge_component_has(grandchild, parentComponent.typeId), 0);
    assert.equal(abi.ge_entity_parent(grandchild, child), kOk);

    const bounds = core._malloc(24);
    assert.equal(abi.ge_entity_bounds(root, bounds), 0);
    core._free(bounds);

    // Direct children only; a buffer too small still gets the count.
    const ids = core._malloc(8);
    assert.equal(abi.ge_entity_children(root, ids, 2), 1);
    assert.equal(new DataView(core.HEAPU8.buffer, ids, 4).getUint32(0, true), child);
    assert.equal(abi.ge_entity_children(child, ids, 0), 1);
    assert.equal(abi.ge_entity_children(grandchild, ids, 2), 0);
    core._free(ids);

    // A built-in mesh: the name is checked first, then the renderer, which the headless boot has none of.
    withString('teapot', (name) => assert.equal(abi.ge_entity_set_mesh(child, name), kFailed));
    assert.match(lastError(), /^ge_entity_set_mesh: 'teapot' is not a built-in mesh; use 'plane', 'cube', 'sphere' or 'capsule'\./);
    withString('plane', (name) => assert.equal(abi.ge_entity_set_mesh(child, name), kFailed));
    assert.match(lastError(), /^ge_entity_set_mesh needs the renderer that ge_create brings up/);

    assert.equal(abi.ge_entity_destroy(root), kOk);
    withString('plane', (name) => assert.equal(abi.ge_entity_set_mesh(root, name), kFailed));
    assert.match(lastError(), /^ge_entity_set_mesh: entity \d+ does not exist/);
    for (const e of [root, child, grandchild]) {
        assert.equal(abi.ge_component_has(e, transform.typeId), kFailed, `entity ${e} outlived its root`);
        assert.match(lastError(), new RegExp(`entity ${e} does not exist`));
    }
    assert.equal(abi.ge_entity_destroy(root), kFailed);
    assert.match(lastError(), /^ge_entity_destroy: entity \d+ does not exist/);
    assert.equal(abi.ge_entity_children(root, 0, 0), kFailed);
    assert.match(lastError(), /^ge_entity_children: entity \d+ does not exist/);
});

// ---- components -------------------------------------------------------------------------

test('components: add with defaults, has, remove, and the errors', () => {
    boot();
    const light = componentNamed(reflection(), 'Light');
    const e = abi.ge_entity_create();
    assert.equal(abi.ge_component_has(e, light.typeId), 0);
    assert.equal(abi.ge_component_add(e, light.typeId), kOk);
    assert.equal(abi.ge_component_has(e, light.typeId), 1);
    assert.equal(abi.ge_component_add(e, light.typeId), kFailed);
    assert.match(lastError(), new RegExp(`^ge_component_add: entity ${e} already has a Light`));
    assert.equal(abi.ge_component_remove(e, light.typeId), kOk);
    assert.equal(abi.ge_component_has(e, light.typeId), 0);
    assert.equal(abi.ge_component_remove(e, light.typeId), kFailed);
    assert.match(lastError(), new RegExp(`^ge_component_remove: entity ${e} has no Light`));
    assert.equal(abi.ge_component_add(e, 12345n), kFailed);
    assert.match(lastError(), /^ge_component_add: 12345 is not a component this engine reflects/);
    assert.equal(abi.ge_entity_destroy(e), kOk);
});

// ---- animation --------------------------------------------------------------------------

// The playing side needs a skinned model the renderer instantiated: the browser gate's
// viewer-animation-motion step (Tools/Web/web_package_gate.py) runs it on the model viewer.
test('animation: an entity that is not an animated model is refused with the message, and a suspended call refuses them', async () => {
    boot();
    const e = abi.ge_entity_create();
    assert.equal(abi.ge_animation_clips(e), 0);
    assert.match(lastError(), new RegExp(`^ge_animation_clips: entity ${e} is not an animated model; call it on the entity scene.load returned`));
    assert.equal(withString('Run', (clip) => abi.ge_animation_play(e, clip, 1)), kFailed);
    assert.match(lastError(), new RegExp(`^ge_animation_play: entity ${e} is not an animated model`));
    assert.equal(abi.ge_animation_pause(e), kFailed);
    assert.match(lastError(), new RegExp(`^ge_animation_pause: entity ${e} is not an animated model`));
    assert.equal(abi.ge_entity_destroy(e), kOk);
    assert.equal(abi.ge_animation_pause(e), kFailed);
    assert.match(lastError(), new RegExp(`^ge_animation_pause: entity ${e} does not exist`));

    const gate = deferred();
    core.smokeGate = gate.promise;
    const suspended = core.ccall('smoke_suspend', 'number', [], [], { async: true });
    try {
        await sleep(10);
        assert.equal(withString('Run', (clip) => abi.ge_animation_play(e, clip, 1)), kFailed);
        assert.match(lastError(), /^ge_animation_play was called while smoke_suspend is suspended/);
    } finally {
        gate.resolve();
        assert.equal(await suspended, 0);
    }
});

// ---- fields -----------------------------------------------------------------------------

function sampleBytes(field, otherEntity) {
    const bytes = new Uint8Array(field.size);
    const view = new DataView(bytes.buffer);
    const elementSize = field.size / field.count;
    for (let i = 0; i < field.count; ++i) {
        const at = i * elementSize;
        switch (field.kind) {
            case 'Bool': view.setUint8(at, 1); break;
            case 'Float': case 'Vec3': case 'Color':
                for (let k = 0; k < elementSize / 4; ++k) view.setFloat32(at + 4 * k, 0.25 + i + k * 1.5, true);
                break;
            case 'Double': view.setFloat64(at, 2.5 + i, true); break;
            case 'EntityHandle': view.setUint32(at, otherEntity, true); break;
            case 'String': break;
            default:
                for (let k = 0; k < elementSize; ++k) bytes[at + k] = (17 * (i + k) + 3) & 0x7f;
        }
    }
    if (field.kind === 'String') bytes.set(new TextEncoder().encode('smoke'));
    if (field.enum) {
        // An enum field takes one of its values.
        const value = field.enum[field.enum.length - 1].value;
        if (elementSize === 1) view.setUint8(0, value); else if (elementSize === 2) view.setUint16(0, value, true); else view.setInt32(0, value, true);
    }
    return bytes;
}

test('fields: the bytes of every field kind the registry reflects round-trip, and the errors', () => {
    boot();
    const document = reflection();
    const scanned = JSON.parse(fs.readFileSync(scannerJsonPath, 'utf8'));
    const kinds = [...new Set(scanned.components.flatMap((c) => c.fields.map((f) => f.kind)))].sort();
    const other = abi.ge_entity_create();
    const tried = {};
    const covered = {};
    for (const kind of kinds) {
        tried[kind] = [];
        for (const component of document.components) {
            const field = component.fields.find((f) => f.kind === kind && !f.readOnly);
            if (!field || covered[kind]) continue;
            const typeId = BigInt(component.typeId);
            const e = abi.ge_entity_create();
            if (abi.ge_component_has(e, typeId) !== 1 && abi.ge_component_add(e, typeId) !== kOk) {
                abi.ge_entity_destroy(e);
                continue;
            }
            const fieldId = component.fields.indexOf(field);
            const written = sampleBytes(field, other);
            assert.equal(withBytes(written, (ptr) => abi.ge_field_set(e, typeId, fieldId, ptr)), kOk, `${component.name}.${field.name}: ${lastError()}`);
            const { code, bytes } = readField(e, typeId, fieldId, field.size);
            assert.equal(code, kOk);
            tried[kind].push(`${component.name}.${field.name}`);
            if (Buffer.compare(Buffer.from(bytes), Buffer.from(written)) === 0) covered[kind] = `${component.name}.${field.name}`;
            abi.ge_entity_destroy(e);
        }
    }
    report.fieldKinds = covered;
    // Vec3 appears only in ParticleCollisionEvent, an array element struct no entity holds, so
    // no component on an entity carries one. Pinned, so a Vec3 field on a component turns this red.
    const noEntityComponent = kinds.filter((kind) => tried[kind].length === 0);
    report.fieldKindsOnNoEntityComponent = noEntityComponent;
    assert.deepEqual(noEntityComponent, ['Vec3']);
    for (const kind of kinds.filter((k) => tried[k].length > 0)) {
        assert.ok(covered[kind], `no ${kind} field round-tripped (tried ${tried[kind].join(', ')})`);
    }

    const light = componentNamed(document, 'Light');
    const e = abi.ge_entity_create();
    assert.equal(abi.ge_component_add(e, light.typeId), kOk);
    const out = core._malloc(64);
    assert.equal(abi.ge_field_get(e, light.typeId, light.fields.length, out), kFailed);
    assert.match(lastError(), new RegExp(`^ge_field_get: Light has ${light.fields.length} fields; field ${light.fields.length} does not exist`));
    const name = componentNamed(document, 'Name');
    assert.equal(abi.ge_field_get(e, name.typeId, 0, out), kFailed);
    assert.match(lastError(), new RegExp(`^ge_field_get: entity ${e} has no Name`));
    core._free(out);
    assert.equal(abi.ge_component_add(e, name.typeId), kOk);
    const unterminated = new Uint8Array(name.fields[0].size).fill(0x41);
    assert.equal(withBytes(unterminated, (ptr) => abi.ge_field_set(e, name.typeId, 0, ptr)), kFailed);
    assert.match(lastError(), /^ge_field_set: value holds at most \d+ bytes of UTF-8 including its terminating NUL/);
    abi.ge_entity_destroy(e);
    abi.ge_entity_destroy(other);
});

test('a frame\'s field reads and writes allocate nothing once the component has been touched', () => {
    boot();
    const transform = componentNamed(reflection(), 'Transform');
    // The field the facade reads and writes every frame (ts/src/entity.ts).
    const matrix = transform.fields.findIndex((f) => f.tsName === 'matrix');
    assert.notEqual(matrix, -1, 'Transform reflects no matrix');
    const size = transform.fields[matrix].size;
    const e = abi.ge_entity_create();
    const ptr = core._malloc(size);
    try {
        assert.equal(abi.ge_field_get(e, transform.typeId, matrix, ptr), kOk, lastError());
        assert.equal(abi.ge_field_set(e, transform.typeId, matrix, ptr), kOk, lastError());
        const before = core._smoke_main_thread_mallocs();
        for (let frame = 0; frame < 100; ++frame) {
            assert.equal(abi.ge_field_get(e, transform.typeId, matrix, ptr), kOk);
            assert.equal(abi.ge_field_set(e, transform.typeId, matrix, ptr), kOk);
        }
        assert.equal(core._smoke_main_thread_mallocs() - before, 0, 'mallocs over 100 get/set pairs');
    } finally {
        core._free(ptr);
        abi.ge_entity_destroy(e);
    }
});

// ---- queries ----------------------------------------------------------------------------

// The driver's buffers for one query's calls, allocated once so a query run allocates nothing here.
const g_QueryBuffers = { ids: 0, strides: 0, columns: 0, count: 0 };

function queryBuffers() {
    if (g_QueryBuffers.ids === 0) {
        g_QueryBuffers.ids = core._malloc(8 * 16);
        g_QueryBuffers.strides = core._malloc(4 * 16);
        g_QueryBuffers.columns = core._malloc(4 * 17);
        g_QueryBuffers.count = core._malloc(4);
    }
    return g_QueryBuffers;
}

// Runs one query over `read` and `write` (typeIds) to its end; calls `visit(ids, columns, count)`
// per chunk with the chunk's entity ids and column addresses. Returns the column strides.
function runQuery(read, write, visit) {
    const { ids, strides, columns, count } = queryBuffers();
    const typeIds = new BigUint64Array(core.HEAPU8.buffer, ids, read.length + write.length);
    [...read, ...write].forEach((id, i) => { typeIds[i] = id; });
    assert.equal(abi.ge_query_begin(ids, read.length, ids + 8 * read.length, write.length, strides), kOk, lastError());
    const strideValues = Array.from(new Uint32Array(core.HEAPU8.buffer, strides, read.length + write.length));
    for (;;) {
        const more = abi.ge_query_next_chunk(columns, count);
        assert.notEqual(more, kFailed, lastError());
        if (more === 0) break;
        const n = new Uint32Array(core.HEAPU8.buffer, count, 1)[0];
        const addresses = Array.from(new Uint32Array(core.HEAPU8.buffer, columns, 1 + read.length + write.length));
        visit(Array.from(new Uint32Array(core.HEAPU8.buffer, addresses[0], n)), addresses.slice(1), n);
    }
    return strideValues;
}

test('queries: every entity once across the chunks, writes land, and only a write column is stamped', () => {
    boot();
    const document = reflection();
    const transform = componentNamed(document, 'Transform');
    const light = componentNamed(document, 'Light');
    const matrix = transform.fields.find((f) => f.tsName === 'matrix');
    // More entities than one chunk holds, so the walk crosses chunks.
    const entities = Array.from({ length: 600 }, () => {
        const e = abi.ge_entity_create();
        assert.equal(abi.ge_component_add(e, light.typeId), kOk, lastError());
        return e;
    });
    const seen = new Map();
    let chunks = 0;
    const versionsBefore = entities.map((e) => core._smoke_column_version(e, transform.typeId));
    const [lightStride, transformStride] = runQuery([light.typeId, transform.typeId], [], (ids) => {
        ++chunks;
        for (const id of ids) seen.set(id, (seen.get(id) ?? 0) + 1);
    });
    assert.ok(chunks > 1, `600 entities fit one chunk (${chunks})`);
    for (const e of entities) assert.equal(seen.get(e), 1, `entity ${e} visited ${seen.get(e) ?? 0} times`);
    assert.equal(seen.size, entities.length, 'the query visited entities without a Light');
    assert.ok(lightStride > 0 && transformStride >= matrix.offset + matrix.size, `strides ${lightStride}, ${transformStride}`);
    assert.deepEqual(entities.map((e) => core._smoke_column_version(e, transform.typeId)), versionsBefore,
        'a read-only query stamped Transform');

    // A write column: every entity's matrix translation set through the chunk's memory.
    runQuery([light.typeId], [transform.typeId], (ids, [, transformColumn]) => {
        const floats = new Float32Array(core.HEAPU8.buffer, transformColumn, (ids.length * transformStride) / 4);
        ids.forEach((id, i) => { floats[(i * transformStride + matrix.offset) / 4 + 12] = id; });
    });
    const out = core._malloc(matrix.size);
    try {
        for (const e of entities) {
            assert.equal(abi.ge_field_get(e, transform.typeId, transform.fields.indexOf(matrix), out), kOk, lastError());
            assert.equal(new Float32Array(core.HEAPU8.buffer, out, 16)[12], e, `entity ${e}'s write did not land`);
        }
    } finally {
        core._free(out);
    }
    entities.forEach((e, i) => assert.ok(core._smoke_column_version(e, transform.typeId) > versionsBefore[i],
        `entity ${e}'s Transform column was not stamped by the write query`));
    for (const e of entities) abi.ge_entity_destroy(e);
});

test('queries: structural changes and frames are refused while one runs, and the errors', () => {
    boot();
    const document = reflection();
    const transform = componentNamed(document, 'Transform');
    const light = componentNamed(document, 'Light');
    const e = abi.ge_entity_create();
    const refusals = [];
    let resizeError = '';
    runQuery([transform.typeId], [], () => {
        if (refusals.length) return;
        assert.equal(abi.ge_entity_create(), kInvalidEntity);
        refusals.push(['ge_entity_create', lastError()]);
        assert.equal(abi.ge_entity_destroy(e), kFailed);
        refusals.push(['ge_entity_destroy', lastError()]);
        assert.equal(abi.ge_component_add(e, light.typeId), kFailed);
        refusals.push(['ge_component_add', lastError()]);
        assert.equal(abi.ge_component_remove(e, transform.typeId), kFailed);
        refusals.push(['ge_component_remove', lastError()]);
        assert.equal(abi.ge_entity_parent(e, kInvalidEntity), kFailed);
        refusals.push(['ge_entity_parent', lastError()]);
        assert.equal(abi.ge_instantiate_model(1, kInvalidEntity), kInvalidEntity);
        refusals.push(['ge_instantiate_model', lastError()]);
        assert.equal(abi.ge_tick(), kFailed);
        refusals.push(['ge_tick', lastError()]);
        assert.equal(abi.ge_update_assets(), kFailed);
        refusals.push(['ge_update_assets', lastError()]);
        // A resize moves no chunk: it is not refused for the query (here it fails for want of a
        // ge_create, as it does outside a query).
        assert.equal(abi.ge_resize(640, 480, 1), kFailed);
        resizeError = lastError();
    });
    assert.equal(refusals.length, 8, 'the query visited no chunk');
    for (const [call, message] of refusals)
        assert.match(message, new RegExp(`^${call} was called while a query is walking the world's chunks`));
    assert.doesNotMatch(resizeError, /while a query is walking/);
    // The query ended with its last chunk: the world changes again.
    assert.equal(abi.ge_component_add(e, light.typeId), kOk, lastError());

    // A query ended early (a page callback that threw) frees the world as well, and no query
    // starts inside another.
    const ids = core._malloc(16);
    const columns = core._malloc(8);
    const count = core._malloc(4);
    try {
        new BigUint64Array(core.HEAPU8.buffer, ids, 1)[0] = transform.typeId;
        assert.equal(abi.ge_query_begin(ids, 1, 0, 0, columns), kOk, lastError());
        assert.equal(abi.ge_query_next_chunk(columns, count), 1, lastError());
        assert.equal(abi.ge_query_begin(ids, 1, 0, 0, columns), kFailed);
        assert.match(lastError(), /^ge_query_begin: another query is still running/);
        assert.equal(abi.ge_query_end(), kOk);
        assert.equal(abi.ge_entity_destroy(e), kOk, lastError());
        assert.equal(abi.ge_query_next_chunk(columns, count), kFailed);
        assert.match(lastError(), /^ge_query_next_chunk was called with no query running/);
        new BigUint64Array(core.HEAPU8.buffer, ids, 2).set([transform.typeId, transform.typeId]);
        assert.equal(abi.ge_query_begin(ids, 1, ids + 8, 1, columns), kFailed);
        assert.match(lastError(), /^ge_query_begin: a component is listed twice/);
        new BigUint64Array(core.HEAPU8.buffer, ids, 1)[0] = 12345n;
        assert.equal(abi.ge_query_begin(ids, 1, 0, 0, columns), kFailed);
        assert.match(lastError(), /^ge_query_begin: 12345 is not a component this engine reflects/);
        // Counts whose uint32 sum wraps to a small number are refused before any id is read.
        assert.equal(abi.ge_query_begin(ids, 0xffffffff, ids, 2, columns), kFailed);
        assert.match(lastError(), /^ge_query_begin takes 1 to 16 components in all .*\(got 4294967295 read and 2 written\)/);
    } finally {
        for (const ptr of [ids, columns, count]) core._free(ptr);
    }
});

test('a frame\'s query over the same components allocates nothing in the engine once it has run', () => {
    boot();
    const transform = componentNamed(reflection(), 'Transform');
    const entities = Array.from({ length: 300 }, () => abi.ge_entity_create());
    runQuery([], [transform.typeId], () => {});
    const before = core._smoke_main_thread_mallocs();
    for (let frame = 0; frame < 20; ++frame) runQuery([], [transform.typeId], () => {});
    assert.equal(core._smoke_main_thread_mallocs() - before, 0, 'mallocs over 20 queries');
    for (const e of entities) abi.ge_entity_destroy(e);
});

// ---- loads ------------------------------------------------------------------------------

function fetchLanded(id) {
    const entry = core.geUrlFetches?.pending[id];
    return !entry || entry.done;
}

test('a load moves only inside ge_update_assets: status reads change nothing, frames or not', async () => {
    boot();
    const handle = withString(`${serverUrl}/model.glb`, (url) => abi.ge_load_asset(url));
    assert.notEqual(handle, 0, lastError());
    const fetchId = Math.max(...Object.keys(core.geUrlFetches.pending).map(Number));
    const fetchStart = performance.now();
    while (!fetchLanded(fetchId)) {
        assert.equal(abi.ge_asset_status(handle), AssetStatus.Loading);
        await sleep(5);
    }
    const fetchMs = performance.now() - fetchStart;
    // Longer than the decode takes on either build (report.load.longestUpdateMs): a load
    // that could move on a read or between calls has moved by the end of this.
    const readUntil = performance.now() + 1000;
    while (performance.now() < readUntil) {
        assert.equal(abi.ge_asset_status(handle), AssetStatus.Loading, 'a status read or the time between calls moved the load');
        await sleep(20);
    }

    const updates = [];
    let status = AssetStatus.Loading;
    const deadline = performance.now() + 60_000;
    while (status === AssetStatus.Loading && performance.now() < deadline) {
        const start = performance.now();
        assert.equal(abi.ge_update_assets(), kOk, lastError());
        updates.push(performance.now() - start);
        status = abi.ge_asset_status(handle);
        for (let read = 0; read < 10; ++read) assert.equal(abi.ge_asset_status(handle), status, 'a status read moved the load');
        if (status === AssetStatus.Loading) await sleep(5);
    }
    assert.equal(status, AssetStatus.Ready, lastError());
    report.load = {
        modelBytes: fs.statSync(modelPath).size,
        fetchMs: Math.round(fetchMs),
        updateCalls: updates.length,
        longestUpdateMs: Math.round(Math.max(...updates)),
        firstUpdateMs: Math.round(updates[0]),
    };

    assert.equal(abi.ge_instantiate_model(handle, kInvalidEntity), kInvalidEntity);
    assert.match(lastError(), /^ge_instantiate_model needs the renderer that ge_create brings up/);
});

test('a URL loads once: loading it again returns the same load, with no fetch and no rewrite', async () => {
    boot();
    const url = `${serverUrl}/repeat/model.glb`;
    const first = withString(url, (ptr) => abi.ge_load_asset(ptr));
    assert.notEqual(first, 0, lastError());
    let status = AssetStatus.Loading;
    const deadline = performance.now() + 60_000;
    while (status === AssetStatus.Loading && performance.now() < deadline) {
        await sleep(5);
        assert.equal(abi.ge_update_assets(), kOk, lastError());
        status = abi.ge_asset_status(first);
    }
    assert.equal(status, AssetStatus.Ready, lastError());

    // Rewriting the file would let a file watcher reload the asset under the instances that
    // bind its textures; the second load is the first one.
    const again = withString(url, (ptr) => abi.ge_load_asset(ptr));
    assert.equal(again, first);
    assert.equal(abi.ge_asset_status(again), AssetStatus.Ready);
    await sleep(50);
    assert.equal(abi.ge_update_assets(), kOk, lastError());
    assert.equal(served.get('/repeat/model.glb'), 1, 'the URL was fetched again');
});

test('a file the page opened loads through its blob: URL, named by the fragment', async () => {
    boot();
    const blobUrl = URL.createObjectURL(new Blob([fs.readFileSync(modelPath)]));
    assert.equal(withString(blobUrl, (url) => abi.ge_load_asset(url)), 0, 'a blob: URL without a name is refused');
    assert.match(lastError(), /a blob: URL carries the file's name after '#'/);
    const handle = withString(`${blobUrl}#local model.glb`, (url) => abi.ge_load_asset(url));
    assert.notEqual(handle, 0, lastError());
    let status = AssetStatus.Loading;
    const deadline = performance.now() + 60_000;
    while (status === AssetStatus.Loading && performance.now() < deadline) {
        assert.equal(abi.ge_update_assets(), kOk, lastError());
        status = abi.ge_asset_status(handle);
        if (status === AssetStatus.Loading) await sleep(5);
    }
    assert.equal(status, AssetStatus.Ready, lastError());
    URL.revokeObjectURL(blobUrl);
});

test('a load that cannot start or cannot fetch reports why', async () => {
    boot();
    assert.equal(withString('models/robot.glb', (url) => abi.ge_load_asset(url)), 0);
    assert.match(lastError(), /^ge_load_asset needs an absolute URL/);
    assert.equal(withString(`${serverUrl}/model`, (url) => abi.ge_load_asset(url)), 0);
    assert.match(lastError(), /must end in a file name with its extension/);

    const handle = withString(`${serverUrl}/missing.glb`, (url) => abi.ge_load_asset(url));
    assert.notEqual(handle, 0);
    let status = AssetStatus.Loading;
    for (let attempt = 0; attempt < 200 && status === AssetStatus.Loading; ++attempt) {
        await sleep(5);
        assert.equal(abi.ge_update_assets(), kOk);
        status = abi.ge_asset_status(handle);
    }
    assert.equal(status, AssetStatus.Failed);
    assert.match(lastError(), /^ge_asset_status: could not fetch '.*missing\.glb': HTTP 404/);
    assert.equal(abi.ge_asset_status(999999), kFailed);
    assert.match(lastError(), /^ge_asset_status: no load has handle 999999/);
});

// Loads `url` to its end through ge_update_assets; its last status.
async function loadToTheEnd(url) {
    const handle = withString(url, (ptr) => abi.ge_load_asset(ptr));
    assert.notEqual(handle, 0, lastError());
    let status = AssetStatus.Loading;
    const deadline = performance.now() + 60_000;
    while (status === AssetStatus.Loading && performance.now() < deadline) {
        await sleep(5);
        assert.equal(abi.ge_update_assets(), kOk, lastError());
        status = abi.ge_asset_status(handle);
    }
    return status;
}

test('a .gltf whose buffer file is not beside it fails its load, and the next load succeeds', async () => {
    boot();
    assert.equal(await loadToTheEnd(`${serverUrl}/triangle.gltf`), AssetStatus.Failed, 'the load never failed');
    assert.match(lastError(), /^ge_asset_status: the engine could not load '.*triangle\.gltf'/);
    assert.equal(await loadToTheEnd(`${serverUrl}/after-gltf/model.glb`), AssetStatus.Ready, lastError());
});

test('an audio file that does not decode fails its load, and the next load succeeds', async () => {
    boot();
    assert.equal(await loadToTheEnd(`${serverUrl}/corrupt.wav`), AssetStatus.Failed, 'the load never failed');
    assert.match(lastError(), /^ge_asset_status: the engine could not load '.*corrupt\.wav'/);
    assert.equal(await loadToTheEnd(`${serverUrl}/after-wav/model.glb`), AssetStatus.Ready, lastError());
});

// ---- the facade over the real module ----------------------------------------------------

test('the facade over the module: a second scene.load waits for the first to settle', async () => {
    boot();
    const facade = path.join(repoRoot, 'Apps', 'WebLibrary', 'ts', 'dist');
    const { createEngine } = await import(pathToFileURL(path.join(facade, 'src', 'engine.js')).href);
    const { FakeHost, fakeCanvas, settle } = await import(pathToFileURL(path.join(facade, 'test', 'harness.js')).href);

    const calls = [];
    const facadeAbi = {
        get HEAPU8() { return core.HEAPU8; },
        _malloc: abi._malloc,
        _free: abi._free,
        // The two calls that need a page: the engine is already up from the headless boot,
        // and the canvas is a stand-in.
        ge_create: async () => kOk,
        ge_resize: () => kOk,
    };
    for (const name of ['ge_tick', 'ge_update_assets', 'ge_shutdown', 'ge_load_asset', 'ge_asset_status', 'ge_instantiate_model',
        'ge_entity_create', 'ge_entity_bounds', 'ge_entity_destroy', 'ge_entity_parent', 'ge_component_add', 'ge_component_remove',
        'ge_component_has', 'ge_field_get', 'ge_field_set', 'ge_reflection_json', 'ge_last_error']) {
        facadeAbi[name] = (...args) => {
            calls.push(name);
            return abi[name](...args);
        };
    }
    class LocalHost extends FakeHost {
        resolveUrl(url) { return new URL(url, `${serverUrl}/`).href; }
    }
    const host = new LocalHost();
    const engine = await createEngine({ canvas: fakeCanvas() }, host, async () => facadeAbi, { version: '0.0.0', fingerprint: null });

    const first = engine.scene.load('model.glb');
    const second = engine.scene.load('missing.glb');
    const outcomes = [];
    first.then(() => outcomes.push('first resolved'), (error) => outcomes.push(`first rejected: ${error.message}`));
    second.then(() => outcomes.push('second resolved'), (error) => outcomes.push(`second rejected: ${error.message}`));

    for (let step = 0; step < 4000 && outcomes.length < 2; ++step) {
        await settle();
        if (outcomes.length === 0) assert.equal(calls.filter((c) => c === 'ge_load_asset').length, 1, 'the second load started before the first settled');
        host.fireFrames(step * 16);
    }
    assert.equal(outcomes.length, 2, `the loads never settled: ${outcomes.join('; ')}`);
    assert.match(outcomes[0], /^first rejected: .*ge_instantiate_model needs the renderer/);
    assert.match(outcomes[1], /^second rejected: .*could not fetch '.*missing\.glb': HTTP 404/);
    const loads = calls.map((c, i) => [c, i]).filter(([c]) => c === 'ge_load_asset').map(([, i]) => i);
    const instantiate = calls.indexOf('ge_instantiate_model');
    assert.equal(loads.length, 2);
    assert.ok(instantiate > loads[0] && instantiate < loads[1], 'the second ge_load_asset ran before the first load settled');
});
