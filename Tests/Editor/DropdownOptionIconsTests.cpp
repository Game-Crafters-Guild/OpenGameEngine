// A dropdown option's icon is a CSS class the declaring field names beside the
// option's value and label; theme/dropdown-option-icons.css says which image
// that class is. Three things have to agree, and nothing at runtime complains
// loudly when they stop: a declared class with no rule silently shows nothing,
// and a rule naming a deleted file only warns once at stylesheet load.
//
// Source-level, for the reason SkyInspectorSectionsTests is: a component
// inspector runs over a live World and RenderServices and cannot be built in
// this process. What is locked is the agreement between the declarations, the
// stylesheet and the artwork — not the wording of any option.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace
{

std::filesystem::path EditorPath(const std::string& relativePath)
{
    return std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath;
}

std::string ReadEditorFile(const std::string& relativePath)
{
    std::ifstream in(EditorPath(relativePath), std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const char* kOptionIconStylesheet = "Assets/UI/theme/dropdown-option-icons.css";

// Every "dropdown-icon--..." spelled in the given text, whatever it spells it for.
std::set<std::string> IconClassesIn(const std::string& text)
{
    static const std::regex pattern(R"(dropdown-icon--[a-z0-9-]+)");
    std::set<std::string> found;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern);
         it != std::sregex_iterator(); ++it)
    {
        found.insert(it->str());
    }
    return found;
}

// Every declaration in the editor's sources, found rather than listed: a hand
// list would leave the next field that declares an icon unchecked, and would go
// red for a correct change the moment someone added a rule for it.
std::set<std::string> DeclaredIconClasses()
{
    std::set<std::string> declared;
    int scanned = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(EditorPath("Source")))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp")
            continue;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in)
            continue;
        std::ostringstream text;
        text << in.rdbuf();
        declared.merge(IconClassesIn(text.str()));
        ++scanned;
    }
    EXPECT_GT(scanned, 0) << "no editor sources were scanned";
    return declared;
}

} // namespace

TEST(DropdownOptionIconsTests, EveryDeclaredClassHasAStylesheetRule)
{
    const std::string sheet = ReadEditorFile(kOptionIconStylesheet);
    ASSERT_FALSE(sheet.empty()) << kOptionIconStylesheet << " did not read";

    const std::set<std::string> declared = DeclaredIconClasses();
    EXPECT_FALSE(declared.empty()) << "no field declares an option icon class any more";
    for (const std::string& className : declared)
    {
        EXPECT_NE(sheet.find("." + className + " {"), std::string::npos)
            << className << " is declared by a field but has no rule in " << kOptionIconStylesheet;
    }
}

TEST(DropdownOptionIconsTests, EveryStylesheetRuleIsDeclaredByAField)
{
    // The artwork is trimmed to what ships: a class nothing declares is dead
    // content, which is how the unreferenced half of an icon set accumulates.
    const std::string sheet = ReadEditorFile(kOptionIconStylesheet);
    ASSERT_FALSE(sheet.empty()) << kOptionIconStylesheet << " did not read";

    const std::set<std::string> declared = DeclaredIconClasses();
    for (const std::string& className : IconClassesIn(sheet))
    {
        EXPECT_TRUE(declared.count(className) == 1)
            << className << " has a rule but no field declares it";
    }
}

TEST(DropdownOptionIconsTests, EveryRuleNamesTheFileItsClassIsNamedAfter)
{
    // One stem per option: `.dropdown-icon--<stem>` means `<stem>.svg`. Uniform
    // by rule rather than by memory, so a reader never has to open the sheet to
    // learn which file a class is.
    const std::string sheet = ReadEditorFile(kOptionIconStylesheet);
    ASSERT_FALSE(sheet.empty()) << kOptionIconStylesheet << " did not read";

    static const std::regex rule(
        R"RX(\.dropdown-icon--([a-z0-9-]+)\s*\{[^}]*url\("editor:Icons/Dropdown/([^"]+)\.svg"\))RX");
    int checked = 0;
    for (auto it = std::sregex_iterator(sheet.begin(), sheet.end(), rule);
         it != std::sregex_iterator(); ++it)
    {
        EXPECT_EQ((*it)[1].str(), (*it)[2].str())
            << "class stem and file stem differ in " << kOptionIconStylesheet;
        ++checked;
    }
    EXPECT_EQ(checked, static_cast<int>(IconClassesIn(sheet).size()))
        << "a rule does not match the one-stem-per-option shape";
}

TEST(DropdownOptionIconsTests, EveryStylesheetRuleNamesAnExistingImage)
{
    const std::string sheet = ReadEditorFile(kOptionIconStylesheet);
    ASSERT_FALSE(sheet.empty()) << kOptionIconStylesheet << " did not read";

    // url("editor:<path>") resolves against the editor asset source, which is
    // Apps/Editor/Assets in the tree and the staged Assets folder at runtime.
    static const std::regex pattern(R"RX(url\("editor:([^"]+)"\))RX");
    int checked = 0;
    for (auto it = std::sregex_iterator(sheet.begin(), sheet.end(), pattern);
         it != std::sregex_iterator(); ++it)
    {
        const std::string relative = (*it)[1].str();
        EXPECT_TRUE(std::filesystem::exists(EditorPath("Assets/" + relative)))
            << relative << " is named by " << kOptionIconStylesheet << " but does not exist";
        ++checked;
    }
    EXPECT_GT(checked, 0) << "the stylesheet names no artwork at all";
}
