// C12 editor-kind unload refusal diagnostics — the editor registries stamp
// registrations with the loader's active-module bracket and answer "what pins
// module X" via AppendModulePins. The refusal in
// NativeScriptManager::UnloadSupersededModuleVersions folds these answers into
// one loud warning naming the pinning entries.

#include "ECS/ModuleRegistration.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "InspectorRegistry.h"
#include "Picking/EditorPickProviders.h"
#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace ed = GameEngine::Editor;
namespace ecs = GameEngine::ECS;

namespace
{
struct StampScope
{
    StampScope(std::string_view moduleId, std::uint64_t generation)
    {
        ecs::SetActiveRegistrationModule(moduleId, generation);
    }
    ~StampScope() { ecs::ClearActiveRegistrationModule(); }
};

bool HasPinContaining(const std::vector<std::string>& pins, std::string_view needle)
{
    return std::any_of(pins.begin(), pins.end(), [&](const std::string& pin) {
        return pin.find(needle) != std::string::npos;
    });
}

class PinTestPlugin final : public ed::IEditorPlugin
{
public:
    PinTestPlugin() { m_Descriptor = {"test.pin.plugin", "Pin Test Plugin", "1.0.0", true}; }
    const GameEngine::Plugins::PluginDescriptor& GetDescriptor() const override { return m_Descriptor; }

private:
    GameEngine::Plugins::PluginDescriptor m_Descriptor;
};
} // namespace

// Registrations made inside a module bracket carry the stamp; AppendModulePins
// names them per registry, and other modules see nothing.
TEST(EditorSdkModulePins, StampedRegistrationsAreReported)
{
    static PinTestPlugin plugin; // registry keeps the pointer for process life
    constexpr ecs::ComponentTypeId kTraitsType = 0xE001;
    constexpr ecs::ComponentTypeId kPickType = 0xE002;
    constexpr ecs::ComponentTypeId kInspectorType = 0xE003;

    {
        StampScope stamp("PinPack", 1);
        ed::EditorPluginRegistry::Get().RegisterPlugin(plugin);

        ed::EditorPanelDescriptor panel;
        panel.PanelId = "PinTestPanel";
        panel.Factory = [] { return std::unique_ptr<GameEngine::UIElement>(); };
        ed::EditorPanelRegistry::Get().RegisterPanel(std::move(panel));
        ed::EditorPanelRegistry::Get().RegisterEditorStyleSheet({"pinpack", "Editor/UI/Pin.css"});

        ed::EditorComponentTraits traits;
        traits.DisplayName = "PinTraits";
        ed::EditorComponentTraitsRegistry::Get().Register(kTraitsType, std::move(traits));

        ed::Picking::EditorPickProvider provider;
        provider.Resolve = [](ecs::World&, ecs::EntityHandle, ed::Picking::SyntheticMeshPick&) {
            return false;
        };
        ed::Picking::EditorPickProviderRegistry::Get().Register(kPickType, std::move(provider));

        GameEngine::InspectorRegistry::Get().RegisterComponentInspectorByTypeId(
            kInspectorType, [](const GameEngine::InspectorContext&) {});
        GameEngine::InspectorRegistry::Get().RegisterAssetInspector(
            GameEngine::AssetType::NavigationGrid,
            [](const GameEngine::InspectorContext&) {});
    }

    std::vector<std::string> pins;
    ed::EditorPluginRegistry::Get().AppendModulePins("PinPack", pins);
    ed::EditorPanelRegistry::Get().AppendModulePins("PinPack", pins);
    ed::EditorComponentTraitsRegistry::Get().AppendModulePins("PinPack", pins);
    ed::Picking::EditorPickProviderRegistry::Get().AppendModulePins("PinPack", pins);
    GameEngine::InspectorRegistry::Get().AppendModulePins("PinPack", pins);

    EXPECT_TRUE(HasPinContaining(pins, "editor plugin 'test.pin.plugin'"));
    EXPECT_TRUE(HasPinContaining(pins, "dockable panel 'PinTestPanel'"));
    EXPECT_TRUE(HasPinContaining(pins, "editor stylesheet 'pinpack:Editor/UI/Pin.css'"));
    EXPECT_TRUE(HasPinContaining(pins, "component traits"));
    EXPECT_TRUE(HasPinContaining(pins, "pick provider"));
    EXPECT_TRUE(HasPinContaining(pins, "component inspector"));
    EXPECT_TRUE(HasPinContaining(pins, "asset inspector"));
    EXPECT_TRUE(GameEngine::InspectorRegistry::Get().IsComponentInspectorOwnedBy(
        kInspectorType, "PinPack"));
    EXPECT_TRUE(GameEngine::InspectorRegistry::Get().IsAssetInspectorOwnedBy(
        GameEngine::AssetType::NavigationGrid, "PinPack"));

    // Another module sees none of them.
    std::vector<std::string> otherPins;
    ed::EditorPluginRegistry::Get().AppendModulePins("OtherPack", otherPins);
    ed::EditorPanelRegistry::Get().AppendModulePins("OtherPack", otherPins);
    ed::EditorComponentTraitsRegistry::Get().AppendModulePins("OtherPack", otherPins);
    ed::Picking::EditorPickProviderRegistry::Get().AppendModulePins("OtherPack", otherPins);
    GameEngine::InspectorRegistry::Get().AppendModulePins("OtherPack", otherPins);
    EXPECT_TRUE(otherPins.empty());
}

// A reload replay (newer generation) re-stamps in place — the pin stays
// singular, attributed to the module, not duplicated per generation.
TEST(EditorSdkModulePins, ReloadRestampKeepsSinglePin)
{
    {
        StampScope stamp("PinPack2", 1);
        ed::EditorPanelDescriptor panel;
        panel.PanelId = "PinReloadPanel";
        panel.Factory = [] { return std::unique_ptr<GameEngine::UIElement>(); };
        ed::EditorPanelRegistry::Get().RegisterPanel(std::move(panel));
    }
    {
        StampScope stamp("PinPack2", 2);
        ed::EditorPanelDescriptor panel;
        panel.PanelId = "PinReloadPanel";
        panel.Factory = [] { return std::unique_ptr<GameEngine::UIElement>(); };
        ed::EditorPanelRegistry::Get().RegisterPanel(std::move(panel));
    }

    std::vector<std::string> pins;
    ed::EditorPanelRegistry::Get().AppendModulePins("PinPack2", pins);
    EXPECT_EQ(std::count_if(pins.begin(), pins.end(),
                            [](const std::string& p) {
                                return p.find("PinReloadPanel") != std::string::npos;
                            }),
              1);
}

// Unstamped (editor-built-in) registrations never appear in any module's pins.
TEST(EditorSdkModulePins, UnstampedRegistrationsInvisible)
{
    ed::EditorPanelDescriptor panel;
    panel.PanelId = "BuiltInPanel";
    panel.Factory = [] { return std::unique_ptr<GameEngine::UIElement>(); };
    ed::EditorPanelRegistry::Get().RegisterPanel(std::move(panel));

    std::vector<std::string> pins;
    ed::EditorPanelRegistry::Get().AppendModulePins("PinPack3", pins);
    EXPECT_FALSE(HasPinContaining(pins, "BuiltInPanel"));
}
