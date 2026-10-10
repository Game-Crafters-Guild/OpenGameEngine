// cbt_page.glsl: the paged height resolve. A terrain's height field is a pyramid of 128-sample
// pages (130 with the apron) held in one physical cache texture; a page table maps each page of
// every level to the cache slot holding it. The resolve samples level L = floor(lambda) and blends
// toward its parent over the last quarter of the level's ring; a level whose page is not resident
// resolves to its first resident ancestor alone (the fallback walk), and a page that has just
// arrived fades in from its parent.
//
// LOCKSTEP with the CPU mirror PagedHeightSampler (Engine/Modules/TerrainECS/.../PagedHeightSampler.
// {h,cpp}): the same walk, the same order of operations, so the two agree bit for bit on the same
// texels. The block between the GE_SHARED_PAGE_RESOLVE markers is compiled as C++ by
// TerrainAtlasTests (GlslShim.h) and swept against the mirror: write it component-wise (no
// swizzles), float literals suffixed `f`, no out parameters.
//
// Pure math, no bindings. The consumer defines the four taps declared below, after including this
// file (GLSL resolves a prototype at link):
//   uint CBT_PageTableEntry(uint index);       // the table's entry `index` (level-major, row-major)
//   float CBT_PageSlotFade(uint slot);         // the arrival fade of a slot: 0 just assigned, 1 settled
//   CBTPageLevel CBT_PageLevelShape(uint level);
//   float CBT_PageCacheBilinear(float texelX, float texelZ);
// The last one reads the cache bilinearly at absolute texel coordinates (an integer is a texel's
// centre): on the GPU a linear-clamp sampler at (texel + 0.5) / CacheDim. The page's apron keeps
// every tap inside its slot, so the sampler never blends a neighboring slot in.
#ifndef CBT_PAGE_GLSL
#define CBT_PAGE_GLSL

// GE_SHARED_PAGE_TYPES_BEGIN
// One level of the pyramid (mirror PageStreaming::PageStoreLevel's page counts). std430, 16 B.
struct CBTPageLevel
{
    uint FirstEntry; // the level's first entry in the table
    uint PagesX;
    uint PagesZ;
    uint Pad;
};

// The field a resolve reads (mirror PageTable's level-0 shape and PageCacheGeometry).
struct CBTPageField
{
    uint Level0SamplesX;
    uint Level0SamplesZ;
    uint LevelCount;  // 0: no field, the resolve answers 0
    uint SlotsPerRow; // the cache texture's slots per row (square-packed)
};

// A level's sample and the resident level that answered it.
struct CBTPageLevelSample
{
    float Height;
    uint Level; // LevelCount when no page down the walk is resident
};
// GE_SHARED_PAGE_TYPES_END

// The consumer's taps (see the header).
uint CBT_PageTableEntry(uint index);
float CBT_PageSlotFade(uint slot);
CBTPageLevel CBT_PageLevelShape(uint level);
float CBT_PageCacheBilinear(float texelX, float texelZ);

// The normalized coordinate of absolute cache texel (texelX, texelZ) in a cache of `cacheDim`
// texels a side: what a consumer's CBT_PageCacheBilinear hands its linear-clamp sampler.
vec2 CBT_PageCacheUV(float texelX, float texelZ, vec2 cacheDim)
{
    return (vec2(texelX, texelZ) + 0.5) / cacheDim;
}

// GE_SHARED_PAGE_RESOLVE_BEGIN
// Mirror PageStreaming::kNoPage, kPageSlotMask, kPageOwnedSamples, kPageApronSamples, kPageStrideSamples.
const uint CBT_PAGE_NO_PAGE = 0xFFFFFFFFu;
const uint CBT_PAGE_SLOT_MASK = 0x00FFFFFFu;
const uint CBT_PAGE_OWNED = 128u;
const uint CBT_PAGE_APRON = 1u;
const uint CBT_PAGE_STRIDE = 130u;

// Mirror PageParentBlend: 0 over the first three quarters of a level's ring, a linear ramp to 1
// over the last quarter.
float CBT_PageParentBlend(float level)
{
    float inRing = level - floor(level);
    return clamp((inRing - 0.75f) / (1.0f - 0.75f), 0.0f, 1.0f);
}

// Level `level`'s lattice coordinate of terrain coordinate t in [0, 1] along an axis of
// `level0Samples`: level-0 sample i at i / (samples - 1), level-L sample j at level-0 j * 2^L.
float CBT_PageLattice(float t, uint level0Samples, uint level)
{
    return clamp(t, 0.0f, 1.0f) * float(level0Samples - 1u) / float(1u << level);
}

