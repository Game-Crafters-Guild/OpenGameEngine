#include <gtest/gtest.h>

#include "AssetCore/PathNormalization.h"

#include <filesystem>
#include <string>

using namespace GameEngine::AssetPaths;

TEST(PathNormalization, EmptyInputReturnsEmpty)
{
    EXPECT_EQ(NormalizeForRegistryKey(std::string_view()), "");
    EXPECT_EQ(NormalizeForRegistryKey(std::filesystem::path()), "");
}

TEST(PathNormalization, AsciiCaseFolded)
{
    EXPECT_EQ(NormalizeForRegistryKey("Foo/Bar.PNG"), "foo/bar.png");
    EXPECT_EQ(NormalizeForRegistryKey("ASSETS/Models/Knight.fbx"), "assets/models/knight.fbx");
}

TEST(PathNormalization, BackslashesBecomeForwardSlashes)
{
    EXPECT_EQ(NormalizeForRegistryKey("Foo\\Bar\\Baz.png"), "foo/bar/baz.png");
}

TEST(PathNormalization, MixedSlashesNormalized)
{
    EXPECT_EQ(NormalizeForRegistryKey("Foo/Bar\\Baz.png"), "foo/bar/baz.png");
}

TEST(PathNormalization, RedundantSeparatorsCollapsed)
{
    EXPECT_EQ(NormalizeForRegistryKey("foo//bar///baz.png"), "foo/bar/baz.png");
}

TEST(PathNormalization, DotSegmentsCollapsed)
{
    EXPECT_EQ(NormalizeForRegistryKey("foo/./bar.png"), "foo/bar.png");
    EXPECT_EQ(NormalizeForRegistryKey("foo/bar/../bar.png"), "foo/bar.png");
}

TEST(PathNormalization, IsIdempotent)
{
    const std::string raw = "Foo\\Bar/BAZ.PNG";
    const std::string once = NormalizeForRegistryKey(raw);
    const std::string twice = NormalizeForRegistryKey(once);
    EXPECT_EQ(once, twice);
}

// FoldStorePathKey is the asset store's row-identity key. It skips the
// fs::path round-trip NormalizeForRegistryKey does, so the two can only stay
// interchangeable if they are pinned against each other over the shapes store
// paths actually take: CanonicalizeStorePath output — relative, forward
// slashes, lexically normal. Drift here forks asset identity.

TEST(PathNormalization, FoldStorePathKeyMatchesRegistryKeyOverStorePathShapes)
{
    const char* kStorePaths[] = {
        "Models/Planes/AircoDH2/scene.gltf",
        "models/planes/aircodh2/scene.gltf",
        "MODELS/PLANES/AIRCODH2/SCENE.GLTF",
        "Scenes/Lanscape.scene",
        "RenderPipelines/ForwardPlus_DebugOverlay.rendergraph",
        "a.png",
        "Models/Planes/AircoDH2/Materials/DefaultWhite_interior.png.material",
        "Übung/Straße/Café.png",       // non-ASCII: casefold + NFC path
        "Модели/Щит.png", // Cyrillic
    };
    for (const char* p : kStorePaths)
    {
        EXPECT_EQ(FoldStorePathKey(p), NormalizeForRegistryKey(p)) << "input: " << p;
    }
    EXPECT_EQ(FoldStorePathKey(std::string_view()), "");
}

TEST(PathNormalization, FoldStorePathKeyIsIdempotentAndCaseBlind)
{
    const std::string once = FoldStorePathKey("Foo\\Bar/BAZ.PNG");
    EXPECT_EQ(once, "foo/bar/baz.png");
    EXPECT_EQ(FoldStorePathKey(once), once);
    EXPECT_EQ(FoldStorePathKey("foo/bar/baz.png"), FoldStorePathKey("FOO/BAR/BAZ.PNG"));
}

// Cross-platform Unicode tests. Two contributors on different OSes (or
// different shell encodings) must produce the same canonical form for the
// same logical path so derived GUIDs match.

TEST(PathNormalization, NfdAndNfcNormalizeToSameForm)
{
    // U+00E9 (LATIN SMALL LETTER E WITH ACUTE, NFC, 2 bytes UTF-8 0xC3 0xA9)
    const std::string nfc = "caf\xC3\xA9/foo.png";

    // Same character in NFD: U+0065 + U+0301 (e + combining acute, 3 bytes)
    const std::string nfd = "cafe\xCC\x81/foo.png";

    EXPECT_EQ(NormalizeForRegistryKey(nfc), NormalizeForRegistryKey(nfd))
        << "NFD and NFC must produce identical canonical paths so a project "
           "authored on macOS-NFD resolves identically on Windows-NFC.";
}

