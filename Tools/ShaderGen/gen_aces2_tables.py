#!/usr/bin/env python3
"""Generates Engine/Modules/Rendering/Shaders/Includes/tonemap_aces2_tables.glsl.

Six ACES 2.0 Rec.709 output transforms at peak luminances 100*2^k nits
(k = 0..5), emitted as tier-indexed GLSL const arrays plus the per-tier
peak-baked scalars, for runtime tier blending in tonemap_aces2.glsl.

Requires opencolorio==2.5.2 EXACTLY (see requirements.txt). The table layout is
version-sensitive: 2.5.1+ emits 363-entry tables (2.5.0/2.4.x emit 362) and the
peak-baked scalar set is located by structural diff, so both the version and
every structural invariant are asserted at runtime. Numeric constants also carry
the generating platform's floating-point rounding at last-ulp scale, so
regeneration on another platform must be validated with
validate_aces2_tiers.py's parity gate, never by diffing bytes. See README.md.

Transform chain per tier (the OCIO ACES-OUTPUT builtin recipe, transcribed from
OpenColorIO v2.5.2 src/OpenColorIO/transforms/builtins/ACES.cpp, wrapped for a
linear Rec.709 renderer):
    Rec.709 -> ACES2065-1
    AP0 -> AP1, clamp[0, ub], AP1 -> AP0        (ub: see upper_bound below)
    FixedFunction ACES_OUTPUT_TRANSFORM_20      (Rec.709 limit + encoding, D65)
    clamp[0, peak/100], limit Rec.709 -> XYZ
    CIE XYZ D65 -> linear Rec.709
At peak 100 this chain is asserted byte-identical to composing OCIO's own
"ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-100nit-REC709_2.0" builtin,
which pins the arbitrary-peak recipe to the reference.

Usage:
    python gen_aces2_tables.py <output.glsl> <output_tables.h>

The .glsl output declares the Aces2Tables SSBO block (set 0, binding 2,
std430); the .h output carries its std430 payload for TonemapPass's one-time
device upload. Shader constants are deliberately NOT emitted: glslang
materializes dynamically indexed const arrays into per-invocation scratch
memory, which at this size faults the device (design doc, B4).
"""
import math
import re
import sys

import numpy as np
import PyOpenColorIO as ocio

REQUIRED_OCIO_VERSION = "2.5.2"
TABLE_SIZE = 363
TIER_PEAKS = [100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0]
SCALAR_COUNT = 18

# The 18 peak-baked scalars located in the emitted bodies, keyed by the tier-0
# (peak 100) value each slot holds. Six of them are closed-form in peak and are
# NOT emitted (Aces2SelectTier computes them; asserted below against the
# emitted values); the EMITTED_SLOTS subset defines the [tier][12] layout.
SLOT_NAMES = [
    ("tsFScale", 1.04710376),
    ("tsSDiv", 0.73009213709383403),
    ("limitJMax", 100.0),
    ("midJ", 34.096539),
    ("focusGainBase", 135.0),
    ("gammaBottomInv", 0.877192974),
    ("inputClampAP1", 1024.0),
    ("mnormCosW.x", 11.341321604032515),
    ("mnormCosW.y", 16.469863649185896),
    ("mnormCosW.z", 7.8842182208776475),
    ("mnormSinW.x", 14.665187919584513),
    ("mnormSinW.y", -6.3725780354404442),
    ("mnormSinW.z", 9.1941277054452897),
    ("mnormOffset", 77.133051547393805),
    ("toeLowScale", 1.29999995),
    ("toeK2Bias", 0.00499999989),
    ("toeHighScale", 2.4000001),
    ("outClamp", 1.0),
]
# Occurrence counts asserted per tier body. limitJMax is the load-bearing one:
# 16 textually-identical "100." literals change with peak while exactly 2 other
# "100." literals (the CAM J scale and the tonescale J reference) must not —
# which is why hand-extraction is forbidden and this script exists.
SLOT_OCCURRENCES = {"limitJMax": 16, "inputClampAP1": 3, "outClamp": 3}

# Emitted [tier][12] layout (the closed-form slots are omitted); mirrored by
# Aces2SelectTier — keep the two in sync.
EMITTED_SLOTS = ["tsFScale", "tsSDiv", "limitJMax", "midJ", "gammaBottomInv",
                 "mnormCosW.x", "mnormCosW.y", "mnormCosW.z",
                 "mnormSinW.x", "mnormSinW.y", "mnormSinW.z", "mnormOffset"]

