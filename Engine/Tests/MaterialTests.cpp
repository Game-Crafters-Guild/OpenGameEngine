// Tests for the runtime Material class: typed access (As<T>), SetParams<T>,
// StringId-keyed named access, dirty tracking, and CPU cache correctness.

#include <gtest/gtest.h>

#include "Engine/Rendering/Material.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "AssetCore/GUID.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Types/StringId.h"

#include <cstddef>

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

// Test PBR parameter struct matching a typical material UBO layout.
struct TestPBRParams
{
    float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float _padding[2] = {0.0f, 0.0f};
};
static_assert(std::is_standard_layout_v<TestPBRParams>);
static_assert(sizeof(TestPBRParams) == 32);

// Helper: create a Material with TestPBRParams layout.
static Material MakeTestPBRMaterial()
{
    Material mat = Material::TestFactory::Create(
        GUID::Generate(), "TestPBR", sizeof(TestPBRParams));

    // Register property layouts matching TestPBRParams offsets.
    Material::TestFactory::AddPropertyLayout(mat, "baseColor"_sid, 0, 16);  // float[4]
    Material::TestFactory::AddPropertyLayout(mat, "metallic"_sid, 16, 4);   // float
    Material::TestFactory::AddPropertyLayout(mat, "roughness"_sid, 20, 4);  // float

    return mat;
}

// ---- As<T>() typed access ----

TEST(MaterialTest, AsTyped_ReadInitialDefaults)
{
    Material mat = MakeTestPBRMaterial();

    // Cache is zero-initialized, so all values should be 0.
    const auto& pbr = mat.As<TestPBRParams>();
    EXPECT_FLOAT_EQ(pbr.baseColor[0], 0.0f);
    EXPECT_FLOAT_EQ(pbr.metallic, 0.0f);
    EXPECT_FLOAT_EQ(pbr.roughness, 0.0f);
}

TEST(MaterialTest, AsTyped_NonConst_BumpsContentEpoch)
{
    Material mat = MakeTestPBRMaterial();
    const uint64_t before = Material::GetGlobalContentEpoch();

    auto& pbr = mat.As<TestPBRParams>();
    (void)pbr; // non-const access bumps the content epoch (assumes a write)
    EXPECT_GT(Material::GetGlobalContentEpoch(), before);
}

TEST(MaterialTest, AsTyped_Const_DoesNotBumpContentEpoch)
{
    Material mat = MakeTestPBRMaterial();
    const uint64_t before = Material::GetGlobalContentEpoch();

    const Material& cmat = mat;
    const auto& pbr = cmat.As<TestPBRParams>();
    (void)pbr;
    EXPECT_EQ(Material::GetGlobalContentEpoch(), before);
}

TEST(MaterialTest, AsTyped_WriteAndReadBack)
{
    Material mat = MakeTestPBRMaterial();

    auto& pbr = mat.As<TestPBRParams>();
    pbr.baseColor[0] = 0.8f;
    pbr.baseColor[1] = 0.2f;
    pbr.baseColor[2] = 0.1f;
    pbr.baseColor[3] = 1.0f;
    pbr.metallic = 0.0f;
    pbr.roughness = 0.7f;

    // Read back through const access.
    const auto& readback = static_cast<const Material&>(mat).As<TestPBRParams>();
    EXPECT_FLOAT_EQ(readback.baseColor[0], 0.8f);
    EXPECT_FLOAT_EQ(readback.roughness, 0.7f);
}

// ---- SetParams<T>() bulk typed set ----

TEST(MaterialTest, SetParams_CopiesData)
{
    Material mat = MakeTestPBRMaterial();

    TestPBRParams params{};
    params.baseColor[0] = 0.5f;
    params.baseColor[1] = 0.6f;
    params.baseColor[2] = 0.7f;
    params.baseColor[3] = 1.0f;
    params.metallic = 1.0f;
    params.roughness = 0.3f;

    const uint64_t before = Material::GetGlobalContentEpoch();
    mat.SetParams(params);
    EXPECT_GT(Material::GetGlobalContentEpoch(), before);

    const auto& readback = static_cast<const Material&>(mat).As<TestPBRParams>();
    EXPECT_FLOAT_EQ(readback.baseColor[0], 0.5f);
    EXPECT_FLOAT_EQ(readback.metallic, 1.0f);
    EXPECT_FLOAT_EQ(readback.roughness, 0.3f);
}

// ---- Named setters/getters ----

TEST(MaterialTest, SetFloat_WritesToCorrectOffset)
{
    Material mat = MakeTestPBRMaterial();

    mat.SetFloat("roughness"_sid, 0.42f);

    // Read back via named getter.
    EXPECT_NEAR(mat.GetFloat("roughness"_sid), 0.42f, 1e-6f);

    // Read back via typed access to verify it wrote to the correct cache offset.
    const auto& pbr = static_cast<const Material&>(mat).As<TestPBRParams>();
    EXPECT_NEAR(pbr.roughness, 0.42f, 1e-6f);
}

TEST(MaterialTest, SetFloat_MetallicWritesToCorrectOffset)
{
    Material mat = MakeTestPBRMaterial();

    mat.SetFloat("metallic"_sid, 0.75f);

    const auto& pbr = static_cast<const Material&>(mat).As<TestPBRParams>();
    EXPECT_NEAR(pbr.metallic, 0.75f, 1e-6f);
}

