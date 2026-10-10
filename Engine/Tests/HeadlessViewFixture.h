#pragma once

// Headless view fixture — a real Vulkan device, a real RenderServices spine and
// a real world view rendered without a window, handing back the raw bytes of a
// destination texture plus the operands a byte gate needs to reason about them.
//
// ── ZERO DEVICE OVERRIDES ────────────────────────────────────────────────────
// Nothing here subclasses VulkanDevice and nothing fakes
// GetSwapchainTextureFormat(). The amplitude arm under test is
// FinalizeQuantizer::Destination, which sizes the dither from the DESTINATION
// render-graph resource's declared format and reads no device at all
// (SRGBEncodePass.cpp: the Destination case falls through to
// EncodeDitherLsbForFormat(dstFormat), where dstFormat comes from
// frame.Graph().ResourceDesc(dst.Id).Format on every non-backbuffer target, and
// a headless frame has no backbuffer). A real RGB10A2_UNORM destination
// therefore exercises the 10-bit amplitude end-to-end on the GPU without the
// fixture being able to perturb what it measures.
//
// ── The PRESENTED DEPTH is reachable on BOTH arms, by supplied format ────────
// The host's per-view finalize (Engine/Source/Engine/Rendering/ViewFinalize.cpp,
// DeclareViewFinalize) names FinalizeQuantizer::Presented and takes the
// presented format as a REQUIRED parameter, which it always forwards ENGAGED
// to the pass — in production the window owner resolves it per frame from its
// own window-target handle (IDevice::GetWindowTargetSwapchainFormat). Here the
// HostPolicy arm forwards HeadlessViewDesc::PresentedFormat (Unknown when
// unset), so an ABSOLUTE Presented amplitude is assertable on either arm the
// moment the desc supplies the format: 1/1023 for a supplied RGB10A2, with no
// device double anywhere — a double would make the render graph's
// attachment-less pipeline-format-key fallback reachable on every GPU-driven
// prologue pass in the frame, so it stays excluded by construction.
//
// An UNSET PresentedFormat still measures the no-surface row: Unknown reaches
// EncodeDitherLsbForFormat's 8-bit default, 1/255 whatever the destination.
// That is no longer a collapse this fixture must characterize around — it is
// the stated contract for a caller with no presented surface, and the gates
// that once pinned the collapse now pin the supplied-format path instead
// (HeadlessViewFixtureTests.cpp).
//
// What IS covered on the host-policy arm: the contract it published (Facts()
// .Step, which the POLICY sets — not the fixture), the space it stamped, the
// format it pinned, and the dither amplitude its pass actually applied,
// measured off the returned F16 image. An F16 destination has no rounding step
// of its own, so that arm resolves an amplitude more sharply than the UNORM
// arms do, not less.
//
// ── LIMIT: deband is pinned OFF by design ────────────────────────────────────
// The suite pins GE_DEBAND=0 at process load and every desc leaves
// VolumeDebandThresholdLsb at 0, so nothing here exercises the gradient-aware
// deband: a green suite says the encode and the dither are right, NOT that the
// anti-banding machinery works. The exclusion is what keeps the CPU models
// exact — a nonzero gate turns the encode into a 4-tap neighbourhood filter no
// model in this fixture reproduces — so deband gates need their own arm with a
// neighbourhood-aware model, not a relaxation of this one.
//
// ── THE TRANSPORT STAGE (I4) ─────────────────────────────────────────────────
// With HeadlessViewDesc::Transport set, AddPixelPerfectUpscalePassRG is
// declared after the finalize, reading the finalize's destination and writing a
// larger destination whose surplus is letterboxed. Both images are read back in
// the SAME submission, so the gate compares transported bytes against that
// frame's own pre-transport bytes through the index map below — there is no
// checked-in golden anywhere in this, and there is nothing for one to be
// regenerated from.
//
// LIMIT: the HDR arms of that pass are unreachable here. Its refusal is keyed
// on frame.IsBackbuffer(Dst), which is false for every destination a headless
// frame can create, so the encoded-under-HDR refusal at
// PixelPerfectUpscalePass.cpp:171-186 cannot be reached from this fixture and
// no gate here may claim to cover it.
//
// ── SHAPE C: the S1 overlay writer is SYNTHETIC, by necessity ────────────────
// The production S1 writer (SceneViewOverlaysRG) is editor-executable code and
// cannot link in this target, so HeadlessViewDesc::Overlay is a synthetic
// stand-in: an opaque region copy into the view colour, declared after the
// content injection and before the finalize. That is sound for what shape C's
// pin asserts — WHERE pixels landed relative to the overlay's boundary and
// relative to the finalize in the schedule — because an opaque writer has one
// possible semantics and nothing for a model to self-fit. It cannot gate how
// the production overlay pass computes its pixels, and no gate here claims to.
//
// ── SHAPE B: the HUD composite is the REAL UI compositor ────────────────────
// HeadlessViewDesc::Hud stands up a real UIManager over the fixture's device
// and declares UIManager::RenderRG onto the finalize's destination with
// RGLoadOp::Load — the production Game View / Player composite shape. The
// target space is NOT a fixture literal: it is computed by the production
// UITargetSpace::ForPipelineOutput from the space the frame actually handed
// off (on the HostPolicy arm, the policy's own published stamp), so a policy
// that stamps the wrong space steers the real compositor into the wrong blend
// arm and the pixel gates see it. A fixture-side synthetic blend here would be
// a self-fit — the blend-space models would score the fixture's own
// arithmetic — which is why this is the one stage that pays for the real
// compositor. PreHud() is the same submission's readback of the destination
// between the finalize and the UI pass (the scheduler orders it by the
// recorded read-before-write hazard), so every composite expectation is
// same-frame relational; when Hud is set, Handoff() holds the COMPOSITE.

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewFinalize.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Passes/FinalizeContract.h"
#include "UI/UITargetSpace.h"
#include "UI/UITextureSpace.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
class UIManager;
namespace UI
{
class IPlatformApi;
}
} // namespace GameEngine

