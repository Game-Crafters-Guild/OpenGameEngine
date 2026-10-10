#include "AssetDatabase/AssetDatabasePaths.h"
#include "Assets/Packages/PackageResolver.h"
#include "Engine/Build/MacBundleAssembler.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/RuntimeDependencyStaging.h"
#include "Engine/Build/ShellUtil.h"
#include "Logger/Logger.h"
#include "Assets/Packages/PackagesIndex.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "Scripting/NativeAotScriptsLibrary.h"
#include "Scripting/PathResolver.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace GameEngine {

namespace {

// Run a shell command, capturing combined stdout/stderr. Returns 0 on a clean exit,
// non-zero otherwise. This file is compiled on every platform (it is in the Engine
// sources) but only CALLED on macOS, so popen/pclose are spelled per-platform to keep
// the Windows (MSVC) build compiling.
#if defined(_WIN32)
  #define GE_MBA_POPEN _popen
  #define GE_MBA_PCLOSE _pclose
#else
  #define GE_MBA_POPEN popen
  #define GE_MBA_PCLOSE pclose
#endif
int RunCommand(const std::string& command, std::string* output = nullptr)
{
    FILE* pipe = GE_MBA_POPEN(command.c_str(), "r");
    if (!pipe)
        return -1;

    std::array<char, 512> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe))
    {
        if (output)
            *output += buf.data();
    }
    return GE_MBA_PCLOSE(pipe);
}
#undef GE_MBA_POPEN
#undef GE_MBA_PCLOSE

// Derive a reverse-DNS bundle identifier from a display name: lowercase, keep
// only [a-z0-9], so "My Game" -> "com.gameengine.mygame".
std::string SanitizeBundleId(const std::string& gameName)
{
    std::string slug;
    for (char c : gameName)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc))
            slug += static_cast<char>(std::tolower(uc));
    }
    if (slug.empty())
        slug = "game";
    return "com.gameengine." + slug;
}

// Set a string key in an Info.plist, adding it if absent. PlistBuddy "Set"
// errors when the key does not exist, so fall back to "Add".
void SetPlistString(const fs::path& plist, const std::string& key, const std::string& value)
{
    // PlistBuddy re-parses the -c argument with its own quote-aware lexer, so a bare
    // value containing an apostrophe/space (e.g. "Bob's Game") breaks both Set and Add
    // and the key is silently never written. Double-quote the value (escaping internal
    // " and \) so PlistBuddy sees one token; ShellQuote then protects the whole -c arg.
    std::string escaped;
    for (char c : value)
    {
        if (c == '"' || c == '\\')
            escaped += '\\';
        escaped += c;
    }
    const std::string quotedVal = "\"" + escaped + "\"";
    const std::string plistArg = ShellQuote(plist);
    const std::string setCmd =
        "/usr/libexec/PlistBuddy -c " + ShellQuote("Set :" + key + " " + quotedVal) + " " + plistArg + " 2>/dev/null";
    if (RunCommand(setCmd) != 0)
    {
        const std::string addCmd =
            "/usr/libexec/PlistBuddy -c " + ShellQuote("Add :" + key + " string " + quotedVal) + " " + plistArg + " 2>&1";
        RunCommand(addCmd);
    }
}

// The rpath every module in the bundle resolves libEngine through: the bundle's own
// Contents/Frameworks, where the template ships it.
constexpr const char* kBundleFrameworksRpath = "@executable_path/../Frameworks";

// The distinct LC_RPATH entries of a Mach-O image, in load-command order. otool prints each as
// "cmd LC_RPATH", "cmdsize N", "path <path> (offset N)", and prints the load commands of every
// slice of a universal image, so a path repeats once per slice; install_name_tool edits every
// slice at once and refuses a path named twice.
std::vector<std::string> ReadRpaths(const fs::path& image)
{
    constexpr std::string_view kPathField = " path ";
    std::string listing;
    RunCommand("otool -l " + ShellQuote(image) + " 2>/dev/null", &listing);
    std::vector<std::string> rpaths;
    bool inRpathCommand = false;
    std::istringstream lines(listing);
    for (std::string line; std::getline(lines, line);)
    {
        if (line.find("cmd LC_RPATH") != std::string::npos)
        {
            inRpathCommand = true;
            continue;
        }
        const size_t pathAt = line.find(kPathField);
        if (!inRpathCommand || pathAt == std::string::npos)
            continue;
        std::string rpath = line.substr(pathAt + kPathField.size());
        rpath = rpath.substr(0, rpath.rfind(" (offset "));
        if (std::find(rpaths.begin(), rpaths.end(), rpath) == rpaths.end())
            rpaths.push_back(std::move(rpath));
        inRpathCommand = false;
    }
    return rpaths;
}

