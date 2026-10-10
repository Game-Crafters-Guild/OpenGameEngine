# Blender to Engine Coordinate System Conversion (.blend)

Canonical conversion is `ModelImport::MakeEngineConversion(BlendImport::SourceConversion(), opts.Axis)`.
The Blender source frame lives in `Engine/Source/Assets/BlendAxisConversion.h` next to
`ModelAssetLoadBlend.cpp`. Shared bake/mirror math is `ModelAxisConversion.h`.
FBX-only ufbx adapters stay in `FbxAxisConversion.h`.
This note records the frames and the default B_to_E map (Y/Z swap then Mirror X).

## Source frame (Blender)

- Right-handed.
- World axes: **+X right, +Y forward, +Z up**.
- Quaternions stored DNA-side as `(W, X, Y, Z)` (real part first).
- Linear unit: metres (Blender's internal storage; FBX cm-to-m conversion
  does NOT apply to .blend).
- UV V origin: bottom (Blender / OpenGL convention).
- Triangle winding: CCW (counter-clockwise from front).

## Target frame (this engine)

- Left-handed.
- World axes: **+X right, +Y up, +Z forward**.
- Quaternions stored as `(X, Y, Z, W)` (real part last).
- Linear unit: metres.
- UV V origin: top (Vulkan-friendly).
- Triangle winding: CW.

## Conversion rule

`BlendImport::SourceConversion()` is the Y/Z swap `(x,y,z)->(x,z,y)`.
Default `FbxLoaderOptions` Mirror X (Unity-equivalent RH→LH bake) composed
with that swap is B_to_E:

```
default MakeEngineConversion(BlendImport::SourceConversion(), AxisOptions{}):
  (x, y, z) -> (-x, z, y)   on points and directions
  (qx, qy, qz, qw) -> (-qx, qz, qy, qw)   on quaternions
                    (the (W, X, Y, Z) -> (X, Y, Z, W) reorder
                     happens BEFORE ConvertQuat)
```

Inspector bake/mirrors (`assets.fbx.bakeRotation*`, `assets.fbx.mirrorAxes`)
compose after that, same kv as FBX/glTF.

Per-datum table (default Mirror X, bake off):

| Datum | Conversion |
|---|---|
| Vertex position `(x, y, z)` | `ConvertVec3` × unitScale (unitScale = 1.0 for Blender) |
| Vertex normal | face cross product in engine space (positions already converted) |
| Bone rest / IBM | `ConvertAffine` on `Bone::arm_mat` (row-major, translation in last row) |
| Animation translation key | `ConvertVec3` × unitScale |
| Animation rotation key | DNA WXYZ reorder, then `ConvertQuat` |
| Animation scale key | `ConvertScale` |
| Triangle winding | Blender loops are reverse vs engine; swap `idx[1]`/`idx[2]` when `!reverseWinding` so default (even det) keeps the old swap |
| UV V coordinate | `1.0 - V` |

## Helper ownership

`ModelAssetLoadBlend.cpp` unnamed-namespace converters take an `AxisConversion`
and forward to `ModelImport::ConvertVec3` / `ConvertQuat` / `ConvertScale` /
`ConvertAffine`. Inline axis math elsewhere in the file is forbidden.

## Quaternion sign / antipode

Blender does not enforce a hemisphere convention on stored quaternions. The
engine's animation pipeline is hemisphere-agnostic at every slerp site
(per Appendix A of `plan-v4.md` and the `QuaternionMath::SlerpShort` helper),
so we do NOT need to flip-on-import. We DO need to be consistent inside
each conversion function: never silently change the sign of a quaternion
mid-import — let the slerp consumers do the flip.

## VRM 0.x note (does NOT apply to .blend)

`ModelAssetLoadBlend.cpp` will NOT inherit the VRM 0.x `+Z forward`
auto-flip from `Animation::ApplyVRM0AxisFlip` (Phase 10 importer). Blender
files always use `+Y forward`; the auto-flip is unique to the legacy VRM
spec.
