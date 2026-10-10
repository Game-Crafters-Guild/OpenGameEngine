#version 450
// Test-only indexed point probe: raster position identifies the LOD-local
// vertex, while the outputs expose the actual fixed-function attribute fetch.
layout(location=0) in vec3 aPosition;
layout(location=2) in vec2 aUV;
layout(location=4) in vec4 aColor;
layout(push_constant) uniform Probe { uint baseVertex; } pc;
layout(location=0) out vec4 color;
layout(location=1) out vec4 positionUv;
void main() {
    float pixel = float(gl_VertexIndex - int(pc.baseVertex)) + 0.5;
    gl_Position = vec4(pixel / 16.0 * 2.0 - 1.0, 0.0, 0.0, 1.0);
    gl_PointSize = 1.0;
    color = aColor;
    positionUv = vec4(aPosition, aUV.x);
}