// Confines a module's @rpath to the bundle: deletes every absolute rpath (the build
// machine's SDK directories the editor linked the module against, which dyld would
// otherwise search before the bundle and which do not exist on a player's machine) and
// adds kBundleFrameworksRpath when absent. Rpaths relative to the image or the
// executable stay. Runs before codesign re-signs the image.
bool ConfineRpathsToBundle(const fs::path& image, std::string& error)
{
    std::string edits;
    bool hasBundleRpath = false;
    for (const std::string& rpath : ReadRpaths(image))
    {
        if (rpath == kBundleFrameworksRpath)
            hasBundleRpath = true;
        else if (!rpath.empty() && rpath.front() != '@')
            edits += " -delete_rpath " + ShellQuote(rpath);
    }
    if (!hasBundleRpath)
        edits += " -add_rpath " + ShellQuote(std::string(kBundleFrameworksRpath));
    if (edits.empty())
        return true;
    std::string output;
    if (RunCommand("install_name_tool" + edits + " " + ShellQuote(image) + " 2>&1", &output) != 0)
    {
        error = output;
        return false;
    }
    return true;
}

// The roots a packaged Player loads native modules from (PlayerApplication): the
// content root for the project's module and Packages/<alias> for each package's.
std::vector<fs::path> NativeModuleRoots(const fs::path& contentRoot)
{
    std::vector<fs::path> roots{contentRoot};
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(contentRoot / kPackagesStagingDirName, ec))
    {
        if (entry.is_directory(ec))
            roots.push_back(entry.path());
    }
    return roots;
}

// Moves the module recorded under <moduleRoot>/NativeScripts into the matching
// directory of Contents/Frameworks and rewrites its record relative to moduleRoot.
// A root with no record has no module and is left alone.
bool MoveNativeModuleIntoFrameworks(const fs::path& contents, const fs::path& moduleRoot,
                                    std::vector<std::string>& errors)
{
    namespace ns = NativeScripting;
    const fs::path recordDir = moduleRoot / "NativeScripts";
    const std::optional<ns::BuildCacheRecord> record = ns::ReadBuildCacheRecord(recordDir);
    if (!record)
        return true;

    std::error_code ec;
    const fs::path staged = (moduleRoot / record->DllPath).lexically_normal();
    if (fs::path(record->DllPath).is_absolute() || !fs::is_regular_file(staged, ec))
    {
        errors.push_back("Native module recorded in '" + recordDir.generic_string() + "' is not staged in the bundle ('" +
                         record->DllPath + "'); the game would run without it");
        return false;
    }

    const fs::path frameworksDir =
        (contents / "Frameworks" / moduleRoot.lexically_relative(contents / "Resources")).lexically_normal();
    const fs::path destination = frameworksDir / staged.filename();
    if (fs::exists(destination, ec))
    {
        errors.push_back("Native module '" + staged.filename().string() + "' would replace '" +
                         destination.lexically_relative(contents).generic_string() +
                         "', which is already in the bundle; rename the module");
        return false;
    }
    fs::create_directories(frameworksDir, ec);
    fs::rename(staged, destination, ec);
    if (ec)
    {
        errors.push_back("Failed to move native module '" + staged.filename().string() +
                         "' into Contents/Frameworks: " + ec.message());
        return false;
    }
    // The staged copy may carry the build output's read-only permissions;
    // install_name_tool edits it in place.
    fs::permissions(destination, fs::perms::owner_write, fs::perm_options::add, ec);

    std::string rpathError;
    if (!ConfineRpathsToBundle(destination, rpathError))
    {
        errors.push_back("Failed to point native module '" + staged.filename().string() +
                         "' at the bundle's Frameworks (the game would run without it): " + rpathError);
        return false;
    }

    ns::BuildCacheRecord moved = *record;
    moved.DllPath = destination.lexically_relative(moduleRoot).generic_string();
    if (!ns::WriteBuildCacheRecord(recordDir, moved))
    {
        errors.push_back("Failed to rewrite the native module record in '" + recordDir.generic_string() +
                         "'; the game would load no module from it");
        return false;
    }
    Logger::Log::Info("MacBundle: moved native module '{}' into {}", staged.filename().string(),
                      frameworksDir.lexically_relative(contents).generic_string());
    return true;
}

} // namespace

