# ShaderGen — generated shader tables

## ACES 2 peak-luminance tiers

`gen_aces2_tables.py` generates the ACES 2.0 Rec.709 output transform's three
363-entry tables plus the 12 per-tier scalars that are not closed-form in
peak, at six peak luminances (100·2^k nits, k = 0..5), consumed by
`tonemap_aces2.glsl`'s runtime F1 tier blend. It emits TWO files: the
`Aces2Tables` SSBO block declaration
(`Engine/Modules/Rendering/Shaders/Includes/tonemap_aces2_tables.glsl`) and
its std430 payload
(`Engine/Modules/Rendering/Source/Passes/TonemapAces2Tables.h`), which
TonemapPass uploads once per device. The data deliberately never becomes
shader constants: glslang materializes dynamically indexed const arrays into
per-invocation scratch memory, which at this size faults the device (design
doc B4; `Aces2PipelineStatsTests` pins this at pipeline creation).

### Invocation

```bash
py -3.12 -m venv .venv && .venv/Scripts/pip install -r requirements.txt
.venv/Scripts/python gen_aces2_tables.py \
    ../../Engine/Modules/Rendering/Shaders/Includes/tonemap_aces2_tables.glsl \
    ../../Engine/Modules/Rendering/Source/Passes/TonemapAces2Tables.h
```

### The OCIO pin (load-bearing)

`requirements.txt` pins `opencolorio==2.5.2` and the generator asserts both the
version and the 363-entry table layout at runtime. 2.5.0/2.4.x emit
**362**-entry tables — a different layout, rejected. All six tiers must come
from one OCIO build; mixing layouts or builds across tiers is a defect. The
generator also asserts every invariant the F1 blend relies on: the six
closed-form scalars against OCIO's emitted per-tier values (rel ≤ 1e-5), the
Mnorm common scale factor (≤ 1e-9), sign structure for every geometric mix,
and the cusp-search window literal (OCIO emits `i + 3` for peaks near
2200–2900; no shipped tier may carry it).

### Platform rounding — validate outputs, never diff bytes

The emitted constants carry the generating build's floating-point rounding.
The tables in-tree are canonical from the win-x64 PyPI wheel; the same version
regenerated on another platform (e.g. macOS ARM64) drifts at last-ulp scale in
the iteratively-derived cusp/hue tables (measured up to ~1.7e-4 relative
against a mac-generated table, with byte-level differences across ~40% of
entries) while being numerically equivalent in output (measured ≤ 4e-6).
After regenerating anywhere, run the gates rather than comparing bytes.

### Gates (mirror of the design doc; run all five)

```bash
.venv/Scripts/python validate_aces2_tiers.py check  <TonemapAces2Tables.h> <body.glsl>
.venv/Scripts/python validate_aces2_tiers.py parity <previous_or_reference.glsl> <TonemapAces2Tables.h> <body.glsl>
.venv/Scripts/python validate_aces2_tiers.py blend  <TonemapAces2Tables.h> <body.glsl>
.venv/Scripts/python validate_aces2_tiers.py edges  <TonemapAces2Tables.h> <body.glsl>
.venv/Scripts/python validate_aces2_tiers.py tokens <body.glsl>
```

- **G1 `check`** — the validator's numpy port vs the OCIO CPU processor at each
  tier (t=0); worst relative error ≤ 1e-3 (measures ~8e-6).
- **G2 `parity`** — tier-0/t=0 output vs a reference single-tier shader
  (regeneration/platform drift bound); worst relative error ≤ 2e-4.
- **G3/G4/G6 `blend`** — dense probes (240 neutral + 96 hues × 8 sats × 48
  luminances) at t ∈ {0.10, 0.25, 0.35, 0.50, 0.60, 0.65, 0.75, 0.90} per rung
  vs exact OCIO generation: p99.9 relative ≤ 1.0% (G3); ΔE_ITP (BT.2124)
  p99.9 ≤ 0.5 JND with the max reported (G4); every >3-JND probe must straddle
  the reference's own proportion==1 discontinuity at its own saturation and
  luminance, zero off-locus (G6). There is deliberately no supremum-style
  gate: the reference's own step discontinuity makes a pointwise worst-case
  unachievable for any tiering scheme — see the design doc.
- **G5 `edges`** — full-circle census (0–360°, 0.002° step) of the
  proportion==1 edges at L ∈ {1, 2, 4} per rung midpoint: an edge with a
  ≥ 0.5-JND step across it must have a blended counterpart within 0.25°;
  sub-0.5-JND (invisible) edges are bounded at 2.0° and may appear/vanish.
- **`tokens`** — the hand-maintained transform body equals OCIO 2.5.2's tier-0
  emission with exactly the mechanical tier parameterization applied; 0 diffs.

The validator's `tier_at()` mirrors `Aces2SelectTier` shape-for-shape and pins
the shader's ENTIRE tier-blend machinery as one contiguous exact-text block
(`_LOCKSTEP_BLOCK`; line-level pins missed 5/8 review mutants, the block
catches all 8); change the two together.

### Changing the pin

Bumping OCIO regenerates every tier AND requires re-deriving the hand-
maintained body of `tonemap_aces2.glsl` from the new emission (the generator's
`bake_tier` output at peak 100), re-applying the tier parameterization: table
lookups through the `Aces2*` accessors, the 12 emitted scalars through
`aces2_p.*`, and the six closed forms in `Aces2SelectTier` — the generator's
structural asserts (18 located scalars; 16 varying `100.` literals + 2 fixed;
closed-form and Mnorm invariants) fail loudly if the upstream shape changed.
