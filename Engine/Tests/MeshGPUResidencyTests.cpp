// Tests for MeshGPUResidency: the upload-stamp ring that answers "are this
// mesh upload's bytes readable by the GPU yet?".
//
// The property under test is that every reading rule fails CLOSED. Several of
// these are red-armed against a specific wrong implementation and say which:
// an encoding where the open marker is not part of what the predicate reads,
// and a ring whose base never advances.

#include <gtest/gtest.h>

#include "Engine/Rendering/MeshGPUResidency.h"

#include <vector>

using namespace GameEngine::Rendering;

namespace
{

// Small enough that exhaustion and wraparound are reachable in a test.
constexpr uint32_t kTinyWindow = 4u;

} // namespace

// 0 is the sentinel every never-uploaded, released and tombstoned entry
// carries. It must never read resident.
TEST(MeshGPUResidencyTest, NeverUploadedSentinelIsNotResident)
{
    MeshGPUResidency residency(kTinyWindow);
    EXPECT_FALSE(residency.IsResident(0u));
}

// RED-ARMED against an encoding where the open marker lives outside the word
// the predicate reads: there the counter is 0 for the whole mid-upload window
// and the entry reads resident with none of its bytes written.
TEST(MeshGPUResidencyTest, OpenUploadIsNotResidentUntilTheBracketCloses)
{
    MeshGPUResidency residency(kTinyWindow);

    const uint64_t seq = residency.BeginUpload();
    ASSERT_NE(seq, 0u);

    // Forced non-residency: the upload is open.
    EXPECT_FALSE(residency.IsResident(seq));

    // Forcing removed: the same stamp is now resident.
    residency.EndUpload(seq);
    EXPECT_TRUE(residency.IsResident(seq));
}

// RED-ARMED against a base that advances by scanning past whatever it finds:
// there the base steps over an open-but-unarmed slot, and the below-base rule
// then reports a half-written mesh as resident.
TEST(MeshGPUResidencyTest, BaseNeverPassesAnOpenStamp)
{
    MeshGPUResidency residency(kTinyWindow);

    const uint64_t stalled = residency.BeginUpload();
    const uint64_t quick   = residency.BeginUpload();
    ASSERT_NE(stalled, 0u);
    ASSERT_NE(quick, 0u);

    residency.EndUpload(quick);

    EXPECT_LE(residency.BaseSeq(), stalled)
        << "the base must not overtake the stamp that is still open";
    EXPECT_FALSE(residency.IsResident(stalled));
    EXPECT_TRUE(residency.IsResident(quick));

    residency.EndUpload(stalled);
    EXPECT_GT(residency.BaseSeq(), quick);
    EXPECT_TRUE(residency.IsResident(stalled));
}

// RED-ARMED against a base that never advances: with the window forced tiny,
// cycling far more uploads than it holds refuses nothing only if each retired
// slot is reclaimed.
TEST(MeshGPUResidencyTest, SteadyStateCyclesFarMoreUploadsThanTheWindow)
{
    MeshGPUResidency residency(kTinyWindow);

    constexpr uint32_t kCycles = 1000u;
    for (uint32_t i = 0; i < kCycles; ++i)
    {
        const uint64_t seq = residency.BeginUpload();
        ASSERT_NE(seq, 0u) << "BeginUpload refused on cycle " << i
                           << " — the ring base is not advancing";
        EXPECT_FALSE(residency.IsResident(seq));
        residency.EndUpload(seq);
        EXPECT_TRUE(residency.IsResident(seq));
    }
    EXPECT_EQ(residency.WindowExhaustedCount(), 0u);
}

