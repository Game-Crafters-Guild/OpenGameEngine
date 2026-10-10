// Cache-key correctness for ShaderCompileService::CompileProgramToCache.
//
// The on-disk cache key must be derived ONLY from true compile inputs, and none
// of them may be machine-specific — a cache cooked on one machine has to
// address identically wherever the content tree lands:
//  - debugName and the source FILE NAME are diagnostics-only — never in the
//    key. (Over-keying on them made every material of a 100-material model
//    compile its own ~105ms shader instead of hitting after the first.)
//  - The source DIRECTORY and req.includeDirs are LOCATION, not content, and
//    stay out. What they were a proxy for — which file each #include resolves
//    to — is folded directly as the closure's content hashes.
//  - stage / entryPoint / defines / source bytes are compile inputs.
//
// Observables: ShaderProgramCompileResult::outputDir is cacheRoot/<hex key>,
// and ShaderCompileService::GetTotalCompilations() counts real compiles
// (cache hits do not increment it).

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderCompileService.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

// Self-cleaning unique temp tree; a fresh cacheRoot per test keeps every
// test's first compile cold regardless of run order.
struct TempTree
{
    fs::path Root;

    explicit TempTree(const char* prefix)
    {
        static std::atomic<uint32_t> counter{0};
        Root = fs::temp_directory_path() /
               (std::string(prefix) + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "_" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(Root);
    }
    ~TempTree()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
};

void WriteTextFile(const fs::path& p, const char* text)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// Valid as both a vertex and a fragment stage (no stage-specific builtins).
constexpr const char* kMinimalGlsl = "#version 450\nvoid main() { }\n";

ShaderProgramCompileRequest MakeInlineRequest(const fs::path& cacheRoot,
                                              const fs::path& sourcePath,
                                              const std::string& debugName,
                                              const char* source = kMinimalGlsl,
                                              const char* stage = "vs")
{
    ShaderProgramCompileRequest req{};
    req.debugName = debugName;
    req.baseDirectory = sourcePath.parent_path();
    req.cacheRoot = cacheRoot;
    ShaderStageCompileSpec s{};
    s.stage = stage;
    s.sourcePath = sourcePath;
    s.inlineSource = source;
    req.stages.push_back(std::move(s));
    return req;
}

// First compile of a test: skip the whole test when shaderc isn't built in.
#define COMPILE_OR_SKIP(req, result)                                                        \
    do                                                                                      \
    {                                                                                       \
        std::string firstErr;                                                               \
        const bool firstOk = ShaderCompileService::CompileProgramToCache((req), ShaderSourceKind::SpirV, (result),   \
                                                                         &firstErr);        \
        if (!firstOk && firstErr.find("shaderc is not available") != std::string::npos)     \
            GTEST_SKIP() << "shaderc not built into this target";                           \
        ASSERT_TRUE(firstOk) << firstErr;                                                   \
    } while (0)

} // namespace

TEST(ShaderCompileCacheKey, DebugNameStaysOutOfKey)
{
    TempTree tree("ge_cachekey_debugname");
    const fs::path cache = tree.Root / "Cache";
    const fs::path src = tree.Root / "probe.vert";

    ShaderProgramCompileResult resA{};
    COMPILE_OR_SKIP(MakeInlineRequest(cache, src, "material_041_69"), resA);

    const uint64_t afterFirst = ShaderCompileService::GetTotalCompilations();

    ShaderProgramCompileResult resB{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        MakeInlineRequest(cache, src, "material_041_70"), ShaderSourceKind::SpirV, resB, &err))
        << err;

    EXPECT_EQ(resA.outputDir, resB.outputDir)
        << "requests differing only in debugName must share a cache entry";
    EXPECT_EQ(resA.shaderPkgPath, resB.shaderPkgPath);
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst)
        << "second request must be a cache HIT, not a recompile";
    EXPECT_FALSE(resB.stageBytes.at("vs").empty());
}

TEST(ShaderCompileCacheKey, SourceFileNameStaysOutOfKey)
{
    // The per-material composed-adapter scheme emits per-material FILE NAMES in
    // one shared directory; identical composed bytes must dedupe across them.
    TempTree tree("ge_cachekey_srcname");
    const fs::path cache = tree.Root / "Cache";

    ShaderProgramCompileResult resA{};
    COMPILE_OR_SKIP(MakeInlineRequest(cache, tree.Root / "matA_composed.vert", "probe"), resA);

    const uint64_t afterFirst = ShaderCompileService::GetTotalCompilations();

    ShaderProgramCompileResult resB{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        MakeInlineRequest(cache, tree.Root / "matB_composed.vert", "probe"), ShaderSourceKind::SpirV, resB, &err))
        << err;

    EXPECT_EQ(resA.outputDir, resB.outputDir)
        << "same directory + same source bytes: the file name must not split the key";
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst);
}

