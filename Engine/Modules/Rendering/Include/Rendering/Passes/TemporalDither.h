#pragma once

#include <cstdint>

namespace GameEngine {
namespace Rendering {
namespace Passes {

// THE temporal-dither phase, decided here and nowhere else.
//
// The terminal encode's TPDF dither is a screen-space pattern: the same pixel
// takes the same perturbation every frame. That is correct for a still image
// and is what every shipped path uses (phase 0). Advancing the pattern per
// frame instead makes the perturbation average toward the unquantized value as
// the eye (or a codec's temporal prediction) integrates over frames, so banding
// a static 1-LSB dither cannot break can dissolve. It also costs: per-frame
// independent noise is strictly harder to compress, and it can read as shimmer
// on content the viewer expects to be still. Both toggles below are therefore
// OFF by default and exist to be A/B'd.
//
// The phase is an offset in the noise's OUTPUT domain (encode_srgb.frag adds it
// to the [0,1) IGN sample and wraps), not a translation of the sample position.
// Two consequences that are the reason for the choice:
//   - it cannot reach the deband. DebandEncoded drives its tap distance and
//     rotation from the same noise function at fixed offsets 5.0 and 11.0; a
//     per-frame smoothing DECISION would flicker on static content. With the
//     phase living past the noise function rather than inside it, the deband is
//     out of reach structurally instead of by discipline.
//   - a uniform sample stays uniform under a wrapped shift, so the dither keeps
//     its triangular distribution and its 1-LSB amplitude exactly. There is no
//     offset-constant degeneracy class to dodge (the one encode_srgb.frag
//     documents for its IGN offset parameter): the phase IS the shift.
// Phase 0 is bitwise the unphased pattern — fract() of a value already in
// [0,1) returns it unchanged — so every path that does not opt in is untouched.
//
// Seeded on a MOVIE ORDINAL for recordings and on the render frame index for
// the screen. Both mappings live here so the two hosts cannot compute different
// phases for the same movie frame, the same discipline that keeps
// SelectTransferQuantizer's answer from drifting between them.

// The phase for a realisation index: an additive golden-ratio recurrence, i.e.
// the low-discrepancy sequence fract(n * phi^-1). Successive indices land far
// apart (no two consecutive frames share a similar realisation) while any run
// of frames covers [0,1) near-uniformly, which is what makes the temporal
// average converge quickly rather than after a long period. Named and derived
// rather than tuned: nothing about it depends on the noise function's internal
// constants, so a change to that function cannot silently collapse the spread.
//
// Evaluated in double so the fractional part survives long recordings: a
// 10^6-frame ordinal still leaves 30+ mantissa bits below the binary point.
float TemporalDitherPhaseForIndex(uint64_t realisationIndex);

// The phase a movie encode declares for the frame it is recording. Returns 0
// unless temporal movie dither is enabled, so the shipped state is the static
// pattern and a recording made with the toggle off is bit-comparable to one
// made before this existed.
//
// `movieOrdinal` must be the host's count of ACCEPTED capture frames
// (m_MovieFramesScheduled), never the render frame index: render frames are
// dropped by back-pressure and by capture warm-up, so seeding on them makes two
// recordings of the same deterministic scene carry different dither. On the
// ordinal, movie frame N always carries realisation N in both hosts.
// (That does not by itself make two recordings identical — which render frames
// are ACCEPTED is still timing-dependent — it adds no new divergence.)
float MovieDitherPhase(uint64_t movieOrdinal);

// The phase an on-screen finalize or terminal encode declares. Returns 0 unless
// temporal screen dither is enabled. Seeded on the render frame index, which is
// exactly the sequence the viewer integrates over; reproducibility is a movie
// property and does not apply.
float ScreenDitherPhase(uint64_t renderFrameIndex);

// The two experimental toggles, process-wide and settable at runtime (the
// editor's Experimental settings page writes them; the Player takes the movie
// one from its command line). Read on the render-declare path, written from the
// UI thread — relaxed atomics: a toggle landing one frame later is invisible.
bool IsTemporalMovieDitherEnabled();
void SetTemporalMovieDitherEnabled(bool enabled);

bool IsTemporalScreenDitherEnabled();
void SetTemporalScreenDitherEnabled(bool enabled);

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