CLOSED_FORM_TOL = 1e-5
MNORM_FACTOR_TOL = 1e-9
# The seven Mnorm Fourier weights share one peak scale factor (asserted at
# MNORM_FACTOR_TOL); Aces2SelectTier blends the factor once.
MNORM_SLOTS = EMITTED_SLOTS[5:]
GEO_SLOTS = ["tsFScale", "tsSDiv", "limitJMax", "gammaBottomInv"]


def closed_forms(peak):
    """The six scalars Aces2SelectTier derives in closed form (L = log2 of the
    paper-white-relative headroom); asserted against the emitted per-tier
    values at CLOSED_FORM_TOL relative. focusGainBase is limitJMax times an
    L-affine ratio."""
    L = math.log2(peak / 100.0)
    return {
        "inputClampAP1": 8.0 * (128.0 + 768.0 * (math.log(peak / 100.0) / math.log(100.0))),
        "outClamp": peak / 100.0,
        "toeK2Bias": 0.5 / peak,
        "toeHighScale": 2.4000001 + 2.3841577 * L,
        "toeLowScale": max(0.2, 1.29999995 - 0.27002399 * L),
        "focusGainBase": None,  # limitJMax * focus_gain_ratio(L), asserted separately
    }


def focus_gain_ratio(L):
    return 1.35 + 0.71118337 * L


FLOAT_RE = re.compile(r"-?\d+\.\d*(?:[eE][+-]?\d+)?")
# Bare integer literals (loop bounds, the cusp-search init window) are masked in
# the structural comparison and pinned separately: OCIO widens the search window
# to "i + 3" for peaks around 2200-2900, which must never reach a shipped tier.
INT_RE = re.compile(r"(?<![\w.])\d+(?![\w.])")
SEARCH_WINDOW_RE = re.compile(r"float\(i \+ (\d+)\)")


def upper_bound(peak):
    """Pre-transform AP1 clamp from the OCIO v2.5.2 builtin recipe.

    Validated float-exact against the shipped builtins at peaks
    100/500/1000/2000/4000 (1024, 3171.23583984375, 4096, 5020.76416015625,
    5945.5283203125)."""
    return 8.0 * (128.0 + 768.0 * (math.log(peak / 100.0) / math.log(10000.0 / 100.0)))


def fmt(v):
    """OCIO-style float text: up to 9 significant digits, always a decimal point."""
    s = "%.9g" % float(np.float32(v))
    if "." not in s and "e" not in s and "E" not in s:
        s += "."
    return s


_config = ocio.Config.CreateRaw()


def _append_rec709_to_ap0(grp):
    t = ocio.BuiltinTransform("UTILITY - ACES-AP1_to_LINEAR-REC709_BFD")
    t.setDirection(ocio.TRANSFORM_DIR_INVERSE)
    grp.appendTransform(t)
    grp.appendTransform(ocio.BuiltinTransform("UTILITY - ACES-AP1_to_CIE-XYZ-D65_BFD"))
    t = ocio.BuiltinTransform("UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD")
    t.setDirection(ocio.TRANSFORM_DIR_INVERSE)
    grp.appendTransform(t)


def _append_xyz_to_rec709(grp):
    t = ocio.BuiltinTransform("UTILITY - ACES-AP1_to_CIE-XYZ-D65_BFD")
    t.setDirection(ocio.TRANSFORM_DIR_INVERSE)
    grp.appendTransform(t)
    grp.appendTransform(ocio.BuiltinTransform("UTILITY - ACES-AP1_to_LINEAR-REC709_BFD"))


def _builtin_group():
    grp = ocio.GroupTransform()
    _append_rec709_to_ap0(grp)
    grp.appendTransform(ocio.BuiltinTransform(
        "ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-100nit-REC709_2.0"))
    _append_xyz_to_rec709(grp)
    return grp


def _shader_desc(transform):
    proc = _config.getProcessor(transform)
    desc = ocio.GpuShaderDesc.CreateShaderDesc(language=ocio.GPU_LANGUAGE_GLSL_4_0)
    desc.setFunctionName("TonemapACES2Generated")
    desc.setResourcePrefix("aces2")
    proc.getDefaultGPUProcessor().extractGpuShaderInfo(desc)
    return desc