namespace GameEngine::Testing
{

/// Why a stand-up or a render could not proceed. The three absences are
/// deliberately distinguishable because the chain contract handles them
/// differently: a missing device and a missing staged asset are defects and
/// must FAIL; a format the running GPU genuinely cannot render is not a defect
/// and must SKIP, named.
///
/// Why that last class SKIPs rather than FAILS, which is a real decision and
/// not an oversight: what these gates assert is a RELATION between a
/// destination's depth and the dither sized for it. On an adapter that cannot
/// render RGB10A2_UNORM as a colour attachment there is no ten-bit destination
/// for the relation to hold between, so the assertion has no subject and a
/// failure would report an engine defect for a fact about the hardware. The
/// two classes that ARE fixable where the suite runs — a device that should
/// have come up, a shader package that should have been staged — keep failing.
/// The skip cannot hide: it names the format and the usage bits, and the probe
/// it rests on is itself gated (the instrument-check test proves that probe can
/// still answer NO, so an unconditional yes-man cannot make every 10-bit
/// assertion nominally universal). The cost accepted is that a fleet on which
/// no adapter had the format would report green having measured nothing; that
/// is answered by keeping one known-capable adapter in the matrix, not by
/// failing developers for their GPUs.
enum class HeadlessViewStatus : uint8_t
{
    Ok,
    NoDevice,           // FAIL — a device that should have been present is not
    StagedAssetMissing, // FAIL — staging is part of the test
    FormatUnsupported   // SKIP, loudly and by name
};

const char* ToString(HeadlessViewStatus status);

/// Which code declares the finalize step under test.
enum class FinalizeDriver : uint8_t
{
    /// The fixture declares AddSRGBEncodePassRG itself, so the destination
    /// format and quantizer in the desc are the ones the pass sees. This is the
    /// arm that reaches a real 10-bit destination.
    PassDirect,
    /// The extracted host policy (Engine::Renderer::DeclareViewFinalize) runs
    /// verbatim. The destination format is NOT the fixture's to choose here —
    /// the policy pins it to the source's — so Desc::DestinationFormat is
    /// ignored and Facts().DestinationFormat reports what the graph actually
    /// declared.
    HostPolicy
};

/// The transport step under test: PixelPerfectUpscalePass, declared after the
/// finalize, reading the finalize's destination and writing a larger one.
///
/// The pass's contract sizes its source at (Reference + 2) texels per axis, so
/// the reference extent is DERIVED from the finalize destination and is
/// deliberately not a field here. A desc able to name a reference size is a
/// desc able to name one the pass does not agree with, and the index map would
/// then be wrong in the same direction as the render — which is precisely the
/// shape of gate that cannot fail.
struct TransportDesc
{
    /// Must exceed Reference * Zoom in both axes or there are no letterbox
    /// pixels and the letterbox assertion has no subject.
    uint32_t DestinationWidth = 0;
    uint32_t DestinationHeight = 0;
    Rendering::TextureFormat DestinationFormat = Rendering::TextureFormat::R16G16B16A16_FLOAT;

