#pragma once

#include <array>
#include <span>
#include <string_view>

namespace GameEngine
{
namespace Rendering
{

// Checked-in VUID suppression table for the validation assert (GE_VK_VALIDATION_ASSERT).
// A source table — not a data file — so every entry is a code-review-visible debt item
// and stays greppable. Suppressed hits still count exactly in ValidationStatsStore
// (SuppressedCount, reported by get_validation_stats) — they can never hide; a match
// only skips the assert.
struct VuidSuppression
{
    std::string_view Vuid;
    std::string_view Reason;
    std::string_view Owner;
    std::string_view Date;
};

// Seeded empty on purpose: the burn-down fixes validation errors rather than
// suppressing them. Add entries only with an owner and a dated reason.
inline constexpr std::array<VuidSuppression, 0> kVuidSuppressions{};

constexpr const VuidSuppression* FindVuidSuppression(
    std::string_view vuid, std::span<const VuidSuppression> table = kVuidSuppressions)
{
    if (vuid.empty())
        return nullptr;
    for (const VuidSuppression& s : table)
    {
        if (s.Vuid == vuid)
            return &s;
    }
    return nullptr;
}

} // namespace Rendering
} // namespace GameEngine
