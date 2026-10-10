#include "AiAssistantSettings.h"

#include "AiAssistantLoginStatusRow.h"
#include "ModelDisplayName.h"
#include "Providers/ClaudeApiProvider.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CodexSessionProvider.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr const char* kPrefProvider = "aiAssistant.provider";
constexpr const char* kPrefClaudeSessionModel = "aiAssistant.claudeSessionModel";
constexpr const char* kPrefClaudeApiModel = "aiAssistant.claudeApiModel";
constexpr const char* kPrefCodexModel = "aiAssistant.codexModel";
constexpr const char* kPrefClaudeSessionEffort = "aiAssistant.claudeSessionEffort";
constexpr const char* kPrefClaudeExecutable = "aiAssistant.claudeExecutable";
constexpr const char* kPrefCodexExecutable = "aiAssistant.codexExecutable";
constexpr const char* kPrefNodeExecutable = "aiAssistant.nodeExecutable";
constexpr const char* kPrefLastMode = "aiAssistant.lastMode";
constexpr const char* kDefaultNodeExecutable = "node";
constexpr AssistantMode kInitialMode = AssistantMode::Auto;
// The effort a Claude session turn asks for until the user chooses one.
constexpr std::string_view kInitialClaudeSessionEffort = ClaudeSessionProvider::kEffortLevels[2];

using Field = Editor::SettingsFieldDescriptor;

std::atomic<uint64_t> s_ProviderGeneration{0};
std::atomic<uint64_t> s_ModelGeneration{0};

// Serializes the read-modify-write of a project's per-user settings: a turn's thread
// stores the session it ran while the panel stores the session's mode.
std::mutex s_ProjectSettingsMutex;

std::string ReadPreference(const char* key, std::string fallback)
{
    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    prefs.Load();
    prefs.TryGetString(key, fallback);
    return fallback;
}

void WritePreference(const char* key, const std::string& value)
{
    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    prefs.Load();
    std::string stored;
    if (prefs.TryGetString(key, stored) && stored == value)
        return;
    prefs.SetString(key, value);
    prefs.Save();
}

// The per-project key of a connection's last session.
std::string SessionKey(std::string_view providerId)
{
    return "aiAssistant.session." + std::string(providerId);
}

// The per-project key of the mode of a connection's last session, stored as
// "<mode id>:<session id>" so a mode never applies to another session.
std::string SessionModeKey(std::string_view providerId)
{
    return "aiAssistant.sessionMode." + std::string(providerId);
}

constexpr AiAssistantSettings::Choice kProviderChoices[] = {
    {ClaudeSessionProvider::kId, "Claude (local session)"},
    {CodexSessionProvider::kId, "Codex (local session)"},
    {ClaudeApiProvider::kId, "Claude (API)"},
};

// The CLI's family aliases, each the latest model of its family, the initial choice
// first; the version that answered is read from the turn (SetAnsweredModel).
constexpr AiAssistantSettings::Choice kClaudeSessionModels[] = {
    {"opus", "Opus (latest)"},
    {"fable", "Fable (latest)"},
    {"sonnet", "Sonnet (latest)"},
    {"haiku", "Haiku (latest)"},
};

// The API takes exact ids; resolving a family at run time is issue #3542.
constexpr AiAssistantSettings::Choice kClaudeApiModels[] = {
    {ClaudeApiProvider::kDefaultModel, "Opus 5.5"},
    {"claude-sonnet-5-5", "Sonnet 5.5"},
    {"claude-haiku-4-5", "Haiku 4.5"},
};

// ClaudeSessionProvider::kEffortLevels in their order.
constexpr AiAssistantSettings::Choice kClaudeSessionEfforts[] = {
    {ClaudeSessionProvider::kEffortLevels[0], "Low"},
    {ClaudeSessionProvider::kEffortLevels[1], "Medium"},
    {ClaudeSessionProvider::kEffortLevels[2], "High"},
    {ClaudeSessionProvider::kEffortLevels[3], "Extra high"},
    {ClaudeSessionProvider::kEffortLevels[4], "Max"},
};
static_assert(std::size(kClaudeSessionEfforts) == std::size(ClaudeSessionProvider::kEffortLevels));