    /// Zoom >= 2 and a Frac keeping every sample clear of an integer texel
    /// boundary — zoom 1 with frac 0 lands every sample on an exact texel
    /// CENTRE, where a bilinear sampler returns the same value a nearest one
    /// does, so such an arm is blind to the mutation this stage exists to
    /// catch. TransportGeometry publishes both distances so the rule is
    /// asserted rather than asserted-about-in-prose.
    uint32_t Zoom = 2;
    /// Sub-texel camera remainder, range [-0.5, 0.5] per axis.
    float FracX = 0.0f;
    float FracY = 0.0f;

    /// EncodedSrgb is the production shape: the finalize already applied the
    /// OETF, so the transport must apply no transfer of its own — raw bytes
    /// into a UNORM or float destination, D(c) into an _SRGB one.
    Rendering::Passes::FinalizeInputSpace InputSpace =
        Rendering::Passes::FinalizeInputSpace::EncodedSrgb;
};

/// Shape C's synthetic S1 writer: an opaque rect of one linear colour copied
/// into the view colour AFTER the content injection and BEFORE the finalize —
/// the position the chain contract's S1 stage occupies (I10: overlays are
/// writers into FinalColor, upstream of the finalize). Synthetic by necessity:
/// the production overlay writer is editor-executable code and cannot link in
/// this target. Its pins are therefore contract-executable — where the pixels
/// landed and where the pass sits in the schedule — and their red arm at step 0
/// is a LOCAL FIXTURE MUTATION (declare this after the finalize), not a
/// production one; the tests that consume this say so in their own comments.
struct OverlayDesc
{
    uint32_t X = 0;
    uint32_t Y = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
    /// Opaque overlay ink, linear light — upstream of the finalize's OETF like
    /// every other S1 write.
    float LinearRgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

/// Shape B's HUD: the REAL UI compositor declared over the finalize's
/// destination. XML becomes the root tree, CSS the one stylesheet; layout runs
/// at content scale 1 over the destination's extent, so CSS px == destination
/// px. The blend space is decided by production code from the frame's own
/// hand-off stamp (see the header block above), never named here — a desc able
/// to name a blend space would be a desc able to name the one the models then
/// score, which is the self-fit this fixture exists to rule out.
struct HudDesc
{
    std::string Xml;
    std::string Css;
};

/// What the HUD declare resolved, read back from production publishes — not
/// restatements of fixture inputs.
struct HudFacts
{
    /// UITargetSpace::ForPipelineOutput(handed-off space, device HDR mode) —
    /// the production consumption of the (image, space) pair. On the
    /// HostPolicy arm the space operand is the policy's own published stamp,
    /// so this fact moves when ViewFinalize.cpp stamps the wrong space.
    std::optional<UI::UITargetSpace> TargetSpace;
    /// UIManager::GetLastResolvedOutputEncoding() after the declare — the SDF
    /// shader convention (1 = SDR encoded bytes). -1 when no HUD was declared.
    int ResolvedOutputEncoding = -1;
    bool TextSubpixelActive = false;
};

struct HeadlessViewDesc
{
    uint32_t Width = 128;
    uint32_t Height = 128;

