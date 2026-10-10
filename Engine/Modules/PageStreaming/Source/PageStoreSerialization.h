#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
class SharedFileReader;
}

namespace GameEngine::PageStreaming
{

/// "GEPG" read as a little-endian uint32.
inline constexpr uint32 kPageStoreMagic = 0x47504547u;
/// magic, version, format, filter, units, faces, samples X, samples Z, level count, quantum
/// offset, quantum step, key.
inline constexpr std::size_t kPageStoreHeaderBytes = 4u + 4u + 4u * 1u + 4u * 3u + 4u * 2u + 8u;
static_assert(kPageStoreHeaderBytes == 40u);
/// samples X, samples Z, pages X, pages Z, first entry.
inline constexpr std::size_t kPageStoreLevelBytes = 4u * 5u;

/// The header, level directory and page index of `layout`, as the store's first DataOffset() bytes.
std::vector<uint8> SerializePageStoreHead(const PageStoreLayout& layout);

/// Reads and validates the head of the store at `baseOffset` of the open `file`, whose store
/// occupies at most `availableBytes` from there. Every count is checked against its bound and
/// every present page against the available bytes before anything is allocated from it. Returns
/// an empty string and fills `out`, else the reason.
std::string ReadPageStoreHead(SharedFileReader& file, uint64 baseOffset, uint64 availableBytes,
                              PageStoreLayout& out);

} // namespace GameEngine::PageStreaming