// The models that answered, by "<providerId>\n<model asked for>"; this process only.
std::mutex s_AnsweredModelsMutex;
std::vector<std::pair<std::string, std::string>> s_AnsweredModels;

std::string AnsweredKey(std::string_view providerId, std::string_view modelId)
{
    return std::string(providerId) + "\n" + std::string(modelId);
}

// Says once per process that a stored value is no longer offered, and what is used.
void ReportUnknownOnce(std::atomic<bool>& reported, std::string_view what, std::string_view stored,
                       std::string_view used)
{
    if (!reported.exchange(true))
        Logger::Log::Warning("AI Assistant: the stored {} '{}' is no longer offered; using '{}'.", what, stored,
                             used);
}

std::atomic<bool> s_ReportedUnknownModel{false};
std::atomic<bool> s_ReportedUnknownEffort{false};

// The preference that holds the model of a connection with a ModelChoices() table.
const char* ModelKey(std::string_view providerId)
{
    if (providerId == ClaudeSessionProvider::kId)
        return kPrefClaudeSessionModel;
    if (providerId == ClaudeApiProvider::kId)
        return kPrefClaudeApiModel;
    return nullptr;
}

bool Contains(std::span<const AiAssistantSettings::Choice> choices, std::string_view id)
{
    return std::any_of(choices.begin(), choices.end(),
                       [id](const AiAssistantSettings::Choice& choice) { return choice.Id == id; });
}

std::vector<Field::DropdownField::Option> DropdownOptions(std::span<const AiAssistantSettings::Choice> choices)
{
    std::vector<Field::DropdownField::Option> options;
    for (const AiAssistantSettings::Choice& choice : choices)
        options.push_back({std::string(choice.Id), std::string(choice.Label)});
    return options;
}

bool IsKnownProvider(std::string_view id)
{
    for (const AiAssistantSettings::Choice& choice : kProviderChoices)
        if (choice.Id == id)
            return true;
    return false;
}

Field DropdownRow(std::string label, std::string tooltip, const char* key, std::string defaultValue,
                  std::vector<Field::DropdownField::Option> options)
{
    Field field;
    field.Label = std::move(label);
    field.Tooltip = std::move(tooltip);
    Field::DropdownField control;
    control.OptionsProvider = [options = std::move(options)] { return options; };
    control.DefaultValue = defaultValue;
    control.Get = [key, defaultValue] { return ReadPreference(key, defaultValue); };
    control.Set = [key](const std::string& value) { WritePreference(key, value); };
    field.Control = std::move(control);
    return field;
}

// A model row: the connection's ModelChoices(), each under its ModelLabel(), read and
// stored through Model() and SetModel() so the panel's button follows it.
Field ModelRow(std::string label, std::string tooltip, std::string_view providerId)
{
    Field field;
    field.Label = std::move(label);
    field.Tooltip = std::move(tooltip);
    Field::DropdownField control;
    control.OptionsProvider = [providerId]
    {
        std::vector<Field::DropdownField::Option> options;
        for (const AiAssistantSettings::Choice& choice : AiAssistantSettings::ModelChoices(providerId))
            options.push_back({std::string(choice.Id), AiAssistantSettings::ModelLabel(providerId, choice.Id)});
        return options;
    };
    control.DefaultValue = std::string(AiAssistantSettings::ModelChoices(providerId).front().Id);
    control.Get = [providerId] { return AiAssistantSettings::Model(providerId); };
    control.Set = [providerId](const std::string& value) { AiAssistantSettings::SetModel(providerId, value); };
    field.Control = std::move(control);
    return field;
}

Field EffortRow()
{
    Field field;
    field.Label = "Claude session effort";
    field.Tooltip = "How much a Claude (local session) turn reasons before it answers. The panel's model button "
                    "changes it too.";
    Field::DropdownField control;
    control.OptionsProvider = [] { return DropdownOptions(kClaudeSessionEfforts); };
    control.DefaultValue = std::string(kInitialClaudeSessionEffort);
    control.Get = [] { return AiAssistantSettings::Effort(ClaudeSessionProvider::kId); };
    control.Set = [](const std::string& value) { AiAssistantSettings::SetEffort(ClaudeSessionProvider::kId, value); };
    field.Control = std::move(control);
    return field;
}

