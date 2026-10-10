# Sponza stress / reference scene

The classic Sponza cathedral interior — Khronos Sample Asset. Used as a
medium-complexity scene for GPU-driven rendering benchmarks (bucketer dispatch
count, cascade cull cost, draw-call totals).

## Fetching the model

Sponza is distributed by Khronos as a multi-file glTF (~25 MB across one
`.gltf`, one `.bin`, and ~30 texture files). Fetch via:

```
cmake --build --preset <your-preset> --target FetchSponzaTestScene
```

The target uses `git sparse-checkout` against a pinned commit
(`2bac6f8c57bf471df0d2a1e8a8ec023c7801dddf` from `KhronosGroup/glTF-Sample-Assets`)
so the download is fully reproducible. Files land under
`Assets/Models/Sponza/` and are gitignored. The model's upstream license,
`Models/Sponza/LICENSE.md` at the pinned commit, is copied unchanged beside it as
`Assets/Models/Sponza/LICENSE.md`; the fetch fails if that file is missing.

## Opening the scene

After fetching, launch the editor with this project root:

```
Editor.exe --project <repo>/Tests/Scenes/Sponza
```

`Assets/Scenes/Sponza.scene` is the baseline reference scene (camera + sun +
sky + Sponza placed at origin). If it doesn't exist yet, the first launch will
need a manual scene authoring pass:

1. Drag `Assets/Models/Sponza/Sponza.gltf` into the SceneView
2. Position the camera to look down the nave
3. File → Save Scene As → `Assets/Scenes/Sponza.scene`
4. Commit the resulting `.scene` (small text file)

## Why this scene

Sponza has ~25 distinct materials with ~25 submeshes — a useful midpoint
between the engine's default near-empty scene and a true production load.
Steady-state stresses the bucketer's per-batch dispatch fan-out
(numBatches × numCascades) but doesn't yet stress per-batch instance counts.

For full bucketer stress (numBatches × instances), add a procedural instance
spawner alongside Sponza in a separate scene — TODO.

## License

Sponza: © 2016 Crytek, under the Cryengine Limited License Agreement; it is not
CC0. Its metadata has a separate CC BY 4.0 license. The model is fetched at build
time and never committed; its license file lands beside it. See
[asset provenance](ASSET_PROVENANCE.md).
