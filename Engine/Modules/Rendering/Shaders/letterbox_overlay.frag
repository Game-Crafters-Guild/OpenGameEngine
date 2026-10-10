#version 450

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform LetterboxOverlayPC
{
    uint viewportWidth;
    uint viewportHeight;
    uint rectX;
    uint rectY;
    uint rectWidth;
    uint rectHeight;
} pc;

void main()
{
    const float x = gl_FragCoord.x;
    const float yTopLeft = float(pc.viewportHeight) - gl_FragCoord.y;

    const float rectX = float(pc.rectX);
    const float rectY = float(pc.rectY);
    const float rectW = float(pc.rectWidth);
    const float rectH = float(pc.rectHeight);

    if (x >= rectX && x < rectX + rectW &&
        yTopLeft >= rectY && yTopLeft < rectY + rectH)
    {
        discard;
    }

    outColor = vec4(0.0, 0.0, 0.0, 1.0);
}
