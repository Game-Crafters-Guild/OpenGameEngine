#include "DebugServer/SkyDebugHandlers.h"

#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Rendering/Sky/SkySettings.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

json Vector3(const float (&v)[3])
{
    return json::array({v[0], v[1], v[2]});
}

// Every float is reported as a double converted from the float, which is exact, so equal JSON means
// equal bits (apart from the sign of zero, which JSON keeps as -0).
json DescribeSkySettings(const Rendering::SkySettings& s)
{
    return json{
        {"timeOfDayHours", s.timeOfDayHours},
        {"exposureEV", s.exposureEV},
        {"scatteringSunDir", Vector3(s.scatteringSunDir)},
        {"primarySunDir", Vector3(s.primarySunDir)},
        {"primarySunColor", Vector3(s.primarySunColor)},
        {"primarySunGroundColor", Vector3(s.primarySunGroundColor)},
        {"primarySunIntensity", s.primarySunIntensity},
        {"showSunDisk", s.showSunDisk},
        {"sunSize", s.sunSize},
        {"moonDirWS", Vector3(s.moonDirWS)},
        {"moonIntensity", s.moonIntensity},
        {"moonAngularRadius", s.moonAngularRadius},
        {"showMoonDisk", s.showMoonDisk},
        {"moonExposureEV", s.moonExposureEV},
        {"moonArcPosition", s.moonArcPosition},
        {"moonPhase01", s.moonPhase01},
        {"fallingStarsEnabled", s.fallingStarsEnabled},
        {"nightSkyBlend", s.nightSkyBlend},
        {"skyTimeSeconds", s.skyTimeSeconds},
        {"starDensity", s.starDensity},
        {"starBrightness", s.starBrightness},
        {"twinkleSpeed", s.twinkleSpeed},
        {"twinkleIntensity", s.twinkleIntensity},
        {"iblIntensity", s.iblIntensity},
        {"iblLowerHemisphereDarkness", s.iblLowerHemisphereDarkness},
        {"skyMode", s.skyMode},
        {"groundAlbedo", Vector3(s.groundAlbedo)},
        {"groundNightColor", Vector3(s.groundNightColor)},
        {"groundBrightness", s.groundBrightness},
        {"belowHorizonMode", s.belowHorizonMode},
        {"belowHorizonBlendSharpness", s.belowHorizonBlendSharpness},
        {"belowHorizonDarkness", s.belowHorizonDarkness},
        {"belowHorizonDarkColor", Vector3(s.belowHorizonDarkColor)},
        {"groundHazeStrength", s.groundHazeStrength},
        {"groundHorizonColor", Vector3(s.groundHorizonColor)},
        {"groundHorizonNightColor", Vector3(s.groundHorizonNightColor)},
        {"nightSkyHorizonColor", Vector3(s.nightSkyHorizonColor)},
        {"groundHorizonCosWidth", s.groundHorizonCosWidth},
        {"groundHorizonNightCosWidth", s.groundHorizonNightCosWidth}};
}

} // namespace

void RegisterSkyDebugHandlers(EditorDebugServer& server)
{
    // get_sky_settings — the SkySettings the sky feature holds for this frame. "active" is false
    // when no sky is drawn (no enabled Sky Environment, or an HDRI skybox without one).
    server.RegisterHandler("get_sky_settings", [](const EditorDebugServer::RequestContext&) -> json
    {
        auto* renderServices = EngineCore::GetInstance().GetRenderServices();
        const auto* sky = renderServices ? renderServices->GetFeature<Engine::Renderer::SkyRenderFeature>() : nullptr;
        if (!sky)
            return Editor::RefuseRequest("No sky feature: the renderer has not created one");
        if (!sky->HasActiveSettings())
            return json{{"active", false}};
        return json{{"active", true}, {"settings", DescribeSkySettings(sky->GetSettings())}};
    });
}

} // namespace GameEngine
