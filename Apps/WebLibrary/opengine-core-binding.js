// The binding the facade imports to obtain the engine module's ABI (ts/src/abi.ts,
// CoreBindingModule): it instantiates opengine-core.<build>.js from `coreUrl` on the page's
// canvas and adapts the module's exports to the Abi interface. Served beside the glue files.

// A wasm export's 32-bit result reaches JavaScript signed. These return unsigned values
// (entity ids, asset handles, pointers), so their results are read back as unsigned:
// kInvalidEntity is 0xffffffff, not -1.
const kUnsignedExports = [
    'ge_load_asset', 'ge_instantiate_model', 'ge_entity_create', 'ge_reflection_json', 'ge_last_error',
    'ge_animation_clips',
];
const kSignedExports = [
    'ge_tick', 'ge_update_assets', 'ge_resize', 'ge_asset_status', 'ge_model_emissive_strength',
    'ge_entity_bounds', 'ge_entity_destroy', 'ge_entity_parent', 'ge_entity_children', 'ge_entity_set_mesh',
    'ge_component_add', 'ge_component_remove', 'ge_component_has',
    'ge_field_get', 'ge_field_set', 'ge_query_begin', 'ge_query_next_chunk', 'ge_query_end',
    'ge_animation_play', 'ge_animation_pause',
];

/** Adapts an instantiated engine module to the Abi interface. */
export function adaptModule(module) {
    const abi = {
        // A heap growth replaces the buffer: read the view at every access.
        get HEAPU8() { return module.HEAPU8; },
        _malloc: (size) => module._malloc(size) >>> 0,
        _free: module._free,
        // The exports that suspend: ccall drives ASYNCIFY's unwind and rewind and resolves with
        // the call's return value once the call has really returned.
        ge_create: (canvasSelector, flags) =>
            module.ccall('ge_create', 'number', ['number', 'number'], [canvasSelector, flags], { async: true }),
        ge_shutdown: () => module.ccall('ge_shutdown', 'number', [], [], { async: true }),
    };
    for (const name of kSignedExports) abi[name] = module[`_${name}`];
    for (const name of kUnsignedExports) {
        const exported = module[`_${name}`];
        abi[name] = (...args) => exported(...args) >>> 0;
    }
    return abi;
}

/**
 * FNV-1a 64 of `bytes` continued from `hash` (four 16-bit limbs, lowest first), as
 * Tools/Web/gepak.py computes it: 16-bit limbs keep every product inside a double's integer range.
 */
function fnv1a64(bytes, hash) {
    let [h0, h1, h2, h3] = hash;
    for (let i = 0; i < bytes.length; ++i) {
        h0 ^= bytes[i];
        // h * 0x100000001b3 = h * 0x1b3 + (h << 40), modulo 2^64.
        const t0 = h0 * 0x1b3;
        const t1 = h1 * 0x1b3 + (t0 >>> 16);
        const t2 = h2 * 0x1b3 + (t1 >>> 16) + ((h0 << 8) & 0xffff);
        const t3 = h3 * 0x1b3 + (t2 >>> 16) + (h0 >>> 8) + ((h1 << 8) & 0xffff);
        h0 = t0 & 0xffff;
        h1 = t1 & 0xffff;
        h2 = t2 & 0xffff;
        h3 = t3 & 0xffff;
    }
    return [h0, h1, h2, h3];
}

const kFnvOffsetBasis = [0x2325, 0x8422, 0x9ce4, 0xcbf2];   // 14695981039346656037

function fnvHex(hash) {
    return '0x' + [...hash].reverse().map((limb) => limb.toString(16).padStart(4, '0')).join('');
}

/** Hands a phase's responses back as they are: the facade passes a tracker that reports progress. */
const untracked = (phase, files) => files.map((file) => file.response);

/**
 * The engine pack's parts, when the folder serves it as parts (a host that refuses large files):
 * the index beside the glue, opengine-core.gepak.parts.json, lists them in order with each part's
 * size, and the whole pack's size and FNV-1a 64. Resolves once every part's response has
 * started, to the index and the responses; null when the folder serves the pack whole.
 */
async function requestPackParts(base) {
    const response = await fetch(new URL('opengine-core.gepak.parts.json', base));
    if (!response.ok)
        return null;
    const index = await response.json();
    const responses = await Promise.all(index.parts.map(async ({ file }) => {
        const part = await fetch(new URL(file, base));
        if (!part.ok)
            throw new Error(`Could not fetch ${file} of the engine pack from ${base}: HTTP ${part.status}.`);
        return part;
    }));
    return { index, responses };
}