    /// Colour the spine's world pass clears the view to, in linear light.
    float ClearLinear[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    /// Authored linear RGBA (4 floats per pixel, row-major, Width*Height
    /// entries) copied over the view's colour AFTER the spine's world pass and
    /// BEFORE the finalize — the fixture's stand-in for drawn geometry, and
    /// upstream of everything under test. Empty leaves the clear standing, which
    /// gives a flat field and a much weaker instrument: a flat value can sit on
    /// an exact code and hide a mis-sized dither entirely.
    std::vector<float> ContentLinearRgba;

    FinalizeDriver Driver = FinalizeDriver::PassDirect;

    /// HostPolicy only: the host-refusal operand forwarded to
    /// DeclareViewFinalize. True models the frame a host keeps on the linear
    /// chain for its own reasons (an HDR movie capture; a HUD-less Player
    /// frame) — the policy must decline exactly as an ineligible device does,
    /// handing back the caller's own image and stamp with no step published.
    bool HostRefusal = false;

    /// PassDirect only. RGB10A2_UNORM is the arm this fixture exists for.
    Rendering::TextureFormat DestinationFormat = Rendering::TextureFormat::RGB10A2_UNORM;

    /// The format FinalizeQuantizer::Presented sizes its filters to
    /// (SRGBEncodePass.h documents the parameter). Both drivers honour it:
    /// PassDirect hands the optional to the pass verbatim, and HostPolicy
    /// forwards it through DeclareViewFinalize's required parameter — Unknown
    /// when unset, the same "no presented surface" statement a production
    /// caller with no window resolves.
    ///
    /// Unset therefore measures the format matrix's Unknown row (1/255
    /// whatever the destination); a supplied format reaches the true presented
    /// step on either arm. An ABSOLUTE Presented amplitude expectation may
    /// only be formed on an arm that supplies this, or on
    /// FinalizeQuantizer::Destination — an unset Presented arm's 1/255 is the
    /// no-surface row, not the depth of any real display.
    std::optional<Rendering::TextureFormat> PresentedFormat;
    Rendering::Passes::FinalizeInputSpace InputSpace = Rendering::Passes::FinalizeInputSpace::Linear;
    Rendering::Passes::FinalizeQuantizer Quantizer = Rendering::Passes::FinalizeQuantizer::Destination;

    /// The world's resolved deband gate handed to the pass. 0 keeps the filter
    /// off, which keeps the encode a per-pixel function and therefore keeps the
    /// CPU model exact; a nonzero value turns the model into a 4-tap
    /// neighbourhood filter the scorers below do NOT reproduce.
    float VolumeDebandThresholdLsb = 0.0f;

    /// Shape C. Declared between the content injection and the finalize.
    std::optional<OverlayDesc> Overlay;

    /// Shape B. Declared after the finalize (and after the PreHud tap), before
    /// the transport — so a Transport on the same desc samples the COMPOSITE.
    std::optional<HudDesc> Hud;

    /// Absent leaves the frame exactly as it was before the transport stage
    /// existed: no upscale pass is declared and TransportOutput() stays empty.
    std::optional<TransportDesc> Transport;
};

/// Where one transport destination pixel's colour must have come from.
struct TransportIndex
{
    /// Outside the centered upscaled rect: the pass owes this pixel black.
    bool Letterbox = true;
    /// The source texel a mapped pixel must be a copy of.
    uint32_t SourceX = 0;
    uint32_t SourceY = 0;
    /// The continuous source-texel coordinate before the nearest snap. Kept so
    /// the 1/32-texel rule is a measurement over real samples, not a claim.
    float SampleX = 0.0f;
    float SampleY = 0.0f;
    /// The snap needed the sampler's clamp-to-edge to stay in range. The padded
    /// source exists so this never happens; if it does, the sample window has
    /// walked off the border and "a copy of exactly one source texel" is being
    /// satisfied by the clamp rather than by the mapping.
    bool Clamped = false;
};

/// The transport's geometry and the statistics a gate needs to know its arm is
/// not vacuous. Every field is derived from the desc, NEVER read back from the
/// pass's push constants: a map recovered from the values the pass computed
/// would move with the pass under exactly the origin-math mutation the byte
/// gate exists to catch.
struct TransportGeometry
{
    uint32_t SourceWidth = 0;
    uint32_t SourceHeight = 0;
    uint32_t ReferenceWidth = 0;
    uint32_t ReferenceHeight = 0;
    uint32_t DestinationWidth = 0;
    uint32_t DestinationHeight = 0;
    uint32_t Zoom = 1;
    float FracX = 0.0f;
    float FracY = 0.0f;
    uint32_t OutputOriginX = 0;
    uint32_t OutputOriginY = 0;

    std::size_t MappedPixels = 0;
    std::size_t LetterboxPixels = 0;
    std::size_t ClampedSamples = 0;

