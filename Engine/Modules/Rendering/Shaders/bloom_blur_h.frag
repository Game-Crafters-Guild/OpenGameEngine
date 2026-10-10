#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 oColor;
layout(set=0,binding=0) uniform sampler2D uSrc;

// 9-tap Gaussian blur horizontally
const float w[5] = float[](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);

void main(){
    ivec2 sz = textureSize(uSrc,0);
    vec2 texel = vec2(1.0/float(sz.x), 0.0);
    vec3 c = texture(uSrc, vUV).rgb * w[0];
    for(int i=1;i<5;++i){
        c += texture(uSrc, vUV + texel*float(i)).rgb * w[i];
        c += texture(uSrc, vUV - texel*float(i)).rgb * w[i];
    }
    oColor = vec4(c,1.0);
}

