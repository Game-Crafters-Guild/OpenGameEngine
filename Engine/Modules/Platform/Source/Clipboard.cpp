#include "Platform/Clipboard.h"
#include <GLFW/glfw3.h>

namespace GameEngine {
namespace Platform {

std::string GetClipboardText()
{
    const char* s = glfwGetClipboardString(nullptr);
    return s ? std::string(s) : std::string();
}

void SetClipboardText(const char* utf8)
{
    glfwSetClipboardString(nullptr, utf8 ? utf8 : "");
}

} // namespace Platform
} // namespace GameEngine
