### Customizable Render Pipeline Guide (RenderGraph Asset)

This guide explains how to **use** the new data-driven render pipeline system and how to **extend** it with new pass types.

---

### What you get

- **A new asset type**: `RenderPipeline` (`.rendergraph`)
- **A runtime pipeline system** in `RenderServices` that:
  - Loads and compiles the pipeline asset into a cached blueprint and node instances
  - Declares fresh passes and resource handles into an `RGFrame` each frame; content changes refresh the cached blueprint
- **Editor SceneView integration**:
  - Scene renders through the active pipeline
  - Editor gizmos render **after** the pipeline, on top of its **`FinalColor`** output
- **MVP pass types**:
  - `WorldRender` (wraps world/entity rendering)
  - `FullscreenShader` (generic fullscreen graphics shader node)
  - `ComputeShader` (generic compute dispatch node)

---

### Where the default pipeline lives

- Default pipeline asset path (relative to asset root):
  - `RenderPipelines/ForwardPlus.rendergraph`

In the repo, it exists at:
- `Assets/RenderPipelines/ForwardPlus.rendergraph`

For the Editor, this folder is staged into the Editor's runtime `Assets/` folder on build.

---

### How the pipeline is used at runtime

#### How it is invoked each frame (ECS)

The frame driver calls `renderServices.Spine().BuildFrameGraph(frame, params)`.
The spine applies the active blueprint and asks each pipeline node to declare its work
into the current `Rendering::RenderGraph::RGFrame`. The host then executes that frame.
Passes and frame resource IDs are not retained between frames.

#### How to switch the active pipeline

Call:
- `renderServices.Spine().SetActiveRenderPipelinePath("RenderPipelines/MyPipeline.rendergraph");`

**Notes**
- This refreshes the active blueprint on the next `BuildFrameGraph(...)`.
- The path can be relative to the asset root (recommended) or absolute.
- The Editor also reads `rendering.activeRenderPipeline` from `<ProjectRoot>/.Editor/ProjectSettings.json` on startup.

---

### How SceneView (Editor) composes with gizmos

SceneView supplies the current frame's color, resolve and depth targets to the frame
spine. The pipeline declares its world and post-process work, then the editor declares
gizmos over `FinalColor` with the scene depth. The UI consumes that frame's output.

Resource accesses establish graph dependencies. Declaration order determines the order
of conflicting accesses; a pass phase is a scheduling priority among otherwise ready
passes, not a substitute for the producer/consumer order.

---

### RenderPipeline asset format (schemaVersion 2)

The asset is JSON. Minimal world-only example:

```json
{
  "schemaVersion": 2,
  "pipelineName": "Default",
  "passes": [
    { "id": "World", "type": "WorldRender", "enabled": true }
  ],
  "outputs": {
    "FinalColor": "View.Resolve"
  }
}
```

#### Required fields

- **`schemaVersion`**: must be `2`
- **`passes`**: array of pass objects

#### Recommended fields

- **`pipelineName`**: used for stable pass/resource naming and debugging
- **`outputs`**: name -> resource reference mapping (see below)

#### Pass order

Put passes in producer-before-consumer order in the `passes` array. The runtime walks
that array to declare the frame. The `phase` field accepts a named phase or an integer
and supplies a scheduling priority; it does not reverse resource dependencies.
The parser retains `tags` and `after` metadata, but the immediate render graph path does
not create tag-ordering edges from them. Do not use those fields to repair array order.

See `Engine/Modules/Rendering/docs/RenderGraph.md` for the graph's resource and execution contract.

---

### Shader references (`shaderPkg`, `shaderDesc`, `stages`)

A bare name such as `tonemap.frag.spv` is an **engine** shader: it resolves in the
`editor` mount — the `Assets/` tree staged next to `Editor.exe` / `Player.exe` — and
nowhere else. When that mount exists and does not hold the file, resolution fails
rather than falling back to the project, so a stale copy in an unrelated project tree
can never be picked up.

