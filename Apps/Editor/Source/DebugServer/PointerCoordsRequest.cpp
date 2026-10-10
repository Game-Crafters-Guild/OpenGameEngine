#include "DebugServer/PointerCoordsRequest.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/InjectedInput.h"

#include <string>

namespace GameEngine::Editor
{

bool ReadPointerCoords(const nlohmann::json& params, const char* xKey, const char* yKey, float& x, float& y,
                       nlohmann::json& error)
{
    x = params[xKey].get<float>();
    y = params[yKey].get<float>();
    if (IsInjectablePointerPosition(x, y))
        return true;

    error = RefuseRequest(std::string(xKey) + "/" + yKey +
                              " must be finite numbers in float range (got " + xKey + "=" +
                              params[xKey].dump() + ", " + yKey + "=" + params[yKey].dump() +
                              "). Pass UI-logical pixel coordinates.");
    return false;
}

} // namespace GameEngine::Editor
