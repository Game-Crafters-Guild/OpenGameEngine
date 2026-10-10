// cbt_sculpt.glsl — sparse virtual sculpt page-table constants + index math, the GPU twin of
// CBTLayout.h (kSculpt*) / SphereSculptPaging.h. Pure math, NO bindings — so it compiles standalone
// in both the compute layout (set 0, whole-buffer ring) and the surface fragment (set 2, per-slot
// offset-bind). The consumer declares the pool + page-table SSBOs and writes the paged bilinear
// sampler around these index helpers (cbt_layout.glsl CBT_SampleSphereSculpt / cbt_surface.glsl
// CBT_SampleSculptFaceUV), both of which MUST reproduce SphereSculptPaging.h SculptResolveTexel /
// SampleSculptFaceUV bit-for-bit — the CPU/GPU bit-lock (endpoint-exact shared sampling).
//
// Write-through addressing (for the GPU modifier-bake arc): CBT_SculptPoolIndex is the WRITE-THROUGH
// twin of the read — a compute bake that resolves a page id from the table and imageStores/SSBO-
// writes at CBT_SculptPoolIndex(page, lx, ly) lands exactly the texel this sampler reads, the same
// way cbt_atlas.glsl CBT_AtlasSlotTexel relates the planar sampler and writer.
#ifndef CBT_SCULPT_GLSL
#define CBT_SCULPT_GLSL

// Mirror CBTLayout.h kSculpt* — locked to the C++ side by CBTLayoutTests.GlslSculptPageConstantsMatchCpp
// (which parses these lines). A physical page is CBT_SCULPT_PAGE_DIM^2 texels; a face's page-table
// region is a fixed cap*cap grid (row stride = cap) so the shader indexes it without pagesPerAxis.
const uint CBT_SCULPT_PAGE_DIM = 128u;
const uint CBT_SCULPT_PAGE_TEXELS = CBT_SCULPT_PAGE_DIM * CBT_SCULPT_PAGE_DIM;
const uint CBT_SCULPT_MAX_PAGES_PER_FACE_AXIS = 128u;
const uint CBT_SCULPT_PAGE_TABLE_FACE_STRIDE =
    CBT_SCULPT_MAX_PAGES_PER_FACE_AXIS * CBT_SCULPT_MAX_PAGES_PER_FACE_AXIS;
const uint CBT_SCULPT_NO_PAGE = 0xFFFFFFFFu;

// Adaptive page levels (S4, mirror CBTLayout.h kSculptPageIdMask / kSculptPageLevelShift /
// kSculptMaxPageLevel): the table entry's top byte carries the page's resolution level; a level-L
// page owns 4^L contiguous pool slots holding a (127*2^L + 1)^2 endpoint-aligned fine grid.
// Test entry == CBT_SCULPT_NO_PAGE FIRST — the sentinel's top byte is not a level.
const uint CBT_SCULPT_PAGE_ID_MASK = 0x00FFFFFFu;
const uint CBT_SCULPT_PAGE_LEVEL_SHIFT = 24u;
const uint CBT_SCULPT_MAX_PAGE_LEVEL = 2u;

// Runtime page geometry (mirror SphereSculptGeometry). The consumer fills it from its params.
struct CBTSculptGeom
{
    uint VirtualDim;   // Dv texels/face axis (radius-scaled)
    uint Cap;          // page-table row stride (CBT_SCULPT_MAX_PAGES_PER_FACE_AXIS)
    uint PagesPerAxis; // Dv / CBT_SCULPT_PAGE_DIM (unused by addressing; kept for parity/debug)
    uint PoolPageCount; // physical pages (0 -> unconfigured, sample 0)
};

// Page-table entry index for virtual page (face, pageX, pageY) — add the ring-slot base in the
// consumer. Mirror of SphereSculptLayer::PageTableEntry.
uint CBT_SculptTableEntry(uint face, uint pageX, uint pageY, uint cap)
{
    return face * CBT_SCULPT_PAGE_TABLE_FACE_STRIDE + pageY * cap + pageX;
}

// Pool linear index within a slot for (page, localX, localY) — add the ring-slot base in the
// consumer. The read/write chokepoint (mirror SphereSculptPaging.h pool index).
uint CBT_SculptPoolIndex(uint page, uint localX, uint localY)
{
    return page * CBT_SCULPT_PAGE_TEXELS + localY * CBT_SCULPT_PAGE_DIM + localX;
}

// ---- Adaptive page-level decode + fine-grid index math (S4; mirror SphereSculptPaging.h) ----

uint CBT_SculptEntryPageId(uint entry)
{
    return entry & CBT_SCULPT_PAGE_ID_MASK;
}

// Level of an entry; the caller must have rejected CBT_SCULPT_NO_PAGE first.
uint CBT_SculptEntryLevel(uint entry)
{
    return entry >> CBT_SCULPT_PAGE_LEVEL_SHIFT;
}

// Fine texels per axis of a level-L page block: 127 base cells * 2^L subdivisions + the inclusive
// endpoint. Level 0 -> 128 == CBT_SCULPT_PAGE_DIM, so the base layout is the L=0 special case of
// the same formula (the pool bytes of an unescalated store are untouched by S4).
uint CBT_SculptFineDim(uint level)
{
    return 127u * (1u << level) + 1u;
}

// Pool linear index of fine texel (jx, jy) inside the block `entry` decodes to — add the ring-slot
// base in the consumer. Mirror SphereSculptPaging.h SculptFineTexel's index expression; at level 0
// it reduces exactly to CBT_SculptPoolIndex.
uint CBT_SculptPoolIndexFine(uint entry, uint jx, uint jy)
{
    uint fdim = CBT_SculptFineDim(CBT_SculptEntryLevel(entry));
    return CBT_SculptEntryPageId(entry) * CBT_SCULPT_PAGE_TEXELS + jy * fdim + jx;
}

#endif // CBT_SCULPT_GLSL
