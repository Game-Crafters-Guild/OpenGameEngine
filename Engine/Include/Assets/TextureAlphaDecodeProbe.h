#pragma once

#include "Types/Types.h"

#include <cstddef>
#include <filesystem>
#include <optional>

namespace GameEngine {

/**
 * @brief Decode tier of the texture alpha probe: proving a cutout never fires.
 *
 * Second tier behind ProbeTextureAlphaFromBytes (Assets/TextureAlphaProbe.h).
 * The header tier answers "does this container encode alpha at all" from
 * metadata alone; when it reports MaybeAlpha the channel exists but its
 * CONTENT is still unknown, and the only way to prove a cutout can never
 * discard is to look at every texel.
 *
 * Two things have to be true before a demotion is safe, and this tier answers
 * BOTH or neither:
 *
 *  1. The exact minimum alpha of the SOURCE. Every texel of every mip level
 *     (and every array layer / cube face) is scanned; nothing is subsampled.
 *     A subsampled minimum could not support a never-discard claim at all: the
 *     one texel skipped may be the transparent one, and demoting on it would
 *     erase a real cutout (foliage rendering as solid quads).
 *
 *  2. A BOUND on how far the texture the GPU actually samples can fall below
 *     that source minimum. The GPU never samples the source: it samples the
 *     cooked, block-compressed upload. Without a bound on that delta, an exact
 *     source minimum proves nothing.
 *
 * Point 2 is why this tier accepts only some containers, and the asymmetry is
 * measured, not assumed (alpha-envelope measurement, 2026-07-25):
 *
 *  - KTX2 carrying a Basis/UASTC payload is transcoded to BC7 at load
 *    (TextureAsset::PopulateFromKtxTexture). UASTC is designed to transcode to
 *    BC7 near-losslessly, and across 1742 real shipped .ktx2 files the alpha
 *    drop was never more than 4 steps (values observed: 0, 1, 4).
 *    kCookedAlphaDropBound covers it with better than 2x headroom.
 *  - KTX2 already holding uncompressed RGBA8 is uploaded as-is. Delta zero.
 *  - PNG / TGA are re-encoded to BC7 by the import cook (CookTexture ->
 *    DirectX::Compress). That delta is NOT bounded by anything this tier can
 *    see. BC7 spends one shared interpolation index across RGBA in its single
 *    subset modes, so a block whose RGB is hard to fit spills its error into
 *    alpha: measured drops reached 29 steps on ordinary-looking content and 96
 *    steps on adversarial content, INCLUDING cases where the source alpha was
 *    perfectly constant. The drop is in fact anti-correlated with the source
 *    alpha range, so no margin computed from the source alpha can bound it.
 *    These sources are therefore reported Unprobeable and keep their Mask.
 *
 * Anything other than Answered means "unknown", which callers must read as
 * "may discard" and resolve by keeping the authored alpha mode. The fail-safe
 * direction is always to keep Mask.
 *
 * Values are in 8-bit steps because that is the precision the decoder actually
 * delivers (Basis transcodes to RGBA32), and it matches the cutoff threshold
 * arithmetic in AlphaCutoffOpaqueThreshold (Assets/AlphaCutoffThreshold.h).
 *
 * Residual: mips the cook generates from an uncompressed source are not
 * represented here, so a non-box downsample filter could ring below this
 * minimum at heavy minification. KTX2 containers STORE their mips and every
 * level is scanned, so the containers this tier accepts carry no such
 * residual — it applies only to the PNG/TGA sources it now refuses.
 */

/// Largest source blob the decode tier will accept, bounding both the read and
/// the decoder's own allocation.
constexpr size_t kMaxProbeSourceBytes = 64ull * 1024ull * 1024ull;

/// Largest total texel count the decode tier will scan, counted over EVERY
/// image the container declares — all mip levels, array layers and cube faces
/// — not just the base image.
///
/// Sized from the largest thing real content ships: a 4096x4096 base-colour
/// atlas WITH its stored mip chain is 22.4M texels (~89 MiB of RGBA8), since a
/// full chain adds about a third to the base. 24M leaves headroom for that
/// without admitting the array and cubemap headers that expanded to hundreds
/// of MiB. Anything larger keeps its Mask.
constexpr uint64 kMaxProbeTexels = 24ull * 1024ull * 1024ull;

/// Steps the GPU-sampled minimum may sit below the source minimum, for the
/// containers this tier accepts. Measured worst case is 4 (1742 real UASTC
/// .ktx2 files); 8 keeps the shipped value and the headroom. Re-measure before
/// extending this tier to any other container: the bound is a property of the
/// transcode, not of alpha.
constexpr uint32 kCookedAlphaDropBound = 8;

/// How many probe decodes may run at once, process-wide.
///
/// A probe is a full decode: measured at 63-304 ms and ~118 MiB peak for a
/// 2048x2048 source in an /O2 build, and slower in Debug. Callers reach it from
/// JobSystem workers (material registration runs inside AssetManager::LoadAsset
/// completion callbacks, which fire inline on the loading worker). Unbounded, N
/// concurrent registrations multiply that spike by N.
///
/// The cap NEVER blocks. A thread that cannot get a slot gives up immediately
/// and reports Deferred. Blocking here would park a pool worker, converting a
/// bounded-memory problem into worker-pool occupancy by sleepers — the failure
/// mode CompileConcurrencyGate.h:13-20 already rules out for this exact reason,
/// and one that would EXTEND the AssetIOService decode-slot hold this cap is
/// meant to relieve rather than shorten it.
///
/// Determinism is preserved by the caller contract instead of by waiting: a
/// Deferred result must not be cached, so a later registration re-probes and
/// the steady state is the same verdict either way. That rule is what makes
/// non-blocking safe — refusing WITHOUT it would let one transient contention
/// miss freeze into a permanent "keeps Mask".
///
/// Overridable with GE_ALPHA_PROBE_DECODE_MAX.
constexpr uint32 kDefaultMaxConcurrentProbeDecodes = 2;

/// Highest number of probe decodes observed running concurrently since the last
/// reset. Process-global, monotonic, and saturating at the cap, so a test must
/// open its own window with ResetPeakConcurrentProbeDecodes rather than assert
/// against zero — otherwise an earlier test's peak keeps it green
/// (TextureAlphaDecodeProbeTests.ConcurrentProbesRespectTheCap).
uint32 PeakConcurrentProbeDecodes();

/// Reopens the observation window for PeakConcurrentProbeDecodes by dropping
/// the high-water mark to whatever is in flight right now. Exists because the
/// peak is monotonic AND saturates at the cap, so a test cannot otherwise tell
/// "my probes ran concurrently" from "an earlier test already saturated it".
void ResetPeakConcurrentProbeDecodes();

/// Why a probe produced no minimum. The distinction exists for exactly one
/// reason: Deferred is a property of the MOMENT, every other outcome is a
/// property of the FILE, and a caller that caches the first as if it were the
/// second turns a transient miss into a permanent one.
enum class TextureAlphaProbeStatus : uint8
{
    Answered,    ///< MinAlpha is exact over every texel.
    Unprobeable, ///< Too large, malformed, unrecognized, or cooked through an
                 ///< encode this tier cannot bound. A property of the file:
                 ///< safe to cache, and stable across runs.
    Deferred,    ///< The concurrency cap was full. Says NOTHING about the file.
                 ///< Callers MUST NOT cache this and MUST fall back to keeping
                 ///< the authored alpha mode for now.
};

struct TextureAlphaProbeResult
{
    TextureAlphaProbeStatus Status = TextureAlphaProbeStatus::Unprobeable;
    uint8 MinAlpha = 0; ///< Meaningful only when Status == Answered.
};

/// Exact minimum alpha over every texel of a container whose cooked delta this
/// tier can bound. Anything else reports Unprobeable or Deferred; all of them
/// resolve to "keep Mask", but only Unprobeable may be remembered.
TextureAlphaProbeResult ProbeTextureMinAlphaFromBytes(const uint8* bytes, size_t size);

/// Reads the file (refusing anything past kMaxProbeSourceBytes) and forwards to
/// ProbeTextureMinAlphaFromBytes. Unreadable files are Unprobeable.
TextureAlphaProbeResult ProbeTextureMinAlphaFromFile(const std::filesystem::path& path);

} // namespace GameEngine