TEST(ShaderCompileCacheKey, SourceDirectoryStaysOutOfKey)
{
    // A source's directory is WHERE it sits, not what it compiles to. Keying it
    // would give every relocated content tree a cold cache — the cooked-cache
    // ship path depends on this staying out.
    TempTree tree("ge_cachekey_srcdir");
    const fs::path cache = tree.Root / "Cache";

    ShaderProgramCompileResult resA{};
    COMPILE_OR_SKIP(MakeInlineRequest(cache, tree.Root / "DirA" / "probe.vert", "probe"), resA);

    const uint64_t afterFirst = ShaderCompileService::GetTotalCompilations();

    ShaderProgramCompileResult resB{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        MakeInlineRequest(cache, tree.Root / "DirB" / "probe.vert", "probe"), ShaderSourceKind::SpirV, resB, &err))
        << err;

    EXPECT_EQ(resA.outputDir, resB.outputDir)
        << "identical source under a different directory must address the same cache entry";
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst)
        << "the relocated request must HIT, not recompile";
}

TEST(ShaderCompileCacheKey, IncludeDirsStayOutOfKey)
{
    // Include roots were only ever a proxy for "which files the #includes
    // resolve to"; the key folds that closure's CONTENT directly, so the roots
    // themselves are machine-specific noise.
    TempTree tree("ge_cachekey_incdirs");
    const fs::path cache = tree.Root / "Cache";
    const fs::path src = tree.Root / "probe.vert";

    ShaderProgramCompileResult resA{};
    COMPILE_OR_SKIP(MakeInlineRequest(cache, src, "probe"), resA);

    auto reqB = MakeInlineRequest(cache, src, "probe");
    reqB.includeDirs = {tree.Root / "ExtraIncludes"};
    ShaderProgramCompileResult resB{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(reqB, ShaderSourceKind::SpirV, resB, &err)) << err;

    EXPECT_EQ(resA.outputDir, resB.outputDir)
        << "an include root that changes no resolved content changes no key";
}

TEST(ShaderCompileCacheKey, SameIncludeSpellingDifferentRootContentIsADifferentKey)
{
    // The discrimination the absolute include literal used to provide, now
    // carried by content: byte-identical source, the same relative #include
    // spelling, two roots holding different files under that spelling.
    TempTree tree("ge_cachekey_rootcontent");
    const fs::path cache = tree.Root / "Cache";
    const fs::path rootA = tree.Root / "RootA";
    const fs::path rootB = tree.Root / "RootB";
    WriteTextFile(rootA / "Surfaces" / "dup.glsl", "// origin A\n");
    WriteTextFile(rootB / "Surfaces" / "dup.glsl", "// origin B\n");

    const char* src = "#version 450\n#include \"Surfaces/dup.glsl\"\nvoid main() { }\n";

    auto reqA = MakeInlineRequest(cache, tree.Root / "probe.vert", "probe", src);
    reqA.includeDirs = {rootA};
    ShaderProgramCompileResult resA{};
    COMPILE_OR_SKIP(reqA, resA);

    auto reqB = MakeInlineRequest(cache, tree.Root / "probe.vert", "probe", src);
    reqB.includeDirs = {rootB};
    ShaderProgramCompileResult resB{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(reqB, ShaderSourceKind::SpirV, resB, &err)) << err;

    EXPECT_NE(resA.outputDir, resB.outputDir)
        << "same-named shaders from different origins must never collide in .Cache/Shaders";
}

