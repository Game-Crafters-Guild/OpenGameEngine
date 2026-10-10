#pragma once

// ShaderMeta as the shader package stores it.
//
// The meta is reflection output: engine-written, engine-read, and never edited
// by hand, so it is stored as a compact binary blob inside the package rather
// than as text. It is read on every compile-cache hit.
//
// The encoding is positional and unversioned on purpose. The package header
// carries the version, and a package whose version does not match is not read
// at all -- so a field added here is a package-version bump, and old entries
// are orphaned rather than migrated, exactly as the compile cache already
// treats every other format change.
//
// Decoding is bounds-checked throughout and must stay that way: a package is a
// file on disk, and a corrupt or truncated one has to fail rather than read off
// the end or allocate on a bogus length.
//
// The payload carries a magic and a checksum because a binary encoding has no
// syntax to violate: almost any byte sequence decodes into a DIFFERENT valid
// meta, and a silently wrong descriptor offset would go on to build a pipeline.
// The magic refuses a chunk that is not this encoding; the checksum refuses a
// damaged one.

#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine { namespace Rendering {

// The same meta always encodes to the same bytes. False, with the reason, for
// a meta the decoder would refuse (types nested past the cap), so nothing is
// ever written that cannot be read back.
bool EncodeShaderMetaBinary(const ShaderMeta& meta, std::vector<uint8_t>& out,
                            std::string* outError = nullptr);

// False on truncated, corrupt or too-deeply-nested input, on bytes after the
// last field, and on an enum value outside its type, leaving `out` unspecified.
// Derived fields (binding name ids, set layout hashes) are recomputed rather
// than stored, so they cannot disagree with the data.
bool DecodeShaderMetaBinary(const uint8_t* data, size_t size, ShaderMeta& out,
                            std::string* outError);

}} // namespace GameEngine::Rendering
