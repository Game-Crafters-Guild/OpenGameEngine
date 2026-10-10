#include "UI/EditorUIFontSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "Engine/UI/FontResolver.h"
#include "Logger/Logger.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIStyle.h"
#include "UI/UIManager.h"

#include <cctype>
#include <optional>
#include <unordered_map>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

std::unordered_map<UIManager*, StylesheetHandle> s_FontPrefsSheetByUi;

static bool UiPresetToCss(const std::string& preset, std::string& outFontFamilyValue)
{
    if (preset.empty() || preset == "default")
        return false;
    if (preset == "roboto")
    {
        outFontFamilyValue = "\"Roboto\", sans-serif";
        return true;
    }
    if (preset == "inter")
    {
        outFontFamilyValue = "\"Inter\", system-ui, sans-serif";
        return true;
    }
    if (preset == "system_ui")
    {
        outFontFamilyValue = "system-ui, sans-serif";
        return true;
    }
    if (preset == "native_ui")
    {
        outFontFamilyValue = "-apple-system, BlinkMacSystemFont, \"Segoe UI\", sans-serif";
        return true;
    }
    return false;
}

// Mirror of `--ui_font_script` in Assets/UI/theme/tokens.css, used when no
// script font preset is saved. Keep the two in step.
static constexpr const char* kDefaultScriptFontStackCss =
    "\"Roboto Mono\", \"Cascadia Code\", \"Consolas\", \"Menlo\", \"DejaVu Sans Mono\", monospace";

static bool ScriptPresetToCss(const std::string& preset, std::string& outFontFamilyValue)
{
    if (preset.empty() || preset == "default")
        return false;
    if (preset == "roboto_mono")
    {
        outFontFamilyValue = "\"Roboto Mono\", monospace";
        return true;
    }
    if (preset == "consolas")
    {
        outFontFamilyValue = "\"Consolas\", \"Courier New\", monospace";
        return true;
    }
    if (preset == "cascadia_code")
    {
        outFontFamilyValue = "\"Cascadia Code\", \"Cascadia Mono\", \"Consolas\", monospace";
        return true;
    }
    if (preset == "jetbrains_mono")
    {
        outFontFamilyValue = "\"JetBrains Mono\", \"Consolas\", monospace";
        return true;
    }
    if (preset == "fira_code")
    {
        outFontFamilyValue = "\"Fira Code\", \"Fira Mono\", monospace";
        return true;
    }
    if (preset == "source_code_pro")
    {
        outFontFamilyValue = "\"Source Code Pro\", \"Consolas\", monospace";
        return true;
    }
    if (preset == "inconsolata")
    {
        outFontFamilyValue = "Inconsolata, \"Consolas\", monospace";
        return true;
    }
    if (preset == "ubuntu_mono")
    {
        outFontFamilyValue = "\"Ubuntu Mono\", \"DejaVu Sans Mono\", monospace";
        return true;
    }
    if (preset == "menlo")
    {
        outFontFamilyValue = "\"Menlo\", \"Monaco\", \"Consolas\", monospace";
        return true;
    }
    if (preset == "monaco")
    {
        outFontFamilyValue = "\"Monaco\", \"Menlo\", monospace";
        return true;
    }
    if (preset == "sf_mono")
    {
        outFontFamilyValue = "\"SF Mono\", \"Menlo\", \"Monaco\", monospace";
        return true;
    }
    if (preset == "lucida_console")
    {
        outFontFamilyValue = "\"Lucida Console\", \"Courier New\", monospace";
        return true;
    }
    if (preset == "courier")
    {
        outFontFamilyValue = "\"Courier New\", Courier, monospace";
        return true;
    }
    if (preset == "system_mono")
    {
        outFontFamilyValue =
            "ui-monospace, SFMono-Regular, \"Segoe UI Mono\", \"Roboto Mono\", \"Consolas\", monospace";
        return true;
    }
    return false;
}

static std::string TrimAscii(std::string_view s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return std::string(s.substr(b, e - b));
}

static std::string StripQuotesFamily(std::string t)
{
    t = TrimAscii(t);
    if (t.size() >= 2)
    {
        if ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))
            return TrimAscii(std::string_view(t).substr(1, t.size() - 2));
    }
    return t;
}