TEST(MaterialTest, SetVector_BaseColorWritesToCorrectOffset)
{
    Material mat = MakeTestPBRMaterial();

    float color[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    mat.SetVector("baseColor"_sid, color, 4);

    const auto& pbr = static_cast<const Material&>(mat).As<TestPBRParams>();
    EXPECT_NEAR(pbr.baseColor[0], 0.1f, 1e-6f);
    EXPECT_NEAR(pbr.baseColor[1], 0.2f, 1e-6f);
    EXPECT_NEAR(pbr.baseColor[2], 0.3f, 1e-6f);
    EXPECT_NEAR(pbr.baseColor[3], 1.0f, 1e-6f);
}

TEST(MaterialTest, GetFloat_UnknownProperty_ReturnsFallback)
{
    Material mat = MakeTestPBRMaterial();

    EXPECT_FLOAT_EQ(mat.GetFloat("nonexistent"_sid, 42.0f), 42.0f);
}

TEST(MaterialTest, GetVector_UnknownProperty_ReturnsFalse)
{
    Material mat = MakeTestPBRMaterial();

    float out[4] = {0};
    EXPECT_FALSE(mat.GetVector("nonexistent"_sid, out, 4));
}

// ---- Named and typed access interop ----

TEST(MaterialTest, NamedAndTyped_WritesAreInteroperable)
{
    Material mat = MakeTestPBRMaterial();

    // Write via named setter.
    mat.SetFloat("roughness"_sid, 0.33f);

    // Read via typed access.
    const auto& pbr = static_cast<const Material&>(mat).As<TestPBRParams>();
    EXPECT_NEAR(pbr.roughness, 0.33f, 1e-6f);

    // Write via typed access.
    mat.As<TestPBRParams>().metallic = 0.88f;

    // Read via named getter.
    EXPECT_NEAR(mat.GetFloat("metallic"_sid), 0.88f, 1e-6f);
}

// ---- Texture bindings ----

TEST(MaterialTest, SetTexture_StoresHandle)
{
    Material mat = MakeTestPBRMaterial();

    TextureHandle tex(42, 1);
    mat.SetTexture("albedoMap"_sid, tex);
    EXPECT_EQ(mat.GetTexture("albedoMap"_sid), tex);
}

TEST(MaterialTest, GetTexture_Unknown_ReturnsInvalid)
{
    Material mat = MakeTestPBRMaterial();
    EXPECT_FALSE(mat.GetTexture("nonexistent"_sid).IsValid());
}

TEST(MaterialTest, SetTexture_SameValue_DoesNotBumpContentEpoch)
{
    Material mat = MakeTestPBRMaterial();

    TextureHandle tex(42, 1);
    mat.SetTexture("albedoMap"_sid, tex);

    // Idempotent rebind (per-frame video override re-assertion) must not bust
    // PackMaterialSSBO's idle skip.
    const uint64_t before = Material::GetGlobalContentEpoch();
    mat.SetTexture("albedoMap"_sid, tex);
    EXPECT_EQ(Material::GetGlobalContentEpoch(), before);
}

TEST(MaterialTest, SetTexture_ChangedValue_BumpsContentEpoch)
{
    Material mat = MakeTestPBRMaterial();
    mat.SetTexture("albedoMap"_sid, TextureHandle(42, 1));

    const uint64_t before = Material::GetGlobalContentEpoch();
    mat.SetTexture("albedoMap"_sid, TextureHandle(43, 1));
    EXPECT_GT(Material::GetGlobalContentEpoch(), before);
    EXPECT_EQ(mat.GetTexture("albedoMap"_sid), TextureHandle(43, 1));
}

// ---- Per-material content revision (T6, processor half) ----
//
// The revision is the per-material signal that a content edit happened; the
// compile version is a different counter that only a shader recompile moves.
// A consumer that compared GetVersion() would miss every slider drag, and one
// that compared the global epoch would fire on every other material's edit.

TEST(MaterialTest, ContentRevision_PropertyEditMovesRevisionNotVersion)
{
    Material mat = MakeTestPBRMaterial();
    const uint32_t revisionBefore = mat.GetContentRevision();
    const uint32_t versionBefore = mat.GetVersion();

    mat.SetFloat("roughness"_sid, 0.25f);

    EXPECT_GT(mat.GetContentRevision(), revisionBefore);
    EXPECT_EQ(mat.GetVersion(), versionBefore)
        << "a parameter edit must not look like a shader recompile";
}

TEST(MaterialTest, ContentRevision_SameValueEditLeavesRevision)
{
    Material mat = MakeTestPBRMaterial();
    mat.SetFloat("roughness"_sid, 0.25f);
    mat.SetTexture("albedoMap"_sid, TextureHandle(42, 1));
    mat.SetBindlessTextureIndex("albedoMap"_sid, 7u);
    const uint32_t revisionBefore = mat.GetContentRevision();

    // Idempotent re-application (document replay, per-frame rebinds) is not a
    // content change and must not invalidate anything keyed on the revision.
    mat.SetFloat("roughness"_sid, 0.25f);
    mat.SetTexture("albedoMap"_sid, TextureHandle(42, 1));
    mat.SetBindlessTextureIndex("albedoMap"_sid, 7u);

    EXPECT_EQ(mat.GetContentRevision(), revisionBefore);
}

TEST(MaterialTest, ContentRevision_EveryMutationOwnerMovesIt)
{
    Material mat = MakeTestPBRMaterial();
    uint32_t last = mat.GetContentRevision();
    const auto expectMoved = [&](const char* what)
    {
        EXPECT_GT(mat.GetContentRevision(), last) << what;
        last = mat.GetContentRevision();
    };

    mat.SetTexture("albedoMap"_sid, TextureHandle(42, 1));
    expectMoved("SetTexture");
    mat.SetBindlessTextureIndex("albedoMap"_sid, 7u);
    expectMoved("SetBindlessTextureIndex");
    mat.SetTextureTransform(TextureSlot::kAlbedo, 2.0f, 2.0f, 0.5f, 0.5f);
    expectMoved("SetTextureTransform");
    mat.SetSlotSamplerOverride("albedoMap"_sid, SamplerPreset::PointRepeat);
    expectMoved("SetSlotSamplerOverride");
    const float rgba[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    mat.SetColor("baseColor"_sid, rgba);
    expectMoved("SetColor");
    TestPBRParams params{};
    params.metallic = 0.75f;
    mat.SetParams(params);
    expectMoved("SetParams");
    (void)mat.As<TestPBRParams>();
    expectMoved("As<T>() non-const");
}

TEST(MaterialTest, ContentRevision_IsPerMaterial)
{
    Material a = MakeTestPBRMaterial();
    Material b = MakeTestPBRMaterial();
    const uint32_t bBefore = b.GetContentRevision();
    const uint64_t epochBefore = Material::GetGlobalContentEpoch();

    a.SetFloat("metallic"_sid, 1.0f);

    // The global epoch moves for any material; the revision moves only for
    // the material that was edited.
    EXPECT_GT(Material::GetGlobalContentEpoch(), epochBefore);
    EXPECT_EQ(b.GetContentRevision(), bBefore);
}

TEST(MaterialTest, HasTextureSlot_LadderNamesResolve_UnknownDoesNot)
{
    Material mat = MakeTestPBRMaterial();

    EXPECT_TRUE(mat.HasTextureSlot("albedoMap"_sid));
    EXPECT_TRUE(mat.HasTextureSlot("emissiveMap"_sid));
    EXPECT_FALSE(mat.HasTextureSlot("uBaseColor"_sid));
    EXPECT_FALSE(mat.HasTextureSlot("nonexistent"_sid));
}

// ---- Identity and schema ----

TEST(MaterialTest, Identity_GuidAndName)
{
    GUID guid = GUID::Generate();
    Material mat = Material::TestFactory::Create(guid, "MyMaterial", 32);

    EXPECT_EQ(mat.GetGuid(), guid);
    EXPECT_EQ(mat.GetName(), "MyMaterial");
    EXPECT_EQ(mat.GetLightingModel(), LightingModel::kStandardPBR);
}

// ---- Cache alignment ----

TEST(MaterialTest, CacheSize_AlignedTo16)
{
    // Request 17 bytes; should align up to 32.
    Material mat = Material::TestFactory::Create(GUID::Generate(), "Test", 17);
    EXPECT_EQ(mat.GetCacheSize(), 32u);
}

TEST(MaterialTest, CacheSize_AlreadyAligned)
{
    Material mat = Material::TestFactory::Create(GUID::Generate(), "Test", 64);
    EXPECT_EQ(mat.GetCacheSize(), 64u);
}

// ---- StringId compile-time literals ----

TEST(MaterialTest, StringIdLiterals_AreDeterministic)
{
    constexpr StringId a = "roughness"_sid;
    constexpr StringId b = "roughness"_sid;
    EXPECT_EQ(a, b);
    EXPECT_NE(a, "metallic"_sid);
}

// ---- Bindless texture index accessors ----

TEST(MaterialTest, BindlessTextureIndex_DefaultIsZero)
{
    Material mat = MakeTestPBRMaterial();
    EXPECT_EQ(mat.GetBindlessTextureIndex("albedoMap"_sid), 0u);
    EXPECT_EQ(mat.GetBindlessTextureIndex("normalMap"_sid), 0u);
}

TEST(MaterialTest, BindlessTextureIndex_SetAndGet)
{
    Material mat = MakeTestPBRMaterial();
    mat.SetBindlessTextureIndex("albedoMap"_sid, 42u);
    mat.SetBindlessTextureIndex("normalMap"_sid, 7u);

    EXPECT_EQ(mat.GetBindlessTextureIndex("albedoMap"_sid), 42u);
    EXPECT_EQ(mat.GetBindlessTextureIndex("normalMap"_sid), 7u);
    // Unset slot still returns 0.
    EXPECT_EQ(mat.GetBindlessTextureIndex("emissiveMap"_sid), 0u);
}

TEST(MaterialTest, BindlessTextureIndex_OverwriteExisting)
{
    Material mat = MakeTestPBRMaterial();
    mat.SetBindlessTextureIndex("albedoMap"_sid, 10u);
    EXPECT_EQ(mat.GetBindlessTextureIndex("albedoMap"_sid), 10u);

    mat.SetBindlessTextureIndex("albedoMap"_sid, 99u);
    EXPECT_EQ(mat.GetBindlessTextureIndex("albedoMap"_sid), 99u);
}

// ---- Named texture slots: @texture map routing (slice K) ----

// A material with a resolved @texture map routes a USER texture name to its
// packed ordinal — the ordinal the shader compiled its GE_TEXSLOT_ macro to.
TEST(MaterialTest, UserTextureSlotMap_RoutesUserNameToPackedOrdinal)
{
    Material mat = MakeTestPBRMaterial();
    // water-like declared set: albedoMap->0, normalMap->1, flowMask->2 (user).
    mat.SetTextureSlotMap({{"albedoMap", 0}, {"normalMap", 1}, {"flowMask", 2}});

    mat.SetBindlessTextureIndex("flowMask"_sid, 77u);
    EXPECT_EQ(mat.GetBindlessTextureIndex("flowMask"_sid), 77u);
    // Ordinal 2 is kMetalRough; the user name landed exactly on that physical slot.
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kMetalRough), 77u);

    // Well-known names still resolve to their canonical ordinal through the map.
    mat.SetBindlessTextureIndex("albedoMap"_sid, 3u);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kAlbedo), 3u);
}

