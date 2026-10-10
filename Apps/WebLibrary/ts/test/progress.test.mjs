// Download progress: Engine.create's engine pack parts and wasm module (the binding's loadCore
// with the facade's tracker) and scene.load's model, in bytes over the network, never
// decreasing, ending at their totals. The files come from a local server; a gzip response counts
// its compressed bytes.
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import http from 'node:http';
import { register } from 'node:module';
import test from 'node:test';
import zlib from 'node:zlib';
import { loadCore } from '../../opengine-core-binding.js';
import { downloadTracker } from '../dist/src/progress.js';
import { settle, startEngine } from '../dist/test/harness.js';

// loadCore imports the glue from the served folder: node imports http: modules through this hook.
register('data:text/javascript,' + encodeURIComponent(`
export async function resolve(specifier, context, next) {
    return specifier.startsWith('http:') ? { url: specifier, shortCircuit: true } : next(specifier, context);
}
export async function load(url, context, next) {
    if (!url.startsWith('http:')) return next(url, context);
    return { format: 'module', source: await (await fetch(url)).text(), shortCircuit: true };
}`));

/**
 * Serves `files` (path to { body, gzip, chunked }) on a local port; resolves to its origin and
 * closer. A chunked file is sent without Content-Length, as a host that compresses on the fly does.
 */
function serve(files) {
    const server = http.createServer((request, response) => {
        const file = files.get(new URL(request.url, 'http://host').pathname);
        if (!file) return response.writeHead(404).end();
        const body = file.gzip ? zlib.gzipSync(file.body) : file.body;
        response.writeHead(200, { ...(file.chunked ? {} : { 'Content-Length': body.length }), ...(file.gzip ? { 'Content-Encoding': 'gzip' } : {}) });
        // Several writes, so the reader sees the body arrive in pieces.
        for (let at = 0; at < body.length; at += 64 * 1024) response.write(body.subarray(at, at + 64 * 1024));
        response.end();
    });
    return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve({
        origin: `http://127.0.0.1:${server.address().port}`,
        close: () => new Promise((done) => server.close(done)),
    })));
}

/** A valid empty wasm module carrying `size` bytes in one custom section. */
function wasmOf(size) {
    const payload = Buffer.concat([Buffer.from([1, 0x78]), Buffer.alloc(size, 7)]);
    const leb = [];
    for (let n = payload.length; ; n >>>= 7) {
        if (n < 0x80) { leb.push(n); break; }
        leb.push((n & 0x7f) | 0x80);
    }
    return Buffer.concat([Buffer.from([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 0, ...leb]), payload]);
}

// A glue that instantiates through the page's instantiateWasm, as the engine module's glue does.
const kGlue = `export default function create(Module) {
    return new Promise((resolve) => Module.instantiateWasm({}, (instance) => resolve({ instance, locateFile: Module.locateFile })));
}`;

function fnv1a64(bytes) {
    let hash = 0xcbf29ce484222325n;
    for (const byte of bytes) hash = ((hash ^ BigInt(byte)) * 0x100000001b3n) & 0xffffffffffffffffn;
    return `0x${hash.toString(16).padStart(16, '0')}`;
}

/** Each phase's reports: `loaded` never decreases, the total never changes, the last is complete. */
function assertProgress(events, phase, total) {
    const reports = events.filter((event) => event.phase === phase);
    assert.ok(reports.length > 2, `${phase}: ${reports.length} reports`);
    assert.deepEqual(reports[0], { phase, loaded: 0, total });
    for (let i = 1; i < reports.length; ++i) {
        assert.ok(reports[i].loaded >= reports[i - 1].loaded, `${phase} went from ${reports[i - 1].loaded} to ${reports[i].loaded}`);
        assert.equal(reports[i].total, total);
    }
    assert.equal(reports.at(-1).loaded, total);
}

/** The engine's files: two pack parts and the wasm, served as `serving` says (path to options). */
function engineFiles(serving) {
    const parts = [Buffer.alloc(900_000, 1), crypto.randomBytes(300_000)];
    const whole = Buffer.concat(parts);
    const wasm = wasmOf(700_000);
    const index = { file: 'opengine-core.gepak', bytes: whole.length, fnv1a64: fnv1a64(whole),
        parts: parts.map((bytes, i) => ({ file: `opengine-core.gepak.part${i}`, bytes: bytes.length })) };
    const files = new Map([
        ['/core/opengine-core.gepak.parts.json', { body: Buffer.from(JSON.stringify(index)) }],
        ['/core/opengine-core.gepak.part0', { body: parts[0], ...serving.part0 }],
        ['/core/opengine-core.gepak.part1', { body: parts[1], ...serving.part1 }],
        ['/core/opengine-core.st.wasm', { body: wasm, ...serving.wasm }],
        ['/core/opengine-core.st.js', { body: Buffer.from(kGlue) }],
    ]);
    return { parts, wasm, files };
}

