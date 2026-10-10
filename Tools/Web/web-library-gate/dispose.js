// The browser gate's dispose check, staged beside the model-viewer example by
// Tools/Web/stage_web_library.py: the engine runs a few seconds, a raw engine call made while
// engine.dispose() is in flight and one made after it is awaited must both be refused, and the
// page logs one "dispose-check: PASS|FAIL {...}" line that browser_gate.py --expect reads.
// ?threads=single|auto picks the build, as Engine.create's option does.
import { Engine } from '@openengine/web';

const kInvalidEntity = 0xffffffff;
const kRunSeconds = 3;

function ReadString(raw, ptr) {
    const heap = raw.HEAPU8;
    let end = ptr;
    while (heap[end] !== 0) ++end;
    return new TextDecoder().decode(heap.slice(ptr, end));
}

// A raw ge_entity_create, past the facade's own refusal: what the module itself answers.
function RawCreate(raw) {
    const entity = raw.ge_entity_create();
    return { entity, error: entity === kInvalidEntity ? ReadString(raw, raw.ge_last_error()) : '' };
}

const threads = new URLSearchParams(location.search).get('threads') ?? 'auto';
const canvas = document.querySelector('canvas');
const engine = await Engine.create({ canvas, threads });
engine.run();
await new Promise((resolve) => setTimeout(resolve, kRunSeconds * 1000));

// The facade refuses every call once dispose starts; the module's answer is only visible below it.
const raw = engine.bridge.m_Raw;
const started = performance.now();
const disposing = engine.dispose();
const during = RawCreate(raw);
await disposing;
const disposeMs = performance.now() - started;
// The wall clock, as the engine's log lines stamp it: the threaded build's log reaches the console
// from a drain thread, later than it was written, so only the stamps order the two.
const now = new Date();
const resolvedAt = `${now.toTimeString().slice(0, 8)}.${String(now.getMilliseconds()).padStart(3, '0')}`;
const after = RawCreate(raw);

const passed = during.entity === kInvalidEntity && after.entity === kInvalidEntity &&
    /after the engine shut down/.test(after.error);
console.log(`dispose-check: ${passed ? 'PASS' : 'FAIL'} ${JSON.stringify({ threads, disposeMs, resolvedAt, during, after })}`);