bool MacBundleAssembler::PrepareBundle(const fs::path& sdkTemplateApp,
                                       const fs::path& bundlePath,
                                       std::vector<std::string>& errors)
{
    std::error_code ec;
    if (!fs::exists(sdkTemplateApp, ec))
    {
        errors.push_back("Prebuilt Player.app template not found in SDK: " + sdkTemplateApp.string() +
                         " (build the Editor so it stages templates/Player.app)");
        return false;
    }

    fs::remove_all(bundlePath, ec);
    fs::create_directories(bundlePath.parent_path(), ec);

    // ditto preserves bundle symlinks (the libvulkan loader aliases) and metadata
    // — a plain recursive file copy would dereference them.
    std::string out;
    int rc = RunCommand("/usr/bin/ditto " + ShellQuote(sdkTemplateApp) + " " + ShellQuote(bundlePath) + " 2>&1", &out);
    if (rc != 0)
    {
        errors.push_back("Failed to copy Player.app template into staging: " + out);
        return false;
    }

    // Scrub what running the template from inside its bundle can leave beside the
    // executable, so the bundle starts generic. The pipeline populates
    // Contents/Resources (Assets/, game.config); the engine assets and Frameworks
    // already there are kept (they ship with the template). The template's managed
    // assemblies (Contents/Resources/Managed, staged for running the Player in place)
    // are dropped: InjectRuntime stages a game's managed set whole, and a game
    // without C# ships none.
    const fs::path macOSDir = bundlePath / "Contents" / "MacOS";
    for (const char* stale : {".Cache", AssetDatabase::kTextureCacheDirectoryName, "game.log", "ScriptAssemblies", "tools"})
        fs::remove_all(macOSDir / stale, ec);
    fs::remove(macOSDir / "AssetDatabase.assetdb", ec);
    fs::remove_all(bundlePath / "Contents" / "Resources" / "Managed", ec);
    // The Player.app the template is made from stages every engine package beside its executable
    // for development runs: package sources, editor-only modules and the marker naming the build
    // machine's checkout. A game ships only the packages the pipeline stages under its content root,
    // Contents/Resources/Packages, so the bundle leaves that development copy out.
    fs::remove_all(macOSDir / kStagedEnginePackagesDirName, ec);
    fs::create_directories(macOSDir, ec);

    Logger::Log::Info("MacBundle: prepared bundle from template at '{}'", bundlePath.string());
    return true;
}

