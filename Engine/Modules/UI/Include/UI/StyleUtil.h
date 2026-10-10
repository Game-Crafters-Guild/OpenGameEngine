#pragma once

#include <string>

namespace GameEngine {
namespace UIUtil {

// Normalize CSS url(...) inner path used by the UI system
// - Strips optional "asset:" scheme
// - Does NOT strip a leading '/' (disallowed form by design)
inline std::string NormalizeCssUrlPath(std::string p)
{
    if (p.rfind("asset:", 0) == 0) p = p.substr(6);
    return p;
}

} // namespace UIUtil
} // namespace GameEngine

