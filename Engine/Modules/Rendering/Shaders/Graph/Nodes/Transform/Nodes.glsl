// @category Transform
// @version 1

// @sgnode TransformUV
// @display "Transform UV"
// @param uv "UV" hint=uv
// @param scale "Scale" default=vec2(1)
// @param offset "Offset" default=vec2(0)
// @out out Out
// @pure
vec2 SG_TransformUV(vec2 uv, vec2 scale, vec2 offset) { return uv * scale + offset; }

// @sgnode TransformPosition
// @display "Transform Position"
// @param position "Position" hint=position
// @out out Out
// @pure
vec3 SG_TransformPosition(vec3 position) { return position; }

// @sgnode TransformDirection
// @display "Transform Direction"
// @param direction "Direction" hint=normal
// @out out Out
// @pure
vec3 SG_TransformDirection(vec3 direction) { return normalize(direction); }

// @sgnode TransformNormal
// @display "Transform Normal"
// @param normal "Normal" hint=normal
// @out out Out
// @pure
vec3 SG_TransformNormal(vec3 normal) { return normalize(normal); }

// @sgnode RotateByAxis
// @display "Rotate By Axis"
// @param vector "Vector"
// @param axis "Axis"
// @param angle "Angle" default=0
// @out out Out
// @pure
vec3 SG_RotateByAxis(vec3 vector, vec3 axis, float angle)
{
    vec3 a = normalize(axis);
    float c = cos(angle);
    float s = sin(angle);
    return vector * c + cross(a, vector) * s + a * dot(a, vector) * (1.0 - c);
}
