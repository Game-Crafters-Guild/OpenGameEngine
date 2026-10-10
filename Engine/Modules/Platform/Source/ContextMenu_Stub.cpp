#include "Platform/ContextMenu.h"
#include "Platform/Window.h"

#if !defined(_WIN32) && !defined(__APPLE__)

#include <memory>

namespace GameEngine {

class NoopContextMenu : public INativeContextMenu {
public:
    void Clear() override {}

    uint32_t AddSubMenu(uint32_t /*parentId*/, const std::string& /*title*/) override { return 1; }

    void AddItem(uint32_t /*parentId*/, const std::string& /*title*/, uint32_t /*commandId*/, uint32_t /*ItemFlags*/) override {}

    void AddSeparator(uint32_t /*parentId*/) override {}

    void SetCommandHandler(CommandCallback /*cb*/) override {}

    void SetStateProvider(StateProviderCallback /*cb*/) override {}

    void SetItemEnabled(uint32_t /*commandId*/, bool /*enabled*/) override {}

    void SetItemChecked(uint32_t /*commandId*/, bool /*checked*/) override {}

    void Show(Platform::Window* /*window*/, int /*x*/, int /*y*/) override {}
};

std::unique_ptr<INativeContextMenu> CreateNativeContextMenu() {
    return std::make_unique<NoopContextMenu>();
}

} // namespace GameEngine

#endif // !_WIN32 && !__APPLE__

