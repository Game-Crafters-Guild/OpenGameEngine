#pragma once

#include "Components/Markup/Markup.h"
#include "ECS/Entity.h"
#include "MarkupECS/MarkupRegionArea.h"
#include "MarkupECS/MarkupRegionMesh.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace GameEngine::MarkupECS
{

// What a region's ground depends on: its outline (the spline's data and version), its placement,
// its member list, the sea level and the ground under it (the conform surface revision). A change
// in any part rebuilds the ground; the extrusion height only rebuilds the mesh on top of it.
struct MarkupRegionGroundKey
{
    uint32 SplineIndex = 0;
    uint32 SplineGeneration = 0;
    uint64 SplineVersion = 0;
    float32 Matrix[16] = {};
    float32 SeaLevel = 0.0f;
    uint64 GroundRevision = 0;
    // The members as the region lists them (Components::MarkupRegion); none for a path.
    uint32 MemberCount = 0;
    Components::MarkupRegionMember Members[Components::kMaxRegionMembers] = {};

    bool operator==(const MarkupRegionGroundKey&) const = default;
};

// The displays of the regions and paths (a path is an open outline at no height), built only for
// those in view and kept while they stay in view: an
// entry is rebuilt when its key changes or a member it lists changes its footprint, and evicted
// kEvictAfterFrames frames after it was last drawn (a hidden, deleted or switched-off region is
// not drawn and goes the same way). A member's footprint key is kept once however many regions
// list it, with the reverse index of the regions listing it, so a member that moves, changes
// shape, is deleted or is revived rebuilds those regions and no other. Owned by the mark-up
// editor bridge, shared by every Scene View and both of the display's drawers (the glow and the
// gizmo); cleared when the scene closes. Main thread only.
class MarkupRegionDisplayCache
{
  public:
    static constexpr uint64 kEvictAfterFrames = 120;

    struct Entry
    {
        MarkupRegionGroundKey GroundKey;
        float32 ExtrudeHeight = 0.0f;
        MarkupRegionGround Ground;
        MarkupRegionMesh Mesh;
        // Moves with every mesh rebuild: what a GPU copy of the mesh was written from.
        uint64 MeshRevision = 0;
        // The frame the mesh last changed and the frame it was last drawn.
        uint64 ChangedFrame = 0;
        uint64 LastSeenFrame = 0;
        // Set when a member it lists changed its footprint since the ground was built.
        bool MemberChanged = false;
    };

    using GroundBuilder = std::function<MarkupRegionGround()>;
    using MemberKeyReader = std::function<MarkupFootprintKey(ECS::EntityHandle member)>;

    // The display of `region` for `key` at `extrudeHeight`, seen on `frame`: each member the key
    // lists is read through `readMember`; the ground is built by `buildGround` only on a new entry,
    // a changed key or a changed member, the mesh only when the ground or the height changed. The
    // entry stays valid until the next EvictUnseen or Clear.
    const Entry& Resolve(ECS::EntityHandle region, const MarkupRegionGroundKey& key, float32 extrudeHeight,
                         uint64 frame, const MemberKeyReader& readMember, const GroundBuilder& buildGround);
    // The entry of `region`, or null; does not build or stamp it.
    const Entry* Find(ECS::EntityHandle region) const;
    // Drops the entries last drawn more than kEvictAfterFrames before `frame`.
    void EvictUnseen(uint64 frame);
    void Clear();

    std::size_t GetSize() const { return m_Entries.Size(); }
    // How many grounds and meshes were built since the cache was made: the rebuild cost's counters.
    uint64 GetGroundBuilds() const { return m_GroundBuilds; }
    uint64 GetMeshBuilds() const { return m_MeshBuilds; }

  private:
    // A member's footprint key as last read, and the regions whose ground lists it (by handle id).
    struct MemberState
    {
        MarkupFootprintKey Key;
        std::vector<uint32> Regions;
    };

    // Reads `member`; a changed footprint marks every region listing it.
    void RefreshMember(ECS::EntityHandle member, const MemberKeyReader& readMember);
    // Adds `region` to the reverse index of each member `key` lists, reading a member listed by no
    // other region.
    void ListMembers(uint32 region, const MarkupRegionGroundKey& key, const MemberKeyReader& readMember);
    void UnlistMembers(uint32 region, const MarkupRegionGroundKey& key);

    FlatMap<uint32, std::unique_ptr<Entry>> m_Entries; // by the region's handle id
    FlatMap<uint32, MemberState> m_Members;            // by the member's handle id
    uint64 m_GroundBuilds = 0;
    uint64 m_MeshBuilds = 0;
    uint64 m_NextMeshRevision = 1;
};

} // namespace GameEngine::MarkupECS
