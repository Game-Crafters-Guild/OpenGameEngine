# GPU Debugging Guide

## Core principle: Start with data, not math

Most GPU rendering bugs are **data pipeline issues** — wrong buffer bound,
stale descriptor set, aliased memory, wrong resource — NOT math errors.
Before investigating matrix conventions, Y-flips, or shader logic, verify
the GPU is reading the correct data.

## General GPU debugging approach

### Step 1: Reproduce visually (5 min)

1. `take_screenshot` to see the current state.
2. `capture_resource <name> rangeMin rangeMax` to inspect render targets and
   depth buffers.
3. `get_log minLevel=error` to check for VUIDs.
4. `get_render_graph_validation` to check for resource conflicts.

### Step 2: Verify the GPU reads the right data (30 min)

**The definitive test for any "GPU ignores my data" bug:**
1. Write a KNOWN test value to the buffer/UBO you suspect is broken
   (e.g., zero out a matrix, write 0.42 to an unused float field).
2. Check if the GPU output changes.
3. If it does NOT change: **the buffer binding is broken**. Investigate
   descriptor sets, render graph buffer resolution, resource aliasing.
4. If it DOES change: the binding works, the issue is in the data itself.

### Step 3: Narrow the scope (15 min per hypothesis)

**Change ONE thing at a time.** When debugging interacting systems (matrix
conventions, coordinate flips, depth ranges), changing multiple variables
simultaneously creates a combinatorial explosion where each change masks
the others. Change one, test, revert if no improvement, try the next.

### Step 4: Use reference implementations

Before writing new GPU code, find a working example in the codebase:
- Render-graph passes: `Engine/Modules/Rendering/Source/Passes/` (`TonemapPass.cpp` is a compact one)
- Shadows: `Engine/Source/Engine/Rendering/ShadowMapRenderFeature.cpp`

Compare your code against the reference line-by-line rather than
reimplementing from scratch.

## Vulkan-specific pitfalls

### Negative viewport Y-flip
The engine uses `VK_KHR_maintenance1` negative viewport height for Y-up
rasterization. This means +clip.Y maps to texture V=0 (top). When sampling
a render target that was written with the negative viewport, the UV mapping
needs `uv.y = 1.0 - uv.y` to compensate. This applies to shadow maps,
reflection probes, or any render-to-texture that's sampled later.

### Render graph transient buffer aliasing
Upload (host-visible) transient buffers are excluded from aliasing by the
`IsAliasCompatible` guard in `RenderGraphCompiler.cpp`. Each Upload buffer
gets its own physical allocation — no workaround needed. For GPU-only
(DeviceLocal) buffers, aliasing is correct because the GPU processes passes
in order with barriers. If a DeviceLocal buffer produces stale results,
check the render graph trace output for unexpected aliasing groups.

