# Visual Shader Upstream Reference

Native C++ port of Godot Engine's Visual Shader node catalog and codegen
semantics. Material graphs use the **shader graph v2** workflow (see
`docs/Rendering/shader-graph-spec.html`): one `.glsl` file per graph with
`@sg-*` tags, helper nodes under `Engine/Modules/Rendering/Shaders/Graph/Nodes/`,
and `ShaderGraph::SgGraphCompiler` for codegen. Game-logic graphs still use
`.graph` JSON in `NodeGraphPanel`.

## Upstream

- Repository: https://github.com/godotengine/godot
- License: MIT (staged from vcpkg package `godot-visual-shader-upstream`)
- Tag pinned: `4.6.3-stable`
- Commit pinned: `35e80b3a8822a9df9be390814b62f44c0a9c69e8`
- Commit date: 2026-03-04

## Primary upstream sources (reference only — not vendored wholesale)

| Godot path | Purpose |
|---|---|
| `scene/resources/visual_shader.cpp` | VisualShader resource, connection model |
| `scene/resources/visual_shader_nodes.cpp` | Per-node `generate_code()` implementations |
| `scene/resources/visual_shader_nodes.h` | Node class declarations |
| `editor/shader/visual_shader_editor_plugin.cpp` | Editor UI (GraphEdit canvas) |
| `editor/shader/visual_shader_editor_plugin.h` | Editor plugin types |

## Engine mapping

Material graphs: `GraphKind::Material`, authoritative tags in `.glsl`, edited in
`NodeGraphPanel`. Editor palette types come from `SgNodeReflector` over helper
`.glsl` files plus pseudo nodes (`EngineInput`, `SurfaceOutput`, `VertexOutput`).
Legacy `TypeId` strings map to `SG_*` helpers (see table below).

Codegen: `Rendering/ShaderGraph/SgGraphCompiler.cpp` emits `EvaluateSurface()` /
`ModifyVertex()` compatible with `Shaders/Includes/surface_io.glsl`.
`Graph/MaterialGraphCompiler.cpp` is a thin bridge from `GraphModel` for tests
and preview.

| TypeId | Upstream class | Upstream source file |
|---|---|---|
| EngineInput (UV, Time, normals, …) | VisualShaderNodeInput | scene/resources/visual_shader_nodes.cpp |
| FloatConstant, Vec2Constant, Vec3Constant, Vec4Constant, ColorConstant | VisualShaderNode*Constant | scene/resources/visual_shader_nodes.cpp |
| SampleTexture2D | VisualShaderNodeTexture2D | scene/resources/visual_shader_nodes.cpp |
| Add, Subtract, Multiply, Divide, Min, Max, Mod, Power, Step | VisualShaderNodeFloatOp | scene/resources/visual_shader_nodes.cpp |
| Abs, Sin, Cos, Sqrt, Saturate, Negate, Floor, Ceil, Fract, Sign, Tan, Exp, Log, OneMinus | VisualShaderNodeFloatFunc | scene/resources/visual_shader_nodes.cpp |

(Full catalog lives in `Engine/Modules/Rendering/Shaders/Graph/Nodes/`; a new
node is authored there as a `// @sgnode`-tagged GLSL function.)
