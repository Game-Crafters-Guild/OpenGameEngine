# Alpha coverage mipmaps

Ordinary mip filtering moves a masked texture's alpha-tested coverage away from
the source's, in either direction: thinner where the material's cutoff sits above
the local alpha density, and fatter where it sits below and the gaps inside a leaf
cluster fill in as the filter coarsens. Either way the silhouette stops matching
what was authored. A source texture can opt into alpha coverage
correction with **Preserve Alpha Coverage** in the Texture Inspector. Set
**Coverage Cutoff** to the material's reference alpha cutoff. Materials do not
implicitly enable this setting or supply its cutoff.

The existing asset metadata stores these values:

| Key | Value |
| --- | --- |
| `assets.texture.alphaCoverage` | Absent or `0` disables correction; `1` enables it. |
| `assets.texture.alphaCutoff` | Finite decimal in `[0, 1]`; absent uses `0.5`. |

An invalid enable token or an invalid cutoff while enabled refuses the upload
and logs an error. A disabled policy ignores a stored cutoff. Settings changes
use the existing recook and upload replacement path.

Use decoded LDR source images with alpha-preserving compression: **None**, **BC7**,
or **Auto** with Color, Packed, or unknown usage. Normal and HDR inputs are
unsupported. Explicit BC1, BC4, BC5 and BC6H requests are refused even when the
current device would resolve those formats to uncompressed storage. Authored
KTX/DDS mip chains and SVG rasterization do not use this source-image policy.
No source-image decoder means enabled coverage is unavailable; a dummy texture
cannot stand in for successful correction.

## What correction guarantees

The shared cook/raw mip builder first creates the ordinary configured chain,
including its usual sRGB filtering. It then changes only alpha below level zero.
Base pixels and RGB bytes at every mip remain identical to the ordinary builder.
This ordering matters because the sRGB resampler weights RGB by alpha.

Coverage is measured after channel swizzling, with `alpha >= cutoff` passing,
matching the material discard boundary. Each mip selects the nearest attainable
passing texel count relative to the base under a uniform nonnegative alpha scale
and final UNORM8 rounding. Ties prefer the scale closest to one, then the smaller
scale. Transparent zero remains zero. Each mip uses one histogram scan, a
fixed-size search, and at most one alpha rewrite pass: O(texels + 256).

Tied alpha values and tiny levels, especially 1×1, cannot always represent the
base ratio. At cutoff zero every texel already passes, including transparent
zero, so correction does nothing. Cutoff one uses quantized byte 255 admission.
Mip disabling and mip limits continue to use the existing settings.

This is a precompression texel-space guarantee. BC7 encoding and
bilinear/trilinear sampling can change measured coverage. Reference tests report
those effects separately; the policy does not guarantee a particular silhouette
at every camera distance, sample phase, material tint, vertex alpha, or cutoff.

Disabled settings preserve the existing cook configuration key and bytes. An
enabled policy adds its algorithm revision and exact cutoff bits to the key.
Source changes remain part of the existing source fingerprint. Both raw uploads
and subsequent cooked artifacts use the same correction implementation.

## Verification targets

`EngineTextureTests` includes the CPU policy, independent
coverage oracle, cache invalidation and corrupt-source tests.
`TextureAlphaCoverageGpuTests` separately initializes a disposable EngineCore
workspace and exercises raw/cooked uploads, queued failure recovery, and masked
cards through the 1×1 tail. Cook-dependent cases require KTX. GPU cases can skip
on an unavailable Vulkan device or BC7 encoder; an acceptance run must check its
actual skip count before claiming that coverage.

The raw/cooked GPU comparison requires identical RGBA values between both paths.
Its additional analytical sRGB check bounds hardware conversion by adjacent
encoded source codes; it does not require exact agreement with a CPU `pow`.
Alpha is compared directly. BC7 decoder comparisons have separate tolerances.
These tests do not enable the policy on any shipped foliage asset.