TEST(PathNormalization, MixedCaseNfdYieldsExactNfcCasefoldedBytes)
{
    // Uppercase NFD input: "CAFE" + U+0301 (combining acute) + "/Foo.PNG".
    // Case fold maps E to e during decomposition, then composition produces
    // U+00E9 (UTF-8 0xC3 0xA9). Pins the exact canonical output bytes — an
    // equality-of-two-inputs test alone cannot catch a transform that
    // degrades both sides the same way.
    EXPECT_EQ(NormalizeForRegistryKey("CAFE\xCC\x81/Foo.PNG"), "caf\xC3\xA9/foo.png");
}

TEST(PathNormalization, CyrillicCaseFolded)
{
    // U+0410 (CYRILLIC CAPITAL LETTER A) folded to U+0430 (CYRILLIC SMALL LETTER A)
    // UTF-8: 0xD0 0x90 -> 0xD0 0xB0
    const std::string upper = "\xD0\x90.fbx";
    const std::string lower = "\xD0\xB0.fbx";
    EXPECT_EQ(NormalizeForRegistryKey(upper), NormalizeForRegistryKey(lower));
}

TEST(PathNormalization, GermanSharpSCaseFoldsToDoubleS)
{
    // U+00DF (LATIN SMALL LETTER SHARP S) case-folds to "ss" under simple Unicode case folding.
    // This ensures user-authored asset paths with German characters resolve identically to
    // the lowercase ASCII equivalent that other contributors might type.
    // Split the literal so 'e' isn't absorbed into the \x9F hex escape.
    EXPECT_EQ(NormalizeForRegistryKey("Stra\xC3\x9F" "e.fbx"), NormalizeForRegistryKey("strasse.fbx"));
}

TEST(PathNormalization, NonAsciiNotMangled)
{
    // Character should be preserved through the pipeline (just case-folded), not stripped.
    const std::string path = "\xD0\xBF\xD0\xB0\xD0\xBF\xD0\xBA\xD0\xB0/foo.png"; // "папка/foo.png"
    const std::string canonical = NormalizeForRegistryKey(path);
    EXPECT_FALSE(canonical.empty());
    EXPECT_NE(canonical.find("foo.png"), std::string::npos);
}

TEST(PathNormalization, FilesystemPathOverloadAgreesWithStringOverload)
{
    const std::string raw = "Foo/Bar.png";
    EXPECT_EQ(NormalizeForRegistryKey(raw),
              NormalizeForRegistryKey(std::filesystem::path(raw)));
}

TEST(PathNormalization, NonAsciiPathSurvivesFilesystemRoundTrip)
{
    // Regression guard: on Windows, constructing std::filesystem::path from a
    // narrow byte string interprets the bytes via the active code page rather
    // than UTF-8 — which would mojibake non-ASCII paths and break the
    // cross-platform GUID-determinism contract. The string overload must build
    // the path from char8_t* explicitly so the same UTF-8 bytes always yield
    // the same canonical key regardless of host ACP.
    //
    // U+00E9 LATIN SMALL LETTER E WITH ACUTE (UTF-8: 0xC3 0xA9)
    const std::string utf8 = "caf\xC3\xA9/icon.png";

    // Build path from explicit UTF-8 (C++20 char8_t overload) — guaranteed UTF-8.
    const auto* u8data = reinterpret_cast<const char8_t*>(utf8.data());
    const std::filesystem::path utf8Path(u8data, u8data + utf8.size());

    // String overload (which we just hardened) and explicit-UTF-8 path overload
    // must agree byte-for-byte.
    EXPECT_EQ(NormalizeForRegistryKey(utf8),
              NormalizeForRegistryKey(utf8Path));

    // And the result must still contain a valid UTF-8 representation of "café"
    // (NFC composed; the case fold leaves lowercase letters unchanged).
    const std::string canonical = NormalizeForRegistryKey(utf8);
    EXPECT_NE(canonical.find("\xC3\xA9"), std::string::npos)
        << "Non-ASCII byte sequence was dropped or mojibake'd during filesystem round-trip.";
}

