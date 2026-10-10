// GPUInstance std430 field list — the single GLSL source for the C++
// GPUScene.h::GPUInstance mirror (240 B stride, static_assert-locked on the
// C++ side). Included INSIDE a struct body:
//
//     struct GPUInstance {
//     #include "Includes/gpu_instance_fields.glsl"   // from Shaders/*.comp
//     };
//
// (instance_io.glsl includes it as "gpu_instance_fields.glsl" — same dir.)
// Field order and types are ABI: any change here must change GPUScene.h and
// its static_asserts in the same commit.
    mat4 transform;
    mat4 prevTransform;
    // Precomputed normal matrix = transpose(inverse(mat3(transform))),
    // columns in xyz (the first spare word stores layers); CPU-side fill keeps
    // mat3 assembly.
    vec3 normalMatrixCol0;
    uint renderLayerMask;
    vec4 normalMatrixCol1;
    vec4 normalMatrixCol2;
    uint meshIndex;
    uint materialIndex;
    uint flags;             // bit 0 cast shadows, bit 1 receive shadows,
                            // bit 2 depth material-dependent, bit 3 depth double-sided
                            // (meaningful only when bit 2 clear), bit 4 mirrored winding
                            // (negative-determinant world transform), bits 16..31 = 16-bit world tag
    float lodBias;          // log2 coverage scale consumed by ge_SelectLOD
    vec3 boundingCenter;    // world-space bounding sphere
    float boundingRadius;
    uint skinPaletteOffset;
    uint runtimeId;         // SkeletonStore runtime id (per-runtime visibility OR)
    // Packed 21-bit-per-axis render-origin sector (camera-relative rendering).
    // Zero => sector (0,0,0) => transform column is full world space and the
    // bytes match the pre-camera-relative layout exactly, so CPU memcmp still
    // treats an untagged instance's 240 B as identity (quiescence preserved).
    // Decode via ge_UnpackInstanceSector (instance_io.glsl); pack mirror is
    // Engine/Rendering/RenderOrigin.h::PackSector.
    uint sectorPacked0;
    uint sectorPacked1;
    vec4 custom0;
