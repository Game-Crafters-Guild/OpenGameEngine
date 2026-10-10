// "Register, don't accrete": a component's external-resource teardown is
// registered by the module that owns the component, never enumerated in a hub.
//
// This scans the tree rather than asserting on runtime state because the failure
// it guards against is a source-level one — the next resource-owning component
// gets its RegisterOnRemove pasted into Engine.cpp because that is where the
// others were, and Core silently re-acquires knowledge of terrain, physics and
// rendering internals. A registrar file per module is the shape; this test is
// what keeps it.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace fs = std::filesystem;

fs::path RepoRoot()
{
    return fs::path(GE_REPO_SOURCE_DIR);
}

std::string ReadFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Blank out `//` and block comments so the scan reads code, not prose. A comment
// naming RegisterOnRemove<T> is documentation, and an instrument that cannot tell
// the two apart reports architecture violations that do not exist. String
// literals are left alone: nothing in this tree spells the token inside one, and
// a literal that did would be worth failing on.
std::string CodeOnly(std::string text)
{
    for (size_t i = 0; i + 1 < text.size();)
    {
        if (text[i] == '/' && text[i + 1] == '/')
        {
            while (i < text.size() && text[i] != '\n')
                text[i++] = ' ';
        }
        else if (text[i] == '/' && text[i + 1] == '*')
        {
            const size_t end = text.find("*/", i + 2);
            const size_t stop = (end == std::string::npos) ? text.size() : end + 2;
            for (; i < stop; ++i)
            {
                if (text[i] != '\n')
                    text[i] = ' ';
            }
        }
        else
        {
            ++i;
        }
    }
    return text;
}

// Production directories only: test sources register hooks freely on their own
// worlds and are not part of the architecture this pins.
const std::vector<fs::path>& ProductionRoots()
{
    static const std::vector<fs::path> roots{
        RepoRoot() / "Engine" / "Source",
        RepoRoot() / "Engine" / "Modules",
        RepoRoot() / "Engine" / "ECSModules",
        RepoRoot() / "Apps",
    };
    return roots;
}

bool IsTestPath(const fs::path& p)
{
    for (const auto& part : p)
    {
        const std::string s = part.string();
        if (s == "Tests" || s == "Test")
            return true;
    }
    return false;
}

// Every file that calls RegisterOnRemove, relative to the repo root.
std::vector<std::string> FilesRegisteringRemoveHooks()
{
    std::vector<std::string> hits;
    for (const auto& root : ProductionRoots())
    {
        if (!fs::exists(root))
            continue;
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;
            const auto& path = entry.path();
            const std::string ext = path.extension().string();
            if (ext != ".cpp" && ext != ".h" && ext != ".inl")
                continue;
            if (IsTestPath(path))
                continue;
            if (CodeOnly(ReadFile(path)).find("RegisterOnRemove<") == std::string::npos)
                continue;
            hits.push_back(path.lexically_relative(RepoRoot()).generic_string());
        }
    }
    std::sort(hits.begin(), hits.end());
    return hits;
}

} // namespace

// The ECS core declares the API; nothing else may register a hook except a
// module's own registrar, whose filename says so.
TEST(WorldHookOwnership, EveryRemoveHookIsRegisteredByAModuleRegistrar)
{
    for (const std::string& file : FilesRegisteringRemoveHooks())
    {
        static constexpr std::string_view kRegistrarSuffix = "WorldHooks.cpp";
        const bool isApiDeclaration = file.find("Engine/Modules/ECS/") == 0;
        const bool isRegistrar = file.size() >= kRegistrarSuffix.size() &&
                                 file.compare(file.size() - kRegistrarSuffix.size(),
                                              kRegistrarSuffix.size(), kRegistrarSuffix) == 0;
        EXPECT_TRUE(isApiDeclaration || isRegistrar)
            << file << " registers an OnRemove hook outside its module's registrar. "
            << "Add a <Module>WorldHooks.cpp that owns it and call the registrar instead.";
    }
}

// The specific accretion this replaces: EnsurePrimaryWorld used to hand-register
// SkeletonRef and MeshGPUData, so Core owned two other modules' teardown.
TEST(WorldHookOwnership, EngineCoreRegistersNoHooksItself)
{
    const fs::path enginecpp = RepoRoot() / "Engine" / "Source" / "Core" / "Engine.cpp";
    ASSERT_TRUE(fs::exists(enginecpp)) << enginecpp.string();
    EXPECT_EQ(CodeOnly(ReadFile(enginecpp)).find("RegisterOnRemove"), std::string::npos)
        << "Engine.cpp must call module registrars, not register hooks itself.";
}