// The page along an axis holding lattice coordinate c.
uint CBT_PageIndex(float c, uint pages)
{
    return min(uint(c) / CBT_PAGE_OWNED, pages - 1u);
}

// The table entry of the page of `level` holding terrain UV (u, v).
uint CBT_PageEntryAt(float u, float v, uint level, CBTPageField field)
{
    CBTPageLevel shape = CBT_PageLevelShape(level);
    uint pageX = CBT_PageIndex(CBT_PageLattice(u, field.Level0SamplesX, level), shape.PagesX);
    uint pageZ = CBT_PageIndex(CBT_PageLattice(v, field.Level0SamplesZ, level), shape.PagesZ);
    return CBT_PageTableEntry(shape.FirstEntry + pageZ * shape.PagesX + pageX);
}

// Level `level` at (u, v) from the page in `slot`: bilinear at the lattice coordinate inside the
// page's stride (its owned samples offset by the apron).
float CBT_PageSampleSlot(float u, float v, uint level, uint slot, CBTPageField field)
{
    CBTPageLevel shape = CBT_PageLevelShape(level);
    float cx = CBT_PageLattice(u, field.Level0SamplesX, level);
    float cz = CBT_PageLattice(v, field.Level0SamplesZ, level);
    uint pageX = CBT_PageIndex(cx, shape.PagesX);
    uint pageZ = CBT_PageIndex(cz, shape.PagesZ);
    float strideX = cx - float(pageX * CBT_PAGE_OWNED) + float(CBT_PAGE_APRON);
    float strideZ = cz - float(pageZ * CBT_PAGE_OWNED) + float(CBT_PAGE_APRON);
    float originX = float((slot % field.SlotsPerRow) * CBT_PAGE_STRIDE);
    float originZ = float((slot / field.SlotsPerRow) * CBT_PAGE_STRIDE);
    return CBT_PageCacheBilinear(originX + strideX, originZ + strideZ);
}

// Level `level` alone at (u, v): its page when resident, faded in from its parent; else its first
// resident ancestor's. A fading page's parent may be fading too, so the walk carries the weight
// still owed to the levels above: each resident page takes `fade` of what is left, a settled page
// (or the top level) takes all of it.
CBTPageLevelSample CBT_PageSampleLevel(float u, float v, uint level, CBTPageField field)
{
    CBTPageLevelSample result;
    result.Height = 0.0f;
    result.Level = field.LevelCount;
    float remaining = 1.0f;
    for (uint walk = level; walk < field.LevelCount; ++walk)
    {
        uint entry = CBT_PageEntryAt(u, v, walk, field);
        if (entry == CBT_PAGE_NO_PAGE)
            continue; // the fallback walk: the first resident ancestor answers
        uint slot = entry & CBT_PAGE_SLOT_MASK;
        float own = CBT_PageSampleSlot(u, v, walk, slot, field);
        float fade = CBT_PageSlotFade(slot);
        if (result.Level == field.LevelCount)
            result.Level = walk;
        if (fade >= 1.0f || walk + 1u >= field.LevelCount)
        {
            result.Height = result.Height + remaining * own;
            return result;
        }
        result.Height = result.Height + remaining * fade * own;
        remaining = remaining * (1.0f - fade);
    }
    return result;
}

// The height at terrain UV (u, v) at continuous level `lambda`: level L's sample blended toward
// level L + 1's by CBT_PageParentBlend, or the fallback ancestor alone when L is not resident.
float CBT_PageSample(float u, float v, float lambda, CBTPageField field)
{
    if (field.LevelCount == 0u)
        return 0.0f;
    uint top = field.LevelCount - 1u;
    float clamped = clamp(lambda, 0.0f, float(top));
    uint base = min(uint(clamped), top);
    CBTPageLevelSample own = CBT_PageSampleLevel(u, v, base, field);
    float w = base < top ? CBT_PageParentBlend(clamped) : 0.0f;
    if (w <= 0.0f || own.Level > base)
        return own.Height; // the fallback ancestor answers alone: its own parent is coarser still
    CBTPageLevelSample parent = CBT_PageSampleLevel(u, v, base + 1u, field);
    return own.Height + (parent.Height - own.Height) * w;
}
// GE_SHARED_PAGE_RESOLVE_END

#endif // CBT_PAGE_GLSL
