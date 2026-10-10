// @category Motion
// @version 1

// @sgnode WindSway
// @display "Wind Sway"
// @param localPos "Position (local)" hint=position
// @param time "Time"
// @param direction "Direction" default=vec3(1,0,0)
// @param amplitude "Amplitude" default=0.1
// @param frequency "Frequency" default=2
// @out offset "Position Offset" hint=position
// @stage any
// @pure
void SG_WindSway(vec3 localPos, float time, vec3 direction, float amplitude, float frequency, out vec3 offset)
{
    float h = max(localPos.y, 0.0);
    float s = sin(time * frequency + localPos.x * 0.5 + localPos.z * 0.5);
    offset = direction * (s * amplitude * h);
}
