// ge_types — shared helpers for the Slang port of the engine adapter set.
//
// GLSL matrix constructors fill COLUMNS; Slang float3x3(...) fills ROWS.
// FromColumns* keeps ported call sites intent-revealing: a GLSL
// `mat3(c0,c1,c2) * v` becomes `mul(GE_FromColumns3(c0,c1,c2), v)`.
module ge_types;

public float3x3 GE_FromColumns3(float3 c0, float3 c1, float3 c2)
{
    return transpose(float3x3(c0, c1, c2));
}

public float4x4 GE_FromColumns4(float4 c0, float4 c1, float4 c2, float4 c3)
{
    return transpose(float4x4(c0, c1, c2, c3));
}

// Storage-texture aliases: Slang core exposes access-qualified textures only
// through the generic _Texture form; these aliases match GLSL's
// `readonly image2D` / `writeonly image2D`.
public typealias RTexture2DFloat = _Texture<float, __Shape2D, 0, 0, 0, /*access*/1, 0, 0, 0>;
public typealias WTexture2DFloat = _Texture<float, __Shape2D, 0, 0, 0, /*access*/2, 0, 0, 0>;

// GLSL bitfieldExtract(int(u), 0, 21) — sign-extend the low 21 bits.
public int GE_SignExtend21(uint u)
{
    return (int(u << 11)) >> 11;
}