    /// Minimum over every mapped sample and both axes of the distance from the
    /// continuous source-texel coordinate to the nearest INTEGER boundary. Near
    /// zero the nearest snap is decided by float noise and the index map stops
    /// being a prediction. Vulkan guarantees only 8 fractional bits in texel
    /// address computation, so the usable floor is 1/256 and the rule is 1/32.
    float MinDistanceToTexelBoundary = 0.0f;
    /// Minimum over every mapped sample and both axes of the distance to a
    /// texel CENTRE (fractional part 0.5). At zero a bilinear sampler collapses
    /// onto the single texel a nearest sampler picks, so the sample cannot
    /// witness a filtering sampler at all — the zoom-1/frac-0 degeneracy.
    float MinDistanceToTexelCentre = 0.0f;
};

/// Build the index map for a transport of `sourceWidth x sourceHeight` into
/// `destinationWidth x destinationHeight` at `zoom` with sub-texel remainder
/// (`fracX`, `fracY`), writing one entry per destination pixel in row-major
/// order and returning the geometry.
///
/// This states the pass's PUBLISHED contract — a reference-sized window out of
/// a source padded by one texel per side, point-sampled and blitted at integer
/// zoom into a centered rect, the surplus black — from the caller's parameters
/// alone. It never reads the pass, its shader or its push constants, which is
/// what leaves it standing still while a mutation of the origin math moves the
/// render.
TransportGeometry BuildTransportIndexMap(uint32_t sourceWidth, uint32_t sourceHeight,
                                         uint32_t destinationWidth, uint32_t destinationHeight,
                                         uint32_t zoom, float fracX, float fracY,
                                         std::vector<TransportIndex>& outMap);

/// The hand-off: bytes and the space stamp as ONE value, so a gate cannot
/// compare the pixels while forgetting what they claim to hold (chain rule 2).
struct ViewOutputBytes
{
    std::vector<uint8_t> Bytes;
    uint32_t Width = 0;
    uint32_t Height = 0;
    /// Read from the readback, i.e. from the resource the graph declared — not
    /// from the desc. This is what defines the LSB unit the scorers report in.
    Rendering::TextureFormat Format = Rendering::TextureFormat::Unknown;
    std::optional<UI::UITextureSpace> Space;
};

/// One operand of the step under test, decoded to float at full precision.
struct ViewStageTap
{
    std::vector<float> Rgba; // 4 per pixel
    uint32_t Width = 0;
    uint32_t Height = 0;
    Rendering::TextureFormat Format = Rendering::TextureFormat::Unknown;
    std::optional<UI::UITextureSpace> Space;
};

/// What the finalize step was told to do, and what the fixture predicts it did.
///
/// The first three fields are GROUND TRUTH. `Step` is the contract the pass was
/// actually given: the desc's own values on the PassDirect arm, where the
/// fixture declares the pass and therefore owns the answer, and the host
/// policy's PUBLISHED decision (ViewFinalizeResult::Step) on the HostPolicy
/// arm, where it is emphatically not the fixture's to state — a fixture-side
/// literal there makes any gate over it a comparison between two test-side
/// constants, which is a gate that cannot fail whatever the policy does. It is
/// absent when the host policy declined to declare a pass at all, so "chose
/// Presented" and "chose nothing" stay distinguishable. DestinationFormat is
/// read back off the graph resource rather than off the desc, so a driver that
/// pins its own format cannot leave this disagreeing with reality.
///
/// The Predicted* fields are the fixture's MODEL of the pass, not a publish
/// from it — GetBackbufferEncodeFacts() is backbuffer-gated and is permanently
/// dead headless. They are safe to model only because every device-reading
/// branch of the selector is unreachable on a non-backbuffer destination, which
/// leaves one branch standing. They must never be asserted against the same
/// function that produced them; what makes a wrong PredictedOutEncoding
/// observable is the three-hypothesis compare, which elects a different winner
/// when the pass takes a different arm.
///
/// That compare does NOT do the same for PredictedDitherLsb: it is amplitude-
/// BLIND, because its three models differ by a whole transfer function while an
/// amplitude error moves the winner by a fraction of one code. Measured on this
/// fixture — a 4x OVERSIZED dither on the ten-bit arm (the RGB10A2_UNORM row
/// dropped from EncodeDitherLsbForFormat, i.e. the production regression this
/// suite exists to catch) moved the winning score 0.031311 -> 0.0324707 against
/// a 1.0 threshold and a 20x separation margin; a 2x undersize moved it to
/// 0.0335083. Three orders of magnitude of headroom absorb the error whole and
/// the compare stays green. Amplitude is MeasureDitherAmplitude()'s business,
/// which recovers it from the pixels rather than predicting it.
struct FinalizeFacts
{
    std::optional<Engine::Renderer::ViewFinalizeStep> Step;
    Rendering::TextureFormat DestinationFormat = Rendering::TextureFormat::Unknown;
    Rendering::TextureFormat SourceFormat = Rendering::TextureFormat::Unknown;