def _recipe_matrices():
    """AP0->AP1, AP1->AP0 and limitRec709->XYZ exactly as the bare SDR builtin
    carries them (row-major m44 lists for ocio.MatrixTransform)."""
    bare = ocio.BuiltinTransform("ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-100nit-REC709_2.0")
    mats = re.findall(r"mat4\(([^)]*)\) \* tmp", _shader_desc(bare).getShaderText())
    assert len(mats) == 3, "SDR builtin op shape changed (expected 3 mat4 ops)"
    out = []
    for m in mats:
        vals = [float(x) for x in m.split(",")]
        assert len(vals) == 16
        rows = [[vals[c * 4 + r] for c in range(4)] for r in range(4)]
        out.append([v for row in rows for v in row])
    return out  # [Rec709->AP1 fold, AP1->AP0, lim709->XYZ]


def build_group(peak, ap0_to_ap1, ap1_to_ap0, lim_to_xyz):
    grp = ocio.GroupTransform()
    _append_rec709_to_ap0(grp)
    grp.appendTransform(ocio.MatrixTransform(ap0_to_ap1))
    rng = ocio.RangeTransform()
    ub = upper_bound(peak)
    rng.setMinInValue(0.0)
    rng.setMinOutValue(0.0)
    rng.setMaxInValue(ub)
    rng.setMaxOutValue(ub)
    grp.appendTransform(rng)
    grp.appendTransform(ocio.MatrixTransform(ap1_to_ap0))
    grp.appendTransform(ocio.FixedFunctionTransform(
        style=ocio.FIXED_FUNCTION_ACES_OUTPUT_TRANSFORM_20,
        params=[peak, 0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290]))
    rng2 = ocio.RangeTransform()
    rng2.setMinInValue(0.0)
    rng2.setMinOutValue(0.0)
    rng2.setMaxInValue(peak / 100.0)
    rng2.setMaxOutValue(peak / 100.0)
    grp.appendTransform(rng2)
    grp.appendTransform(ocio.MatrixTransform(lim_to_xyz))
    _append_xyz_to_rec709(grp)
    return grp


def bake_tier(desc):
    """Shader text with the two LUT samplers baked to const-array indexing, plus
    the raw table data (reach, cusp, hues)."""
    text = desc.getShaderText()
    tables = {}
    for tex in desc.getTextures():
        assert tex.width == TABLE_SIZE, (
            f"table {tex.textureName} has {tex.width} entries, expected {TABLE_SIZE}; "
            f"this OCIO build is not layout-compatible (2.5.0/2.4.x emit 362)")
        vals = list(tex.getValues())
        base = tex.textureName.replace("_0", "")
        single = "RED" in str(tex.channel)
        tables[base] = vals
        n = tex.width
        text = text.replace("uniform sampler1D %s;" % tex.samplerName, "")
        text = text.replace(
            "texture(%s, (float(i_hi) - 1.0 + 0.5) / float(%d)).rgb" % (tex.samplerName, n),
            "%s[i_hi - 1]" % base)
        text = text.replace(
            "texture(%s, (float(i_hi) + 0.5) / float(%d)).rgb" % (tex.samplerName, n),
            "%s[i_hi]" % base)
        text = text.replace(
            "texture(%s, (i_lo + 0.5) / float (%d)).r" % (tex.samplerName, n),
            "%s[int(i_lo)]" % base)
        text = text.replace(
            "texture(%s, (i_hi + 0.5) / float (%d)).r" % (tex.samplerName, n),
            "%s[int(i_hi)]" % base)
        assert single == (base == "aces2_reach_m_table")
    assert "sampler1D" not in text and "texture(" not in text and "Sampler" not in text
    assert set(tables) == {"aces2_reach_m_table", "aces2_gamut_cusp_table"}
    m = re.search(r"const float aces2_gamut_cusp_table_0_hues_array\[(\d+)\] = float\[\d+\]\((.*?)\);",
                  text, re.S)
    assert m, "hues array not found"
    assert int(m.group(1)) == TABLE_SIZE, \
        f"hues array has {m.group(1)} entries, expected {TABLE_SIZE}"
    hues = [float(x) for x in FLOAT_RE.findall(m.group(2))]
    assert len(hues) == TABLE_SIZE
    tables["aces2_hues_table"] = hues
    return text, tables


