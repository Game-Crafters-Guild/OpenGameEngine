#include "AbiErrors.h"

#include <emscripten/emscripten.h>

#include <utility>

namespace GameEngine::WebLibrary
{

namespace
{

std::string g_LastError;
std::string g_ReturnedString;
// The export in progress (the one a suspension holds), or empty.
std::string_view g_CallInProgress;

} // namespace

void SetLastError(std::string message)
{
    g_LastError = std::move(message);
}

AbiCallScope::AbiCallScope(std::string_view call)
{
    if (!g_CallInProgress.empty())
    {
        m_Refused = true;
        SetLastError("{} was called while {} is suspended: no engine call may run until {} returns; await it "
                     "first.",
                     call, g_CallInProgress, g_CallInProgress);
        return;
    }
    g_CallInProgress = call;
}

AbiCallScope::~AbiCallScope()
{
    if (!m_Refused)
        g_CallInProgress = {};
}

const char* ReturnString(std::string text)
{
    g_ReturnedString = std::move(text);
    return g_ReturnedString.c_str();
}

} // namespace GameEngine::WebLibrary

extern "C"
{

/// The message of the last failed call. Callable at any time, during ge_create's suspension too.
EMSCRIPTEN_KEEPALIVE const char* ge_last_error()
{
    return GameEngine::WebLibrary::g_LastError.c_str();
}

} // extern "C"
