#ifndef GE_OCTAHEDRAL_NORMAL_GLSL
#define GE_OCTAHEDRAL_NORMAL_GLSL

// Octahedral unit-vector encode/decode (Cigolle et al. 2014, "A Survey of
// Efficient Representations for Independent Unit Vectors"). Packs a normalized
// vector into two [0,1] components; GE_OctDecode is the exact inverse of
// GE_OctEncode. Storing a normal in two channels frees the other two channels of
// an RGBA target for extra material data (the SSR G-buffer packs roughness +
// metallic there). Precision is far better than a raw xyz*0.5+0.5 in 8/16-bit.

vec2 GE_OctEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = (n.z >= 0.0)
                 ? n.xy
                 : (vec2(1.0) - abs(n.yx)) *
                       vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}

vec3 GE_OctDecode(vec2 f)
{
    vec2 e = f * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

#endif // GE_OCTAHEDRAL_NORMAL_GLSL
