// @category Utility
// @version 1

// @sgnode Fresnel
// @display "Fresnel"
// @param normal "Normal" hint=normal
// @param view "View" hint=normal
// @param power "Power" default=5
// @out out Out
// @stage fragment
float SG_Fresnel(vec3 normal, vec3 view, float power)
{
    return pow(1.0 - max(dot(normalize(normal), normalize(view)), 0.0), power);
}

// @sgnode Remap
// @display "Remap"
// @param value "Value" default=0
// @param inMin "In Min" default=0
// @param inMax "In Max" default=1
// @param outMin "Out Min" default=0
// @param outMax "Out Max" default=1
// @out out Out
// @pure
float SG_Remap(float value, float inMin, float inMax, float outMin, float outMax)
{
    return outMin + (value - inMin) / max(inMax - inMin, 0.0001) * (outMax - outMin);
}

// Selector codes for SG_Compare's function argument. The editor's Function enum
// stores these identifiers as the node's parameter value and the graph compiler
// splices a parameter value verbatim into the call, so each one has to be a real
// GLSL name visible at the call site.
const float SG_COMPARE_GREATER = 0.0;
const float SG_COMPARE_LESS = 1.0;
const float SG_COMPARE_EQUAL = 2.0;
const float SG_COMPARE_NOT_EQUAL = 3.0;
const float SG_COMPARE_GREATER_EQUAL = 4.0;
const float SG_COMPARE_LESS_EQUAL = 5.0;

// Equality on graph floats is a tolerance test: the operands come from user
// edits and interpolated inputs, never from matching bit patterns.
const float SG_COMPARE_EPSILON = 1e-5;

// @sgnode Compare
// @display "Compare"
// @param a "A" default=0
// @param b "B" default=0
// @param function "Function" default=SG_COMPARE_GREATER
// @out out Out
// @pure
float SG_Compare(float a, float b, float function)
{
    // `function` is a literal constant at every generated call site, so this
    // chain folds away at compile time. Code 0 is Greater, which is also what an
    // unset parameter resolves to (the compiler substitutes 0.0).
    float equal = 1.0 - step(SG_COMPARE_EPSILON, abs(a - b));
    if (function == SG_COMPARE_LESS)
        return float(a < b);
    if (function == SG_COMPARE_EQUAL)
        return equal;
    if (function == SG_COMPARE_NOT_EQUAL)
        return 1.0 - equal;
    if (function == SG_COMPARE_GREATER_EQUAL)
        return max(float(a > b), equal);
    if (function == SG_COMPARE_LESS_EQUAL)
        return max(float(a < b), equal);
    return float(a > b);
}

// @sgnode If
// @display "If"
// @param condition "Condition" default=0
// @param trueVal "True" default=1
// @param falseVal "False" default=0
// @out out Out
// @pure
float SG_If(float condition, float trueVal, float falseVal)
{
    return mix(falseVal, trueVal, step(0.5, condition));
}

// @sgnode DistanceFade
// @display "Distance Fade"
// @param start "Start" default=0
// @param end "End" default=100
// @out out Out
// @stage fragment
float SG_DistanceFade(float start, float end, float linearDepth)
{
    return smoothstep(start, end, linearDepth);
}

// @sgnode ProximityFade
// @display "Proximity Fade"
// @param distance "Distance" default=1
// @out out Out
// @stage fragment
float SG_ProximityFade(float distance, float linearDepth)
{
    return clamp(linearDepth / max(distance, 0.0001), 0.0, 1.0);
}

// @sgnode RandomRange
// @display "Random Range"
// @param seed "Seed" default=0
// @param minVal "Min" default=0
// @param maxVal "Max" default=1
// @out out Out
// @pure
float SG_RandomRange(float seed, float minVal, float maxVal)
{
    float h = fract(sin(dot(vec2(seed, seed * 1.37 + 17.0), vec2(12.9898, 78.233))) * 43758.5453);
    return mix(minVal, maxVal, h);
}

// @sgnode Reroute
// @display "Reroute"
// @param in "In" default=0
// @out out Out
// @pure
float SG_Reroute(float valueIn) { return valueIn; }
