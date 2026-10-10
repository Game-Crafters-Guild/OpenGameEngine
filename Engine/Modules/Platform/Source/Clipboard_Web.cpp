// Clipboard on web. Emscripten's GLFW clipboard entry points are no-ops, so the
// desktop TU would silently lose every copy. This routes through the browser's
// async Clipboard API instead: copy fire-and-forgets writeText (called inside the
// Ctrl+C key gesture, where the browser allows it); paste blocks on readText via
// Asyncify (enabled in this build) so the synchronous engine API can return the
// text. Both need a secure context (https/localhost); readText may prompt once.

#include "Platform/Clipboard.h"

#include <emscripten/em_js.h>

#include <cstdlib>
#include <string>

// Returns a malloc'd UTF-8 string the caller frees, or 0 on failure (no
// permission, denied, empty, or not a secure context).
EM_ASYNC_JS(char*, GeWebClipboardRead, (), {
    try {
        const text = await navigator.clipboard.readText();
        if (!text)
            return 0;
        return stringToNewUTF8(text);
    } catch (e) {
        return 0;
    }
});

EM_JS(void, GeWebClipboardWrite, (const char* utf8), {
    try {
        navigator.clipboard.writeText(UTF8ToString(utf8)).catch(function () {});
    } catch (e) {
    }
});

namespace GameEngine
{
namespace Platform
{

std::string GetClipboardText()
{
    char* text = GeWebClipboardRead();
    if (text == nullptr)
        return {};
    std::string result(text);
    std::free(text);
    return result;
}

void SetClipboardText(const char* utf8)
{
    if (utf8 != nullptr)
        GeWebClipboardWrite(utf8);
}

} // namespace Platform
} // namespace GameEngine