TEST(ShaderCompileCacheKey, CookedTreeRelocatesAndStillHitsWithoutACompiler)
{
    // The ship path end to end: cook a program whose source pulls an #include
    // through a search root, move the WHOLE tree (sources, include root and
    // cache) to a second directory, then resolve it there with no compiler.
    TempTree treeA("ge_cachekey_relocate_a");
    TempTree treeB("ge_cachekey_relocate_b");

    const char* src = "#version 450\n#include \"Includes/probe_shared.glsl\"\nvoid main() { }\n";
    WriteTextFile(treeA.Root / "Shaders" / "Includes" / "probe_shared.glsl", "// shared\n");

    auto reqA = MakeInlineRequest(treeA.Root / "Cache", treeA.Root / "Shaders" / "probe.vert",
                                  "relocatable", src);
    reqA.includeDirs = {treeA.Root / "Shaders"};
    ShaderProgramCompileResult cooked{};
    COMPILE_OR_SKIP(reqA, cooked);

    std::error_code ec;
    fs::copy(treeA.Root, treeB.Root, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::remove_all(treeA.Root, ec);

    struct CompilerOff
    {
        CompilerOff() { ShaderCompileService::SetCompilerAvailable(false); }
        ~CompilerOff() { ShaderCompileService::SetCompilerAvailable(true); }
    } compilerOff;

    auto reqB = MakeInlineRequest(treeB.Root / "Cache", treeB.Root / "Shaders" / "probe.vert",
                                  "relocatable", src);
    reqB.includeDirs = {treeB.Root / "Shaders"};
    ShaderProgramCompileResult hit{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(reqB, ShaderSourceKind::SpirV, hit, &err))
        << "a cooked cache must hit after the content tree moves: " << err;
    EXPECT_EQ(hit.outputDir.filename(), cooked.outputDir.filename())
        << "the key must be identical either side of the move";
    EXPECT_FALSE(hit.stageBytes.empty());
}

TEST(ShaderCompileCacheKey, CookedEntryServesARuntimeWithNoCompiler)
{
    // The shaderc-less runtime contract: a cooked entry must be served without
    // a compiler, and a genuine miss must name the key that was expected.
    TempTree tree("ge_cachekey_cookonly");
    const fs::path cache = tree.Root / "Cache";

    ShaderProgramCompileResult cooked{};
    COMPILE_OR_SKIP(MakeInlineRequest(cache, tree.Root / "probe.vert", "cooked"), cooked);

    struct CompilerOff
    {
        CompilerOff() { ShaderCompileService::SetCompilerAvailable(false); }
        ~CompilerOff() { ShaderCompileService::SetCompilerAvailable(true); }
    } compilerOff;
    ASSERT_FALSE(ShaderCompileService::IsCompilerAvailable());

    ShaderProgramCompileResult hit{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        MakeInlineRequest(cache, tree.Root / "probe.vert", "cooked"), ShaderSourceKind::SpirV, hit, &err))
        << err;
    EXPECT_EQ(hit.outputDir, cooked.outputDir);
    EXPECT_FALSE(hit.stageBytes.empty()) << "a cook-only hit must carry the SPIR-V";

    ShaderProgramCompileResult miss{};
    std::string missErr;
    EXPECT_FALSE(ShaderCompileService::CompileProgramToCache(
        MakeInlineRequest(cache, tree.Root / "probe.vert", "uncooked",
                          "#version 450\nvoid main() { gl_Position = vec4(1.0); }\n"), ShaderSourceKind::SpirV,
        miss, &missErr));
    EXPECT_NE(missErr.find(miss.shaderPkgPath.string()), std::string::npos)
        << "a cook-only miss must name the cache entry it expected: " << missErr;
    EXPECT_NE(missErr.find("Cook"), std::string::npos)
        << "a cook-only miss must say how to fix it: " << missErr;
}

TEST(ShaderCompileCacheKey, StageStaysInKey)
{
    TempTree tree("ge_cachekey_stage");
    const fs::path cache = tree.Root / "Cache";
    const fs::path src = tree.Root / "probe.glsl";

    ShaderProgramCompileResult resVs{};
    COMPILE_OR_SKIP(MakeInlineRequest(cache, src, "probe", kMinimalGlsl, "vs"), resVs);

    ShaderProgramCompileResult resFs{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        MakeInlineRequest(cache, src, "probe", kMinimalGlsl, "fs"), ShaderSourceKind::SpirV, resFs, &err))
        << err;

    EXPECT_NE(resVs.outputDir, resFs.outputDir);
}

TEST(ShaderCompileCacheKey, DefinesStayInKey)
{
    TempTree tree("ge_cachekey_defines");
    const fs::path cache = tree.Root / "Cache";
    const fs::path src = tree.Root / "probe.vert";

    auto reqA = MakeInlineRequest(cache, src, "probe");
    reqA.stages[0].defines = {"FOO=1"};
    ShaderProgramCompileResult resA{};
    COMPILE_OR_SKIP(reqA, resA);

    auto reqB = MakeInlineRequest(cache, src, "probe");
    reqB.stages[0].defines = {"FOO=2"};
    ShaderProgramCompileResult resB{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(reqB, ShaderSourceKind::SpirV, resB, &err)) << err;

    EXPECT_NE(resA.outputDir, resB.outputDir);
}

