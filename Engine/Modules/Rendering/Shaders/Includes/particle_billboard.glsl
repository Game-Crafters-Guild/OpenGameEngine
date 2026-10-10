#ifndef GE_PARTICLE_BILLBOARD_GLSL
#define GE_PARTICLE_BILLBOARD_GLSL
vec3 ParticleSafeUnit(vec3 v, vec3 fallback)
{
    float length2 = dot(v, v);
    return length2 > 1e-10 ? v * inversesqrt(length2) : fallback;
}

// Camera columns are right, up, and toward the viewer. This is a left-handed
// sprite basis in the engine's left-handed view convention.
mat3 GE_ParticleBillboardBasis(mat3 axes, mat3 camera, vec3 toEye, vec3 velocity,
                              uint alignment, uint geometry, float angle)
{
    vec3 right = camera[0], up = camera[1], towardCamera = camera[2];
    bool worldOriented = alignment == 0u && geometry != 2u;
    if (worldOriented)
    {
        right = ParticleSafeUnit(axes[0], vec3(1,0,0));
        up = ParticleSafeUnit(axes[1], vec3(0,1,0));
    }
    if (alignment == 4u)
    {
        up = ParticleSafeUnit(axes[1], camera[1]);
        right = ParticleSafeUnit(cross(towardCamera, up), camera[0]);
    }
    if (alignment == 5u)
    {
        towardCamera = ParticleSafeUnit(toEye, camera[2]);
        right = ParticleSafeUnit(cross(towardCamera, camera[1]), camera[0]);
        up = ParticleSafeUnit(cross(right, towardCamera), camera[1]);
    }
    if (alignment == 6u)
    {
        right = vec3(1,0,0);
        up = vec3(0,0,-1);
        worldOriented = true;
    }
    if (alignment == 7u)
    {
        towardCamera = ParticleSafeUnit(velocity, ParticleSafeUnit(axes[2], vec3(0,0,1)));
        vec3 referenceUp = abs(towardCamera.y) < 0.999 ? vec3(0,1,0) : vec3(1,0,0);
        right = ParticleSafeUnit(cross(referenceUp, towardCamera), vec3(1,0,0));
        up = ParticleSafeUnit(cross(towardCamera, right), referenceUp);
        worldOriented = true;
    }
    if (alignment == 2u || alignment == 3u || geometry == 2u)
    {
        up = ParticleSafeUnit(velocity, camera[1]);
        right = ParticleSafeUnit(cross(towardCamera, up), camera[0]);
        if (alignment == 3u && geometry != 2u)
            up = ParticleSafeUnit(cross(right, towardCamera), camera[1]);
    }
    vec3 rotatedRight = right * cos(angle) + up * sin(angle);
    vec3 rotatedUp = up * cos(angle) - right * sin(angle);
    // World-oriented sprites keep their signed lighting axes when viewed
    // from behind; camera-facing modes retain their viewer-facing normal.
    vec3 normal = ParticleSafeUnit(cross(rotatedRight, rotatedUp),
                                  worldOriented ? vec3(0,0,1) : towardCamera);
    if (!worldOriented && dot(normal, towardCamera) < 0.0) normal = -normal;
    return mat3(rotatedRight, rotatedUp, normal);
}

// Shortest fade distance, in metres: a sprite of no size still fades over a tenth of a millimetre
// rather than dividing by zero.
const float GE_PARTICLE_NEAR_FADE_MINIMUM_RADIUS = 1e-4;
// Farthest a sprite starts to fade from the near plane, in metres. The fill a near fade saves comes
// from the many puffs around a camera inside smoke, all well inside this distance; a large sprite
// seen from outside (a cloud, a fog bank) is drawn until the camera is this close to it.
const float GE_PARTICLE_NEAR_FADE_MAXIMUM_DISTANCE = 3.0;

// Whether a sprite of this alignment and authored Velocity Stretch fades near the camera: only a quad
// that turns to face the camera (FaceCamera, FaceCameraPosition, the shader's alignment codes 1 and 5)
// on an emitter that does not stretch its particles along their motion. The authored setting decides,
// not this frame's speed, so a particle at rest on a stretching emitter does not fade and then pop
// back once it moves. A ground ring, a world-oriented card or a spark keeps its look up close.
bool GE_ParticleFadesNearCamera(uint alignment, float velocityStretch)
{
    return (alignment == 1u || alignment == 5u) && velocityStretch <= 0.0;
}

// The opacity factor of a quad close to the camera, by the distance from the camera to the quad's
// centre (`viewPosition`, in view space; its length is the same whatever way the camera turns, so the
// fade does not change as the camera pans across a cloud). Straight ahead, a quad of half-diagonal
// `radius` reaches the near plane at `nearPlane` once that distance minus the radius falls below it;
// towards the edge of a wide view it reaches the plane sooner (see below). The factor
// falls from 1 to 0 as the distance goes from nearPlane + 2 * radius to nearPlane + radius, and a quad
// at 0 is not drawn: those are the largest on screen. Both ends are capped at
// GE_PARTICLE_NEAR_FADE_MAXIMUM_DISTANCE and half of it. A quad larger than the cap is drawn at full
// opacity until the camera is that close, so the near plane cuts it from nearPlane + radius inwards (a
// 20 m puff from about 14 m); it fades over the cap and is not drawn closer than half of it. A quad
// towards the edge of a wide view lies closer to the near plane than its distance says and can be cut
// before it fades.
// The procedural fog's own camera fade (GPUFogCameraFade, gpu_fog_shape.glsl) stays beside it: that
// one is a material setting that thins each fragment by its own depth over an authored range, and
// draws every fragment it thins; this one is per sprite, from its size, and stops rasterizing it.
float GE_ParticleNearFade(vec3 viewPosition, float radius, float nearPlane)
{
    float start = min(2.0 * radius, GE_PARTICLE_NEAR_FADE_MAXIMUM_DISTANCE);
    float end = min(radius, 0.5 * GE_PARTICLE_NEAR_FADE_MAXIMUM_DISTANCE);
    return clamp((length(viewPosition) - nearPlane - end) / max(start - end, GE_PARTICLE_NEAR_FADE_MINIMUM_RADIUS),
                 0.0, 1.0);
}
#endif