// Registration parity: with no @texture map (every legacy surface) routing is
// byte-for-byte the old behavior — well-known via the ladder, unknown names
// ignored by the setter and read back unset.
TEST(MaterialTest, UserTextureSlotMap_EmptyMapKeepsLadderBehavior)
{
    Material mat = MakeTestPBRMaterial();
    mat.SetBindlessTextureIndex("albedoMap"_sid, 12u);
    EXPECT_EQ(mat.GetBindlessTextureIndex("albedoMap"_sid), 12u);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kAlbedo), 12u);

    // A user name that no map declares is unknown to the ladder -> reads unset.
    EXPECT_EQ(mat.GetBindlessTextureIndex("flowMask"_sid), 0u);
}

// ---- TextureSlot enum and TextureSlotFromName ----

TEST(MaterialTest, TextureSlotFromName_KnownNames)
{
    EXPECT_EQ(TextureSlotFromName("albedoMap"_sid), TextureSlot::kAlbedo);
    EXPECT_EQ(TextureSlotFromName("normalMap"_sid), TextureSlot::kNormal);
    EXPECT_EQ(TextureSlotFromName("metallicRoughnessMap"_sid), TextureSlot::kMetalRough);
    EXPECT_EQ(TextureSlotFromName("emissiveMap"_sid), TextureSlot::kEmissive);
    EXPECT_EQ(TextureSlotFromName("aoMap"_sid), TextureSlot::kAO);
    EXPECT_EQ(TextureSlotFromName("coatNormalMap"_sid), TextureSlot::kCoatNormal);
}

