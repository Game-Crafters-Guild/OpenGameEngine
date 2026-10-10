#pragma once

// The bytes of arrays one glTF load may size from a file's accessors. A buffer view bounds an
// accessor's count by the file's bytes and OpenGltfDocument bounds the count of an accessor without
// one, but each primitive, morph target and animation channel that reads an accessor sizes its own
// arrays from it: a file that shares one accessor among N readers costs N copies of it.

#include "Types/Types.h"

#include <filesystem>
#include <string>

struct cgltf_accessor;
struct cgltf_data;

namespace GameEngine
{

/**
 * @brief What a glTF load may size from one file, and what it has charged so far.
 *
 * The budget is kDerivedBytesPerFileByte (64) times the bytes of the file's JSON and of the buffers
 * that hold data, each buffer file once, plus kDerivedBytesAllowance (256 MiB). A loader charges every
 * array it sizes from an accessor before it sizes any, and refuses the file at the first charge that
 * does not fit. Sizes are size_t and saturate, so where size_t is 32 bits (wasm32) no budget passes
 * about 4 GiB. The decoded bytes of a data URI are three quarters of base64 text the budget already
 * counts once in the file's JSON bytes, and the budget is 64 times the file's bytes plus 256 MiB, so a
 * decode cannot approach it.
 */
class GltfAllocationBudget
{
public:
    /// The budget of `document`, loaded from `gltfPath`, beside which its external buffers resolve.
    GltfAllocationBudget(const cgltf_data& document, const std::filesystem::path& gltfPath);

    /**
     * @brief Charges `elementCount` elements of `accessor`, each sized into `bytesPerElement` bytes.
     *        Zero bytes per element charge nothing.
     *
     * @return Empty when the charge fits the bytes left; otherwise the reason, worded to follow a
     *         reader's description such as "primitive 3 of mesh 0 has an accessor for POSITION ". It
     *         names the accessor, the element count, the bytes per element, the bytes left and the
     *         budget; how many primitives, morph targets and animation channels read the accessor; the
     *         bytes charged with this copy; and the fix in the file's data, chosen by the cause. A charge
     *         that alone passes the whole budget asks for fewer elements declared. When two or more
     *         read this accessor, the fix is a reader of its own accessor. Otherwise earlier charges used
     *         the budget, through shared accessors or accessors without a buffer view: the reason states
     *         the bytes other arrays already charged and asks for both fixes. A charge that does not fit
     *         charges nothing.
     */
    std::string Charge(const cgltf_accessor& accessor, size_t elementCount, size_t bytesPerElement);

private:
    const cgltf_data& m_Document;
    size_t m_FileBytes = 0;
    size_t m_Budget = 0;
    size_t m_Charged = 0;
};

} // namespace GameEngine
