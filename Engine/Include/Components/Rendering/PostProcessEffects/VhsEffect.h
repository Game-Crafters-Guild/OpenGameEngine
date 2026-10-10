#pragma once

#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <string_view>

namespace GameEngine::Components {

// Animated VHS tape distortion. Attach to the same entity as a
// PostProcessVolume.
struct VhsEffect {
    static constexpr float32 kIntensityMax = 1.0f;
    static constexpr float32 kStrengthMax = 2.0f;
    static constexpr float32 kColorBleedOffsetMax = 16.0f;
    static constexpr float32 kSpeedMax = 4.0f;
    static constexpr float32 kOverlaySizeMin = 0.5f;
    static constexpr float32 kOverlaySizeMax = 4.0f;
    static constexpr uint32 kOverlayFontMax = 3;
    static constexpr uint32 kOverlayTextCapacity = 8;
    static constexpr uint32 kTransportModeMax = 2;
    static constexpr uint32 kTimeBasePresetMax = 5;
    static constexpr uint32 kCompositeSignalModeMax = 3;
    static constexpr uint32 kDateBurnYearMin = 1900;
    static constexpr uint32 kDateBurnYearMax = 2099;

    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 Intensity{1.0f};
    float32 Wobble{0.45f};
    float32 Tracking{0.45f};
    float32 SignalGlitches{0.15f};
    float32 GlitchOffsets{0.12f};
    uint32 TimeBasePreset{0};
    float32 Interference{0.10f};
    float32 FrameFeedback{0.16f};
    float32 FeedbackDecay{0.70f};
    float32 FeedbackMotionThreshold{0.06f};
    float32 FeedbackTrailLength{3.0f};
    uint32 CompositeSignalMode{0};
    float32 DotCrawl{0.20f};
    float32 ColorBleed{1.0f};
    float32 ColorBleedOffset{4.0f};
    float32 TapeNoise{0.18f};
    float32 ChromaStreaks{0.08f};
    float32 Dropouts{0.05f};
    float32 Scanlines{0.08f};
    float32 Speed{1.0f};
    bool OverlayEnabled{true};
    float32 OverlayColor[3]{0.35f, 1.0f, 0.35f};
    float32 OverlayOpacity{0.85f};
    float32 OverlaySize{2.0f};
    uint32 OverlayFont{0};
    float32 OverlayPositionX{0.01957f};
    float32 OverlayPositionY{0.0f};
    char OverlayText[kOverlayTextCapacity + 1]{"REC"};
    bool DateBurnEnabled{true};
    float32 DateBurnColor[3]{1.0f, 0.95f, 0.78f};
    float32 DateBurnSize{1.5f};
    float32 DateBurnPositionX{0.031875f};
    float32 DateBurnPositionY{0.072773f};
    uint32 DateBurnYear{2018};
    uint32 DateBurnMonth{5};
    uint32 DateBurnDay{16};
    uint32 DateBurnHour{10};
    uint32 DateBurnMinute{44};
    uint32 TransportMode{0};
    float32 TransportStrength{0.65f};

    void SetOverlayText(std::string_view value)
    {
        uint32 written = 0;
        for (char character : value)
        {
            if (written >= kOverlayTextCapacity)
                break;
            if (character >= 'a' && character <= 'z')
                character = static_cast<char>(character - 'a' + 'A');
            const bool supported = (character >= 'A' && character <= 'Z')
                                || (character >= '0' && character <= '9')
                                || character == ' ' || character == ':'
                                || character == '-' || character == '.'
                                || character == '/';
            OverlayText[written++] = supported ? character : ' ';
        }
        while (written <= kOverlayTextCapacity)
            OverlayText[written++] = '\0';
    }

    std::string_view GetOverlayText() const
    {
        return FixedStringView(OverlayText).substr(0, kOverlayTextCapacity);
    }
};

} // namespace GameEngine::Components
