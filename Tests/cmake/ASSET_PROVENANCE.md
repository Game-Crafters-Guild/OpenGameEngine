# Fetched fixture provenance

These archives are fetched by the CMake recipes in this folder. The SHA-256
pins below identify the complete archive, including its models and textures.
`FetchBlendSamples.cmake` copies the existing `BlendSamplesLicenses.md` to
`LICENSES.md` beside the extracted and staged fixtures.

| Asset | Source and credit | License | Public redistribution |
|---|---|---|---|
| Human Base Meshes Bundle 1.4.1 | [Blender Studio and community contributors](https://download.blender.org/demo/asset-bundles/human-base-meshes/human-base-meshes-bundle-v1.4.1.zip); SHA-256 `811f43accbb31a88266d932f8f5563b2d13586fca0ba2693aad1f5fe582b3515` | CC0 1.0 | Allowed. |
| Ellie Pose Library 2.0.0 | [Blender Studio](https://download.blender.org/demo/asset-bundles/ellie-pose-library/ellie-pose-library-v2.0.0.zip); SHA-256 `547a997488b91683df68eb2925423fcfbbb1b6e3e7bedd0d4cd4f67d23090231` | CC BY 4.0 | Allowed with the source, credit, license link and changes identified. Fetched unchanged. |
| Animation Fundamentals Rigs 1.0 | [Blender Cloud, rigs by Rik Schutte](https://studio.blender.org/training/animation-fundamentals/5d69ab4dea6789db11ee65d1/); SHA-256 `f0f57b1292c147b76d45c179c01e4ee2397576348a194dd62c24c022dd00fbee` | CC BY-SA 4.0; archive `LICENSE.txt` | Allowed with attribution; distributed adaptations must retain the required share-alike license. Fetched unchanged. |
| `BusinessMale.fbx`, `A_Walk_F_Masc.fbx`, `A_Idle_Standing_Masc.fbx`, `Knight_skinned.fbx`, `Barrel_static.fbx` | License-holder-supplied archive accepted by `FetchFbxTestFixtures.cmake`; SHA-256 `e4dd639f91b60585e37dfa3dde616cc50a7469bb4851a819bb5868a3cdb5f1ec` | Store terms; no redistribution grant | Not allowed for public redistribution. No files or public download URL are tracked. A replacement is pending. |

The exact credit and license links for the permitted bundles are retained in
[BlendSamplesLicenses.md](BlendSamplesLicenses.md). They are optional test inputs,
not part of the engine's runtime package.
