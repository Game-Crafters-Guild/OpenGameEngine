#version 450
layout(location=0) in vec4 color;
layout(location=1) in vec4 positionUv;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outPositionUv;
void main() { outColor = color; outPositionUv = positionUv; }
