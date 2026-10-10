#include <gtest/gtest.h>

#include "Panels/ScriptSyntax.h"

#include <string>
#include <vector>

using GameEngine::Editor::ScriptLanguage;
using GameEngine::Editor::ScriptLanguageForPath;
using GameEngine::Editor::SyntaxToken;
using GameEngine::Editor::SyntaxTokenType;
using GameEngine::Editor::TokenizeScript;

namespace {

std::vector<SyntaxToken> Tokenize(ScriptLanguage language, const std::string& text)
{
    std::vector<SyntaxToken> tokens;
    TokenizeScript(language, text, tokens);
    return tokens;
}

std::string TextOf(const std::string& text, const SyntaxToken& token)
{
    return text.substr(token.Start, token.Length);
}

// The type of the token whose text is exactly `word`, or Default when absent.
SyntaxTokenType TypeOf(const std::string& text, const std::vector<SyntaxToken>& tokens, const std::string& word)
{
    for (const SyntaxToken& token : tokens)
        if (TextOf(text, token) == word)
            return token.Type;
    return SyntaxTokenType::Default;
}

} // namespace

TEST(ScriptSyntaxTests, ShaderExtensionsAreGlslAndTheRestIsCSharp)
{
    for (const char* shader : {"a.glsl", "a.vert", "a.frag", "a.comp", "a.geom", "a.GLSL"})
        EXPECT_EQ(ScriptLanguageForPath(shader), ScriptLanguage::Glsl) << shader;
    EXPECT_EQ(ScriptLanguageForPath("a.cs"), ScriptLanguage::CSharp);
    EXPECT_EQ(ScriptLanguageForPath("a.cpp"), ScriptLanguage::CSharp);
    EXPECT_EQ(ScriptLanguageForPath("noext"), ScriptLanguage::CSharp);
}

TEST(ScriptSyntaxTests, GlslKeywordsTypesAndBuiltinsAreClassified)
{
    const std::string text = "layout(location = 0) in vec3 aPos;\nuniform sampler2D albedo;\n"
                             "void main() { gl_Position = vec4(normalize(aPos), 1.0); }";
    const auto tokens = Tokenize(ScriptLanguage::Glsl, text);
    EXPECT_EQ(TypeOf(text, tokens, "layout"), SyntaxTokenType::Keyword);
    EXPECT_EQ(TypeOf(text, tokens, "in"), SyntaxTokenType::Keyword);
    EXPECT_EQ(TypeOf(text, tokens, "uniform"), SyntaxTokenType::Keyword);
    EXPECT_EQ(TypeOf(text, tokens, "vec3"), SyntaxTokenType::Type);
    EXPECT_EQ(TypeOf(text, tokens, "sampler2D"), SyntaxTokenType::Type);
    EXPECT_EQ(TypeOf(text, tokens, "void"), SyntaxTokenType::Type);
    EXPECT_EQ(TypeOf(text, tokens, "gl_Position"), SyntaxTokenType::Type);
    EXPECT_EQ(TypeOf(text, tokens, "normalize"), SyntaxTokenType::Type);
    EXPECT_EQ(TypeOf(text, tokens, "aPos"), SyntaxTokenType::Identifier);
    EXPECT_EQ(TypeOf(text, tokens, "main"), SyntaxTokenType::Identifier);
    EXPECT_EQ(TypeOf(text, tokens, "1.0"), SyntaxTokenType::Number);
    EXPECT_EQ(TypeOf(text, tokens, "0"), SyntaxTokenType::Number);
}

TEST(ScriptSyntaxTests, CSharpWordsAreNotGlslWordsAndViceVersa)
{
    const std::string text = "vec3 foreach sampler2D namespace";
    const auto glsl = Tokenize(ScriptLanguage::Glsl, text);
    EXPECT_EQ(TypeOf(text, glsl, "foreach"), SyntaxTokenType::Identifier);
    EXPECT_EQ(TypeOf(text, glsl, "namespace"), SyntaxTokenType::Identifier);
    const auto cs = Tokenize(ScriptLanguage::CSharp, text);
    EXPECT_EQ(TypeOf(text, cs, "foreach"), SyntaxTokenType::Keyword);
    EXPECT_EQ(TypeOf(text, cs, "namespace"), SyntaxTokenType::Keyword);
    EXPECT_EQ(TypeOf(text, cs, "vec3"), SyntaxTokenType::Identifier);
}

