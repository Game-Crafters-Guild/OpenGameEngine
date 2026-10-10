#!/usr/bin/env python3
"""Numeric gates for the ACES 2 peak-luminance tier ladder (F1 blend).

Evaluates the tonemap_aces2.glsl transform in numpy (float32, mirroring the
GLSL body statement-for-statement, with matrices/tables/scalars parsed from the
shader sources so the code under test supplies its own constants). tier_at()
mirrors Aces2SelectTier's F1 blend shape-for-shape — six closed forms,
endpoint-exact geometric mixes (a*exp2(t*log2(b/a))) for the power-law slots
and the reach/cusp J/M tables, one geometric Mnorm scale factor applied to the
lower tier's weights, linear mixes for midJ, the cusp upper-hull gamma and the
hue angles. Every gate evaluates through this one function, and
_assert_shader_lockstep pins the shader's literal blend/closed-form text, so
the validator cannot silently diverge from the shader; change Aces2SelectTier
and tier_at together.

Gates. The reference transform's own proportion==1 step discontinuity makes
every pointwise-supremum criterion unachievable, so the tails are gated by
percentile, perceptual error, and the discontinuity's literal geometry:
  G1 check : numpy port vs the OCIO CPU processor at each tier (t=0);
             worst relative error <= 1e-3 (measures ~8e-6).
  G2 parity: tier-0/t=0 output vs a pre-tier single-peak tonemap_aces2.glsl;
             worst relative error <= 2e-4 (platform/regeneration drift bound).
  G3 blend : dense probes at t in {0.10, 0.25, 0.35, 0.50, 0.60, 0.65, 0.75,
             0.90} per rung vs exact OCIO generation at the integer peak
             nearest 100*2^(k+t): p99.9 relative error <= 1.0% on every run.
  G4 blend : dE_ITP (BT.2124, 1.0 = 1 JND) p99.9 <= 0.5 JND on every run;
             the maximum is reported, and its tail is governed by G6's census
             (a max cap is a pointwise supremum — see the design doc).
  G5 edges : full-circle census (0..360 deg, 0.002 deg step) of the gamut
             mapper's proportion==1 edges at L in {1, 2, 4} per rung midpoint:
             each exact-arm edge with a step >= 0.5 JND across it must have a
             blended counterpart within 0.25 deg (see G5_TIGHT_DEG for the
             derivation); edges with sub-0.5-JND steps (invisible boundaries —
             the change across them is below one JND) are bounded loosely at
             2.0 deg and may appear/vanish.
  G6 blend : every probe above 3 JND must straddle the reference's own
             discontinuity (an exact-arm proportion==1 edge within 0.5 deg of
             its hue, swept at the probe's own saturation and luminance);
             zero probes above 3 JND anywhere else.

Dense probe set (pinned): 240 log2-spaced neutral luminances in
[2^-10, 4x headroom] plus 96 hues x 8 saturations (0.15..1.0) x 48 log2-spaced
luminances in [2^-6, 2x headroom]. Relative metric per probe: max-channel
absolute delta over max(reference max-channel, 1% of the reference peak).

Requires the venv from requirements.txt (check/blend/edges generate OCIO
ground truth; parity parses files only but shares the harness).

Usage ("<tables.h>" is the generated TonemapAces2Tables.h — the exact payload
TonemapPass uploads to the Aces2Tables device buffer; tier data no longer
lives in GLSL):
  validate_aces2_tiers.py check  <tables.h> <body.glsl>
  validate_aces2_tiers.py parity <reference_single_tier.glsl> <tables.h> <body.glsl>
  validate_aces2_tiers.py blend  <tables.h> <body.glsl>
  validate_aces2_tiers.py edges  <tables.h> <body.glsl>
  validate_aces2_tiers.py tokens <body.glsl>
      Proves the hand-maintained transform body is OCIO 2.5.2's tier-0 emission
      with exactly the mechanical tier parameterization applied (table lookups
      through the accessors, the 18 peak-baked literals through aces2_p.*,
      nothing else): regenerates the emission, applies the mapping, and
      compares token streams — zero diffs required.
"""
import re
import sys

import numpy as np

FLOAT_RE = re.compile(r"-?\d+\.\d*(?:[eE][+-]?\d+)?")
F = np.float32

TIER_COUNT = 6
EMITTED_SLOT_COUNT = 12
# Closed-form coefficients — must match Aces2SelectTier literally (pinned by
# _assert_shader_lockstep).
CF_INPUT_CLAMP = (1024.0, 924.7641466)
CF_TOE_HIGH = (2.4000001, 2.3841577)
CF_TOE_LOW = (1.29999995, 0.27002399, 0.2)
CF_FOCUS_RATIO = (1.35, 0.71118337)
CF_TOE_K2 = 0.005


def _f32(x):
    return np.asarray(x, dtype=np.float32)


def _parse_mats(text):
    """The two mat4 ops (head fold, AP1->AP0) and four FF mat3s, as row-major
    float32, in order of appearance."""
    mat4s = [np.array([float(v) for v in m.split(",")], dtype=np.float64).reshape(4, 4).T
             for m in re.findall(r"mat4\(([^)]*)\) \* tmp", text)]
    assert len(mat4s) == 2, f"expected 2 mat4 ops, found {len(mat4s)}"
    mat3s = [np.array([float(v) for v in m.split(",")], dtype=np.float64).reshape(3, 3).T
             for m in re.findall(r"mat3\(([^)]*)\) \* ", text)]
    assert len(mat3s) == 4, f"expected 4 mat3 ops, found {len(mat3s)}"
    return [m.astype(np.float32) for m in mat4s], [m.astype(np.float32) for m in mat3s]


def _assert_shader_lockstep(body_text):
    """Pins the ENTIRE tier-blend machinery of tonemap_aces2.glsl (globals,
    Aces2MixGeo, all four accessors and Aces2SelectTier) as one contiguous
    exact-text block, so any semantic mutation inside it fails every gate run
    loudly instead of being silently measured against the unmutated tier_at().
    (Line-level pins missed 5/8 review mutants; a block pin cannot.)
    Regenerate by copying the shader text between `int aces2_tier_lo = 0;` and
    `// Declaration of all helper methods` whenever Aces2SelectTier changes,
    and change tier_at() below to match."""
    assert _LOCKSTEP_BLOCK in body_text, (
        "tonemap_aces2.glsl's tier-blend machinery does not match the "
        "validator's pinned block; update _LOCKSTEP_BLOCK and tier_at() "
        "together (B3 lockstep)")


