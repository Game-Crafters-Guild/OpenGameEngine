// hzb_build — Slang port of hzb_build.comp (R2.1 two-phase HZB occlusion).
//
// SPIKE LANE ONLY: compiled at runtime via slangc when GE_SHADER_SLANG_LANE=1
// (HZBBuildNode::TryCompileSlangChainShader); hzb_build.comp remains the
// shipped source of truth — keep the two in lockstep or delete this port.
//
// One dispatch per destination mip. Mode 0 copies the phase-A depth into the
// pyramid base; mode 1 is a conservative 2x2 MIN reduce (reverse-Z: farthest
// surface has the SMALLEST depth, so MIN can only push a texel farther —
// weakens culling, never culls a visible instance). NPOT handling folds the
// trailing odd row/column into the last dest texel (clamped gather).
module hzb_build;

import ge_types;

[[vk::binding(0, 0)]] [format("r32f")] RTexture2DFloat uSrc;
[[vk::binding(1, 0)]] [format("r32f")] WTexture2DFloat uDst;

struct HzbPC
{
    uint dstW;
    uint dstH;
    uint srcW;
    uint srcH;
    uint mode; // 0 = copy depth into mip 0, 1 = 2x2 MIN reduce
}

[[vk::push_constant]]
HzbPC pc;

float LoadClamped(int2 c, int2 maxCoord)
{
    return uSrc.Load(clamp(c, int2(0), maxCoord));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const int2 d = int2(tid.xy);
    if (tid.x >= pc.dstW || tid.y >= pc.dstH)
        return;

    const int2 maxSrc = int2(int(pc.srcW) - 1, int(pc.srcH) - 1);

    if (pc.mode == 0)
    {
        // Mip 0: same-size copy of the phase-A depth.
        uDst.Store(d, LoadClamped(d, maxSrc));
        return;
    }

    const int2 base = d * 2;
    float farthest = LoadClamped(base, maxSrc);
    farthest = min(farthest, LoadClamped(base + int2(1, 0), maxSrc));
    farthest = min(farthest, LoadClamped(base + int2(0, 1), maxSrc));
    farthest = min(farthest, LoadClamped(base + int2(1, 1), maxSrc));

    // Odd source dimension: fold the trailing texel into the last row/column.
    const bool oddW = (pc.srcW & 1) != 0;
    const bool oddH = (pc.srcH & 1) != 0;
    const bool lastCol = tid.x == pc.dstW - 1;
    const bool lastRow = tid.y == pc.dstH - 1;

    if (oddW && lastCol)
    {
        farthest = min(farthest, LoadClamped(base + int2(2, 0), maxSrc));
        farthest = min(farthest, LoadClamped(base + int2(2, 1), maxSrc));
    }
    if (oddH && lastRow)
    {
        farthest = min(farthest, LoadClamped(base + int2(0, 2), maxSrc));
        farthest = min(farthest, LoadClamped(base + int2(1, 2), maxSrc));
    }
    if (oddW && oddH && lastCol && lastRow)
        farthest = min(farthest, LoadClamped(base + int2(2, 2), maxSrc));

    uDst.Store(d, farthest);
}
