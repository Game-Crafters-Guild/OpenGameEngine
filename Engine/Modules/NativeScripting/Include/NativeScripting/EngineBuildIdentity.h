#pragma once

// EngineBuildIdentity — which Engine BUILD is running, as a short hex string.
//
// The engine-ABI digest (BuildCacheRecord.h) answers "what inputs was this module
// compiled with", hashed from the import lib's path:size:mtime at BUILD time. That
// is unavailable to a runtime that never builds anything: a Player has no
// NativeBuildConfig, no import lib, and no toolchain. This answers the runtime
// question instead — "which Engine.dll am I, right now" — so a prebuilt user
// module can be checked against the engine about to map it, BEFORE LoadLibrary
// runs its static initializers (which is far too late: a mismatched module heap-
// faults inside DLL_PROCESS_ATTACH, before any exported handshake is reachable).
//
// The value is derived from the linker's per-link stamp on the running engine
// image, so it changes whenever Engine.dll is relinked and is identical for every
// process that loaded the same binary — including a copy of it at another path,
// which is what makes it usable as a cache key across worktrees.

#include <string>

namespace GameEngine
{
namespace NativeScripting
{

// Identity of the engine image this code is linked into (NativeScripting compiles
// into Engine.dll), computed once on first call.
//
// EMPTY means "this platform/build cannot identify itself" — callers must treat
// that as "cannot verify", never as a mismatch, or they would refuse every module
// on a platform that simply has no stamp. It is never empty on Windows: the PE
// optional header always carries the fallback inputs.
const std::string& EngineBuildIdentity();

} // namespace NativeScripting
} // namespace GameEngine
