#ifndef GE_PARTICLE_MESH_ANIMATION_GLSL
#define GE_PARTICLE_MESH_ANIMATION_GLSL
// Source mesh-particle graphs offset already-scaled vertices in emitter space.
vec3 GE_ParticleWingDisplacement(vec3 emitterUp, float time, float particleIndex,
                                float speed, vec2 limits, float mask)
{
    float flap = clamp(sin((time + particleIndex) * speed), limits.x, limits.y);
    float length2 = dot(emitterUp, emitterUp);
    vec3 up = length2 > 1e-10 ? emitterUp * inversesqrt(length2) : vec3(0,1,0);
    return up * (flap * clamp(mask, 0.02, 0.5));
}
#endif
