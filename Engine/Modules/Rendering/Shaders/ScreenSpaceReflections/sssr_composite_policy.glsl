#ifndef GE_SSSR_COMPOSITE_POLICY_GLSL
#define GE_SSSR_COMPOSITE_POLICY_GLSL
vec3 GE_SssrComposite(vec3 scene, vec3 radiance, vec3 probe, vec3 specularWeight, float replacement)
{
    return max(scene + replacement * specularWeight * (radiance - probe), vec3(0.0));
}
#endif
