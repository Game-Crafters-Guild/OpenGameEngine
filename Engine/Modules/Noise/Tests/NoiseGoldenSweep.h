#pragma once

#include <cstdint>
#include <cstring>

// The fixed sample sweep behind the golden test.
//
// The sweep lives here, once, as a template over the implementation under test,
// so that values captured from one implementation and values recomputed by
// another cannot drift apart through an edited copy of the loop. An Impl
// supplies the four static functions called below.

namespace GameEngine::Noise::Testing
{

// FNV-1a over the raw bits of every float the sweep produces, in order. Hashing
// the bits rather than comparing rounded decimals is what makes this a
// byte-identity check instead of an approximate one.
class BitHasher
{
public:
    void Feed(float value)
    {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "float must be 32 bits");
        std::memcpy(&bits, &value, sizeof(bits));
        for (int byte = 0; byte < 4; ++byte)
        {
            m_Hash ^= static_cast<uint64_t>((bits >> (byte * 8)) & 0xFFu);
            m_Hash *= 1099511628211ull;
        }
    }

    uint64_t Value() const { return m_Hash; }

private:
    uint64_t m_Hash = 1469598103934665603ull;
};

// Coordinates deliberately straddle zero and land off the lattice, so the sweep
// exercises negative cell indices and every corner of the interpolation rather
// than only the well-behaved first quadrant.
inline constexpr int kCoordSteps = 48;
inline constexpr float kCoordOrigin = -7.0f;
inline constexpr float kCoordStepX = 0.37f;
inline constexpr float kCoordStepZ = 0.41f;

inline constexpr uint32_t kSeeds[] = {0u, 1u, 1337u, 12345u, 4294967295u};
inline constexpr int kSeedCount = 5;

// Octave counts include 0 (sums nothing) and 99 (above the clamp), which are the
// two ends the loop bound has to get right.
inline constexpr uint32_t kOctaveCounts[] = {0u, 1u, 4u, 8u, 99u};
inline constexpr int kOctaveCountCount = 5;

inline constexpr float kFrequencies[] = {0.01f, 0.1f, 1.0f};
inline constexpr int kFrequencyCount = 3;

inline constexpr float kAmplitudes[] = {1.0f, 7.5f};
inline constexpr int kAmplitudeCount = 2;

inline constexpr float kLacunarities[] = {2.0f, 2.13f};
inline constexpr int kLacunarityCount = 2;

inline constexpr float kPersistences[] = {0.5f, 0.75f};
inline constexpr int kPersistenceCount = 2;

/// Sweeps the gradient-noise lattice over every coordinate and seed.
template <typename Impl>
uint64_t SweepGradientNoise2D()
{
    BitHasher hasher;
    for (int s = 0; s < kSeedCount; ++s)
    {
        for (int i = 0; i < kCoordSteps; ++i)
        {
            const float x = kCoordOrigin + static_cast<float>(i) * kCoordStepX;
            for (int j = 0; j < kCoordSteps; ++j)
            {
                const float z = kCoordOrigin + static_cast<float>(j) * kCoordStepZ;
                hasher.Feed(Impl::Gradient(x, z, kSeeds[s]));
            }
        }
    }
    return hasher.Value();
}

/// Sweeps the gradient lattice's analytic derivatives.
template <typename Impl>
uint64_t SweepGradientNoise2DWithDerivatives()
{
    BitHasher hasher;
    for (int s = 0; s < kSeedCount; ++s)
    {
        for (int i = 0; i < kCoordSteps; ++i)
        {
            const float x = kCoordOrigin + static_cast<float>(i) * kCoordStepX;
            for (int j = 0; j < kCoordSteps; ++j)
            {
                const float z = kCoordOrigin + static_cast<float>(j) * kCoordStepZ;
                float value = 0.0f, dx = 0.0f, dz = 0.0f;
                Impl::GradientWithDerivatives(x, z, kSeeds[s], value, dx, dz);
                hasher.Feed(value);
                hasher.Feed(dx);
                hasher.Feed(dz);
            }
        }
    }
    return hasher.Value();
}

// The fractal sweeps walk a coarser coordinate grid than the lattice sweeps
// because they multiply it by the whole parameter cross-product.
inline constexpr int kFractalCoordSteps = 8;

/// Sweeps fBM across the full parameter cross-product.
template <typename Impl>
uint64_t SweepFBMNoise2D()
{
    BitHasher hasher;
    for (int s = 0; s < kSeedCount; ++s)
        for (int o = 0; o < kOctaveCountCount; ++o)
            for (int f = 0; f < kFrequencyCount; ++f)
                for (int a = 0; a < kAmplitudeCount; ++a)
                    for (int l = 0; l < kLacunarityCount; ++l)
                        for (int p = 0; p < kPersistenceCount; ++p)
                            for (int i = 0; i < kFractalCoordSteps; ++i)
                            {
                                const float x = kCoordOrigin + static_cast<float>(i) * kCoordStepX;
                                for (int j = 0; j < kFractalCoordSteps; ++j)
                                {
                                    const float z =
                                        kCoordOrigin + static_cast<float>(j) * kCoordStepZ;
                                    hasher.Feed(Impl::FBM(x, z, kFrequencies[f], kAmplitudes[a],
                                                          kOctaveCounts[o], kSeeds[s],
                                                          kLacunarities[l], kPersistences[p]));
                                }
                            }
    return hasher.Value();
}

/// Sweeps fBM's analytic derivatives across the same cross-product.
template <typename Impl>
uint64_t SweepFBMNoise2DWithDerivatives()
{
    BitHasher hasher;
    for (int s = 0; s < kSeedCount; ++s)
        for (int o = 0; o < kOctaveCountCount; ++o)
            for (int f = 0; f < kFrequencyCount; ++f)
                for (int a = 0; a < kAmplitudeCount; ++a)
                    for (int l = 0; l < kLacunarityCount; ++l)
                        for (int p = 0; p < kPersistenceCount; ++p)
                            for (int i = 0; i < kFractalCoordSteps; ++i)
                            {
                                const float x = kCoordOrigin + static_cast<float>(i) * kCoordStepX;
                                for (int j = 0; j < kFractalCoordSteps; ++j)
                                {
                                    const float z =
                                        kCoordOrigin + static_cast<float>(j) * kCoordStepZ;
                                    float value = 0.0f, dx = 0.0f, dz = 0.0f;
                                    Impl::FBMWithDerivatives(
                                        x, z, kFrequencies[f], kAmplitudes[a], kOctaveCounts[o],
                                        kSeeds[s], kLacunarities[l], kPersistences[p], value, dx,
                                        dz);
                                    hasher.Feed(value);
                                    hasher.Feed(dx);
                                    hasher.Feed(dz);
                                }
                            }
    return hasher.Value();
}

} // namespace GameEngine::Noise::Testing