A **project or package** shader is named with its asset source prefix:

```json
{ "id": "Custom", "type": "FullscreenShader", "output": "View.Resolve",
  "shaderPkg": "project:RenderPipelines/Shaders/custom.shaderpkg" }
```

The prefix selects exactly one source — `project:`, `editor:`, or a mounted package
alias such as `nature-pack:`. The alias is case-insensitive, and the remainder may be
written rooted (`nature-pack:/Shaders/custom.shaderpkg`) or with a leading `Assets/`.
A prefixed name never falls back to another source: an unknown alias or an absent file
is reported as a missing shader.

Absolute paths are used as given. A deployment with no `editor` mount — unit tests and
packaged games, which fuse the engine shaders into the project mount — resolves bare
names project-first.

An export ships a prefixed `.shaderpkg` through the build manifest, from the source the
prefix names; bare names ship with the engine `Shaders` folder, which the export copies whole.

---

### Resource references

Many node fields refer to textures/buffers via a string "ref".

#### Built-in view refs

- **`View.Color`**: view's color attachment
- **`View.Depth`**: view's depth attachment
- **`View.Resolve`**: view's resolve attachment (or `View.Color` if no explicit resolve)
- **`View.DepthResolved`**: a single-sample copy of the view depth, published by the `DepthResolve` node. It holds everything the color pass depth-tests against, grass included. Bind it in any pass that measures distance to what is on screen or decides what is in front: fog, water, reflections, temporal reprojection, depth of field.
- **`View.OccluderDepthResolved`**: the same copy without the non-occluding grass heads. Only occlusion passes that grass must not darken read it: the engine's GTAO and contact-shadow nodes. A C++ node of that kind takes it from `ViewDeclare::ViewOccluderDepthResolved`; every other pass binds `View.DepthResolved`. Without grass in view, both names are the same texture.

#### Pipeline-defined resources

You can declare textures/buffers in `resources` and reference them by name. The pipeline runtime resolves them into the current frame's typed RenderGraph handles.

**Resource schema example:**

```json
{
  "resources": {
    "SceneColor": {
      "kind": "Texture",
      "scope": "PerView",
      "format": "RGBA8_UNORM",
      "usage": ["RenderTarget", "ShaderResource"],
      "sampleCount": 1,
      "extent": { "scale": [1.0, 1.0] }
    }
  }
}
```

Examples of supported texture `format` strings:
- `RGBA8_UNORM`, `RGBA8_SRGB`, `BGRA8_UNORM`, `BGRA8_SRGB`, `R16G16B16A16_FLOAT`, `D24_UNORM_S8_UINT`, `D32_FLOAT`

Relative `extent.scale` uses the view's internal render extent by default. Set
`extent.basis` to `"output"` for a resource sized to the display extent after upscaling.
At render scale 1, both extents are equal.

---

### Pass types (MVP)

#### `WorldRender`

Schedules world/entity rendering for each active view. Keywords control which shader
variant is used for all draws in the pass.

Optional fields:
- `keywords` (string array) — pass-level shader keywords, e.g. `"ForwardPlus"`, `"Instanced"`

Example:

```json
{ "id": "World", "type": "WorldRender", "enabled": true, "keywords": ["Instanced"] }
```

#### `FullscreenShader`

Generic fullscreen draw that:
- Attaches a color output (`output`)
- Declares input texture reads in RenderGraph (`inputs`)
- Uses the metadata inside a `.shaderpkg` to bind **set 0** combined image samplers by **binding name**

Example (tonemap-like):

```json
{
  "id": "Tonemap",
  "type": "FullscreenShader",
  "enabled": true,
  "inputs": { "uHDRColor": "SceneColor" },
  "output": "View.Resolve",
  "shaderPkg": "Shaders/tonemap.shaderpkg",
  "pushConstants": { "exposure": 1.0, "tonemapMode": 5 }
}
```

