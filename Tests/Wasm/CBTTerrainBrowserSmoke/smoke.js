// Runs the production cooked WGSL, not the native SPIR-V heap32 variant.
// The camera fixture matches CBTDecodeGoldenTests' screen-space conformity case.
// Before scalar neighbor storage, frame 10 loses concurrent edge updates on
// WebGPU/Metal while the Vulkan version of the same test passes.
// Resource sizes and push-constant offsets mirror CBTLayout.h / CBTInstance.cpp.
const output = document.querySelector("#result");
const log = (s) => {
  output.textContent += "\n" + s;
  console.log(s);
};
let device;
try {
  const screenMode =
    new URLSearchParams(location.search).get("mode") !== "depth";
  const adapter = await navigator.gpu.requestAdapter();
  log("Adapter ready.");
  device = await adapter.requestDevice({
    requiredFeatures: ["float32-filterable"],
    requiredLimits: { maxStorageBuffersPerShaderStage: 10 },
  });
  log("Device ready.");
  let gpuError;
  device.addEventListener("uncapturederror", (e) => {
    gpuError = e.error;
    log("GPU ERROR: " + e.error.message);
  });
  const poolSize = 1 << 20,
    bitfieldWords = poolSize / 32,
    sumTreeBlocks = bitfieldWords / 4,
    sumTreeWords = sumTreeBlocks + 2 * bitfieldWords - 1;
  const sizes = {
    0: poolSize * 8,
    1: poolSize * 16,
    2: poolSize * 16,
    3: poolSize * 32,
    4: bitfieldWords * 4,
    5: sumTreeWords * 4,
    6: (16 + poolSize * 3) * 4,
    7: 36,
    8: 60,
    9: poolSize * 4,
    10: poolSize * 4,
    11: poolSize * 4,
    12: poolSize * 96,
    13: 64,
    14: 1808 * 4,
    16: 4,
    17: 16,
    20: 4,
    21: 4,
  };
  const buffers = {};
  for (const [key, size] of Object.entries(sizes))
    buffers[key] = device.createBuffer({
      label: "CBT binding " + key,
      size,
      usage:
        (key === "14" ? GPUBufferUsage.UNIFORM : GPUBufferUsage.STORAGE) |
        GPUBufferUsage.COPY_SRC |
        GPUBufferUsage.COPY_DST |
        (key === "7" ? GPUBufferUsage.INDIRECT : 0),
    });
  const texture = device.createTexture({
    size: [1, 1],
    format: "r32float",
    usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
  });
  const sampler = device.createSampler({
    magFilter: "linear",
    minFilter: "linear",
  });
  const pipes = {},
    sets = {},
    pcLayouts = {},
    emptySets = {};
  await Promise.all(
    [...Array(17).keys(), 20, 21].map(async (k) => {
      const code = await (
        await fetch(`Shaders/cbt_kernels_${k}.comp.wgsl`)
      ).text();
      const module = device.createShaderModule({ code });
      const messages = await module.getCompilationInfo();
      if (messages.messages.some((x) => x.type === "error"))
        throw Error(JSON.stringify(messages.messages));
      const p = await device.createComputePipelineAsync({
        layout: "auto",
        compute: { module, entryPoint: "main" },
      });
      pipes[k] = p;
      const entries = [
        ...code.matchAll(
          /@group\(0\) @binding\((\d+)\)\s+var(?:<[^>]+>)?\s+\w+:\s*([^;]+);/g,
        ),
      ].map((m) => {
        const binding = Number(m[1]);
        const type = m[2];
        return {
          binding,
          resource:
            type === "sampler"
              ? sampler
              : type.startsWith("texture_")
                ? texture.createView()
                : { buffer: buffers[binding] },
        };
      });
      sets[k] = device.createBindGroup({
        layout: p.getBindGroupLayout(0),
        entries,
      });
      if (code.includes("@group(3)")) {
        pcLayouts[k] = p.getBindGroupLayout(3);
        emptySets[k] = [1, 2].map((n) =>
          device.createBindGroup({
            layout: p.getBindGroupLayout(n),
            entries: [],
          }),
        );
      }
    }),
  );
  log("Production pipelines ready.");
  const params = new Float32Array(1808);
  for (let f = 0; f < 4; f++) {
    params[f * 452 + 24] = 1;
    params[f * 452 + 25] = 1;
  }
  if (screenMode) {
    const eye = [0, 60, -320],
      length = Math.hypot(60, 320),
      sy = 60 / length,
      cz = 320 / length;
    const view = [1, 0, 0, 0, 0, cz, -sy, 0, 0, sy, cz, 0, 0, 0, length, 1];
    const f = 1 / Math.tan(1.0472 / 2),
      n = 0.5,
      z = 3000;
    const proj = [
      f / (16 / 9),
      0,
      0,
      0,
      0,
      f,
      0,
      0,
      0,
      0,
      n / (n - z),
      1,
      0,
      0,
      (n * z) / (z - n),
      0,
    ];
    const vp = new Float32Array(16);
    for (let c = 0; c < 4; c++)
      for (let r = 0; r < 4; r++)
        for (let k = 0; k < 4; k++)
          vp[c * 4 + r] += proj[k * 4 + r] * view[c * 4 + k];
    for (let slot = 0; slot < 4; slot++) {
      const b = slot * 452;
      params.set(vp, b);
      params.set([...eye, 1], b + 16);
      params.set([1600, 900, 24, 12], b + 20);
      params.set([500, 500, 0, 0], b + 24);
      params.set([-250, -250, 16, 0], b + 28);
    }
  }
  device.queue.writeBuffer(buffers[14], 0, params);
  device.queue.writeBuffer(buffers[0], 0, new Uint32Array([2, 0, 3, 0]));
  device.queue.writeBuffer(
    buffers[1],
    0,
    new Uint32Array([
      0xffffffff, 0xffffffff, 1, 0, 0xffffffff, 0xffffffff, 0, 0,
    ]),
  );
  device.queue.writeBuffer(
    buffers[3],
    0,
    new Uint32Array([0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0]),
  );
  device.queue.writeBuffer(buffers[4], 0, new Uint32Array([3]));
  device.queue.writeBuffer(buffers[9], 0, new Uint32Array([0, 1]));
  const pcBuffer = device.createBuffer({
    size: 65536,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
  });
  let serial = 0,
    encoder;
  function dispatch(k, pc, direct, slot = 0) {
    const pass = encoder.beginComputePass({ label: "kernel " + k });
    pass.setPipeline(pipes[k]);
    pass.setBindGroup(0, sets[k]);
    if (pcLayouts[k]) {
      const offset = serial++ * 256;
      device.queue.writeBuffer(pcBuffer, offset, pc);
      pass.setBindGroup(1, emptySets[k][0]);
      pass.setBindGroup(2, emptySets[k][1]);
      pass.setBindGroup(
        3,
        device.createBindGroup({
          layout: pcLayouts[k],
          entries: [
            { binding: 0, resource: { buffer: pcBuffer, offset, size: 256 } },
          ],
        }),
      );
    }
    if (direct !== null) pass.dispatchWorkgroups(direct);
    else pass.dispatchWorkgroupsIndirect(buffers[7], slot * 12);
    pass.end();
  }
  async function readSummary() {
    const rb = device.createBuffer({
      size: 188,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const e = device.createCommandEncoder();
    e.copyBufferToBuffer(buffers[13], 0, rb, 0, 64);
    e.copyBufferToBuffer(buffers[8], 0, rb, 64, 60);
    e.copyBufferToBuffer(buffers[6], 0, rb, 124, 64);
    device.queue.submit([e.finish()]);
    await rb.mapAsync(GPUMapMode.READ);
    const data = new Uint32Array(rb.getMappedRange().slice(0));
    rb.unmap();
    rb.destroy();
    return data;
  }
  const pc = new Uint32Array(21);
  pc[1] = 1;
  pc[2] = poolSize;
  pc[3] = poolSize;
  pc[4] = 1;
  pc[6] = screenMode ? 16 : 19;
  pc[8] = screenMode ? 1 : 0;
  pc[17] = 1;
  pc[18] = 1;
  encoder = device.createCommandEncoder();
  dispatch(10, pc, sumTreeBlocks / 64);
  dispatch(11, pc, bitfieldWords / 64);
  dispatch(12, pc, 1);
  device.queue.submit([encoder.finish()]);
  await device.queue.onSubmittedWorkDone();
  let live = 0,
    peakLive = 0,
    merges = 0;
  for (let frame = 0; frame < (screenMode ? 80 : 40); frame++) {
    if (screenMode && frame === 40) {
      // Relax screen-space detail to exercise Simplify and its scatter writes.
      for (let slot = 0; slot < 4; slot++) {
        params[slot * 452 + 22] = 96;
        params[slot * 452 + 23] = 48;
      }
      device.queue.writeBuffer(buffers[14], 0, params);
    }
    serial = 0;
    pc[4] = frame % 2 === 0 ? 1 : 0;
    pc[7] = frame;
    encoder = device.createCommandEncoder();
    const prep = (c) => {
      const p = pc.slice();
      p[0] = c;
      dispatch(2, p, 1);
    };
    // Reset, validate compact lists, then evaluate demand on the current mesh.
    dispatch(0, pc, 1);
    dispatch(21, pc, null, 1);
    if (frame === 0 || screenMode) dispatch(16, pc, null, 1);
    dispatch(1, pc, null);
    // Split -> allocate -> copy neighbors -> bisect -> propagate.
    prep(2);
    dispatch(3, pc, null);
    prep(4);
    dispatch(4, pc, null);
    dispatch(20, pc, null, 1);
    dispatch(5, pc, null);
    prep(5);
    dispatch(6, pc, null);
    // Prepare and apply merges, then repair their neighbor links.
    prep(3);
    dispatch(7, pc, null);
    prep(7);
    dispatch(8, pc, null);
    prep(6);
    dispatch(9, pc, null);
    // Rebuild the free-slot tree, compact indices, evaluate vertices and validate.
    dispatch(10, pc, sumTreeBlocks / 64);
    dispatch(11, pc, bitfieldWords / 64);
    dispatch(12, pc, 1);
    dispatch(13, pc, poolSize / 64);
    dispatch(14, pc, 1);
    dispatch(16, pc, null);
    dispatch(15, pc, poolSize / 64);
    device.queue.submit([encoder.finish()]);
    const v = await readSummary();
    live = v[16] / 3;
    peakLive = Math.max(peakLive, live);
    merges += v[38];
    log(
      `frame=${frame} live=${v[16] / 3} links=${v[0]} budget=${v[1]} zombie=${v[2]} compact=${v[3]} split=${v[33]} alloc=${v[35]} merge=${v[38]}`,
    );
    if (gpuError) throw gpuError;
    if (v[0] || v[1] || v[2] || v[3])
      throw Error("Topology invariant failed at frame " + frame);
  }
  if (gpuError) throw gpuError;
  if (peakLive < (screenMode ? 10000 : 200000))
    throw Error("Terrain did not refine: only " + live + " live triangles");
  if (screenMode && (merges === 0 || live >= peakLive))
    throw Error("Terrain did not coarsen after relaxing screen-space detail");
  log("PASS: CBT " + (screenMode ? "screen-space" : "depth") + " topology");
  document.documentElement.dataset.result = "pass";
} catch (e) {
  document.documentElement.dataset.result = "fail";
  log("FAIL: " + e.stack);
} finally {
  device?.destroy();
}