TEST(ScriptSyntaxTests, APreprocessorLineIsOneToken)
{
    const std::string text = "#version 450\n#ifdef GE_USER_PULSE\nfloat x;\n  #endif";
    const auto tokens = Tokenize(ScriptLanguage::Glsl, text);
    EXPECT_EQ(TypeOf(text, tokens, "#version 450"), SyntaxTokenType::Preprocessor);
    EXPECT_EQ(TypeOf(text, tokens, "#ifdef GE_USER_PULSE"), SyntaxTokenType::Preprocessor);
    EXPECT_EQ(TypeOf(text, tokens, "#endif"), SyntaxTokenType::Preprocessor);
    EXPECT_EQ(TypeOf(text, tokens, "float"), SyntaxTokenType::Type);
    // A `#` that is not at the start of its line is an ordinary operator.
    const std::string inline_ = "a # b";
    EXPECT_EQ(TypeOf(inline_, Tokenize(ScriptLanguage::Glsl, inline_), "#"), SyntaxTokenType::Operator);
}

TEST(ScriptSyntaxTests, SurfaceShaderTagsInsideCommentsAreAnnotations)
{
    const std::string text = "// @texture albedoMap srgb\n// plain comment @ not a tag\nfloat f;";
    const auto tokens = Tokenize(ScriptLanguage::Glsl, text);
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].Type, SyntaxTokenType::Comment);
    EXPECT_EQ(TextOf(text, tokens[0]), "// ");
    EXPECT_EQ(tokens[1].Type, SyntaxTokenType::Annotation);
    EXPECT_EQ(TextOf(text, tokens[1]), "@texture");
    EXPECT_EQ(tokens[2].Type, SyntaxTokenType::Comment);
    EXPECT_EQ(TextOf(text, tokens[2]), " albedoMap srgb");
    EXPECT_EQ(tokens[3].Type, SyntaxTokenType::Comment);
    EXPECT_EQ(TextOf(text, tokens[3]), "// plain comment @ not a tag");
    // C# comments are prose: no annotation split.
    const std::string cs = "// @texture x";
    const auto csTokens = Tokenize(ScriptLanguage::CSharp, cs);
    ASSERT_EQ(csTokens.size(), 1u);
    EXPECT_EQ(csTokens[0].Type, SyntaxTokenType::Comment);
}

TEST(ScriptSyntaxTests, StringsHonourEscapesAndStopAtLineEnd)
{
    const std::string text = "s = \"a \\\" b\"; t = \"open\nnext";
    const auto tokens = Tokenize(ScriptLanguage::CSharp, text);
    EXPECT_EQ(TypeOf(text, tokens, "\"a \\\" b\""), SyntaxTokenType::String);
    EXPECT_EQ(TypeOf(text, tokens, "\"open"), SyntaxTokenType::String);
    EXPECT_EQ(TypeOf(text, tokens, "next"), SyntaxTokenType::Identifier);
}

TEST(ScriptSyntaxTests, BlockCommentsSpanLinesAndNumbersKeepTheirSuffixes)
{
    const std::string text = "/* a\n b */ x = 0.25f + 0x1F + 1e-3 + 2.5E+4 - 0x1E-3;";
    const auto tokens = Tokenize(ScriptLanguage::Glsl, text);
    EXPECT_EQ(TypeOf(text, tokens, "/* a\n b */"), SyntaxTokenType::Comment);
    EXPECT_EQ(TypeOf(text, tokens, "0.25f"), SyntaxTokenType::Number);
    EXPECT_EQ(TypeOf(text, tokens, "0x1F"), SyntaxTokenType::Number);
    // A signed exponent is part of the literal; in a hex literal `E` is a digit,
    // so `0x1E-3` is a subtraction.
    EXPECT_EQ(TypeOf(text, tokens, "1e-3"), SyntaxTokenType::Number);
    EXPECT_EQ(TypeOf(text, tokens, "2.5E+4"), SyntaxTokenType::Number);
    EXPECT_EQ(TypeOf(text, tokens, "0x1E-3"), SyntaxTokenType::Default);
    EXPECT_EQ(TypeOf(text, tokens, "0x1E"), SyntaxTokenType::Number);
}

TEST(ScriptSyntaxTests, TokensAreAscendingAndNonOverlapping)
{
    const std::string text = "#version 450\n// @texture t linear\nvec4 c = texture(t, uv) * \"x\";";
    const auto tokens = Tokenize(ScriptLanguage::Glsl, text);
    ASSERT_FALSE(tokens.empty());
    for (size_t i = 1; i < tokens.size(); ++i)
        EXPECT_GE(tokens[i].Start, tokens[i - 1].Start + tokens[i - 1].Length) << "token " << i;
    for (const SyntaxToken& token : tokens)
        EXPECT_GT(token.Length, 0u);
}
