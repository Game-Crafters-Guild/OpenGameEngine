#pragma once

// ShaderComposer: composes adapter + surface + lighting model shader source
// strings from a MaterialDocument and ShaderVariantKey.
//
// This is a pure string-manipulation utility. It does NOT compile shaders or
// touch the GPU. The output is a pair of source strings (vertex + fragment)
// that can be written to disk and compiled by ShaderCompileService.
//
// Responsibilities:
//   - Select the correct adapter template (instanced vs push-constant, lit vs unlit)
//   - Inject preprocessor defines from ShaderVariantKey
//   - Resolve the surface shader include path
//   - Emit the program's declared-property accessors (GE_Props / Props)
//   - Produce compilable GLSL source strings
//
// Ownership:
//   - Stateless utility. No persistent state.
//   - Called by MaterialBuildService during material compilation.

#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/ShaderPropertyTable.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
struct MaterialDocument;

namespace Rendering
{

// Result of scanning a surface shader's `// @texture <name> [srgb|linear]`
// declarations and resolving each declared name to a bindless texture slot
// ordinal. A pure function of the surface source (see
// ShaderComposer::ResolveTextureSlots) — identical for every material that uses
// that surface, so it never fragments batching or the shader variant.
struct TextureSlotResolution
{
    // Every declared name in declaration order, paired with its resolved ordinal.
    // Drives both the emitted `#define GE_TEXSLOT_<name> <ordinal>` macros
    // (composition) and the per-material name->slot routing map (registration).
    std::vector<std::pair<std::string, uint8_t>> DeclaredSlots;

    // True when the surface declared at least one `@texture` tag. False for
    // every legacy surface (no tags) — those keep the fixed-ladder behavior with
    // zero change.
    bool HasDeclarations = false;

    // Set when the surface mixes user (non-well-known) `@texture` names with
    // raw-ordinal texture access (a triplanar/collapse helper or a numeric
    // GE_SLOT_TEX(<n>)): the user name would silently alias a raw ordinal. A
    // hard compose failure, never a silent misroute. Also set when a user name
    // cannot fit in the [0,8) slot window.
    bool Rejected = false;
    std::string RejectReason;
};

struct ComposedShaderSource
{
    std::string vertexSource;
    std::string fragmentSource;

    // Diagnostic: which adapter templates were used.
    std::string vertexAdapterName;
    std::string fragmentAdapterName;

    // The preprocessor defines that were injected.
    std::vector<std::string> defines;

    // The declared-property table the GE_Props block was generated from; rides
    // into ShaderMeta::DeclaredProperties so the inspector and the validator
    // see exactly what the shader reads.
    std::shared_ptr<const ShaderPropertyTable> declaredProperties;

    bool IsValid() const
    {
        return !vertexSource.empty() && !fragmentSource.empty();
    }
};

class ShaderComposer
{
  public:
    // Compose shader sources for a material document.
    //
    // `materialDir` is used to resolve relative surface shader paths first.
    // `context` supplies the remaining shader roots: package Shaders/ dirs
    //   (highest mount priority first), then AdapterShaderDir — the engine's
    //   built-in shader tree and the home of the adapter templates.
    //
    // Returns a ComposedShaderSource with vertex and fragment GLSL strings.
    // On failure, returns an invalid result and appends errors. An AUTHORED
    // surfaceShader that does not resolve against any root is a hard failure
    // (error names the material and every searched root); the built-in default
    // surface is substituted ONLY when the document authors no surface.
    static ComposedShaderSource Compose(const MaterialDocument& doc,
                                        const ShaderVariantKey& key,
                                        const std::filesystem::path& materialDir,
                                        const MaterialBuildContext& context,
                                        std::vector<std::string>* outErrors = nullptr);

    // Resolve a surface / vertex-modifier reference against the shader root
    // chain: materialDir -> context.ProjectRoots -> context.PackageShaderDirs
    // (in order) -> context.AdapterShaderDir. An EMPTY materialDir means the
    // material's asset path is unavailable and that probe is skipped — a
    // stand-in directory must never masquerade as the material's own. Absolute
    // references pass through unchanged (GUID-reconciled references arrive
    // absolute from the asset registry). When no root contains the file,
    // returns the materialDir candidate (engine-tree candidate when
    // materialDir is empty) so the caller's existence check / diagnostics name
    // a concrete path.
    static std::filesystem::path ResolveShaderReference(const std::string& relativePath,
                                                        const std::filesystem::path& materialDir,
                                                        const MaterialBuildContext& context);

    // The surface a document composes with: its authored reference, or the built-in default
    // surface when it authors none, resolved through ResolveShaderReference.
    static std::filesystem::path ResolveSurfaceShaderPath(const std::string& surfaceShader,
                                                          const std::filesystem::path& materialDir,
                                                          const MaterialBuildContext& context);

    // The inverse spelling for a path ResolveShaderReference produced: relative
    // to the first shader root that contains it, in the same probe order, or the
    // file name alone when none does. What a derived artifact records about a
    // file, so a machine-local path never leaves the process that way.
    static std::string ShaderRootRelative(const std::filesystem::path& path,
                                          const std::filesystem::path& materialDir,
                                          const MaterialBuildContext& context);

    // Generate the preprocessor preamble (the block of #define lines) for a variant.
    // `lightingModelName` is the original string (e.g. "StandardPBR") for the define.
    static std::string GenerateDefinePreamble(const ShaderVariantKey& key,
                                              const std::string& lightingModelName);

    // Generate the preprocessor preamble from an explicit list of define strings.
    static std::string GenerateDefinePreamble(const std::vector<std::string>& defines);

    // Resolve a surface's `// @texture <name> [srgb|linear]` declarations to
    // bindless slot ordinals. Well-known names keep their canonical ordinal (the
    // fixed adapter aliases albedoMap=0 .. metallicMap=7, plus the triplanar
    // front-end names); user names pack into the lowest ordinals not claimed by a
    // declared well-known name, in lexicographic order. Pure function of the
    // source: the same call at composition (macro emission) and at registration
    // (name->slot routing) cannot drift. Rejects a surface that mixes user names
    // with raw-ordinal access — see TextureSlotResolution::Rejected.
    static TextureSlotResolution ResolveTextureSlots(const std::string& surfaceSource);

    // One diagnostic line per material keyword the surface never reads, with a
    // nearest-match suggestion when the keyword looks like a typo of one it does
    // read. A misspelled keyword is otherwise a silent no-op: the preamble emits
    // GE_USER_<TYPO>, no #ifdef matches, and the material renders as if the
    // keyword were absent.
    //
    // Pure function of the surface source and the keyword list; the caller logs
    // the lines. These are WARNINGS by construction — an unread keyword is legal
    // (it may be read from an #include, which this scan does not expand), so the
    // result must never fail a compile.
    static std::vector<std::string> DiagnoseUnreadKeywords(const std::string& surfaceSource,
                                                           const std::vector<std::string>& keywords,
                                                           const std::string& surfaceName);

  private:
    // The GLSL block substituted for the adapters' `// GE_DECLARED_PROPERTIES`
    // marker: `struct GE_Props`, the `Props` accessor macro and
    // GE_LoadDeclaredProperties(), which fills every member from its packed
    // lane (`ge_MatData.uParams[lane].swizzle`) or, for an adapter read no
    // producer stores, from its declared default as a compile-time constant.
    // Pure function of the table.
    static std::string GenerateDeclaredPropertiesBlock(const ShaderPropertyTable& table);
};

} // namespace Rendering
} // namespace GameEngine
