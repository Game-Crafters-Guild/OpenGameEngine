#include "Assets/Packages/PackageCodeDiscovery.h"

#include "AssetCore/AssetIgnoreRules.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/EditorOnlyAssetPath.h"
#include "Logger/Logger.h"

#include <array>
#include <string>

namespace GameEngine
{

namespace
{

using ModuleKind = PackageModuleRecord::ModuleKind;
using ModuleLang = PackageModuleRecord::ModuleLang;

constexpr std::array<ModuleLang, 2> kLanguages = {ModuleLang::CSharp, ModuleLang::Cpp};
constexpr std::array<ModuleKind, 2> kKinds = {ModuleKind::Runtime, ModuleKind::Editor};

constexpr size_t Index(ModuleLang lang)
{
    return lang == ModuleLang::Cpp ? 1u : 0u;
}

constexpr size_t Index(ModuleKind kind)
{
    return kind == ModuleKind::Editor ? 1u : 0u;
}

const char* LanguageName(ModuleLang lang)
{
    return lang == ModuleLang::Cpp ? "C++" : "C#";
}

// The sources of one (language, kind), reduced as the walk visits them.
struct SourceGroup
{
    std::filesystem::path Root;      // deepest common directory so far
    bool HasSource = false;
    bool HasTranslationUnit = false; // C++ only; C# needs no equivalent
};

// A C++ module needs something to compile. Headers widen the root — the
// module's include path is its root — but build nothing on their own.
bool GroupIsModule(ModuleLang lang, const SourceGroup& group)
{
    return group.HasSource && (lang != ModuleLang::Cpp || group.HasTranslationUnit);
}

// Language and compiled-ness of a source file. The translation-unit set is the
// one the generated project compiles (UserProjectGenerator globs *.cpp, *.cc,
// *.cxx and *.c); the rest of AssetType::NativeSource is headers.
bool TryClassifySource(const std::filesystem::path& file, ModuleLang& outLang, bool& outIsTranslationUnit)
{
    const String extension = file.extension().string();
    const AssetType type = GetAssetTypeFromExtension(extension);
    if (type == AssetType::NativeSource)
    {
        outLang = ModuleLang::Cpp;
        outIsTranslationUnit = extension == ".cpp" || extension == ".cc" || extension == ".cxx" ||
                               extension == ".c";
        return true;
    }
    // AssetType::Script covers more than one language; C# is the only one that
    // compiles into an assembly.
    if (type == AssetType::Script && extension == ".cs")
    {
        outLang = ModuleLang::CSharp;
        outIsTranslationUnit = true;
        return true;
    }
    return false;
}

std::filesystem::path DeepestCommonDirectory(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::filesystem::path common;
    auto itA = a.begin();
    auto itB = b.begin();
    for (; itA != a.end() && itB != b.end() && *itA == *itB; ++itA, ++itB)
        common /= *itA;
    return common;
}

bool ContainsOrEquals(const std::filesystem::path& outer, const std::filesystem::path& inner)
{
    auto itOuter = outer.begin();
    auto itInner = inner.begin();
    for (; itOuter != outer.end(); ++itOuter, ++itInner)
    {
        if (itInner == inner.end() || *itOuter != *itInner)
            return false;
    }
    return true;
}

} // namespace

std::vector<DiscoveredPackageModule> DiscoverPackageCodeModules(const std::filesystem::path& packageRoot,
                                                                std::string_view packageName)
{
    std::vector<DiscoveredPackageModule> modules;

    std::error_code rootEc;
    if (packageRoot.empty() || !std::filesystem::is_directory(packageRoot, rootEc))
        return modules;

    AssetIgnoreRules ignoreRules = AssetIgnoreRules::LoadForAssetRoot(packageRoot);
    ignoreRules.IgnorePackageTestsFolder(packageRoot, packageRoot);
    std::array<std::array<SourceGroup, kKinds.size()>, kLanguages.size()> groups;

    std::error_code walkEc;
    const std::filesystem::recursive_directory_iterator end;
    for (auto it = std::filesystem::recursive_directory_iterator(
             packageRoot, std::filesystem::directory_options::skip_permission_denied, walkEc);
         !walkEc && it != end; it.increment(walkEc))
    {
        const std::filesystem::path& path = it->path();

        std::error_code entryEc;
        if (it->is_directory(entryEc))
        {
            if (ignoreRules.ShouldIgnoreDirectory(path, packageRoot))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(entryEc) || ignoreRules.ShouldIgnoreFile(path, packageRoot))
            continue;

        ModuleLang lang = ModuleLang::CSharp;
        bool isTranslationUnit = false;
        if (!TryClassifySource(path, lang, isTranslationUnit))
            continue;

        const std::filesystem::path relative = std::filesystem::relative(path, packageRoot, entryEc);
        const ModuleKind kind =
            IsEditorOnlyAssetPath(relative) ? ModuleKind::Editor : ModuleKind::Runtime;

        SourceGroup& group = groups[Index(lang)][Index(kind)];
        const std::filesystem::path directory = path.parent_path();
        group.Root = group.HasSource ? DeepestCommonDirectory(group.Root, directory) : directory;
        group.HasSource = true;
        group.HasTranslationUnit = group.HasTranslationUnit || isTranslationUnit;
    }

    if (walkEc)
    {
        Logger::Log::Warning("[Packages] package '{}': walking '{}' stopped early ({}); its code "
                             "modules may be incomplete",
                             packageName, packageRoot.generic_string(), walkEc.message());
    }

    for (ModuleLang lang : kLanguages)
    {
        const SourceGroup& runtime = groups[Index(lang)][Index(ModuleKind::Runtime)];
        const SourceGroup& editor = groups[Index(lang)][Index(ModuleKind::Editor)];
        if (GroupIsModule(lang, runtime) && GroupIsModule(lang, editor) &&
            (ContainsOrEquals(runtime.Root, editor.Root) || ContainsOrEquals(editor.Root, runtime.Root)))
        {
            Logger::Log::Error(
                "[Packages] package '{}': its {} runtime sources at '{}' and editor sources at '{}' "
                "nest. A module compiles every source under its root, so the two roots must be "
                "separate directories — put the runtime sources in a sibling of the Editor folder. "
                "Both {} modules skipped.",
                packageName, LanguageName(lang), runtime.Root.generic_string(),
                editor.Root.generic_string(), LanguageName(lang));
            continue;
        }

        for (ModuleKind kind : kKinds)
        {
            const SourceGroup& group = groups[Index(lang)][Index(kind)];
            if (!GroupIsModule(lang, group))
                continue;
            DiscoveredPackageModule module;
            module.Kind = kind;
            module.Lang = lang;
            module.RootDir = group.Root.lexically_normal();
            modules.push_back(std::move(module));
        }
    }
    return modules;
}

} // namespace GameEngine
