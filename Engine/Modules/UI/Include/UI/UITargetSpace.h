#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Rendering
{
enum class HdrOutputMode : uint8_t;
}

namespace UI
{

class UITextureSpace;

/// Colour space of the attachment a UI render composites INTO, declared by the
/// host per RenderRG call.
///
/// The space is a property of the ATTACHMENT and its downstream consumers,
/// never of whatever display happens to be connected: a display-bound target
/// follows the active output mode, an offscreen SDR artifact stays SDR under
/// an HDR display, and a frozen pipeline output keeps the space its producer
/// rendered. It drives the SDF shader's output arm (the HDR paper-white lift)
/// and the subpixel text activation gate.
///
/// Construction is restricted to the named factories below. There is
/// deliberately no default constructor and no conversion from a raw
/// enumerator — a caller cannot state a target space by writing nothing.
class UITargetSpace
{
public:
    /// Discriminator for exhaustive switches. Nameable, but not constructible
    /// into a UITargetSpace: the converting constructor is private.
    ///
    /// `Count` is a size sentinel, never a space — no factory produces it, so
    /// a switch over a real space never sees it. It exists so every site that
    /// translates a space into something else can pin kKindCount and fail to
    /// compile when a space is added: MSVC's unhandled-enumerator warning
    /// (C4062) is OFF at /W4, so the gate cannot rest on warnings-as-errors.
    enum class Kind : uint8_t
    {
        LinearSdr,
        EncodedSrgb,
        HdrPq,
        Hlg,
        ScRgb,
        Count
    };

    /// Pin for translation sites: a `static_assert` on this next to a switch
    /// turns a newly added space into a compile error at that switch.
    static constexpr uint8_t kKindCount = static_cast<uint8_t>(Kind::Count);

    /// Display-referred linear, [0,1]; a terminal sRGB encode (manual pass or
    /// hardware-sRGB view) owns the OETF downstream. The SDR editor composite,
    /// offscreen SDR captures, test targets, SDR backbuffer direct-attach.
    static constexpr UITargetSpace LinearSdr()
    {
        return UITargetSpace(Kind::LinearSdr);
    }

    /// SDR attachment that BLENDS on raw sRGB-encoded bytes — the browser /
    /// Skia compositing model (#767): CSS paint enters undecoded, sampled
    /// linear sources are encoded at sample time, a source stamped
    /// UITextureSpace::SdrFinalized enters untouched (it already carries this
    /// curve), text coverage takes the Skia-direction retarget, and the ROP
    /// interpolates encoded values.
    /// SDR-only by construction: no HDR flavour exists and the paper-white
    /// lift never runs.
    ///
    /// Contract on the attachment itself (the host owns the texture desc, so
    /// the host enforces it): the view must NOT be an `_SRGB` format — the
    /// ROP's write-side OETF would re-encode already-encoded bytes. UNORM or
    /// float views only. Downstream, the bytes are already display-encoded:
    /// terminal passes run the Finalize contract — never a second OETF, a
    /// requantize plus whichever filters their stated FinalizeQuantizer sizes
    /// (`D(c)` into an `_SRGB` dst; a host that finalized its world BEFORE the
    /// UI names None and filters nothing) — and readbacks declare
    /// UITextureSpace::SrgbAuthored (encoded at rest — no second OETF), which
    /// is legal at readback even from a float target: readback states what
    /// the bytes HOLD, not whether the pairing would be a valid sampling
    /// registration (see UITextureSpace.h on the two predicates).
    static constexpr UITargetSpace EncodedSrgb()
    {
        return UITargetSpace(Kind::EncodedSrgb);
    }

    /// Paper-white-relative linear feeding an HDR10 PQ (or HDR10+) terminal
    /// encode: SDR-authored UI takes the paper-white lift.
    static constexpr UITargetSpace HdrPq()
    {
        return UITargetSpace(Kind::HdrPq);
    }

    /// Same lift, HLG terminal encode.
    static constexpr UITargetSpace Hlg()
    {
        return UITargetSpace(Kind::Hlg);
    }

    /// Same lift, scRGB (linear FP16) output.
    static constexpr UITargetSpace ScRgb()
    {
        return UITargetSpace(Kind::ScRgb);
    }

    /// The space of a display-bound attachment: the backbuffer itself, or a
    /// linear composite whose terminal encode presents it. Follows the active
    /// output mode; this is the ONLY place a display mode becomes a target
    /// space. Never use it for an attachment whose consumer is not the
    /// display.
    static UITargetSpace ForDisplay(Rendering::HdrOutputMode activeMode);

    /// The space of a pipeline-output attachment a HUD composites onto
    /// (Player / Game View / Scene View FinalColor): carries the PRODUCER's
    /// stamped space, never the consumer's display. `activeMode` only picks
    /// the HDR flavour when the stamp is HdrLinear — the pipeline's tonemap
    /// keyed on the same frame-stable mode. A stamp the UI cannot composite
    /// into (SrgbAuthored, or HdrLinear under an SDR mode) resolves LinearSdr
    /// with a dev-diag warning.
    static UITargetSpace ForPipelineOutput(UITextureSpace producedSpace,
                                           Rendering::HdrOutputMode activeMode);

    /// What a readback of an attachment declared in this space HOLDS. States
    /// the bytes' space explicitly — never inferred from the attachment's
    /// format: an EncodedSrgb declaration deliberately parks sRGB-encoded
    /// bytes in a float attachment, so its readback is SrgbAuthored (encoded
    /// at rest); the HDR flavours read back HdrLinear; LinearSdr reads back
    /// DisplayLinearSdr.
    UITextureSpace ReadbackSpace() const;

    constexpr Kind GetKind() const
    {
        return m_Kind;
    }

    friend constexpr bool operator==(UITargetSpace, UITargetSpace) = default;

private:
    constexpr explicit UITargetSpace(Kind kind) : m_Kind(kind) {}

    Kind m_Kind;
};

/// Human-readable space name for diagnostics.
const char* ToString(UITargetSpace space);

} // namespace UI
} // namespace GameEngine
