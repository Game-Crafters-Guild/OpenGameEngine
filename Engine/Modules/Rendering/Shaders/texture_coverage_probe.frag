#version 450
layout(set=0,binding=0) uniform sampler2D sourceImage;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform Probe { float lod; float cutoff; vec2 phase; uint mode; uint pad0; } probe;
void main()
{
    if(probe.mode==0u)
    {
        result=texelFetch(sourceImage,ivec2(gl_FragCoord.xy),int(probe.lod));
        return;
    }
    vec4 texel=textureLod(sourceImage,uv+probe.phase,probe.lod);
    // Same inclusive boundary as the production forward/depth adapters.
    if(texel.a<probe.cutoff) discard;
    result=vec4(1.0);
}
