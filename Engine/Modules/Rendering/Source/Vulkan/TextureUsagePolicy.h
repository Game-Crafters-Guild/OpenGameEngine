#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

/// What the audit has seen so far. ViolationKeys entries are "<debugName>#<MissingBit>".
struct TextureUsageSnapshot
{
    uint64_t ObservedUses = 0;
    uint64_t UnresolvedUses = 0;
    std::vector<std::string> ViolationKeys;
};

/**
 * @brief Whether a TextureDesc's declared transfer usage is believed, and what
 *        happens when the declaration is wrong.
 *
 * TextureUsage::TransferSrc / TransferDst are the only usage bits Vulkan does
 * not enforce here, because the backend ORs
 * VK_IMAGE_USAGE_TRANSFER_SRC_BIT|TRANSFER_DST_BIT into every VkImage. A
 * declaration that omits a bit it actually uses therefore works on this backend
 * and fails only on one that validates strictly — a port, years later, far from
 * whoever wrote it.
 *
 * Two levels:
 *
 *  - AUDIT (default in developer configs) finds the drift at the use site: each
 *    transfer command compares itself against the declaration and names the
 *    textures copied, blitted or cleared without having declared it. Passive —
 *    it never changes what Vulkan is asked to do.
 *  - STRICT (opt-in, every config) drops the blanket OR, so the same drift
 *    becomes a validation error at the command instead of a log line. Deliberate
 *    and not yet a default: paths no platform has exercised would fail here
 *    first.
 */
class TextureUsagePolicy
{
public:
    /**
     * @brief Whether transfer commands check themselves against the declaration.
     *
     * On by default in developer configs (GE_DEV_DIAG) so a violation reaches
     * whoever introduces it; GE_TEXTURE_USAGE_AUDIT=0 opts out. Off otherwise,
     * where GE_TEXTURE_USAGE_AUDIT=1 opts in — the hooks stay compiled so a
     * Release build can still harvest a platform the developer configs cannot
     * reach. Disabled cost is one cached bool load per transfer command, which
     * is not a per-draw frequency. Resolved once.
     */
    static bool IsAuditEnabled();

    /**
     * @brief Whether declared transfer usage is authoritative at image creation.
     *
     * When true the Vulkan backend maps exactly the declared bits and adds
     * nothing, so a copy of an undeclared image is a validation error rather
     * than a silent success. GE_STRICT_TEXTURE_USAGE=1; off by default on every
     * config. Resolved once.
     */
    static bool IsStrict();

    /**
     * @brief Record one transfer use of a texture.
     * @param handle       Texture the command reads or writes as a transfer resource.
     * @param requiredUsage TextureUsage::TransferSrc or TextureUsage::TransferDst.
     * @param operation    Stable literal naming the command, e.g. "CopyTexture(src)".
     *
     * Every call is counted; a call whose declared usage lacks @p requiredUsage
     * is additionally recorded as a violation and logged the first time it is
     * seen for that texture. Callers must guard with IsAuditEnabled().
     */
    static void RecordTransferUse(TextureHandle handle, TextureUsage requiredUsage, const char* operation);

    /// Emit the harvested table. Safe to call when disabled (emits nothing).
    static void Report();

    /// Current tallies. Lets a test separate "nothing violated" from "hook never ran".
    static TextureUsageSnapshot Snapshot();
};

} // namespace Rendering
} // namespace GameEngine
