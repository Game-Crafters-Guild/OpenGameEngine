#include <gtest/gtest.h>

#include "EngineHostAbiLayoutProbe.h"

#include "Core/Application.h"
#include "Core/Engine.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

// The engine ships as Engine.dll and its hosts — the Editor, a packaged game,
// a native user-script module — compile the same public headers into their own
// images. Every inline accessor in a host therefore reads engine objects at
// offsets its own compile produced. Two properties keep that sound, and this
// suite is the gate for both.
//
// 1. A host's configuration decides NDEBUG; the engine's decides its own. The
//    Editor's daily driver is DebugFast (no NDEBUG) and the game it packages is
//    built Release (NDEBUG), so any engine type that changes layout with NDEBUG
//    is a type the two sides disagree about. Public headers therefore declare
//    their debug-only members under GE_DEBUG_INSTRUMENTATION, which travels with
//    the engine. The probe compares the types a host reads through inline
//    accessors; the header sweep covers every public header, including types
//    the probe does not name.
//
// 2. Whatever else diverges, the disagreement is announced. EngineCore::Initialize
//    compares the host's GE_DEBUG_INSTRUMENTATION against its own and refuses
//    rather than handing back objects the caller will read at the wrong offsets.
namespace
{

using GameEngine::Testing::EnginePublicTypeLayout;

TEST(EngineHostAbiLayout, PublicTypesLayOutTheSameWithAndWithoutNDebug)
{
    const EnginePublicTypeLayout withNDebug =
        GameEngine::Testing::MeasureEnginePublicTypeLayoutWithNDebug();
    const EnginePublicTypeLayout withoutNDebug =
        GameEngine::Testing::MeasureEnginePublicTypeLayoutWithoutNDebug();

    // Instrument check before any comparison: the two probes are only evidence
    // if they really were compiled under different NDEBUG states. If a build
    // change ever made them agree, every comparison below would compare a
    // value with itself.
    ASSERT_TRUE(withNDebug.NDebugDefined);
    ASSERT_FALSE(withoutNDebug.NDebugDefined);

    // A failure here names the type whose public header grew or lost a member
    // under NDEBUG. Gate that member on GE_DEBUG_INSTRUMENTATION instead: the
    // engine's build sets it, the SDK republishes the staged engine's value, so
    // both sides of the DLL boundary agree whatever the host's configuration is.
    EXPECT_EQ(withNDebug.RenderServices, withoutNDebug.RenderServices);
    EXPECT_EQ(withNDebug.MeshGPURegistry, withoutNDebug.MeshGPURegistry);
    EXPECT_EQ(withNDebug.MaterialBinder, withoutNDebug.MaterialBinder);
    EXPECT_EQ(withNDebug.PipelineVariantCache, withoutNDebug.PipelineVariantCache);
    EXPECT_EQ(withNDebug.RenderExtractionSystem, withoutNDebug.RenderExtractionSystem);
    EXPECT_EQ(withNDebug.WorkStealingThreadPool, withoutNDebug.WorkStealingThreadPool);
    EXPECT_EQ(withNDebug.TaskDependencyGraph, withoutNDebug.TaskDependencyGraph);
    EXPECT_EQ(withNDebug.ApplicationConfig, withoutNDebug.ApplicationConfig);
}

TEST(EngineHostAbiLayout, InitializeRefusesAHostBuiltAgainstTheOtherInstrumentationState)
{
    // The refusal runs before Initialize claims the lifecycle, so this leaves
    // the process's EngineCore untouched and needs no window, device or shutdown.
    GameEngine::ApplicationConfig config;
    ASSERT_EQ(config.HostDebugInstrumentation,
              static_cast<GameEngine::uint32>(GE_DEBUG_INSTRUMENTATION));

    config.HostDebugInstrumentation = GE_DEBUG_INSTRUMENTATION ? 0u : 1u;
    EXPECT_FALSE(GameEngine::EngineCore::GetInstance().Initialize(config));
    EXPECT_FALSE(GameEngine::EngineCore::GetInstance().IsInitialized());
}

// Macros whose value a host's configuration decides independently of the
// engine's: NDEBUG follows the host's own configuration, GE_DEV_DIAG and
// GE_DEBUGFAST are set by this repository's CMake and never republished by the
// SDK. A declaration a public header gates on any of them is compiled into a
// host and into the engine under different conditions. _DEBUG is deliberately
// not listed: it follows the CRT flavour, which the SDK's library selection and
// the native ABI marker's CrtId already refuse to mix.
//
// Each allowed site is one that cannot move a member. Its file must still test
// the macro (a stale entry fails), and any hit outside this list fails with the
// fix named.
struct AllowedConsumerSideGuard
{
    std::string_view RelativePath;
    std::string_view Reason;
};

constexpr std::array<AllowedConsumerSideGuard, 5> kAllowedConsumerSideGuards{{
    {"Engine/Include/Engine/Rendering/VkValidationRequest.h",
     "inline default inside a function body; the calling app's config, not layout"},
    {"Engine/Modules/ECS/Include/ECS/ArchetypeTable.h",
     "inline zero-fill of a freshly allocated chunk; code, not a member"},
    {"Engine/Modules/JobSystem/Include/JobSystem/JobCounter.h",
     "assert bodies; assert() follows NDEBUG whatever surrounds it"},
    {"Engine/Modules/Logger/Include/Logger/Logger.h",
     "LOG_TRACE and LOG_DEBUG selection; the consumer's own choice"},
    {"Engine/Modules/Rendering/Include/Rendering/Core/TextureFormatSupportGate.h",
     "inline return value; behaviour, not layout"},
}};

struct ConsumerSideGuardHit
{
    std::string RelativePath;
    int Line;
    std::string Text;
};

bool IsPublicHeader(const std::filesystem::path& path)
{
    const std::string extension = path.extension().generic_string();
    return extension == ".h" || extension == ".hpp" || extension == ".inl";
}

void CollectPublicHeadersUnder(const std::filesystem::path& root, bool includeDirectoriesOnly,
                               std::vector<std::filesystem::path>& headers)
{
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
    {
        if (ec || !it->is_regular_file() || !IsPublicHeader(it->path()))
            continue;
        const std::string relative = std::filesystem::relative(it->path(), root, ec).generic_string();
        if (includeDirectoriesOnly && relative.find("/Include/") == std::string::npos)
            continue;
        headers.push_back(it->path());
    }
}

// Public header roots: Engine/Include, Apps/Editor/Include (the EditorSDK
// surface) and every Include directory under Engine/Modules and
// Engine/ECSModules, nested module SDKs included.
std::vector<std::filesystem::path> CollectPublicHeaders(const std::filesystem::path& sourceDir)
{
    std::vector<std::filesystem::path> headers;
    CollectPublicHeadersUnder(sourceDir / "Engine" / "Include", false, headers);
    CollectPublicHeadersUnder(sourceDir / "Apps" / "Editor" / "Include", false, headers);
    CollectPublicHeadersUnder(sourceDir / "Engine" / "Modules", true, headers);
    CollectPublicHeadersUnder(sourceDir / "Engine" / "ECSModules", true, headers);
    return headers;
}

std::vector<ConsumerSideGuardHit> FindConsumerSideGuards(const std::filesystem::path& sourceDir)
{
    static const std::regex kGuardLine(
        R"(^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\b(?:NDEBUG|GE_DEV_DIAG|GE_DEBUGFAST)\b)");
    std::vector<ConsumerSideGuardHit> hits;
    for (const std::filesystem::path& header : CollectPublicHeaders(sourceDir))
    {
        std::ifstream in(header);
        std::string line;
        int lineNumber = 0;
        while (std::getline(in, line))
        {
            ++lineNumber;
            const std::string code = line.substr(0, line.find("//"));
            if (!std::regex_search(code, kGuardLine))
                continue;
            std::error_code ec;
            hits.push_back({std::filesystem::relative(header, sourceDir, ec).generic_string(), lineNumber, line});
        }
    }
    return hits;
}

TEST(EngineHostAbiLayout, PublicHeadersGateDebugDeclarationsOnTheEngineSwitch)
{
    const std::filesystem::path sourceDir(GE_ENGINE_SOURCE_DIR);
    std::error_code ec;
    if (!std::filesystem::is_directory(sourceDir, ec))
        GTEST_SKIP() << "engine source tree not present at " << sourceDir.generic_string()
                     << "; the public-header sweep needs the headers this binary was built from";

    const std::vector<ConsumerSideGuardHit> hits = FindConsumerSideGuards(sourceDir);
    ASSERT_FALSE(hits.empty()) << "the sweep matched no public header at all; the instrument is broken";

    for (const ConsumerSideGuardHit& hit : hits)
    {
        const bool allowed =
            std::any_of(kAllowedConsumerSideGuards.begin(), kAllowedConsumerSideGuards.end(),
                        [&](const AllowedConsumerSideGuard& guard) { return guard.RelativePath == hit.RelativePath; });
        EXPECT_TRUE(allowed)
            << hit.RelativePath << ":" << hit.Line << ": " << hit.Text << "\n"
            << "A public header tests a macro the host's configuration decides. If this block declares a "
               "member, a virtual, a base or a default argument, gate it on GE_DEBUG_INSTRUMENTATION instead "
               "(the engine's build sets it and the SDK republishes it). If it cannot move a member, add the "
               "file to kAllowedConsumerSideGuards with the reason.";
    }

    for (const AllowedConsumerSideGuard& guard : kAllowedConsumerSideGuards)
    {
        const bool stillPresent =
            std::any_of(hits.begin(), hits.end(),
                        [&](const ConsumerSideGuardHit& hit) { return hit.RelativePath == guard.RelativePath; });
        EXPECT_TRUE(stillPresent) << guard.RelativePath
                                  << " no longer tests a consumer-side macro; remove it from kAllowedConsumerSideGuards";
    }
}

} // namespace
