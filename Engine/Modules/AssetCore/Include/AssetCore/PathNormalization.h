#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine::AssetPaths
{

// Normalize a path for use as a registry key.
//
// The returned string is the deterministic canonical form used by the asset
// registry to derive GUIDs and key its lookup maps. Two paths that refer to
// the same logical file on any platform produce the same canonical form.
//
// Steps applied:
//   1. UTF-8 decode (input is assumed to be valid UTF-8)
//   2. NFC normalization (collapses macOS NFD into the same form as Windows/Linux)
//   3. Unicode full case fold via utf8proc — locale-independent, supports 1:N
//      expansions (e.g. German "ß" → "ss", ligature "ﬁ" → "fi"). Matches Unicode
//      TR#21 "Full" case folding rather than "Simple" — chosen so contributors
//      who type the obvious ASCII transliteration see the same canonical key
//      as authors using the original character.
//   4. Backslashes converted to forward slashes
//   5. lexically_normal applied (collapses .. and . segments, redundant slashes)
//
// Two paths differing only in case or normalization form produce the same
// canonical key — i.e. the registry treats paths as case-insensitive at the
// identity level, matching Unity/Unreal/Godot conventions for cross-platform
// collaboration. This is intentional: a project authored on Windows must
// resolve identically on macOS/Linux even when filenames carry non-ASCII or
// pre-composed accent characters.
//
// Inputs are not validated; an empty input returns an empty string.
//
// The string-view and filesystem::path overloads are unambiguous; the
// std::string and const char* overloads exist solely to avoid an ambiguity
// between the two when callers pass those types (both have implicit
// conversions to string_view AND to filesystem::path).
std::string NormalizeForRegistryKey(std::string_view utf8Path);
std::string NormalizeForRegistryKey(const std::filesystem::path& path);
inline std::string NormalizeForRegistryKey(const std::string& s)
{
    return NormalizeForRegistryKey(std::string_view(s));
}
inline std::string NormalizeForRegistryKey(const char* s)
{
    return NormalizeForRegistryKey(std::string_view(s ? s : ""));
}

// Convenience: case-fold + NFC normalize a single UTF-8 segment without
// applying path-separator or lexical_normal logic. Useful for normalizing
// non-path identifiers that share the same Unicode policy.
std::string CaseFoldUtf8(std::string_view utf8);

// Normalize a mount root to the form files are OPENED from: absolute,
// lexically normal (".." and "." collapsed), trailing separators trimmed so
// filename()/parent_path() are stable. The CALLER's spelling is preserved
// byte for byte — no case folding, no Unicode folding. It is not resolved
// against the filesystem, so it is the on-disk spelling only insofar as the
// caller supplied one; on a case-insensitive filesystem any case variant of a
// real directory opens it either way.
//
// This is the counterpart to NormalizeForRegistryKey, not a variant of it.
// A registry key is an identity: folding it is correct, and full case folding
// is 1:N, so "Straße" folds to "strasse" and the string changes length. That
// is fine to compare and hash, and wrong to open — the folded form names a
// directory that does not exist, and creating it produces a phantom root
// beside the real one. Roots, and every path joined onto one to reach a file,
// use this function; keys use NormalizeForRegistryKey.
std::filesystem::path NormalizeMountRoot(const std::filesystem::path& path);

// Canonicalize a project-relative path string for use as a record key in
// asset stores. Pure string normalization — no Unicode case folding, no
// filesystem touching. Steps:
//   1. Backslashes converted to forward slashes
//   2. Leading "./" segments stripped
//   3. Trailing slashes trimmed
//
// This is the form keys take in the on-disk authoritative store and in
// the reconcile path's canonical-path comparisons. It preserves case so the
// on-disk JSONL stays human-readable and platform-native fs operations can
// use the result directly — but that case is spelling, not identity: run the
// result through FoldStorePathKey below to key or compare rows.
std::string CanonicalizeStorePath(std::string s);

// Identity key for a canonical store path — the case-folded form the asset
// store indexes rows by.
//
// A store row keeps the spelling it was written with (human-readable JSONL,
// usable directly by platform-native fs calls); its IDENTITY is this folded
// key, so one file is one row and one GUID whatever case a query, a platform,
// or a historical journal line supplies. That matches the identity policy
// NormalizeForRegistryKey already applies at GUID::Derive callsites: two paths
// differing only in case are one asset on every platform.
//
// Equal to NormalizeForRegistryKey over the same input, minus the fs::path
// round-trip — the domain is a CanonicalizeStorePath'd relative path, which is
// already separator-normalized and lexically normal, so lexically_normal would
// be identity. PathNormalizationTests pins the two against each other over the
// shapes store paths actually take.
std::string FoldStorePathKey(std::string_view canonicalStorePath);

// Canonical relative path of `assetPath` within `assetRoot`, in the same
// CanonicalizeStorePath form store rows record: forward slashes, no leading
// "./", and `assetPath`'s own spelling.
//
// Containment and spelling are decided separately, because they are different
// questions. Containment compares the two paths element by element in the
// identity domain, so it holds however either side is spelled — a mount root
// carries the caller's spelling while a registry-normalized asset path is
// folded on case-insensitive platforms, and fs::path compares case-sensitively,
// so a lexical pairing of the two answers "outside the root" for every asset.
// The tail is then emitted verbatim, because this string is joined back onto a
// root to reopen the file and on a case-sensitive filesystem only the on-disk
// spelling reopens it. Callers that need a spelling-blind key run the result
// through FoldStorePathKey, exactly as the store does with its rows.
//
// Purely lexical: std::filesystem::relative() consults the filesystem and fails
// for paths that do not exist, which is the common case during atomic save
// temp+rename flows.
//
// Containment is folded on every platform, which makes it a superset of what a
// case-sensitive filesystem would answer: on ext4, "/proj/assets/x.png" reports
// as inside a "/proj/Assets" mount even though those are two directories. That
// is accepted rather than made platform-conditional — the registry already
// treats case-differing paths as one asset everywhere (NormalizeForRegistryKey
// at the GUID::Derive callsites, FoldStorePathKey in the store), so a mount
// whose sibling differs only in case is already outside the identity model.
//
// Returns false when `assetPath` is not strictly inside `assetRoot`.
bool TryMakeCanonicalRelativePath(const std::filesystem::path& assetRoot,
                                  const std::filesystem::path& assetPath,
                                  std::string& outCanonical);

// Parent directory of a canonical store path, as a view into the input.
// Store paths carry forward slashes only (CanonicalizeStorePath), so the
// last '/' is the only separator that can end the parent — no
// std::filesystem::path needs to be built to find it. Over that canonical
// form the result equals path::parent_path().generic_string(), which is
// how the source snapshot keys its per-directory rows. A path with no
// separator has no parent and yields an empty view.
std::string_view StorePathParentDir(std::string_view canonicalPath);

// True when a store path violates the canonical-relative contract by being
// rooted. Store paths pass through CanonicalizeStorePath (forward slashes
// only), so two rooted shapes cover every writer platform:
//   "/users/…"   POSIX-absolute (a leading slash also matches UNC "//srv/…")
//   "C:/…"       Windows drive-absolute
// std::filesystem::path::is_absolute() deliberately isn't used here: it
// answers for the host platform, but a leaked record may have been written
// by a different OS (e.g. macOS paths in a Windows project's store).
bool IsAbsoluteStorePath(std::string_view canonicalPath);

} // namespace GameEngine::AssetPaths
