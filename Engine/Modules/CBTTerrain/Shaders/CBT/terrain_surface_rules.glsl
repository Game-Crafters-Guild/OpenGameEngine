// Terrain SURFACE RULES on the GPU: the twin of TerrainECS/TerrainSurfaceRuleEval.h
// (the row evaluator) and of TerrainECS/TerrainSplatComposite.h (the splat write),
// so the GPU splat bake produces the same bytes the CPU bake would.
//
// The two arms are held together by a test, not by care: the marked blocks below are
// extracted verbatim at build time (Tests/ExtractShaderBlock.cmake) and compiled as C++
// through GlslShim.h by TerrainSurfaceRuleGpuParityTests, which sweeps them against the
// C++ headers. An edit here is an edit to what that gate measures.
//
// UNITS AND CONVENTIONS — the rule vocabulary has two units for slope and two for
// height, and reading a band in the wrong one silently moves a material:
//
//   SlopeDegrees      the true surface angle from horizontal, [0, 90], measured from the
//                     gradient with height taken back into METRES.
//   SlopeNormalized   `1 - max(N.y, 0)` off the heightfield's own normal, [0, 1]. NOT a
//                     geometric cosine and NOT SlopeDegrees rescaled: that normal comes
//                     from heights normalized to [0, 1] against metre spacing, so the
//                     quantity is squashed by the terrain's HeightScale, so a threshold in
//                     it is not a fixed angle. Prefer SlopeDegrees.
//   HeightMetres      METRES ABOVE THE TERRAIN'S BASE — the normalized height sample times
//                     HeightScale. NOT world-space Y: the terrain entity's own Y
//                     translation is not folded in, matching the other modifier effects.
//   HeightNormalized  `(h - minH) / heightRange` over the terrain's live global range, so
//                     the band chases the heightfield.
//   Noise             the terrain's own gradient-noise basis at ONE octave, remapped to
//                     [0, 1].
//
// FalloffCurve defaults to CLAMPED LINEAR — a band's edge is exactly as soft as its
// authored Feather. Smoothstep is the optional member: softer corners.
//
// SPLAT CHANNELS ARE CARRIED AS BYTES IN [0, 255], as floats. The CPU bake writes uint8
// and every following row reads those bytes back, so a row's input is the previous row's
// QUANTIZED output. Mirroring that round trip is what makes the two arms comparable byte
// for byte rather than approximately.
//
// The includer sets GE_SURFACE_RULES_BIND_BUFFERS to also get the row/condition SSBOs and
// the two functions that read them; without it only the types and the pure maths appear,
// which is what the height kernel variant needs.

#ifndef GE_TERRAIN_SURFACE_RULES_DECLARED
#define GE_TERRAIN_SURFACE_RULES_DECLARED

// GE_SURFACE_RULE_TYPES_BEGIN
// Condition kinds — mirror Components::TerrainRuleConditionKind's VALUES, which the
// scene file stores; renumbering either side silently re-reads authored bands.
const uint GE_RULE_KIND_SLOPE_DEGREES     = 0u;
const uint GE_RULE_KIND_SLOPE_NORMALIZED  = 1u;
const uint GE_RULE_KIND_HEIGHT_METRES     = 2u;
const uint GE_RULE_KIND_HEIGHT_NORMALIZED = 3u;
const uint GE_RULE_KIND_NOISE             = 4u;

// Mirror Components::TerrainRuleFalloffCurve.
const uint GE_RULE_CURVE_CLAMPED_LINEAR = 0u;
const uint GE_RULE_CURVE_SMOOTHSTEP     = 1u;

// The volume scope a row inherits. Circle/Rectangle mirror the packer's
// Components::TerrainModifierShape ids; Global is TerrainVolumeShape::Global, which has no
// TerrainModifierShape member and so gets its own id here rather than a footprint.
const uint GE_RULE_SHAPE_CIRCLE    = 0u;
const uint GE_RULE_SHAPE_RECTANGLE = 1u;
const uint GE_RULE_SHAPE_GLOBAL    = 2u;

// Components::kMaxTerrainRuleConditions. A packed row whose count exceeds it is capped
// here exactly as the CPU evaluator caps it, so an over-cap row degrades the same way on
// both arms instead of reading a neighbour's condition.
const uint GE_MAX_RULE_CONDITIONS = 4u;

// std430 twin of Components::TerrainRuleCondition. 32 bytes, all scalars.
struct GESurfaceRuleCondition
{
    uint  Kind;
    uint  Curve;
    float Min;
    float Max;
    float Feather;
    float NoiseFrequency;
    uint  NoiseSeed;
    uint  Pad0;
};

