// Instance data interface for the flexible render pipeline.
//
// When GE_INSTANCED is defined, shaders fetch per-instance data from SSBOs
// using gl_InstanceIndex. The indirection buffer maps gl_InstanceIndex to
// GPUScene instance indices, enabling instanced batching of compatible draws.
//
// When GE_INSTANCED is NOT defined, instance data comes from push constants
// (the non-instanced per-draw path). The InstanceData struct is still populated so
// surface shaders and vertex modifiers have a uniform interface.

#ifndef GE_INSTANCE_IO_GLSL
#define GE_INSTANCE_IO_GLSL

// Bit layout of the crossfade channel packed into the indirection word by
// draw_command_scatter.comp. Included unconditionally because the instance-index
// mask below is unconditional — the high byte is zero on every non-fading
// record, but the mask is what guarantees it.
#include "lod_crossfade.glsl"

// Per-instance data visible to surface shaders and vertex modifiers.
// This is the "contract" between the engine adapter and user shader code.
struct InstanceData
{
    mat4 modelMatrix;
    mat3 normalMatrix;
    uint instanceIndex;       // GPUScene instance index (for accessing other SSBO data)
    uint materialIndex;       // GPUScene material index
    uint skinPaletteOffset;   // Offset (in bone-slot units; 1 slot = 3 vec4 rows, see bone_palette.glsl) into the bone palette atlas SSBO (0 if unskinned)
    uint flags;               // Per-instance flags: bit0=castShadows, bit1=receiveShadows (GPUScene.h)
    vec4 custom0;             // Per-instance shader payload.
    ivec3 sector;             // Render-origin sector (camera-relative rendering); (0,0,0) = sector-local == world.
#ifdef GE_USER_PARTICLE_BUFFER
    vec4 particleVelocityAngle;
    vec4 particleAnimation;
    uint particleAlignment;
    uint particleGeometry;
    vec2 particleTrailAlpha;
    float particleIndex;
#endif
#ifdef GE_LOD_CROSSFADE
    uint lodFadeCode;         // Packed dither weight + phase (lod_crossfade.glsl); 0 = not fading.
#endif
#ifdef GE_VERTEX_DEFORMATION
    float deformationTimeSeconds;   // Seconds since the deformation origin, this endpoint.
    float deformationScrollSeconds; // Bounded scroll clock this endpoint deforms at (wraps).
#endif
};

// Decode the packed 21-bit-per-axis render-origin sector from the two reserved
// GPUInstance words. bitfieldExtract sign-extends the 21-bit two's-complement
// axis. C++ pack/unpack mirror: Engine/Rendering/RenderOrigin.h (locked by
// RenderOriginTests). Kept out of gpu_instance_fields.glsl because that file is
// #included inside a struct body.
ivec3 ge_UnpackInstanceSector(uint p0, uint p1)
{
    uint ux = p0 & 0x1FFFFFu;
    uint uy = ((p0 >> 21) & 0x7FFu) | ((p1 & 0x3FFu) << 11);
    uint uz = (p1 >> 10) & 0x1FFFFFu;
    return ivec3(bitfieldExtract(int(ux), 0, 21),
                 bitfieldExtract(int(uy), 0, 21),
                 bitfieldExtract(int(uz), 0, 21));
}

#ifdef GE_VERTEX_DEFORMATION
// The deformation clock is part of the endpoint, not a global: a vertex modifier
// evaluated against an InstanceData deforms at that value's clock, so the same
// modifier can run at a second endpoint by receiving a second InstanceData. Every
// site that builds an InstanceData stamps both lanes from the per-view light
// block. An includer that defines GE_VERTEX_DEFORMATION declares that block
// (`Light`, set 0 binding 6) before this file; adapter_vertex.glsl does.
//
// The endpoint lanes are zw, not the xy globals a fragment stage reads: the
// animation lane is seconds since the deformation origin every view shares
// (ViewTemporalHistory), so two endpoints of one frame difference to exactly
// that frame's delta at any uptime. Absolute uptime does not — its fp32 spacing reaches a frame's length
// after a few days of uptime and both endpoints round to the same value.
void ge_StampDeformationClock(inout InstanceData inst)
{
    inst.deformationTimeSeconds = Light.uTimeParams.z;
    inst.deformationScrollSeconds = Light.uTimeParams.w;
}

