#include "Platform/Toolbar.h"
#include "Platform/Window.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <memory>

namespace GameEngine {

class NoopToolbar : public INativeToolbar {
public:
    bool Install(Platform::Window* /*window*/) override { return true; }
    void Uninstall() override {}
    void Clear() override {}
    void SetCommandHandler(CommandCallback /*cb*/) override {}
    uint32_t AddMenu(const std::string& /*title*/) override { return 1; }
    uint32_t AddSubMenu(uint32_t /*parentMenuId*/, const std::string& /*title*/) override { return 1; }
    void     AddItem(uint32_t /*parentMenuId*/, const std::string& /*title*/, uint32_t /*commandId*/) override {}
    void     SetItemIcon(uint32_t /*commandId*/, const std::string& /*imagePath*/) override {}
};

std::unique_ptr<INativeToolbar> CreateNativeToolbar() {
    return std::make_unique<NoopToolbar>();
}

} // namespace GameEngine

#endif // !_WIN32 && !__APPLE__
