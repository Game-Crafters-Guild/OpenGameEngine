# Rendering buffers and frames in flight

This document clarifies how to think about **per-frame data**, **frames in flight**, and when to use
`BufferRing`, `RingBuffer<T>`, `TransientBufferPool`, or dedicated GPU buffers such as those in
`GPUScene`.

The goal is to prevent subtle CPU/GPU lifetime bugs (e.g., updating a buffer for frame _N+1_ while the
GPU is still reading it for frame _N_) and to make it obvious which abstraction to reach for.

---

## 1. Terminology

- **Frame**: A single logical update + render of the world.
- **Frames in flight**: How many frames the GPU can have "in progress" at once (3 on the Vulkan
  backend — `VulkanDevice::MAX_FRAMES_IN_FLIGHT`; always query `IDevice::GetFramesInFlight()` rather
  than hard-coding a count).
- **Per-frame data**: Data produced fresh every frame (camera constants, UI vertices, indirect args).
- **Multi-frame data**: Data that is stable across many frames (meshes, static materials, lightmaps).

The device (`IDevice`) owns the authoritative **frame index** and enforces fences between frames.
Helpers in this document assume you use the device's frame index consistently.

---

## 2. BufferRing -- single-frame CPU-visible uploads

`Rendering::BufferRing` is a **single CPU-visible upload buffer** treated as a ring allocator **within
one frame**.

**Intended use:**

- Small, transient uploads whose lifetime is strictly **one frame** (UBOs, tiny structs).
- You reset and reuse the whole buffer every frame.

**Key rules:**

1. Call `Initialize()` once.
2. For each device frame:
   - After present / frame finalization, call `ResetForNewFrame()` **once**.
   - Call `Allocate(size, align)` for all per-frame writes.
3. Do **not** hold onto offsets or pointers across frames.
4. `BufferRing` **does not know about frames in flight**; it assumes the GPU is done with the
   previous frame by the time you call `ResetForNewFrame()`.

If you need the GPU to read different slices of a buffer concurrently for multiple frames in flight,
**do not use a single BufferRing** -- use `RingBuffer<T>` or per-frame buffers instead.

---

## 3. RingBuffer<T> -- per-frame slices with frames-in-flight awareness

`Rendering::RingBuffer<T>` (see `Rendering/Utils/RingBufferHelpers.h`) is designed for data that must
be **safe across multiple frames in flight**.

Characteristics:

- The buffer is divided into `framesInFlight` equal **slices**.
- Each slice has its own `head` and `overflowed` flag.
- You write into the slice for the current device frame index.

**Typical flow:**

1. Create a ring buffer:
   - `auto rb = CreateRingBuffer<Vertex>(device, framesInFlight, countPerFrame, "Name");`
2. At the start of frame `frameIndex`:
   - `ResetRingBufferFrame(rb, frameIndex);`
3. To write N elements for that frame:
   - `auto alloc = MapRingBuffer(device, rb, frameIndex, N);`
   - Fill `alloc.ptr`, then `AdvanceRingBuffer(rb, frameIndex, bytesWritten);`
4. If in GPUOnly mode, enqueue the copy for this frame via `FlushRingBufferWrites`.

**Rules and caveats:**

- `frameIndex` **must** match the device's current frame-in-flight index.
- Do not write to slice _i_ for frame _N+1_ until the device has signaled that frame _N_ using slice
  _i_ is complete.
- Capacity is per frame; if you overflow a slice you will get `nullptr` from `MapRingBuffer` and
  `overflowed[frameIndex]` becomes `true`.

Use `RingBuffer<T>` for things like UI vertices/indices or other streaming data that must be
consistent with the engine's frames-in-flight model.

---

## 4. TransientBufferPool -- cross-frame reuse with budgets

`Rendering::TransientBufferPool` / `TransientTexturePool` manage **reusable GPU resources** across
frames with a memory budget and optional idle eviction.

They are appropriate for:

- Buffers/textures that might live for several frames but are not permanent assets.
- Temporary render targets, readback buffers, large scratch buffers, etc.

Rules of thumb:

- Call `Acquire(desc)` when you need a transient resource.
- Call `ResetFrame()` once per device frame to mark all resources as not in-use and optionally evict
  idle ones.
- Do **not** assume a resource will survive indefinitely across many frames unless you keep acquiring
  it frequently; the pool may evict it when over budget or idle for too long.

The pool does **not** by itself ensure CPU/GPU synchronization; it relies on the device's per-frame
fences and the caller not reusing a handle while the GPU is still using it.

---

## 5. GPUScene -- global scene buffers

`Rendering::GPUScene` owns the GPU-resident representation of the scene (instances, meshes,
materials). Historically it also owned visibility buffers and culling data; in the
target architecture **per-view visibility lists and HZB/occlusion resources live in a dedicated
GPU visibility/HZB pipeline** (fronted by `GPUCullingPipeline`).
GPUScene remains the scene data backend that those pipelines consume.

Current behavior:

- Uses `static constexpr uint32_t kMaxFramesInFlight = IDevice::kMaxSupportedFramesInFlight` for
  array sizing and `m_FramesInFlight` (queried from `device->GetFramesInFlight()` at construction)
  for runtime logic.
- CPU-side vectors are updated as ECS changes; `UpdateGPUBuffers()` uploads them via `UpdateBuffer`.
- `BeginFrame()` increments an internal `m_frameIndex` and derives the active frame slot from
  `m_device->GetFrameIndex() % m_FramesInFlight`. Calls `UpdateGPUBuffers()`.
- `EndFrame()` clears dirty flags once culling work is scheduled.
- Per-frame buffers (instances, visibility, indirect-args, culling data) are created for each
  frame-in-flight slot, giving each in-flight frame its own stable view of scene data.

**Key invariants:**

- GPUScene assumes a **single logical writer** and that all updates happen on the render thread.
- It relies on the device's per-frame fences to avoid CPU writing into buffers while they are still in
  use by the GPU.
- Per-frame views of buffers are exposed via `GetInstanceBuffer()` (current frame) and
  `GetInstanceBufferForFrame(frameIndex)` (explicit slot).

---

## 6. Per-frame GPUScene buffers (implemented)

GPUScene uses true per-frame buffers for frame-in-flight safety:

- `struct GPUSceneFrameResources { BufferHandle instanceBuffer; BufferHandle visibilityBuffer;
  BufferHandle indirectArgsBuffer; };`

- `static constexpr uint32_t kMaxFramesInFlight = IDevice::kMaxSupportedFramesInFlight;`
  (the compile-time upper bound defined once on `IDevice`; runtime frame count is queried
  via `device->GetFramesInFlight()` and stored as `m_FramesInFlight`).
- `GPUSceneFrameResources m_frames[kMaxFramesInFlight];`
- `uint32_t m_frameIndex;` which is advanced once per `BeginFrame`; the active frame slot
  is derived from `m_device->GetFrameIndex() % m_FramesInFlight`.

API:

- Current-frame accessors:
  - `BufferHandle GetInstanceBuffer() const;`
  - `BufferHandle GetVisibilityBuffer() const;`
  - `BufferHandle GetIndirectArgsBuffer() const;`

- Explicit per-frame accessors (for tests and advanced callers):
  - `BufferHandle GetInstanceBufferForFrame(uint32_t frameIndex) const;`
  - `BufferHandle GetVisibilityBufferForFrame(uint32_t frameIndex) const;`
  - `BufferHandle GetIndirectArgsBufferForFrame(uint32_t frameIndex) const;`

Update path:

- `BeginFrame()`:
  - Increment `m_frameIndex`.
  - Derive `m_frameSlot` from `m_device->GetFrameIndex() % m_FramesInFlight`.
  - Upload dirty CPU-side instance/material data into that frame's buffers.

- `EndFrame()`:
  - Clear dirty flags once all culling/visibility work for the current frame has been scheduled.
  - Does **not** perform synchronization; the device's per-frame fences still own correctness.

Notes:

- Material/mesh buffers are updated less frequently and may remain **shared** across frames, relying
  on the device's fences for safety when they are updated.
- The `kMaxFramesInFlight` constant comes from `IDevice::kMaxSupportedFramesInFlight` (currently 4),
  shared across all modules: GPUScene, GPUCulling, SkyRenderer, Material, UITextureRegistry,
  FrameBufferAllocator, and LogicalBuffer.

---

## 7. Quick decision guide

- "I need a few KB of per-frame constants/UBOs." -> **FrameBufferAllocator** (frame-in-flight safe;
  `Initialize` with `BufferUsage::Uniform` and the UBO offset alignment, or pass the alignment per call to `Allocate(bytes, alignment)`).
- "I need per-frame SSBOs or mixed-usage transient GPU data." -> **FrameBufferAllocator** (N
  `BufferRing`s rotated by frame index) or **PerFrameWritePool** (aggregates multiple allocators by
  usage class).
- "I need a buffer inside a render graph pass." -> **Transient RG buffers** via
  `RGFrame::CreateBuffer()` before declaring the consuming pass. Declare its accesses with
  `RGPassBuilder::Read` / `Write` inside the setup callback; the frame manages its transient allocation.
- "I need to stream per-frame vertex/index data safely across N frames in flight." ->
  **RingBuffer<T>** / **FrameBufferRing<T>** (single buffer divided into per-frame slices).
- "I need temporary large buffers/textures with a budget and reuse across frames." ->
  **TransientBufferPool / TransientTexturePool**.
- "I need long-lived scene data (instances, meshes, materials)." -> **Dedicated GPUScene / asset
  buffers**, not BufferRing/RingBuffer.

**Never use `BufferRing` directly for per-frame GPU data.** `BufferRing` is a low-level building
block that manages a single physical buffer with no frame-in-flight awareness. Use it only when you
manage frame rotation externally (e.g., `PerFrameWritePool` already owns one `BufferRing` per frame
slot). For any new per-frame allocation, prefer `FrameBufferAllocator` or transient RG buffers.