_LOCKSTEP_BLOCK = "int aces2_tier_lo = 0;\nint aces2_tier_hi = 0;\nfloat aces2_tier_t = 0.0;\nAces2TierParams aces2_p;\n\n// Endpoint-exact geometric mix: returns a exactly at t = 0 (exp2(0) == 1), so\n// SDR stays byte-identical to tier 0. Valid only for same-signed nonzero\n// pairs — the generator asserts positivity for every geo-blended slot and\n// table entry, and constant per-slot signs for the Mnorm weights.\nfloat Aces2MixGeo(float a, float b, float t)\n{\n    return a * exp2(t * log2(b / a));\n}\n\n// Reach M is a power law of peak: geometric per entry.\nfloat Aces2ReachM(int i)\n{\n    return Aces2MixGeo(aces2_reach_m_tables[aces2_tier_lo][i], aces2_reach_m_tables[aces2_tier_hi][i], aces2_tier_t);\n}\n\n// Cusp J and M are power laws of peak (geometric); the upper-hull gamma in .z\n// is log2-affine (linear). Entries are vec4 in the buffer (std430 packing);\n// .w is unused.\nvec3 Aces2GamutCusp(int i)\n{\n    vec4 lo = aces2_gamut_cusp_tables[aces2_tier_lo][i];\n    vec4 hi = aces2_gamut_cusp_tables[aces2_tier_hi][i];\n    return vec3(Aces2MixGeo(lo.x, hi.x, aces2_tier_t),\n                Aces2MixGeo(lo.y, hi.y, aces2_tier_t),\n                mix(lo.z, hi.z, aces2_tier_t));\n}\n\n// Hue positions are angles: always linear, never geometric.\nfloat Aces2CuspHue(int i)\n{\n    return mix(aces2_hues_tables[aces2_tier_lo][i], aces2_hues_tables[aces2_tier_hi][i], aces2_tier_t);\n}\n\nfloat Aces2ScalarLin(int slot)\n{\n    return mix(aces2_tier_scalars[aces2_tier_lo][slot], aces2_tier_scalars[aces2_tier_hi][slot], aces2_tier_t);\n}\n\nfloat Aces2ScalarGeo(int slot)\n{\n    return Aces2MixGeo(aces2_tier_scalars[aces2_tier_lo][slot], aces2_tier_scalars[aces2_tier_hi][slot], aces2_tier_t);\n}\n\n// Selects the tier pair bracketing the display headroom and derives the\n// peak-baked parameters (the F1 blend). The ladder is 100*2^k nit OCIO peaks,\n// so idx = log2 of the paper-white-relative headroom is the tier coordinate;\n// SDR (outputMax 1.0) lands exactly on tier 0 with t = 0 and every parameter\n// equal to the tier-0 table value, and panels beyond 32x headroom saturate on\n// the last tier.\n//\n// Closed forms (generator-asserted against OCIO's emitted per-tier values at\n// rel 1e-5; coefficients from the design doc):\n//   inputClampAP1 = 8*(128 + 768*log10(peak/100)/2) = 1024 + 924.7641466*idx\n//   outClamp      = peak/100                        = exp2(idx)\n//   toeK2Bias     = 0.5/peak                        = 0.005*exp2(-idx)\n//   toeHighScale  = 2.4000001 + 2.3841577*idx\n//   toeLowScale   = max(0.2, 1.29999995 - 0.27002399*idx)\n//   focusGainBase = limitJMax * (1.35 + 0.71118337*idx)\n// The seven Mnorm Fourier weights share one peak scale factor\n// (generator-asserted), blended geometrically once from the mnormOffset slot\n// and applied to the lower tier's weights, preserving each weight's sign.\nvoid Aces2SelectTier(float outputMax)\n{\n    float idx = clamp(log2(max(outputMax, 1.0)), 0.0, 5.0);\n    int lo = int(min(idx, 4.0));\n    aces2_tier_lo = lo;\n    aces2_tier_hi = lo + 1;\n    aces2_tier_t = idx - float(lo);\n    aces2_p.tsFScale       = Aces2ScalarGeo(0);\n    aces2_p.tsSDiv         = Aces2ScalarGeo(1);\n    aces2_p.limitJMax      = Aces2ScalarGeo(2);\n    aces2_p.midJ           = Aces2ScalarLin(3);\n    aces2_p.gammaBottomInv = Aces2ScalarGeo(4);\n    float mnormScale = exp2(aces2_tier_t * log2(aces2_tier_scalars[aces2_tier_hi][11] / aces2_tier_scalars[lo][11]));\n    aces2_p.mnormCosW      = vec3(aces2_tier_scalars[lo][5], aces2_tier_scalars[lo][6], aces2_tier_scalars[lo][7]) * mnormScale;\n    aces2_p.mnormSinW      = vec3(aces2_tier_scalars[lo][8], aces2_tier_scalars[lo][9], aces2_tier_scalars[lo][10]) * mnormScale;\n    aces2_p.mnormOffset    = aces2_tier_scalars[lo][11] * mnormScale;\n    aces2_p.inputClampAP1  = 1024.0 + 924.7641466 * idx;\n    aces2_p.outClamp       = exp2(idx);\n    aces2_p.toeK2Bias      = 0.005 * exp2(-idx);\n    aces2_p.toeHighScale   = 2.4000001 + 2.3841577 * idx;\n    aces2_p.toeLowScale    = max(0.2, 1.29999995 - 0.27002399 * idx);\n    aces2_p.focusGainBase  = aces2_p.limitJMax * (1.35 + 0.71118337 * idx);\n}\n"


class TierData:
    """Concrete parameters + tables for one evaluation."""

    FIELDS = ("tsFScale", "tsSDiv", "limitJMax", "midJ", "focusGainBase",
              "gammaBottomInv", "inputClampAP1", "mnormCosW", "mnormSinW",
              "mnormOffset", "toeLowScale", "toeK2Bias", "toeHighScale", "outClamp")

    def __init__(self, reach, cusp, hues, **params):
        self.reach = _f32(reach)
        self.cusp = _f32(cusp).reshape(-1, 3)
        self.hues = _f32(hues)
        self.n = len(self.reach)
        for f in self.FIELDS:
            setattr(self, f, params[f])


class RawTier:
    def __init__(self, reach, cusp, hues, slots):
        self.reach = _f32(reach)
        self.cusp = _f32(cusp).reshape(-1, 3)
        self.hues = _f32(hues)
        self.s = _f32(slots)
        assert len(self.s) == EMITTED_SLOT_COUNT


def _parse_table(text, name):
    m = re.search(re.escape(name) + r"\[(\d+)\](?:\[\d+\])? = \w+\[\d+\](?:\[\d+\])?\((.*?)\);",
                  text, re.S)
    if not m:
        return None
    vals = np.array([float(x) for x in FLOAT_RE.findall(m.group(2))], dtype=np.float32)
    return vals, int(m.group(1))


