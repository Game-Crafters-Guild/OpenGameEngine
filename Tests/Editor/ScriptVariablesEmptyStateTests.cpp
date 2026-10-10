#include <gtest/gtest.h>

#include "Panels/ScriptVariablesEmptyState.h"

using GameEngine::Editor::EmptyScriptVariablesText;

namespace
{

TEST(ScriptVariablesEmptyStateTests, ACSharpScriptGetsTheCSharpFieldAdvice)
{
    const std::string text = EmptyScriptVariablesText("Assets/Player.cs");
    EXPECT_NE(text.find("public int myField"), std::string::npos);
    EXPECT_NE(text.find("[SerializeField]"), std::string::npos);
}

// The regression this exists for: the Script Editor fires the same "no variables"
// notification for a shader, and the inspector used to answer it with C# syntax.
TEST(ScriptVariablesEmptyStateTests, AShaderNeverGetsCSharpAdvice)
{
    for (const char* path : {"Assets/Rock.glsl", "Assets/Rock.GLSL", "Assets/Rock.hlsl"})
    {
        const std::string text = EmptyScriptVariablesText(path);
        EXPECT_EQ(text.find("public int myField"), std::string::npos) << path;
        EXPECT_EQ(text.find("[SerializeField]"), std::string::npos) << path;
        EXPECT_NE(text.find("Surface shader"), std::string::npos) << path;
    }
}

TEST(ScriptVariablesEmptyStateTests, AnyOtherFileGetsNeitherLanguagesAdvice)
{
    for (const char* path : {"Assets/Thing.cpp", "Assets/Thing.h", "Assets/Thing"})
    {
        const std::string text = EmptyScriptVariablesText(path);
        EXPECT_EQ(text.find("public int myField"), std::string::npos) << path;
        EXPECT_EQ(text.find("Surface shader"), std::string::npos) << path;
        EXPECT_FALSE(text.empty()) << path;
    }
}

} // namespace