TEST(MaterialTest, TextureSlotFromName_UnknownName)
{
    EXPECT_EQ(TextureSlotFromName("nonexistent"_sid), TextureSlot::kCount);
    EXPECT_EQ(TextureSlotFromName("specularMap"_sid), TextureSlot::kCount);
}

// The well-known name ladder exists twice by necessity (module layering):
// TextureSlotFromName here in Engine drives binding, ShaderComposer's
// kWellKnownSlots in the Rendering module drives GE_TEXSLOT_* macro emission.
// If they ever disagree, a well-known name binds one ordinal and samples
// another — silent wrong-texture with no error. This test is the guard.
TEST(MaterialTest, WellKnownLadder_AgreesWithComposerResolution)
{
    const std::pair<const char*, StringId> kNames[] = {
        {"albedoMap", "albedoMap"_sid},
        {"normalMap", "normalMap"_sid},
        {"metallicRoughnessMap", "metallicRoughnessMap"_sid},
        {"emissiveMap", "emissiveMap"_sid},
        {"aoMap", "aoMap"_sid},
        {"coatNormalMap", "coatNormalMap"_sid},
        {"roughnessMap", "roughnessMap"_sid},
        {"metallicMap", "metallicMap"_sid},
        {"triplanarAlbedoTop", "triplanarAlbedoTop"_sid},
        {"triplanarNormalTop", "triplanarNormalTop"_sid},
        {"triplanarAlbedoSide", "triplanarAlbedoSide"_sid},
        {"triplanarNormalSide", "triplanarNormalSide"_sid},
        {"triplanarAlbedoBottom", "triplanarAlbedoBottom"_sid},
        {"triplanarNormalBottom", "triplanarNormalBottom"_sid},
    };
    for (const auto& [name, sid] : kNames)
    {
        const std::string source = std::string("// @texture ") + name + "\n";
        const auto resolution = ShaderComposer::ResolveTextureSlots(source);
        ASSERT_FALSE(resolution.Rejected) << name;
        ASSERT_EQ(resolution.DeclaredSlots.size(), 1u) << name;
        EXPECT_EQ(resolution.DeclaredSlots[0].first, name);
        const TextureSlot engineSlot = TextureSlotFromName(sid);
        ASSERT_NE(engineSlot, TextureSlot::kCount) << name;
        EXPECT_EQ(resolution.DeclaredSlots[0].second,
                  static_cast<uint8_t>(engineSlot))
            << "ladders disagree for " << name;
    }
}

// ---- triplanar_pbr front-end: slot mapping + param layout ----

// The 3-axis albedo/normal set is interleaved across the 8 shared slots so the
// triset-collapse fast path lands in slots 0-1. Albedo -> even slots, normal -> odd.
TEST(MaterialTest, TextureSlotFromName_TriplanarInterleavedLayout)
{
    EXPECT_EQ(TextureSlotFromName("triplanarAlbedoTop"_sid), TextureSlot::kAlbedo);       // 0
    EXPECT_EQ(TextureSlotFromName("triplanarNormalTop"_sid), TextureSlot::kNormal);       // 1
    EXPECT_EQ(TextureSlotFromName("triplanarAlbedoSide"_sid), TextureSlot::kMetalRough);  // 2
    EXPECT_EQ(TextureSlotFromName("triplanarNormalSide"_sid), TextureSlot::kEmissive);    // 3
    EXPECT_EQ(TextureSlotFromName("triplanarAlbedoBottom"_sid), TextureSlot::kAO);        // 4
    EXPECT_EQ(TextureSlotFromName("triplanarNormalBottom"_sid), TextureSlot::kCoatNormal);// 5
}

