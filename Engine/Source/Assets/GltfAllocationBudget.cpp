#include "GltfAllocationBudget.h"

#if defined(GE_HAVE_CGLTF)

#include <cgltf.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

namespace GameEngine
{

namespace
{

// The bytes a file may derive per byte of its JSON and buffers. The loaders' largest expansion of one
// unshared element is a 128-byte AnimKeyframe from a 4-byte quantized rotation key (32 times), a
// 40-byte morph target entry from a 4-byte quantized delta (10 times) or a 48-byte Vertex from a 4-byte
// quantized POSITION (12 times); float attributes expand about 1 to 1.5 times. 64 admits every
// unshared file, and one float vertex accessor shared by about 40 material primitives. A file that
// shares one accessor among more primitives is refused, though each copy would fit memory: the loader
// sizes a copy per reader instead of decoding each accessor once. How many sharing primitives fit
// depends on the attributes the accessor's file bytes hold: about 40 when POSITION, NORMAL and
// TEXCOORD_0 are float (32 bytes per vertex against 52 charged), about 16 for a float POSITION alone
// (12 bytes).
constexpr size_t kDerivedBytesPerFileByte = 64;

// The bytes any file may derive whatever its size: an unshared mesh of about five million 48-byte
// vertices, or a clip of about two million 128-byte keys. A file of a few kilobytes that shares or
// zero-fills its accessors costs at most this much.
constexpr size_t kDerivedBytesAllowance = size_t{256} * 1024 * 1024;

constexpr size_t kLargestSize = std::numeric_limits<size_t>::max();

// `a + b`, or the largest size_t when the sum does not fit one.
size_t SaturatingAdd(size_t a, size_t b)
{
    return b > kLargestSize - a ? kLargestSize : a + b;
}

// `a * b`, or the largest size_t when the product does not fit one.
size_t SaturatingMultiply(size_t a, size_t b)
{
    return b != 0 && a > kLargestSize / b ? kLargestSize : a * b;
}

// `count` and `noun`, plural when `count` is not 1: "1 primitive", "16 primitives".
std::string Counted(size_t count, const char* noun)
{
    return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

// True when one of `attributes` reads `accessor`.
bool AttributesRead(const cgltf_attribute* attributes, size_t attributeCount, const cgltf_accessor& accessor)
{
    for (size_t i = 0; i < attributeCount; ++i)
    {
        if (attributes[i].data == &accessor)
            return true;
    }
    return false;
}

// The sentence that names what in `document` reads `accessor`, each of which sizes its own copy of it:
// primitives (an attribute or the indices), morph targets, and animation channels (the key times).
// Empty unless two or more read it.
std::string ReadersSentence(const cgltf_data& document, const cgltf_accessor& accessor)
{
    size_t primitives = 0;
    size_t morphTargets = 0;
    for (cgltf_size meshIndex = 0; meshIndex < document.meshes_count; ++meshIndex)
    {
        const cgltf_mesh& mesh = document.meshes[meshIndex];
        for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count; ++primitiveIndex)
        {
            const cgltf_primitive& primitive = mesh.primitives[primitiveIndex];
            if (primitive.indices == &accessor ||
                AttributesRead(primitive.attributes, primitive.attributes_count, accessor))
                ++primitives;
            for (cgltf_size targetIndex = 0; targetIndex < primitive.targets_count; ++targetIndex)
            {
                const cgltf_morph_target& target = primitive.targets[targetIndex];
                if (AttributesRead(target.attributes, target.attributes_count, accessor))
                    ++morphTargets;
            }
        }
    }
    size_t channels = 0;
    for (cgltf_size animationIndex = 0; animationIndex < document.animations_count; ++animationIndex)
    {
        const cgltf_animation& animation = document.animations[animationIndex];
        for (cgltf_size channelIndex = 0; channelIndex < animation.channels_count; ++channelIndex)
        {
            const cgltf_animation_sampler* sampler = animation.channels[channelIndex].sampler;
            if (sampler && sampler->input == &accessor)
                ++channels;
        }
    }

    if (primitives + morphTargets + channels < 2)
        return {};
    Vector<std::string> readers;
    if (primitives > 0)
        readers.push_back(Counted(primitives, "primitive"));
    if (morphTargets > 0)
        readers.push_back(Counted(morphTargets, "morph target"));
    if (channels > 0)
        readers.push_back(Counted(channels, "animation channel"));
    std::string sentence;
    for (size_t i = 0; i < readers.size(); ++i)
        sentence += (i == 0 ? "" : i + 1 == readers.size() ? " and " : ", ") + readers[i];
    return sentence + " read this accessor, and each one sizes its own copy of it. ";
}

// True when `uri` is a data URI, whose bytes the document's JSON already holds (base64, 4/3 of the
// decoded size).
bool IsDataUri(const char* uri)
{
    return std::strncmp(uri, "data:", 5) == 0;
}

// The file a buffer's `uri` names, in a form two names of one file share. The path is built as
// cgltf_load_buffers builds it: the text of `gltfPath` up to its last '/' or '\', then the
// percent-decoded URI (the URI alone when `gltfPath` has no separator), so a URI that starts with '/'
// names a file inside the glTF file's folder. ".", "..", repeated separators and symbolic links are then
// resolved where the file system has them. Two hard links to one file stay two files.
std::filesystem::path BufferFile(const char* uri, const std::filesystem::path& gltfPath)
{
    std::string decoded(uri);
    decoded.resize(cgltf_decode_uri(decoded.data()));
    const std::string gltfFile = gltfPath.string();
    const size_t lastSeparator = gltfFile.find_last_of("/\\");
    const std::filesystem::path named(
        lastSeparator == std::string::npos ? decoded : gltfFile.substr(0, lastSeparator + 1) + decoded);
    std::error_code error;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(named, error);
    return error ? named.lexically_normal() : resolved;
}

// The bytes of the document's JSON and of the buffers that hold data, each file once:
// - a buffer without a URI outside a .glb holds none (cgltf_load_buffers skips it and cgltf_validate
//   does not check its byteLength), so its declared byteLength does not count;
// - a data URI's bytes are counted in the JSON, which holds them base64-encoded;
// - buffers that name one file, by any spelling BufferFile resolves, count its largest byteLength once.
size_t FileBytes(const cgltf_data& document, const std::filesystem::path& gltfPath)
{
    size_t bytes = document.json_size;
    Vector<std::pair<std::filesystem::path, size_t>> files;
    for (cgltf_size i = 0; i < document.buffers_count; ++i)
    {
        const cgltf_buffer& buffer = document.buffers[i];
        if (!buffer.data)
            continue;
        if (!buffer.uri)
            bytes = SaturatingAdd(bytes, buffer.size);
        else if (!IsDataUri(buffer.uri))
            files.emplace_back(BufferFile(buffer.uri, gltfPath), buffer.size);
    }
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i < files.size(); ++i)
    {
        const bool lastOfItsFile = i + 1 == files.size() || files[i + 1].first != files[i].first;
        if (lastOfItsFile)
            bytes = SaturatingAdd(bytes, files[i].second);
    }
    return bytes;
}

} // namespace