#ifdef GE_MOTION_VECTORS
// The previous endpoint's clock and the view-projection it projects through.
#include "deformation_motion.glsl"

// Mirror of ge_StampDeformationClock for the previous endpoint: the same two
// lanes, read from the frame this view last rendered.
void ge_StampPreviousDeformationClock(inout InstanceData inst)
{
    inst.deformationTimeSeconds = MotionParams.uMotionPrevTimeParams.x;
    inst.deformationScrollSeconds = MotionParams.uMotionPrevTimeParams.y;
}
#endif
#endif

#if defined(GE_USER_PARTICLE_BUFFER) && defined(GE_INSTANCED) && !defined(CUSTOM_VERTEX_SHADER)
// One particle record, laid out as ParticleRenderData on the CPU. Both instanced
// fetch forms read the same record: the compatibility profile from a set-0 SSBO,
// the device-address path through a buffer reference.
struct ParticleInstanceData
{
#include "particle_instance_fields.glsl"
};

// The InstanceData a particle record draws with. `index` is the record's slot in
// this frame's particle upload. The size scales the record's basis, so sprites,
// meshes and ribbons share one model matrix form.
InstanceData ge_ParticleInstanceData(ParticleInstanceData p, uint index)
{
    InstanceData inst;
    float size = p.positionSize.w;
    inst.modelMatrix = mat4(vec4(p.axisX.xyz * size, 0.0), vec4(p.axisY.xyz * size, 0.0),
                            vec4(p.axisZ.xyz * size, 0.0), vec4(p.positionSize.xyz, 1.0));
    inst.normalMatrix = mat3(p.axisX.xyz, p.axisY.xyz, p.axisZ.xyz);
    inst.instanceIndex = index;
    inst.materialIndex = p.metadata.x;
    inst.flags = p.metadata.z;
    inst.skinPaletteOffset = 0u;
    inst.custom0 = p.color;
    inst.sector = ivec3(0);
    inst.particleVelocityAngle = p.velocityAngle;
    inst.particleAnimation = p.animation;
    inst.particleAlignment = p.metadata.y;
    inst.particleGeometry = p.metadata.w;
    inst.particleTrailAlpha = vec2(p.axisX.w, p.axisY.w);
    inst.particleIndex = p.axisZ.w;
#ifdef GE_LOD_CROSSFADE
    inst.lodFadeCode = 0u; // particle records carry no mesh LOD chain to fade
#endif
#ifdef GE_VERTEX_DEFORMATION
    ge_StampDeformationClock(inst);
#endif
    return inst;
}
#endif

#if defined(CUSTOM_VERTEX_SHADER)

// Fully-procedural geometry (CBT/LEB, GPU-driven): there is no vertex buffer,
// no per-draw instance SSBO, and no push-constant block. The vertex modifier
// derives all geometry from gl_InstanceIndex (e.g. a CBT bisector id), so the
// fetch is a pure identity that just passes the raw index through. Suppresses
// both the instanced SSBO/buffer-reference path and the legacy push-constant
// path so neither declares resources the procedural draw never binds.
InstanceData ge_FetchInstanceData()
{
    InstanceData inst;
    inst.modelMatrix = mat4(1.0);
    inst.normalMatrix = mat3(1.0);
    inst.instanceIndex = uint(gl_InstanceIndex);
    inst.materialIndex = 0u;
    inst.skinPaletteOffset = 0u;
    inst.flags = 2u; // default receiveShadows on (bit 1)
    inst.custom0 = vec4(1.0);
    inst.sector = ivec3(0); // procedural geometry rebases in its own domain (CBT)
#ifdef GE_LOD_CROSSFADE
    inst.lodFadeCode = 0u; // procedural geometry has no mesh LOD chain to fade
#endif
#ifdef GE_VERTEX_DEFORMATION
    ge_StampDeformationClock(inst);
#endif
    return inst;
}

#elif defined(GE_INSTANCED) && defined(GE_COMPAT_PROFILE)

#ifdef GE_USER_PARTICLE_BUFFER
// The CPU orders a separate upload per view; the 32-bit base avoids relying
// on firstInstance surviving shader translation on compatibility backends.
layout(std430, set = 0, binding = 29) readonly buffer ParticleInstancesSSBO
{
    ParticleInstanceData particles[];
} ParticleInstances;
layout(push_constant) uniform CompatParticlePC
{
    uint ge_compatBaseParticle;
} pc_particle;

