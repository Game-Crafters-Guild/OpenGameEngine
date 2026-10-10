#version 450

// Sun glare, MULTISAMPLED scene depth. Identical to sun_glare.vert except for the depth
// uniform's type and the probe it calls: the glare reads the scene depth attachment the world
// pass wrote, and that attachment is multisampled whenever MSAA is on. The node picks this
// variant from the attachment's own sample count at declare time.
//
// The sample count travels in params.w, which is also the probe-enabled flag: it is the count
// when the probe is meaningful and 0 when it is not.

layout(set = 0, binding = 1) uniform sampler2DMS uSceneDepth;

#define GE_SUN_GLARE_PROBE(sunUV, radiusUV) \
    GE_SunGlareScreenVisibilityMS(uSceneDepth, int(pc.params.w + 0.5), sunUV, radiusUV)

#include "Includes/sun_glare_vs.glsl"