// std430 twin of one authored row PLUS the scope it was authored inside: the CPU
// multiplies the volume's shape weight into every row, so a flattened row carries its
// volume's framing rather than pointing at a second table.
//
// Conditions live in their OWN buffer, addressed by ConditionBase, and are never an array
// member here. A dynamically indexed array inside a struct copied out of an SSBO makes
// glslang emit a whole-struct per-invocation scratch copy — the %indexable spill this
// codebase has paid for before. 64 bytes, all scalars.
struct GESurfaceRule
{
    uint  MaterialSlot;    // splat channel, already clamped by the packer
    float Strength;
    uint  Replace;         // bool: 0 accumulate + renormalize, 1 lerp toward the pure material
    uint  ConditionCount;

    uint  ConditionBase;   // first condition index in the condition buffer
    uint  Shape;           // GE_RULE_SHAPE_*
    float CenterX;
    float CenterZ;

    float Yaw;             // radians
    float Radius;
    float RectHalfX;
    float RectHalfZ;

    float Falloff;         // outward half of the shape ramp, metres
    float FalloffInward;   // inward half, metres
    float VolumeWeight;    // the volume's master strength
    uint  Pad0;
};

// A texel's measurements — the twin of TerrainECS::TerrainRuleSample. Computed once per
// texel and reused by every row and condition that reads them.
struct GESurfaceRuleSample
{
    float SlopeNormalized;
    float SlopeDegrees;
    float HeightMetres;
    float HeightNormalized;
    float WorldX;
    float WorldZ;
};
// GE_SURFACE_RULE_TYPES_END

// GE_SURFACE_RULE_MATH_BEGIN
// ---- Noise basis (mirror GradientNoise2D in Engine/Modules/Noise) ------------

uint GE_GradHash(int ix, int iz, uint seed)
{
    uint h = uint(ix) * 374761393u + uint(iz) * 668265263u + seed;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return h ^ (h >> 16u);
}

float GE_Grad(uint h, float dx, float dz)
{
    uint q = h & 3u;
    if (q == 0u) return  dx + dz;
    if (q == 1u) return -dx + dz;
    if (q == 2u) return  dx - dz;
    return -dx - dz;
}

float GE_GradientNoise2D(float x, float z, uint seed)
{
    int ix = int(floor(x));
    int iz = int(floor(z));
    float fx = x - float(ix);
    float fz = z - float(iz);

    float u = fx * fx * (3.0f - 2.0f * fx);
    float v = fz * fz * (3.0f - 2.0f * fz);

    float n00 = GE_Grad(GE_GradHash(ix,     iz,     seed), fx,        fz);
    float n10 = GE_Grad(GE_GradHash(ix + 1, iz,     seed), fx - 1.0f, fz);
    float n01 = GE_Grad(GE_GradHash(ix,     iz + 1, seed), fx,        fz - 1.0f);
    float n11 = GE_Grad(GE_GradHash(ix + 1, iz + 1, seed), fx - 1.0f, fz - 1.0f);

    float nx0 = n00 + u * (n10 - n00);
    float nx1 = n01 + u * (n11 - n01);
    return nx0 + v * (nx1 - nx0);
}

// The field a Noise condition bands (mirror SurfaceRuleNoiseSample): ONE octave at unit
// amplitude, remapped from roughly [-1, 1] into [0, 1] so a band has a fixed domain. A
// second scale is a second condition, not a second octave.
float GE_SurfaceRuleNoise(float worldX, float worldZ, float frequency, uint seed)
{
    return clamp(0.5f + 0.5f * GE_GradientNoise2D(worldX * frequency, worldZ * frequency, seed),
                 0.0f, 1.0f);
}

// ---- Row maths (mirror TerrainSurfaceRuleEval.h) -----------------------------

// Build a texel's measurements from what the bake already has in hand (mirror
// MakeTerrainRuleSample). `heightRange` is the caller's already-guarded
// max(SplatMaxH - SplatMinH, eps).
GESurfaceRuleSample GE_MakeSurfaceRuleSample(float normalX, float normalY, float normalZ,
                                             float sampleHeight, float heightScale,
                                             float splatMinHeight, float heightRange,
                                             float worldX, float worldZ)
{
    GESurfaceRuleSample s;
    s.SlopeNormalized = 1.0f - max(normalY, 0.0f);
    // normalY is strictly positive (it is 1 before normalizing), so the reciprocal is safe.
    float invY = 1.0f / normalY;
    float gradX = -normalX * invY * heightScale;
    float gradZ = -normalZ * invY * heightScale;
    s.SlopeDegrees = atan(sqrt(gradX * gradX + gradZ * gradZ)) * (180.0f / 3.14159265f);
    // Terrain-LOCAL metres: the normalized sample scaled back up, with no terrain entity
    // translation folded in.
    s.HeightMetres = sampleHeight * heightScale;
    s.HeightNormalized = (sampleHeight - splatMinHeight) / heightRange;
    s.WorldX = worldX;
    s.WorldZ = worldZ;
    return s;
}

