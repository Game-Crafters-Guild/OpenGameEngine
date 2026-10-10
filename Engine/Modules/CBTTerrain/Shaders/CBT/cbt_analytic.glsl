// cbt_analytic.glsl — analytic sphere-modifier evaluation, the GPU twin of
// SphereAnalyticModifiers.h (sculpt shape-accuracy S2). Pure math, NO bindings — compiles in both
// the compute layout (CBTFrameParams tail) and the surface fragment (CBTSurfaceParamsData tail).
//
// A circular flatten's shape is a pure closed form of world direction; evaluating it AT SAMPLE TIME
// inside the sculpt-sampling chokepoints reproduces the exact circle at ANY planet radius, instead
// of the store-texel staircase a bake quantizes it to (8 m/texel at R=50k, ~611 m/texel at Earth).
// CBT_EvalSphereAnalyticFlatten MUST stay in expression-lockstep with
// EvaluateSphereAnalyticFlatten / EvaluateSpherePlacement's flatten branch (the CPU authority);
// SphereSculptShapeAccuracyTests.GlslAnalyticEvalMatchesCpp parses THESE lines against the CPU
// forms, the cbt_sculpt.glsl constants-lock pattern.
#ifndef CBT_ANALYTIC_GLSL
#define CBT_ANALYTIC_GLSL

// Mirror CBTLayout.h kMaxSphereAnalyticModifiers (locked by the parse test).
const uint CBT_MAX_SPHERE_ANALYTIC = 16u;

// One analytic circular flatten, 3 vec4 (mirror PackSphereAnalyticFlatten):
//   NRadius   = [unit centre direction N | circle radius, m]
//   E1Falloff = [tangent E1 at N          | falloff skirt, m (0 = hard edge)]
//   E2Target  = [E2 = cross(N, E1)        | target radius, m from planet centre]
struct CBTSphereAnalyticFlatten
{
    vec4 NRadius;
    vec4 E1Falloff;
    vec4 E2Target;
};

// Height offset (metres) the flatten contributes at UNIT world direction `dir`. reliefAtDir is the
// closed-form base relief (metres) at that direction — cancelled so the pad lands flat (the radial
// analogue of the planar h' = lerp(h, target, weight)). Expression twin of
// EvaluateSphereAnalyticFlatten: far-hemisphere reject, tangent-plane metres, smoothstep edge.
float CBT_EvalSphereAnalyticFlatten(CBTSphereAnalyticFlatten f, float planetRadius, vec3 dir,
                                    float reliefAtDir)
{
    if (dot(dir, f.NRadius.xyz) <= 0.0)
        return 0.0; // far hemisphere
    float l0 = planetRadius * dot(dir, f.E1Falloff.xyz);
    float l1 = planetRadius * dot(dir, f.E2Target.xyz);
    float distFromEdge = f.NRadius.w - sqrt(l0 * l0 + l1 * l1);
    float weight;
    if (distFromEdge <= 0.0)
    {
        if (f.E1Falloff.w <= 0.0)
            return 0.0;
        float t = clamp(1.0 + distFromEdge / f.E1Falloff.w, 0.0, 1.0);
        weight = t * t * (3.0 - 2.0 * t);
    }
    else
    {
        weight = 1.0;
    }
    return (f.E2Target.w - planetRadius - reliefAtDir) * weight;
}

// True when unit `dir` lies inside the flatten's footprint (shape + falloff skirt) — the
// crease-residency twin of SphereAnalyticFootprintCovers. An analytic modifier allocates no sculpt
// pages, so the edit-driven retess residency gate extends to page-resident OR this.
bool CBT_SphereAnalyticFlattenCovers(CBTSphereAnalyticFlatten f, float planetRadius, vec3 dir)
{
    if (dot(dir, f.NRadius.xyz) <= 0.0)
        return false;
    float l0 = planetRadius * dot(dir, f.E1Falloff.xyz);
    float l1 = planetRadius * dot(dir, f.E2Target.xyz);
    return sqrt(l0 * l0 + l1 * l1) <= f.NRadius.w + max(f.E1Falloff.w, 0.0);
}

// ---- Transient brush dabs (sculpt shape-accuracy S3 — analytic-while-stroking) ----
// A held brush stroke's dabs ride the SAME 3-vec4 slots after the flattens (analyticParams.y
// count, PackSphereAnalyticDab layout):
//   NRadius   = [unit dab centre direction N | planetRadius * sin(angular radius), m]
//   E1Falloff = [tangent E1 at N             | signed amplitude, m]
//   E2Target  = [E2 = cross(N, E1)           | 0]
// Tangent-plane-metres form ON PURPOSE (the SphereAnalyticModifiers.h numerical-form note): a
// dot-then-acos eval of the true angular distance is fp32-degenerate for few-metre brushes at
// planet radius (cos(angR) rounds to 1.0), so the dab uses the flatten's well-conditioned l0/l1
// frame; the falloff argument sin(ang)/sin(angR) matches ApplyDab's ang/angR at the centre and
// the cap edge and to angR^2/6 between. Expression twin of EvaluateSphereAnalyticDab (the CPU
// authority); locked by the parse test.

// Height offset (metres) the dab contributes at UNIT world direction `dir`.
float CBT_EvalSphereAnalyticDab(CBTSphereAnalyticFlatten f, float planetRadius, vec3 dir)
{
    if (dot(dir, f.NRadius.xyz) <= 0.0)
        return 0.0; // far hemisphere
    float l0 = planetRadius * dot(dir, f.E1Falloff.xyz);
    float l1 = planetRadius * dot(dir, f.E2Target.xyz);
    float t = clamp(1.0 - sqrt(l0 * l0 + l1 * l1) / f.NRadius.w, 0.0, 1.0);
    return f.E1Falloff.w * (t * t * (3.0 - 2.0 * t));
}

// True when unit `dir` lies inside the dab's cap — the crease-residency twin of
// SphereAnalyticDabCovers (the smoothstep support ends exactly at the cap edge; no skirt).
bool CBT_SphereAnalyticDabCovers(CBTSphereAnalyticFlatten f, float planetRadius, vec3 dir)
{
    if (dot(dir, f.NRadius.xyz) <= 0.0)
        return false;
    float l0 = planetRadius * dot(dir, f.E1Falloff.xyz);
    float l1 = planetRadius * dot(dir, f.E2Target.xyz);
    return sqrt(l0 * l0 + l1 * l1) <= f.NRadius.w;
}

#endif // CBT_ANALYTIC_GLSL
