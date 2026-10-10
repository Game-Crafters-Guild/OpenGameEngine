#pragma once

#include "Types/Types.h"

namespace GameEngine::Components {

enum class FilmGateMask : uint32 {
    None = 0,
    RoundedGate,
    Academy137,
    Widescreen185,
    Anamorphic239
};

enum class FilmGrainMode : uint32 {
    FidelityFXFast = 0,
    Filmic
};

// Photochemical film characteristics collected into one effect. Highlight
// halation is evaluated in HDR, grain after tone mapping, and mechanical film
// artifacts are composited at the end of the LDR post-process chain.
struct FilmSimulationEffect {
    // Valid parameter ranges, shared by the inspector and render extraction so
    // the authoring UI and the GPU-facing clamps cannot drift apart.
    static constexpr float32 kFrameRateMin = 1.0f;
    static constexpr float32 kFrameRateMax = 120.0f;
    static constexpr float32 kHalationIntensityMax = 4.0f;
    static constexpr float32 kHalationRadiusMax = 64.0f;
    static constexpr float32 kGrainIntensityMax = 4.0f;
    static constexpr float32 kGrainSizeMin = 0.25f;
    static constexpr float32 kGrainSizeMax = 8.0f;
    static constexpr float32 kGrainResponseMax = 2.0f;
    static constexpr uint32 kGrainModeLast = static_cast<uint32>(FilmGrainMode::Filmic);
    // Hair/scratch amounts are authored as an average count out of the
    // shader's per-frame candidate pool; extraction divides by the pool size
    // to produce the 0-1 spawn probability the GPU consumes.
    static constexpr float32 kArtifactCandidateCount = 8.0f;
    static constexpr float32 kHairAmountMax = kArtifactCandidateCount;
    static constexpr float32 kScratchAmountMax = kArtifactCandidateCount;
    // Hair/scratch intensity is authored on a 0-4 range for drag precision;
    // extraction divides by the max to produce the 0-1 opacity the GPU uses.
    static constexpr float32 kArtifactIntensityMax = 4.0f;
    static constexpr float32 kHairWidthMin = 0.05f;
    static constexpr float32 kHairWidthMax = 8.0f;
    static constexpr float32 kHairLengthMin = 8.0f;
    static constexpr float32 kHairLengthMax = 1024.0f;
    static constexpr float32 kScratchWidthMin = 0.05f;
    static constexpr float32 kScratchWidthMax = 8.0f;
    static constexpr float32 kScratchLengthMin = 0.05f;
    static constexpr float32 kScratchLengthMax = 1.0f;
    static constexpr float32 kDustAmountMax = kArtifactCandidateCount;
    static constexpr float32 kDustSizeMin = 0.5f;
    static constexpr float32 kDustSizeMax = 24.0f;
    static constexpr float32 kGateWeaveOffsetMax = 32.0f;
    static constexpr float32 kGateWeaveRotationMax = 2.0f;
    static constexpr float32 kGateMaskFeatherMax = 256.0f;
    static constexpr float32 kGateMaskRoundnessMax = 1.0f;
    static constexpr uint32 kGateMaskLast = static_cast<uint32>(FilmGateMask::Anamorphic239);

    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 FilmFrameRate{24.0f};

    bool HalationEnabled{true};
    float32 HalationIntensity{2.0f};
    float32 HalationRadius{3.0f};
    float32 HalationTint[3]{1.0f, 0.12f, 0.025f};

    bool GrainEnabled{true};
    FilmGrainMode GrainMode{FilmGrainMode::Filmic};
    float32 GrainIntensity{1.0f};
    float32 GrainSize{3.0f};
    bool GrainSmooth{true};
    float32 GrainDensity{1.0f};
    float32 GrainShadowResponse{1.0f};
    float32 GrainMidtoneResponse{1.0f};
    float32 GrainHighlightResponse{0.35f};
    bool GrainColored{true};

    bool HairEnabled{true};
    float32 HairAmount{0.082891f};
    float32 HairIntensity{1.2f};
    float32 HairWidth{1.25f};
    float32 HairLength{220.0f};
    float32 HairRandomSize{1.0f};
    float32 HairCurl{0.25f};
    float32 HairCurlRandomness{1.0f};

    bool ScratchesEnabled{true};
    float32 ScratchAmount{0.262656f};
    float32 ScratchIntensity{0.327031f};
    float32 ScratchWidth{0.8f};
    float32 ScratchLength{0.65f};

    bool DustEnabled{true};
    float32 DustAmount{2.5f};
    float32 DustIntensity{1.0f};
    float32 DustSize{3.24375f};
    float32 DustRandomSize{1.0f};

    bool GateWeaveEnabled{true};
    float32 GateWeaveHorizontal{0.75f};
    float32 GateWeaveVertical{0.5f};
    float32 GateWeaveRotation{0.08f};

    FilmGateMask GateMask{FilmGateMask::RoundedGate};
    float32 GateMaskFeather{3.0f};
    // Corner radius as a fraction of the gate's half extent; 0 = sharp corners.
    float32 GateMaskRoundness{0.025f};
};

} // namespace GameEngine::Components