// The condition's own measurement, in the unit its kind names. Noise is absent: it is the
// one kind that needs a field sampled at (X, Z) rather than a value the texel carries.
float GE_SurfaceRuleConditionValue(uint kind, GESurfaceRuleSample s)
{
    if (kind == GE_RULE_KIND_SLOPE_NORMALIZED)  return s.SlopeNormalized;
    if (kind == GE_RULE_KIND_SLOPE_DEGREES)     return s.SlopeDegrees;
    if (kind == GE_RULE_KIND_HEIGHT_METRES)     return s.HeightMetres;
    if (kind == GE_RULE_KIND_HEIGHT_NORMALIZED) return s.HeightNormalized;
    return 0.0f;
}

// Ramp shape across an already-clamped [0, 1] edge parameter.
float GE_SurfaceRuleCurve(uint curve, float t)
{
    if (curve == GE_RULE_CURVE_SMOOTHSTEP)
        return t * t * (3.0f - 2.0f * t);
    return t;
}

// Weight [0, 1] for one condition given its already-measured value.
//
// 1 inside [Min, Max]; outside, a ramp of width Feather down to 0. Feather 0 is a hard
// edge. A degenerate band (Min == Max) is therefore a symmetric peak, and an inverted band
// (Min > Max) has no plateau — only the two ramps.
float GE_SurfaceRuleConditionWeight(GESurfaceRuleCondition c, float value)
{
    if (value >= c.Min && value <= c.Max)
        return 1.0f;
    if (c.Feather <= 0.0f)
        return 0.0f;

    float distance = (value < c.Min) ? (c.Min - value) : (value - c.Max);
    float t = clamp(1.0f - distance / c.Feather, 0.0f, 1.0f);
    return GE_SurfaceRuleCurve(c.Curve, t);
}

// One condition folded into a row's running product — the AND, which is a MULTIPLY.
float GE_AccumulateSurfaceRuleCondition(float weight, GESurfaceRuleCondition c,
                                        GESurfaceRuleSample s)
{
    float value = (c.Kind == GE_RULE_KIND_NOISE)
        ? GE_SurfaceRuleNoise(s.WorldX, s.WorldZ, c.NoiseFrequency, c.NoiseSeed)
        : GE_SurfaceRuleConditionValue(c.Kind, s);
    return weight * GE_SurfaceRuleConditionWeight(c, value);
}

// ---- Volume scope (mirror ShapeFalloffWeight + ComputeWeight) ----------------