### A target read before its first write (`GE_VK_FILL_NEW_TARGETS_NAN=1`)
New GPU memory is not zeroed. A pass that reads a render target, a storage
image or a history buffer before anything has written it shows whatever the
memory last held: zeros when the driver hands out fresh memory, or whatever a
resource the same session freed left there, NaN included, so the defect comes
and goes between sessions. With `GE_VK_FILL_NEW_TARGETS_NAN=1`
set before the editor or a test starts, every new float color target (render
target or storage, any sample count) is cleared to NaN when it is created and
every new storage buffer is filled with a word that reads NaN as a float or
as either half, so such a read shows NaN on every run. Render-graph
transients are created fresh each frame instead of recycled, so a transient
read before its first write shows NaN too rather than last frame's contents.
An integer storage buffer read before its first write reads 2143322048, which
can make an indirect dispatch or draw count huge. Read back the pass outputs (for example
with the editor's `capture_resource`) and the first target that holds NaN
names the pass that consumed an unwritten input. The switch is read once at
startup; off, it costs nothing per frame. A persistent history the pool keeps
under its name across frames keeps its contents, as it must. The NaN reaches
the reading pass only because the driver keeps the memory through the render
graph's first transition from the undefined layout, which the Vulkan
specification allows it to discard; this holds on NVIDIA drivers and has not
been verified on other vendors.

### Descriptor set binding validation
Vulkan silently ignores `BindDescriptorSet` if the pipeline doesn't declare
a matching descriptor set layout. Always verify the depth/compute pipeline
includes `descriptorSetLayouts` in its `PipelineDesc`.

### Shader variant keyword interaction
Depth-only passes should compile with minimal keywords (e.g., only `Instanced`)
to avoid pulling in Forward+ bindings (ClusterBuffer, LightBuffer) that the
depth pass doesn't provide. Use `DepthOnlyKeywords = true` in `DepthOnlyPassParams`.

---

## Shadow mapping specific

### Shadow map verification

1. Capture shadow map: `capture_resource ShadowMapArray.View1 rangeMin=0.0 rangeMax=1.0 layer=0`
2. Capture scene depth: `capture_resource SceneView.Depth rangeMin=0.0 rangeMax=1.0`
3. They should look COMPLETELY DIFFERENT (orthographic top-down vs perspective).
4. If they look similar: the depth pass uses the wrong VP. **Go to Step 2 above.**

### Shadow sampling verification

1. Use debug overlay: `oColor = vec4(1-sf, sf, 0.2, 1.0)` (RED=shadowed, GREEN=lit)
2. If all green: check if refDepth is OOB (< 0 or > 1).
3. If shadow at wrong position: check UV.y flip.
4. If shadow shape is wrong (wedge instead of oval): check light direction sign
   and camera placement.

### Cascade verification

1. Capture all 4 layers: they should show different scales (cascade 0 tightest).
2. Log VP[0] and VP[5] per cascade — they should differ by ~2x between adjacent cascades.
3. Check `ge_shadowSplits` values — they should be increasing positive values
   in view-space depth.

### Common shadow pitfalls

- **Light direction**: `directionWS` is the entity's +Z axis (direction light shines).
  Eye = `center - directionWS * extension` places camera opposite to light direction.
- **Cascade camera UBO**: Must use persistent buffers, not render graph transient pool
  (see buffer aliasing note above).
- **Frame data timing**: Upload lambda must execute before cascade exec reads cached data.
  Don't clear the frame cache in the build phase.

---

## RenderDoc

Frame captures are the most powerful debugging tool for GPU issues. A single
capture showing bound descriptor sets and buffer contents at draw time would
have diagnosed the shadow buffer aliasing bug in one session.

### Taking a capture

1. `launch_editor renderdoc=true` — runs the staged editor under
   `renderdoccmd capture`. It defaults `captureCompat=true`
   (`GE_VK_CAPTURE_COMPAT=1`), which drops `VK_EXT_descriptor_buffer` for the
   session; without it nearly every resource sits behind a buffer device address
   and the `.rdc` routinely fails to replay.
   **Caveat:** that same variable makes HZB occlusion culling report zero culled
   draws (`VulkanDevice.cpp`, capture-compat path), so a compat session is not
   valid for occlusion or perf analysis — pass `captureCompat=false` for those,
   and accept that the capture may not replay.
   To use the GUI's launch options instead, launch under `qrenderdoc` by hand.
2. `wait_for_editor`, then `trigger_capture`. It returns
   `{captured, captureIndex, filePath}` once RenderDoc has finished writing.
   The render loop stalls while RenderDoc serializes, so a >100 MB capture can
   take seconds of wall time while consuming only tens of frames.
3. If `trigger_capture` times out, the capture is still finishing — recover its
   path with `get_capture_status`, which answers immediately.

Captures land at `<editor exe dir>/Captures/editor_frameN.rdc`. The editor only
replaces RenderDoc's own `%TEMP%\RenderDoc\...` default — a template set by
`renderdoccmd --capture-file` or the qrenderdoc launch dialog is left as-is.
`GE_RENDERDOC_CAPTURE_DIR` (or `launch_editor`'s `captureDir`) overrides either.
When RenderDoc is not attached, `trigger_capture` answers with an `{error: ...}`
body rather than a thrown error.

If the editor never comes up under `renderdoc=true`, read the `renderdoccmdLog`
path in the launch result — a failed inject is reported only there.

The Render Graph panel's "Capture in RenderDoc" button does the same thing and
shows the resulting filename next to itself.

### Analyzing a capture

Drive `qrenderdoc --python <script>` over the `.rdc`. A script opens the
capture with `renderdoc.OpenCaptureFile`, walks the action tree, and reads
pipeline state per action — the same API the qrenderdoc interface uses.

**First-run gotcha:** qrenderdoc's analytics dialog blocks `--python` until it is
answered, so a scripted run hangs with no output on a fresh profile. Pre-seed
`%APPDATA%\qrenderdoc\UI.config` with `"Analytics_TotalOptOut": true`.

## Radeon GPU Profiler (AMD only)

> ⚠️ **UNVERIFIED-ON-AMD** — the `rgp_capture` tool was written and tested on an
> NVIDIA machine, where only its refusal path can run. The AMD-side flow has
> never been executed: the service handshake, the panel invocation and its
> flags, the `.rgp` output location, whether the engine's debug-utils labels
> actually appear as RGP user markers, and the developer-mode driver toggle are
> all untested. Verify each of those on AMD before trusting a result.

RGP is the AMD counterpart to Nsight: per-event GPU timing, occupancy, and
barrier/stall attribution — the tool to reach for when a pass is slow on Radeon
rather than wrong. The engine's `VK_EXT_debug_utils` labels should surface as
RGP user markers, naming passes in the timeline — untested on AMD, and worth
confirming first, since an unnamed timeline is much harder to read.

Requires an AMD Radeon GPU, the developer-mode driver, and the Radeon Developer
Tool Suite (https://gpuopen.com/rdp/) supplying `RadeonDeveloperServiceCLI` and
`RadeonDeveloperPanelCLI`.

```bash
node mcp/ge.mjs get_gpu_tooling                    # preflight: vendor + debugUtilsEnabled
node mcp/ge.mjs rgp_capture --duration-sec 5       # capture the running editor
```

`rgp_capture` refuses on non-AMD hardware, naming the GPU it actually found. It
ensures the service broker is running (spawning it detached if needed), drives
the panel CLI to trigger a capture, and returns the newest `.rgp` written after
the trigger. Every spawned CLI's output goes to `build/mcp-rgp-logs/`, and those
paths are in the tool result — read them first when a capture comes back empty.

## Key engine tools

| Tool | Use |
|------|-----|
| `take_screenshot` | Capture viewport |
| `capture_resource name rangeMin rangeMax layer` | Capture any RG texture |
| `get_log count minLevel filter` | Check VUIDs and warnings |
| `get_render_graph_validation` | Check resource conflicts |
| `get_render_graph_pass_detail passName` | Inspect per-pass resource accesses |
| `get_render_graph_resources type aliveOnly` | List all RG resources |
| `get_render_graph_dependencies` | Check pass ordering |