    int32_t PredictedOutEncoding = 0;
    float PredictedDitherLsb = 0.0f;
    float PredictedDebandThresholdEncoded = 0.0f;
};

// ── The unit every statistic below reports in ────────────────────────────────
// ENCODED [0,1] — the units of the shader's own pc.ditherLsb and of a UNORM
// destination's value before its code scale is applied. One unit for every
// destination, chosen because the arms differ in whether they HAVE a code at
// all: an F16 target quantizes at no step, so a number reported "in LSBs"
// there would be a number in units of nothing. A caller wanting destination
// LSBs multiplies by DestinationCodeScale(), which is a fixture-side property
// of the format and not a reading of the production amplitude table — a
// statistic scaled by the table could not be used to gate the table.

/// Mean |observed - model| over every pixel and channel, in encoded units.
struct HypothesisScores
{
    double EncodeOnce = 0.0;
    double LinearNoEncode = 0.0;
    double DoubleEncode = 0.0;
};

struct ResidualStats
{
    double MeanEncoded = 0.0;
    double MaxEncoded = 0.0;
    /// Sample standard deviation of the SIGNED displacement (observed minus
    /// undithered model), mean removed. The reference-free dither-presence
    /// statistic: a 1-LSB TPDF plus the destination's rounding gives
    /// sqrt(1/6 + 1/12) = 0.5 destination LSB; rounding alone gives
    /// sqrt(1/12) = 0.2887, and that is what ANY near-constant pattern
    /// collapses to, whatever DC offset it carries (the mean removal is what
    /// guarantees that). Nothing here reads TriangularDitherRef or the
    /// production amplitude table, so no shared-pattern transcription can
    /// satisfy it — the degeneracy class the amplitude projection is
    /// structurally blind to.
    double StdEncoded = 0.0;
    std::size_t Samples = 0;
};

/// The dither amplitude the GPU ACTUALLY applied, recovered from the pixels.
///
/// A max-displacement statistic cannot serve as an amplitude gate: it sums the
/// dither with the destination's own rounding, and rounding alone already
/// reaches 0.5 destination LSB, so a 4x undersized dither still lands at ~0.76
/// and any threshold placed under it is a threshold placed under the defect.
/// This is the separation instead — the observed displacement projected onto
/// the shader's own TPDF pattern, which rounding is uncorrelated with and
/// therefore enters as variance rather than as offset.
///
/// THREE ESCAPE CLASSES the projection does not close:
/// - A pattern the shader and TriangularDitherRef SHARE. The slope divides the
///   pattern's shape out, so it recovers the amplitude scalar for any common
///   pattern — including a constant and the collapsed near-constant IGN
///   difference encode_srgb.frag records as having shipped once. Both read
///   ~1.0 here. That class is gated by ResidualStats::StdEncoded, which never
///   references the pattern (0.5 LSB healthy, 0.2887 for every degenerate
///   variant).
/// - A spatially NON-UNIFORM amplitude that averages to 1.0 (e.g. modulating
///   ~0.7x-1.3x across the image). The slope reads the mean amplitude, and the
///   spread moves only quadratically (0.505 for that range, inside any usable
///   band), so neither statistic pins it. Named and accepted: no gate here
///   claims per-pixel amplitude uniformity.
/// - A BADLY SHAPED but still-spread IGN difference. The first class collapses
///   only when 52.9829189 * dot(IGN's gradient constants, (delta, 1 - delta))
///   lands near an integer; land it near a mid-range fraction f instead and the
///   difference becomes a two-level wrap pattern. For f(1-f) in [0.12, 0.22] —
///   roughly 41% of arbitrary deltas — the total spread falls inside
///   StdEncoded's [0.45, 0.55] band while a resynced reference still projects
///   to ~1.0, so both gates read green. Reachable from the same bug family that
///   shipped, and NOT closed here. Two things keep it off the blocker list: it
///   needs the reference resynced to the broken shader (without that the slope
///   catches every member — 0.707 at f = 0.25), and the surviving patterns
///   still carry >= 0.34 LSB of genuine spread, i.e. badly shaped dither rather
///   than the spreadless defect that actually shipped.
struct DitherAmplitude
{
    /// Least-squares slope of (observed - undithered model) against
    /// TriangularDitherRef, in encoded units. Equals pc.ditherLsb when the
    /// fixture's dither reference is in phase with the shader's. 0 when no
    /// dither ran.
    double Encoded = 0.0;
    /// Correlation between that displacement and the TPDF pattern. A
    /// DIAGNOSTIC, not a criterion, in both directions: it necessarily falls
    /// as the amplitude approaches the destination's rounding noise (~0.82 for
    /// a correctly sized dither into a UNORM target, ~1 into a float one), so
    /// it cannot take a lower bound — and it stays HIGH (0.77-0.87 measured)
    /// when shader and reference share a degenerate pattern, so it cannot gate
    /// that class either. Its one diagnostic use: near 0 alongside a small
    /// Encoded but a HEALTHY StdEncoded says the dither is running and the
    /// reference has fallen out of phase with the shader; with StdEncoded
    /// degenerate too, the dither itself is off.
    double Correlation = 0.0;
    std::size_t Samples = 0;
};

class HeadlessViewFixture
{
  public:
    // Both out-of-line: Pools is incomplete here, so the unique_ptr's deleter
    // must be instantiated where it is not.
    HeadlessViewFixture();
    ~HeadlessViewFixture();
    HeadlessViewFixture(const HeadlessViewFixture&) = delete;
    HeadlessViewFixture& operator=(const HeadlessViewFixture&) = delete;

