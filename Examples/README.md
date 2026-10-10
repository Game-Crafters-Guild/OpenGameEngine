# Examples

Standalone programs that host the engine outside the Editor and Player. Build them with
`-DBUILD_EXAMPLES=ON`; they land under `build/<preset>/bin/<Config>/Demos/`.

| Target | What it shows |
|---|---|
| `PhysicsFallingSpheresDemo` | A minimal host: a `Platform::Window`, a `RenderDeviceContext` with `RenderServices`, an immediate-mode render graph frame, an ECS world running the transform, render-extraction and PhysicsECS systems, and a field of Jolt-simulated spheres falling onto a plane. `R` resets, `Esc` quits. |

```bash
cmake --preset vs2026-x64-local -DBUILD_EXAMPLES=ON
cmake --build --preset vs2026-x64-local --config DebugFast --target PhysicsFallingSpheresDemo
```

Engine API usage that a test already covers belongs in that test, not here.