// Shape weight [0, 1] for a sample `distFromEdge` metres from the edge, POSITIVE INSIDE.
// `falloff` and `falloffInward` are the two halves of ONE ramp, evaluated from a single
// normalized parameter so they cannot disagree at the edge they share.
//
// NOTE for anyone extending the height kernel: this is the COMPLETE ramp, unlike the
// height variant's ComputeWeight, which implements the outward half only. That asymmetry
// is load-bearing there — PackModifiersForGpuBake refuses a bake whose volume has an
// inward feather precisely because the height kernel cannot express it. Wiring this
// function into the height path without relaxing that refusal changes nothing; relaxing
// the refusal without wiring it in breaks height parity.
float GE_ShapeFalloffWeight(float distFromEdge, float falloff, float falloffInward)
{
    if (falloff > 0.0f && falloffInward > 0.0f)
    {
        float t = clamp((distFromEdge + falloff) / (falloff + falloffInward), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    if (distFromEdge <= 0.0f)
    {
        if (falloff <= 0.0f)
            return 0.0f;
        float t = clamp(1.0f + distFromEdge / falloff, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    if (falloffInward <= 0.0f)
        return 1.0f;
    float t = clamp(distFromEdge / falloffInward, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The scope weight a row inherits from its volume. A GLOBAL volume has no edge, so no
// ramp: it contributes its master strength at every sample, and both falloffs are ignored
// rather than applied to a boundary that does not exist.
float GE_SurfaceRuleShapeWeight(GESurfaceRule r, float worldX, float worldZ)
{
    if (r.Shape == GE_RULE_SHAPE_GLOBAL)
        return r.VolumeWeight;

    float dx = worldX - r.CenterX;
    float dz = worldZ - r.CenterZ;
    float distFromEdge;
    if (r.Shape == GE_RULE_SHAPE_CIRCLE)
    {
        float dist = sqrt(dx * dx + dz * dz);
        distFromEdge = r.Radius - dist;
    }
    else
    {
        float c = cos(-r.Yaw);
        float s = sin(-r.Yaw);
        float lx = abs(c * dx - s * dz);
        float lz = abs(s * dx + c * dz);
        distFromEdge = min(r.RectHalfX - lx, r.RectHalfZ - lz);
    }
    return GE_ShapeFalloffWeight(distFromEdge, r.Falloff, r.FalloffInward) * r.VolumeWeight;
}

// ---- Splat write (mirror CompositeSplatTexel) --------------------------------

// Write one material's weight into a splat texel whose channels are BYTES in [0, 255]
// carried as floats. `replace` false accumulates and renormalizes; true lerps every
// channel toward the pure material, which preserves the weight sum and makes the edge
// exactly as soft as the ramp.
//
// The truncating uint conversion is the shipped `static_cast<uint8>(clamp(w, 0, 255))`,
// not a round — a nearest-rounding pack is off by one byte over half the domain.
vec4 GE_CompositeSplatTexel(vec4 pixel, uint layerIdx, float weight, bool replace)
{
    int layer = int(layerIdx);

    if (replace)
    {
        vec4 replaced = vec4(0.0f);
        for (int i = 0; i < 4; ++i)
        {
            float target = (i == layer) ? 255.0f : 0.0f;
            replaced[i] = float(uint(clamp(pixel[i] * (1.0f - weight) + target * weight,
                                           0.0f, 255.0f)));
        }
        return replaced;
    }

    vec4 weights = vec4(0.0f);
    for (int i = 0; i < 4; ++i)
        weights[i] = pixel[i] / 255.0f;

    weights[layer] += weight;

    float total = 0.0f;
    for (int i = 0; i < 4; ++i)
        total += weights[i];

    vec4 composited = pixel;
    if (total > 0.0f)
    {
        for (int i = 0; i < 4; ++i)
            composited[i] = float(uint(clamp(weights[i] / total * 255.0f, 0.0f, 255.0f)));
    }
    return composited;
}
// GE_SURFACE_RULE_MATH_END

#ifdef GE_SURFACE_RULES_BIND_BUFFERS

layout(std430, set = 0, binding = 2) readonly buffer SurfaceRuleBuffer
{
    GESurfaceRule Items[];
} gSurfaceRules;

layout(std430, set = 0, binding = 3) readonly buffer SurfaceRuleConditionBuffer
{
    GESurfaceRuleCondition Items[];
} gSurfaceRuleConditions;

// GE_SURFACE_RULE_ROWS_BEGIN
// Weight [0, 1] for one row: its conditions ANDed by multiplication, times the row's
// strength. A row with ZERO conditions is unconditional.
//
// The rows and conditions are addressed in their buffers rather than copied into locals:
// that is what keeps the dynamic index off a local array and out of per-invocation
// scratch. The parity test supplies host stand-ins with the same names.
float GE_EvaluateSurfaceRuleWeight(uint ruleIndex, GESurfaceRuleSample s)
{
    float weight = gSurfaceRules.Items[ruleIndex].Strength;
    uint base = gSurfaceRules.Items[ruleIndex].ConditionBase;
    uint count = min(gSurfaceRules.Items[ruleIndex].ConditionCount, GE_MAX_RULE_CONDITIONS);

    for (uint i = 0u; i < count && weight > 0.0f; ++i)
        weight = GE_AccumulateSurfaceRuleCondition(weight, gSurfaceRuleConditions.Items[base + i], s);

    return clamp(weight, 0.0f, 1.0f);
}

// One row applied to one texel: the volume's scope weight times the row's condition
// product, composited with the paint semantics the row selected. Rows are applied in
// buffer order, which the packer builds as (modifier priority, then stack order) — the
// same order ApplySplatModifiers walks.
vec4 GE_ApplySurfaceRuleRow(vec4 pixel, uint ruleIndex, GESurfaceRuleSample s)
{
    float volumeWeight = GE_SurfaceRuleShapeWeight(gSurfaceRules.Items[ruleIndex], s.WorldX, s.WorldZ);
    if (volumeWeight <= 0.0f)
        return pixel;

    float weight = GE_EvaluateSurfaceRuleWeight(ruleIndex, s) * volumeWeight;
    if (weight <= 0.0f)
        return pixel;

    return GE_CompositeSplatTexel(pixel, gSurfaceRules.Items[ruleIndex].MaterialSlot, weight,
                                  gSurfaceRules.Items[ruleIndex].Replace != 0u);
}
// GE_SURFACE_RULE_ROWS_END

#endif // GE_SURFACE_RULES_BIND_BUFFERS

#endif // GE_TERRAIN_SURFACE_RULES_DECLARED
