#include "Rendering/Materials/MaterialBuildService.h"

#include "FileSystem/FileSystem.h"
#include "Rendering/Core/Device.h"
#include "Types/StringUtils.h"

#include <fstream>
#include <optional>
#include <sstream>

#include "Rendering/Materials/MaterialKeywordDerivation.h"
#include "Rendering/Materials/MaterialShaderIncludeRoots.h"
#include "Rendering/Materials/ShaderCapabilityDetector.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "ShaderIncludeClosure.h"

namespace GameEngine::Rendering
{
namespace
{
static std::filesystem::path MakeGeneratedAdapterPath(const std::filesystem::path& cacheRoot,
                                                      const std::filesystem::path& materialPath,
                                                      const char* leafName)
{
    namespace fs = std::filesystem;
    const std::string key = materialPath.lexically_normal().string() + (leafName ? std::string("|") + leafName : std::string());
    const size_t h = std::hash<std::string>{}(key);
    std::ostringstream oss;
    oss << std::hex << h;
    const fs::path dir = (cacheRoot / "Generated" / "Materials").lexically_normal();
    const fs::path file = (std::string(materialPath.stem().string()) + "_" + oss.str() + "_" + (leafName ? leafName : "adapter")).c_str();
    return (dir / file).lexically_normal();
}

// On a failed compile, materialize the in-memory composed source so the
// diagnostics reference something a human can open (the compile itself is
// in-memory only — the "Source:" path in the error never exists on disk).
// Unique temp file + rename keeps concurrent variant builds of the same
// material from interleaving writes into one dump path.
static void DumpFailedComposedSource(const std::filesystem::path& cacheRoot,
                                     const ShaderStageCompileSpec& spec,
                                     std::vector<std::string>& errors)
{
    if (spec.inlineSource.empty())
        return;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = (cacheRoot / "Failed").lexically_normal();
    fs::create_directories(dir, ec);
    if (ec)
        return;
    const fs::path target = dir / spec.sourcePath.filename();
    const fs::path tmp = FileSystem::MakeTemporarySiblingPath(target);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return;
        out << spec.inlineSource;
    }
    if (!FileSystem::PublishFile(tmp, target))
        return;
    errors.push_back("Composed " + spec.stage + " source dumped to: " + target.string());
}

// The declared texture slots of the surface `doc` composes with, or nullopt when that surface does
// not resolve, cannot be read or is rejected (the compose reports those by name).
std::optional<TextureSlotResolution> ResolveDeclaredSurfaceSlots(const GameEngine::MaterialDocument& doc,
                                                                 const std::filesystem::path& materialDir,
                                                                 const MaterialBuildContext& context)
{
    std::string source;
    if (!ReadShaderFileText(ShaderComposer::ResolveSurfaceShaderPath(doc.surfaceShader, materialDir, context), source,
                            nullptr))
        return std::nullopt;
    TextureSlotResolution slots = ShaderComposer::ResolveTextureSlots(source);
    if (slots.Rejected)
        return std::nullopt;
    return slots;
}
} // namespace

void ShaderPackageDeleter::operator()(ShaderPackage* p) const noexcept
{
    delete p;
}

