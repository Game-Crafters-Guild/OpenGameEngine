#pragma once

#include "AssistantMode.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace GameEngine
{
namespace Editor
{
struct SettingsCategoryDescriptor;
}

/// The AI Assistant's user settings: which connection the panel talks to, each
/// connection's model, the Claude session's effort (and, in memory, the model that
/// answered each), the two CLI executables, the Node.js executable that runs the
/// editor's MCP server and the mode new conversations start in, kept in the editor
/// preferences under `aiAssistant.*`; and each local session's last session and its
/// mode per project, kept in the per-user settings of that project. There is no key setting: the Claude (API) connection reads
/// its key from the environment when a turn starts (ClaudeApiProvider).
class AiAssistantSettings
{
public:
    /// The settings page id and the prefix of every preference key.
    static constexpr std::string_view kCategoryId = "aiAssistant";

    /// A value the user can choose: a connection, a model or an effort.
    struct Choice
    {
        /// The stored value: the provider's IAgentProvider::Id(), a model as the
        /// connection names it, an effort level.
        std::string_view Id;
        /// What the settings page and the panel show.
        std::string_view Label;
    };
    /// Every connection, in the order the settings page and the panel list them.
    static std::span<const Choice> ProviderChoices();

    /// The connection id the panel uses (an IAgentProvider::Id()); the Claude
    /// session when none was chosen or the stored one is unknown.
    static std::string Provider();
    /// Stores the connection id; an unknown id is ignored.
    static void SetProvider(const std::string& providerId);
    /// Counts the SetProvider() calls that changed the stored connection in this
    /// process, so an open panel can follow a choice made on the settings page.
    static uint64_t ProviderGeneration();

    /// The models the settings page and the panel offer for `providerId`, the initial
    /// choice first; empty for a connection whose model is free text (Codex). The
    /// Claude session offers the CLI's family aliases, never a version.
    static std::span<const Choice> ModelChoices(std::string_view providerId);
    /// The model a turn of `providerId` asks for: the stored ModelChoices() id, the
    /// first one when none is stored or the stored one is no longer offered (said once
    /// in the log); Codex's free text, empty for its own default.
    static std::string Model(std::string_view providerId);
    /// Stores `modelId` for `providerId`; an id outside ModelChoices() is ignored.
    static void SetModel(std::string_view providerId, std::string_view modelId);
    /// What the panel and the settings page call `modelId` of `providerId`: the model
    /// that last answered a turn asking for it ("Opus 5.5", AnsweredModel()), else its
    /// ModelChoices() label ("Opus (latest)"), else the id in its vendor's words.
    static std::string ModelLabel(std::string_view providerId, std::string_view modelId);
    /// The model that answered this process's last successful turn of `providerId`
    /// that asked for `modelId`, as the provider named it; empty until one has.
    static std::string AnsweredModel(std::string_view providerId, std::string_view modelId);
    /// Records the model that answered a successful turn of `providerId` that asked
    /// for `modelId`; a change bumps ModelGeneration().
    static void SetAnsweredModel(std::string_view providerId, std::string_view modelId,
                                 std::string_view answeredModel);

    /// The efforts the settings page and the panel offer for `providerId`, lowest
    /// first; empty for a connection without effort levels.
    static std::span<const Choice> EffortChoices(std::string_view providerId);
    /// The effort a turn of `providerId` asks for: the stored EffortChoices() id,
    /// High when none is stored or the stored one is no longer offered (said once in
    /// the log); empty for a connection without effort levels.
    static std::string Effort(std::string_view providerId);
    /// Stores `effortId` for `providerId`; an id outside EffortChoices() is ignored.
    static void SetEffort(std::string_view providerId, std::string_view effortId);
    /// Counts the changes of a stored model or effort and of an answered model in this
    /// process, so the panel's model button follows a choice made on the settings page
    /// and names the model that answered.
    static uint64_t ModelGeneration();

    /// The `claude` executable: a program name searched for on PATH or a full path.
    static std::string ClaudeExecutable();
    /// The `codex` executable; empty until the user sets it, because Codex is not
    /// normally on PATH.
    static std::string CodexExecutable();

    /// The Node.js executable that runs the editor's MCP server for a conversation
    /// that acts in the editor: a program name searched for on PATH or a full path.
    static std::string NodeExecutable();

    /// The mode a new conversation starts in: the one last chosen, Auto until the
    /// user first chooses.
    static AssistantMode LastMode();
    /// Stores the mode last chosen.
    static void SetLastMode(AssistantMode mode);

    /// The mode the session `sessionId` of `providerId` last ran in, in the project at
    /// `projectRoot`; nullopt when that session is not the connection's last session
    /// there (a session this editor never ran, or an older one).
    static std::optional<AssistantMode> SessionMode(std::string_view providerId,
                                                    const std::filesystem::path& projectRoot,
                                                    std::string_view sessionId);
    /// Stores `mode` as the mode of the session `sessionId` of `providerId`, beside its
    /// last session (`aiAssistant.sessionMode.<providerId>`). Does nothing when
    /// `projectRoot` or `sessionId` is empty.
    static void SetSessionMode(std::string_view providerId, const std::filesystem::path& projectRoot,
                               std::string_view sessionId, AssistantMode mode);

    /// The session `providerId` last ran in the project at `projectRoot`, kept in the
    /// editor's per-user settings for that project (`aiAssistant.session.<providerId>`)
    /// so a later editor run can resume it; empty when none is stored or
    /// `projectRoot` is empty.
    static std::string LastSession(std::string_view providerId, const std::filesystem::path& projectRoot);
    /// Stores the session `providerId` runs in the project at `projectRoot`; an empty
    /// `sessionId` removes it. Does nothing when `projectRoot` is empty. Reads and
    /// writes the settings file: call it off the UI thread when it may run often.
    static void SetLastSession(std::string_view providerId, const std::filesystem::path& projectRoot,
                               const std::string& sessionId);

    /// The settings page: Provider, the three Model rows, the Claude session effort,
    /// the mode new conversations start in, the two executables, the Node executable
    /// and the Login status row.
    static Editor::SettingsCategoryDescriptor BuildCategory();
    /// Registers BuildCategory() with the editor's settings registry.
    static void Register();
};
} // namespace GameEngine