GltfAllocationBudget::GltfAllocationBudget(const cgltf_data& document, const std::filesystem::path& gltfPath)
    : m_Document(document)
    , m_FileBytes(FileBytes(document, gltfPath))
{
    const bool multipleFits = m_FileBytes <= kLargestSize / kDerivedBytesPerFileByte;
    m_Budget = SaturatingAdd(multipleFits ? m_FileBytes * kDerivedBytesPerFileByte : kLargestSize,
                             kDerivedBytesAllowance);
}

std::string GltfAllocationBudget::Charge(const cgltf_accessor& accessor, size_t elementCount, size_t bytesPerElement)
{
    // m_Charged never passes m_Budget, and elementCount * bytesPerElement is charged only once it is
    // known to fit the bytes left, so no charge wraps; the total a refusal states saturates. Zero bytes
    // per element charge nothing.
    const size_t bytesLeft = m_Budget - m_Charged;
    if (bytesPerElement == 0 || elementCount <= bytesLeft / bytesPerElement)
    {
        m_Charged += elementCount * bytesPerElement;
        return {};
    }
    const size_t chargedWithThisCopy = SaturatingAdd(m_Charged, SaturatingMultiply(elementCount, bytesPerElement));
    // The fix follows the cause. Only an accessor without a buffer view declares more elements than the
    // whole budget holds. Otherwise earlier charges used the bytes left: copies of a shared accessor, this
    // one's or another's, or accessors without a buffer view, whose elements the file holds no data for.
    // Two or more readers of this accessor name its own copies; with fewer, the reason states the bytes
    // already charged and names both causes.
    const std::string readers = ReadersSentence(m_Document, accessor);
    const bool pastTheWholeBudget = elementCount > m_Budget / bytesPerElement;
    std::string earlierCharges;
    const char* fix = "To load the file, re-export it so that each primitive, morph target and animation channel "
                      "reads its own accessor, or merge the primitives that share an accessor. Engine issue #2469 "
                      "removes the copies.";
    if (pastTheWholeBudget)
    {
        fix = "To load the file, re-export it so that this accessor declares no more elements than the file holds "
              "data for.";
    }
    else if (readers.empty())
    {
        earlierCharges =
            "Other arrays of the file already charge " + std::to_string(m_Charged) + " bytes of the budget. ";
        fix = "To load the file, re-export it so that each primitive, morph target and animation channel reads its "
              "own accessor, and each accessor declares no more elements than the file holds data for. Engine "
              "issue #2469 removes the copies of shared accessors.";
    }
    return "(accessor " + std::to_string(&accessor - m_Document.accessors) + ") whose " +
           std::to_string(elementCount) + " elements are sized into " + std::to_string(bytesPerElement) +
           " bytes each, more than the " + std::to_string(bytesLeft) + " bytes left of the file's budget of " +
           std::to_string(m_Budget) + " (" + std::to_string(kDerivedBytesPerFileByte) + " times its " +
           std::to_string(m_FileBytes) + " bytes of JSON and buffers, plus " +
           std::to_string(kDerivedBytesAllowance) + "). " + readers + earlierCharges +
           "With this copy, the file is charged " + std::to_string(chargedWithThisCopy) +
           " bytes. No import setting changes this budget. " + fix;
}

} // namespace GameEngine

#endif // GE_HAVE_CGLTF