TABLE_SIZE = 363


def load_tiered(tables_header_path):
    """Tier data from the generated TonemapAces2Tables.h payload — the exact
    bytes TonemapPass uploads to the Aces2Tables device buffer (std430,
    cusp-as-vec4 first, then reach, hues, scalars, matching the GLSL block)."""
    text = open(tables_header_path, encoding="utf-8").read()
    m = re.search(r"kAces2Tables\[kAces2TablesFloatCount\] = \{(.*?)\};", text, re.S)
    assert m, "kAces2Tables payload not found — pass the generated TonemapAces2Tables.h"
    blob = np.array([float(x) for x in re.findall(r"(-?\d+\.\d*(?:[eE][+-]?\d+)?)f", m.group(1))],
                    dtype=np.float32)
    n_cusp = TIER_COUNT * TABLE_SIZE * 4
    n_tab = TIER_COUNT * TABLE_SIZE
    n_scal = TIER_COUNT * EMITTED_SLOT_COUNT
    assert len(blob) == n_cusp + 2 * n_tab + n_scal, \
        f"payload has {len(blob)} floats, expected {n_cusp + 2 * n_tab + n_scal}"
    cusp4 = blob[:n_cusp].reshape(TIER_COUNT, TABLE_SIZE, 4)
    reach = blob[n_cusp:n_cusp + n_tab].reshape(TIER_COUNT, TABLE_SIZE)
    hues = blob[n_cusp + n_tab:n_cusp + 2 * n_tab].reshape(TIER_COUNT, TABLE_SIZE)
    scal = blob[n_cusp + 2 * n_tab:].reshape(TIER_COUNT, EMITTED_SLOT_COUNT)
    out = []
    for k in range(TIER_COUNT):
        out.append(RawTier(reach[k], cusp4[k, :, :3].reshape(-1), hues[k], scal[k]))
    return out


def tier_at(tiers, idx_float):
    """Aces2SelectTier's F1 blend, shape-for-shape in float32."""
    idx = F(np.clip(idx_float, 0.0, 5.0))
    lo = int(min(float(idx), 4.0))
    t = F(idx - F(lo))
    a, b = tiers[lo], tiers[lo + 1]

    def geo(x, y):  # mirrors Aces2MixGeo: endpoint-exact at t = 0
        return (x * np.exp2(t * np.log2(y / x))).astype(np.float32)

    def lin(x, y):  # mirrors GLSL mix()
        return (x * (F(1.0) - t) + y * t).astype(np.float32)

    limitJMax = geo(a.s[2], b.s[2])
    mnormScale = np.exp2(t * np.log2(b.s[11] / a.s[11])).astype(np.float32)
    reach = geo(a.reach, b.reach)
    cusp = np.stack([geo(a.cusp[:, 0], b.cusp[:, 0]),
                     geo(a.cusp[:, 1], b.cusp[:, 1]),
                     lin(a.cusp[:, 2], b.cusp[:, 2])], axis=1)
    hues = lin(a.hues, b.hues)
    return TierData(
        reach, cusp, hues,
        tsFScale=geo(a.s[0], b.s[0]),
        tsSDiv=geo(a.s[1], b.s[1]),
        limitJMax=limitJMax,
        midJ=lin(a.s[3], b.s[3]),
        gammaBottomInv=geo(a.s[4], b.s[4]),
        mnormCosW=(a.s[5:8] * mnormScale).astype(np.float32),
        mnormSinW=(a.s[8:11] * mnormScale).astype(np.float32),
        mnormOffset=np.float32(a.s[11] * mnormScale),
        inputClampAP1=F(CF_INPUT_CLAMP[0]) + F(CF_INPUT_CLAMP[1]) * idx,
        outClamp=np.float32(np.exp2(idx)),
        toeK2Bias=np.float32(F(CF_TOE_K2) * np.exp2(-idx)),
        toeHighScale=F(CF_TOE_HIGH[0]) + F(CF_TOE_HIGH[1]) * idx,
        toeLowScale=np.float32(np.maximum(F(CF_TOE_LOW[2]), F(CF_TOE_LOW[0]) - F(CF_TOE_LOW[1]) * idx)),
        focusGainBase=np.float32(limitJMax * (F(CF_FOCUS_RATIO[0]) + F(CF_FOCUS_RATIO[1]) * idx)))


def load_single_tier(path):
    """A pre-tier single-peak tonemap_aces2.glsl (362-entry layout, inline
    scalar literals) as TierData + its matrices."""
    text = open(path).read()
    reach, _ = _parse_table(text, "aces2_reach_m_table")
    cusp, _ = _parse_table(text, "aces2_gamut_cusp_table")
    hues, _ = _parse_table(text, "aces2_gamut_cusp_table_0_hues_array")

    def grab(pattern):
        m = re.search(pattern, text)
        assert m, pattern
        return F(float(m.group(1)))

    num = r"(-?\d+\.\d*(?:[eE][+-]?\d+)?)"
    clamps = re.findall(r"min\(vec3\(" + num + r", [\d.eE+-]+, [\d.eE+-]+\), outColor\.rgb\)", text)
    assert len(clamps) == 2, clamps
    cw = re.search(r"vec3 cosine_weights = vec3\(([^)]*)\)", text).group(1)
    sw = re.search(r"vec3 sine_weights = vec3\(([^)]*)\)", text).group(1)
    return TierData(
        reach, cusp, hues,
        tsFScale=grab(r"float f = " + num + r" \* pow\(Y / \(Y \+ "),
        tsSDiv=grab(r"float f = [\d.]+ \* pow\(Y / \(Y \+ " + num),
        limitJMax=grab(r"float thr = mix\(cuspJ, " + num),
        midJ=grab(r"float focusJ = mix\(JMcusp\.r, " + num),
        focusGainBase=grab(r"float slope_gain = " + num + r" \* aces2_get_focus_gain0"),
        gammaBottomInv=grab(r"float gamma_bottom_inv = " + num),
        inputClampAP1=F(float(clamps[0])),
        outClamp=F(float(clamps[1])),
        mnormCosW=_f32([float(v) for v in cw.split(",")]),
        mnormSinW=_f32([float(v) for v in sw.split(",")]),
        mnormOffset=grab(r"dot\(sines, sine_weights\) \+ " + num),
        toeLowScale=grab(r"snJ \* " + num + r", sqrt\(nJ \* nJ \+ "),
        toeK2Bias=grab(r"sqrt\(nJ \* nJ \+ " + num),
        toeHighScale=grab(r"aces2_toe_fwd0\(M_cp, limit, nJ \* " + num)), _parse_mats(text)


