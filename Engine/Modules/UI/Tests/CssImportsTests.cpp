#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "../Source/Assets/CssImports.h"
#include "AssetCore/GUID.h"

using namespace GameEngine::UI;

namespace
{

// Mirrors kMaxImportDepth in CssImports.cpp: the deepest import level that is still inlined.
constexpr int kMaxImportDepth = 32;

// Stylesheets written to a fresh directory; the expansion tests read real files and pass no
// AssetManager, so every import resolves relative to its importer.
class CssImportExpansionTests : public ::testing::Test
{
  protected:
    void SetUp() override { std::filesystem::create_directories(m_Directory); }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Directory, ec);
    }

    void Write(const std::filesystem::path& relativePath, std::string_view contents) const
    {
        const std::filesystem::path path = m_Directory / relativePath;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << contents;
    }

    std::filesystem::path Path(const std::filesystem::path& relativePath) const { return m_Directory / relativePath; }

    // Writes level1.css .. level<count>.css, each importing the next, and returns a root text that
    // imports level1.css.
    std::string WriteImportChain(int count) const
    {
        for (int level = 1; level <= count; ++level)
        {
            std::string text;
            if (level < count)
                text = "@import \"level" + std::to_string(level + 1) + ".css\";\n";
            text += ".level" + std::to_string(level) + " { width: 1px; }";
            Write("level" + std::to_string(level) + ".css", text);
        }
        return "@import \"level1.css\";\n.root { width: 1px; }";
    }

  private:
    std::filesystem::path m_Directory =
        std::filesystem::temp_directory_path() / ("css-imports-test-" + GameEngine::GUID::Generate().ToString());
};

std::vector<std::string> FileNames(const std::vector<std::filesystem::path>& paths)
{
    std::vector<std::string> names;
    names.reserve(paths.size());
    for (const std::filesystem::path& path : paths)
        names.push_back(path.filename().string());
    return names;
}

} // namespace

TEST(CssImportsTests, FindsQuotedAndUrlTargetsInOrder)
{
    constexpr std::string_view css = "@import \"a.css\";\n@IMPORT url('b.css');\n.x { color: red; }\n";
    const auto statements = FindTopLevelCssImports(css);
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0].Target, "a.css");
    EXPECT_EQ(css.substr(statements[0].Begin, statements[0].End - statements[0].Begin), "@import \"a.css\";");
    EXPECT_EQ(statements[1].Target, "b.css");
    EXPECT_EQ(css[statements[1].End - 1], ';');
}

TEST(CssImportsTests, IgnoresImportsInCommentsStringsAndBlocks)
{
    constexpr std::string_view css =
        "/* @import \"comment.css\"; */\n"
        ".a { content: \"@import 'string.css';\"; }\n"
        ".b { @import \"block.css\"; }\n"
        "@import url(top.css);\n";
    const auto statements = FindTopLevelCssImports(css);
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0].Target, "top.css");
}

TEST(CssImportsTests, SkipsStatementsWithoutTargetOrTerminator)
{
    EXPECT_TRUE(FindTopLevelCssImports("@import ;\n").empty());
    EXPECT_TRUE(FindTopLevelCssImports("@import \"unterminated.css\"").empty());
}

TEST_F(CssImportExpansionTests, InlinesNestedImportsRelativeToEachImporter)
{
    Write("sub/grand.css", ".grand { width: 1px; }");
    Write("child.css", "@import \"sub/grand.css\";\n.child { width: 2px; }");
    constexpr std::string_view rootText = "@import url(child.css);\n.root { width: 3px; }";

    std::string expanded;
    std::vector<std::filesystem::path> files;
    ExpandCssImports(Path("root.css"), rootText, nullptr, expanded, files);

    EXPECT_EQ(expanded.find("@import"), std::string::npos);
    const size_t grand = expanded.find(".grand");
    const size_t child = expanded.find(".child");
    const size_t root = expanded.find(".root");
    ASSERT_NE(grand, std::string::npos);
    ASSERT_NE(child, std::string::npos);
    ASSERT_NE(root, std::string::npos);
    EXPECT_LT(grand, child);
    EXPECT_LT(child, root);
    EXPECT_EQ(FileNames(files), (std::vector<std::string>{"child.css", "grand.css"}));

    std::vector<std::filesystem::path> collected;
    CollectCssImportFiles(Path("root.css"), rootText, nullptr, collected);
    EXPECT_EQ(collected, files);
}

TEST_F(CssImportExpansionTests, DropsCyclicImportsAndKeepsTheRest)
{
    Write("b.css", "@import \"a.css\";\n.b { width: 1px; }");
    constexpr std::string_view aText = "@import \"b.css\";\n@import \"a.css\";\n.a { width: 2px; }";
    Write("a.css", aText);

    std::string expanded;
    std::vector<std::filesystem::path> files;
    ExpandCssImports(Path("a.css"), aText, nullptr, expanded, files);

    EXPECT_EQ(expanded.find("@import"), std::string::npos);
    const size_t b = expanded.find(".b {");
    const size_t a = expanded.find(".a {");
    ASSERT_NE(b, std::string::npos);
    ASSERT_NE(a, std::string::npos);
    EXPECT_LT(b, a);
    EXPECT_EQ(expanded.find(".a {", a + 1), std::string::npos);
    EXPECT_EQ(FileNames(files), (std::vector<std::string>{"b.css"}));

    std::vector<std::filesystem::path> collected;
    CollectCssImportFiles(Path("a.css"), aText, nullptr, collected);
    EXPECT_EQ(collected, files);
}

TEST_F(CssImportExpansionTests, StopsAtTheDepthLimit)
{
    const int chainLength = kMaxImportDepth + 2;
    const std::string rootText = WriteImportChain(chainLength);

    std::string expanded;
    std::vector<std::filesystem::path> files;
    ExpandCssImports(Path("root.css"), rootText, nullptr, expanded, files);

    ASSERT_EQ(files.size(), static_cast<size_t>(kMaxImportDepth));
    EXPECT_EQ(files.front().filename().string(), "level1.css");
    EXPECT_EQ(files.back().filename().string(), "level" + std::to_string(kMaxImportDepth) + ".css");
    const std::string deepestKept = ".level" + std::to_string(kMaxImportDepth) + " ";
    const std::string firstDropped = ".level" + std::to_string(kMaxImportDepth + 1) + " ";
    EXPECT_NE(expanded.find(deepestKept), std::string::npos);
    EXPECT_EQ(expanded.find(firstDropped), std::string::npos);

    std::vector<std::filesystem::path> collected;
    CollectCssImportFiles(Path("root.css"), rootText, nullptr, collected);
    EXPECT_EQ(collected, files);
}