TEST(ShaderCompileCacheKey, EntryPointStaysInKey)
{
    // HLSL is the only language where the entry point is caller-chosen (GLSL
    // is always 'main'); same source, two entries -> two cache entries.
    TempTree tree("ge_cachekey_entry");
    const fs::path cache = tree.Root / "Cache";
    const fs::path src = tree.Root / "probe.hlsl";
    const char* hlsl =
        "float4 EntryA() : SV_Position { return float4(0, 0, 0, 1); }\n"
        "float4 EntryB() : SV_Position { return float4(0, 0, 0, 1); }\n";

    auto reqA = MakeInlineRequest(cache, src, "probe", hlsl);
    reqA.stages[0].entryPoint = "EntryA";
    ShaderProgramCompileResult resA{};
    COMPILE_OR_SKIP(reqA, resA);

    auto reqB = MakeInlineRequest(cache, src, "probe", hlsl);
    reqB.stages[0].entryPoint = "EntryB";
    ShaderProgramCompileResult resB{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(reqB, ShaderSourceKind::SpirV, resB, &err)) << err;

    EXPECT_NE(resA.outputDir, resB.outputDir);
}

namespace
{
// Save/set/restore an environment variable so lane-flag tests can't leak into
// each other (or inherit ambient state from the invoking shell).
class ScopedEnvVar
{
public:
    ScopedEnvVar(const char* name, const char* value) : m_Name(name)
    {
        if (const char* old = std::getenv(name))
        {
            m_Old = old;
            m_HadOld = true;
        }
        Set(value);
    }
    ~ScopedEnvVar() { Set(m_HadOld ? m_Old.c_str() : ""); }

private:
    void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s(m_Name, value); // empty value deletes; updates CRT + Win32 env
#else
        if (value && value[0])
            setenv(m_Name, value, 1);
        else
            unsetenv(m_Name);
#endif
    }
    const char* m_Name;
    std::string m_Old;
    bool m_HadOld = false;
};

bool SlangcConfigured()
{
    const char* p = std::getenv("GE_SLANGC");
    return p && p[0];
}

// Valid Slang compute source importing a module — exercises the depfile ->
// include-hash revalidation path.
constexpr const char* kProbeModuleV1 =
    "module ge_probe_mod;\n"
    "public float GE_ProbeValue() { return 1.0; }\n";
constexpr const char* kProbeModuleV2 =
    "module ge_probe_mod;\n"
    "public float GE_ProbeValue() { return 2.0; }\n";
constexpr const char* kProbeMainSlang =
    "import ge_probe_mod;\n"
    "struct PC { uint count; }\n"
    "[[vk::push_constant]] PC pc;\n"
    "[[vk::binding(0, 0)]] RWStructuredBuffer<float> outBuf;\n"
    "[shader(\"compute\")]\n"
    "[numthreads(64, 1, 1)]\n"
    "void main(uint3 tid : SV_DispatchThreadID)\n"
    "{\n"
    "    if (tid.x >= pc.count) return;\n"
    "    outBuf[tid.x] = GE_ProbeValue();\n"
    "}\n";

} // namespace

TEST(SlangCompileLane, LaneOffRejectsSlangStageBeforeAnyCacheProbe)
{
    ScopedEnvVar lane("GE_SHADER_SLANG_LANE", "");
    TempTree tree("ge_slang_off");
    const fs::path cache = tree.Root / "Cache";

    const uint64_t before = ShaderCompileService::GetTotalCompilations();

    ShaderProgramCompileResult res{};
    std::string err;
    const bool ok = ShaderCompileService::CompileProgramToCache(
        MakeInlineRequest(cache, tree.Root / "probe.slang", "probe", kProbeMainSlang, "cs"), ShaderSourceKind::SpirV, res,
        &err);

    EXPECT_FALSE(ok) << ".slang stages must hard-error when the lane flag is off";
    EXPECT_NE(err.find("GE_SHADER_SLANG_LANE"), std::string::npos)
        << "the rejection must tell the author how to enable the lane, got: " << err;
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), before);
}

TEST(SlangCompileLane, GlslKeyIndependentOfLaneFlag)
{
    // Turning the lane ON must not perturb GLSL cache keys: the same .vert
    // request must HIT the entry a flag-off compile wrote.
    TempTree tree("ge_slang_glsl_inert");
    const fs::path cache = tree.Root / "Cache";
    const fs::path src = tree.Root / "probe.vert";

    ShaderProgramCompileResult resOff{};
    {
        ScopedEnvVar lane("GE_SHADER_SLANG_LANE", "");
        COMPILE_OR_SKIP(MakeInlineRequest(cache, src, "probe"), resOff);
    }
    const uint64_t afterFirst = ShaderCompileService::GetTotalCompilations();

    ShaderProgramCompileResult resOn{};
    {
        ScopedEnvVar lane("GE_SHADER_SLANG_LANE", "1");
        std::string err;
        ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
            MakeInlineRequest(cache, src, "probe"), ShaderSourceKind::SpirV, resOn, &err))
            << err;
    }

    EXPECT_EQ(resOff.outputDir, resOn.outputDir)
        << "the lane flag must not enter glsl/hlsl cache keys";
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst)
        << "flag-on GLSL request must HIT the flag-off cache entry";
}

