// GPUCullingData std430 fields, shared by frustum and both HZB phases.
// Included inside a struct body. Keep GPUScene.h offsets and reflection tests
// synchronized: 656 bytes, with renderLayerMask in the former final padding word.
    mat4 viewMatrix;
    mat4 projMatrix;
    mat4 viewProjMatrix;
    // Per-view frustum planes: [view][L,R,B,T,N,F]. For the single-view path
    // only frustumPlanes[0] is meaningful; remaining slots are ignored. For
    // multi-cascade dispatches, fill [c] for c in [0..kViewCount) with that
    // cascade's planes (or degenerate planes for empty cascades).
    vec4 frustumPlanes[4][6];
    vec3 cameraPosition;
    float nearPlane;
    vec3 cameraForward;
    float farPlane;
    // Candidate range encoded as [firstInstance, firstInstance + instanceCount)
    // in the global instance buffer, so one dispatch can operate on different
    // subranges for different views or passes.
    uint firstInstance;
    uint instanceCount;
    uint frameIndex;
    float deltaTime;
    uint cullShadowCasters; // 1 = shadow dispatch: cull instances w/o cast bit
    // Per-frustum radius inflation (std430 float-array stride 4, matches C++).
    float cullMargins[4];
    uint hzbMode;
    uint hzbMipCount;
    uint renderLayerMask;
