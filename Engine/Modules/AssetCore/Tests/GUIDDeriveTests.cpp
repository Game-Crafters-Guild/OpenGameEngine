// Tests for GUID::Derive — the deterministic path-hash that is becoming
// persisted asset identity (embedded material/clip GUIDs are baked into
// scenes, and derived-identity sources compute every asset's GUID from its
// canonical path). The byte layout is a PERSISTENCE CONTRACT: the golden
// vectors below freeze it so an accidental future change to the hash, the
// accumulator width, or the byte packing is caught immediately rather than
// silently re-keying every derived asset.

#include "AssetCore/GUID.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using GameEngine::GUID;

// Mirror of the alias-namespace derivation used by the registry
// (Derive(Null, "asset-source-namespace:" + alias)).
GUID SourceNamespace(const std::string& alias)
{
    return GUID::Derive(GUID::Null(), "asset-source-namespace:" + alias);
}

// ---------------------------------------------------------------------------
// (1) Determinism
// ---------------------------------------------------------------------------

TEST(GUIDDerive, IsDeterministicAcrossCalls)
{
    const GUID parent = GUID::Derive(GUID::Null(), "parent");
    EXPECT_EQ(GUID::Derive(parent, "x"), GUID::Derive(parent, "x"));
    EXPECT_EQ(GUID::Derive(GUID::Null(), "asset-source-namespace:project"),
              GUID::Derive(GUID::Null(), "asset-source-namespace:project"));
}

TEST(GUIDDerive, DiffersOnDifferentParent)
{
    const GUID a = GUID::Derive(GUID::Null(), "a");
    const GUID b = GUID::Derive(GUID::Null(), "b");
    ASSERT_NE(a, b);
    EXPECT_NE(GUID::Derive(a, "x"), GUID::Derive(b, "x"));
}

TEST(GUIDDerive, DiffersOnDifferentSubKey)
{
    const GUID parent = GUID::Derive(GUID::Null(), "parent");
    EXPECT_NE(GUID::Derive(parent, "x"), GUID::Derive(parent, "y"));
}

// ---------------------------------------------------------------------------
// (2) Golden stability — frozen byte layout. If any of these fail, the hash
//     or its serialization changed and every persisted derived GUID just
//     moved. That is a deliberate, coordinated re-bake — not an accident.
// ---------------------------------------------------------------------------

TEST(GUIDDerive, GoldenSourceNamespaceVectors)
{
    EXPECT_EQ(SourceNamespace("project").ToString(), "9400c1f3-1ee2-41d0-8aaf-290456ed973d");
    EXPECT_EQ(SourceNamespace("editor").ToString(),  "baaefb61-6066-42f8-8493-c0c9fdb32f60");
    EXPECT_EQ(SourceNamespace("package").ToString(), "5d32b907-4eff-45ed-9270-8a002be22f40");
}

TEST(GUIDDerive, GoldenDerivedKeyVectors)
{
    // A fixed namespace-scoped key, as a derived-identity source would produce
    // for "<alias>/<canonicalRelPath>".
    EXPECT_EQ(GUID::Derive(GUID::Null(), "project/models/foo.glb").ToString(),
              "d5c3fd20-39bb-48b6-819a-64fb1c1dadf0");

    // Two-level derivation (embedded-sub-asset style: child off a derived parent).
    const GUID parentA = GUID::Derive(GUID::Null(), "a");
    EXPECT_EQ(GUID::Derive(parentA, "x").ToString(), "0428572b-8871-4c3f-a5d5-ad6d61b117a5");
}

TEST(GUIDDerive, GoldenRfc4122StyleBits)
{
    // The packing forces version/variant nibbles; pin them so the bit-twiddle
    // can't silently drift.
    const GUID g = GUID::Derive(GUID::Null(), "project/models/foo.glb");
    const auto& d = g.GetData();
    EXPECT_EQ(d[6] & 0xF0, 0x40);          // version 4-style
    EXPECT_EQ(d[8] & 0xC0, 0x80);          // RFC4122 variant
}

// ---------------------------------------------------------------------------
// (3) Namespace pairwise-distinct for the fixed alias set
// ---------------------------------------------------------------------------

TEST(GUIDDerive, SourceNamespacesArePairwiseDistinct)
{
    const GUID project = SourceNamespace("project");
    const GUID editor  = SourceNamespace("editor");
    const GUID package = SourceNamespace("package");

    EXPECT_NE(project, editor);
    EXPECT_NE(project, package);
    EXPECT_NE(editor,  package);

    // None should collapse to Null.
    EXPECT_FALSE(project.IsNull());
    EXPECT_FALSE(editor.IsNull());
    EXPECT_FALSE(package.IsNull());
}

// ---------------------------------------------------------------------------
// (4) Mount/source scoping: the SAME relative path under different source
//     mounts derives DISTINCT GUIDs. The alias is baked into both the
//     namespace and the key, so mounts never collide — project, editor, and a
//     future packages/plugins mount are independent GUID spaces.
// ---------------------------------------------------------------------------

TEST(GUIDDerive, SameRelativePathUnderDifferentMountsDerivesDistinctGuids)
{
    // The registry derives an asset's GUID as
    //   Derive( SourceNamespace(alias), "<alias>/<canonicalRelPath>" ).
    // (It also case-folds + NFC-normalizes the key; the alias scoping that drives
    // distinctness here is unaffected by that normalization.)
    const auto deriveFor = [](const char* alias, const std::string& rel) {
        return GUID::Derive(SourceNamespace(alias), std::string(alias) + "/" + rel);
    };

    const std::string rel = "models/fox/fox.glb";
    const GUID viaProject = deriveFor("project", rel);
    const GUID viaEditor = deriveFor("editor", rel);
    const GUID viaPackage = deriveFor("package", rel);

    // Same relative path, different mount -> different GUID (no cross-mount collision).
    EXPECT_NE(viaProject, viaEditor);
    EXPECT_NE(viaProject, viaPackage);
    EXPECT_NE(viaEditor, viaPackage);
    EXPECT_FALSE(viaProject.IsNull());

    // ...and stable per mount.
    EXPECT_EQ(viaProject, deriveFor("project", rel));
}

} // namespace
