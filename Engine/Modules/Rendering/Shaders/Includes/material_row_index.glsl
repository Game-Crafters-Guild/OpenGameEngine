// Safe row index into the shared MaterialParams SSBO.
//
// The binding's range is exactly the rows MaterialSystem::PackMaterialSSBO
// published for this frame. When the whole material table does not fit the
// per-frame ring slot, that frame publishes the single zero-filled fallback
// row instead — so a per-instance materialIndex can point past the binding,
// and neither backend enables robust buffer access. Clamping collapses every
// draw onto the fallback row for that one frame; without it the fetch is a GPU
// out-of-bounds read.
//
// The clamp derives from the bound array length, so there is no row count to
// keep in sync with the C++ side. `rows` must be the SSBO's runtime-sized
// array; `index` is the per-instance material index.
#ifndef GE_MATERIAL_ROW_INDEX_INCLUDED
#define GE_MATERIAL_ROW_INDEX_INCLUDED

#define GE_MATERIAL_ROW_INDEX(rows, index) min(index, uint(max(rows.length(), 1) - 1))

#endif // GE_MATERIAL_ROW_INDEX_INCLUDED