// The collapse fast path (top==side==bottom) binds only the top pair; that pair MUST be
// slots 0-1 so the other four stay at their bindless defaults. This pins the invariant.
TEST(MaterialTest, TextureSlotFromName_TriplanarCollapsePairIsSlots0And1)
{
    EXPECT_EQ(static_cast<uint32_t>(TextureSlotFromName("triplanarAlbedoTop"_sid)), 0u);
    EXPECT_EQ(static_cast<uint32_t>(TextureSlotFromName("triplanarNormalTop"_sid)), 1u);
}

// The triplanar params live in lanes 23-25 (uParams22-24 by legacy name). Register 12 distinct
// property layouts at the production offsets and prove none aliases another (a mid-list
// reorder or an overlapping offset would cross-contaminate here).
TEST(MaterialTest, TriplanarParams_RoundTripWithoutAliasing)
{
    using GameEngine::MaterialGpuParams;
    Material mat = Material::TestFactory::Create(
        GUID::Generate(), "TestTriplanar", kMaterialParamBlockBytes);

    const uint32_t tBase = MaterialParamLaneOffset(23);
    const uint32_t mBase = MaterialParamLaneOffset(24);
    const uint32_t rBase = MaterialParamLaneOffset(25);

    struct Named { const char* name; uint32_t offset; float value; };
    const Named params[] = {
        {"triplanarTilingTop",       tBase + 0,  0.11f},
        {"triplanarTilingSide",      tBase + 4,  0.22f},
        {"triplanarTilingBottom",    tBase + 8,  0.33f},
        {"triplanarBlendSharpness",  tBase + 12, 0.44f},
        {"triplanarMetallicTop",     mBase + 0,  0.55f},
        {"triplanarMetallicSide",    mBase + 4,  0.66f},
        {"triplanarMetallicBottom",  mBase + 8,  0.77f},
        {"triplanarLayered",         mBase + 12, 1.00f},
        {"triplanarRoughnessTop",    rBase + 0,  0.12f},
        {"triplanarRoughnessSide",   rBase + 4,  0.23f},
        {"triplanarRoughnessBottom", rBase + 8,  0.34f},
        {"triplanarNormalLayered",   rBase + 12, 1.00f},
    };

    for (const auto& p : params)
        Material::TestFactory::AddPropertyLayout(mat, HashStringId(p.name), p.offset, 4);
    for (const auto& p : params)
        mat.SetFloat(HashStringId(p.name), p.value);

    // Each reads back exactly what was written -> the 12 lanes are disjoint.
    for (const auto& p : params)
        EXPECT_FLOAT_EQ(mat.GetFloat(HashStringId(p.name)), p.value) << p.name;
}

TEST(MaterialTest, BindlessTextureIndex_DirectSlotAccess)
{
    Material mat = MakeTestPBRMaterial();
    mat.SetBindlessTextureIndex(TextureSlot::kAlbedo, 42u);
    mat.SetBindlessTextureIndex(TextureSlot::kNormal, 7u);

    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kAlbedo), 42u);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kNormal), 7u);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kEmissive), 0u);
}

TEST(MaterialTest, BindlessTextureIndices_BulkAccessor)
{
    Material mat = MakeTestPBRMaterial();
    mat.SetBindlessTextureIndex(TextureSlot::kAlbedo, 10u);
    mat.SetBindlessTextureIndex(TextureSlot::kNormal, 20u);
    mat.SetBindlessTextureIndex(TextureSlot::kMetalRough, 30u);
    mat.SetBindlessTextureIndex(TextureSlot::kEmissive, 40u);

    const auto& indices = mat.GetBindlessTextureIndices();
    EXPECT_EQ(indices[0], 10u);
    EXPECT_EQ(indices[1], 20u);
    EXPECT_EQ(indices[2], 30u);
    EXPECT_EQ(indices[3], 40u);
    // Unused slots remain zero.
    for (uint32_t i = 4; i < kTextureSlotArraySize; ++i)
        EXPECT_EQ(indices[i], 0u);
}

TEST(MaterialTest, BindlessTextureIndex_UnknownSlotName_ReturnsZero)
{
    Material mat = MakeTestPBRMaterial();
    mat.SetBindlessTextureIndex(TextureSlot::kAlbedo, 42u);

    // Unknown names return 0, not the value of any other slot.
    EXPECT_EQ(mat.GetBindlessTextureIndex("nonexistent"_sid), 0u);
}

// A name that resolves to no slot reaches this setter through the document
// replay paths (decode-complete bind, hot-reload sweeps, device-rebuild
// replay) whenever a hand-edited .material key — or a name tracked before a
// surface's @texture set changed — arrives here. It must degrade to a no-op,
// mirroring the getter's 0, never abort the process.
TEST(MaterialTest, SetBindlessTextureIndex_UnknownName_IsIgnoredNotFatal)
{
    Material mat = MakeTestPBRMaterial();
    mat.InitBindlessDefaults(/*white*/100u, /*flatNormal*/200u, /*black*/400u);
    uint32_t snapshot[kTextureSlotArraySize];
    std::memcpy(snapshot, mat.GetBindlessTextureIndices(), sizeof snapshot);
    const uint64_t before = Material::GetGlobalContentEpoch();

    mat.SetBindlessTextureIndex("baseColorMap"_sid, 42u);

    EXPECT_EQ(mat.GetBindlessTextureIndex("baseColorMap"_sid), 0u);
    EXPECT_EQ(std::memcmp(snapshot, mat.GetBindlessTextureIndices(), sizeof snapshot), 0)
        << "an unknown name must not write any slot";
    EXPECT_EQ(Material::GetGlobalContentEpoch(), before)
        << "an ignored write must not dirty the material";
}

