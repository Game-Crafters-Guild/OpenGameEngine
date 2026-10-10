#include "Noise/FractalNoise2D.h"
#include "Noise/GradientNoise2D.h"

#include "NoiseGoldenSweep.h"

#include <gtest/gtest.h>

#include <cmath>

// Byte-identity gate for the shared noise primitives.
//
// The expected hashes were produced by sweeping the implementations this module
// replaced, compiled standalone from their pre-move sources and run at both /Od
// and /O2 (the two agree, so optimisation level does not move them). A change
// here that alters any sampled value by one bit fails this test, which is what
// keeps existing terrain bakes reproducing exactly.

namespace
{

using namespace GameEngine::Noise;
using namespace GameEngine::Noise::Testing;

constexpr uint64_t kGoldenGradientNoise2D = 0x70FFF0140F2BE3AAull;
constexpr uint64_t kGoldenGradientNoise2DDerivatives = 0x9992AA87FBA3B39Dull;
constexpr uint64_t kGoldenFBMNoise2D = 0x59EAC39A2F8FDBE4ull;
constexpr uint64_t kGoldenFBMNoise2DDerivatives = 0x54C24D266E3807F1ull;

struct SharedModule
{
    static float Gradient(float x, float z, uint32_t seed)
    {
        return GradientNoise2D(x, z, seed);
    }

    static void GradientWithDerivatives(float x, float z, uint32_t seed, float& value, float& dx,
                                        float& dz)
    {
        const NoiseSample s = GradientNoise2DWithDerivatives(x, z, seed);
        value = s.Value;
        dx = s.DValueDX;
        dz = s.DValueDZ;
    }

    static float FBM(float x, float z, float frequency, float amplitude, uint32_t octaves,
                     uint32_t seed, float lacunarity, float persistence)
    {
        return FBMNoise2D(x, z, frequency, amplitude, octaves, seed, lacunarity, persistence);
    }

    static void FBMWithDerivatives(float x, float z, float frequency, float amplitude,
                                   uint32_t octaves, uint32_t seed, float lacunarity,
                                   float persistence, float& value, float& dx, float& dz)
    {
        const NoiseSample s = FBMNoise2DWithDerivatives(x, z, frequency, amplitude, octaves, seed,
                                                        lacunarity, persistence);
        value = s.Value;
        dx = s.DValueDX;
        dz = s.DValueDZ;
    }
};

// One constant of the lattice hash moved by one. Nothing else differs, so if the
// sweep cannot tell this apart from the real thing then the sweep is not
// measuring the hash at all and every green above is worthless.
uint32_t PerturbedLatticeHash(int32_t ix, int32_t iz, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(ix) * 374761394u +
                 static_cast<uint32_t>(iz) * 668265263u + seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

struct PerturbedHash
{
    static float Gradient(float x, float z, uint32_t seed)
    {
        const int32_t ix = static_cast<int32_t>(std::floor(x));
        const int32_t iz = static_cast<int32_t>(std::floor(z));
        const float fx = x - static_cast<float>(ix);
        const float fz = z - static_cast<float>(iz);

        const float u = fx * fx * (3.0f - 2.0f * fx);
        const float v = fz * fz * (3.0f - 2.0f * fz);

        const GradientDirection g00 = CornerGradient(PerturbedLatticeHash(ix, iz, seed));
        const GradientDirection g10 = CornerGradient(PerturbedLatticeHash(ix + 1, iz, seed));
        const GradientDirection g01 = CornerGradient(PerturbedLatticeHash(ix, iz + 1, seed));
        const GradientDirection g11 = CornerGradient(PerturbedLatticeHash(ix + 1, iz + 1, seed));

        const float n00 = g00.SignX * fx + g00.SignZ * fz;
        const float n10 = g10.SignX * (fx - 1.0f) + g10.SignZ * fz;
        const float n01 = g01.SignX * fx + g01.SignZ * (fz - 1.0f);
        const float n11 = g11.SignX * (fx - 1.0f) + g11.SignZ * (fz - 1.0f);

        const float nx0 = n00 + u * (n10 - n00);
        const float nx1 = n01 + u * (n11 - n01);
        return nx0 + v * (nx1 - nx0);
    }
};

} // namespace

TEST(NoiseGolden, GradientNoise2DReproducesTheValuesItReplaced)
{
    EXPECT_EQ(SweepGradientNoise2D<SharedModule>(), kGoldenGradientNoise2D);
}

TEST(NoiseGolden, GradientNoise2DDerivativesReproduceTheValuesTheyReplaced)
{
    EXPECT_EQ(SweepGradientNoise2DWithDerivatives<SharedModule>(),
              kGoldenGradientNoise2DDerivatives);
}

TEST(NoiseGolden, FBMNoise2DReproducesTheValuesItReplaced)
{
    EXPECT_EQ(SweepFBMNoise2D<SharedModule>(), kGoldenFBMNoise2D);
}

TEST(NoiseGolden, FBMNoise2DDerivativesReproduceTheValuesTheyReplaced)
{
    EXPECT_EQ(SweepFBMNoise2DWithDerivatives<SharedModule>(), kGoldenFBMNoise2DDerivatives);
}

// The control for the four above: the sweep must be able to see a one-digit
// change to the lattice hash.
TEST(NoiseGolden, SweepDetectsAPerturbedLatticeHash)
{
    EXPECT_NE(SweepGradientNoise2D<PerturbedHash>(), kGoldenGradientNoise2D);
}

// The value path and the derivative path share one gradient table, so their
// values have to agree exactly rather than approximately. The erosion filter
// relies on this: arming it with zero gully octaves must leave the terrain
// untouched.
TEST(NoiseGolden, DerivativeCarryingSampleValueMatchesThePlainValue)
{
    for (int i = 0; i < kCoordSteps; ++i)
    {
        const float x = kCoordOrigin + static_cast<float>(i) * kCoordStepX;
        for (int j = 0; j < kCoordSteps; ++j)
        {
            const float z = kCoordOrigin + static_cast<float>(j) * kCoordStepZ;
            EXPECT_EQ(GradientNoise2DWithDerivatives(x, z, 1337u).Value,
                      GradientNoise2D(x, z, 1337u));
        }
    }
}

TEST(NoiseGolden, OctaveCountIsClampedToTheModuleBound)
{
    const float clamped = FBMNoise2D(1.5f, -2.25f, 0.1f, 1.0f, kMaxOctaves, 7u, 2.0f, 0.5f);
    const float absurd = FBMNoise2D(1.5f, -2.25f, 0.1f, 1.0f, 4096u, 7u, 2.0f, 0.5f);
    EXPECT_EQ(clamped, absurd);
}

TEST(NoiseGolden, ZeroOctavesSumsNothing)
{
    EXPECT_EQ(FBMNoise2D(1.5f, -2.25f, 0.1f, 1.0f, 0u, 7u, 2.0f, 0.5f), 0.0f);
}
