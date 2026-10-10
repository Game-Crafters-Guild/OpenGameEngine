#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
enum class ChangeSetKind : std::uint8_t
{
    None = 0,
    Subset = 1,
    All = 2,
};

template <typename IdT>
struct ChangeSet
{
    ChangeSetKind Kind = ChangeSetKind::None;
    std::vector<IdT> Ids;
    std::uint64_t Version = 0;
    // Monotonic counter bumped only by MarkStructureChanged (see the contract
    // note on ChangeTrackingProviderBase). Movement since the last consume means
    // the item set's identity/order/geometry footprint changed structurally, so
    // the consumer must rebuild its window (List/Grid) or flat row set (Tree)
    // rather than just repaint the affected cells.
    std::uint64_t StructureVersion = 0;
};

// Change-tracking contract for virtualized data providers.
//
//   MarkChanged / MarkChangedBatch  => PAINT-ONLY. The item's content changed
//     but its identity, index and geometry footprint are unchanged (unless the
//     view re-measures). The consumer rebinds only the affected pooled cells.
//   MarkStructureChanged            => STRUCTURAL. Items were added/removed/
//     reordered, or an item's expandability changed — anything that alters the
//     flat row set or the item-index mapping. Bumps a separate StructureVersion
//     (and the main version, so pull paths wake) without forcing an All
//     changeset; consumers treat StructureVersion movement as guard-breaking.
//   MarkAllChanged                  => STRUCTURAL + full repaint. Every item may
//     have new content; the consumer fully rebinds.
//
// Structural mutations MUST use MarkStructureChanged or MarkAllChanged — a bare
// MarkChanged leaves the flat/window mapping stale until an unrelated change
// happens to escalate it.
template <typename IdT>
class ChangeTrackingProviderBase
{
  public:
    using IdType = IdT;
    using ChangeSetType = ChangeSet<IdT>;

    void MarkChanged(IdT id)
    {
        MarkChangedBatch(&id, 1);
    }

    void MarkChangedBatch(const std::vector<IdT>& ids)
    {
        MarkChangedBatch(ids.data(), ids.size());
    }

    void MarkAllChanged()
    {
        ++m_Version;
        m_AllChangedVersion = m_Version;
        m_StructureVersion = m_Version;
        m_ChangedVersions.clear();
    }

    // Structural mutation that does not require repainting every existing cell's
    // content (adds/removes/reorders/expandability). Bumps the structure version
    // so consumers rebuild their window/flat set, and the main version so the
    // per-frame change pump and pull paths wake.
    void MarkStructureChanged()
    {
        ++m_Version;
        m_StructureVersion = m_Version;
    }

    // Current change-tracking version. Cheap counter read (no allocation) used
    // by the UIManager per-frame change pump to detect data mutations on an
    // otherwise-idle control.
    std::uint64_t GetChangeVersion() const { return m_Version; }

    void ConsumeChanges(std::uint64_t sinceVersion, ChangeSetType& out) const
    {
        out.Version = m_Version;
        out.StructureVersion = m_StructureVersion;
        if (sinceVersion == m_Version)
        {
            out.Kind = ChangeSetKind::None;
            out.Ids.clear();
            return;
        }

        if (m_AllChangedVersion != 0 && sinceVersion < m_AllChangedVersion)
        {
            out.Kind = ChangeSetKind::All;
            out.Ids.clear();
            return;
        }

        out.Kind = ChangeSetKind::Subset;
        out.Ids.clear();
        out.Ids.reserve(m_ChangedVersions.size());
        for (const auto& it : m_ChangedVersions)
        {
            if (it.second > sinceVersion)
                out.Ids.push_back(it.first);
        }

        if (out.Ids.empty())
        {
            out.Kind = ChangeSetKind::None;
            return;
        }

        if (out.Ids.size() > m_MaxIdsForSubset)
        {
            out.Kind = ChangeSetKind::All;
            out.Ids.clear();
        }
    }

  protected:
    void MarkChangedBatch(const IdT* ids, std::size_t count)
    {
        if (!ids || count == 0)
            return;

        ++m_Version;
        for (std::size_t i = 0; i < count; ++i)
        {
            m_ChangedVersions[ids[i]] = m_Version;
        }
        if (m_ChangedVersions.size() > m_MaxIdsForSubset)
        {
            m_AllChangedVersion = m_Version;
            m_ChangedVersions.clear();
        }
    }

    void SetMaxIdsForSubset(std::size_t maxIds)
    {
        m_MaxIdsForSubset = maxIds;
    }

  private:
    std::uint64_t m_Version = 0;
    std::uint64_t m_AllChangedVersion = 0;
    std::uint64_t m_StructureVersion = 0;
    std::unordered_map<IdT, std::uint64_t> m_ChangedVersions;
    std::size_t m_MaxIdsForSubset = 256;
};
} // namespace GameEngine