// ---- ValidateDocumentTextureKey: where .material keys meet the slot tables ----

TEST(MaterialTest, ValidateDocumentTextureKey_ResolvesLadderAndUserNames)
{
    Material mat = MakeTestPBRMaterial();
    mat.SetTextureSlotMap({{"albedoMap", 0}, {"flowMask", 2}});

    EXPECT_TRUE(mat.ValidateDocumentTextureKey("albedoMap"));
    EXPECT_TRUE(mat.ValidateDocumentTextureKey("flowMask"));  // surface-declared
    // A declaring surface owns its ordinals: a ladder name it does not declare is unknown there.
    EXPECT_FALSE(mat.ValidateDocumentTextureKey("normalMap"));
    EXPECT_FALSE(mat.ValidateDocumentTextureKey("baseColorMap"));
    EXPECT_FALSE(mat.ValidateDocumentTextureKey("uBaseColor"));
}

namespace
{
// Collects the unknown-texture-key warnings ValidateDocumentTextureKey emits.
// The sink outlives the capture (Log::AddSink takes ownership), so state is
// shared_ptr-owned — same idiom as MaterialBridgeIntegrationTests' counters.
class UnknownKeyWarningCapture
{
  public:
    explicit UnknownKeyWarningCapture(const GUID& materialGuid)
    {
        Logger::Log::Initialize({});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        sink->RegisterCallback([state = m_State, materialTag = "(" + materialGuid.ToString() + ")"](const Logger::LogMessage& msg) {
            if (msg.Message.find("unknown texture key") != Logger::String::npos &&
                msg.Message.find(materialTag) != Logger::String::npos)
            {
                std::lock_guard lock(state->Mutex);
                state->Messages.push_back(msg.Message);
            }
        });
        Logger::Log::AddSink(std::move(sink));
    }

    int Count() const
    {
        Logger::Log::Flush();
        std::lock_guard lock(m_State->Mutex);
        return static_cast<int>(m_State->Messages.size());
    }

    Logger::String First() const
    {
        Logger::Log::Flush();
        std::lock_guard lock(m_State->Mutex);
        return m_State->Messages.empty() ? Logger::String{} : m_State->Messages.front();
    }

  private:
    struct State
    {
        std::mutex Mutex;
        std::vector<Logger::String> Messages;
    };
    std::shared_ptr<State> m_State = std::make_shared<State>();
};
} // namespace

TEST(MaterialTest, ValidateDocumentTextureKey_WarnsOnceNamingKeyAndCandidates)
{
    Material mat = MakeTestPBRMaterial();
    UnknownKeyWarningCapture warnings(mat.GetGuid());
    mat.SetTextureSlotMap({{"albedoMap", 0}, {"flowMask", 2}});

    // Other materials may log while this capture is active or still queued.
    Material unrelated = MakeTestPBRMaterial();
    EXPECT_FALSE(unrelated.ValidateDocumentTextureKey("unrelatedKey"));

    EXPECT_FALSE(mat.ValidateDocumentTextureKey("baseColorMap"));
    ASSERT_EQ(warnings.Count(), 1);
    const Logger::String msg = warnings.First();
    EXPECT_NE(msg.find("TestPBR"), Logger::String::npos) << "names the material";
    EXPECT_NE(msg.find("baseColorMap"), Logger::String::npos) << "names the offending key";
    EXPECT_NE(msg.find("flowMask"), Logger::String::npos) << "lists the surface-declared slots";
    EXPECT_NE(msg.find("albedoMap"), Logger::String::npos) << "lists the well-known names";

    // Documents re-apply per frame; the report must not.
    EXPECT_FALSE(mat.ValidateDocumentTextureKey("baseColorMap"));
    EXPECT_EQ(warnings.Count(), 1);
}

// The standard surface declares albedo through coat normal on their ladder ordinals and heightMap
// on ordinal 6, which the fixed ladder gives to roughnessMap. A stale roughnessMap key must resolve
// to nothing and report, never bind the height slot.
TEST(MaterialTest, UndeclaredNameOnADeclaringSurfaceResolvesUnboundAndWarns)
{
    Material mat = MakeTestPBRMaterial();
    UnknownKeyWarningCapture warnings(mat.GetGuid());
    mat.SetTextureSlotMap({{"albedoMap", 0}, {"normalMap", 1}, {"metallicRoughnessMap", 2}, {"emissiveMap", 3},
                           {"aoMap", 4}, {"coatNormalMap", 5}, {"heightMap", 6}});

    EXPECT_TRUE(mat.HasTextureSlot("heightMap"_sid));
    EXPECT_FALSE(mat.HasTextureSlot("roughnessMap"_sid));
    EXPECT_FALSE(mat.ValidateDocumentTextureKey("roughnessMap"));
    EXPECT_EQ(warnings.Count(), 1) << "the stale key reports, naming itself";
    EXPECT_NE(warnings.First().find("roughnessMap"), Logger::String::npos);

    mat.SetBindlessTextureIndex("heightMap"_sid, 41u);
    mat.SetBindlessTextureIndex("roughnessMap"_sid, 99u);
    EXPECT_EQ(mat.GetBindlessTextureIndex(static_cast<TextureSlot>(6)), 41u)
        << "the stale ladder name wrote over the height slot";
    EXPECT_EQ(mat.GetBindlessTextureIndex("roughnessMap"_sid), 0u);
}