// A full window of open uploads must refuse the next mint rather than alias a
// live slot, and report the refusal.
TEST(MeshGPUResidencyTest, WindowExhaustionFailsClosedAndIsCounted)
{
    MeshGPUResidency residency(kTinyWindow);

    std::vector<uint64_t> open;
    for (uint32_t i = 0; i < residency.PendingWindow(); ++i)
    {
        const uint64_t seq = residency.BeginUpload();
        ASSERT_NE(seq, 0u);
        open.push_back(seq);
    }

    EXPECT_EQ(residency.BeginUpload(), 0u);
    EXPECT_EQ(residency.WindowExhaustedCount(), 1u);

    // The refusal is the never-uploaded sentinel, which reads non-resident —
    // that is what makes UploadMesh's degraded entry undrawable.
    EXPECT_FALSE(residency.IsResident(0u));

    for (uint64_t seq : open)
        residency.EndUpload(seq);
    EXPECT_NE(residency.BeginUpload(), 0u);
}

// Cancelling releases the slot so the ring keeps moving, which is what stops a
// released entry from pinning the base for the rest of the session.
TEST(MeshGPUResidencyTest, CancelRetiresAStampAndReleasesItsSlot)
{
    MeshGPUResidency residency(kTinyWindow);

    std::vector<uint64_t> open;
    for (uint32_t i = 0; i < residency.PendingWindow(); ++i)
        open.push_back(residency.BeginUpload());
    ASSERT_EQ(residency.BeginUpload(), 0u);

    for (uint64_t seq : open)
        residency.CancelRecordsFor(seq);

    for (uint32_t i = 0; i < residency.PendingWindow(); ++i)
    {
        const uint64_t seq = residency.BeginUpload();
        EXPECT_NE(seq, 0u) << "cancelled slots must be reusable";
        residency.EndUpload(seq);
    }
}

// 0 is a shared sentinel, not an upload identity, so cancelling it is defined
// and does nothing.
TEST(MeshGPUResidencyTest, CancelOfTheSentinelIsADefinedNoOp)
{
    MeshGPUResidency residency(kTinyWindow);

    const uint64_t before = residency.BaseSeq();
    residency.CancelRecordsFor(0u);
    EXPECT_EQ(residency.BaseSeq(), before);

    const uint64_t seq = residency.BeginUpload();
    ASSERT_NE(seq, 0u);
    residency.EndUpload(seq);
    EXPECT_TRUE(residency.IsResident(seq));
}

// A rebuild freed the storage every open stamp was written into, so the table
// is discarded rather than drained. Note what that means for a stamp minted
// before the reset: it reads RESIDENT, because it has no outstanding
// obligation. That is exactly why the contract requires callers to clear the
// stamps they hold in the same step, and MeshGPURegistry does.
TEST(MeshGPUResidencyTest, ResetAfterDeviceRebuildDiscardsTheWholeTable)
{
    MeshGPUResidency residency(kTinyWindow);

    const uint64_t openBefore = residency.BeginUpload();
    ASSERT_NE(openBefore, 0u);
    ASSERT_FALSE(residency.IsResident(openBefore));

    residency.ResetAfterDeviceRebuild();

    EXPECT_GT(residency.BaseSeq(), openBefore) << "the base must jump past every minted stamp";

    // The whole window is free again immediately after a reset.
    for (uint32_t i = 0; i < residency.PendingWindow(); ++i)
    {
        const uint64_t seq = residency.BeginUpload();
        EXPECT_NE(seq, 0u);
    }
    EXPECT_EQ(residency.WindowExhaustedCount(), 0u);
}

// The window is rounded up to a power of two so the slot index can be a mask,
// and clamped so a degenerate request still leaves room to advance.
TEST(MeshGPUResidencyTest, WindowIsRoundedToAPowerOfTwoAndClamped)
{
    EXPECT_EQ(MeshGPUResidency(0u).PendingWindow(), 2u);
    EXPECT_EQ(MeshGPUResidency(1u).PendingWindow(), 2u);
    EXPECT_EQ(MeshGPUResidency(3u).PendingWindow(), 4u);
    EXPECT_EQ(MeshGPUResidency(4u).PendingWindow(), 4u);
    EXPECT_EQ(MeshGPUResidency(1000u).PendingWindow(), 1024u);
}