Notes / limitations:
- Currently binds **only set 0**.
- Uses linear clamp by default. `samplers` maps binding names to presets; `linearRepeat` selects linear repeat.
- `pushConstants` supplies named float or integer defaults. `ppOverrides: true` lets matching post-process settings override them.

Skipping a stage and gating an input:

- `skipWhen` (object, post-process settings field name to value): the stage is skipped for a
  view when every listed field reads within 1/1024 of its value; a field name the settings do
  not know keeps the stage running, and the compiler warns about it. A skipped stage publishes
  its input under its `output` name, so the stages after it read the chain unchanged.
  `passthroughInput` names which input is published when the stage has more than one, and
  `"stitchWhenSkipped": false` publishes nothing, for a stage whose output no running stage
  reads while it is skipped.
- `inputGates` (object, input binding to a post-process settings gate such as
  `bloomScatteringActive`): while the gate reads inactive, this stage neither resolves nor
  reads that input and its binding samples the engine's black texture; a skipped stage
  upstream that passes its input through can still allocate that input (#2158). The key must
  be one of the stage's `inputs` and the value the name
  of a computed `*Active` gate (not a value field such as `bloomOctaves`); the compiler warns
  otherwise. The stage itself still runs; combine it with `skipWhen` to skip the whole stage.

```json
{
  "id": "BloomCombine",
  "type": "FullscreenShader",
  "inputs": { "uHDR": "HDRUpscaled", "uBloom": "BloomNormalized", "uScattering": "ScatteringNormalized" },
  "inputGates": { "uBloom": "bloomHighlightsActive", "uScattering": "bloomScatteringActive" },
  "passthroughInput": "uHDR",
  "skipWhen": { "bloomChainActive": 0.0 },
  "output": "HDRCombined",
  "shaderPkg": "Shaders/bloom_combine.shaderpkg"
}
```

#### `ComputeShader`

Generic compute dispatch. Optional targets can be declared so RenderGraph tracks write hazards:
- `targetTexture` (ref)
- `targetBuffer` (ref)

Example:

```json
{
  "id": "MyCompute",
  "type": "ComputeShader",
  "enabled": true,
  "shaderPkg": "Shaders/ubo_copy.shaderpkg",
  "dispatch": { "x": 8, "y": 8, "z": 1 },
  "targetBuffer": "SomeBuffer"
}
```

Notes / limitations:
- `inputs` and `buffers` map reflected set-0 binding names to graph resources; `inputUsages` and `bufferUsages` declare their access modes. The package must contain the compute stage and matching metadata.

#### `CpuTextureInput`

Per-view producer that imports an engine-owned CPU texture revision (a runtime mask or
simulation field published through `TextureService::CreateCpuTextureSource`) and publishes
its texture under `output` and its fixed metadata block under `parameters`, so a later
`FullscreenShader` can consume both through `inputs` / `buffers`.

```json
{
  "id": "VisibilityInput",
  "type": "CpuTextureInput",
  "source": "Visibility",
  "output": "VisibilityMask",
  "parameters": "VisibilityBounds",
  "viewPurpose": "Game"
}
```

- `source` is the publisher's `StringId` name; `parameters` is optional and must be omitted for a
  source without metadata; `viewPurpose` defaults to `Game`.
- Publishes nothing when the source is absent, stale, not yet uploaded, or the device is not
  healthy — the consuming stage then skips (see the `FullscreenShader` missing-input behavior).
- Producer API, lifetime and upload contract: `docs/Rendering/cpu-texture-source.html`.

---

### Extending the system

#### Add a new node type

1) **Create a node class** implementing `IRenderPipelineNode`:
- `Initialize(nodeId, nodeJson, outError)`
- `GetTypeName()`
- `Declare(instance, ctx)` for frame-wide work, or `DeclareForView(ViewDeclare&)` for per-view work