Field ExecutableRow(std::string label, std::string tooltip, const char* key, std::string defaultValue)
{
    Field field;
    field.Label = std::move(label);
    field.Tooltip = std::move(tooltip);
    Field::PathField control;
    control.PathKind = Field::PathField::Kind::File;
    control.DefaultValue = defaultValue;
    control.Get = [key, defaultValue] { return ReadPreference(key, defaultValue); };
    control.Set = [key](const std::string& value) { WritePreference(key, value); };
    field.Control = std::move(control);
    return field;
}
} // namespace

std::span<const AiAssistantSettings::Choice> AiAssistantSettings::ProviderChoices()
{
    return kProviderChoices;
}

std::string AiAssistantSettings::Provider()
{
    std::string id = ReadPreference(kPrefProvider, std::string(ClaudeSessionProvider::kId));
    return IsKnownProvider(id) ? id : std::string(ClaudeSessionProvider::kId);
}

void AiAssistantSettings::SetProvider(const std::string& providerId)
{
    if (!IsKnownProvider(providerId) || providerId == Provider())
        return;
    WritePreference(kPrefProvider, providerId);
    s_ProviderGeneration.fetch_add(1, std::memory_order_relaxed);
}

uint64_t AiAssistantSettings::ProviderGeneration()
{
    return s_ProviderGeneration.load(std::memory_order_relaxed);
}

std::span<const AiAssistantSettings::Choice> AiAssistantSettings::ModelChoices(std::string_view providerId)
{
    if (providerId == ClaudeSessionProvider::kId)
        return kClaudeSessionModels;
    if (providerId == ClaudeApiProvider::kId)
        return kClaudeApiModels;
    return {};
}

std::string AiAssistantSettings::Model(std::string_view providerId)
{
    if (providerId == CodexSessionProvider::kId)
        return ReadPreference(kPrefCodexModel, {});
    const char* key = ModelKey(providerId);
    if (!key)
        return {};
    const std::string first(ModelChoices(providerId).front().Id);
    const std::string model = ReadPreference(key, first);
    if (Contains(ModelChoices(providerId), model))
        return model;
    ReportUnknownOnce(s_ReportedUnknownModel, "model", model, first);
    return first;
}

void AiAssistantSettings::SetModel(std::string_view providerId, std::string_view modelId)
{
    const char* key = ModelKey(providerId);
    if (!key || !Contains(ModelChoices(providerId), modelId) || modelId == Model(providerId))
        return;
    WritePreference(key, std::string(modelId));
    s_ModelGeneration.fetch_add(1, std::memory_order_relaxed);
}

std::string AiAssistantSettings::ModelLabel(std::string_view providerId, std::string_view modelId)
{
    if (const std::string answered = AnsweredModel(providerId, modelId); !answered.empty())
        return ModelDisplayName(answered);
    for (const Choice& choice : ModelChoices(providerId))
        if (choice.Id == modelId)
            return std::string(choice.Label);
    return ModelDisplayName(modelId);
}

std::string AiAssistantSettings::AnsweredModel(std::string_view providerId, std::string_view modelId)
{
    const std::string key = AnsweredKey(providerId, modelId);
    std::lock_guard lock(s_AnsweredModelsMutex);
    for (const auto& [asked, answered] : s_AnsweredModels)
        if (asked == key)
            return answered;
    return {};
}

void AiAssistantSettings::SetAnsweredModel(std::string_view providerId, std::string_view modelId,
                                           std::string_view answeredModel)
{
    const std::string key = AnsweredKey(providerId, modelId);
    {
        std::lock_guard lock(s_AnsweredModelsMutex);
        auto entry = std::find_if(s_AnsweredModels.begin(), s_AnsweredModels.end(),
                                  [&key](const auto& asked) { return asked.first == key; });
        if (entry == s_AnsweredModels.end())
            s_AnsweredModels.emplace_back(key, answeredModel);
        else if (entry->second != answeredModel)
            entry->second = answeredModel;
        else
            return;
    }
    s_ModelGeneration.fetch_add(1, std::memory_order_relaxed);
}

std::span<const AiAssistantSettings::Choice> AiAssistantSettings::EffortChoices(std::string_view providerId)
{
    if (providerId == ClaudeSessionProvider::kId)
        return kClaudeSessionEfforts;
    return {};
}

