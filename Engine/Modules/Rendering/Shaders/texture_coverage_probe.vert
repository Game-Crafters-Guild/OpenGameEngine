#version 450
layout(location=0) out vec2 uv;
void main()
{
    // Two actual card triangles; the fragment pass exercises masked rasterization.
    const vec2 corners[6]=vec2[](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));
    uv=corners[gl_VertexIndex];
    gl_Position=vec4(uv*2.0-1.0,0.5,1.0);
}
