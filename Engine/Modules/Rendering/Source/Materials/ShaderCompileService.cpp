#include "Rendering/Materials/ShaderCompileService.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Common/Utils.h"
#include "Types/StringUtils.h"
#include "Rendering/Materials/ShaderMetaValidation.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "ShaderIncludeClosure.h"
#include "ShaderSourceCache.h"
#include "SlangCompileLane.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>

#if defined(RENDERING_HAS_SHADERC) && RENDERING_HAS_SHADERC
#include <shaderc/shaderc.hpp>
#endif

namespace GameEngine { namespace Rendering {

namespace {
static std::atomic<uint64_t> g_TotalCompilations{0};
// Highest SPIR-V version the active backend can consume (see SetMaxSupportedSpirv).
// Defaults to 1.5 so that, absent a backend, shaders compile to the widely-loadable
// target.
static std::atomic<ShaderCompileService::SpirvTarget> g_MaxBackendSpirv{
    ShaderCompileService::SpirvTarget::Spirv_1_5};
// Whether this process may compile shader source. A shaderc-less build can
// never turn it on; a build that has shaderc may turn it OFF to run exactly as
// a cook-only runtime does (see SetCompilerAvailable).
static std::atomic<bool> g_CompilerAvailable{
#if defined(RENDERING_HAS_SHADERC) && RENDERING_HAS_SHADERC
    true
#else
    false
#endif
};
} // namespace

bool ShaderCompileService::IsCompilerAvailable()
{
    return g_CompilerAvailable.load(std::memory_order_relaxed);
}

void ShaderCompileService::SetCompilerAvailable(bool available)
{
#if defined(RENDERING_HAS_SHADERC) && RENDERING_HAS_SHADERC
    g_CompilerAvailable.store(available, std::memory_order_relaxed);
#else
    (void)available;
#endif
}

uint64_t ShaderCompileService::GetTotalCompilations()
{
    return g_TotalCompilations.load(std::memory_order_relaxed);
}

bool ShaderCompileService::IsSlangLaneEnabled()
{
    return SlangLane::Enabled();
}

void ShaderCompileService::SetMaxSupportedSpirv(SpirvTarget target)
{
    g_MaxBackendSpirv.store(target, std::memory_order_relaxed);
}

ShaderCompileService::SpirvTarget ShaderCompileService::MaxSupportedSpirv()
{
    return g_MaxBackendSpirv.load(std::memory_order_relaxed);
}

namespace {

// Resolve a stage's source text: the in-memory inlineSource when present, else read srcPath.
static bool ResolveStageSource(const ShaderStageCompileSpec& spec,
                               const std::filesystem::path& srcPath,
                               std::string& out, std::string* outError)
{
    if (!spec.inlineSource.empty())
    {
        out = spec.inlineSource;
        return true;
    }
    return ReadShaderFileText(srcPath, out, outError);
}

static std::string Hex64(uint64_t v)
{
    std::ostringstream oss;
    oss << std::hex;
    oss.width(16);
    oss.fill('0');
    oss << v;
    return oss.str();
}

static bool IsHlslPath(const std::filesystem::path& p)
{
    std::string ext = ToLowerAscii(p.extension().string());
    return ext == ".hlsl";
}

static bool IsGlslPath(const std::filesystem::path& p)
{
    std::string ext = ToLowerAscii(p.extension().string());
    return ext == ".glsl" || ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".geom";
}

// Dev-flag Slang lane (see SlangCompileLane.h): .slang stages compile via an
// external slangc and are rejected loudly unless GE_SHADER_SLANG_LANE is set.
static bool IsSlangPath(const std::filesystem::path& p)
{
    std::string ext = ToLowerAscii(p.extension().string());
    return ext == ".slang";
}

static bool MapStage(const std::string& stageLower, ShaderStageKind& outStage, std::string& outErr)
{
    if (stageLower == "vs") { outStage = ShaderStageKind::Vertex; return true; }
    if (stageLower == "fs") { outStage = ShaderStageKind::Fragment; return true; }
    if (stageLower == "cs") { outStage = ShaderStageKind::Compute; return true; }
    if (stageLower == "gs") { outStage = ShaderStageKind::Geometry; return true; }
    if (stageLower == "ms") { outStage = ShaderStageKind::Mesh; return true; }
    outErr = "Unsupported stage key: " + stageLower;
    return false;
}

#if defined(RENDERING_HAS_SHADERC) && RENDERING_HAS_SHADERC

class FileIncluder final : public shaderc::CompileOptions::IncluderInterface
{
public:
    explicit FileIncluder(std::vector<std::filesystem::path> dirs,
                          std::unordered_map<std::string, uint64_t>* outIncludeHashes)
        : m_Dirs(std::move(dirs))
        , m_OutIncludeHashes(outIncludeHashes)
    {
    }

