#pragma once

#include "Editor/Settings/SettingsStore.h"
#include <cstdint>

namespace GameEngine {

/**
 * Settings for syntax highlighting colors.
 * These are user-tweakable (via Settings panel) and used by the script editor.
 * Settings are persisted in Editor preferences (Preferences.json).
 *
 * Colors are ARGB (0xAARRGGBB).
 */
struct SyntaxHighlightSettings {
    uint32_t KeywordColor = 0xFFBFBFBF;    // Keywords - light gray
    uint32_t StringColor = 0xFF7ED321;     // Strings - vibrant green
    uint32_t CommentColor = 0xFFE5A54B;    // Comments - orange/amber
    uint32_t NumberColor = 0xFFB87FE8;     // Numbers - vibrant purple
    uint32_t TypeColor = 0xFF5DADE2;       // Types - vibrant blue
    uint32_t DefaultColor = 0xFF999999;    // Default text - 60% white (reduced to make syntax colors pop)

    // Load settings from preferences
    void Load()
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        
        int64_t color;
        if (prefs.TryGetInt64("syntax.keywordColor", color))
            KeywordColor = static_cast<uint32_t>(color);
        if (prefs.TryGetInt64("syntax.stringColor", color))
            StringColor = static_cast<uint32_t>(color);
        if (prefs.TryGetInt64("syntax.commentColor", color))
            CommentColor = static_cast<uint32_t>(color);
        if (prefs.TryGetInt64("syntax.numberColor", color))
            NumberColor = static_cast<uint32_t>(color);
        if (prefs.TryGetInt64("syntax.typeColor", color))
            TypeColor = static_cast<uint32_t>(color);
        if (prefs.TryGetInt64("syntax.defaultColor", color))
            DefaultColor = static_cast<uint32_t>(color);
    }
    
    // Save settings to preferences
    void Save()
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err); // Load existing to preserve other settings
        
        prefs.SetInt64("syntax.keywordColor", static_cast<int64_t>(KeywordColor));
        prefs.SetInt64("syntax.stringColor", static_cast<int64_t>(StringColor));
        prefs.SetInt64("syntax.commentColor", static_cast<int64_t>(CommentColor));
        prefs.SetInt64("syntax.numberColor", static_cast<int64_t>(NumberColor));
        prefs.SetInt64("syntax.typeColor", static_cast<int64_t>(TypeColor));
        prefs.SetInt64("syntax.defaultColor", static_cast<int64_t>(DefaultColor));
        
        prefs.Save(&err);
    }

    static SyntaxHighlightSettings& Get()
    {
        static SyntaxHighlightSettings instance;
        return instance;
    }

private:
    SyntaxHighlightSettings() 
    {
        Load(); // Load on first access
    }
};

} // namespace GameEngine

