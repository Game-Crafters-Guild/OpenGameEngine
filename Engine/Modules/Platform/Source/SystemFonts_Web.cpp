#include "Platform/SystemFonts.h"

namespace GameEngine::Platform
{

// The browser sandbox has no enumerable system font files; text stacks on
// wasm load fonts from mounted asset data instead. Resolution always misses
// so callers take their bundled-font fallback.
bool TryResolveSystemFontFile(std::string_view /*family*/, int /*weight*/, SystemFontStyle /*style*/,
                              SystemFontFile& /*out*/)
{
    return false;
}

} // namespace GameEngine::Platform
