// Owner validation AT THE REAL EXPORT BOUNDARY: the GE_UI_ type-lifecycle exports as a script
// actually reaches them, out of the built GameEngine.Native.dll.
//
// WHY THIS SUITE AND NOT A MANAGED ONE. The property under test is that a managed load context
// cannot name another context's types by passing an owner id it was never given. The managed test
// host cannot assert it: the assembly those suites load as "GameEngine.Native" is a test double
// (target GameEngine.NativeShim, Tests/Jobs/CMakeLists.txt, OUTPUT_NAME collides deliberately)
// that implements exactly one GE_UI_ function and none of the type lifecycle. Teaching the double
// to refuse a forged id would assert that the DOUBLE refuses it, which says nothing about
// ManagedTypeOwners. The shim's own comment nominates this suite instead, and this is the suite
// that links the real DLL.
//
// Nor does the caller's LANGUAGE change the threat model. A C# type declaring its own DllImport
// and a C++ caller passing a forged number arrive at the same validating code path with the same
// uint64_t; there is no managed-side enforcement surface for a managed arm to cover. What is
// specific to managed callers is that they are OUTSIDE the trust boundary at all, which is why
// the validation exists — and the export is where it is applied.
//
// WHAT THIS ADDS OVER THE UITests ARMS. ManagedTypeLifecycleTests asserts the engine-side
// contract (ManagedTypeOwners::ValidateOwner) directly; it does not link GameEngine.Native and
// cannot see whether the exports actually gate on it. These arms call the exported symbols and
// read the refusal out of Engine.dll's registry, so they also pin the wiring BETWEEN the two
// modules: a delta of 0 here would mean the export never consulted the registry the engine keeps
// (or that the two DLLs did not share it), whatever the engine-side unit arms say.

#include "Scripting/ManagedTypeOwners.h"
#include "Scripting/ScriptingABI.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace
{
using GameEngine::Scripting::ManagedTypeOwners;

// Refusals are counted for the life of the process and other suites in this binary share the
// registry, so every arm asserts on a DELTA it brackets itself. ResetForTests is deliberately not
// used: it would make these arms depend on running before whatever else touches the registry.
std::size_t Rejections()
{
    return ManagedTypeOwners::Instance().RejectedCallCount();
}

// Unique per arm: ClaimTag refuses a tag another owner already holds, so two arms sharing a name
// would make the second one's positive control fail for the wrong reason.
std::string FreshTag(const char* stem)
{
    static std::uint64_t s_Counter = 0;
    return std::string("ge-abi-owner-test-") + stem + "-" + std::to_string(++s_Counter);
}
} // namespace

TEST(ScriptingAbiUiTypeOwner, AForgedOwnerIsRefusedByEveryExportThatTakesOne)
{
    // The forged case: a script that declared its own DllImport and passed a number it liked.
    constexpr std::uint64_t kForged = 0xDEAD'BEEF'FEED'FACEull;
    ASSERT_FALSE(ManagedTypeOwners::Instance().IsLive(kForged))
        << "the constant this arm forges collided with a live minted id";

    const std::size_t before = Rejections();

    uint64_t tagId = 1234ull; // Non-zero going in, so "left at 0" is an assertion and not luck.
    const std::string tag = FreshTag("forged");
    EXPECT_EQ(GE_UI_RegisterElementType(kForged, tag.c_str(), &tagId), GE_Result_InvalidArg);
    EXPECT_EQ(tagId, 0ull) << "a refused registration must not hand back a claimable tag id";

    uint64_t orphaned = 99ull;
    EXPECT_EQ(GE_UI_OrphanElementTypes(kForged, &orphaned), GE_Result_InvalidArg);
    EXPECT_EQ(orphaned, 0ull);

    EXPECT_EQ(GE_UI_NotifyReloadCompleted(kForged), GE_Result_InvalidArg);
    EXPECT_EQ(GE_UI_ReleaseTypeOwner(kForged), GE_Result_InvalidArg);

    // Four refusals, one per export. A delta of 0 would mean the exports refused without
    // recording it — invisible, which is the failure mode the counter exists to close — or that
    // the export and this test are not looking at the same registry.
    EXPECT_EQ(Rejections() - before, 4u)
        << "a refusal that is not recorded leaves a forged id indistinguishable from a call that "
           "legitimately had nothing to do";
}

TEST(ScriptingAbiUiTypeOwner, AnOwnerThatWasReleasedIsStaleAndTheExportsRefuseIt)
{
    uint64_t owner = 0;
    ASSERT_EQ(GE_UI_AcquireTypeOwner(&owner), GE_Result_Ok);
    ASSERT_NE(owner, 0ull);
    ASSERT_EQ(GE_UI_ReleaseTypeOwner(owner), GE_Result_Ok);

    const std::size_t before = Rejections();

    // The stale case: a context that already handed its id back — and, because retired ids are
    // never minted again, an id that can never become valid once more.
    uint64_t tagId = 1234ull;
    const std::string tag = FreshTag("stale");
    EXPECT_EQ(GE_UI_RegisterElementType(owner, tag.c_str(), &tagId), GE_Result_InvalidArg);
    EXPECT_EQ(tagId, 0ull);

    uint64_t orphaned = 99ull;
    EXPECT_EQ(GE_UI_OrphanElementTypes(owner, &orphaned), GE_Result_InvalidArg);
    EXPECT_EQ(orphaned, 0ull);

    EXPECT_EQ(GE_UI_NotifyReloadCompleted(owner), GE_Result_InvalidArg);
    EXPECT_EQ(GE_UI_ReleaseTypeOwner(owner), GE_Result_InvalidArg) << "a double release";

    EXPECT_EQ(Rejections() - before, 4u);
}

// The positive control for both arms above. Without it, an export that refused EVERYTHING —
// because the engine failed to initialize, say — would satisfy them exactly as well as one that
// validates.
TEST(ScriptingAbiUiTypeOwner, AMintedOwnerIsAcceptedByTheSameExportsAndRecordsNoRefusal)
{
    uint64_t owner = 0;
    ASSERT_EQ(GE_UI_AcquireTypeOwner(&owner), GE_Result_Ok);
    ASSERT_NE(owner, 0ull);

    const std::size_t before = Rejections();

    uint64_t tagId = 0;
    const std::string tag = FreshTag("live");
    ASSERT_EQ(GE_UI_RegisterElementType(owner, tag.c_str(), &tagId), GE_Result_Ok);
    EXPECT_NE(tagId, 0ull) << "a live owner's fresh tag must be claimed";

    // The required order, which is also this arm's cleanup: orphan at the unload seam, then the
    // verdict with the same id, and only then retire it.
    uint64_t orphaned = 0;
    EXPECT_EQ(GE_UI_OrphanElementTypes(owner, &orphaned), GE_Result_Ok);
    EXPECT_EQ(GE_UI_NotifyReloadCompleted(owner), GE_Result_Ok);
    EXPECT_EQ(GE_UI_ReleaseTypeOwner(owner), GE_Result_Ok);

    EXPECT_EQ(Rejections() - before, 0u) << "a live owner must not be recorded as a refusal";
}