TEST(SlangCompileLane, SlangKeyNeverAliasesGlslKey)
{
    // The no-alias guarantee behind the lane discriminator: byte-identical
    // source under identical dirs/stage must key differently per language.
    // kMinimalGlsl happens to be accepted by BOTH compilers, so a missing
    // discriminator would make the .slang request silently HIT (and serve)
    // the glslang-compiled entry.
    if (!SlangcConfigured())
        GTEST_SKIP() << "GE_SLANGC not set — external slangc unavailable";
    ScopedEnvVar lane("GE_SHADER_SLANG_LANE", "1");

    TempTree tree("ge_slang_noalias");
    const fs::path cache = tree.Root / "Cache";

    ShaderProgramCompileResult resGlsl{};
    COMPILE_OR_SKIP(MakeInlineRequest(cache, tree.Root / "probe.vert", "probe"), resGlsl);

    const uint64_t afterGlsl = ShaderCompileService::GetTotalCompilations();

    ShaderProgramCompileResult resSlang{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        MakeInlineRequest(cache, tree.Root / "probe.slang", "probe", kMinimalGlsl, "vs"), ShaderSourceKind::SpirV,
        resSlang, &err))
        << err;

    EXPECT_NE(resGlsl.outputDir, resSlang.outputDir)
        << "identical bytes must never share a cache entry across compiler lanes";
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterGlsl + 1)
        << "the .slang request must be a real slangc compile, not a cross-lane cache HIT";
    EXPECT_FALSE(resSlang.stageBytes.at("vs").empty());
}

TEST(SlangCompileLane, CompilesEndToEndAndRevalidatesModuleImports)
{
    if (!SlangcConfigured())
        GTEST_SKIP() << "GE_SLANGC not set — external slangc unavailable";
    ScopedEnvVar lane("GE_SHADER_SLANG_LANE", "1");

    TempTree tree("ge_slang_e2e");
    const fs::path cache = tree.Root / "Cache";
    const fs::path modPath = tree.Root / "ge_probe_mod.slang";
    const fs::path mainPath = tree.Root / "probe_main.slang";
    WriteTextFile(modPath, kProbeModuleV1);
    WriteTextFile(mainPath, kProbeMainSlang);

    // File-based (no inlineSource): the compile reads the staged-style layout.
    ShaderProgramCompileRequest req{};
    req.debugName = "slang_e2e";
    req.baseDirectory = tree.Root;
    req.cacheRoot = cache;
    ShaderStageCompileSpec spec{};
    spec.stage = "cs";
    spec.sourcePath = mainPath;
    req.stages.push_back(std::move(spec));

    ShaderProgramCompileResult res{};
    COMPILE_OR_SKIP(req, res);
    ASSERT_FALSE(res.stageBytes.at("cs").empty());
    EXPECT_FALSE(res.meta.Sets.empty()) << "reflection must see the storage buffer";
    ASSERT_TRUE(res.cacheInfoJson.has_value());
    EXPECT_NE(res.cacheInfoJson->find("ge_probe_mod"), std::string::npos)
        << "imported module must be recorded for revalidation, got: " << *res.cacheInfoJson;

    const uint64_t afterFirst = ShaderCompileService::GetTotalCompilations();

    // Unchanged inputs: HIT.
    ShaderProgramCompileResult resHit{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, resHit, &err)) << err;
    EXPECT_EQ(resHit.outputDir, res.outputDir);
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst);

    // Module edit: same key (root source unchanged), but the include-hash
    // revalidation must flip the entry stale and recompile.
    WriteTextFile(modPath, kProbeModuleV2);
    std::error_code ec;
    fs::last_write_time(modPath, fs::file_time_type::clock::now() + std::chrono::seconds(2), ec);

    ShaderProgramCompileResult resStale{};
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, resStale, &err)) << err;
    EXPECT_EQ(resStale.outputDir, res.outputDir) << "module edits must not change the key";
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst + 1)
        << "an imported-module edit must force a recompile, not serve stale SPIR-V";
}

