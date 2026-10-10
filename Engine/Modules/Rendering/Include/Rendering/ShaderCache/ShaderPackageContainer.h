#pragma once

// The .shaderpkg container: a header, a table of named chunks, then their
// payloads. This is the one reader and the one writer of that layout; the
// engine, the ShaderReflect tool and the web cook (through the tool) all go
// through it. Standard library only, so the standalone tool can link it.
//
// Layout (little-endian):
// - magic(8) = "GESHDRPK"
// - version(u32) = kShaderPackageVersion
// - chunkCount(u32)
// - chunk table [chunkCount] of { name[16] (NUL-padded), type(u32), pad(u32),
//   offset(u64), size(u64) }
// - chunk payloads, each inside the file, none overlapping the table or another
//
// Chunks, identified by name:
// - "meta-bin"     : reflection metadata, binary (ShaderMetaBinary)
// - "<stage>-spv"  : SPIR-V for stage key vs/fs/cs/gs/ms
// - "<stage>-wgsl" : WGSL text for the same stage keys, the WebGPU backend's
//                    browser format (cooked by Tools/ShaderCook)
// - "cache-info"   : compiler diagnostics and include dependencies, JSON
// Readers skip chunk names they do not know.
//
// Any change to the layout or to a chunk's encoding is a version bump. A
// package of another version is refused, never migrated: the compile cache
// rebuilds the entry, and staged packages are rebuilt by the build.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Rendering
{

inline constexpr uint32_t kShaderPackageVersion = 2;

// Longest chunk name the table can hold; the 16-byte field keeps a NUL.
inline constexpr size_t kShaderPackageChunkNameMax = 15;

inline constexpr std::string_view kShaderPackageMetaChunk = "meta-bin";
inline constexpr std::string_view kShaderPackageCacheInfoChunk = "cache-info";
inline constexpr std::string_view kShaderPackageSpirvSuffix = "-spv";
inline constexpr std::string_view kShaderPackageWgslSuffix = "-wgsl";

// What a read made of the bytes. UnsupportedVersion is a well-formed header of
// another format version: the package was written by a different build and is
// rebuilt, not repaired, and which rebuild applies is for the caller that knows
// where the package came from to say. Every other refusal is Malformed.
enum class ShaderPackageReadResult : uint8_t
{
    Read,
    UnsupportedVersion,
    Malformed,
};

// A chunk to write. The bytes are borrowed and must outlive the write call.
struct ShaderPackageChunkSource
{
    std::string Name;
    std::span<const uint8_t> Bytes;
};

// A chunk read from a container. `Bytes` points into the buffer that was read.
struct ShaderPackageChunkView
{
    std::string Name;
    uint32_t Type = 0;
    std::span<const uint8_t> Bytes;
};

// The complete container for `chunks`, in the order given. False, with the
// reason, for a name that is empty, too long, repeated or not one of the
// chunk kinds above.
bool WriteShaderPackageContainer(std::span<const ShaderPackageChunkSource> chunks,
                                 std::vector<uint8_t>& out,
                                 std::string* outError = nullptr);

// Validate the whole container (magic, version, table and payload bounds, no
// overlap) before returning any chunk. Read on success; otherwise the failure
// kind, with the reason in `outError`.
ShaderPackageReadResult ReadShaderPackageContainer(std::span<const uint8_t> bytes,
                                                   std::vector<ShaderPackageChunkView>& out,
                                                   std::string* outError = nullptr);

} // namespace GameEngine::Rendering
