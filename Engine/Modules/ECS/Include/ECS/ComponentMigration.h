#pragma once

// Component schema migration across a native hot-reload.
//
// When a user edits a component struct (adds/removes/reorders a field) that is already
// placed on entities, the next reload changes the component's byte layout. ComponentLayoutChange
// captures the old + new field tables, sizes, and the new default bytes; World::MigrateComponentLayout
// re-packs every existing instance — preserving same-named fields and defaulting new ones — and
// resizes the archetype columns so the recorded size and the storage stride agree again.

#include "ECS/Reflection.h"  // FieldInfo

#include <cstdint>
#include <vector>

namespace GameEngine {
namespace ECS {

// Mirror of ECS.h's alias (kept here so this stays a light header).
using ComponentTypeId = std::uint64_t;

// A component whose byte layout changed across a reload.
struct ComponentLayoutChange
{
    ComponentTypeId Id = 0;
    std::vector<FieldInfo> OldFields; // field table before the reload (string views into the old DLL)
    std::size_t OldSize = 0;          // sizeof before the reload
    std::vector<FieldInfo> NewFields; // field table after the reload
    std::size_t NewSize = 0;          // sizeof after the reload (== NewDefaults.size())
    std::vector<std::uint8_t> NewDefaults; // default-constructed bytes at the new size
};

// One field carried across the layout change: copy [SrcOffset, +Size) of the old bytes
// into [DstOffset) of the new bytes.
struct FieldByteMove
{
    std::uint32_t SrcOffset;
    std::uint32_t DstOffset;
    std::uint32_t Size;
};

// Build the preserve-by-name remap. A field is carried over only when its Name, Type, and
// Size all match (a rename or retype is treated as drop-old + default-new, never a raw byte
// copy at a mismatched offset). Fields only in newFields keep their default; fields only in
// oldFields are dropped.
inline std::vector<FieldByteMove> BuildFieldRemap(const std::vector<FieldInfo>& oldFields,
                                                  const std::vector<FieldInfo>& newFields)
{
    std::vector<FieldByteMove> moves;
    moves.reserve(newFields.size());
    for (const auto& nf : newFields)
    {
        for (const auto& of : oldFields)
        {
            if (of.Name == nf.Name && of.Type == nf.Type && of.Size == nf.Size)
            {
                moves.push_back({of.Offset, nf.Offset, nf.Size});
                break;
            }
        }
    }
    return moves;
}

// True if the component's storage layout changed across a reload and existing instances must
// be migrated: a different total size, or any field added / removed / moved / retyped. Used to
// skip the (archetype-churning) migration when a reload left a component's layout untouched.
inline bool LayoutsDiffer(const std::vector<FieldInfo>& oldFields, std::size_t oldSize,
                          const std::vector<FieldInfo>& newFields, std::size_t newSize)
{
    if (oldSize != newSize || oldFields.size() != newFields.size())
        return true;
    for (const auto& nf : newFields)
    {
        bool matched = false;
        for (const auto& of : oldFields)
        {
            if (of.Name == nf.Name)
            {
                if (of.Offset != nf.Offset || of.Size != nf.Size || of.Type != nf.Type)
                    return true; // moved or retyped
                matched = true;
                break;
            }
        }
        if (!matched)
            return true; // added (or renamed)
    }
    return false;
}

} // namespace ECS
} // namespace GameEngine
