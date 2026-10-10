#include "Editor/Settings/EditorSettingsRegistry.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
// Whether a field can produce the value (or the action) its row needs. Rows
// that hold no value answer for their own wiring instead.
bool FieldIsWired(const SettingsFieldDescriptor& field)
{
    using Field = SettingsFieldDescriptor;
    if (const auto* button = std::get_if<Field::ButtonField>(&field.Control))
        return static_cast<bool>(button->OnClick);
    if (const auto* custom = std::get_if<Field::CustomField>(&field.Control))
        return static_cast<bool>(custom->CreateRow);
    // Color rows have no preference-key load path; they must be accessor-backed.
    if (const auto* color = std::get_if<Field::ColorField>(&field.Control))
        return static_cast<bool>(color->Get);
    if (const auto* toggleColor = std::get_if<Field::ToggleColorField>(&field.Control))
        return static_cast<bool>(toggleColor->GetEnabled) && static_cast<bool>(toggleColor->GetColor);
    if (!field.PrefKey.empty())
        return true;
    return std::visit(
        [](const auto& control)
        {
            using Control = std::decay_t<decltype(control)>;
            if constexpr (std::is_same_v<Control, Field::ButtonField> ||
                          std::is_same_v<Control, Field::CustomField> ||
                          std::is_same_v<Control, Field::ToggleColorField> ||
                          std::is_same_v<Control, Field::ColorField>)
                return false; // handled above
            else
                return static_cast<bool>(control.Get);
        },
        field.Control);
}

// Button and custom rows carry their own label (or none at all), so only
// value-bearing rows need the label column filled in.
bool FieldNeedsLabel(const SettingsFieldDescriptor& field)
{
    return !std::holds_alternative<SettingsFieldDescriptor::ButtonField>(field.Control) &&
           !std::holds_alternative<SettingsFieldDescriptor::CustomField>(field.Control);
}
} // namespace

EditorSettingsRegistry& EditorSettingsRegistry::Get()
{
    static EditorSettingsRegistry s_Instance;
    return s_Instance;
}

void EditorSettingsRegistry::RegisterCategory(SettingsCategoryDescriptor descriptor)
{
    if (descriptor.CategoryId.empty() || descriptor.Title.empty())
    {
        Logger::Log::Error("EditorSettingsRegistry: rejecting category registration '{}' — "
                           "CategoryId and Title are required",
                           descriptor.CategoryId.empty() ? descriptor.Title : descriptor.CategoryId);
        return;
    }
    for (const SettingsFieldDescriptor& field : descriptor.Fields)
    {
        if ((FieldNeedsLabel(field) && field.Label.empty()) || !FieldIsWired(field))
        {
            Logger::Log::Error("EditorSettingsRegistry: rejecting category '{}' — field '{}' "
                               "needs a Label and either a PrefKey, a Get accessor, an OnClick "
                               "or a CreateRow",
                               descriptor.CategoryId, field.Label);
            return;
        }
    }

    const auto sameId = [&](const Entry& existing)
    {
        return existing.Descriptor.CategoryId == descriptor.CategoryId;
    };
    const auto existing = std::find_if(m_Categories.begin(), m_Categories.end(), sameId);
    if (existing != m_Categories.end())
    {
        // Replace-forward: the previous module DLL stays mapped for the process
        // lifetime, so swapping the descriptor in place is safe and keeps the
        // snapshot index (and therefore the settings-tree id) stable.
        Logger::Log::Info("EditorSettingsRegistry: category '{}' re-registered (module reload); "
                          "replacing forward",
                          descriptor.CategoryId);
        existing->Descriptor = std::move(descriptor);
        existing->Module = ECS::GetActiveRegistrationModule();
        if (m_Observer)
            m_Observer(existing->Descriptor);
        return;
    }

    m_Categories.push_back(Entry{std::move(descriptor), ECS::GetActiveRegistrationModule()});
    if (m_Observer)
        m_Observer(m_Categories.back().Descriptor);
}

std::vector<SettingsCategoryDescriptor> EditorSettingsRegistry::Snapshot() const
{
    std::vector<SettingsCategoryDescriptor> result;
    result.reserve(m_Categories.size());
    for (const Entry& entry : m_Categories)
        result.push_back(entry.Descriptor);
    return result;
}

bool EditorSettingsRegistry::TryGet(std::string_view categoryId,
                                    SettingsCategoryDescriptor& outDescriptor) const
{
    for (const Entry& entry : m_Categories)
    {
        if (entry.Descriptor.CategoryId == categoryId)
        {
            outDescriptor = entry.Descriptor;
            return true;
        }
    }
    return false;
}

void EditorSettingsRegistry::SetRegistrationObserver(RegistrationObserver observer)
{
    m_Observer = std::move(observer);

    // Replay for categories that registered before the observer existed, so
    // attach order never decides whether a category is seen.
    if (m_Observer)
        for (const Entry& entry : m_Categories)
            m_Observer(entry.Descriptor);
}

void EditorSettingsRegistry::AppendModulePins(std::string_view moduleId,
                                              std::vector<std::string>& outPins) const
{
    if (moduleId.empty())
        return;
    for (const Entry& entry : m_Categories)
    {
        if (entry.Module.ModuleId == moduleId)
            outPins.push_back("Settings category '" + entry.Descriptor.CategoryId + "'");
    }
}

} // namespace GameEngine::Editor
