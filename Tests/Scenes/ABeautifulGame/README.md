# ABeautifulGame stress / reference scene

A chess set on a marble board — Khronos Sample Asset, single 43 MB `.glb`.
Used as a smaller-but-material-rich complement to Sponza for GPU-driven
rendering benchmarks. ~12 distinct materials including a clear-coat board, gold
+ silver pieces, marble, and felt.

## Fetching the model

```
cmake --build --preset <your-preset> --target FetchABeautifulGameTestScene
```

The target invokes `cmake -P FetchABeautifulGame.cmake` which does an
idempotent `file(DOWNLOAD)` with SHA256 verification against a Khronos-pinned
commit (`2bac6f8c57bf471df0d2a1e8a8ec023c7801dddf` from
`KhronosGroup/glTF-Sample-Assets`). The file lands at
`Assets/Models/ABeautifulGame.glb` and is gitignored.

## Opening the scene

```
Editor.exe --project <repo>/Tests/Scenes/ABeautifulGame
```

`Assets/Scenes/ABeautifulGame.scene` is the baseline reference. If it doesn't
exist yet, follow the same manual authoring path as the Sponza scene:

1. Drag `Assets/Models/ABeautifulGame.glb` into the SceneView
2. Frame the board with the camera
3. Save as `Assets/Scenes/ABeautifulGame.scene`
4. Commit the `.scene` file

## Why this scene

Material-rich without being geometrically dense. Useful for testing
material-binding hot paths (per-material descriptor sets, the bindless
texture-index path, clear-coat / PBR shader variants) without paying for
Sponza's vertex bandwidth.

## License

ABeautifulGame is licensed under CC BY 4.0. The original model is by the
MaterialX Project, copyright 2020 ASWF; the glTF conversion is copyright 2022
Ed Mackey. See [asset provenance](ASSET_PROVENANCE.md) and the
[notice beside the fetched model](Assets/Models/THIRD_PARTY_NOTICES.md).
