#pragma once

#include "Types/Types.h"

#include <optional>

namespace GameEngine {

/**
 * @brief Shared arithmetic for "can this alpha cutoff provably never discard".
 *
 * Two demotion sites ask the same question — the material registration probe
 * (MaterialSystem::ProbedFileCannotDiscard) and the FBX footprint demotion
 * (DemoteMaskMaterialsWithOpaqueUVFootprint, ModelAssetLoadFbx.cpp) — and had
 * two copies of the formula, which drifted into two copies of the same two
 * defects. It lives here once.
 *
 * The cutoff arrives from authored material data and is therefore arbitrary
 * float input, including NaN. NaN must be rejected explicitly: every ordered
 * comparison against NaN is false, so a `cutoff > kMax` guard passes it
 * through, std::clamp returns NaN, and casting NaN to uint32 is undefined
 * behaviour — in practice 0 on MSVC/x64, which produces the LOWEST possible
 * threshold and therefore demotes almost everything. The failure direction of
 * a malformed cutoff must be "refuse to demote", never "demote freely".
 */

/// Smallest 8-bit alpha value that provably never discards at `cutoff`, once
/// `marginSteps` of cooked-texture error is allowed for.
///
/// Returns nullopt when no such value exists, which is the honest answer in
/// two distinct cases and must be read as "cannot prove it, keep Mask":
///  - `cutoff` is not a finite number.
///  - the cutoff sits so high that the margin does not fit below 255. The
///    previous formula clamped the threshold to 255 here, which silently ate
///    the margin instead of admitting it had run out: at cutoff 0.99 an
///    8-step margin became a 2-step one. Refusing keeps the margin meaning
///    what it says.
std::optional<uint32> AlphaCutoffOpaqueThreshold(float cutoff, uint32 marginSteps);

} // namespace GameEngine
