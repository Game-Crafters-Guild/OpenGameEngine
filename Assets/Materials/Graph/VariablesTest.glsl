// @sg-graph     VariablesTest
// @sg-version   1
// @sg-stage     surface
// @sg-property  uTiling float default=0.25
// @sg-property  uTint vec3 default=1, 0.5, 0.2
//
// @sg-node      node_tiling type=FloatParameter pos=(120,80) slot=0 component=x variableName=uTiling
// @sg-node      node_tint type=Vec3Parameter pos=(120,180) slot=0 swizzle=xyz variableName=uTint
// @sg-node      node_output type=SurfaceOutput pos=(400,140)
// @sg-edge      node_tint.value -> node_output.BaseColor
// @sg-edge      node_tiling.value -> node_output.Roughness

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    o.baseColor = Mat.uParams0.xyz;
    o.roughness = Mat.uParams0.x;
    return o;
}
