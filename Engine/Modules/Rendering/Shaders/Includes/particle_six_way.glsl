#ifndef GE_PARTICLE_SIX_WAY_GLSL
#define GE_PARTICLE_SIX_WAY_GLSL
// Packed RGBA stores back in A.b and front in B.b.
// Our sprite-local +Z points out of the front of the sprite.
void GE_DecodeSixWayRGBA(vec4 a, vec4 b, out vec3 positive, out vec3 negative,
                         out float opacity, out float emission)
{
    positive = max(vec3(a.r, a.g, b.b), vec3(0.0));
    negative = max(vec3(b.r, b.g, a.b), vec3(0.0));
    opacity = clamp(a.a, 0.0, 1.0);
    emission = max(b.a, 0.0);
}

vec3 GE_ParticleEmission(vec3 source, vec4 color, float intensity)
{
    return max(source, vec3(0.0)) * max(color.rgb, vec3(0.0)) *
           max(color.a, 0.0) * max(intensity, 0.0);
}

// Map A holds top, left and right in RGB; map B holds bottom, back and front.
void GE_DecodeSixWayTopLeftRightBottomBackFront(vec3 topLeftRight, vec3 bottomBackFront, out vec3 positive,
                                                out vec3 negative)
{
    positive = max(vec3(topLeftRight.b, topLeftRight.r, bottomBackFront.b), vec3(0.0));
    negative = max(vec3(topLeftRight.g, bottomBackFront.r, bottomBackFront.g), vec3(0.0));
}
// Squared direction weights sum to one for a unit vector. Each signed axis
// selects exactly one RGB channel from the two linear response textures.
float GE_SixWayResponse(vec3 positive, vec3 negative, vec3 lightDirectionLocal)
{
    vec3 direction = lightDirectionLocal * inversesqrt(max(dot(lightDirectionLocal, lightDirectionLocal), 1e-12));
    vec3 response = mix(negative, positive, greaterThanEqual(direction, vec3(0.0)));
    return dot(max(response, vec3(0.0)), direction * direction);
}
#endif
