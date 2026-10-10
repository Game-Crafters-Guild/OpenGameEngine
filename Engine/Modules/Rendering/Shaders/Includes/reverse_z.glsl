// Reverse-Z depth linearization (near=1.0, far=0.0 -> view-space meters).
// Requires view_params_fields.glsl (ViewParams.ge_nearFar) to be included first.

float LinearizeReverseZ(float depth)
{
    float zNear = max(ViewParams.ge_nearFar.x, 1e-5);
    float zFar = max(ViewParams.ge_nearFar.y, zNear + 1.0);
    return (zNear * zFar) / max(depth * (zFar - zNear) + zNear, 1e-6);
}