TEST(ShaderCompileCacheKey, HundredIdenticalMaterialsCompileOnce)
{
    // The motivating regression: 100 materials with byte-identical composed
    // source, differing only in debugName and composed file name (shared
    // directory — the MakeGeneratedAdapterPath shape), must cost ONE compile.
    TempTree tree("ge_cachekey_hundred");
    const fs::path cache = tree.Root / "Cache";
    const fs::path composedDir = tree.Root / "Generated" / "Materials";

    ShaderProgramCompileResult first{};
    COMPILE_OR_SKIP(
        MakeInlineRequest(cache, composedDir / "material_000_composed.vert", "material_000"),
        first);

    const uint64_t afterFirst = ShaderCompileService::GetTotalCompilations();

    for (int i = 1; i < 100; ++i)
    {
        const std::string name = "material_" + std::to_string(i);
        ShaderProgramCompileResult res{};
        std::string err;
        ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
            MakeInlineRequest(cache, composedDir / (name + "_composed.vert"), name), ShaderSourceKind::SpirV, res, &err))
            << err;
        ASSERT_EQ(res.outputDir, first.outputDir) << "request " << i;
        ASSERT_FALSE(res.stageBytes.at("vs").empty()) << "request " << i;
    }

    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst)
        << "99 identical follow-up requests must all be cache HITs";
}

// --- Include CONTENT in the key -------------------------------------------
//
// A shader's #include closure is as much a compile input as its top-level bytes:
// a material reaches its surface shader through a substituted-literal
// `#include "<surface>"`, and every engine adapter reaches
// standard_pbr/clustered_lighting/... the same way. Keying only the top-level
// bytes left those edits to a staleness check whose `includeMtime <=
// packageMtime` shortcut mtime-preserving tooling defeats — "save, nothing
// changes on screen", and the hand-wipe-.Cache/Shaders-after-adapter-edits
// discipline that grew around it.
//
// These tests deliberately INVERT the mtime-push discipline the rest of the
// suite uses: they restore the original mtime and keep the byte LENGTH identical,
// so nothing but content differs. Anything that passes is keying on content.

namespace
{
// Two same-length surface bodies whose SPIR-V differs, so a stale serve shows up
// in the compiled BYTES and not merely in a counter.
constexpr const char* kProbeSurfaceV1 = "float GE_Probe() { return 1.0; }\n";
constexpr const char* kProbeSurfaceV2 = "float GE_Probe() { return 2.0; }\n";
static_assert(sizeof(kProbeSurfaceV1) == sizeof(kProbeSurfaceV2),
              "the point of these tests is a SAME-LENGTH rewrite");

constexpr const char* kProbeFragUsingSurface =
    "#version 450\n"
    "#include \"probe_surface.glsl\"\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main() { oColor = vec4(GE_Probe()); }\n";

// Same-length bodies for the transitively-included helper.
constexpr const char* kProbeBrdfV1 = "float GE_ProbeBrdf() { return 1.0; }\n";
constexpr const char* kProbeBrdfV2 = "float GE_ProbeBrdf() { return 2.0; }\n";
static_assert(sizeof(kProbeBrdfV1) == sizeof(kProbeBrdfV2), "same-length rewrite");

// Overwrite with identical byte length and restore the original mtime — the
// shape mtime-preserving tooling produces (robocopy /COPY:DAT, timestamp-
// restoring syncs) and the shape a same-tick save produces. After the rewrite the
// file is INDISTINGUISHABLE from the original by size or timestamp; only its
// content differs.
void RewritePreservingMtimeAndLength(const fs::path& p, const char* newText)
{
    std::error_code ec;
    const auto originalMtime = fs::last_write_time(p, ec);
    ASSERT_FALSE(ec) << "could not read mtime of " << p.string();
    const auto originalSize = fs::file_size(p, ec);
    ASSERT_FALSE(ec) << "could not read size of " << p.string();

    WriteTextFile(p, newText);

    ASSERT_EQ(fs::file_size(p, ec), originalSize)
        << "the rewrite must not change the byte length — that is the whole point";
    fs::last_write_time(p, originalMtime, ec);
    ASSERT_FALSE(ec) << "could not restore mtime of " << p.string();
    ASSERT_EQ(fs::last_write_time(p, ec), originalMtime) << "mtime restore did not stick";
}
} // namespace

