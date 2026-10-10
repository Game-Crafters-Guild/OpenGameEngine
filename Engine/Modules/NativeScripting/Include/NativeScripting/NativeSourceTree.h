#pragma once

#include <cstdint>
#include <filesystem>

namespace GameEngine
{
namespace NativeScripting
{

// The one walk over a native module's source root.
//
// Every reader of that tree skips the same directories — the engine's
// AssetIgnoreRules for the root (build output, IDE and tool trees, plus whatever
// the root's .assetignore adds), and UserProjectGenerator emits the same list
// into the generated project's source glob. They have to agree: a package whose
// sources sit at its root builds into a .Cache inside that root, so a walk that
// hashed or compiled what the build writes there would leave the module stale
// against its own output on every launch.

// True for the source extensions the native-script watcher reacts to: the
// translation units the generated project compiles (.cpp/.cc/.cxx/.c) plus every
// header the ComponentScanner reflects from (.h/.hpp/.hxx/.hh). Case-insensitive.
bool IsWatchedNativeSourceExtension(const std::filesystem::path& path);

// True when at least one watched native source lives under `sourceRoot` — the
// gate that lets a package-only project skip the empty user-module build. The
// build pipeline's Player template sources (Assets/Source/{main,PlayerApplication}.cpp,
// copied in by every packaged build and compiled into the Player EXE, never the
// user module) are not user code and do not open that gate.
bool HasWatchedNativeSource(const std::filesystem::path& sourceRoot);

// Continues `seed` over the sorted {root-relative path}:{mtime}:{size} of every
// watched native source under `sourceRoot`, so the result changes on any source
// edit, add or remove and on nothing else. `packageRoot` is the module's package
// (empty for a project's own scripts): its top-level Tests folder is skipped, as
// the generated project skips it.
std::uint64_t HashWatchedNativeSources(const std::filesystem::path& sourceRoot,
                                       const std::filesystem::path& packageRoot, std::uint64_t seed);

} // namespace NativeScripting
} // namespace GameEngine
