#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>

#include "Rendering/Materials/ShaderMeta.h"

namespace GameEngine { namespace Rendering {

enum class ShaderSourceKind : uint8_t;

// Inputs for compiling a multi-stage shader \"program\" (typically from a .shader asset).
struct ShaderStageCompileSpec
{
    // Stage key: \"vs\",\"fs\",\"cs\",\"gs\",\"ms\" (mesh may be unsupported by shaderc in v1).
    std::string stage;
    std::filesystem::path sourcePath;  // source file path (logical name even when inlineSource is set)
    std::string entryPoint = "main";   // meaningful for HLSL
    std::vector<std::string> defines;  // preprocessor defines
    // When non-empty, the stage compiles from this text directly and sourcePath is
    // used only as the logical name (diagnostics + #include search base) — the file is
    // NOT read. Generated material shaders pass their composed source here instead of
    // round-tripping through a shared on-disk file, which races when the same material is
    // built concurrently (a torn read feeds glslang truncated GLSL -> heap corruption).
    std::string inlineSource;
};

struct ShaderProgramCompileRequest
{
    std::string debugName;                 // diagnostics/log output only — never part of the cache key
    std::filesystem::path baseDirectory;   // base for resolving relative source paths
    std::filesystem::path cacheRoot;       // <workspace>/.Cache/Shaders
    std::vector<std::filesystem::path> includeDirs;
    std::vector<ShaderStageCompileSpec> stages;
};

struct ShaderProgramCompileResult
{
    std::filesystem::path outputDir;       // cache directory for this build
    std::filesystem::path shaderPkgPath;   // emitted .shaderpkg path
    ShaderMeta meta{};
    // stage -> cooked bytes in the ShaderSourceKind the compile was asked for
    // (see CompileProgramToCache).
    std::unordered_map<std::string, std::vector<uint8_t>> stageBytes;
    std::optional<std::string> cacheInfoJson; // optional JSON chunk

    // Resolved absolute (lexically normal) paths of every file this program
    // depends on beyond the top-level stage sources: the textual #include
    // closure the v6 key scanner walks (deduped, scan order), plus any
    // recorded-but-not-scannable dependencies (Slang module imports, macro-form
    // includes) from the compile's include record / a hit's cache-info.
    // Populated on cache hits, on fresh compiles, AND on compile failures that
    // got past key construction: a failed build still names every file it
    // RESOLVED, so editing one of them re-triggers the compile instead of
    // leaving the material stuck on its memoized failure.
    //
    // Scope: resolved files only. An #include operand found in no search dir is
    // recorded nowhere — the closure scan skips it, and the includer logs an
    // error and substitutes empty content without recording it — so creating
    // that missing file re-triggers nothing.
    std::vector<std::string> includeClosurePaths;
};

class ShaderCompileService
{
public:
    // Resolve a shader program to its cached .shaderpkg, compiling it if the
    // cache misses and this runtime can compile. The cache key is derived from
    // compile inputs only (source bytes, defines, entry point, stage, and the
    // CONTENT of the #include closure) and carries nothing machine-specific, so
    // a cache cooked on one machine addresses identically wherever the content
    // tree lands. Returns false and sets outError on failure — including the
    // cook-only case, where a genuine miss names the key that was expected.
    //
    // `kind` is the form the consuming device ingests
    // (IDevice::PreferredShaderSource) and decides what `out.stageBytes`
    // carries. A cache hit serves the package's WGSL chunk for every stage that
    // has one; a stage without one, and every stage of a fresh compile (the
    // compiler emits SPIR-V and nothing else), stays SPIR-V — an un-cooked
    // program then fails at module creation with the backend's own "SPIR-V
    // ingestion is absent" error instead of silently losing its stages.
    static bool CompileProgramToCache(const ShaderProgramCompileRequest& req,
                                      ShaderSourceKind kind,
                                      ShaderProgramCompileResult& out,
                                      std::string* outError = nullptr);

    // Whether this process may compile shader source. False on builds without a
    // shader compiler (wasm), where every program must come from a pre-cooked
    // cache. Turning it off on a build that HAS one reproduces that runtime
    // exactly — the offline cook's own verification pass, and how the desktop
    // suite proves cooked variants stand alone. A shaderc-less build ignores
    // an attempt to turn it on: there is nothing to turn on.
    static bool IsCompilerAvailable();
    static void SetCompilerAvailable(bool available);

    // Total number of successful compilations performed in this process. Used by
    // the Monitors panel to chart shader/pipeline build pressure.
    static uint64_t GetTotalCompilations();

    // True when the GE_SHADER_SLANG_LANE dev flag is set: .slang stage sources
    // compile via an external slangc (GE_SLANGC) instead of being rejected.
    // Spike-grade seam: dev-only, and the slangc version and flag set it is known to
    // behave with are recorded in SlangCompileLane.h.
    static bool IsSlangLaneEnabled();

    // Highest SPIR-V version the active rendering backend can consume. A backend-
    // agnostic capability: each backend maps its own support to a SPIR-V version
    // (Vulkan from its API version, Metal from its SPIRV-Cross input support) and
    // publishes it once at device init. The service emits every stage at this
    // version, raised to any per-stage compile floor (mesh shaders require 1.6).
    // Defaults to Spirv_1_5 (loads on the widest range of drivers) until set.
    enum class SpirvTarget { Spirv_1_5, Spirv_1_6 };
    static void SetMaxSupportedSpirv(SpirvTarget target);
    // The target feeds the program cache KEY, so anything that needs to compile at a
    // specific one (a test asserting a cooked cache addresses identically) has to be able
    // to set it and put back what it found — a device published its own value here and
    // every later compile in the process inherits it.
    static SpirvTarget MaxSupportedSpirv();
};

}} // namespace GameEngine::Rendering