    shaderc_include_result* GetInclude(const char* requested_source,
                                       shaderc_include_type /*type*/,
                                       const char* requesting_source,
                                       size_t /*include_depth*/) override
    {
        auto* result = new shaderc_include_result();
        std::filesystem::path req(requested_source ? requested_source : "");

        std::vector<std::filesystem::path> search;
        if (requesting_source && requesting_source[0])
        {
            std::filesystem::path rs(requesting_source);
            if (!rs.empty())
                search.push_back(rs.parent_path());
        }
        for (const auto& d : m_Dirs)
            search.push_back(d);

        std::error_code ec;
        for (const auto& dir : search)
        {
            std::filesystem::path cand = dir / req;
            if (std::filesystem::exists(cand, ec))
            {
                std::ifstream in(cand, std::ios::binary);
                std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

                // Record include dependency hash (resolved absolute path).
                if (m_OutIncludeHashes)
                {
                    std::error_code ec2;
                    std::filesystem::path abs = std::filesystem::absolute(cand, ec2);
                    if (ec2)
                        abs = cand;
                    const std::string key = abs.lexically_normal().string();
                    if (m_OutIncludeHashes->find(key) == m_OutIncludeHashes->end())
                    {
                        const uint64_t h = HashShaderBytes(contents.data(), contents.size());
                        (*m_OutIncludeHashes)[key] = h;
                    }
                }

                m_OwnedStrings.emplace_back(cand.string());
                m_OwnedStrings.emplace_back(std::move(contents));

                const std::string& resolvedName = m_OwnedStrings[m_OwnedStrings.size() - 2];
                const std::string& resolvedText = m_OwnedStrings[m_OwnedStrings.size() - 1];

                result->source_name = resolvedName.c_str();
                result->source_name_length = resolvedName.size();
                result->content = resolvedText.c_str();
                result->content_length = resolvedText.size();
                result->user_data = nullptr;
                return result;
            }
        }

        // Not found. shaderc treats a result with empty content as a SUCCESSFUL
        // (empty) include, not an error — so an unresolved #include silently drops
        // the file's contents (e.g. a material's surface shader), leaving whatever
        // the file was meant to define undefined and producing wrong output with no
        // diagnostic. That silent path cost a full CBT-C3 bisection session. Make it
        // LOUD: any include the searcher can't find is almost always a real bug
        // (bad path, missing staging), and if it were genuinely optional the author
        // should guard it, not lean on empty content.
        {
            std::string searched;
            for (const auto& dir : search)
            {
                if (!searched.empty())
                    searched += ", ";
                searched += (dir / req).generic_string();
            }
            Logger::Log::Error("ShaderCompileService: #include \"{}\" (from '{}') NOT FOUND — shaderc "
                               "will substitute EMPTY content. Searched: [{}]",
                               requested_source ? requested_source : "",
                               requesting_source ? requesting_source : "<root>", searched);
        }
        m_OwnedStrings.emplace_back(std::string(requested_source ? requested_source : ""));
        m_OwnedStrings.emplace_back(std::string());
        const std::string& nm = m_OwnedStrings[m_OwnedStrings.size() - 2];
        const std::string& txt = m_OwnedStrings[m_OwnedStrings.size() - 1];
        result->source_name = nm.c_str();
        result->source_name_length = nm.size();
        result->content = txt.c_str();
        result->content_length = 0;
        result->user_data = nullptr;
        return result;
    }