def locate_scalars(bodies):
    """Positions of the peak-baked literals, verified structurally across tiers.

    Returns per-slot value tuples ordered by SLOT_NAMES."""
    # The hues table is tier data, not a scalar; it is emitted separately.
    hues_decl = re.compile(
        r"const float aces2_gamut_cusp_table_0_hues_array\[\d+\] = float\[\d+\]\(.*?\);", re.S)
    bodies = [hues_decl.sub("HUES_TABLE;", b) for b in bodies]
    masked = [INT_RE.sub("@", FLOAT_RE.sub("#", b)) for b in bodies]
    assert all(m == masked[0] for m in masked[1:]), \
        "tier bodies differ structurally; the scalar model does not hold"
    # Integer literals are masked above, so pin the one that is known to vary
    # with peak: every tier's cusp-search init window must match tier 0's
    # (the hand-maintained body hardcodes it).
    windows = [SEARCH_WINDOW_RE.findall(b) for b in bodies]
    assert all(w == windows[0] for w in windows), \
        f"cusp-search window varies across tiers: {windows} (OCIO emits i+3 near peaks 2200-2900)"
    assert windows[0] == ["0", "2"], \
        f"cusp-search window {windows[0]} does not match the hand-maintained body's 'i + 0'/'i + 2'"
    streams = [[float(x) for x in FLOAT_RE.findall(b)] for b in bodies]
    n = len(streams[0])
    assert all(len(s) == n for s in streams)
    groups = {}
    for i in range(n):
        tup = tuple(s[i] for s in streams)
        if len(set(tup)) > 1:
            groups.setdefault(tup, []).append(i)
    assert len(groups) == SCALAR_COUNT, \
        f"expected {SCALAR_COUNT} peak-baked scalars, found {len(groups)}: " + \
        ", ".join(fmt(k[0]) for k in groups)
    slots = [None] * SCALAR_COUNT
    for tup, positions in groups.items():
        matches = [idx for idx, (_, v0) in enumerate(SLOT_NAMES)
                   if abs(tup[0] - v0) <= 1e-9 * max(abs(v0), 1.0)]
        assert len(matches) == 1, f"tier-0 value {tup[0]!r} matched slots {matches}"
        assert slots[matches[0]] is None
        slots[matches[0]] = tup
        name = SLOT_NAMES[matches[0]][0]
        if name in SLOT_OCCURRENCES:
            assert len(positions) == SLOT_OCCURRENCES[name], \
                f"{name}: {len(positions)} occurrences, expected {SLOT_OCCURRENCES[name]}"
    assert all(s is not None for s in slots)
    fixed_100s = [i for i in range(n)
                  if streams[0][i] == 100.0 and len({s[i] for s in streams}) == 1]
    assert len(fixed_100s) == 2, \
        f"expected exactly 2 fixed '100.' literals, found {len(fixed_100s)}"
    return slots


def generate_tiers(peaks=TIER_PEAKS):
    """(bodies, tier_tables) for the given peaks — also used by the validator."""
    ap0_to_ap1, ap1_to_ap0, lim_to_xyz = _recipe_matrices()
    bodies, tier_tables = [], []
    for peak in peaks:
        body, tables = bake_tier(_shader_desc(build_group(peak, ap0_to_ap1, ap1_to_ap0, lim_to_xyz)))
        bodies.append(body)
        tier_tables.append(tables)
    return bodies, tier_tables


def cpu_processor(peak):
    """OCIO CPU processor for the full chain at an arbitrary peak (validator ground truth)."""
    ap0_to_ap1, ap1_to_ap0, lim_to_xyz = _recipe_matrices()
    grp = build_group(peak, ap0_to_ap1, ap1_to_ap0, lim_to_xyz)
    return _config.getProcessor(grp).getDefaultCPUProcessor()


def assert_version():
    assert ocio.__version__ == REQUIRED_OCIO_VERSION, (
        f"OpenColorIO {REQUIRED_OCIO_VERSION} required, found {ocio.__version__}. "
        f"2.5.0/2.4.x emit 362-entry tables and are rejected; install via "
        f"requirements.txt.")