static std::vector<std::string> SplitFontFamilyCss(const std::string& css)
{
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(css.size());
    for (char c : css)
    {
        if (c == ',')
        {
            std::string t = TrimAscii(cur);
            if (!t.empty())
                out.push_back(std::move(t));
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    if (!cur.empty())
    {
        std::string t = TrimAscii(cur);
        if (!t.empty())
            out.push_back(std::move(t));
    }
    return out;
}

static bool IsSkippableFamilyTokenKey(const std::string& normKey)
{
    if (normKey.empty())
        return true;
    // CSS generics + OS UI keywords that are not concrete install names.
    static const char* kSkip[] = {"serif",
                                  "sans-serif",
                                  "monospace",
                                  "cursive",
                                  "fantasy",
                                  "system-ui",
                                  "ui-serif",
                                  "ui-sans-serif",
                                  "ui-monospace",
                                  "ui-rounded",
                                  "emoji",
                                  "math",
                                  "fangsong",
                                  "-apple-system",
                                  "blinkmacsystemfont"};
    for (const char* s : kSkip)
    {
        if (normKey == s)
            return true;
    }
    return false;
}

static std::optional<std::string> FirstConcreteFamilyForProbe(const std::string& cssStack)
{
    for (const std::string& rawTok : SplitFontFamilyCss(cssStack))
    {
        const std::string display = StripQuotesFamily(rawTok);
        const std::string norm = GameEngine::Engine::UI::FontFamilyIndex::NormalizeKey(display);
        if (IsSkippableFamilyTokenKey(norm))
            continue;
        return display;
    }
    return std::nullopt;
}

static bool ShouldIncludeUiFontPreset(GameEngine::Engine::UI::FontResolver& resolver, const std::string& presetId)
{
    if (presetId == "default" || presetId == "system_ui" || presetId == "native_ui")
        return true;
    std::string css;
    if (!UiPresetToCss(presetId, css))
        return false;
    const auto first = FirstConcreteFamilyForProbe(css);
    if (!first)
        return true;
    return resolver.TryIsFamilyResolvable(*first, 400, GameEngine::Engine::UI::FontStyle::Normal);
}

static bool ShouldIncludeScriptFontPreset(GameEngine::Engine::UI::FontResolver& resolver, const std::string& presetId)
{
    if (presetId == "default" || presetId == "system_mono")
        return true;
    std::string css;
    if (!ScriptPresetToCss(presetId, css))
        return false;
    const auto first = FirstConcreteFamilyForProbe(css);
    if (!first)
        return true;
    return resolver.TryIsFamilyResolvable(*first, 400, GameEngine::Engine::UI::FontStyle::Normal);
}

} // namespace

void PrefetchEditorFontOptions(UIManager* ui)
{
    if (!ui)
        return;
    ui->RequestFontFamily("Roboto");
    ui->RequestFontFamily("Inter");
    ui->RequestFontFamily("Roboto Mono");
    ui->RequestFontFamily("system-ui");
    ui->RequestFontFamily("Consolas");
    // Common IDE monospace faces (OS / user-installed); resolver falls back gracefully if absent.
    ui->RequestFontFamily("Cascadia Code");
    ui->RequestFontFamily("JetBrains Mono");
    ui->RequestFontFamily("Fira Code");
    ui->RequestFontFamily("Source Code Pro");
    ui->RequestFontFamily("Inconsolata");
    ui->RequestFontFamily("Ubuntu Mono");
    ui->RequestFontFamily("Menlo");
    ui->RequestFontFamily("Monaco");
    ui->RequestFontFamily("SF Mono");
}

std::vector<std::string> GetScriptFontFamilyStack(const std::string& scriptFontPreset)
{
    std::string css;
    if (!ScriptPresetToCss(scriptFontPreset, css))
        css = kDefaultScriptFontStackCss;

    std::vector<std::string> families;
    for (const std::string& token : SplitFontFamilyCss(css))
        families.push_back(StripQuotesFamily(token));
    return families;
}

void ApplySavedEditorFontPreferences(UIManager* ui)
{
    if (!ui)
        return;

    std::vector<const Stylesheet*> oldBlock;
    auto it = s_FontPrefsSheetByUi.find(ui);
    if (it != s_FontPrefsSheetByUi.end() && it->second)
        oldBlock.push_back(it->second.get());

    auto prefs = OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);

    std::string uiPreset;
    std::string scriptPreset;
    (void)prefs.TryGetString(kPrefUiFontPreset, uiPreset);
    (void)prefs.TryGetString(kPrefScriptFontPreset, scriptPreset);

    std::string uiCss;
    std::string scriptCss;
    const bool hasUi = UiPresetToCss(uiPreset, uiCss);
    const bool hasScript = ScriptPresetToCss(scriptPreset, scriptCss);

    if (!hasUi && !hasScript)
    {
        ui->ReplaceGlobalStylesheetBlock(oldBlock, {});
        if (it != s_FontPrefsSheetByUi.end())
            s_FontPrefsSheetByUi.erase(it);
        ui->MarkStyleDirtyAll();
        return;
    }

    std::string css = ":root { ";
    if (hasUi)
    {
        css += "--ui_font_family: ";
        css += uiCss;
        css += "; ";
    }
    if (hasScript)
    {
        css += "--ui_font_script: ";
        css += scriptCss;
        css += "; ";
    }
    css += "}";

    auto newSheet = std::make_shared<Stylesheet>();
    if (!UIParsing::CSSParser::ParseStylesFromString(css, *newSheet))
    {
        Logger::Log::Warning("Editor: failed to parse runtime font preference stylesheet");
        ui->ReplaceGlobalStylesheetBlock(oldBlock, {});
        if (it != s_FontPrefsSheetByUi.end())
            s_FontPrefsSheetByUi.erase(it);
        ui->MarkStyleDirtyAll();
        return;
    }

    StylesheetHandle newHandle = newSheet;
    ui->ReplaceGlobalStylesheetBlock(oldBlock, std::vector<StylesheetHandle>{newHandle});
    s_FontPrefsSheetByUi[ui] = std::move(newHandle);
    ui->MarkStyleDirtyAll();
}