// The names a material has a slot for, which editors write back to clear a removed binding: a
// declaring surface's own names (user names included), or the canonical ladder otherwise.
TEST(MaterialTest, TextureSlotNamesFollowTheSurfaceDeclarations)
{
    Material mat = MakeTestPBRMaterial();
    const std::vector<std::string_view> ladder = mat.GetTextureSlotNames();
    const std::vector<std::string_view> expectedLadder = {"albedoMap",   "normalMap", "metallicRoughnessMap",
                                                          "emissiveMap", "aoMap",     "coatNormalMap",
                                                          "roughnessMap", "metallicMap"};
    EXPECT_EQ(ladder, expectedLadder);

    mat.SetTextureSlotMap({{"albedoMap", 0}, {"flowMask", 1}, {"heightMap", 6}});
    const std::vector<std::string_view> declared = mat.GetTextureSlotNames();
    const std::vector<std::string_view> expectedDeclared = {"albedoMap", "flowMask", "heightMap"};
    EXPECT_EQ(declared, expectedDeclared);
    for (const std::string_view name : declared)
        EXPECT_TRUE(mat.HasTextureSlot(HashStringId(name))) << name;
}

TEST(MaterialTest, ValidateDocumentTextureKey_RearmsWhenKeyBecomesDeclared)
{
    Material mat = MakeTestPBRMaterial();
    UnknownKeyWarningCapture warnings(mat.GetGuid());

    EXPECT_FALSE(mat.ValidateDocumentTextureKey("flowMask"));
    EXPECT_EQ(warnings.Count(), 1);

    // The surface gains `// @texture flowMask`: the key resolves and the
    // dedup entry clears...
    mat.SetTextureSlotMap({{"flowMask", 0}});
    EXPECT_TRUE(mat.ValidateDocumentTextureKey("flowMask"));

    // ...so removing the declaration again reports again, not silently.
    mat.SetTextureSlotMap({});
    EXPECT_FALSE(mat.ValidateDocumentTextureKey("flowMask"));
    EXPECT_EQ(warnings.Count(), 2);
}

TEST(MaterialTest, BindlessTextureIndex_InitDefaults)
{
    constexpr uint32_t kWhite      = 100u;
    constexpr uint32_t kFlatNormal = 200u;
    constexpr uint32_t kBlack      = 400u;

    Material mat = MakeTestPBRMaterial();
    mat.InitBindlessDefaults(kWhite, kFlatNormal, kBlack);

    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kAlbedo),     kWhite);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kNormal),     kFlatNormal);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kMetalRough), kWhite);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kEmissive),   kWhite); // emission multiplies the texture
    // All 8 slots must hold a valid bindless index — uninitialised slots fall
    // through to bindless idx 0 ("not set"), which the shader samples as
    // undefined data and produces the "stale texture leaks onto blank object"
    // symptom for any extended-PBR surface shader.
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kAO),         kWhite);
    // Coat normal seeds to the flat-normal default (not black): an unassigned coat
    // normal must read flat (0.5,0.5,1.0) so it leaves the coat lobe unperturbed.
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kCoatNormal), kFlatNormal);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kRoughness),  kWhite);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kMetallic),   kBlack);
}

TEST(MaterialTest, BindlessTextureIndex_StringIdAndSlotInterop)
{
    Material mat = MakeTestPBRMaterial();

    // Write via StringId, read via TextureSlot.
    mat.SetBindlessTextureIndex("albedoMap"_sid, 55u);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kAlbedo), 55u);

    // Write via TextureSlot, read via StringId.
    mat.SetBindlessTextureIndex(TextureSlot::kNormal, 77u);
    EXPECT_EQ(mat.GetBindlessTextureIndex("normalMap"_sid), 77u);
}

TEST(MaterialTest, BindlessTextureIndex_InitDefaults_OverriddenBySet)
{
    Material mat = MakeTestPBRMaterial();
    mat.InitBindlessDefaults(/*white*/100u, /*flatNormal*/200u, /*black*/400u);

    // Override one slot.
    mat.SetBindlessTextureIndex(TextureSlot::kAlbedo, 999u);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kAlbedo), 999u);
    // Others unchanged.
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kNormal), 200u);
}

// ---- GPU scene material index ----

TEST(MaterialTest, GpuSceneMaterialIndex_DefaultIsUnset)
{
    Material mat = MakeTestPBRMaterial();
    EXPECT_EQ(mat.GetGpuSceneMaterialIndex(), 0xFFFFFFFFu);
}

TEST(MaterialTest, GpuSceneMaterialIndex_SetViaTestFactory)
{
    Material mat = MakeTestPBRMaterial();
    Material::TestFactory::SetGpuSceneMaterialIndex(mat, 5u);
    EXPECT_EQ(mat.GetGpuSceneMaterialIndex(), 5u);
}

// ---- Graphics pipeline id via TestFactory ----

TEST(MaterialTest, GraphicsPipelineId_DefaultIsInvalid)
{
    Material mat = MakeTestPBRMaterial();
    EXPECT_FALSE(mat.GetGraphicsPipelineId().IsValid());
}

