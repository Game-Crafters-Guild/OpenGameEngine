#include "Platform/SystemFonts.h"

namespace GameEngine::Platform
{

bool TryResolveSystemFontFile(std::string_view /*family*/, int /*weight*/, SystemFontStyle /*style*/, SystemFontFile& /*out*/)
{
    return false;
}

} // namespace GameEngine::Platform

