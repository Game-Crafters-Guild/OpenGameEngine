#pragma once

#include <cstdint>

#include "Rendering/Core/Device.h"

namespace GameEngine
{
namespace UI
{

/// Colour space of the pixels an external texture holds, stamped by whoever
/// produced them.
///
/// The space is a property of the STORED BYTES, not of the display: the same
/// producer yields different spaces under different output modes, and a frozen
/// snapshot keeps the space it was captured in no matter what the display does
/// afterwards.
///
/// Construction is restricted to the named factories below. There is
/// deliberately no default constructor, no conversion from a raw enumerator and
/// no conversion from an output mode — a caller cannot state a space by writing
/// nothing, nor derive one from the display.
class UITextureSpace
{
public:
    /// Discriminator for exhaustive switches. Nameable, but not constructible
    /// into a UITextureSpace: the converting constructor is private.
    ///
    /// `Count` is a size sentinel, never a space — no factory produces it, so a
    /// switch over a real space never sees it. It exists so every site that
    /// translates a space into something else can pin kKindCount and fail to
    /// compile when a space is added: MSVC's unhandled-enumerator warning
    /// (C4062) is OFF at /W4, so the gate cannot rest on warnings-as-errors.
    enum class Kind : uint8_t
    {
        SrgbAuthored,
        SdrFinalized,
        DisplayLinearSdr,
        HdrLinear,
        Count
    };

    /// Pin for translation sites: a `static_assert` on this next to a switch
    /// turns a newly added space into a compile error at that switch.
    static constexpr uint8_t kKindCount = static_cast<uint8_t>(Kind::Count);

    /// Authored sRGB. At rest as encoded bytes; the sampler may hand them over
    /// decoded depending on the view format. CSS bytes, icons, glyph atlases,
    /// decoded video frames, PNG-reloaded snapshots.
    static constexpr UITextureSpace SrgbAuthored()
    {
        return UITextureSpace(Kind::SrgbAuthored);
    }

    /// A world view that finalized itself: sRGB-encoded, and already carrying
    /// the deband and the TPDF dither sized to the quantizer of the surface it
    /// will be presented on. Produced by a per-view finalize, never by an
    /// importer.
    ///
    /// Distinct from SrgbAuthored on the one question the encoded UI blend space
    /// asks: SrgbAuthored content reaches the sampler DECODED (its view carries
    /// the sRGB transfer), so the encoded target re-applies the OETF at sample
    /// time; SdrFinalized content is stored behind a plain view and arrives
    /// exactly as written, so its texels ALREADY ARE blend-space values and a
    /// second OETF would darken the whole viewport. That makes it the one
    /// external space whose sample adapter is the identity.
    ///
    /// "Finalized" is about the FILTERS, not about a grid: the values are still
    /// continuous, and the rounding onto code values happens where it always
    /// has — in the store that writes the presented surface. What the name
    /// promises is that the noise which has to precede that rounding is already
    /// in the image, sized to that surface's step, and confined to the pixels
    /// that needed it. Everything downstream therefore filters NOTHING: the UI
    /// blends these values, the composite carries them, and the terminal pass
    /// rounds them and adds nothing of its own.
    static constexpr UITextureSpace SdrFinalized()
    {
        return UITextureSpace(Kind::SdrFinalized);
    }

    /// Tonemapped display-referred linear, [0,1]. A viewport, game view,
    /// thumbnail or snapshot rendered under an SDR output mode.
    static constexpr UITextureSpace DisplayLinearSdr()
    {
        return UITextureSpace(Kind::DisplayLinearSdr);
    }

    /// Paper-white-relative linear, unbounded. The same producers rendered
    /// under an HDR output mode.
    static constexpr UITextureSpace HdrLinear()
    {
        return UITextureSpace(Kind::HdrLinear);
    }

    constexpr Kind GetKind() const
    {
        return m_Kind;
    }

    friend constexpr bool operator==(UITextureSpace, UITextureSpace) = default;

private:
    constexpr explicit UITextureSpace(Kind kind) : m_Kind(kind) {}

    Kind m_Kind;
};

// ── The two space-vs-format questions, and why they are different predicates ──
//
// CheckTextureSpaceAgainstFormat judges a SAMPLING REGISTRATION: could this
// format have faithfully carried content of this space to the UI's sampler?
// It is a validity judgment with a failure channel — an invalid pair refuses
// the registration. F16 + SrgbAuthored is ContradictsFormat here: an external
// texture claiming authored-sRGB bytes behind a float view is sampled
// UNDECODED while its space bits promise decoded content. F16 + SdrFinalized is
// Valid for the mirror-image reason — that space promises undecoded encoded
// texels, which is exactly what a plain float view delivers.
//
// IsEncodedAtRest answers a READBACK/ATTACHMENT question: do the bytes I am
// holding carry the transfer curve right now? It is a factual declaration
// about CONTENT — every space has an answer, none is illegal. The #767
// EncodedSrgb UI target writes sRGB-encoded bytes into a float attachment BY
// DESIGN, so a capture of that composite declares SrgbAuthored and is
// correct — at readback. The same (F16, SrgbAuthored) pair is thus an
// invalid sampling registration and a legitimate readback declaration
// simultaneously; collapsing the two questions into one predicate would
// either let a lying registration through or force a capture to double-
// encode. A readback consumer may ask IsEncodedAtRest and nothing else;
// registration keeps its rejection.

/// Outcome of checking a stamped space against the texture's actual format.
enum class UITextureSpaceCheck : uint8_t
{
    /// The format can hold the stated space.
    Valid,
    /// The format cannot hold the stated space — one of the two is a lie.
    ContradictsFormat,
    /// The format carries no colour-space meaning (Unknown, integer, depth),
    /// so the pair cannot be judged. Never silently treated as valid.
    FormatNotClassifiable
};

/// Cross-checks a producer's stamp against the format the texture was created
/// with. Encoded-8-bit and 8-bit UNORM views cannot hold linear content of
/// either kind; wide formats (float, >8bpc UNORM) cannot hold sRGB-encoded
/// bytes without the sampler silently double-decoding them downstream.
UITextureSpaceCheck CheckTextureSpaceAgainstFormat(Rendering::TextureFormat format,
                                                   UITextureSpace space);

/// True when the bytes already carry the sRGB transfer curve, so anything
/// producing an encoded artifact from them (a capture, a PNG, a movie frame)
/// must NOT apply the curve a second time. False for both linear spaces.
///
/// This is the one question a readback converter may ask about a space. It is
/// deliberately not a question about the format: an F16 target can hold encoded
/// bytes, and answering from the format is how a capture double-encodes.
bool IsEncodedAtRest(UITextureSpace space);

/// Human-readable space name for diagnostics.
const char* ToString(UITextureSpace space);

} // namespace UI
} // namespace GameEngine
