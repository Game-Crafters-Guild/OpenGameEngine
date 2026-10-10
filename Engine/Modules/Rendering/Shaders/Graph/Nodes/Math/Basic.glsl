// @category Math/Basic
// @version 1

// @sgnode Add
// @display "Add"
// @pure
float SG_Add(float a, float b) { return a + b; }

// @sgnode Subtract
// @display "Subtract"
// @pure
float SG_Subtract(float a, float b) { return a - b; }

// @sgnode Multiply
// @display "Multiply"
// @pure
float SG_Multiply(float a, float b) { return a * b; }

// @sgnode Divide
// @display "Divide"
// @pure
float SG_Divide(float a, float b) { return a / max(b, 0.0001); }

// @sgnode OneMinus
// @display "OneMinus"
// @pure
float SG_OneMinus(float value) { return 1.0 - value; }

// @sgnode Lerp
// @display "Lerp"
// @pure
float SG_Lerp(float a, float b, float weight) { return mix(a, b, weight); }

// @sgnode Power
// @display "Power"
// @pure
float SG_Power(float base, float exp) { return pow(base, exp); }

// @sgnode Abs
// @display "Abs"
// @pure
float SG_Abs(float value) { return abs(value); }

// @sgnode Sin
// @display "Sin"
// @pure
float SG_Sin(float value) { return sin(value); }

// @sgnode Cos
// @display "Cos"
// @pure
float SG_Cos(float value) { return cos(value); }

// @sgnode Sqrt
// @display "Sqrt"
// @pure
float SG_Sqrt(float value) { return sqrt(max(value, 0.0)); }

// @sgnode Saturate
// @display "Saturate"
// @pure
float SG_Saturate(float value) { return clamp(value, 0.0, 1.0); }

// @sgnode Negate
// @display "Negate"
// @pure
float SG_Negate(float value) { return -value; }

// @sgnode Min
// @display "Min"
// @pure
float SG_Min(float a, float b) { return min(a, b); }

// @sgnode Max
// @display "Max"
// @pure
float SG_Max(float a, float b) { return max(a, b); }

// @sgnode Floor
// @display "Floor"
// @pure
float SG_Floor(float value) { return floor(value); }

// @sgnode Ceil
// @display "Ceil"
// @pure
float SG_Ceil(float value) { return ceil(value); }

// @sgnode Fract
// @display "Fract"
// @pure
float SG_Fract(float value) { return fract(value); }

// @sgnode Sign
// @display "Sign"
// @pure
float SG_Sign(float value) { return sign(value); }

// @sgnode Mod
// @display "Mod"
// @pure
float SG_Mod(float a, float b) { return mod(a, b); }

// @sgnode Tan
// @display "Tan"
// @pure
float SG_Tan(float value) { return tan(value); }

// @sgnode Exp
// @display "Exp"
// @pure
float SG_Exp(float value) { return exp(value); }

// @sgnode Log
// @display "Log"
// @pure
float SG_Log(float value) { return log(max(value, 1e-6)); }

// @sgnode Step
// @display "Step"
// @pure
float SG_Step(float edge, float value) { return step(edge, value); }

// @sgnode SmoothStep
// @display "SmoothStep"
// @pure
float SG_SmoothStep(float edge0, float edge1, float value) { return smoothstep(edge0, edge1, value); }

// @sgnode MultiplyAdd
// @display "MultiplyAdd"
// @pure
float SG_MultiplyAdd(float a, float b, float c) { return a * b + c; }

// @sgnode Clamp
// @display "Clamp"
// @pure
float SG_Clamp(float value, float minVal, float maxVal) { return clamp(value, minVal, maxVal); }