std::string AiAssistantSettings::Effort(std::string_view providerId)
{
    if (providerId != ClaudeSessionProvider::kId)
        return {};
    const std::string initial(kInitialClaudeSessionEffort);
    const std::string effort = ReadPreference(kPrefClaudeSessionEffort, initial);
    if (Contains(kClaudeSessionEfforts, effort))
        return effort;
    ReportUnknownOnce(s_ReportedUnknownEffort, "effort", effort, initial);
    return initial;
}

void AiAssistantSettings::SetEffort(std::string_view providerId, std::string_view effortId)
{
    if (!Contains(EffortChoices(providerId), effortId) || effortId == Effort(providerId))
        return;
    WritePreference(kPrefClaudeSessionEffort, std::string(effortId));
    s_ModelGeneration.fetch_add(1, std::memory_order_relaxed);
}

uint64_t AiAssistantSettings::ModelGeneration()
{
    return s_ModelGeneration.load(std::memory_order_relaxed);
}

std::string AiAssistantSettings::ClaudeExecutable()
{
    return ReadPreference(kPrefClaudeExecutable, std::string(ClaudeSessionProvider::kDefaultExecutable));
}

std::string AiAssistantSettings::CodexExecutable()
{
    return ReadPreference(kPrefCodexExecutable, {});
}

std::string AiAssistantSettings::NodeExecutable()
{
    return ReadPreference(kPrefNodeExecutable, kDefaultNodeExecutable);
}

AssistantMode AiAssistantSettings::LastMode()
{
    return ParseAssistantMode(ReadPreference(kPrefLastMode, {})).value_or(kInitialMode);
}

void AiAssistantSettings::SetLastMode(AssistantMode mode)
{
    WritePreference(kPrefLastMode, std::string(DescribeAssistantMode(mode).Id));
}

std::optional<AssistantMode> AiAssistantSettings::SessionMode(std::string_view providerId,
                                                              const std::filesystem::path& projectRoot,
                                                              std::string_view sessionId)
{
    if (projectRoot.empty() || sessionId.empty())
        return std::nullopt;
    std::lock_guard lock(s_ProjectSettingsMutex);
    Editor::SettingsStore settings = Editor::OpenUserProjectSettings(projectRoot);
    settings.Load();
    std::string stored;
    settings.TryGetString(SessionModeKey(providerId), stored);
    const std::size_t colon = stored.find(':');
    if (colon == std::string::npos || std::string_view(stored).substr(colon + 1) != sessionId)
        return std::nullopt;
    return ParseAssistantMode(std::string_view(stored).substr(0, colon));
}

void AiAssistantSettings::SetSessionMode(std::string_view providerId, const std::filesystem::path& projectRoot,
                                         std::string_view sessionId, AssistantMode mode)
{
    if (projectRoot.empty() || sessionId.empty())
        return;
    std::lock_guard lock(s_ProjectSettingsMutex);
    Editor::SettingsStore settings = Editor::OpenUserProjectSettings(projectRoot);
    settings.Load();
    const std::string key = SessionModeKey(providerId);
    const std::string value = std::string(DescribeAssistantMode(mode).Id) + ":" + std::string(sessionId);
    std::string stored;
    if (settings.TryGetString(key, stored) && stored == value)
        return;
    settings.SetString(key, value);
    settings.Save();
}

std::string AiAssistantSettings::LastSession(std::string_view providerId, const std::filesystem::path& projectRoot)
{
    if (projectRoot.empty())
        return {};
    std::lock_guard lock(s_ProjectSettingsMutex);
    Editor::SettingsStore settings = Editor::OpenUserProjectSettings(projectRoot);
    settings.Load();
    std::string sessionId;
    settings.TryGetString(SessionKey(providerId), sessionId);
    return sessionId;
}

void AiAssistantSettings::SetLastSession(std::string_view providerId, const std::filesystem::path& projectRoot,
                                         const std::string& sessionId)
{
    if (projectRoot.empty())
        return;
    std::lock_guard lock(s_ProjectSettingsMutex);
    Editor::SettingsStore settings = Editor::OpenUserProjectSettings(projectRoot);
    settings.Load();
    const std::string key = SessionKey(providerId);
    if (sessionId.empty())
    {
        if (!settings.Remove(key))
            return;
    }
    else
    {
        std::string stored;
        if (settings.TryGetString(key, stored) && stored == sessionId)
            return;
        settings.SetString(key, sessionId);
    }
    settings.Save();
}

