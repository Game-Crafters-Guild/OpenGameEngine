#include "Platform/WebEnvironment.h"

#include <emscripten/emscripten.h>

#include <cstdlib>
#include <string>

namespace GameEngine::Platform::Web
{

void ImportEnvironmentFromUrl()
{
    // One JS hop for the whole set: the pairs come back as a NUL-separated
    // name/value block, terminated by the empty name the loop below stops on.
    char* packed = static_cast<char*>(EM_ASM_PTR({
        const params = new URLSearchParams(globalThis.location.search);
        const nul = String.fromCharCode(0);
        let packed = '';
        for (const [name, value] of params)
        {
            // NUL separates the C++ name/value pairs. Reject embedded NULs
            // so a URL cannot forge another name or consume the terminator.
            if (name.startsWith('GE_') && !name.includes(nul) && !value.includes(nul))
            {
                packed += name + nul + value + nul;
            }
        }
        return packed.length === 0 ? 0 : stringToNewUTF8(packed);
    }));
    if (packed == nullptr)
    {
        return;
    }

    const char* cursor = packed;
    while (*cursor != '\0')
    {
        const std::string name(cursor);
        cursor += name.size() + 1;
        const std::string value(cursor);
        cursor += value.size() + 1;
        // Overwrite: a URL is the most specific statement of intent a page run
        // has, and nothing earlier could have set a GE_ name anyway.
        ::setenv(name.c_str(), value.c_str(), /*overwrite*/ 1);
    }
    std::free(packed);
}

} // namespace GameEngine::Platform::Web