std::vector<EditorFontDropdownOption> BuildEditorUiFontDropdownOptions(GameEngine::Engine::UI::FontResolver& resolver)
{
    static const struct
    {
        const char* value;
        const char* label;
    } kRows[] = {
        {"default", "Theme default (Roboto)"},
        {"roboto", "Roboto"},
        {"inter", "Inter"},
        {"system_ui", "System UI"},
        {"native_ui", "Native (Segoe UI / San Francisco)"},
    };
    std::vector<EditorFontDropdownOption> out;
    out.reserve(sizeof(kRows) / sizeof(kRows[0]));
    for (const auto& row : kRows)
    {
        if (ShouldIncludeUiFontPreset(resolver, row.value))
            out.push_back(EditorFontDropdownOption{std::string(row.value), std::string(row.label)});
    }
    return out;
}

std::vector<EditorFontDropdownOption> BuildScriptFontDropdownOptions(GameEngine::Engine::UI::FontResolver& resolver)
{
    static const struct
    {
        const char* value;
        const char* label;
    } kRows[] = {
        {"default", "Match theme (Roboto Mono)"},
        {"system_mono", "System monospace (UI stack)"},
        {"roboto_mono", "Roboto Mono"},
        {"cascadia_code", "Cascadia Code"},
        {"jetbrains_mono", "JetBrains Mono"},
        {"fira_code", "Fira Code"},
        {"source_code_pro", "Source Code Pro"},
        {"inconsolata", "Inconsolata"},
        {"ubuntu_mono", "Ubuntu Mono"},
        {"consolas", "Consolas"},
        {"menlo", "Menlo"},
        {"monaco", "Monaco"},
        {"sf_mono", "SF Mono"},
        {"lucida_console", "Lucida Console"},
        {"courier", "Courier New"},
    };
    std::vector<EditorFontDropdownOption> out;
    out.reserve(sizeof(kRows) / sizeof(kRows[0]));
    for (const auto& row : kRows)
    {
        if (ShouldIncludeScriptFontPreset(resolver, row.value))
            out.push_back(EditorFontDropdownOption{std::string(row.value), std::string(row.label)});
    }
    return out;
}

std::string SanitizeFontPresetAgainstOptions(const std::string& stored, const std::vector<EditorFontDropdownOption>& options)
{
    if (options.empty())
        return "default";
    for (const auto& o : options)
    {
        if (o.value == stored)
            return stored;
    }
    return options.front().value;
}

} // namespace GameEngine::Editor
