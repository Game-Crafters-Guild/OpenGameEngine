#pragma once

#include <string>

namespace GameEngine {
namespace Platform {

// Clipboard utilities (requires GLFW to be initialized)

// Get the current clipboard text content.
// Returns an empty string if clipboard is empty or doesn't contain text.
std::string GetClipboardText();

// Set the clipboard text content.
// utf8: The text to copy to clipboard (UTF-8 encoded). Pass nullptr or empty string to clear.
void SetClipboardText(const char* utf8);

} // namespace Platform
} // namespace GameEngine