bool MacBundleAssembler::InjectRuntime(const BuildSettings& settings,
                                       const fs::path& bundlePath,
                                       const fs::path& scriptScratchDir,
                                       const fs::path& icnsPath,
                                       std::vector<std::string>& errors,
                                       std::vector<std::string>& warnings)
{
    std::error_code ec;
    const fs::path contents = bundlePath / "Contents";
    const fs::path macOSDir = contents / "MacOS";
    const fs::path resources = contents / "Resources";
    const fs::path managedDst = resources / "Managed";

    if (!fs::is_directory(macOSDir, ec))
    {
        errors.push_back("Bundle is missing Contents/MacOS — PrepareBundle must run first");
        return false;
    }

    // --- C# managed assemblies (CoreCLR path) ---
    // CompileScripts wrote the user's GameEngine.Scripts.dll (+ referenced
    // assemblies + deps/runtimeconfig) into <scratch>/Managed. Copy it into
    // Contents/Resources/Managed, the Player's engine managed directory
    // (PathResolver), then stage beside it the engine assemblies the Player loads,
    // from this editor's own managed directory: the set the scripts compiled
    // against. Drop *.pdb so codesign does not reject unsigned subcomponents.
    const fs::path managedSrc = scriptScratchDir / "Managed";
    if (fs::is_directory(managedSrc, ec))
    {
        fs::create_directories(managedDst, ec);
        int copied = 0, failed = 0;
        for (const auto& entry : fs::recursive_directory_iterator(managedSrc, ec))
        {
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() == ".pdb")
                continue;
            const fs::path rel = fs::relative(entry.path(), managedSrc, ec);
            const fs::path dst = managedDst / rel;
            std::error_code cpEc;
            fs::create_directories(dst.parent_path(), cpEc);
            fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing, cpEc);
            cpEc ? ++failed : ++copied;
        }
        if (failed > 0)
            warnings.push_back("Failed to inject " + std::to_string(failed) +
                               " managed assembly file(s) into Contents/Resources/Managed");
        Logger::Log::Info("MacBundle: injected {} managed assembly file(s) into Contents/Resources/Managed", copied);

        if (!StageManagedRuntimeAssemblies(ScriptingPaths::ResolveEngineManagedDirectoryFrom(settings.runtimeDepsPath),
                                           managedDst, errors))
            return false;
    }

    // --- C# NativeAOT (Release) ---
    // The self-contained scripts dylib goes next to the Player binary, where the
    // Player auto-detects it (kNativeAotScriptsLibraryName). GameEngine.Native
    // (the C ABI shim it P/Invokes into) already ships in Contents/Frameworks.
    {
        const fs::path aotSrc = scriptScratchDir / kNativeAotScriptsLibraryName;
        if (fs::exists(aotSrc, ec))
        {
            fs::copy_file(aotSrc, macOSDir / kNativeAotScriptsLibraryName,
                          fs::copy_options::overwrite_existing, ec);
            Logger::Log::Info("MacBundle: injected NativeAOT scripts dylib next to Player");
        }
    }

    // --- App icon ---
    if (!icnsPath.empty() && fs::exists(icnsPath, ec))
    {
        fs::create_directories(resources, ec);
        fs::copy_file(icnsPath, resources / "AppIcon.icns", fs::copy_options::overwrite_existing, ec);
    }

    // --- Info.plist branding (keep CFBundleExecutable=Player; only the .app dir
    // is renamed to the game name) ---
    const fs::path plist = contents / "Info.plist";
    if (fs::exists(plist, ec))
    {
        const std::string& gameName = settings.playerConfig.gameName;
        SetPlistString(plist, "CFBundleName", gameName);
        SetPlistString(plist, "CFBundleDisplayName", gameName);
        SetPlistString(plist, "CFBundleIdentifier", SanitizeBundleId(gameName));
        if (!icnsPath.empty() && fs::exists(resources / "AppIcon.icns", ec))
            SetPlistString(plist, "CFBundleIconFile", "AppIcon");
    }
    else
    {
        warnings.push_back("Bundle Info.plist not found — branding not applied");
    }

    return true;
}

bool MacBundleAssembler::MoveNativeModulesIntoFrameworks(const fs::path& bundlePath, std::vector<std::string>& errors)
{
    const fs::path contents = bundlePath / "Contents";
    bool moved = true;
    for (const fs::path& moduleRoot : NativeModuleRoots(contents / "Resources"))
        moved = MoveNativeModuleIntoFrameworks(contents, moduleRoot, errors) && moved;
    return moved;
}

bool MacBundleAssembler::Codesign(const fs::path& bundlePath,
                                  const std::string& identity,
                                  std::vector<std::string>& errors)
{
    std::error_code ec;
    const std::string id = identity.empty() ? "-" : identity;

    // Injection invalidated the template's signature; start clean.
    fs::remove_all(bundlePath / "Contents" / "_CodeSignature", ec);
    RunCommand("xattr -cr " + ShellQuote(bundlePath) + " 2>&1");
    RunCommand("chmod -R u+w " + ShellQuote(bundlePath) + " 2>&1");

    // --deep signs nested code (Frameworks, dylibs) before the outer bundle.
    std::string out;
    const int rc = RunCommand(
        "codesign --force --deep --sign " + ShellQuote(id) + " " + ShellQuote(bundlePath) + " 2>&1", &out);
    if (rc != 0)
    {
        errors.push_back("codesign failed: " + out);
        return false;
    }

    out.clear();
    if (RunCommand("codesign --verify --deep --strict " + ShellQuote(bundlePath) + " 2>&1", &out) != 0)
    {
        errors.push_back("Signed bundle verification failed: " + out);
        return false;
    }

    Logger::Log::Info("MacBundle: re-signed bundle ({}) at '{}'",
                      identity.empty() ? "ad-hoc" : identity, bundlePath.string());
    return true;
}

} // namespace GameEngine
