// node --test Tools/Benchmarks/Rendering/perf-bench/*.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { reducePassTimings, groupGpuMs } from './gpu-pass-totals.mjs';

const pass = (name, gpuSpanMs, spanShared, spanCounted) => ({ name, gpuSpanMs, spanShared, spanCounted });
const sample = (passes, distinctSpanGpuMs) => ({ gpu: { lastFrame: { passes }, resolveStats: { distinctSpanGpuMs } } });

test('passes that share one encoder count its span once in their group', () => {
    // Encoder-span (Metal): three culling passes recorded into one compute
    // encoder each report its 0.6 ms span; the fold counts it on one of them.
    const samples = [
        sample([
            pass('GPUCulling.PrevVisibleInit', 0.6, true, true),
            pass('GPUCulling.PrevVisibleReset', 0.6, true, false),
            pass('GPUCulling.VisibilityUnion', 0.6, true, false),
            pass('GPUDrawStream.Bucketer', 0.25, false, true),
        ], 0.85),
    ];
    const means = reducePassTimings(samples);
    assert.equal(groupGpuMs(means, 'GPUCulling.'), 0.6);
    assert.equal(groupGpuMs(means, 'GPUDrawStream.'), 0.25);
    // Per-pass rows still show each pass's full (shared) span.
    assert.equal(means.find(p => p.name === 'GPUCulling.VisibilityUnion').gpuSpanMs, 0.6);
});

test('group totals average over the window and never exceed the distinct total', () => {
    const samples = [
        sample([pass('GPUCulling.A', 1.0, true, true), pass('GPUCulling.B', 1.0, true, false)], 1.0),
        sample([pass('GPUCulling.A', 2.0, true, false), pass('GPUCulling.B', 2.0, true, true)], 2.0),
    ];
    const means = reducePassTimings(samples);
    const culling = groupGpuMs(means, 'GPUCulling.');
    assert.equal(culling, 1.5);
    const meanDistinct = samples.reduce((x, s) => x + s.gpu.resolveStats.distinctSpanGpuMs, 0) / samples.length;
    assert.ok(culling <= meanDistinct);
});

test('an encoder shared across two groups is charged to its counted pass only', () => {
    const samples = [
        sample([pass('GPUCulling.Last', 0.4, true, true), pass('GPUDrawStream.First', 0.4, true, false)], 0.4),
    ];
    const means = reducePassTimings(samples);
    const both = groupGpuMs(means, 'GPUCulling.') + groupGpuMs(means, 'GPUDrawStream.');
    assert.equal(both, 0.4);
});

test('pipeline-point timings (every pass counted) sum as before', () => {
    const samples = [
        sample([pass('GPUCulling.A', 0.3, false, true), pass('GPUCulling.B', 0.2, false, true)], 0.5),
    ];
    assert.equal(groupGpuMs(reducePassTimings(samples), 'GPUCulling.'), 0.5);
});

test('a payload without spanCounted is refused, not silently totalled', () => {
    const samples = [sample([{ name: 'GPUCulling.A', gpuSpanMs: 0.3, spanShared: false }], 0.3)];
    assert.throws(() => reducePassTimings(samples), /spanCounted/);
});
