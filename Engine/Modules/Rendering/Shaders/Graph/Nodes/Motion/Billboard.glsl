// @category Motion
// @version 1

// @sgnode Billboard
// @display "Billboard"
// @param position "Position" hint=position
// @out out "Position"
// @stage vertex
vec3 SG_Billboard(vec3 position, InstanceData inst)
{
    vec3 centerWS = (inst.modelMatrix * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    vec3 cameraRightWS = normalize(vec3(Cam.uV[0][0], Cam.uV[1][0], Cam.uV[2][0]));
    vec3 cameraUpWS = normalize(vec3(Cam.uV[0][1], Cam.uV[1][1], Cam.uV[2][1]));
    vec3 worldPosition = centerWS + cameraRightWS * position.x + cameraUpWS * position.y;
    return (inverse(inst.modelMatrix) * vec4(worldPosition, 1.0)).xyz;
}