MaterialBuildResult BuildMaterialToShaderPackage(const GameEngine::MaterialDocument& doc,
                                                 const std::filesystem::path& materialPath,
                                                 const std::string& debugName,
                                                 const MaterialBuildContext& context,
                                                 ShaderSourceKind kind,
                                                 MaterialKeyword pipelineKeywords,
                                                 VertexAttributeFlags meshVertexFlags)
{
    MaterialBuildResult out{};

    if (materialPath.empty())
    {
        out.errors.push_back("Material build failed: empty materialPath.");
        return out;
    }
    if (doc.schemaVersion <= 2 && doc.surfaceShader.empty())
    {
        out.errors.push_back("Material build failed: missing required field 'surfaceShader'.");
        return out;
    }

    try
    {
        const auto& cacheRoot = context.CacheRoot;
        const auto& adapterDir = context.AdapterShaderDir;

        ShaderProgramCompileRequest creq{};
        creq.debugName = debugName.empty() ? materialPath.stem().string() : debugName;
        creq.baseDirectory = materialPath.parent_path();
        creq.cacheRoot = cacheRoot;

        ShaderVariantKey variantKey{};
        variantKey.vertexFlags = VertexAttributeFlags::StandardMesh;
        if (meshVertexFlags != VertexAttributeFlags::None)
            variantKey.vertexFlags = meshVertexFlags;
        variantKey.materialKeywords = pipelineKeywords;
        ApplyAlphaModeKeyword(variantKey, doc.alphaMode);
        // Unconditional: pipelineKeywords arrive from a caller's key, which may already
        // carry a modifier bit. This is the authority on the form, so it clears both and
        // sets the one the resolved file actually implements rather than merging on top.
        ApplyVertexModifierKeyword(variantKey, doc.vertexModifier, materialPath.parent_path(),
                                   context);
        // The same authority for the relief march: a cook of a height-mapped material composes
        // the parallax variant of every row it is asked for, and a cache-driven compile's stand-in
        // document carries the height binding exactly when registration derived the keyword.
        // Reporting a refusal is the registration path's job.
        const std::optional<TextureSlotResolution> surfaceSlots =
            ResolveDeclaredSurfaceSlots(doc, materialPath.parent_path(), context);
        ApplyParallaxKeyword(variantKey, doc, surfaceSlots ? &surfaceSlots->DeclaredSlots : nullptr);
        // The relief's depth reaches only the program that marches, and never the compatibility
        // profile, whichever row asked for it.
        constexpr MaterialKeyword reliefDepth =
            MaterialKeyword::ParallaxDepthOffset | MaterialKeyword::ParallaxDepthFromPrepass |
            MaterialKeyword::ParallaxPrepassDepthMultisample | MaterialKeyword::ParallaxDepthTolerance;
        variantKey.materialKeywords =
            (variantKey.materialKeywords & ~reliefDepth)
            | NarrowColorPassKeywords(variantKey.materialKeywords, variantKey.materialKeywords & reliefDepth);
        if (doc.customVertexShader)
            ApplyCustomVertexShaderClamp(variantKey);
        variantKey.lightingModel = doc.lightingModel.empty()
            ? LightingModel::kUnlit
            : HashStringId(GameEngine::ToLowerAscii(doc.lightingModel));

        if (adapterDir.empty())
        {
            out.errors.push_back("Material build failed: MaterialBuildContext has empty AdapterShaderDir.");
            return out;
        }

        std::vector<std::string> composeErrors;
        auto composed = ShaderComposer::Compose(
            doc, variantKey, materialPath.parent_path(), context, &composeErrors);

        if (!composed.IsValid())
        {
            for (const auto& ce : composeErrors)
                out.errors.push_back("ShaderComposer: " + ce);
            out.errors.push_back("Material build failed: ShaderComposer could not produce source.");
            return out;
        }

        out.composedVertexSource = composed.vertexSource;
        out.composedFragmentSource = composed.fragmentSource;
        out.composedDefines = composed.defines;

        // customVertexShader materials have no vertex buffer — never let a
        // surface-graph HAS_TANGENT define drag the layout back off None.
        if (!doc.customVertexShader)
        {
            for (const std::string& def : composed.defines)
            {
                if (def == "HAS_TANGENT")
                {
                    variantKey.vertexFlags = VertexAttributeFlags::StandardMeshWithTangent;
                    break;
                }
            }
        }

        // The same roots ShaderComposer spelled its substituted #include
        // literals against (BuildMaterialIncludeRoots is the single authority),
        // so every literal it emitted resolves here.
        creq.includeDirs = BuildMaterialIncludeRoots(context, materialPath.parent_path());

        // Compile the composed source in-memory. The path is the logical name only
        // (diagnostics + #include search base); it is intentionally NOT written. Writing
        // to a path keyed on materialPath alone — shared by every variant and by any
        // concurrent build of the same material — let one thread read a half-written file
        // while another rewrote it, feeding glslang truncated GLSL and corrupting the heap.
        const std::filesystem::path vsGenPath = MakeGeneratedAdapterPath(cacheRoot, materialPath, "composed.vert");
        ShaderStageCompileSpec vs{};
        vs.stage = "vs";
        vs.sourcePath = vsGenPath;
        vs.inlineSource = std::move(composed.vertexSource);
        vs.defines = composed.defines;
        creq.stages.push_back(std::move(vs));

        const std::filesystem::path fsGenPath = MakeGeneratedAdapterPath(cacheRoot, materialPath, "composed.frag");
        ShaderStageCompileSpec fs{};
        fs.stage = "fs";
        fs.sourcePath = fsGenPath;
        fs.inlineSource = std::move(composed.fragmentSource);
        fs.defines = composed.defines;
        creq.stages.push_back(std::move(fs));

        // Dependency record for the shader-edit invalidation trigger: the
        // adapter templates are inlined into the composed source (they never
        // appear as #include directives, so the compile service's closure scan
        // cannot see them) — record them here; the compile service appends the
        // rest (surface, vertex modifier, transitive includes). Filled on
        // failure too, so a shader that failed to compile still names the files
        // it RESOLVED and an edit to one of them re-triggers it. An operand that
        // resolved nowhere is not recorded at all (see includeClosurePaths).
        auto recordClosure = [&](const ShaderProgramCompileResult& cres)
        {
            std::error_code closureEc;
            for (const char* adapterFile : {composed.vertexAdapterName.c_str(),
                                            composed.fragmentAdapterName.c_str()})
            {
                std::filesystem::path abs =
                    std::filesystem::absolute(adapterDir / adapterFile, closureEc);
                if (closureEc)
                    abs = adapterDir / adapterFile;
                out.includeClosurePaths.push_back(abs.lexically_normal().string());
            }
            out.includeClosurePaths.insert(out.includeClosurePaths.end(),
                                           cres.includeClosurePaths.begin(),
                                           cres.includeClosurePaths.end());
        };

        ShaderProgramCompileResult cres{};
        std::string compileErr;
        if (!ShaderCompileService::CompileProgramToCache(creq, kind, cres, &compileErr))
        {
            recordClosure(cres);
            out.errors.push_back("Material build failed: " + compileErr);
            // A missing/mis-signatured EvaluateSurface errors at the ADAPTER's
            // call site, which is truthful but points away from the actual fix.
            // Name the user's surface file explicitly.
            if (!doc.surfaceShader.empty() &&
                compileErr.find("'EvaluateSurface'") != std::string::npos)
            {
                out.errors.push_back(
                    "Hint: the surface shader '" + doc.surfaceShader +
                    "' must define exactly: SurfaceOutput EvaluateSurface(SurfaceInput). "
                    "See Engine/Modules/Rendering/docs/SurfaceShaderAuthoring.html.");
            }
            // Dump only the stage that failed (CompileProgramToCache stops at
            // the first failing stage — "Compile failed (<stage>):"). If the
            // marker is missing, dump everything rather than nothing.
            std::string failedStage;
            {
                const std::string marker = "Compile failed (";
                const size_t at = compileErr.find(marker);
                const size_t end = at == std::string::npos
                    ? std::string::npos
                    : compileErr.find(')', at + marker.size());
                if (end != std::string::npos)
                    failedStage = compileErr.substr(at + marker.size(), end - at - marker.size());
            }
            for (const auto& stage : creq.stages)
            {
                if (failedStage.empty() || stage.stage == failedStage)
                    DumpFailedComposedSource(cacheRoot, stage, out.errors);
            }
            return out;
        }

        recordClosure(cres);
        out.generatedShaderPkgPath = cres.shaderPkgPath.string();
        auto pkg = std::unique_ptr<ShaderPackage, ShaderPackageDeleter>(new ShaderPackage());
        pkg->version = 1;
        pkg->meta = std::move(cres.meta);
        // The declared-property table is a function of the composed source, not
        // of the SPIR-V, so it is attached here on cache hits and misses alike.
        if (composed.declaredProperties)
        {
            pkg->meta.DeclaredProperties = composed.declaredProperties->Properties;
            // The package is a derived artifact: it names a declaration's file by
            // its shader root, never by this machine's path to it.
            for (ShaderProperty& p : pkg->meta.DeclaredProperties)
                p.SourceFile =
                    ShaderComposer::ShaderRootRelative(p.SourceFile, materialPath.parent_path(), context);
        }
        pkg->stageBytes = std::move(cres.stageBytes);
        pkg->cacheInfoJson = std::move(cres.cacheInfoJson);
        out.package = std::move(pkg);
        out.success = true;
        return out;
    }
    catch (const std::exception& e)
    {
        out.errors.push_back(std::string("Material build exception: ") + e.what());
        return out;
    }
}
} // namespace GameEngine::Rendering

