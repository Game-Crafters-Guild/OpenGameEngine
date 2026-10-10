#include "UI/UITargetSpace.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "UI/UITextureSpace.h"

namespace GameEngine
{
namespace UI
{

UITargetSpace UITargetSpace::ForDisplay(Rendering::HdrOutputMode activeMode)
{
    // The three HDR arms map to their flavour; everything else — Off, Auto
    // (never an ACTIVE mode), and any future enumerator until it is named
    // here — is the SDR arm. Matches the SDF shader's outputEncoding
    // convention via UIManager::ResolveOutputEncoding.
    switch (activeMode)
    {
    case Rendering::HdrOutputMode::HDR10_PQ:
    case Rendering::HdrOutputMode::HDR10Plus:
        return HdrPq();
    case Rendering::HdrOutputMode::HLG:
        return Hlg();
    case Rendering::HdrOutputMode::ScRGB:
        return ScRgb();
    default:
        return LinearSdr();
    }
}

UITextureSpace UITargetSpace::ReadbackSpace() const
{
    static_assert(kKindCount == 5, "A new UITargetSpace needs a readback-space rule here.");
    switch (m_Kind)
    {
    case Kind::EncodedSrgb:
        return UITextureSpace::SrgbAuthored(); // encoded at rest
    case Kind::HdrPq:
    case Kind::Hlg:
    case Kind::ScRgb:
        return UITextureSpace::HdrLinear();
    case Kind::LinearSdr:
    case Kind::Count: // size sentinel; no factory produces it
        break;
    }
    return UITextureSpace::DisplayLinearSdr();
}

UITargetSpace UITargetSpace::ForPipelineOutput(UITextureSpace producedSpace,
                                               Rendering::HdrOutputMode activeMode)
{
    static_assert(UITextureSpace::kKindCount == 4,
                  "A new UITextureSpace needs a compositing-target rule here.");
    switch (producedSpace.GetKind())
    {
    case UITextureSpace::Kind::DisplayLinearSdr:
        return LinearSdr();
    case UITextureSpace::Kind::SdrFinalized:
        // A finalized view already carries the output curve, so the composite it
        // joins must blend in that same encoded domain: EncodedSrgb is the only
        // target space that leaves its code values where the finalize put them.
        return EncodedSrgb();
    case UITextureSpace::Kind::HdrLinear:
    {
        const UITargetSpace flavour = ForDisplay(activeMode);
        if (flavour == LinearSdr())
        {
#if defined(GE_DEV_DIAG)
            static bool s_WarnedContradiction = false;
            if (!s_WarnedContradiction)
            {
                s_WarnedContradiction = true;
                Logger::Log::Warning(
                    "[UI TargetSpace] pipeline output stamped HdrLinear while no HDR output "
                    "mode is active — compositing as LinearSdr");
            }
#endif
            return LinearSdr();
        }
        return flavour;
    }
    case UITextureSpace::Kind::SrgbAuthored:
    case UITextureSpace::Kind::Count: // size sentinel; no factory produces it
        break;
    }
#if defined(GE_DEV_DIAG)
    static bool s_WarnedEncoded = false;
    if (!s_WarnedEncoded)
    {
        s_WarnedEncoded = true;
        Logger::Log::Warning("[UI TargetSpace] pipeline output stamped {} — not a space the UI "
                             "composites into; compositing as LinearSdr",
                             ToString(producedSpace));
    }
#endif
    return LinearSdr();
}

const char* ToString(UITargetSpace space)
{
    static_assert(UITargetSpace::kKindCount == 5, "A new UITargetSpace needs a name here.");
    const char* name = "";
    switch (space.GetKind())
    {
    case UITargetSpace::Kind::LinearSdr:
        name = "LinearSdr";
        break;
    case UITargetSpace::Kind::EncodedSrgb:
        name = "EncodedSrgb";
        break;
    case UITargetSpace::Kind::HdrPq:
        name = "HdrPq";
        break;
    case UITargetSpace::Kind::Hlg:
        name = "Hlg";
        break;
    case UITargetSpace::Kind::ScRgb:
        name = "ScRgb";
        break;
    case UITargetSpace::Kind::Count:
        break; // size sentinel; no factory produces it
    }
    return name;
}

} // namespace UI
} // namespace GameEngine
