#pragma once

// Package Manager panel actions: the manifest/lock mutations behind Add /
// Remove / Re-pin / Relocate / Check-updates, kept engine-side so they are
// unit-testable against real temp manifests without the editor. Every action
// edits <project>/Packages/manifest.json (and packages-lock.json where git
// pins are involved) — mounts follow on the next project open (the P3 drift
// model); callers re-resolve for display and surface the reopen notice.

#include <filesystem>
#include <string>

namespace GameEngine
{

// What an Add-flow free-text input is: a git spec ("git+https://...#ref" or
// bare http(s) URL) or a filesystem path (everything else).
bool IsGitAddInput(const std::string& input);

// The dependency the Add flow wrote: name (learned from package.json) and the
// exact manifest spec text.
struct PackageAddResult
{
    std::string Name;
    std::string Spec;
};

// Add a local package from a path typed or browsed by the user. Relative
// paths resolve against <project>/Packages (the same anchoring "file:" specs
// use); an optional leading "file:" is accepted and stripped. The directory
// must contain a parseable package.json — its name becomes the dependency
// key. A path that IS <project>/Packages/<name> is written as "embedded",
// anything else as "file:<path>" (path kept relative when typed relative).
bool AddLocalPackageToProject(const std::filesystem::path& projectRoot,
                              const std::string& inputPath,
                              PackageAddResult& out,
                              std::string& outError);

// Add a git package: parse the spec, acquire it into the global cache NOW
// (errors — bad URL, missing ref, no package.json — surface inline instead of
// at next resolve), learn the package name from the fetched package.json,
// append the manifest dependency, and write the lock pin so the follow-up
// resolve takes the offline fast-path.
bool AddGitPackageToProject(const std::filesystem::path& projectRoot,
                            const std::string& spec,
                            PackageAddResult& out,
                            std::string& outError);

// Remove a declared dependency: drops the manifest entry, any disabled[]
// mention, and the lock pin. Fails when the name is not declared.
bool RemovePackageFromProject(const std::filesystem::path& projectRoot,
                              const std::string& packageName,
                              std::string& outError);

// Re-pin a git dependency to `newRef` (tag / branch / commit sha), keeping
// the URL and &path= subdir. ALWAYS drops the lock pin, so the next resolve
// acquires exactly the requested ref — passing the current ref is the
// "update to latest on ref" action (an unchanged spec would otherwise honor
// the stale pin forever, by design).
bool RepinGitPackageToRef(const std::filesystem::path& projectRoot,
                          const std::string& packageName,
                          const std::string& newRef,
                          std::string& outError);

// Point a "file:" dependency at a new directory (the MISSING-package Locate
// action). The directory must contain a package.json whose name matches the
// dependency — a mismatched folder is refused, not written. The path is
// stored relative to <project>/Packages when the new location is inside the
// project, absolute otherwise.
bool RelocatePackageInProject(const std::filesystem::path& projectRoot,
                              const std::string& packageName,
                              const std::filesystem::path& newDir,
                              std::string& outError);

// Read-only "check for updates" for a git dependency: resolve the ref's
// current remote commit (git ls-remote) and compare against the locked pin.
// Writes nothing; outStatus is a human sentence for the panel status line.
bool CheckGitPackageForUpdates(const std::filesystem::path& projectRoot,
                               const std::string& packageName,
                               std::string& outStatus,
                               std::string& outError);

} // namespace GameEngine
