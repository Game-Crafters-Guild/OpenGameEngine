// @category Math/Vector
// @version 1

// @sgnode VectorAdd
// @display "VectorAdd"
// @pure
vec3 SG_VectorAdd(vec3 a, vec3 b) { return a + b; }

// @sgnode VectorSubtract
// @display "VectorSubtract"
// @pure
vec3 SG_VectorSubtract(vec3 a, vec3 b) { return a - b; }

// @sgnode VectorMultiply
// @display "VectorMultiply"
// @pure
vec3 SG_VectorMultiply(vec3 a, vec3 b) { return a * b; }

// @sgnode VectorDivide
// @display "VectorDivide"
// @pure
vec3 SG_VectorDivide(vec3 a, vec3 b) { return a / max(b, vec3(0.0001)); }

// @sgnode VectorLerp
// @display "VectorLerp"
// @pure
vec3 SG_VectorLerp(vec3 a, vec3 b, float weight) { return mix(a, b, weight); }

// @sgnode VectorClamp
// @display "VectorClamp"
// @pure
vec3 SG_VectorClamp(vec3 value, vec3 minVal, vec3 maxVal) { return clamp(value, minVal, maxVal); }

// @sgnode Length
// @display "Length"
// @pure
float SG_Length(vec3 value) { return length(value); }

// @sgnode Normalize
// @display "Normalize"
// @pure
vec3 SG_Normalize(vec3 value) { return normalize(value); }

// @sgnode Cross
// @display "Cross"
// @pure
vec3 SG_Cross(vec3 a, vec3 b) { return cross(a, b); }

// @sgnode DotProduct
// @display "DotProduct"
// @pure
float SG_DotProduct(vec3 a, vec3 b) { return dot(a, b); }

// @sgnode VectorDistance
// @display "VectorDistance"
// @pure
float SG_VectorDistance(vec3 a, vec3 b) { return distance(a, b); }

// @sgnode Reflect
// @display "Reflect"
// @pure
vec3 SG_Reflect(vec3 vector, vec3 normal) { return reflect(vector, normalize(normal)); }

// @sgnode Refract
// @display "Refract"
// @pure
vec3 SG_Refract(vec3 vector, vec3 normal, float eta) { return refract(vector, normalize(normal), eta); }

// @sgnode ColorMix
// @display "ColorMix"
// @pure
vec3 SG_ColorMix(vec3 a, vec3 b, float weight) { return mix(a, b, weight); }

// @sgnode Grayscale
// @display "Grayscale"
// @pure
vec3 SG_Grayscale(vec3 color) { return vec3(dot(color, vec3(0.299, 0.587, 0.114))); }

// @sgnode VectorCompose
// @display "Vector Compose"
// @pure
vec3 SG_VectorCompose(float x, float y, float z) { return vec3(x, y, z); }

// @sgnode VectorDecompose
// @display "Vector Decompose"
// @out x X
// @out y Y
// @out z Z
// @pure
void SG_VectorDecompose(vec3 value, out float x, out float y, out float z) { x = value.x; y = value.y; z = value.z; }

// @sgnode CombineVec4
// @display "Combine Vec4"
// @pure
vec4 SG_CombineVec4(vec3 rgb, float a) { return vec4(rgb, a); }
