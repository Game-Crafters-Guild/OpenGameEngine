// Per-pass GPU timings from get_gpu_profiler, reduced over a capture window.
//
// Under encoder-span timing (Metal) every pass recorded into one encoder
// reports that encoder's whole span (spanShared), so adding gpuSpanMs over a
// group of passes counts one measurement once per pass. The render graph's
// fold marks exactly one pass per measurement spanCounted: the same decision
// behind resolveStats.distinctSpanGpuMs and the Editor's Visual Profiler total.
// A group total therefore adds only the counted passes. A shared encoder that
// carries passes of two groups is charged to the group of its counted pass, so
// per frame the group totals never add up to more than distinctSpanGpuMs. Over
// a capture window that holds only when every pass is in every sample, since
// each pass is averaged over the samples that carry it. Under pipeline-point
// timing (Vulkan) every pass is counted and nothing changes.

// samples: [{ gpu: <get_gpu_profiler response> }, ...]
// Returns [{ name, gpuSpanMs, countedGpuMs }] sorted by gpuSpanMs, each the
// mean over the samples that carried the pass. countedGpuMs is the part of the
// pass's span that is not already counted on another pass.
export function reducePassTimings(samples) {
    const byName = new Map();
    for (const s of samples) {
        for (const p of s.gpu?.lastFrame?.passes ?? []) {
            if (typeof p.spanCounted !== 'boolean')
                throw new Error(`get_gpu_profiler pass '${p.name}' has no spanCounted; the editor build predates it`);
            const e = byName.get(p.name) ?? { span: 0, counted: 0, n: 0 };
            e.span += p.gpuSpanMs;
            if (p.spanCounted)
                e.counted += p.gpuSpanMs;
            e.n += 1;
            byName.set(p.name, e);
        }
    }
    return [...byName.entries()]
        .map(([name, e]) => ({ name, gpuSpanMs: e.span / e.n, countedGpuMs: e.counted / e.n }))
        .sort((x, y) => y.gpuSpanMs - x.gpuSpanMs);
}

// GPU ms of the passes whose name starts with prefix, each measurement once.
export function groupGpuMs(passMeans, prefix) {
    return passMeans
        .filter(p => p.name.startsWith(prefix))
        .reduce((sum, p) => sum + p.countedGpuMs, 0);
}