    /// Device + RenderServices + camera + view. Nothing here can report
    /// FormatUnsupported: the destination format is a property of a render, not
    /// of the stand-up, so the capability question is asked in Render().
    HeadlessViewStatus Up();
    void Down();

    /// Declare and execute one frame: the real spine, the content injection,
    /// the finalize step, and the two readbacks. Every accessor below refers to
    /// the most recent successful Render.
    HeadlessViewStatus Render(const HeadlessViewDesc& desc);

    /// The last status message, naming the missing device / asset / capability.
    const std::string& StatusMessage() const { return m_StatusMessage; }

    /// The finalize destination's final contents this frame — the COMPOSITE
    /// when the desc carried a Hud.
    const ViewOutputBytes& Handoff() const { return m_Handoff; }
    /// The destination between the finalize and the HUD composite, read back
    /// in the same submission (scheduled there by the recorded
    /// read-before-write hazard — the tests assert the schedule rather than
    /// trusting this sentence). Empty when the desc carried no Hud.
    const ViewOutputBytes& PreHud() const { return m_PreHud; }
    const HudFacts& HudResolved() const { return m_HudFacts; }
    /// The live manager from the last Hud render — for element-layout queries.
    /// Null when the last desc carried no Hud.
    UIManager* HudManager() { return m_Hud.get(); }
    /// The finalize's ACTUAL input, tapped in the same submission as the
    /// hand-off so no temporal term can decorrelate them.
    const ViewStageTap& SourceBeforeFinalize() const { return m_Source; }
    const FinalizeFacts& Facts() const { return m_Facts; }

    /// The transport's output, read back in the same submission as Handoff() —
    /// which is therefore this frame's own pre-transport image, and the only
    /// reference the byte gate compares against. Empty when the desc carried no
    /// transport.
    const ViewOutputBytes& TransportOutput() const { return m_Transport; }
    /// One entry per TransportOutput() pixel, row-major.
    const std::vector<TransportIndex>& TransportIndexMap() const { return m_TransportIndex; }
    const TransportGeometry& TransportShape() const { return m_TransportShape; }

    /// Pass names declared in the last frame, in graph order — lets a gate
    /// assert the real spine ran rather than assuming it. Query with
    /// Testing::RGQuery::CountIn / IndexIn (exact/subtree/family — never
    /// substring: pass names gain segments, and a substring needle absorbs
    /// siblings).
    const std::vector<std::string>& PassNames() const { return m_PassNames; }

    /// Pass names in the graph's SCHEDULED order, culled passes excluded.
    /// Presence here is evidence a pass ran; presence in PassNames() is only
    /// evidence one was declared, and a culled pass appears in both.
    const std::vector<std::string>& ScheduledPassNames() const { return m_ScheduledPassNames; }

    /// Chain rule 3. Reads the hand-off, the source tap and the facts from the
    /// fixture's own last frame and takes NO caller-supplied source: a caller
    /// able to supply one is a caller able to supply a wrong one and still be
    /// handed a winner. Scores are mean |Δ| in encoded units. Amplitude-blind —
    /// see FinalizeFacts.
    HypothesisScores ScoreEncodeHypotheses() const;

    /// |observed - undithered encode-once model| in encoded units: the TOTAL
    /// displacement the pass produced, dither and destination rounding
    /// together. Not an amplitude — the two mechanisms are summed here and
    /// rounding alone reaches half a code, which is why separating them needs
    /// MeasureDitherAmplitude(). What this bounds is everything else: an offset
    /// or a wrong curve moves it and leaves the amplitude untouched. StdEncoded
    /// is the paired presence check: it answers "is a dither-shaped spread in
    /// the pixels at all", which the projection cannot (see DitherAmplitude's
    /// escape classes).
    ResidualStats EncodeOnceResidual() const;

    /// The dither amplitude the pass applied, separated from the destination's
    /// rounding. Reads only the fixture's own last frame; the format table
    /// plays no part, so the result is usable to gate that table.
    DitherAmplitude MeasureDitherAmplitude() const;

    /// Chain rule 4 — a diagnostic, never a criterion. Writes a PPM of
    /// |observed - UNDITHERED encode-once model|, 32 bytes of brightness per
    /// destination LSB of displacement, saturating at 255 (~8 LSB), and
    /// returns its path (empty on failure). The model must stay undithered:
    /// the healthy dither renders as uniform grain (~13 bytes mean), and a
    /// mis-sized one as anomalous brightness — a model that folded the
    /// PREDICTED dither in would track the production table under a
    /// wrong-amplitude mutation and render clean.
    std::filesystem::path WriteResidualMap(const std::string& tag) const;