// ----------------------------------------------------------------------------
// StorePathParentDir — the dir-gate key for the warm reconcile pass.
//
// The reconcile pass extracts each record's parent directory to probe the
// source snapshot's per-directory mtime rows. Those rows are keyed by
// std::filesystem::path::parent_path().generic_string() (AssetRegistry's
// snapshot writer), so the string-level extraction has to agree with it
// exactly over the canonical store-path form or the gate silently stops
// hitting. These tests are that agreement, checked against the real
// parent_path() rather than a hand-written expectation.
// ----------------------------------------------------------------------------
TEST(PathNormalization, StorePathParentDirMatchesParentPathOverCanonicalForm)
{
    // Inputs are written the way callers hand them in; each is put through
    // CanonicalizeStorePath first, exactly as the store does on upsert and
    // on load, so the comparison runs over the canonical domain the
    // extraction documents.
    const char* rawInputs[] = {
        "asset.png",
        "dir/asset.png",
        "a/b/c/asset.png",
        "Models\\Knight\\body.fbx",
        "./Textures/albedo.png",
        "dir.v2/asset.png",
        "dir/LICENSE",
        "Assets/Materials/stone.material",
        "caf\xC3\xA9/icon.png",
        "a/b/",
    };

    for (const char* raw : rawInputs)
    {
        const std::string canonical = CanonicalizeStorePath(raw);
        const std::string expected =
            std::filesystem::path(canonical).parent_path().generic_string();
        EXPECT_EQ(std::string(StorePathParentDir(canonical)), expected)
            << "raw input: " << raw << " canonical: " << canonical;
    }
}

TEST(PathNormalization, StorePathParentDirHasNoParentWithoutASeparator)
{
    // No separator means no parent directory. The reconcile gate treats an
    // empty key as "no snapshot row", which falls through to the existence
    // check — the conservative direction.
    EXPECT_TRUE(StorePathParentDir("asset.png").empty());
    EXPECT_TRUE(StorePathParentDir("").empty());
}

TEST(PathNormalization, StorePathParentDirViewsTheInput)
{
    // The result is a view into the caller's string, which is the point:
    // the per-record probe allocates nothing.
    const std::string canonical = "a/b/asset.png";
    const std::string_view parent = StorePathParentDir(canonical);
    EXPECT_EQ(parent, "a/b");
    EXPECT_EQ(parent.data(), canonical.data());
}

// ----------------------------------------------------------------------------
// NormalizeMountRoot — the path-domain counterpart to NormalizeForRegistryKey.
//
// A mount root is opened, so it must keep the spelling that exists on disk.
// These tests pin the two functions apart: the key folds, the root does not.
// ----------------------------------------------------------------------------
TEST(PathNormalization, MountRootKeepsOnDiskCase)
{
    const std::filesystem::path root = std::filesystem::current_path() / "Dev" / "MyProject" / "Assets";
    EXPECT_EQ(NormalizeMountRoot(root), root.lexically_normal());
}

TEST(PathNormalization, MountRootKeepsCharactersWhoseFoldChangesLength)
{
    // U+00DF folds to "ss", so the key is a different string of a different
    // length. Opening that key names a directory that does not exist — the
    // root has to come through byte-identical.
    const std::string utf8 = "Stra\xC3\x9F" "e1307";
    const auto* u8data = reinterpret_cast<const char8_t*>(utf8.data());
    const std::filesystem::path leaf(u8data, u8data + utf8.size());
    const std::filesystem::path root = std::filesystem::current_path() / leaf / "Assets";

    const std::filesystem::path normalized = NormalizeMountRoot(root);
    EXPECT_EQ(normalized, root.lexically_normal());
    EXPECT_EQ(normalized.parent_path().filename(), leaf);

    // And the key for the same root really does differ in length, so the two
    // functions are not interchangeable.
    const std::string key = NormalizeForRegistryKey(root);
    EXPECT_NE(key.find("strasse1307"), std::string::npos);
    EXPECT_EQ(key.find("stra\xC3\x9F" "e1307"), std::string::npos);
}

TEST(PathNormalization, MountRootTrimsTrailingSeparatorAndDotSegments)
{
    const std::filesystem::path base = std::filesystem::current_path() / "Proj";
    EXPECT_EQ(NormalizeMountRoot(base / "Assets" / ""), (base / "Assets").lexically_normal());
    EXPECT_EQ(NormalizeMountRoot(base / "Assets" / "." ), (base / "Assets").lexically_normal());
    EXPECT_EQ(NormalizeMountRoot(base / "Assets" / "Sub" / ".."), (base / "Assets").lexically_normal());
    EXPECT_FALSE(NormalizeMountRoot(base / "Assets" / "").filename().empty());
}

TEST(PathNormalization, MountRootMakesRelativeInputAbsolute)
{
    const std::filesystem::path normalized = NormalizeMountRoot("Assets");
    EXPECT_TRUE(normalized.is_absolute());
    EXPECT_EQ(normalized, (std::filesystem::current_path() / "Assets").lexically_normal());
}

TEST(PathNormalization, MountRootEmptyStaysEmpty)
{
    EXPECT_TRUE(NormalizeMountRoot(std::filesystem::path()).empty());
}
