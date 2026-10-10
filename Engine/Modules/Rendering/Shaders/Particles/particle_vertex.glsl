// Particle vertex output modifier. The compact instance buffer is shared
// across views; each view derives its own billboard and six-way lighting basis.
// @property float wingSpeed "Wing Speed" default=30 range=0,100 group=Wings
// @property float wingMin "Wing Minimum" default=-0.15 range=-1,1 group=Wings
// @property float wingMax "Wing Maximum" default=0.15 range=-1,1 group=Wings
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV0;

#include "Includes/particle_billboard.glsl"
#include "Includes/particle_mesh_animation.glsl"
#include "Includes/view_params.glsl"
#if !defined(GE_USER_PARTICLE_LIT)
#include "Includes/view_exposure.glsl"
#endif
#if !defined(GE_COMPAT_PROFILE)
#extension GL_EXT_nonuniform_qualifier : require
#endif
#include "Includes/bindless_textures.glsl"

float ParticleWingMask(vec2 uv)
{
#if defined(GE_COMPAT_PROFILE)
    return ge_CompatSampleSlotLod(uint(GE_TEXSLOT_wingMaskMap), uv, 0.0).r;
#else
    uint slot = uint(GE_TEXSLOT_wingMaskMap);
    return textureLod(sampler2D(ge_BindlessTextures[nonuniformEXT(Mat.TextureIndices[slot])],
        ge_BindlessSamplers[nonuniformEXT((Mat.SamplerIndices >> (slot*4u)) & 0xFu)]), uv, 0.0).r;
#endif
}

void ModifyVertex(inout VertexOutput v, InstanceData inst)
{
    vec3 center = inst.modelMatrix[3].xyz;
    vec3 cameraRight = vec3(Cam.uV[0][0], Cam.uV[1][0], Cam.uV[2][0]);
    vec3 cameraUp = vec3(Cam.uV[0][1], Cam.uV[1][1], Cam.uV[2][1]);
    vec3 towardCamera = -vec3(Cam.uV[0][2], Cam.uV[1][2], Cam.uV[2][2]);
    float width = length(inst.modelMatrix[0].xyz);
    float height = length(inst.modelMatrix[1].xyz);
    vec3 velocity = inst.particleVelocityAngle.xyz;
    uint alignment = inst.particleAlignment;
    vec3 toEye = Cam.uCameraPos.xyz - center;

    v.uv0 = aUV0;
    v.custom0 = inst.custom0;
    v.particleAnimation = inst.particleAnimation;
#if !defined(GE_USER_PARTICLE_LIT)
    // Unlit colour and emission are relative to the view's exposure (particle_surface.glsl). The exposure
    // is the same for the whole draw, so it is read here, per vertex, never per fragment.
    v.particleColorScale = 1.0 / GE_ViewExposureScale();
#endif
    if (inst.particleGeometry == 2u)
    {
        float t = aPosition.z + 0.5;
        vec3 tangent = t < 0.5 ? inst.modelMatrix[0].xyz : inst.modelMatrix[1].xyz;
        vec3 side = ParticleSafeUnit(cross(towardCamera, tangent), cameraRight);
        v.position = center + velocity * (t - 0.5) + side * (aPosition.x * length(tangent));
        v.normal = towardCamera;
        v.particleBasis = mat3(side, ParticleSafeUnit(velocity, cameraUp), towardCamera);
        v.uv0 = vec2(mix(inst.particleAnimation.x, inst.particleAnimation.y, t), aUV0.x * inst.particleAnimation.z);
        v.custom0.a *= mix(inst.particleTrailAlpha.x, inst.particleTrailAlpha.y, t);
        v.particleAnimation.y = 0.0;
    }
    else if (inst.particleGeometry == 1u)
    {
        float angle = inst.particleVelocityAngle.w;
        mat3 rotation = mat3(vec3(cos(angle), sin(angle), 0), vec3(-sin(angle), cos(angle), 0), vec3(0,0,1));
        mat3 basis = mat3(inst.modelMatrix) * rotation;
        if (alignment != 0u)
        {
            mat3 orientation = GE_ParticleBillboardBasis(mat3(inst.modelMatrix),
                mat3(cameraRight, cameraUp, towardCamera), toEye, velocity,
                alignment, inst.particleGeometry, angle);
            float stretchedHeight = height + length(velocity) * max(inst.particleAnimation.z, 0.0);
            basis = orientation * mat3(vec3(width,0,0), vec3(0,stretchedHeight,0),
                vec3(0,0,length(inst.modelMatrix[2].xyz)));
        }
        v.position = center + basis * aPosition;
        if (Props.wingFlutter > 0.5)
        {
            float wingMask = ParticleWingMask(aUV0);
            // The flap phase adds the particle's index to time and offsets the vertices in
            // emitter space: the particle's size and rotation scale neither the amplitude
            // nor its up axis.
            v.position += GE_ParticleWingDisplacement(inst.modelMatrix[1].xyz, Light.uTimeParams.x,
                inst.particleIndex, Props.wingSpeed, vec2(Props.wingMin, Props.wingMax), wingMask);
        }
        v.normal = ParticleSafeUnit(transpose(inverse(basis)) * aNormal, towardCamera);
        v.particleBasis = mat3(ParticleSafeUnit(basis[0], cameraRight),
                               ParticleSafeUnit(basis[1], cameraUp),
                               ParticleSafeUnit(basis[2], towardCamera));
    }
    else
    {
        mat3 basis = GE_ParticleBillboardBasis(mat3(inst.modelMatrix), mat3(cameraRight, cameraUp, towardCamera),
            toEye, velocity, alignment, inst.particleGeometry, inst.particleVelocityAngle.w);
        vec3 rotatedRight = basis[0], rotatedUp = basis[1];
        height += length(velocity) * max(inst.particleAnimation.z, 0.0);
        if (inst.particleGeometry == 2u) height = inst.particleAnimation.w;
        // A sprite close to the camera fades out before the near plane can cut it; at 0 its corners
        // collapse onto the centre and it rasterizes nothing. The extent multiplies the corner offsets
        // by exactly 1 otherwise, so a sprite outside the fade lands on the same pixels as without it.
        float nearFade = GE_ParticleFadesNearCamera(alignment, inst.particleAnimation.z)
            ? GE_ParticleNearFade((Cam.uV * vec4(center, 1.0)).xyz, 0.5 * length(vec2(width, height)), ge_nearFar.x)
            : 1.0;
        v.custom0.a *= nearFade;
        v.particleNearFade = nearFade;
        float extent = nearFade > 0.0 ? 1.0 : 0.0;
        v.position = center + rotatedRight * (aPosition.x * width * extent) + rotatedUp * (aPosition.z * height * extent);
        v.normal = basis[2];
        v.particleBasis = basis;
    }
}
