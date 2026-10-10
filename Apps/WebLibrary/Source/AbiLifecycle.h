#pragma once

#include <string_view>

namespace GameEngine::WebLibrary
{

class WebLibraryApplication;

/// The engine ge_create brought up, or null before it succeeds and after ge_shutdown.
WebLibraryApplication* GetRunningApplication();

/// True, with the last error naming `call`, after ge_shutdown: every later call fails.
bool RefuseAfterShutdown(std::string_view call);

} // namespace GameEngine::WebLibrary