/**
 * Reads the parts, checks every one against the index, and joins them into one Blob whose object
 * URL stands in for opengine-core.gepak.
 */
async function joinPackParts({ parts, bytes, fnv1a64: expected }, responses) {
    const contents = await Promise.all(parts.map(async ({ file, bytes: size }, i) => {
        const content = new Uint8Array(await responses[i].arrayBuffer());
        if (content.length !== size)
            throw new Error(`${file} of the engine pack is ${content.length} bytes; the index says ${size}. Serve the parts the index was written with.`);
        return content;
    }));
    const total = contents.reduce((sum, content) => sum + content.length, 0);
    let hash = kFnvOffsetBasis;
    for (const content of contents)
        hash = fnv1a64(content, hash);
    if (total !== bytes || fnvHex(hash) !== expected)
        throw new Error(`The engine pack's parts join into ${total} bytes with FNV-1a ${fnvHex(hash)}; the index says ${bytes} bytes and ${expected}. A part is stale or damaged.`);
    return URL.createObjectURL(new Blob(contents));
}

/** A phase's tracked files for the parts' responses: each part's size is its index entry's. */
function trackPackParts({ index, responses }, track) {
    return track('pack', responses.map((response, i) => ({ response, decodedBytes: index.parts[i].bytes })));
}

/**
 * The engine pack joined from its parts (requestPackParts), its download reported through
 * `track` in the 'pack' phase; null when the folder serves the pack whole.
 */
export async function joinEnginePack(base, track = untracked) {
    const parts = await requestPackParts(base);
    return parts && joinPackParts(parts.index, trackPackParts(parts, track));
}

/** Compiles the module from `response` as it downloads. */
function instantiateStreaming(response, imports) {
    // The server's content type may not be application/wasm, which streaming compilation needs.
    return WebAssembly.instantiateStreaming(new Response(response.body, { headers: { 'Content-Type': 'application/wasm' } }), imports);
}

/**
 * Loads the `build` ('st' or 'mt') engine module from `coreUrl` on `canvas`. The module and the
 * pack's parts download side by side, reported through `track` in the 'wasm' and 'pack' phases
 * once both have started; `wasmBytes` is the wasm file's size, which counts a compressed
 * response in its compressed bytes.
 */
export async function loadCore({ build, coreUrl, canvas, wasmBytes, track = untracked }) {
    const base = new URL(coreUrl, globalThis.location?.href);
    const wasmFile = `opengine-core.${build}.wasm`;
    const [wasmResponse, parts] = await Promise.all([
        fetch(new URL(wasmFile, base), { credentials: 'same-origin' }),
        requestPackParts(base),
    ]);
    if (!wasmResponse.ok)
        throw new Error(`Could not fetch ${wasmFile} from ${base}: HTTP ${wasmResponse.status}.`);
    const packResponses = parts && trackPackParts(parts, track);
    const [wasm] = track('wasm', [{ response: wasmResponse, decodedBytes: wasmBytes }]);
    const glue = new URL(`opengine-core.${build}.js`, base).href;
    const { default: createOpenEngine } = await import(glue);
    // The glue waits on instantiateWasm's callback alone: a failed compile rejects through here.
    let failInstantiation;
    const instantiationFailed = new Promise((_, reject) => { failInstantiation = reject; });
    let packUrl = null;
    const [module, joined] = await Promise.all([
        Promise.race([instantiationFailed, createOpenEngine({
            canvas,
            // The engine pack and the threaded build's worker script sit beside the glue, wherever
            // the page itself is served from; a pack served as parts is the joined Blob, which
            // the module fetches in ge_create.
            locateFile: (path) => (path === 'opengine-core.gepak' && packUrl) || new URL(path, base).href,
            instantiateWasm(imports, receive) {
                instantiateStreaming(wasm, imports).then(({ instance, module: compiled }) => receive(instance, compiled), failInstantiation);
                return {};
            },
        })]),
        parts && joinPackParts(parts.index, packResponses),
    ]);
    packUrl = joined;
    const abi = adaptModule(module);
    if (!packUrl)
        return abi;
    // ge_create fetches the pack; once it has returned the joined Blob is no longer needed.
    const create = abi.ge_create;
    abi.ge_create = async (...args) => {
        try {
            return await create(...args);
        } finally {
            URL.revokeObjectURL(packUrl);
        }
    };
    return abi;
}
