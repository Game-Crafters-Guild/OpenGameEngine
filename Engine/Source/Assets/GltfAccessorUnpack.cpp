#include "GltfAccessorUnpack.h"

#if defined(GE_HAVE_CGLTF)

#include <cgltf.h>

#include <algorithm>
#include <string_view>

namespace GameEngine
{

namespace
{

constexpr std::string_view kDataNotLoaded = "whose data does not load (its buffer, or the buffer of its sparse "
                                            "indices or values, holds no data); re-export it with every buffer "
                                            "embedded or beside it.";

// The accessor that reads a sparse block's `count` indices, tightly packed from their buffer view.
cgltf_accessor SparseIndicesAccessor(const cgltf_accessor_sparse& sparse)
{
    cgltf_accessor indices{};
    indices.component_type = sparse.indices_component_type;
    indices.type = cgltf_type_scalar;
    indices.count = sparse.count;
    indices.buffer_view = sparse.indices_buffer_view;
    indices.offset = sparse.indices_byte_offset;
    indices.stride = cgltf_component_size(sparse.indices_component_type);
    return indices;
}

// The accessor that reads the sparse values of `accessor`, tightly packed from their buffer view.
cgltf_accessor SparseValuesAccessor(const cgltf_accessor& accessor)
{
    cgltf_accessor values{};
    values.component_type = accessor.component_type;
    values.normalized = accessor.normalized;
    values.type = accessor.type;
    values.count = accessor.sparse.count;
    values.buffer_view = accessor.sparse.values_buffer_view;
    values.offset = accessor.sparse.values_byte_offset;
    values.stride = cgltf_calc_size(accessor.type, accessor.component_type);
    return values;
}

// The reason an accessor of `count` elements is refused for sparse index `index`, worded to follow
// "an accessor".
std::string SparseIndexPastCount(uint32 index, size_t count)
{
    return "whose sparse index " + std::to_string(index) + " is at or past its count of " + std::to_string(count) +
           "; re-export it.";
}

} // namespace

std::string UnpackAccessorFloats(const cgltf_accessor& accessor, Vector<float>& out, GltfSparseScratch& scratch)
{
    const size_t components = cgltf_num_components(accessor.type);
    const size_t floatCount = accessor.count * components;
    out.resize(floatCount);

    cgltf_accessor dense = accessor;
    dense.is_sparse = 0;
    if (cgltf_accessor_unpack_floats(&dense, out.data(), floatCount) != floatCount)
        return std::string(kDataNotLoaded);
    if (!accessor.is_sparse)
        return {};

    const size_t entryCount = accessor.sparse.count;
    const cgltf_accessor indices = SparseIndicesAccessor(accessor.sparse);
    scratch.Indices.resize(entryCount);
    if (cgltf_accessor_unpack_indices(&indices, scratch.Indices.data(), sizeof(uint32), entryCount) != entryCount)
        return std::string(kDataNotLoaded);

    const cgltf_accessor values = SparseValuesAccessor(accessor);
    scratch.Values.resize(entryCount * components);
    if (cgltf_accessor_unpack_floats(&values, scratch.Values.data(), scratch.Values.size()) != scratch.Values.size())
        return std::string(kDataNotLoaded);

    for (size_t entry = 0; entry < entryCount; ++entry)
    {
        const uint32 index = scratch.Indices[entry];
        if (index >= accessor.count)
            return SparseIndexPastCount(index, accessor.count);
        std::copy_n(scratch.Values.data() + entry * components, components, out.data() + index * components);
    }
    return {};
}

std::string UnpackAccessorIntegers(const cgltf_accessor& accessor, Vector<uint32>& out, GltfSparseScratch& scratch)
{
    const size_t components = cgltf_num_components(accessor.type);
    out.resize(accessor.count * components);

    cgltf_accessor dense = accessor;
    dense.is_sparse = 0;
    for (size_t element = 0; element < accessor.count; ++element)
    {
        if (!cgltf_accessor_read_uint(&dense, element, out.data() + element * components, components))
            return std::string(kDataNotLoaded);
    }
    if (!accessor.is_sparse)
        return {};

    const size_t entryCount = accessor.sparse.count;
    const cgltf_accessor indices = SparseIndicesAccessor(accessor.sparse);
    scratch.Indices.resize(entryCount);
    if (cgltf_accessor_unpack_indices(&indices, scratch.Indices.data(), sizeof(uint32), entryCount) != entryCount)
        return std::string(kDataNotLoaded);

    const cgltf_accessor values = SparseValuesAccessor(accessor);
    for (size_t entry = 0; entry < entryCount; ++entry)
    {
        const uint32 index = scratch.Indices[entry];
        if (index >= accessor.count)
            return SparseIndexPastCount(index, accessor.count);
        if (!cgltf_accessor_read_uint(&values, entry, out.data() + index * components, components))
            return std::string(kDataNotLoaded);
    }
    return {};
}

} // namespace GameEngine

#endif // GE_HAVE_CGLTF
