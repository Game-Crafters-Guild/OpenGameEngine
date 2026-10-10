// @category Normal
// @version 1
//
// Tangent-space normal operators. SgGraphCompiler treats these as tangent-space
// sources (NormalInputUsesTangentSpace), so a graph wiring one into
// SurfaceOutput.Normal gets the TBN transform and HAS_TANGENT automatically.
// Inputs and outputs are unpacked normals in [-1,1], not [0,1] texture values.

// @sgnode NormalStrength
// @display "Normal Strength"
// @param value "Normal" hint=normal default=vec3(0, 0, 1)
// @param strength "Strength" default=1
// @out result Out
// @pure
vec3 SG_NormalStrength(vec3 value, float strength)
{
    return vec3(value.xy * strength, mix(1.0, value.z, clamp(strength, 0.0, 1.0)));
}

// @sgnode NormalBlend
// @display "Normal Blend"
// @param a "A" hint=normal default=vec3(0, 0, 1)
// @param b "B" hint=normal default=vec3(0, 0, 1)
// @out result Out
// @pure
vec3 SG_NormalBlend(vec3 a, vec3 b)
{
    return normalize(vec3(a.xy + b.xy, a.z * b.z));
}
