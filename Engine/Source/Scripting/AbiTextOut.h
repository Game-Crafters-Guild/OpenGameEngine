#pragma once

// AbiTextOut.h - The copy-what-fits text-out contract of the scripting ABI, shared by
// GE_UIElement_GetValueText, GE_UIElement_GetDropdownSelectedLabel and GE_Model_GetExtras.
// Two older exports keep contracts of their own: GE_Platform_* (PlatformServicesABI.cpp) accepts a null
// buffer with a positive size, and GE_ECSABI_GetName NUL-terminates what it copies.

#include "Scripting/ScriptingABI.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace GameEngine::ScriptingAbi
{

/**
 * @brief Copies min(text length, bufferLen) UTF-8 bytes of `text` into `buffer`, with no NUL terminator,
 * and always reports the text's full byte length in *outLen, so a short buffer is a visible retry rather
 * than silent truncation. buffer may be null only when bufferLen is 0: that is the call that asks the
 * length.
 *
 * @return GE_Result_InvalidArg for a null outLen, a negative bufferLen or a null buffer with a nonzero
 *         bufferLen; GE_Result_Ok otherwise.
 */
inline GE_Result CopyTextOut(std::string_view text, char* buffer, int32_t bufferLen, int32_t* outLen)
{
    if (!outLen || bufferLen < 0 || (!buffer && bufferLen != 0))
        return GE_Result_InvalidArg;
    *outLen = static_cast<int32_t>(text.size());
    const int32_t copy = std::min<int32_t>(*outLen, bufferLen);
    if (copy > 0)
        std::memcpy(buffer, text.data(), static_cast<size_t>(copy));
    return GE_Result_Ok;
}

} // namespace GameEngine::ScriptingAbi