    void ReleaseInclude(shaderc_include_result* data) override
    {
        delete data;
    }

private:
    std::vector<std::filesystem::path> m_Dirs;
    std::unordered_map<std::string, uint64_t>* m_OutIncludeHashes = nullptr;
    // Keeps the strings whose c_str() we hand to shaderc alive for the whole compile.
    // MUST be std::deque (not std::vector): shaderc holds those pointers across later
    // GetInclude calls (nested includes), and std::vector reallocation on a subsequent
    // emplace_back would invalidate them -> use-after-free / heap corruption in glslang's
    // SPIR-V generation. std::deque::push_back never invalidates references to existing
    // elements, so the pointers stay valid until the includer is destroyed.
    std::deque<std::string> m_OwnedStrings;
};

static shaderc_shader_kind ToShadercKind(ShaderStageKind k)
{
    switch (k)
    {
    case ShaderStageKind::Vertex:   return shaderc_vertex_shader;
    case ShaderStageKind::Fragment: return shaderc_fragment_shader;
    case ShaderStageKind::Compute:  return shaderc_compute_shader;
    case ShaderStageKind::Geometry: return shaderc_geometry_shader;
    case ShaderStageKind::Mesh:     return shaderc_mesh_shader;
    default:                        return shaderc_glsl_infer_from_source;
    }
}

// Per-stage compile floor: GL_EXT_mesh_shader can only be compiled at SPIR-V 1.6;
// other stages have no floor (1.5 compiles fine and loads on the widest range of
// drivers). This is a property of the shader/toolchain, not of any backend.
static ShaderCompileService::SpirvTarget StageMinSpirv(ShaderStageKind k)
{
    return k == ShaderStageKind::Mesh ? ShaderCompileService::SpirvTarget::Spirv_1_6
                                      : ShaderCompileService::SpirvTarget::Spirv_1_5;
}

// Emit at the backend's max SPIR-V version, raised to the stage's compile floor.
static ShaderCompileService::SpirvTarget ChooseSpirv(ShaderStageKind k)
{
    const ShaderCompileService::SpirvTarget backendMax =
        g_MaxBackendSpirv.load(std::memory_order_relaxed);
    const ShaderCompileService::SpirvTarget floor = StageMinSpirv(k);
    return static_cast<int>(floor) > static_cast<int>(backendMax) ? floor : backendMax;
}

static bool CompileOneStage(const ShaderStageCompileSpec& spec,
                            const ShaderProgramCompileRequest& req,
                            ShaderStageKind stageKind,
                            std::vector<uint32_t>& outWords,
                            std::string& outDiagnostics,
                            std::unordered_map<std::string, uint64_t>* outIncludeHashes)
{
    const std::filesystem::path srcPath = spec.sourcePath.is_absolute() ? spec.sourcePath : (req.baseDirectory / spec.sourcePath);
    std::string sourceText;
    std::string readErr;
    if (!ResolveStageSource(spec, srcPath, sourceText, &readErr))
    {
        outDiagnostics = readErr;
        return false;
    }

    const bool isHlsl = IsHlslPath(srcPath);
    const bool isGlsl = IsGlslPath(srcPath);
    const bool isSlang = IsSlangPath(srcPath);
    if (!isHlsl && !isGlsl && !isSlang)
    {
        outDiagnostics = "Unknown shader source language for: " + srcPath.string();
        return false;
    }

    // GLSL/Slang entry point must be main (the backend binds pName "main";
    // the Slang lane also passes -entry main).
    if (isGlsl || isSlang)
    {
        if (!spec.entryPoint.empty() && ToLowerAscii(spec.entryPoint) != "main")
        {
            outDiagnostics = std::string(isSlang ? "Slang" : "GLSL") +
                             " entry point must be 'main' (got '" + spec.entryPoint + "')";
            return false;
        }
    }

    if (isSlang)
    {
        SlangLane::StageInput si{};
        si.sourcePath = srcPath;
        si.inlineSource = spec.inlineSource;
        si.stageKey = ToLowerAscii(spec.stage);
        si.defines = spec.defines;
        // Same effective search order as the GLSL FileIncluder: the source's
        // own directory (native for file compiles, appended for inline ones),
        // then req.includeDirs.
        si.includeDirs = req.includeDirs;
        si.includeDirs.push_back(srcPath.parent_path());
        si.targetSpirv16 = ChooseSpirv(stageKind) == ShaderCompileService::SpirvTarget::Spirv_1_6;
        si.scratchDir = req.cacheRoot / "SlangScratch";

        std::vector<std::string> moduleDeps;
        std::string diag;
        if (!SlangLane::CompileStage(si, outWords, diag, moduleDeps))
        {
            outDiagnostics =
                "Source: " + srcPath.string() + "\n" +
                "Stage: " + spec.stage + "\n" +
                "slang lane:\n" + diag;
            return false;
        }
        // Imported modules feed the same include-hash revalidation the GLSL
        // includer populates: a module edit flips the cached entry stale.
        if (outIncludeHashes)
        {
            for (const auto& dep : moduleDeps)
            {
                if (outIncludeHashes->find(dep) != outIncludeHashes->end())
                    continue;
                std::string bytes;
                std::string depErr;
                if (!ReadShaderFileText(dep, bytes, &depErr))
                {
                    outDiagnostics = "SlangLane: failed hashing module dependency '" + dep +
                                     "': " + depErr;
                    return false;
                }
                (*outIncludeHashes)[dep] = HashShaderBytes(bytes.data(), bytes.size());
            }
        }
        return true;
    }

    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    // shaderc needs a target env paired with the SPIR-V version; that pairing is
    // internal toolchain plumbing. The emitted SPIR-V is either consumed directly
    // (Vulkan) or cross-compiled to MSL (Metal) by the backend.
    if (ChooseSpirv(stageKind) == ShaderCompileService::SpirvTarget::Spirv_1_6)
    {
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        options.SetTargetSpirv(shaderc_spirv_version_1_6);
    }
    else
    {
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        options.SetTargetSpirv(shaderc_spirv_version_1_5);
    }
    options.SetOptimizationLevel(shaderc_optimization_level_zero);

    if (isHlsl)
    {
        options.SetSourceLanguage(shaderc_source_language_hlsl);
    }
    else
    {
        options.SetSourceLanguage(shaderc_source_language_glsl);
    }

    for (const auto& d : spec.defines)
    {
        if (!d.empty())
            options.AddMacroDefinition(d);
    }

    // Include paths
    std::vector<std::filesystem::path> includeDirs = req.includeDirs;
    includeDirs.push_back(srcPath.parent_path());
    options.SetIncluder(std::make_unique<FileIncluder>(includeDirs, outIncludeHashes));

    const shaderc_shader_kind sk = ToShadercKind(stageKind);
    if (stageKind == ShaderStageKind::Mesh || sk == shaderc_glsl_infer_from_source)
    {
        outDiagnostics = "Stage not supported by shaderc in this build: " + spec.stage;
        return false;
    }

    shaderc::SpvCompilationResult result =
        compiler.CompileGlslToSpv(sourceText, sk, srcPath.string().c_str(),
                                  isHlsl ? spec.entryPoint.c_str() : "main",
                                  options);

    outDiagnostics = result.GetErrorMessage();
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
    {
        // Provide a useful diagnostic even when shaderc returns an empty message.
        if (outDiagnostics.empty())
        {
            outDiagnostics = "Shader compilation failed (no diagnostics from shaderc)";
        }
        outDiagnostics =
            "Source: " + srcPath.string() + "\n" +
            "Stage: " + spec.stage + "\n" +
            "Entry: " + (isHlsl ? spec.entryPoint : std::string("main")) + "\n" +
            "Defines: " + [&]() {
                std::string d;
                for (size_t i = 0; i < spec.defines.size(); ++i) {
                    if (i) d += ", ";
                    d += spec.defines[i];
                }
                return d;
            }() + "\n" +
            "shaderc:\n" + outDiagnostics;
        return false;
    }

    outWords.assign(result.cbegin(), result.cend());
    return true;
}

#endif // shaderc

} // namespace

bool ShaderCompileService::CompileProgramToCache(const ShaderProgramCompileRequest& req,
                                                ShaderSourceKind kind,
                                                ShaderProgramCompileResult& out,
                                                std::string* outError)
{
    auto tCompileStart = std::chrono::high_resolution_clock::now();
    out = ShaderProgramCompileResult{};
    auto setErr = [&](const std::string& msg) {
        if (outError) {
            *outError = msg;
            if (outError->empty()) {
                *outError = "Unknown shader compile error";
            }
        }
    };

    if (req.cacheRoot.empty())
    {
        setErr("ShaderCompileService: cacheRoot is empty");
        return false;
    }
    if (req.stages.empty())
    {
        setErr("ShaderCompileService: no stages provided");
        return false;
    }

    // Compute a deterministic, content-derived cache key. Only true compile
    // inputs may enter: folding in anything diagnostic-only (debugName, the
    // source's file NAME) splits identical compilations into distinct entries,
    // so N same-source materials compile N times instead of hitting once.
    //
    // RELOCATABILITY IS A KEY INVARIANT: nothing machine-specific may enter.
    // Absolute paths — include roots, the source's directory, the resolved
    // spelling of each include — are all facts about WHERE the tree sits, not
    // about what gets compiled, so a cache cooked on one machine must address
    // identically on another. What the roots were a proxy for (which files an
    // #include resolves to) is measured directly by folding the closure's
    // CONTENT, so dropping them loses no discrimination.
    uint64_t h = kShaderHashOffsetBasis;
    // Encode key-format version + chosen compile target so 1.2 and 1.3 builds
    // never collide in a shared cache. The version stands for everything the
    // engine itself writes into a package that the key's inputs cannot see —
    // the key format, the embedded compiler, the reflection walker's output —
    // so it is bumped whenever any of those changes, and the entries an older
    // engine wrote are orphaned rather than migrated: never addressed again.
    // v8: reflection preserves unsigned sampled-image types for WebGPU layouts.
    h = HashShaderString(h, g_MaxBackendSpirv.load(std::memory_order_relaxed) == ShaderCompileService::SpirvTarget::Spirv_1_6
            ? "ShaderCompileServiceTarget:v8:backend-max=spv1.6;mesh-floor=spv1.6"
            : "ShaderCompileServiceTarget:v8:backend-max=spv1.5;mesh-floor=spv1.6");
    // Accumulated across stages: vs and fs of one material share most of their
    // include closure, so a file already folded by an earlier stage need not be
    // read again — the key covers it either way.
    IncludeClosure closure{};
    for (const auto& s : req.stages)
    {
        h = HashShaderString(h, ToLowerAscii(s.stage));
        const std::filesystem::path srcPath = s.sourcePath.is_absolute() ? s.sourcePath : (req.baseDirectory / s.sourcePath);
        // Of the source path, only its extension feeds the key: it selects the
        // source language (IsHlslPath/IsGlslPath/IsSlangPath). The name and the
        // directory are location, not content.
        //
        // The language string doubles as the compiler-lane discriminator:
        // "slang" is a NEW value (.slang previously hard-errored before any
        // cache write), so no pre-existing glsl/hlsl entry can alias a slang
        // key. The Slang lane additionally folds the external slangc's version
        // (the embedded shaderc needs no such fold: it only changes with an
        // engine rebuild, which the key-format version above accounts for).
        const bool stageIsSlang = IsSlangPath(srcPath);
        h = HashShaderString(h, IsHlslPath(srcPath) ? "hlsl" : (stageIsSlang ? "slang" : "glsl"));
        if (stageIsSlang)
        {
            // Reject up front (before any cache probe): with the lane off, a
            // .slang request must not serve a cache entry a flag-on run wrote.
            std::string laneErr;
            if (!SlangLane::EnsureAvailable(&laneErr))
            {
                setErr(laneErr);
                return false;
            }
            h = HashShaderString(h, SlangLane::KeyDiscriminator());
        }
        h = HashShaderString(h, s.entryPoint);
        for (const auto& d : s.defines)
            h = HashShaderString(h, d);

        // Hash source bytes as well: the in-memory source when provided, else the file.
        std::string bytes;
        std::string err;
        if (!ResolveStageSource(s, srcPath, bytes, &err))
        {
            setErr(err);
            return false;
        }
        h = HashShaderBytes(bytes.data(), bytes.size(), h);

        // Includes are compile inputs exactly as much as the top-level bytes:
        // fold their content too. This is what makes an mtime-preserving,
        // same-length edit of a surface shader or an engine include produce a
        // DIFFERENT key instead of relying on a staleness check to notice.
        // Content only — the resolved paths are machine-specific; scan order is
        // deterministic, so position carries the ordering the paths used to.
        std::vector<std::filesystem::path> searchDirs = req.includeDirs;
        searchDirs.push_back(srcPath.parent_path());
        const size_t foldedBefore = closure.paths.size();
        CollectIncludeClosure(bytes, srcPath, searchDirs, closure);
        for (size_t i = foldedBefore; i < closure.paths.size(); ++i)
            h = HashShaderBytes(&closure.hashes[i], sizeof(uint64_t), h);
    }

    // Expose the scanned closure BEFORE any hit/compile/failure exit: every
    // path below (hit, fresh compile, failed compile) leaves the attempted
    // dependency set on the result for the shader-edit invalidation trigger.
    // The "<unreadable>" marker is key-folding detail only — strip it so the
    // exposed list holds plain paths.
    out.includeClosurePaths.reserve(closure.paths.size());
    for (const std::string& p : closure.paths)
    {
        static constexpr std::string_view kUnreadable = "<unreadable>";
        if (p.size() >= kUnreadable.size()
            && p.compare(p.size() - kUnreadable.size(), kUnreadable.size(), kUnreadable) == 0)
            out.includeClosurePaths.push_back(p.substr(0, p.size() - kUnreadable.size()));
        else
            out.includeClosurePaths.push_back(p);
    }

    const std::string key = Hex64(h);
    out.outputDir = req.cacheRoot / key;

    // Cache hit: if artifacts already exist for this key, load and return.
    // Fixed file name inside the key-named directory: the package path is cache
    // addressing, so debugName (diagnostics/log output only) must not decide
    // whether a same-key request hits.
    out.shaderPkgPath = out.outputDir / "program.shaderpkg";
    {
        std::error_code ec;
        if (std::filesystem::exists(out.shaderPkgPath, ec))
        {
            ShaderPackage pkg{};
            std::string loadErr;
            if (LoadShaderPkg(out.shaderPkgPath.string(), kind, pkg, &loadErr))
            {
                // Residual staleness check for dependencies the KEY cannot cover.
                // Everything in `closure.covered` is already keyed by content, so
                // reaching this entry proves those bytes are what was compiled.
                // What remains are dependencies no textual #include scan can
                // resolve — Slang `import` modules (recorded from slangc's
                // depfile) and macro-form directives — which still need a content
                // hash. There is deliberately NO mtime shortcut: an
                // mtime-preserving rewrite is precisely the case that must be
                // caught, and a timestamp is not evidence about content.
                //
                // CONSERVATIVE: missing/empty cacheInfoJson means we can't validate includes —
                // treat as stale rather than risk loading SPIR-V compiled against a now-changed
                // include. Older cache entries written before include tracking landed have no
                // cacheInfoJson; this path forces them to recompile on first hit.
                // Start with "stale until proven fresh" so any failure to validate
                // (missing cacheInfoJson, parse failure, include unreadable, hash mismatch)
                // forces a recompile. The OLD logic defaulted to fresh and bumped to
                // stale on detected mismatch — but that defaulted-to-fresh path silently
                // returned stale binaries when cacheInfoJson was missing or the JSON
                // failed to parse. Conservative-by-default is the right choice for cache
                // correctness; recompiling unnecessarily is a perf regression but
                // returning stale SPIR-V is a correctness regression.
                bool includesStale = true;
                ShaderCacheInfo ci{};
                if (pkg.cacheInfoJson.has_value() && !pkg.cacheInfoJson->empty())
                {
                    std::string ciErr;
                    if (ParseShaderCacheInfoJson(*pkg.cacheInfoJson, ci, &ciErr))
                    {
                        // Tentatively fresh; flip back to stale if any include fails.
                        includesStale = false;
                        for (const auto& inc : ci.includes)
                        {
                            if (inc.path.empty())
                                continue;
                            // Already folded into the key — re-reading it could
                            // only confirm what the key match already proved.
                            if (closure.covered.count(inc.path) != 0)
                                continue;
                            // Through the shared source cache: it re-reads
                            // and re-compares the bytes, so this is as strict
                            // as a direct read, and these are the same engine
                            // headers every other program's revalidation asks
                            // about.
                            const ShaderSourceCache::EntryPtr cached =
                                ShaderSourceCache::Get().Read(inc.path, inc.path);
                            if (!cached)
                            {
                                includesStale = true;
                                break;
                            }
                            const uint64_t hNow = cached->Hash;
                            if (hNow != inc.hash64)
                            {
                                includesStale = true;
                                break;
                            }
                        }
                    }
                }

                if (!includesStale)
                {
                    // The recorded include set can exceed the scanned closure
                    // (Slang module imports, macro-form directives) — merge the
                    // extras so the exposed dependency list is ground truth.
                    for (const auto& inc : ci.includes)
                    {
                        if (!inc.path.empty() && closure.covered.count(inc.path) == 0)
                            out.includeClosurePaths.push_back(inc.path);
                    }
                    out.meta = std::move(pkg.meta);
                    out.stageBytes = std::move(pkg.stageBytes);
                    out.cacheInfoJson = std::move(pkg.cacheInfoJson);
                    {
                        auto ms = std::chrono::duration<double, std::milli>(
                            std::chrono::high_resolution_clock::now() - tCompileStart).count();
                        if (ms > 5.0)
                            Logger::Log::Info("[ShaderCache] HIT '{}': {:.1f}ms", req.debugName, ms);
                    }
                    return true;
                }
                // Include changed: fall through to recompile.
            }
            // If load failed, fall through to recompile.
        }
    }

    // Genuine miss. Only now does a compiler become a requirement: a runtime
    // built without one (wasm) serves cooked entries perfectly well, and gating
    // on the compiler before the probe would make every cooked cache unusable
    // there. Name the key so the miss is actionable — it is the exact directory
    // the offline cook must have produced.
    if (!IsCompilerAvailable())
    {
        // The key folds the per-stage defines and source bytes, so a miss that
        // "should" have been cooked is almost always a define or source-text
        // skew between the cook and this runtime — list the defines so the two
        // sides can be diffed from this one message.
        std::string stages;
        for (const auto& s : req.stages)
        {
            if (!stages.empty())
                stages += ", ";
            stages += s.stage + ":" + s.sourcePath.filename().string() + " [defines:";
            for (const auto& d : s.defines)
                stages += " " + d;
            stages += "]";
        }
        setErr("ShaderCompileService: no cooked program for '" + req.debugName + "' (" + stages
               + ") and this runtime has no shader compiler. Expected "
               + out.shaderPkgPath.string()
               + ". Cook this variant offline (Tools/MaterialVariantCook) and ship the cache root"
                 " with the content, or run on a build that has shaderc.");
        return false;
    }

#if defined(RENDERING_HAS_SHADERC) && RENDERING_HAS_SHADERC
    // Compile all stages. The compiler emits SPIR-V and nothing else, so this
    // arm ignores `kind`: the package it writes carries SPIR-V chunks only, and
    // a Wgsl-kind caller that lands here has an un-cooked program.
    std::unordered_map<std::string, std::vector<uint8_t>> spvBytes;
    std::vector<StageReflectionResult> reflectedStages;
    reflectedStages.reserve(req.stages.size());
    std::unordered_map<std::string, uint64_t> includeHashes;

    for (const auto& stageSpec : req.stages)
    {
        const std::string stageKey = ToLowerAscii(stageSpec.stage);
        ShaderStageKind stageKind{};
        std::string stageErr;
        if (!MapStage(stageKey, stageKind, stageErr))
        {
            setErr(stageErr);
            return false;
        }

        std::vector<uint32_t> words;
        std::string diag;
        if (!CompileOneStage(stageSpec, req, stageKind, words, diag, &includeHashes))
        {
            setErr("Compile failed (" + stageKey + "):\n" + diag);
            return false;
        }

        // Keep SPIR-V bytes in-memory; we package them into a single .shaderpkg.
        std::vector<uint8_t> bytes(words.size() * sizeof(uint32_t));
        if (!bytes.empty())
            std::memcpy(bytes.data(), words.data(), bytes.size());
        spvBytes[stageKey] = std::move(bytes);

        // Reflect
        StageReflectionResult rr{};
        ReflectionOptions ro{};
        std::string reflErr;
        if (!ReflectSpirv(stageKind, words.data(), words.size(), ro, rr, &reflErr))
        {
            setErr("Reflection failed (" + stageKey + "): " + reflErr);
            return false;
        }
        reflectedStages.push_back(std::move(rr));
    }

    {
        auto ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - tCompileStart).count();
        Logger::Log::Info("[ShaderCache] COMPILE '{}': {:.1f}ms ({} stages)", req.debugName, ms, req.stages.size());
    }

