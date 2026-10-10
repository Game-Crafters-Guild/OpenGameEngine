#pragma once

#include "Animation/HumanBone.h"
#include "Types/Types.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace Animation
{

// Bone-name heuristic matcher. Maps a source-rig bone name (Mixamo / Synty /
// MetaHuman / Maya / VRM / glTF conventions) onto a HumanBone canonical slot.
//
// Implementation: a static priority-ordered table over normalized name
// tokens. Each row lists end-anchored aliases that must equal one of the
// body's trailing-token joins (mimics the `$` anchor of the previous regex
// table). First matching row wins. Stateless and thread-safe.
class HumanoidNameMatcher
{
  public:
    // Per-skeleton match output. SourceBoneName is empty when no source bone
    // was assigned (Canonical == HumanBone::None). CachedSourceIndex is the
    // bone's index into the source skeleton when produced via MatchSkeleton.
    struct Mapping
    {
        HumanBone Canonical = HumanBone::None;
        std::string SourceBoneName;
        uint32 CachedSourceIndex = ~0u;
    };

    // Whole-skeleton match summary. Mappings is parallel to canonical-slot
    // ownership: at most one mapping per HumanBone. Coverage is the fraction
    // of "required" body bones that matched; the threshold for an acceptable
    // import is enforced in AutoImportHumanoidRig.
    struct Result
    {
        std::vector<Mapping> Mappings;
        // Per-canonical-bone reverse index into Mappings; ~0u when unmapped.
        std::array<uint32, static_cast<size_t>(HumanBone::Count)> ByCanonical{};
        uint32 RequiredMatched = 0;
        uint32 RequiredTotal = 0;
        uint32 OptionalMatched = 0;
        uint32 OptionalTotal = 0;
        float Coverage = 0.0f; // RequiredMatched / RequiredTotal

        Result()
        {
            ByCanonical.fill(~0u);
        }

        // Lookup helper. Returns nullptr when the canonical slot is unmapped.
        const Mapping* Find(HumanBone canonical) const;
    };

    HumanoidNameMatcher() = default;

    // Match a single bone name to a canonical HumanBone. Returns
    // HumanBone::None if no pattern matches. Case-insensitive.
    HumanBone Match(std::string_view boneName) const;

    // Match a whole list of bone names. Each canonical slot is claimed by at
    // most one source bone (first-match-wins by canonical iteration order).
    Result MatchSkeleton(const std::vector<std::string>& boneNames) const;

    // Return true if HumanBone is a "required" body bone (Hips, Spine, Head,
    // arms, legs). Required bones drive the auto-import coverage gate.
    static bool IsRequiredBone(HumanBone bone);
};

} // namespace Animation
} // namespace GameEngine