2) **Register it** through the frame spine:
- `renderServices.Spine().RegisterPipelineNodeType("MyNodeType", factory, perView)`
- Registration can arrive **after** the graph that needs it was compiled — a project's native
  module loads at project open, well after the editor compiled the authored pipeline and rejected
  it for the unknown type. A successful registration schedules one retry of that rejected compile,
  so the authored pipeline applies by itself: no asset edit, reselection or restart. Several
  registrations before the next frame coalesce into a single attempt, a graph that is still invalid
  caches its new rejection, and a blueprint installed programmatically
  (`SetActiveRenderPipelineBlueprint`) keeps ownership.

3) **Declare the current frame's work**:
- Use `ctx.Frame` or `ViewDeclare::Frame` to create resources and call `RGFrame::AddPass(...)`.
- Per-view nodes resolve inputs with `ViewDeclare::ResolveTexture(...)` and `ResolveBuffer(...)`,
  and publish outputs with `PublishTexture(...)` and `PublishBuffer(...)`.
- In the pass setup callback, attach targets with `AttachColor(...)` / `AttachDepth(...)`
  and declare accesses with the typed `Read(...)` / `Write(...)` overloads.

Declare passes every frame they are needed. Persistent node state may cache device resources,
but must not retain frame handles or capture declaration-local references in execute callbacks;
those callbacks execute after declaration returns.

#### Add a new output name

In the asset:

```json
{
  "outputs": {
    "FinalColor": "View.Resolve",
    "SceneColor": "SceneColor"
  }
}
```

Within the currently declared pipeline instance:
- `RenderPipelineInstance::GetOutputRG(viewId, "SceneColor")`

For the host's `FinalColor`, use `renderServices.GetPipelineOutputRG(frame, viewId)` after
`BuildFrameGraph` for that frame. The lookup validates the frame identity and returns the
current output and its physical texture. Do not cache the result across frames.

#### Add validation for node inputs/materials

The current MVP already:
- Validates `ShaderMeta` (push constant limits, etc.) for `FullscreenShader`
- Warns if `inputs` names don't exist as bindings in the shader package's metadata

To extend validation further:
- Read metadata from the shader package and validate via `ValidateShaderMeta`.
- For nodes that reference a `.material`, validate material properties/textures against meta using:
  - `ValidateMaterialAgainstMeta(meta, input)`

---

### Troubleshooting

- **Pipeline not found / doesn't load**:
  - Ensure the `.rendergraph` file exists in a mounted asset root. GUIDs and import metadata live in `AssetDatabase.assetdb`; `.meta` sidecars are ignored.
- **Fullscreen shader inputs don't work**:
  - Ensure the `.shaderpkg` metadata uses matching **binding names** for set 0 (e.g., `uHDRColor` in the tonemap package).
- **No post effect visible**:
  - Confirm your node is `enabled: true` and its `output` is `View.Resolve` (or is referenced by `outputs.FinalColor`).
- **The scene renders through the engine's ForwardPlus (or the previous pipeline) instead of the authored one**:
  - The compile was rejected. The log names the graph and every issue, including the node id whose
    `type` did not resolve, and the Scene View and Game View show a notice with the reason. The
    engine's copy in the Editor's `Assets/` folder stands in, never a project file of the same name;
    if that copy cannot be used either, nothing is drawn and the notice says why.
  - A packaged game has no stand-in: a pipeline it cannot use, or one missing from the package, draws
    nothing, and the log has the reason.
- **Some passes are missing while the project's scripts build**:
  - A pass whose type the project's scripts register waits while they build: the rest of the authored
    pipeline draws, and the Scene View and Game View name the waiting passes. Nothing needs doing: the
    graph is compiled again as soon as a script registers the type, and the pass joins on the next
    frame. If the scripts finish and the type is still unknown, the pipeline is refused as above.
  - A pass that reads what a waiting script pass writes waits with it, and the note names it.
    Declare a script pass's outputs under `resources`: a pass that reads an undeclared output of a
    script pass is refused.