    // Merge compiler-recorded dependencies the textual scan cannot resolve
    // (Slang module imports, macro-form includes) into the exposed closure.
    for (const auto& kv : includeHashes)
    {
        if (closure.covered.count(kv.first) == 0)
            out.includeClosurePaths.push_back(kv.first);
    }

    // Merge and validate metadata
    ShaderMeta meta = MergeStages(reflectedStages);
    {
        auto report = ValidateShaderMeta(meta, 128);
        if (report.HasErrors())
        {
            if (outError)
            {
                std::string msg = "ShaderMeta validation failed: ";
                for (const auto& i : report.Issues)
                {
                    if (i.Severity == IssueSeverity::Error)
                    {
                        msg += i.Code + ": " + i.Message + "; ";
                    }
                }
                setErr(msg);
            }
            return false;
        }
    }

    out.meta = std::move(meta);

    // Emit shaderpkg
    std::error_code ec;
    std::filesystem::create_directories(out.outputDir, ec);

    // Record ONLY the dependencies the key could not cover — Slang module
    // imports and macro-form directives, which no textual scan resolves. A
    // covered include is proven by the key match itself, and recording its
    // absolute path here would put a machine-specific fact into the package and
    // make a relocated cache fail revalidation on a file that has not changed.
    {
        ShaderCacheInfo ci{};
        for (const auto& kv : includeHashes)
        {
            if (closure.covered.count(kv.first) != 0)
                continue;
            ShaderCacheInfo::Include inc{};
            inc.path = kv.first;
            inc.hash64 = kv.second;
            ci.includes.push_back(std::move(inc));
        }
        const std::string ciJson = BuildShaderCacheInfoJson(ci);
        if (!ciJson.empty())
            out.cacheInfoJson = ciJson;
        else
            out.cacheInfoJson.reset();
    }

    std::string saveErr;
    if (!SaveShaderPkg(out.shaderPkgPath, out.meta, spvBytes, out.cacheInfoJson, &saveErr))
    {
        setErr(saveErr.empty() ? "Failed to save .shaderpkg" : saveErr);
        return false;
    }
    out.stageBytes = std::move(spvBytes);

    g_TotalCompilations.fetch_add(1, std::memory_order_relaxed);
    return true;
#else
    // Never reached: IsCompilerAvailable() is false in a build with no compiler,
    // so a miss already returned above.
    return false;
#endif
}

}} // namespace GameEngine::Rendering