def eval_aces2(rgb, mats, tier, return_diag=False):
    """The tonemap_aces2.glsl transform, statement-for-statement in float32.

    rgb: (N,3) scene-linear Rec.709. Returns (N,3) display-linear, plus the
    gamut mapper's no_remap/passthrough masks when return_diag."""
    mat4s, mat3s = mats
    p = tier
    v = _f32(rgb)
    v = (mat4s[0][:3, :3] @ v.T).T.astype(np.float32)
    v = np.clip(v, F(0.0), p.inputClampAP1)
    v = (mat4s[1][:3, :3] @ v.T).T.astype(np.float32)

    # RGB (AP0) -> JMh
    lms = (mat3s[0] @ v.T).T.astype(np.float32)
    F_L_v = np.power(np.abs(lms), F(0.419999987), dtype=np.float32)
    rgb_a = (np.sign(lms) * F_L_v) / (F(27.1299992) + F_L_v)
    Aab = (mat3s[1] @ rgb_a.T).T.astype(np.float32)
    J = np.where(Aab[:, 0] <= 0.0, F(0.0),
                 F(100.0) * np.power(np.maximum(Aab[:, 0], F(1e-30)), F(1.13705599), dtype=np.float32))
    M = np.where(J == 0.0, F(0.0), np.sqrt(Aab[:, 1] ** 2 + Aab[:, 2] ** 2, dtype=np.float32))
    h = np.where(Aab[:, 1] == 0.0, F(0.0),
                 np.arctan2(Aab[:, 2], Aab[:, 1], dtype=np.float32) * F(57.29577951308238))
    h = h - np.floor(h / F(360.0)) * F(360.0)
    h = np.where(h < 0.0, h + F(360.0), h).astype(np.float32)
    zero_mask = Aab[:, 0] <= 0.0
    J = np.where(zero_mask, F(0.0), J)
    M = np.where(zero_mask, F(0.0), M)
    h = np.where(zero_mask, F(0.0), h)

    h_rad = h * F(0.0174532924)
    cos_hr = np.cos(h_rad, dtype=np.float32)
    sin_hr = np.sin(h_rad, dtype=np.float32)

    # ToneScale (fwd)
    A = F(0.0323680267) * np.power(np.abs(J) * F(0.00999999978), F(0.879464149), dtype=np.float32)
    Y = np.power((F(27.1299992) * A) / np.maximum(F(1.0) - A, F(1e-10)), F(2.3809523809523809), dtype=np.float32)
    f = p.tsFScale * np.power(Y / (Y + p.tsSDiv), F(1.14999998), dtype=np.float32)
    Y_ts = np.maximum(F(0.0), f * f / (f + F(0.0399999991)))
    F_L_Y = np.power(F(0.79370057210326195) * Y_ts, F(0.42), dtype=np.float32)
    J_ts = F(100.0) * np.power((F_L_Y / (F(27.1299992) + F_L_Y)) * F(30.8946857), F(1.13705599), dtype=np.float32)
    J_ts = (np.sign(J) * J_ts).astype(np.float32)

    # reach table sample
    i_base = np.floor(h).astype(np.float32)
    i_lo = (i_base + F(1.0)).astype(np.int32)
    i_hi = i_lo + 1
    reach_lo = p.reach[i_lo]
    reach_hi = p.reach[i_hi]
    t_r = h - i_base
    reachMaxM = (reach_lo * (F(1.0) - t_r) + reach_hi * t_r).astype(np.float32)

    # ChromaCompress (fwd)
    def toe_fwd(x, limit, k1_in, k2_in):
        k2 = np.maximum(k2_in, F(0.001))
        k1 = np.sqrt(k1_in * k1_in + k2 * k2, dtype=np.float32)
        k3 = (limit + k1) / (limit + k2)
        expanded = F(0.5) * (k3 * x - k1 + np.sqrt((k3 * x - k1) * (k3 * x - k1) + F(4.0) * k2 * k3 * x, dtype=np.float32))
        return np.where(x > limit, x, expanded).astype(np.float32)

    nJ = J_ts / p.limitJMax
    snJ = np.maximum(F(0.0), F(1.0) - nJ)
    cos_hr2 = F(2.0) * cos_hr * cos_hr - F(1.0)
    sin_hr2 = F(2.0) * cos_hr * sin_hr
    cos_hr3 = F(4.0) * cos_hr ** 3 - F(3.0) * cos_hr
    sin_hr3 = F(3.0) * sin_hr - F(4.0) * sin_hr ** 3
    Mnorm = (cos_hr * p.mnormCosW[0] + cos_hr2 * p.mnormCosW[1] + cos_hr3 * p.mnormCosW[2] +
             sin_hr * p.mnormSinW[0] + sin_hr2 * p.mnormSinW[1] + sin_hr3 * p.mnormSinW[2] +
             p.mnormOffset).astype(np.float32)
    limit = np.power(nJ, F(0.879464149), dtype=np.float32) * reachMaxM / Mnorm
    with np.errstate(divide="ignore", invalid="ignore"):
        M_cp = M * np.power(np.where(J > 0, J_ts / np.maximum(J, F(1e-30)), F(1.0)), F(0.879464149), dtype=np.float32)
    M_cp = M_cp / Mnorm
    M_cp = limit - toe_fwd(limit - M_cp, limit - F(0.001), snJ * p.toeLowScale,
                           np.sqrt(nJ * nJ + p.toeK2Bias, dtype=np.float32))
    M_cp = toe_fwd(M_cp, limit, nJ * p.toeHighScale, snJ)
    M_cp = (M_cp * Mnorm).astype(np.float32)
    M_cp = np.where(M != 0.0, M_cp, M).astype(np.float32)
    J_work, M_work, h_work = J_ts, M_cp, h

    # cusp table sample (binary search over hues, mirroring the emitted loop)
    i = np.floor(h_work).astype(np.int32) + 1
    i_lo = np.maximum(0, i)
    i_hi = np.minimum(361, i + 2)
    for _ in range(12):
        active = (i_lo + 1) < i_hi
        if not active.any():
            break
        hcur = p.hues[np.clip(i, 0, p.n - 1)]
        go_up = h_work > hcur
        i_lo = np.where(active & go_up, i, i_lo)
        i_hi = np.where(active & ~go_up, i, i_hi)
        i = np.where(active, (i_lo + i_hi) // 2, i)
    cusp_lo = p.cusp[i_hi - 1]
    cusp_hi = p.cusp[i_hi]
    t_c = ((h_work - p.hues[i_hi - 1]) / (p.hues[i_hi] - p.hues[i_hi - 1])).astype(np.float32)
    JMGcusp = (cusp_lo * (F(1.0) - t_c)[:, None] + cusp_hi * t_c[:, None]).astype(np.float32)

    # GamutCompress (fwd)
    def solve_J_intersect(Jv, Mv, focusJ, slope_gain):
        M_scaled = Mv / slope_gain
        a = M_scaled / focusJ
        b_lo = F(1.0) - M_scaled
        c_lo = -Jv
        b_hi = -(F(1.0) + M_scaled + p.limitJMax * a)
        c_hi = p.limitJMax * M_scaled + Jv
        b = np.where(Jv < focusJ, b_lo, b_hi).astype(np.float32)
        c = np.where(Jv < focusJ, c_lo, c_hi).astype(np.float32)
        det = b * b - F(4.0) * a * c
        root = np.sqrt(np.maximum(det, F(0.0)), dtype=np.float32)
        denom = np.where(Jv < focusJ, b + root, b - root)
        return (F(-2.0) * c / denom).astype(np.float32)

    JMcuspJ, JMcuspM = JMGcusp[:, 0], JMGcusp[:, 1]
    focusJ = JMcuspJ + (p.midJ - JMcuspJ) * np.minimum(F(1.0), F(1.3) - (JMcuspJ / p.limitJMax))
    focusJ = focusJ.astype(np.float32)
    thr = JMcuspJ + (p.limitJMax - JMcuspJ) * F(0.3)
    with np.errstate(divide="ignore", invalid="ignore"):
        gain = (p.limitJMax - thr) / np.maximum(F(0.0001), p.limitJMax - J_work)
        gain = (np.log(gain, dtype=np.float32) / np.log(F(10.0))).astype(np.float32)
    focus_gain = np.where(J_work > thr, gain * gain + F(1.0), F(1.0)).astype(np.float32)
    slope_gain = p.focusGainBase * focus_gain
    J_int_src = solve_J_intersect(J_work, M_work, focusJ, slope_gain)
    gamut_slope = np.where(J_int_src < focusJ, J_int_src, p.limitJMax - J_int_src)
    gamut_slope = (gamut_slope * (J_int_src - focusJ) / (focusJ * slope_gain)).astype(np.float32)
    J_int_cusp = solve_J_intersect(JMcuspJ, JMcuspM, focusJ, slope_gain)
    gamma_top_inv = JMGcusp[:, 2]
    with np.errstate(divide="ignore", invalid="ignore"):
        M_b_lower = J_int_cusp * np.power(np.maximum(J_int_src / J_int_cusp, F(1e-30)), p.gammaBottomInv, dtype=np.float32) \
            / (JMcuspJ / JMcuspM - gamut_slope)
        M_b_upper = JMcuspM * (p.limitJMax - J_int_cusp) * \
            np.power(np.maximum((p.limitJMax - J_int_src) / (p.limitJMax - J_int_cusp), F(1e-30)), gamma_top_inv, dtype=np.float32) \
            / (gamut_slope * JMcuspM + p.limitJMax - JMcuspJ)
    s = F(0.119999997) * JMcuspM
    h_smin = np.maximum(s - np.abs(M_b_lower - M_b_upper), F(0.0)) / s
    gamutBoundaryM = np.minimum(M_b_lower, M_b_upper) - h_smin ** 3 * s * F(0.16666666666666666)
    gamutBoundaryM = gamutBoundaryM.astype(np.float32)
    with np.errstate(divide="ignore", invalid="ignore"):
        reachBoundaryM = p.limitJMax * np.power(np.maximum(J_int_src / p.limitJMax, F(1e-30)), F(0.879464149), dtype=np.float32)
        reachBoundaryM = (reachBoundaryM / ((p.limitJMax / reachMaxM) - gamut_slope)).astype(np.float32)

        boundary_ratio = gamutBoundaryM / reachBoundaryM
        proportion = np.maximum(boundary_ratio, F(0.75))
        threshold = proportion * gamutBoundaryM
        m_offset = M_work - threshold
        gamut_offset = gamutBoundaryM - threshold
        reach_offset = reachBoundaryM - threshold
        scale = reach_offset / ((reach_offset / gamut_offset) - F(1.0))
        nd = m_offset / scale
        remapped = threshold + scale * nd / (F(1.0) + nd)
    no_remap = (proportion >= 1.0) | (M_work <= threshold)
    remapped_M = np.where(no_remap, M_work, remapped).astype(np.float32)
    remapped_J = (J_int_src + remapped_M * gamut_slope).astype(np.float32)

    passthrough = (M_work <= 0.0) | (J_work > p.limitJMax) | (gamutBoundaryM <= 0.0)
    J_out = np.where(passthrough, J_work, remapped_J).astype(np.float32)
    M_out = np.where(passthrough, F(0.0), remapped_M).astype(np.float32)

    # JMh -> RGB
    Aab_r = np.power(np.maximum(J_out * F(0.00999999978), F(0.0)), F(0.879464149), dtype=np.float32)
    Aab = np.stack([Aab_r, M_out * cos_hr, M_out * sin_hr], axis=1).astype(np.float32)
    rgb_a = (mat3s[2] @ Aab.T).T.astype(np.float32)
    rgb_a_lim = np.minimum(np.abs(rgb_a), F(0.99000001))
    lms = np.sign(rgb_a) * np.power(F(27.1299992) * rgb_a_lim / (F(1.0) - rgb_a_lim), F(2.38095236), dtype=np.float32)
    out = (mat3s[3] @ lms.T).T.astype(np.float32)

    out = np.clip(out, F(0.0), p.outClamp)
    if return_diag:
        return out, dict(no_remap=no_remap, passthrough=passthrough)
    return out


# ---- probes, metrics, ground truth ----------------------------------------

def hue_rgb(deg):
    x = (deg / 60.0) % 6.0
    r = np.clip(abs(x - 3) - 1, 0.0, 1.0)
    g = np.clip(2 - abs(x - 2), 0.0, 1.0)
    b = np.clip(2 - abs(x - 4), 0.0, 1.0)
    return np.array([r, g, b])


def dense_probes(headroom, nh=96, ns=8, nl=48):
    out = [np.stack([np.exp2(np.linspace(-10.0, np.log2(headroom) + 2.0, 240)).astype(np.float32)] * 3, axis=1)]
    lum = np.exp2(np.linspace(-6.0, np.log2(headroom) + 1.0, nl))
    grid = []
    for hd in np.linspace(0, 360, nh, endpoint=False):
        base = hue_rgb(float(hd))
        for sat in np.linspace(0.15, 1.0, ns):
            col = (1.0 - sat) + sat * base
            for L in lum:
                grid.append(col * L)
    out.append(np.array(grid, dtype=np.float32))
    return np.concatenate(out, axis=0)


def compare(a, b, peak_ref):
    num = np.max(np.abs(a - b), axis=1)
    den = np.maximum(np.max(np.abs(b), axis=1), 0.01 * peak_ref)
    return num / den


# BT.2124 dE_ITP (1.0 == 1 JND); display-linear paper-white-relative values are
# scaled by 100 (nits at the 100-nit OCIO paper white) before PQ.
_PQ_M1, _PQ_M2 = 2610.0 / 16384.0, 2523.0 / 4096.0 * 128.0
_PQ_C1, _PQ_C2, _PQ_C3 = 3424.0 / 4096.0, 2413.0 / 4096.0 * 32.0, 2392.0 / 4096.0 * 32.0
_RGB2XYZ = np.array([[0.4123907992659595, 0.35758433938387796, 0.1804807884018343],
                     [0.21263900587151036, 0.7151686787677559, 0.07219231536073371],
                     [0.019330818715591851, 0.11919477979462599, 0.9505321522496606]])
_XYZ2LMS = np.array([[0.3592832590121217, 0.6976051147779502, -0.0358915932320290],
                     [-0.1920808463704993, 1.1004767970374321, 0.0753748658519118],
                     [0.0070797844607479, 0.0748396662186362, 0.8433265453898935]])


def _pq(L):
    Ym = np.power(np.maximum(L, 0.0) / 10000.0, _PQ_M1)
    return np.power((_PQ_C1 + _PQ_C2 * Ym) / (1.0 + _PQ_C3 * Ym), _PQ_M2)


def _ictcp(rgb):
    Lp = _pq((_XYZ2LMS @ (_RGB2XYZ @ (np.asarray(rgb, dtype=np.float64) * 100.0).T)).T)
    return np.stack([0.5 * Lp[:, 0] + 0.5 * Lp[:, 1],
                     (6610 * Lp[:, 0] - 13613 * Lp[:, 1] + 7003 * Lp[:, 2]) / 4096.0,
                     (17933 * Lp[:, 0] - 17390 * Lp[:, 1] - 543 * Lp[:, 2]) / 4096.0], axis=1)


def de_itp(a, b):
    d = _ictcp(a) - _ictcp(b)
    return 720.0 * np.sqrt(d[:, 0] ** 2 + 0.25 * d[:, 1] ** 2 + d[:, 2] ** 2)


_REF_CACHE = {}


def ocio_reference(peak, rgb):
    import gen_aces2_tables as g
    g.assert_version()
    key = (int(peak), rgb.shape[0])
    if key not in _REF_CACHE:
        cpu = g.cpu_processor(float(peak))
        img = np.ascontiguousarray(rgb.astype(np.float32).copy())
        cpu.applyRGB(img)
        _REF_CACHE[key] = img
    return _REF_CACHE[key]


def exact_tier_at(peak):
    """Exact per-peak tier data straight from OCIO (the G5 exact arm). Integer
    literals are masked during scalar location, so the peak-dependent
    cusp-search window (i+3 near peaks 2200-2900) does not break it; the
    evaluation itself always uses the shipped i+2 window."""
    import gen_aces2_tables as g
    bodies, tabs = g.generate_tiers(peaks=[100.0, 200.0, float(peak)])
    hues_decl = re.compile(
        r"const float aces2_gamut_cusp_table_0_hues_array\[\d+\] = float\[\d+\]\(.*?\);", re.S)
    bs = [hues_decl.sub("HUES;", b) for b in bodies]
    masked = [g.INT_RE.sub("@", g.FLOAT_RE.sub("#", b)) for b in bs]
    assert all(m == masked[0] for m in masked[1:])
    streams = [[float(x) for x in g.FLOAT_RE.findall(b)] for b in bs]
    groups = {}
    for i in range(len(streams[0])):
        tup = tuple(s[i] for s in streams)
        if len(set(tup)) > 1:
            groups.setdefault(tup, []).append(i)
    assert len(groups) == g.SCALAR_COUNT
    by_name = {}
    for tup in groups:
        mt = [n for (n, v0) in g.SLOT_NAMES if abs(tup[0] - v0) <= 1e-9 * max(abs(v0), 1.0)]
        assert len(mt) == 1
        by_name[mt[0]] = F(tup[2])
    t = tabs[2]
    return TierData(
        t["aces2_reach_m_table"], t["aces2_gamut_cusp_table"], t["aces2_hues_table"],
        tsFScale=by_name["tsFScale"], tsSDiv=by_name["tsSDiv"],
        limitJMax=by_name["limitJMax"], midJ=by_name["midJ"],
        focusGainBase=by_name["focusGainBase"], gammaBottomInv=by_name["gammaBottomInv"],
        inputClampAP1=by_name["inputClampAP1"], outClamp=by_name["outClamp"],
        mnormCosW=_f32([by_name["mnormCosW.x"], by_name["mnormCosW.y"], by_name["mnormCosW.z"]]),
        mnormSinW=_f32([by_name["mnormSinW.x"], by_name["mnormSinW.y"], by_name["mnormSinW.z"]]),
        mnormOffset=by_name["mnormOffset"], toeLowScale=by_name["toeLowScale"],
        toeK2Bias=by_name["toeK2Bias"], toeHighScale=by_name["toeHighScale"])


# ---- gate runners ----------------------------------------------------------

RUNG_TS = (0.10, 0.25, 0.35, 0.50, 0.60, 0.65, 0.75, 0.90)
G3_P999_REL = 0.010
G4_P999_JND = 0.5
G5_STEP_JND = 0.5      # edges with at least this step across them gate tightly
# 0.25 deg: 1.5x the measured full-circle worst (0.162 deg, the upper-branch
# blue edge at rung 4-5 — the same branch that measures 0.128 deg inside
# 230-250 deg), half of G6's 0.5 deg locus tolerance, and an order below the
# ~2 deg hue-angle discriminability that would make an edge shift visible.
# The bound's function is structural (the rejected blend schemes displaced
# edges by >= 1 deg), not perceptual fine-tuning. Design doc, gate iteration v3.
G5_TIGHT_DEG = 0.25
G5_LOOSE_DEG = 2.0
G5_LUMS = (1.0, 2.0, 4.0)
G6_LOCUS_DEG = 0.5     # a >3-JND probe must have a reference edge this close


def _load(tables_path, body_path):
    body = open(body_path, encoding="utf-8").read()
    _assert_shader_lockstep(body)
    return load_tiered(tables_path), _parse_mats(body)


def _diag_edges(diag):
    nr, pt = diag["no_remap"], diag["passthrough"]
    return [c for c in np.where(np.diff(nr.astype(np.int8)) != 0)[0]
            if not pt[c] and not pt[c + 1]]


def _on_locus(exact_td, mats, hue, sat, lum):
    """True iff the exact arm has a proportion==1 edge within G6_LOCUS_DEG of
    `hue`, swept at the probe's own saturation and luminance."""
    hd = np.linspace(hue - 2.0 * G6_LOCUS_DEG, hue + 2.0 * G6_LOCUS_DEG, 2001)
    base = np.array([hue_rgb(float(h % 360.0)) for h in hd])
    rgb = (((1.0 - sat) + sat * base) * lum).astype(np.float32)
    _, diag = eval_aces2(rgb, mats, exact_td, return_diag=True)
    return any(abs(float(hd[c]) - hue) <= G6_LOCUS_DEG for c in _diag_edges(diag))


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""

    if mode == "check":
        tiers, mats = _load(sys.argv[2], sys.argv[3])
        ok = True
        for k in range(TIER_COUNT):
            peak = 100.0 * 2 ** k
            rgb = dense_probes(2.0 ** k)
            mine = eval_aces2(rgb, mats, tier_at(tiers, float(k)))
            ref = ocio_reference(peak, rgb)
            rel = compare(mine, ref, 2.0 ** k)
            print(f"check tier {k} (peak {peak:6g}): worst={rel.max():.3e} "
                  f"p99={np.percentile(rel, 99):.3e} rms={np.sqrt((rel**2).mean()):.3e}")
            ok &= rel.max() < 1e-3
        print("G1 CHECK", "PASS" if ok else "FAIL", "(port-vs-OCIO tolerance 1e-3)")
        sys.exit(0 if ok else 1)

    if mode == "parity":
        ref_tier, ref_mats = load_single_tier(sys.argv[2])
        tiers, mats = _load(sys.argv[3], sys.argv[4])
        rgb = dense_probes(1.0)
        old = eval_aces2(rgb, ref_mats, ref_tier)
        new = eval_aces2(rgb, mats, tier_at(tiers, 0.0))
        rel = compare(new, old, 1.0)
        w = float(rel.max())
        print(f"parity tier0 vs reference: worst={w:.3e} p99={np.percentile(rel, 99):.3e} "
              f"rms={np.sqrt((rel**2).mean()):.3e} neutral-worst={rel[:240].max():.3e}")
        print("G2 PARITY", "PASS" if w <= 2e-4 else "FAIL", "(tolerance 2e-4)")
        sys.exit(0 if w <= 2e-4 else 1)

    if mode == "blend":
        tiers, mats = _load(sys.argv[2], sys.argv[3])
        worst = p999 = de_max = de_p999 = 0.0
        over3 = total = 0
        exact_cache = {}
        over3_on = over3_off = 0
        for k in range(TIER_COUNT - 1):
            for tt in RUNG_TS:
                peak = int(round(100.0 * 2 ** (k + tt)))
                idx = float(np.log2(peak / 100.0))
                hr = peak / 100.0
                rgb = dense_probes(hr)
                out = eval_aces2(rgb, mats, tier_at(tiers, idx))
                out = np.minimum(out, F(hr))  # the mode-7 routing clamp in tonemap.frag
                ref = ocio_reference(peak, rgb)
                rel = compare(out, ref, hr)
                d = de_itp(out, ref)
                worst = max(worst, float(rel.max()))
                p999 = max(p999, float(np.percentile(rel, 99.9)))
                de_max = max(de_max, float(d.max()))
                de_p999 = max(de_p999, float(np.percentile(d, 99.9)))
                over3_idx = np.where(d > 3.0)[0]
                over3 += len(over3_idx)
                total += len(d)
                # G6 census: a >3-JND probe must straddle the reference's own
                # discontinuity at its own saturation and luminance.
                locus = []
                if len(over3_idx):
                    if peak not in exact_cache:
                        exact_cache[peak] = exact_tier_at(peak)
                    hues_ax = np.linspace(0, 360, 96, endpoint=False)
                    sats_ax = np.linspace(0.15, 1.0, 8)
                    lums_ax = np.exp2(np.linspace(-6.0, np.log2(hr) + 1.0, 48))
                    for i in over3_idx:
                        j = int(i) - 240
                        if j < 0:  # a neutral probe cannot be a hue-discontinuity artefact
                            locus.append((-1.0, 0.0, 0.0, float(d[i]), False))
                            over3_off += 1
                            continue
                        hue = float(hues_ax[j // (8 * 48)])
                        sat = float(sats_ax[(j // 48) % 8])
                        lum = float(lums_ax[j % 48])
                        on = _on_locus(exact_cache[peak], mats, hue, sat, lum)
                        locus.append((hue, sat, lum, float(d[i]), on))
                        if on:
                            over3_on += 1
                        else:
                            over3_off += 1
                print(f"blend rung {k}-{k+1} t={tt:.2f} peak {peak:5d}: "
                      f"rel worst={rel.max():.4f} p99.9={np.percentile(rel, 99.9):.4f} | "
                      f"dE max={d.max():.2f} p99.9={np.percentile(d, 99.9):.3f} >3JND={len(over3_idx)}"
                      + "".join(f"\n    >3JND probe hue={h:.2f} sat={s:.2f} L={l:.3f} dE={e:.2f} "
                                f"{'ON-locus' if on else 'OFF-LOCUS'}" for h, s, l, e, on in locus))
        g3 = p999 <= G3_P999_REL
        g4 = de_p999 <= G4_P999_JND
        g6 = over3_off == 0
        print(f"G3 {'PASS' if g3 else 'FAIL'} (rel p99.9 {p999:.4f} vs {G3_P999_REL}); "
              f"G4 {'PASS' if g4 else 'FAIL'} (dE p99.9 {de_p999:.3f} vs {G4_P999_JND}; "
              f"max {de_max:.2f} reported, tail governed by G6); "
              f"G6 {'PASS' if g6 else 'FAIL'} ({over3}/{total} probes >3 JND: "
              f"{over3_on} on the reference discontinuity, {over3_off} off-locus)")
        sys.exit(0 if (g3 and g4 and g6) else 1)

    if mode == "edges":
        tiers, mats = _load(sys.argv[2], sys.argv[3])
        hd = np.arange(0.0, 360.0, 0.002)
        rgb_base = np.array([hue_rgb(float(h)) for h in hd])
        worst_tight = worst_loose = 0.0
        ok = True
        for k in range(TIER_COUNT - 1):
            peak = int(round(100.0 * 2 ** (k + 0.5)))
            idx = float(np.log2(peak / 100.0))
            exact = exact_tier_at(peak)
            blended = tier_at(tiers, idx)
            for L in G5_LUMS:
                rgb = (rgb_base * L).astype(np.float32)
                oe, de = eval_aces2(rgb, mats, exact, return_diag=True)
                _, db = eval_aces2(rgb, mats, blended, return_diag=True)
                ee, eb = _diag_edges(de), _diag_edges(db)
                for c in ee:
                    # Step magnitude across the exact edge (+/- 0.05 deg).
                    w = 25
                    lo_i, hi_i = max(c - w, 0), min(c + w, len(hd) - 1)
                    step = float(de_itp(oe[lo_i:lo_i + 1], oe[hi_i:hi_i + 1])[0])
                    disp = min((abs(float(hd[c]) - float(hd[b])) for b in eb),
                               default=float("inf"))
                    tight = step >= G5_STEP_JND
                    if tight:
                        bad = disp > G5_TIGHT_DEG
                        worst_tight = max(worst_tight, disp)
                    else:
                        # Invisible boundary (change across it below one JND):
                        # may drift within the loose bound or vanish entirely.
                        bad = disp != float("inf") and disp > G5_LOOSE_DEG
                        if disp != float("inf"):
                            worst_loose = max(worst_loose, disp)
                    ok &= not bad
                    print(f"edges rung {k}-{k+1} peak {peak:5d} L={L:g}: "
                          f"edge {float(hd[c]):8.3f} step={step:.3f} JND "
                          f"[{'tight' if tight else 'loose'}] "
                          f"disp={'VANISHED' if disp == float('inf') else f'{disp:.4f} deg'}"
                          f"{'  <-- FAIL' if bad else ''}")
        print(f"G5 {'PASS' if ok else 'FAIL'} (worst tight-edge displacement {worst_tight:.4f} deg vs "
              f"{G5_TIGHT_DEG}; worst loose-edge {worst_loose:.4f} deg vs {G5_LOOSE_DEG}; "
              f"step threshold {G5_STEP_JND} JND; full-circle sweep, 0.002 deg step)")
        sys.exit(0 if ok else 1)

    if mode == "tokens":
        body = open(sys.argv[2], encoding="utf-8").read()
        _assert_shader_lockstep(body)
        import gen_aces2_tables as g
        g.assert_version()
        em = g.generate_tiers(peaks=[100.0])[0][0]
        em = re.sub(r"const float aces2_gamut_cusp_table_0_hues_array\[\d+\] = float\[\d+\]\(.*?\);\n",
                    "", em, flags=re.S)
        # The mechanical tier parameterization, identical to how the hand body
        # was derived: table lookups -> accessors, peak-baked literals -> fields.
        reps = [
            ("aces2_reach_m_table[int(i_lo)]", "Aces2ReachM(int(i_lo))"),
            ("aces2_reach_m_table[int(i_hi)]", "Aces2ReachM(int(i_hi))"),
            ("aces2_gamut_cusp_table_0_hues_array[i]", "Aces2CuspHue(i)"),
            ("aces2_gamut_cusp_table[i_hi - 1]", "Aces2GamutCusp(i_hi - 1)"),
            ("aces2_gamut_cusp_table[i_hi]", "Aces2GamutCusp(i_hi)"),
            ("aces2_gamut_cusp_table_0_hues_array[i_hi - 1]", "Aces2CuspHue(i_hi - 1)"),
            ("aces2_gamut_cusp_table_0_hues_array[i_hi]", "Aces2CuspHue(i_hi)"),
            ("1.04710376", "aces2_p.tsFScale"),
            ("0.73009213709383403", "aces2_p.tsSDiv"),
            ("34.096539", "aces2_p.midJ"),
            ("135.", "aces2_p.focusGainBase"),
            ("0.877192974", "aces2_p.gammaBottomInv"),
            ("1.29999995", "aces2_p.toeLowScale"),
            ("0.00499999989", "aces2_p.toeK2Bias"),
            ("2.4000001", "aces2_p.toeHighScale"),
            ("vec3(11.341321604032515, 16.469863649185896, 7.8842182208776475)", "aces2_p.mnormCosW"),
            ("vec3(14.665187919584513, -6.3725780354404442, 9.1941277054452897)", "aces2_p.mnormSinW"),
            ("77.133051547393805", "aces2_p.mnormOffset"),
            ("min(vec3(1024., 1024., 1024.), outColor.rgb)", "min(vec3(aces2_p.inputClampAP1), outColor.rgb)"),
            ("min(vec3(1., 1., 1.), outColor.rgb)", "min(vec3(aces2_p.outClamp), outColor.rgb)"),
        ]
        for old, new in reps:
            em = em.replace(old, new)
        # limitJMax: "100.000000" always; bare "100." everywhere EXCEPT the two
        # fixed CAM/tonescale references (the 16-vs-2 split).
        fixed = ["float J = 100. * pow(Aab.r, 1.13705599);",
                 "float J_ts = 100. * pow((F_L_Y / ( 27.1299992 + F_L_Y)) * 30.8946857, 1.13705599);"]
        em = em.replace("100.000000", "aces2_p.limitJMax")
        em_lines = []
        for line in em.split("\n"):
            if line.strip() in [f.strip() for f in fixed]:
                em_lines.append(line)
            else:
                em_lines.append(line.replace("100.", "aces2_p.limitJMax"))
        em = "\n".join(em_lines)

        anchor = "// Declaration of all helper methods"
        tok_re = re.compile(r"[A-Za-z_]\w*|\d+\.\d*(?:[eE][+-]?\d+)?|\d+|\S")

        def section(text, stop=None):
            s = text[text.index(anchor):]
            if stop and stop in s:
                s = s[:s.index(stop)]
            s = re.sub(r"//[^\n]*", "", s)  # comments are not semantic tokens
            return tok_re.findall(s)

        t_body = section(body, stop="vec3 TonemapACES2(")
        t_em = section(em)
        n = min(len(t_body), len(t_em))
        diffs = [(i, t_body[i], t_em[i]) for i in range(n) if t_body[i] != t_em[i]]
        print(f"tokens: hand body {len(t_body)}, mapped emission {len(t_em)}, diffs {len(diffs)}")
        for i, a, b in diffs[:10]:
            print(f"  token {i}: body {a!r} vs emission {b!r}")
        ok = not diffs and len(t_body) == len(t_em)
        print("TOKENS", "PASS" if ok else "FAIL",
              "(transform body == OCIO emission + mechanical parameterization)")
        sys.exit(0 if ok else 1)

    sys.exit(__doc__)


if __name__ == "__main__":
    main()
