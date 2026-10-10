// Generation-shape tests for the persisted user-project CMakeLists (#370).
//
// The file is IDE-openable and outlives the generating Editor, so engine paths under
// the SDK root must go through the GAMEENGINE_SDK_DIR variable with the live root
// baked only as a last-known-good default; a dead baked root must be refreshed (with
// a diagnostic) on the next engine touch; out-of-SDK layouts keep absolute paths.

#include "NativeScripting/NativeBuildConfig.h"
#include "NativeScripting/UserProjectGenerator.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace ns = GameEngine::NativeScripting;
namespace fs = std::filesystem;

namespace
{
// An editor-shaped config: project under <root>/Project, staged SDK under sdkRoot.
ns::NativeBuildConfig MakeSdkConfig(const fs::path& projectRoot, const fs::path& sdkRoot)
{
    ns::NativeBuildConfig config;
    config.SourceDir = projectRoot / "Assets";
    config.BuildDir = projectRoot / ".Cache" / "NativeScripts" / "build";
    config.SdkRoot = sdkRoot;
    config.IncludeDirs = {sdkRoot / "include", sdkRoot / "vcpkg-include"};
    config.EngineImportLib = sdkRoot / "lib" / "Debug" / "Engine.lib";
    config.SdkEntrySource = sdkRoot / "nativescripting" / "UserModuleEntry.cpp";
    return config;
}

std::size_t CountOccurrences(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

TEST(UserProjectGenerator, SdkPathsGoThroughVariableWithLastKnownGoodDefault)
{
    const auto config = MakeSdkConfig("/geproj", "/editor/SDK");
    const std::string content = ns::UserProjectGenerator::BuildCMakeListsContent(config);

    // The variable block: engine-driven configures override it; the baked default is
    // the IDE last-known-good, guarded by a loud failure when the directory is gone.
    EXPECT_NE(content.find("if(NOT DEFINED GAMEENGINE_SDK_DIR)"), std::string::npos);
    EXPECT_NE(content.find("set(GAMEENGINE_SDK_DIR \"/editor/SDK\")"), std::string::npos);
    EXPECT_NE(content.find("message(FATAL_ERROR \"GameEngine SDK not found at '${GAMEENGINE_SDK_DIR}'"),
              std::string::npos);

    // Every SDK-rooted engine path is expressed through the variable...
    EXPECT_NE(content.find("\"${GAMEENGINE_SDK_DIR}/nativescripting/UserModuleEntry.cpp\""), std::string::npos);
    EXPECT_NE(content.find("\"${GAMEENGINE_SDK_DIR}/include\""), std::string::npos);
    EXPECT_NE(content.find("\"${GAMEENGINE_SDK_DIR}/vcpkg-include\""), std::string::npos);
    EXPECT_NE(content.find("\"${GAMEENGINE_SDK_DIR}/lib/Debug/Engine.lib\""), std::string::npos);
    // ...so the machine's SDK location appears exactly once: the last-known-good default.
    EXPECT_EQ(CountOccurrences(content, "/editor/SDK"), 1u);

    // Project-side paths are relocation-proof too: the user include dir is the source
    // dir itself, and scanner outputs are globbed project-relative (content therefore
    // does not depend on whether the scanner ran).
    EXPECT_NE(content.find("\"${CMAKE_CURRENT_SOURCE_DIR}\""), std::string::npos);
    EXPECT_NE(content.find("file(GLOB _generated_sources CONFIGURE_DEPENDS "
                           "\"${CMAKE_CURRENT_SOURCE_DIR}/../.Cache/NativeScripts/build/*.gen.cpp\")"),
              std::string::npos);
    EXPECT_EQ(content.find("/geproj"), std::string::npos) << "project paths must not be baked absolute";
}

TEST(UserProjectGenerator, NonDebugConfigsCarryDebugInfoFlags)
{
    // C6: engine DebugFast/Release map to a "Release" user build; without explicit
    // flags CMake's Release defaults emit no PDB and user crashes decode name-only.
    // The generated project must add RelWithDebInfo-style symbols to every
    // non-Debug config, MSVC and otherwise.
    const auto config = MakeSdkConfig("/geproj", "/editor/SDK");
    const std::string content = ns::UserProjectGenerator::BuildCMakeListsContent(config);

    EXPECT_NE(content.find("target_compile_options(UserScripts PRIVATE "
                           "$<$<NOT:$<CONFIG:Debug>>:/Zi> $<$<NOT:$<CONFIG:Debug>>:/Zo>)"),
              std::string::npos);
    EXPECT_NE(content.find("target_link_options(UserScripts PRIVATE $<$<NOT:$<CONFIG:Debug>>:/DEBUG>)"),
              std::string::npos);
    EXPECT_NE(content.find("target_compile_options(UserScripts PRIVATE $<$<NOT:$<CONFIG:Debug>>:-g>)"),
              std::string::npos);
}

TEST(UserProjectGenerator, DebugFastIsDeclaredForMultiConfigGenerators)
{
    // A user module builds in the HOST engine's config, and DebugFast is one CMake does
    // not know. Multi-config generators fix their configuration list at project() time
    // and reject `--build --config DebugFast` (MSB8013) before compiling anything, so the
    // generated project must declare it — and must do so AFTER project(), which is what
    // seeds CMAKE_CONFIGURATION_TYPES in the first place.
    const auto config = MakeSdkConfig("/geproj", "/editor/SDK");
    const std::string content = ns::UserProjectGenerator::BuildCMakeListsContent(config);

    const std::size_t projectCall = content.find("project(UserScripts LANGUAGES CXX C)");
    const std::size_t declare =
        content.find("if(CMAKE_CONFIGURATION_TYPES AND NOT \"DebugFast\" IN_LIST CMAKE_CONFIGURATION_TYPES)");
    ASSERT_NE(projectCall, std::string::npos);
    ASSERT_NE(declare, std::string::npos);
    EXPECT_GT(declare, projectCall) << "DebugFast declared before project() would be overwritten by it";
    EXPECT_NE(content.find("list(APPEND CMAKE_CONFIGURATION_TYPES DebugFast)"), std::string::npos);

    // Declaring the config without flags would build it with none of Debug's /Od + /Ob0, so
    // the seeding is part of the same contract; /RTC1 is what DebugFast drops.
    EXPECT_NE(content.find("string(REPLACE \"_DEBUGFAST\" \"_DEBUG\" _ge_seed_var \"${_ge_flag}\")"),
              std::string::npos);
    EXPECT_NE(content.find("string(REPLACE \"/RTC1\" \"\" _ge_seed \"${${_ge_seed_var}}\")"),
              std::string::npos);
    // FORCE is load-bearing, and so is its guard: a single-config generator pre-creates an
    // empty cache entry for the active build type, which a plain CACHE set would never
    // overwrite, while an unguarded FORCE would discard a user-supplied value.
    EXPECT_NE(content.find("    if(NOT ${_ge_flag})"), std::string::npos);
    EXPECT_NE(content.find("CACHE STRING \"DebugFast build flags\" FORCE)"), std::string::npos);
}

TEST(UserProjectGenerator, MsvcUserModulesCompileWithBigObj)
{
    // COFF caps an object file at 65,279 sections and emits one per COMDAT, so a TU that
    // includes the EditorSDK or the UI headers approaches it: the worst in the package
    // class measures 86% of the cap. The flag widens a field in the object format and
    // changes no codegen, so it is unconditional.
    //
    // PLACEMENT is what this pins, not merely presence: /bigobj must sit inside the MSVC
    // arm. A refactor that hoisted it out would leave a find() assertion green while every
    // non-MSVC user build broke on an unknown flag, and the else() arm cannot carry an
    // equivalent -- MinGW's -Wa,-mbig-obj is not valid for Linux or macOS clang.
    const auto config = MakeSdkConfig("/geproj", "/editor/SDK");
    const std::string content = ns::UserProjectGenerator::BuildCMakeListsContent(config);

    const std::size_t msvcArm = content.find("if(MSVC)");
    const std::size_t bigObj =
        content.find("target_compile_options(UserScripts PRIVATE /bigobj)");
    ASSERT_NE(msvcArm, std::string::npos);
    ASSERT_NE(bigObj, std::string::npos);
    EXPECT_GT(bigObj, msvcArm);
    EXPECT_LT(bigObj, content.find("else()", msvcArm));
}

// EditorSDK phase 1: an Editor-kind module config links the EditorSDK import
// lib and sees the staged editor headers; a runtime config emits neither.
TEST(UserProjectGenerator, EditorModuleConfigLinksEditorSdk)
{
    auto config = MakeSdkConfig("/geproj", "/editor/SDK");
    config.ModuleName = "Toolkit.Editor";
    config.EditorImportLib = fs::path("/editor/SDK") / "lib" / "Debug" / "EditorSDK.lib";
    config.EditorIncludeDirs = {fs::path("/editor/SDK") / "include-editor"};

    const std::string content = ns::UserProjectGenerator::BuildCMakeListsContent(config);
    EXPECT_NE(content.find("\"${GAMEENGINE_SDK_DIR}/lib/Debug/EditorSDK.lib\""), std::string::npos);
    EXPECT_NE(content.find("\"${GAMEENGINE_SDK_DIR}/include-editor\""), std::string::npos);

    // Runtime shape: no editor interface anywhere in the generated project.
    const std::string runtimeContent =
        ns::UserProjectGenerator::BuildCMakeListsContent(MakeSdkConfig("/geproj", "/editor/SDK"));
    EXPECT_EQ(runtimeContent.find("EditorSDK.lib"), std::string::npos);
    EXPECT_EQ(runtimeContent.find("include-editor"), std::string::npos);
}

TEST(UserProjectGenerator, SourceGlobExcludesBuildTreesByTheAssetIgnoreRules)
{
    const auto config = MakeSdkConfig("/geproj", "/editor/SDK");
    const std::string content = ns::UserProjectGenerator::BuildCMakeListsContent(config);

    // The excluded names are the engine's asset ignore rules for the source root, not a
    // second list: a package whose sources sit at its root builds into a .Cache inside
    // that root, and the staleness digest walks the same rules.
    constexpr std::string_view kIgnoredDirsKey = "set(_ge_ignored_dirs \"";
    const std::size_t keyAt = content.find(kIgnoredDirsKey);
    ASSERT_NE(keyAt, std::string::npos);
    const std::size_t valueAt = keyAt + kIgnoredDirsKey.size();
    const std::size_t valueEnd = content.find('"', valueAt);
    ASSERT_NE(valueEnd, std::string::npos);
    const std::string names = ";" + content.substr(valueAt, valueEnd - valueAt) + ";";
    for (const char* name : {".cache", ".git", "cmakefiles", "out", "obj", "bin", "build"})
        EXPECT_NE(names.find(";" + std::string(name) + ";"), std::string::npos) << name;

    // Build trees that name themselves after a configuration need a name prefix.
    EXPECT_NE(content.find("set(_ge_ignored_dir_prefixes \"cmake-build-\")"), std::string::npos);

    // Matched on the lower-cased path relative to the source dir: ".Cache" and ".cache" are
    // the same directory, and a project that happens to live under an "out" or "bin"
    // directory does not filter its own sources away.
    EXPECT_NE(content.find("file(RELATIVE_PATH _ge_relative \"${GAMEENGINE_USER_SOURCE_DIR}\" "
                           "\"${_ge_source}\")"),
              std::string::npos);
    EXPECT_NE(content.find("string(TOLOWER \"/${_ge_relative}\" _ge_relative_lower)"),
              std::string::npos);
}

TEST(UserProjectGenerator, SourceGlobHonoursTheSourceRootsOwnAssetIgnoreFile)
{
    // A root-relative path prefix is a directory rule too: the staleness digest skips it, so the
    // compile must skip it as well, or an edit under it would compile without ever invalidating
    // the build cache.
    const fs::path root = fs::temp_directory_path() / "ge_userprojgen_assetignore";
    std::error_code ec;
    fs::remove_all(root, ec);
    const auto config = MakeSdkConfig(root, "/editor/SDK");
    fs::create_directories(config.SourceDir, ec);
    {
        std::ofstream ignoreFile(config.SourceDir / ".assetignore");
        ignoreFile << "# vendored test suites never compile into the module\n"
                      "ThirdParty/Tests/**\n"
                      "Scratch/\n";
    }

    const std::string content = ns::UserProjectGenerator::BuildCMakeListsContent(config);
    EXPECT_NE(content.find("set(_ge_ignored_path_prefixes \"thirdparty/tests/\")"), std::string::npos);
    EXPECT_NE(content.find("string(FIND \"${_ge_relative_lower}\" \"/${_ge_ignored_path_prefix}\" "
                           "_ge_match)"),
              std::string::npos);
    EXPECT_NE(content.find("if(_ge_match EQUAL 0)"), std::string::npos);
    // A directory-name rule from the same file joins the default names.
    EXPECT_NE(content.find(";scratch;"), std::string::npos);

    fs::remove_all(root, ec);
}

TEST(UserProjectGenerator, PackageModuleAtThePackageRootSkipsThePackagesTestsFolder)
{
    // A module whose root is the package root globs everything under it, so the
    // package's top-level Tests folder must be excluded by name here as it is in
    // discovery; a module rooted below the package root never reaches it.
    auto config = MakeSdkConfig("/geproj", "/editor/SDK");
    config.PackageRootDir = "/packages/helper-pack";
    config.SourceDir = config.PackageRootDir;
    const std::string atRoot = ns::UserProjectGenerator::BuildCMakeListsContent(config);
    EXPECT_NE(atRoot.find("set(_ge_ignored_path_prefixes \"tests/\")"), std::string::npos);

    config.SourceDir = config.PackageRootDir / "Editor";
    const std::string belowRoot = ns::UserProjectGenerator::BuildCMakeListsContent(config);
    EXPECT_NE(belowRoot.find("set(_ge_ignored_path_prefixes \"\")"), std::string::npos);

    config.PackageRootDir.clear();
    config.SourceDir = "/geproj/Assets";
    const std::string projectScripts = ns::UserProjectGenerator::BuildCMakeListsContent(config);
    EXPECT_NE(projectScripts.find("set(_ge_ignored_path_prefixes \"\")"), std::string::npos)
        << "a project's own Tests folder is its business";
}

TEST(UserProjectGenerator, OutOfSdkLayoutKeepsAbsolutePathsAndNoSdkBlock)
{
    // The integration-test shape: engine interface fed from the engine build tree,
    // no SDK root. Paths stay absolute and no GAMEENGINE_SDK_DIR machinery is emitted.
    ns::NativeBuildConfig config;
    config.SourceDir = "/work/Gameplay";
    config.BuildDir = "/work/build";
    config.IncludeDirs = {"/enginebuild/include"};
    config.EngineImportLib = "/enginebuild/lib/Engine.lib";
    config.SdkEntrySource = "/enginebuild/GameSDK/Source/UserModuleEntry.cpp";

    const std::string content = ns::UserProjectGenerator::BuildCMakeListsContent(config);

    EXPECT_EQ(content.find("GAMEENGINE_SDK_DIR"), std::string::npos);
    EXPECT_NE(content.find("\"/enginebuild/lib/Engine.lib\""), std::string::npos);
    EXPECT_NE(content.find("\"/enginebuild/GameSDK/Source/UserModuleEntry.cpp\""), std::string::npos);
    // BuildDir is a sibling of SourceDir, so the gen-TU glob still relativizes.
    EXPECT_NE(content.find("\"${CMAKE_CURRENT_SOURCE_DIR}/../build/*.gen.cpp\""), std::string::npos);
}

TEST(UserProjectGenerator, SeparateProjectDirKeepsGeneratedFilesOutOfPackageSources)
{
    const fs::path root = fs::temp_directory_path() / "ge_userprojgen_package_cache";
    std::error_code ec;
    fs::remove_all(root, ec);

    auto config = MakeSdkConfig(root / "Package", root / "Editor" / "SDK");
    config.SourceDir = root / "SignedEditor.app" / "Contents" / "MacOS" /
        "Packages" / "sample" / "Native";
    config.ProjectDir = root / "Derived" / "NativeScripts" / "project";
    config.BuildDir = root / "Derived" / "NativeScripts" / "build";
    fs::create_directories(config.SourceDir, ec);
    ASSERT_FALSE(ec);

    bool changed = false;
    std::string error;
    const fs::path generated = ns::UserProjectGenerator::Generate(config, changed, error);
    ASSERT_EQ(generated, config.ProjectDir / "CMakeLists.txt") << error;
    EXPECT_TRUE(changed);
    EXPECT_FALSE(fs::exists(config.SourceDir / "CMakeLists.txt"));

    const std::string content = ReadFile(generated);
    EXPECT_NE(content.find("set(GAMEENGINE_USER_SOURCE_DIR \"" +
                           config.SourceDir.generic_string() + "\")"),
              std::string::npos);
    EXPECT_NE(content.find("\"${GAMEENGINE_USER_SOURCE_DIR}/*.cpp\""), std::string::npos);
    EXPECT_NE(content.find("\"${GAMEENGINE_USER_SOURCE_DIR}\""), std::string::npos);
    EXPECT_NE(content.find("\"${CMAKE_CURRENT_SOURCE_DIR}/../build/*.gen.cpp\""),
              std::string::npos);

    fs::remove_all(root, ec);
}

TEST(UserProjectGenerator, SeparateProjectDirRemovesStaleGeneratedListsFromSources)
{
    const fs::path root = fs::temp_directory_path() / "ge_userprojgen_stray_cleanup";
    std::error_code ec;
    fs::remove_all(root, ec);

    auto config = MakeSdkConfig(root / "Package", root / "Editor" / "SDK");
    config.SourceDir = root / "Bundle" / "Packages" / "sample" / "Native";
    config.ProjectDir = root / "Derived" / "NativeScripts" / "project";
    config.BuildDir = root / "Derived" / "NativeScripts" / "build";
    fs::create_directories(config.SourceDir, ec);
    ASSERT_FALSE(ec);

    // A stray written into the source root by an engine that predates ProjectDir:
    // the marker prefix matches even though the tail differs from today's header.
    const fs::path stray = config.SourceDir / "CMakeLists.txt";
    {
        std::ofstream out(stray);
        out << "# AUTO-GENERATED by NativeScripting UserProjectGenerator (older engine)\n"
               "cmake_minimum_required(VERSION 3.20)\n";
    }

    bool changed = false;
    std::string error;
    ASSERT_FALSE(ns::UserProjectGenerator::Generate(config, changed, error).empty()) << error;
    EXPECT_FALSE(fs::exists(stray)) << "marker-bearing stray must be removed";

    // A user-authored CMakeLists.txt (no marker) is never touched.
    {
        std::ofstream out(stray);
        out << "# hand-authored package build\n";
    }
    ASSERT_FALSE(ns::UserProjectGenerator::Generate(config, changed, error).empty()) << error;
    EXPECT_TRUE(fs::exists(stray)) << "user-authored CMakeLists.txt must be preserved";

    fs::remove_all(root, ec);
}

TEST(UserProjectGenerator, DeadBakedSdkDirIsRefreshedOnNextGenerate)
{
    const fs::path root = fs::temp_directory_path() / "ge_userprojgen_relocation";
    std::error_code ec;
    fs::remove_all(root, ec);

    // Generate against the "old" SDK location, then delete it — the on-disk file now
    // bakes a dead last-known-good, exactly what a moved/deleted Editor leaves behind.
    const fs::path oldSdk = root / "OldEditor" / "SDK";
    auto config = MakeSdkConfig(root / "Project", oldSdk);
    fs::create_directories(config.SourceDir, ec);
    fs::create_directories(oldSdk, ec);
    ASSERT_FALSE(ec);

    bool changed = false;
    std::string error;
    const fs::path file = ns::UserProjectGenerator::Generate(config, changed, error);
    ASSERT_FALSE(file.empty()) << error;
    EXPECT_TRUE(changed);
    ASSERT_NE(ReadFile(file).find(oldSdk.generic_string()), std::string::npos);

    fs::remove_all(root / "OldEditor", ec);

    // The engine touches the project again from the SDK's new home: the file must be
    // rewritten with the new last-known-good (Generate also fires the ERROR diagnostic
    // naming the dead dir — message discipline pinned by review, not asserted here).
    const fs::path newSdk = root / "NewEditor" / "SDK";
    config = MakeSdkConfig(root / "Project", newSdk);
    fs::create_directories(newSdk, ec);
    changed = false;
    ASSERT_FALSE(ns::UserProjectGenerator::Generate(config, changed, error).empty()) << error;
    EXPECT_TRUE(changed) << "a dead baked SDK dir must be refreshed on the next engine touch";

    const std::string refreshed = ReadFile(file);
    EXPECT_NE(refreshed.find("set(GAMEENGINE_SDK_DIR \"" + newSdk.generic_string() + "\")"), std::string::npos);
    EXPECT_EQ(refreshed.find(oldSdk.generic_string()), std::string::npos);

    // Steady state: a repeat touch (the cached-load refresh path) is a byte-identical no-op.
    changed = true;
    ASSERT_FALSE(ns::UserProjectGenerator::Generate(config, changed, error).empty()) << error;
    EXPECT_FALSE(changed);

    fs::remove_all(root, ec);
}
