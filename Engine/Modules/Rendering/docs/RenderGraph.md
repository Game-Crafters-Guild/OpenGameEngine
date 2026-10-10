# RenderGraph — Quick Reference

The engine's render graph is **immediate-mode**: the whole frame graph is declared fresh each
frame into a bump arena, compiled in one sweep, executed, and reset. There is no retained pass
state, no enable/disable/invalidate, no incremental compile — a pass that should not run this
frame is simply not declared.

**Authoritative sources** (prefer these over any prose):

| What | Where |
|---|---|
| Public API (declare/import/execute) | `Include/Rendering/Core/RenderGraph/RGFrame.h` |
| Core IR (cull, schedule, barriers) | `Include/Rendering/Core/RenderGraph/RGGraph.h` |
| Pools, upload ring, rotation | `RGResourcePool.h`, `RGTransientPool.h`, `RGUploadRing.h`, `RGRotatingImport.h` |
| Design rationale + field comparison | "Render Graph Redesign — Immediate-Mode, Arena-Backed, Queue-Aware", in the support repository |
| Living usage examples | `Engine/Source/Engine/Rendering/Pipeline/Nodes/`, `Engine/Modules/Rendering/Tests/RenderGraph/` |
| Architecture overview | `Rendering_Architecture_Guide.html` (§The Render Graph, §Writing a Render Pass) |

## The rules that matter

- **Declare every access.** Barriers, ordering, and culling derive from `Read`/`Write`/`Attach*`
  declarations. A bindless/descriptor-direct consumer still needs a declared `Read` — undeclared
  access means no edge and no barrier.
- **`LoadOp::Load` is a read.** Depth testing and blending consume prior contents; the Load-derived
  read edge is what keeps the producer alive. `Clear`/`DontCare` create no edge — a producer feeding
  a cleared attachment is correctly culled.
- **Recording order is hazard order.** Declare producers before consumers within a frame
  (read-before-write derives WAR by design).
- **Imports are per-frame.** `ImportPersistent*` = pool-owned with cross-frame layout carry;
  `ImportExternal*` = engine-owned, no write-back; `RGRotatingTexture/Buffer` = the only N-buffer
  rotation (history, CPU readback — readback rings need count ≥ framesInFlight + 1).
- **CPU per-frame data → `AllocUpload<T>()`.** One upload ring; there is no `perFrame` flag.
- **Consume-only passes need `PreventCulling()`** (readbacks, queries). Everything else survives by
  transitively feeding a sink (`MarkOutput` / present / export).
- **Frame-local ids die with their `RGFrame`.** Never cache `RGTexture`/`RGBuffer` across frames
  without a frame-identity guard.
- **Queues**: the production default collapses all logical queues onto one graphics submission
  (`kSingleQueueMap`); async compute is an explicit opt-in map, not a per-pass accident.
- **Execute lambdas capture by value** — they run after declaration returns.