def assert_blend_model(slots, tier_tables):
    """Every invariant Aces2SelectTier's F1 blend relies on, against the
    emitted per-tier truth."""
    name_to_idx = {n: i for i, (n, _) in enumerate(SLOT_NAMES)}

    def val(name, tier):
        return slots[name_to_idx[name]][tier]

    for tier, peak in enumerate(TIER_PEAKS):
        forms = closed_forms(peak)
        for name, predicted in forms.items():
            if predicted is None:
                predicted = val("limitJMax", tier) * focus_gain_ratio(math.log2(peak / 100.0))
                name = "focusGainBase"
            actual = val(name, tier)
            rel = abs(predicted - actual) / max(abs(actual), 1e-300)
            assert rel <= CLOSED_FORM_TOL, \
                f"closed form {name} tier {tier}: predicted {predicted!r} vs emitted {actual!r} (rel {rel:.2e})"
        # Sign structure required by the geometric mixes.
        for name in GEO_SLOTS:
            assert val(name, tier) > 0.0, f"{name} tier {tier} not positive"
        assert val("mnormSinW.y", tier) < 0.0, f"mnormSinW.y tier {tier} not negative"
        for name in MNORM_SLOTS:
            same = (val(name, tier) > 0.0) == (val(name, 0) > 0.0)
            assert same and val(name, tier) != 0.0, f"{name} tier {tier} changed sign"
        tabs = tier_tables[tier]
        assert min(tabs["aces2_reach_m_table"]) > 0.0, f"reach table tier {tier} not positive"
        cusp = tabs["aces2_gamut_cusp_table"]
        assert min(cusp[0::3]) > 0.0 and min(cusp[1::3]) > 0.0, \
            f"cusp J/M tier {tier} not positive"
    # The seven Mnorm weights must share one scale factor per tier.
    for tier in range(len(TIER_PEAKS)):
        ratios = [val(n, tier) / val(n, 0) for n in MNORM_SLOTS]
        dev = max(abs(r - ratios[0]) for r in ratios)
        assert dev <= MNORM_FACTOR_TOL, \
            f"mnorm common-factor invariant broken at tier {tier}: deviation {dev:.2e}"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out_path = sys.argv[1]
    header_path = sys.argv[2]
    assert_version()

    bodies, tier_tables = generate_tiers()

    # Equivalence pin: at peak 100 the arbitrary-peak chain must reproduce the
    # shipped SDR builtin byte-for-byte.
    ref_body, _ = bake_tier(_shader_desc(_builtin_group()))
    assert bodies[0] == ref_body, \
        "peak-100 chain no longer matches the SDR-100nit-REC709 builtin"

    slots = locate_scalars(bodies)
    assert_blend_model(slots, tier_tables)

    nt = len(TIER_PEAKS)
    name_to_idx = {n: i for i, (n, _) in enumerate(SLOT_NAMES)}
    ne = len(EMITTED_SLOTS)

    # ---- GLSL: the Aces2Tables SSBO block declaration (data lives in the C++
    # header and is uploaded once by TonemapPass — dynamically indexed const
    # arrays are materialized per-invocation into scratch memory by glslang,
    # which both costs ~10 ns/px and faults the device at this size).
    lines = []
    w = lines.append
    w("// Generated by Tools/ShaderGen/gen_aces2_tables.py - DO NOT EDIT.")
    w("// The Aces2Tables device buffer: OpenColorIO %s ACES 2.0 Rec.709" % REQUIRED_OCIO_VERSION)
    w("// output-transform tables at peak luminances 100*2^k nits, k = 0..5, plus")
    w("// the per-tier peak-baked scalars that are not closed-form in peak. The")
    w("// data is generated into TonemapAces2Tables.h and uploaded once per device")
    w("// (TonemapPass); tonemap_aces2.glsl blends adjacent tiers at runtime. The")
    w("// aces2_tier_scalars slot layout is defined by EMITTED_SLOTS in the")
    w("// generator and mirrored by Aces2SelectTier. Member order is vec4-first so")
    w("// std430 packs with zero padding (offsets asserted in the C++ header).")
    w("// SPDX-License-Identifier: BSD-3-Clause")
    w("// Copyright Contributors to the OpenColorIO Project.")
    w("// See ThirdParty/OpenColorIO/LICENSE.txt and UPSTREAM.md.")
    w("")
    w("#ifndef GE_TONEMAP_ACES2_TABLES_GLSL")
    w("#define GE_TONEMAP_ACES2_TABLES_GLSL")
    w("")
    w("const int kAces2TierCount = %d;" % nt)
    w("")
    w("// [tier][slot] slots: " + ", ".join("%d=%s" % (i, n) for i, n in enumerate(EMITTED_SLOTS)))
    w("layout(set = 0, binding = 2, std430) readonly buffer Aces2Tables")
    w("{")
    w("    vec4  aces2_gamut_cusp_tables[%d][%d]; // xyz = cusp J / cusp M / upper-hull gamma; w unused" % (nt, TABLE_SIZE))
    w("    float aces2_reach_m_tables[%d][%d];" % (nt, TABLE_SIZE))
    w("    float aces2_hues_tables[%d][%d];" % (nt, TABLE_SIZE))
    w("    float aces2_tier_scalars[%d][%d];" % (nt, ne))
    w("};")
    w("")
    w("#endif")
    open(out_path, "w", newline="\n").write("\n".join(lines) + "\n")
    print("wrote %s" % out_path)

    # ---- C++ header: the std430 blob TonemapPass uploads. Same member order as
    # the GLSL block; float count per member asserted below.
    blob = []
    for tables in tier_tables:  # cusp as vec4 (w = 0)
        vals = tables["aces2_gamut_cusp_table"]
        for i in range(TABLE_SIZE):
            blob.extend([vals[3 * i], vals[3 * i + 1], vals[3 * i + 2], 0.0])
    for tables in tier_tables:
        blob.extend(tables["aces2_reach_m_table"])
    for tables in tier_tables:
        blob.extend(tables["aces2_hues_table"])
    for tier in range(nt):
        blob.extend(float(slots[name_to_idx[n]][tier]) for n in EMITTED_SLOTS)
    expected = nt * TABLE_SIZE * 4 + 2 * nt * TABLE_SIZE + nt * ne
    assert len(blob) == expected, (len(blob), expected)

    h = []
    w = h.append
    w("// Generated by Tools/ShaderGen/gen_aces2_tables.py - DO NOT EDIT.")
    w("// std430 payload of tonemap_aces2_tables.glsl's Aces2Tables block, uploaded")
    w("// once per device by TonemapPass. Member order matches the block: cusp vec4")
    w("// [%d][%d] @ 0, reach [%d][%d] @ %d, hues @ %d, scalars [%d][%d] @ %d." % (
        nt, TABLE_SIZE, nt, TABLE_SIZE, nt * TABLE_SIZE * 16,
        nt * TABLE_SIZE * 16 + nt * TABLE_SIZE * 4, nt, ne,
        nt * TABLE_SIZE * 16 + 2 * nt * TABLE_SIZE * 4))
    w("// SPDX-License-Identifier: BSD-3-Clause")
    w("// Copyright Contributors to the OpenColorIO Project.")
    w("// See ThirdParty/OpenColorIO/LICENSE.txt and UPSTREAM.md.")
    w("#pragma once")
    w("")
    w("#include <cstdint>")
    w("")
    w("namespace GameEngine {")
    w("namespace Rendering {")
    w("namespace Passes {")
    w("")
    w("inline constexpr uint32_t kAces2TablesFloatCount = %du;" % expected)
    w("inline constexpr uint32_t kAces2TablesBytes = %du;" % (expected * 4))
    w("")
    per_line = 8
    rows = []
    for i in range(0, len(blob), per_line):
        rows.append("    " + ", ".join(fmt(v) + "f" for v in blob[i:i + per_line]))
    w("inline constexpr float kAces2Tables[kAces2TablesFloatCount] = {")
    w(",\n".join(rows))
    w("};")
    w("")
    w("} // namespace Passes")
    w("} // namespace Rendering")
    w("} // namespace GameEngine")
    open(header_path, "w", newline="\n").write("\n".join(h) + "\n")
    print("wrote %s (%d floats, %d bytes)" % (header_path, expected, expected * 4))
    for tier, peak in enumerate(TIER_PEAKS):
        print("  tier %d: peak %5g nits  ub=%s  outClamp=%s  limitJMax=%s" % (
            tier, peak, fmt(slots[6][tier]), fmt(slots[17][tier]), fmt(slots[2][tier])))


if __name__ == "__main__":
    main()
