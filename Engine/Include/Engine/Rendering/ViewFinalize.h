#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/FinalizeContract.h"
#include "UI/UITextureSpace.h"

#include <optional>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class RenderServices;

/// The deband gate a view's finalize sizes its filter to, in output LSBs: the
/// primary world's resolved PostProcessVolume lever (opt-in, 0 without one).
/// Environment overrides are applied inside the pass.
///
/// One resolution for every view, which is what the terminal encode did before
/// the filters moved into the views — a per-view volume lookup becomes worth
/// doing the moment a view can show a world the primary one is not.
float ResolveViewDebandThresholdLsb(RenderServices* services);

/// The finalize contract this policy handed the encode pass — the operands
/// that decide the pass's arm and the step it sizes its filters to
/// (FinalizeContract.h), published because they are the policy's decision and
/// nothing downstream can recover them from the image.
struct ViewFinalizeStep
{
    ::GameEngine::Rendering::Passes::FinalizeInputSpace InputSpace;
    ::GameEngine::Rendering::Passes::FinalizeQuantizer Quantizer;
    /// The format the Presented quantizer sized its filters to — the third
    /// operand of the same contract, published for the same reason the first
    /// two are. Unknown is the honest "no presented surface" statement and
    /// resolves through the format matrix's 8-bit row
    /// (EncodeDitherLsbForFormat).
    ::GameEngine::Rendering::TextureFormat PresentedFormat;
};

/// What a per-view finalize produced, and what its bytes hold.
///
/// `Image` is always sampleable: when the finalize declines it is the caller's
/// own input, so there is no "finalized or nothing" state for a caller to get
/// wrong. `Space` is the producer's stamp for whichever image came back, never
/// inferred downstream from the format (an F16 target holds encoded code values
/// on the finalized path and linear light on the declining one).
///
/// `Step` is engaged exactly when a pass was declared. Absent on every
/// declining path, because there is no encode step to describe there and an
/// "unset" enumerator would be indistinguishable from a real choice — a caller
/// asking what the finalize decided must confront the case where it decided
/// nothing.
struct ViewFinalizeResult
{
    ::GameEngine::Rendering::RenderGraph::RGTexture Image{};
    ::GameEngine::UI::UITextureSpace Space;
    std::optional<ViewFinalizeStep> Step;
};

/// Whether world views finalize themselves this frame — and therefore whether
/// the window's UI composites in the encoded blend space.
///
/// These are ONE decision, which is why they read one predicate: a finalized
/// view carries the sRGB curve, so a linear-blending composite would treat its
/// code values as light and wash the viewport out. SDR only, deliberately: the
/// HDR arms keep the linear composite and their single terminal OETF, because a
/// PQ blend variant and a PQ-calibrated text-coverage retarget are their own
/// slice. Under HDR the terminal pass therefore keeps owning the transfer
/// function and its dither, full-frame.
bool ViewFinalizeEligible(::GameEngine::Rendering::IDevice* device);

/// Declare a world view's own finalize: sRGB encode, the opt-in deband, and a
/// TPDF dither at the PRESENTED surface's true LSB, into a fresh image the UI
/// then composites. The rounding onto code values stays where it always was —
/// in the store that writes the presented surface — so what moves here is the
/// FILTERING, from a pass that could not tell world from chrome to one that only
/// ever sees world.
///
/// Declared at the view's own resolve — after everything that composes the view,
/// the editor's overlay and gizmo passes included, and before any UI touches it.
/// That position is what makes the filters world-only by construction: the
/// texture the deband taps IS the world, so its taps clamp to the view's edge
/// and can never reach chrome, and the dither only ever perturbs pixels that
/// have a staircase to break.
///
/// "Before any UI touches it" is not only sampling: the game view's HUD
/// COMPOSITES INTO the returned image (#767 slice iv), which is what puts HUD
/// pixels on encoded bytes. The world-only guarantee is unaffected — the filters
/// ran to completion before that write — but a caller must not assume the
/// returned image still holds world pixels alone once it has handed it on.
///
/// The dither lands at the view's OUTPUT extent because the render-scale
/// crossing sits upstream of the whole post chain — a resample after the dither
/// would low-pass the noise and then re-round the recovered detail undithered.
///
/// `pipelineSpace` is the stamp for the unfinalized image, returned unchanged
/// when the finalize declines (HDR active, invalid input, shaders unstaged).
///
/// `presentedFormat` is REQUIRED for the same reason the pass contract's
/// `inputSpace` and `quantizer` are (FinalizeContract.h): a new call site must
/// decide, never inherit silently. It is the swapchain format of the window
/// this view presents into, and the owner resolves it PER FRAME from its own
/// window-target handle (IDevice::GetWindowTargetSwapchainFormat) — never
/// cached in a member, because a swapchain recreate (monitor move, HDR
/// toggle) between frames would leave a cached value sizing the dither to a
/// surface that no longer exists. Unknown states "no presented surface" and
/// resolves through the format matrix's 8-bit row.
///
/// `hostRefusal` is the caller's own reason this frame keeps the linear chain,
/// stated to the policy rather than expressed by branching around the call —
/// the whole decline decision lives here, and a declined frame still gets the
/// (image, space) hand-off contract above. True declines exactly as an
/// ineligible device does: the caller's image and stamp come back unchanged
/// and no pass is declared. Required, same rule as `presentedFormat`: a call
/// site with no refusal of its own says so explicitly. The one real refusal is
/// a frame recording an HDR movie: that capture's encode carries a PQ/HLG
/// `encodeOverride`, and an already-encoded source under it is a Finalize
/// contract violation the pass refuses outright.
///
/// Cost is NOT a refusal reason. "Nothing composites, so the linear chain pays
/// nothing" prices the encode and ignores the filters: a linear source at the
/// terminal selects the passthrough arm, which zeroes the dither AND the
/// deband, so declining hands that frame no filters at all on a hardware-sRGB
/// swapchain. Decline for frames a finalize would make WRONG, never for frames
/// it would merely cost.
ViewFinalizeResult DeclareViewFinalize(::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                                       ::GameEngine::Rendering::RenderGraph::RGTexture viewColor,
                                       ::GameEngine::UI::UITextureSpace pipelineSpace,
                                       const char* viewName, float volumeDebandThresholdLsb,
                                       ::GameEngine::Rendering::TextureFormat presentedFormat,
                                       bool hostRefusal);

} // namespace GameEngine::Engine::Renderer