TEST(MaterialTest, GraphicsPipelineId_SetViaTestFactory)
{
    Material mat = MakeTestPBRMaterial();
    Rendering::GraphicsPipelineId id{42u};
    Material::TestFactory::SetGraphicsPipelineId(mat, id);
    EXPECT_TRUE(mat.GetGraphicsPipelineId().IsValid());
    EXPECT_EQ(mat.GetGraphicsPipelineId().Value, id.Value);
}

// ---- User @texture slots default to multiplicative identity (white) ----

// A USER @texture name packs into whatever ordinal is free, so it inherits that
// ordinal's ladder default — flat normal on slot 1, black on 3 and 7. None of
// those mean anything for a name the engine knows nothing about, and a mask
// sampled at 0.5 (or 0.0) silently attenuates whatever the surface multiplies it
// into. White is the only defensible default: it leaves the surface's own maths
// alone. Regresses the starter template, whose `accentMask` packs onto slot 1.
TEST(MaterialTest, UserTextureSlot_DefaultsToWhiteNotTheLadderDefault)
{
    constexpr uint32_t kWhite      = 100u;
    constexpr uint32_t kFlatNormal = 200u;
    constexpr uint32_t kBlack      = 400u;

    Material mat = MakeTestPBRMaterial();
    mat.InitBindlessDefaults(kWhite, kFlatNormal, kBlack);
    // Template shape: one declared well-known name (slot 0) + one user name,
    // which packs onto the lowest free ordinal (1 = kNormal).
    mat.SetTextureSlotMap({{"albedoMap", 0}, {"accentMask", 1}});

    EXPECT_EQ(mat.GetBindlessTextureIndex("accentMask"_sid), kWhite)
        << "an unassigned user slot must sample 1x1 white, not the slot's ladder default";
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kNormal), kWhite);
    // The declared well-known slot keeps its own ladder default.
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kAlbedo), kWhite);
}

// Mutation guard: a DECLARED well-known name must keep the ladder default the
// engine chose for it. Re-seeding every mapped slot to white would flatten the
// coat normal and the metallic black.
TEST(MaterialTest, WellKnownTextureSlot_KeepsItsLadderDefaultWhenMapped)
{
    constexpr uint32_t kWhite      = 100u;
    constexpr uint32_t kFlatNormal = 200u;
    constexpr uint32_t kBlack      = 400u;

    Material mat = MakeTestPBRMaterial();
    mat.InitBindlessDefaults(kWhite, kFlatNormal, kBlack);
    mat.SetTextureSlotMap({{"normalMap", 1}, {"coatNormalMap", 5}, {"metallicMap", 7}});

    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kNormal),     kFlatNormal);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kCoatNormal), kFlatNormal);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kMetallic),   kBlack);
}

// Re-registration installs the same slot map on a material whose bindings are
// already live, so the map arriving must never clobber an assigned texture.
TEST(MaterialTest, UserTextureSlot_SlotMapArrivalKeepsAnAssignedTexture)
{
    constexpr uint32_t kWhite = 100u;

    Material mat = MakeTestPBRMaterial();
    mat.InitBindlessDefaults(kWhite, /*flatNormal*/200u, /*black*/400u);
    mat.SetTextureSlotMap({{"albedoMap", 0}, {"accentMask", 1}});

    mat.SetTexture("accentMask"_sid, Rendering::TextureHandle{7u});
    mat.SetBindlessTextureIndex("accentMask"_sid, 555u);

    mat.SetTextureSlotMap({{"albedoMap", 0}, {"accentMask", 1}});
    EXPECT_EQ(mat.GetBindlessTextureIndex("accentMask"_sid), 555u);
}

// InitBindlessDefaults is a FULL re-bake: a device rebuild kills every
// previously-baked index and TextureService replays the real bindings right
// after, so it re-seeds bound slots too. What matters is WHERE a user ordinal
// lands in the window before that replay — white, never the meaning of the
// ordinal it borrowed.
TEST(MaterialTest, UserTextureSlot_DeviceRebuildRebakesToWhiteNotTheLadderDefault)
{
    constexpr uint32_t kWhite      = 100u;
    constexpr uint32_t kFlatNormal = 200u;

    Material mat = MakeTestPBRMaterial();
    mat.InitBindlessDefaults(kWhite, kFlatNormal, /*black*/400u);
    mat.SetTextureSlotMap({{"albedoMap", 0}, {"accentMask", 1}});
    mat.SetTexture("accentMask"_sid, Rendering::TextureHandle{7u});
    mat.SetBindlessTextureIndex("accentMask"_sid, 555u);

    mat.InitBindlessDefaults(kWhite, kFlatNormal, /*black*/400u);
    EXPECT_EQ(mat.GetBindlessTextureIndex("accentMask"_sid), kWhite)
        << "a user slot must never fall back to the meaning of the ordinal it borrowed";
}

// Legacy surfaces (no @texture declarations) get an empty map and must keep the
// fixed ladder byte-for-byte.
TEST(MaterialTest, LegacySurfaceWithNoSlotMapKeepsTheFullLadder)
{
    constexpr uint32_t kWhite      = 100u;
    constexpr uint32_t kFlatNormal = 200u;
    constexpr uint32_t kBlack      = 400u;

    Material mat = MakeTestPBRMaterial();
    mat.InitBindlessDefaults(kWhite, kFlatNormal, kBlack);

    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kNormal),     kFlatNormal);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kEmissive),   kWhite);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kCoatNormal), kFlatNormal);
    EXPECT_EQ(mat.GetBindlessTextureIndex(TextureSlot::kMetallic),   kBlack);
}
