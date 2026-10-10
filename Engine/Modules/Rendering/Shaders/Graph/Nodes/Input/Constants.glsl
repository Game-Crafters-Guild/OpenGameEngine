// @category Input/Constants
// @version 1

// @sgnode FloatConstant
// @display "Float"
// @param value "Value" default=0
// @out value Value
// @pure
float SG_FloatConstant(float value) { return value; }

// @sgnode Vec2Constant
// @display "Vec2"
// @param x "X" default=0
// @param y "Y" default=0
// @out value Value
// @pure
vec2 SG_Vec2Constant(float x, float y) { return vec2(x, y); }

// @sgnode Vec3Constant
// @display "Vec3"
// @param x "X" default=0
// @param y "Y" default=0
// @param z "Z" default=0
// @out value Value
// @pure
vec3 SG_Vec3Constant(float x, float y, float z) { return vec3(x, y, z); }

// @sgnode Vec4Constant
// @display "Vec4"
// @param x "X" default=0
// @param y "Y" default=0
// @param z "Z" default=0
// @param w "W" default=1
// @out value Value
// @pure
vec4 SG_Vec4Constant(float x, float y, float z, float w) { return vec4(x, y, z, w); }

// @sgnode ColorConstant
// @display "Color"
// @param r "R" default=1
// @param g "G" default=1
// @param b "B" default=1
// @out value Value
// @pure
vec3 SG_ColorConstant(float r, float g, float b) { return vec3(r, g, b); }