InstanceData ge_FetchInstanceData()
{
    uint index = pc_particle.ge_compatBaseParticle + uint(gl_InstanceIndex);
    return ge_ParticleInstanceData(ParticleInstances.particles[index], index);
}
#else
// Compatibility profile: no buffer_device_address and no uint64 push constants
// (WebGPU has neither), so the instance buffer is a plain set-0 SSBO.
//
// The indirection is CpuDrawStreamBuilder's per-view instance index list,
// bound as a set-0 SSBO: each (material, mesh) batch's visible GPUScene
// indices sit contiguously in it, the batch's first entry rides a 32-bit push
// constant, and one instanced draw covers the batch however its scene indices
// are spread. This reads instance `list[first + gl_InstanceIndex]`.
//
// firstInstance is deliberately not the carrier for `first`: it does not
// reach gl_InstanceIndex through SPIRV-Cross/Metal, and a silently-zero base
// draws every batch from entry 0 (observed). The instanceCount-driven half of
// gl_InstanceIndex — 0..instanceCount-1 — is the portable half. On WebGPU the
// constant costs one emulated push-constant slot per draw either way: the
// backend writes a slot for every draw of a pipeline that declares the block.
struct GPUInstanceData
{
#include "gpu_instance_fields.glsl"
};

layout(std430, set = 0, binding = 29) readonly buffer GPUInstancesSSBO
{
    GPUInstanceData ge_instances[];
} GPUInstances;

// Bound by name (CpuDrawStreamBuilder::kIndexListBindingName).
layout(std430, set = 0, binding = 50) readonly buffer CompatInstanceListSSBO
{
    uint ge_sceneIndices[];
} CompatInstanceList;

layout(push_constant) uniform CompatInstancePC
{
    uint ge_compatListFirst;
} pc_compat;

InstanceData ge_FetchInstanceData()
{
    uint sceneIdx =
        CompatInstanceList.ge_sceneIndices[pc_compat.ge_compatListFirst + uint(gl_InstanceIndex)];
    GPUInstanceData gpuInst = GPUInstances.ge_instances[sceneIdx];

    InstanceData inst;
    inst.modelMatrix = gpuInst.transform;
    inst.normalMatrix = mat3(gpuInst.normalMatrixCol0.xyz,
                             gpuInst.normalMatrixCol1.xyz,
                             gpuInst.normalMatrixCol2.xyz);
    inst.instanceIndex = sceneIdx;
    inst.materialIndex = gpuInst.materialIndex;
    inst.skinPaletteOffset = gpuInst.skinPaletteOffset;
    inst.flags = gpuInst.flags;
    inst.custom0 = gpuInst.custom0;
    inst.sector = ge_UnpackInstanceSector(gpuInst.sectorPacked0, gpuInst.sectorPacked1);
#ifdef GE_LOD_CROSSFADE
    inst.lodFadeCode = 0u; // per-instance draws carry no fade record channel
#endif
#ifdef GE_VERTEX_DEFORMATION
    ge_StampDeformationClock(inst);
#endif
    return inst;
}

#ifdef GE_MOTION_VECTORS
// The scene index comes from the caller's already-fetched endpoint, so the
// list is not read again per vertex.
mat4 ge_FetchPreviousModelMatrix(uint sceneIdx)
{
    return GPUInstances.ge_instances[sceneIdx].prevTransform;
}
#endif

#endif // GE_USER_PARTICLE_BUFFER

#elif defined(GE_INSTANCED)

// GL_EXT_buffer_reference: instanced draws consume an instance-indirection
// buffer and GPUScene's per-frame instance buffer via device addresses
// delivered in push constants. World draws read GPUDrawStreamBuilder's shared indirection
// buffer, which the scatter writes; passes that sort their own instances
// (sorted transparents, smoke particles) upload their own. This removes both
// SSBOs from the set-0 descriptor layout so the set's bindings are all
// app-lifetime (Cam + optional BonePaletteAtlas), enabling the persistent
// set-0 cache to work for the instanced depth path. See audit §7.1.7.