    Rendering::IDevice* Device() { return m_Device.get(); }
    Engine::Renderer::RenderServices& Services() { return *m_Services; }
    Rendering::ViewId View() const { return m_View; }

    /// The usage bits the destination is created with — the set a capability
    /// probe must ask about. IsTextureFormatSupported(fmt, 0) returns true
    /// unconditionally, so a probe with no usage bits is a probe that cannot
    /// fail.
    static std::uint32_t DestinationUsageFlags();

    /// The usage bits the finalize destination is created with when a transport
    /// stage will SAMPLE it. Sampling is its own capability: a probe asking only
    /// about the render-target bits could never skip, and a device unable to
    /// sample the format would fail somewhere inside the pass instead.
    static std::uint32_t TransportSourceUsageFlags();

  private:
    struct Pools;

    /// Builds the UIManager, parses the desc's XML/CSS and settles style,
    /// layout and primitive generation with declare-only frames over throwaway
    /// pools — the UI-harness settle pattern — before the real frame declares.
    HeadlessViewStatus StandUpHud(const HeadlessViewDesc& desc);

    std::unique_ptr<Rendering::IDevice> m_Device;
    std::unique_ptr<Engine::Renderer::RenderServices> m_Services;
    std::unique_ptr<Pools> m_Pools;
    Rendering::CameraId m_Camera = 0;
    Rendering::ViewId m_View = 0;
    std::uint64_t m_FrameIndex = 0;

    ViewOutputBytes m_Handoff;
    ViewOutputBytes m_PreHud;
    HudFacts m_HudFacts;
    std::unique_ptr<UI::IPlatformApi> m_HudPlatform;
    std::unique_ptr<UIManager> m_Hud;
    ViewStageTap m_Source;
    FinalizeFacts m_Facts;
    ViewOutputBytes m_Transport;
    std::vector<TransportIndex> m_TransportIndex;
    TransportGeometry m_TransportShape;
    std::vector<std::string> m_PassNames;
    std::vector<std::string> m_ScheduledPassNames;
    std::string m_StatusMessage;
};

/// The shader's LinearToSRGB, one channel — the reference the CPU models
/// encode with. Kept public so a gate can state its own model rather than
/// trusting the fixture's.
float LinearToSrgbRef(float c);

/// The shader's TriangularDither(gl_FragCoord.xy) at pixel (x, y). Fragment
/// coordinates are texel CENTRES: (x + 0.5, y + 0.5). Integer coordinates give
/// a different IGN value and silently inflate every residual.
float TriangularDitherRef(std::uint32_t x, std::uint32_t y);

/// Full-scale code count for a destination format: 1023 for RGB10A2_UNORM, 255
/// for the 8-bit UNORM/SRGB formats. 0 for formats with no integer quantizer.
///
/// A property of the FORMAT — what "one code of this surface" means — derived
/// from its bit depth and not from EncodeDitherLsbForFormat. That independence
/// is what lets a gate state its expectation as "the amplitude is one
/// destination code" and have it survive a mutation of the production table.
float DestinationCodeScale(Rendering::TextureFormat format);

/// The four stored channel codes of one pixel, in R,G,B,A order and in the
/// surface's OWN units: the UNORM code for a quantizing format, and the raw
/// 16-bit half bit pattern for F16.
///
/// I4's claim — "every output pixel is a copy of exactly one source texel" — is
/// a statement about stored codes, so this is the form the byte gate must
/// compare in. Decoding to float first would let a format conversion hide
/// inside the decode, and comparing floats would need a tolerance, which chain
/// rule 5 rejects. Returns false for a format this fixture does not unpack.
bool DestinationCodes(const ViewOutputBytes& image, std::size_t pixelIndex, std::uint32_t rgba[4]);

/// Colour channel `channel` (0..2) of one pixel decoded to ENCODED [0,1] — a
/// quantizing format's code over its own full scale, a float format's stored
/// value verbatim. The unit the composite blend models are stated in. Returns
/// false for alpha (RGB10A2's 2-bit alpha has a different scale) and for a
/// format this fixture does not unpack.
bool ReadEncodedChannel(const ViewOutputBytes& image, std::size_t pixelIndex, int channel,
                        float& outEncoded);

/// The code channel `channel` (0..3) holds at full scale in this format — 1023
/// for RGB10A2's colour channels but 3 for its alpha, 255 for the 8-bit
/// formats, and half(1.0) = 0x3C00 for F16. Returns 0 for a format this fixture
/// does not unpack.
std::uint32_t DestinationFullScaleCode(Rendering::TextureFormat format, int channel);

} // namespace GameEngine::Testing
