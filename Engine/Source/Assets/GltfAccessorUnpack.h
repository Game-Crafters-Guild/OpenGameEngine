#pragma once

// Reads a glTF accessor's elements as floats or unsigned integers with its sparse block applied here:
// cgltf's per-element readers read nothing from a sparse accessor, and cgltf_accessor_unpack_floats
// reads sparse values with the byte stride of the accessor's own buffer view (glTF packs them tightly)
// and writes each one at its sparse index without a bound.

#include "Types/Types.h"

#include <string>

struct cgltf_accessor;

namespace GameEngine
{

/// Scratch that the unpack functions reuse across calls for a sparse block's indices, and
/// UnpackAccessorFloats for its values.
struct GltfSparseScratch
{
    Vector<uint32> Indices;
    Vector<float> Values;
};

/**
 * @brief Reads every element of `accessor` into `out`, one float per component.
 *
 * The dense part is read with the accessor's byte stride, or is zero when the accessor has no buffer
 * view. A sparse block's indices and values are read tightly packed from their buffer views and each
 * value replaces the element at its index. Every read stays within the extents cgltf_validate checked
 * against the loaded buffers. `out` holds accessor.count elements: a buffer view bounds that count by
 * the file's bytes, and OpenGltfDocument bounds the count of an accessor without one.
 *
 * @return Empty when every element was read; otherwise the reason, worded to follow "an accessor" in
 *         a refusal: a buffer it reads holds no data, or a sparse index is at or past its count.
 */
std::string UnpackAccessorFloats(const cgltf_accessor& accessor, Vector<float>& out, GltfSparseScratch& scratch);

/**
 * @brief Reads every element of `accessor` into `out`, one unsigned integer per component, the way
 *        UnpackAccessorFloats reads floats.
 *
 * `accessor` is a scalar or vector accessor. Components convert as cgltf_accessor_read_uint converts
 * them: an unsigned component keeps its value, a signed one is sign-extended to 32 bits, a float one
 * reads 0.
 *
 * @return As UnpackAccessorFloats.
 */
std::string UnpackAccessorIntegers(const cgltf_accessor& accessor, Vector<uint32>& out, GltfSparseScratch& scratch);

} // namespace GameEngine