/** loadCore over `files`, with every progress report. */
async function loadEngine(files, wasmBytes) {
    const site = await serve(files);
    try {
        const events = [];
        const abi = await loadCore({ build: 'st', coreUrl: `${site.origin}/core/`, canvas: {}, wasmBytes,
            track: downloadTracker((event) => events.push(event)) });
        assert.ok(abi.ge_create, 'the module was adapted');
        return events;
    } finally {
        await site.close();
    }
}

test('Engine.create reports the pack parts and the wasm module from the start, in network bytes', async () => {
    const { parts, wasm, files } = engineFiles({ part0: { gzip: true }, wasm: { gzip: true } });
    const events = await loadEngine(files, wasm.length);
    // Both phases report their totals before either reports bytes.
    assert.deepEqual(events.slice(0, 2).map((event) => [event.phase, event.loaded]), [['pack', 0], ['wasm', 0]]);
    assertProgress(events, 'pack', zlib.gzipSync(parts[0]).length + parts[1].length);
    assertProgress(events, 'wasm', zlib.gzipSync(wasm).length);
});

test('a compressing host without Content-Length reports the decompressed bytes of the known sizes', async () => {
    const chunked = { gzip: true, chunked: true };
    const { parts, wasm, files } = engineFiles({ part0: chunked, part1: chunked, wasm: chunked });
    const events = await loadEngine(files, wasm.length);
    assertProgress(events, 'pack', parts[0].length + parts[1].length);
    assertProgress(events, 'wasm', wasm.length);
});

/** scene.load of `url` with `onProgress`, firing frames until it settles. */
async function loadModel(engine, host, url, onProgress) {
    const loading = engine.scene.load(url, { onProgress });
    let done = false;
    loading.then(() => { done = true; }, () => { done = true; });
    for (let frame = 1; !done && frame < 400; ++frame) {
        await new Promise((resolve) => setTimeout(resolve, 5));
        host.fireFrames(frame * 16);
    }
    await settle();
    return loading;
}

test('scene.load reports the model download, and a URL loaded again downloads nothing', async () => {
    const model = crypto.randomBytes(500_000);
    const site = await serve(new Map([['/models/robot.glb', { body: model }]]));
    try {
        const { abi, host, engine } = await startEngine();
        const load = (onProgress) => loadModel(engine, host, `${site.origin}/models/robot.glb`, onProgress);
        const events = [];
        await load((event) => events.push(event));
        assertProgress(events, 'model', model.length);
        const handed = abi.calls.filter((call) => call.startsWith('ge_load_asset('));
        assert.match(handed[0], /^ge_load_asset\(blob:.*#robot\.glb\)$/);
        // The engine holds the bytes once the load is done: the download's object URL is released.
        await assert.rejects(fetch(handed[0].slice('ge_load_asset('.length, handed[0].indexOf('#'))));

        const again = [];
        await load((event) => again.push(event));
        assert.deepEqual(again, []);
        assert.equal(abi.calls.filter((call) => call.startsWith('ge_load_asset(')).at(-1), handed[0]);
    } finally {
        await site.close();
    }
});

test('a model the engine refuses releases its download', async () => {
    const site = await serve(new Map([['/models/robot.glb', { body: crypto.randomBytes(100_000) }]]));
    try {
        const { abi, host, engine } = await startEngine();
        abi.refuseLoads = true;
        await assert.rejects(loadModel(engine, host, `${site.origin}/models/robot.glb`, () => {}), { message: /the URL was refused/ });
        const handed = abi.calls.find((call) => call.startsWith('ge_load_asset('));
        await assert.rejects(fetch(handed.slice('ge_load_asset('.length, handed.indexOf('#'))));
    } finally {
        await site.close();
    }
});

test('a compressed model of unknown size has no total until it has arrived', async () => {
    const model = crypto.randomBytes(400_000);
    const site = await serve(new Map([['/models/robot.glb', { body: model, gzip: true }]]));
    try {
        const { host, engine } = await startEngine();
        const events = [];
        await loadModel(engine, host, `${site.origin}/models/robot.glb`, (event) => events.push(event));
        assert.ok(events.length > 2, `${events.length} reports`);
        assert.deepEqual(events.slice(0, -1).map((event) => event.total), events.slice(0, -1).map(() => 0));
        for (let i = 1; i < events.length; ++i) assert.ok(events[i].loaded >= events[i - 1].loaded);
        assert.deepEqual(events.at(-1), { phase: 'model', loaded: model.length, total: model.length });
    } finally {
        await site.close();
    }
});