Editor::SettingsCategoryDescriptor AiAssistantSettings::BuildCategory()
{
    Editor::SettingsCategoryDescriptor category;
    category.CategoryId = std::string(kCategoryId);
    category.Title = "AI Assistant";
    category.Group = Editor::SettingsCategoryGroup::UserSettings;
    category.TreeRowClass = "ai-assistant-row";
    category.SearchKeywords = "ai assistant agent claude codex model login session provider";
    category.Description =
        "Claude (local session) and Codex (local session) run your own claude and codex logins on this "
        "machine. Claude (API) reads ANTHROPIC_API_KEY from the editor's environment when a turn starts: set "
        "it before starting the editor. The editor never stores a key.";

    Field provider = DropdownRow("Provider", "The connection the AI Assistant panel talks to.", kPrefProvider,
                                 std::string(ClaudeSessionProvider::kId), DropdownOptions(kProviderChoices));
    // Through SetProvider, so an open panel follows the choice.
    std::get<Field::DropdownField>(provider.Control).Set = [](const std::string& value) { SetProvider(value); };
    category.Fields.push_back(std::move(provider));
    // Through SetModel and SetEffort, so the panel's model button follows the choice.
    category.Fields.push_back(ModelRow(
        "Claude session model",
        "The model a Claude (local session) turn asks for: the latest of the family, named by its version once "
        "one has answered. The panel's model button changes it too.",
        ClaudeSessionProvider::kId));
    category.Fields.push_back(EffortRow());
    category.Fields.push_back(ModelRow("Claude API model", "The model a Claude (API) turn asks for.",
                                       ClaudeApiProvider::kId));

    Field codexModel;
    codexModel.Label = "Codex model";
    codexModel.Tooltip = "The model a Codex (local session) turn asks for, as Codex names it; empty uses "
                         "Codex's own default.";
    Field::StringField codexModelControl;
    codexModelControl.Get = [] { return ReadPreference(kPrefCodexModel, {}); };
    codexModelControl.Set = [](const std::string& value) { WritePreference(kPrefCodexModel, value); };
    codexModel.Control = std::move(codexModelControl);
    category.Fields.push_back(std::move(codexModel));

    std::vector<Field::DropdownField::Option> modes;
    for (const AssistantModeChoice& choice : AssistantModeChoices())
        modes.push_back({std::string(choice.Id), std::string(choice.Label)});
    category.Fields.push_back(DropdownRow(
        "Mode for new conversations",
        "What the assistant may do in the editor when a new conversation starts; the panel's mode control "
        "changes it, and a resumed session keeps its own.",
        kPrefLastMode, std::string(DescribeAssistantMode(kInitialMode).Id), std::move(modes)));

    category.Fields.push_back(ExecutableRow(
        "Claude executable", "The claude program: a name found on PATH or a full path to claude.exe.",
        kPrefClaudeExecutable, std::string(ClaudeSessionProvider::kDefaultExecutable)));
    category.Fields.push_back(ExecutableRow(
        "Codex executable", "The full path to the codex program; Codex is usually not on PATH.",
        kPrefCodexExecutable, {}));
    category.Fields.push_back(ExecutableRow(
        "Node executable",
        "The node program that runs the editor's tools for the assistant (Node 20 or later): a name found on "
        "PATH or a full path to node.exe.",
        kPrefNodeExecutable, kDefaultNodeExecutable));

    Field login;
    login.Label = "Login status";
    login.Tooltip = "Asks the chosen local session's CLI whether it is logged in.";
    login.Control = Field::CustomField{[] { return std::unique_ptr<UIElement>(std::make_unique<AiAssistantLoginStatusRow>()); }};
    category.Fields.push_back(std::move(login));
    return category;
}

void AiAssistantSettings::Register()
{
    Editor::EditorSettingsRegistry::Get().RegisterCategory(BuildCategory());
}
} // namespace GameEngine