TEST(ShaderCompileCacheKey, IncludedSurfaceContentEntersKeyDespitePreservedMtime)
{
    TempTree tree("ge_cachekey_surface_content");
    const fs::path cache = tree.Root / "Cache";
    const fs::path src = tree.Root / "probe.frag";
    const fs::path surface = tree.Root / "probe_surface.glsl";
    WriteTextFile(surface, kProbeSurfaceV1);

    ShaderProgramCompileResult resV1{};
    COMPILE_OR_SKIP(MakeInlineRequest(cache, src, "probe", kProbeFragUsingSurface, "fs"), resV1);
    ASSERT_FALSE(resV1.stageBytes.at("fs").empty());

    const uint64_t afterFirst = ShaderCompileService::GetTotalCompilations();

    RewritePreservingMtimeAndLength(surface, kProbeSurfaceV2);

    ShaderProgramCompileResult resV2{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        MakeInlineRequest(cache, src, "probe", kProbeFragUsingSurface, "fs"), ShaderSourceKind::SpirV, resV2, &err))
        << err;

    EXPECT_NE(resV2.outputDir, resV1.outputDir)
        << "the surface's CONTENT is a compile input: an edit must move the key";
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst + 1)
        << "an unchanged mtime is not evidence about content — this must recompile";
    EXPECT_NE(resV2.stageBytes.at("fs"), resV1.stageBytes.at("fs"))
        << "the POST-edit surface must be what got compiled, not the pre-edit bytes";
    EXPECT_TRUE(fs::exists(resV1.shaderPkgPath))
        << "the superseded entry is orphaned at its own key, not deleted or overwritten";
}

TEST(ShaderCompileCacheKey, TransitiveAdapterIncludeContentEntersKey)
{
    // The composed-adapter shape the material pipeline actually uses: the
    // top-level source is IN-MEMORY (its logical path names a Generated/ dir that
    // never exists on disk) and reaches engine helpers through `../Includes/...`
    // resolved out of includeDirs — exactly how adapter_forward.glsl reaches
    // clustered_lighting.glsl, which reaches standard_pbr.glsl. An edit two
    // levels down is what used to require hand-wiping .Cache/Shaders.
    TempTree tree("ge_cachekey_transitive_include");
    const fs::path cache = tree.Root / "Cache";
    const fs::path shaderRoot = tree.Root / "Shaders";
    const fs::path adaptersDir = shaderRoot / "Adapters";
    const fs::path includesDir = shaderRoot / "Includes";
    const fs::path brdf = includesDir / "probe_pbr.glsl";

    fs::create_directories(adaptersDir);
    WriteTextFile(includesDir / "probe_lighting.glsl",
                  "#include \"probe_pbr.glsl\"\n"
                  "float GE_ProbeLight() { return GE_ProbeBrdf(); }\n");
    WriteTextFile(brdf, kProbeBrdfV1);

    constexpr const char* kComposedFrag =
        "#version 450\n"
        "#include \"../Includes/probe_lighting.glsl\"\n"
        "layout(location = 0) out vec4 oColor;\n"
        "void main() { oColor = vec4(GE_ProbeLight()); }\n";

    // MaterialBuildService's include-dir order: the shader root, then Adapters,
    // then Includes. `../Includes/probe_lighting.glsl` only resolves against the
    // Adapters root, so this also pins the scan to the includer's real search
    // order rather than a normalized guess.
    auto makeRequest = [&]() {
        auto req = MakeInlineRequest(cache, cache / "Generated" / "Materials" / "probe_composed.frag",
                                     "probe", kComposedFrag, "fs");
        req.includeDirs = {shaderRoot, adaptersDir, includesDir};
        return req;
    };

    ShaderProgramCompileResult resV1{};
    COMPILE_OR_SKIP(makeRequest(), resV1);
    ASSERT_FALSE(resV1.stageBytes.at("fs").empty());

    const uint64_t afterFirst = ShaderCompileService::GetTotalCompilations();

    RewritePreservingMtimeAndLength(brdf, kProbeBrdfV2);

    ShaderProgramCompileResult resV2{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(makeRequest(), ShaderSourceKind::SpirV, resV2, &err)) << err;

    EXPECT_NE(resV2.outputDir, resV1.outputDir)
        << "a transitively-included engine helper is a compile input at any depth";
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst + 1)
        << "editing an adapter's nested include must recompile without a cache wipe";
    EXPECT_NE(resV2.stageBytes.at("fs"), resV1.stageBytes.at("fs"))
        << "the POST-edit helper must be what got compiled";
}