// Push-constant block carrying the two device addresses. Declared here
// because it is specific to the GE_INSTANCED draw path. The non-instanced
// path declares its own legacy PC block with model/normal matrices.
// ComposedPCInstanced on the C++ side matches this layout.
layout(push_constant) uniform InstancedPC
{
    uint64_t ge_indirectionAddr;   // instance-indirection buffer (any offset already folded in)
    uint64_t ge_gpuInstancesAddr;  // GPUScene per-frame instance buffer base
} pc_inst;

// Instance indirection buffer: maps gl_InstanceIndex to GPUScene instance
// index. Read via device address instead of an SSBO descriptor so a changing
// handle (arena growth, per-slot drain buffers) does not force set-0
// descriptor rebuilds.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer InstanceIndirectionBuffer
{
    uint ge_instanceIndirection[];
};

// GPUScene instance buffer: per-instance transforms and metadata.
struct GPUInstanceData
{
#include "gpu_instance_fields.glsl"
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GPUInstancesBuffer
{
    GPUInstanceData ge_instances[];
};

// Fetch InstanceData for the current instance (instanced path).
#ifdef GE_USER_PARTICLE_BUFFER
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ParticleInstancesBuffer
{
    ParticleInstanceData particles[];
};
#endif

InstanceData ge_FetchInstanceData()
{
#ifdef GE_USER_PARTICLE_BUFFER
    InstanceIndirectionBuffer indices = InstanceIndirectionBuffer(pc_inst.ge_indirectionAddr);
    uint index = indices.ge_instanceIndirection[gl_InstanceIndex];
    ParticleInstancesBuffer data = ParticleInstancesBuffer(pc_inst.ge_gpuInstancesAddr);
    return ge_ParticleInstanceData(data.particles[index], index);
#else
    InstanceIndirectionBuffer indirBuf = InstanceIndirectionBuffer(pc_inst.ge_indirectionAddr);
    GPUInstancesBuffer       gpuInstBuf = GPUInstancesBuffer(pc_inst.ge_gpuInstancesAddr);

    // The indirection word is the scene instance index in its low 24 bits; the
    // high byte carries the LOD-crossfade code and is zero on every other
    // record. Masking is unconditional so the index stays correct in variants
    // compiled without the keyword — they simply ignore the code.
    uint indirWord = indirBuf.ge_instanceIndirection[gl_InstanceIndex];
    uint sceneIdx = indirWord & kLodFadeIndexMask;
    GPUInstanceData gpuInst = gpuInstBuf.ge_instances[sceneIdx];

    InstanceData inst;
    inst.modelMatrix = gpuInst.transform;
    inst.normalMatrix = mat3(gpuInst.normalMatrixCol0.xyz,
                             gpuInst.normalMatrixCol1.xyz,
                             gpuInst.normalMatrixCol2.xyz);
    inst.instanceIndex = sceneIdx;
    inst.materialIndex = gpuInst.materialIndex;
    inst.skinPaletteOffset = gpuInst.skinPaletteOffset;
    inst.flags = gpuInst.flags;
    inst.custom0 = gpuInst.custom0;
    inst.sector = ge_UnpackInstanceSector(gpuInst.sectorPacked0, gpuInst.sectorPacked1);
#ifdef GE_LOD_CROSSFADE
    inst.lodFadeCode = indirWord & ~kLodFadeIndexMask;
#endif
#ifdef GE_VERTEX_DEFORMATION
    ge_StampDeformationClock(inst);
#endif
    return inst;
#endif
}

#ifdef GE_MOTION_VECTORS
// The scene index comes from the caller's already-fetched endpoint, so the
// indirection word is read ONCE per vertex rather than once per endpoint.
mat4 ge_FetchPreviousModelMatrix(uint sceneIdx)
{
    GPUInstancesBuffer gpuInstBuf = GPUInstancesBuffer(pc_inst.ge_gpuInstancesAddr);
    return gpuInstBuf.ge_instances[sceneIdx].prevTransform;
}
#endif

#else // !GE_INSTANCED

// Push-constant path (legacy, non-instanced).
// The adapter vertex shader populates InstanceData from push constants.
// This declaration is provided so ge_FetchInstanceData() always exists,
// but the actual data is filled by the adapter, not fetched from SSBOs.

// Placeholder: adapter_vertex.glsl provides the implementation for push-constant path.

#endif // GE_INSTANCED

#endif // GE_INSTANCE_IO_GLSL
