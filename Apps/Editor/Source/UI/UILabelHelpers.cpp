#include "UI/UILabelHelpers.h"

#include "Platform/SystemMetrics.h"

#include <chrono>
#include <memory>

#include "Editor/Settings/SettingsStore.h"
#include "UI/UIEvents.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Vector3Field.h"
#include "Rendering/Common/Math.h"

namespace Editor
{

void AddLabelDoubleClickReset(GameEngine::Label* label,
                              GameEngine::Slider* slider,
                              float defaultValue,
                              const std::string& prefKey,
                              std::function<void(float)> onReset)
{
    if (!label || !slider)
        return;

    auto lastClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
    label->RegisterEventHandler(GameEngine::kEventMouseUp,
        [slider, defaultValue, prefKey, onReset, lastClickTime](GameEngine::UIEvent& e) {
            auto now = std::chrono::steady_clock::now();

            if ((now - *lastClickTime) < GameEngine::Platform::GetDoubleClickInterval())
            {
                // Reset to default
                slider->SetValue(defaultValue);
                if (onReset)
                    onReset(defaultValue);
                // Save to preferences
                if (!prefKey.empty())
                {
                    auto prefs = GameEngine::Editor::OpenEditorPreferences();
                    std::string err;
                    prefs.Load(&err);
                    prefs.SetDouble(prefKey, defaultValue);
                    prefs.Save(&err);
                }
                *lastClickTime = {}; // Reset to prevent triple-click triggering
            }
            else
            {
                *lastClickTime = now;
            }
            e.Stop();
        });
}

void AddLabelDoubleClickReset(GameEngine::Label* label,
                              GameEngine::Vector3Field* field,
                              const GameEngine::Rendering::Vector3& defaultValue,
                              std::function<void(const GameEngine::Rendering::Vector3&)> onReset)
{
    if (!label || !field)
        return;

    auto lastClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
    label->RegisterEventHandler(GameEngine::kEventMouseUp,
        [field, defaultValue, onReset, lastClickTime](GameEngine::UIEvent& e) {
            auto now = std::chrono::steady_clock::now();
            
            if ((now - *lastClickTime) < GameEngine::Platform::GetDoubleClickInterval())
            {
                // Reset to default
                field->SetValue(defaultValue);
                if (onReset)
                    onReset(defaultValue);
                *lastClickTime = {}; // Reset to prevent triple-click triggering
            }
            else
            {
                *lastClickTime = now;
            }
            e.Stop();
        });
}

} // namespace Editor