TEST(ShaderCompileCacheKey, UnchangedIncludeClosureStaysACacheHit)
{
    // No always-recompile regression. Content-keying reads the whole closure on
    // every key build, so the guarantee that matters is that an UNCHANGED closure
    // still hits. Sized like the real adapter_forward chain (~18 files, ~130KB
    // total) and looped so the printed figure is the per-hit key-build cost this
    // scheme adds to every cache hit.
    TempTree tree("ge_cachekey_closure_hit");
    const fs::path cache = tree.Root / "Cache";
    const fs::path includesDir = tree.Root / "Includes";

    constexpr int kChainLength = 18;
    constexpr int kPadLines = 120; // ~7KB/file at ~58 bytes/line, like the real includes
    for (int i = 0; i < kChainLength; ++i)
    {
        std::string text;
        if (i + 1 < kChainLength)
            text += "#include \"probe_chain_" + std::to_string(i + 1) + ".glsl\"\n";
        for (int k = 0; k < kPadLines; ++k)
            text += "// realistic comment padding so the hashed byte count matches\n";
        text += "float GE_Chain" + std::to_string(i) + "(float x) { return x + 1.0; }\n";
        WriteTextFile(includesDir / ("probe_chain_" + std::to_string(i) + ".glsl"), text.c_str());
    }

    const std::string composed =
        "#version 450\n"
        "#include \"probe_chain_0.glsl\"\n"
        "layout(location = 0) out vec4 oColor;\n"
        "void main() { oColor = vec4(GE_Chain0(0.0)); }\n";

    auto makeRequest = [&]() {
        auto req = MakeInlineRequest(cache, tree.Root / "probe.frag", "probe", composed.c_str(), "fs");
        req.includeDirs = {includesDir};
        return req;
    };

    ShaderProgramCompileResult first{};
    COMPILE_OR_SKIP(makeRequest(), first);
    const uint64_t afterFirst = ShaderCompileService::GetTotalCompilations();

    constexpr int kHits = 100;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kHits; ++i)
    {
        ShaderProgramCompileResult hit{};
        std::string err;
        ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(makeRequest(), ShaderSourceKind::SpirV, hit, &err)) << err;
        ASSERT_EQ(hit.outputDir, first.outputDir) << "hit " << i;
        ASSERT_FALSE(hit.stageBytes.at("fs").empty()) << "hit " << i;
    }
    const double totalMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst)
        << "an unchanged include closure must stay a cache HIT, not recompile";

    std::cout << "[warm-path] " << kHits << " cache hits over a " << kChainLength
              << "-file include closure: " << totalMs << " ms total, " << (totalMs / kHits)
              << " ms/hit" << std::endl;
}

TEST(ShaderCompileCacheKey, ForeignKeyedEntryIsIgnoredNotServed)
{
    // The key-format bump orphans every pre-change entry rather than migrating
    // it: an entry the current key does not name must be neither served nor a
    // source of error, and must be left alone on disk.
    TempTree tree("ge_cachekey_foreign_entry");
    const fs::path cache = tree.Root / "Cache";
    const fs::path foreign = cache / "0123456789abcdef";
    fs::create_directories(foreign);
    WriteTextFile(foreign / "program.shaderpkg", "not a shader package at all");

    const uint64_t before = ShaderCompileService::GetTotalCompilations();

    ShaderProgramCompileResult res{};
    COMPILE_OR_SKIP(MakeInlineRequest(cache, tree.Root / "probe.vert", "probe"), res);

    EXPECT_NE(res.outputDir, foreign) << "a foreign-keyed entry must never be addressed";
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), before + 1)
        << "the request must compile its own entry";
    EXPECT_FALSE(res.stageBytes.at("vs").empty());
    EXPECT_TRUE(fs::exists(foreign / "program.shaderpkg"))
        << "orphaned entries are ignored, not swept — no migration or deletion code";
}

TEST(ShaderCompileCacheKey, UnparseablePackageAtCurrentKeyRecompilesCleanly)
{
    // A package that cannot be loaded (truncated write, format change) must fall
    // through to a recompile rather than failing the request.
    TempTree tree("ge_cachekey_corrupt_pkg");
    const fs::path cache = tree.Root / "Cache";
    const fs::path src = tree.Root / "probe.vert";

    ShaderProgramCompileResult first{};
    COMPILE_OR_SKIP(MakeInlineRequest(cache, src, "probe"), first);
    const uint64_t afterFirst = ShaderCompileService::GetTotalCompilations();

    WriteTextFile(first.shaderPkgPath, "truncated garbage");

    ShaderProgramCompileResult again{};
    std::string err;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        MakeInlineRequest(cache, src, "probe"), ShaderSourceKind::SpirV, again, &err))
        << err;

    EXPECT_EQ(again.outputDir, first.outputDir) << "the key is unchanged; only the artifact was";
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), afterFirst + 1)
        << "an unloadable package must recompile, not fail the request";
    EXPECT_FALSE(again.stageBytes.at("vs").empty());
}
