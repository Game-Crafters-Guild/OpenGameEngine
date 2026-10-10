#pragma once

// ShaderPropertyTable: the material properties a shader program declares with
//
//   // @property <type> <name> ["Display Name"] [key=value ...] [flags]
//
// reflected into GPU lane placement, authoring defaults and inspector metadata.
// The table is a pure function of the adapter + surface + vertex-modifier
// sources — identical for every material that uses the program — so it can
// never fragment batching or the shader variant. The declared name is the GLSL
// spelling (Props.<name>), the .material key and the inspector identity.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Rendering
{

enum class ShaderPropertyType : uint8_t
{
    Float,
    Vec2,
    Vec3,
    Vec4,
    Color,
    Bool,
    Int,
    Enum,
};

const char* ShaderPropertyTypeName(ShaderPropertyType type);

// Which producer declared the property. Lanes are packed in this order.
enum class ShaderPropertyOrigin : uint8_t
{
    Adapter,
    Surface,
    VertexModifier,
};

struct ShaderProperty
{
    std::string Name;
    std::string DisplayName; // authored, or derived from Name ("pulseSpeed" -> "Pulse Speed")
    ShaderPropertyType Type = ShaderPropertyType::Float;
    ShaderPropertyOrigin Origin = ShaderPropertyOrigin::Surface;

    // Component defaults. Bool is 0/1, int and enum hold the integer value.
    std::array<float, 4> Default{};
    bool HasRange = false;
    float RangeMin = 0.0f;
    float RangeMax = 1.0f;
    std::string Group;
    std::string VisibleIf; // "name", "name=value" or "name!=value"
    std::string Tooltip;
    std::vector<std::string> EnumValues; // enum: labels; the stored value is the index
    bool Hdr = false;
    bool Hidden = false;
    bool HasAlpha = false; // color: four components instead of three

    // Placement in the MaterialGpuParams block. HasLane is false for a keyword
    // bool and for an adapter-declared name that no surface or vertex modifier
    // declares — the adapter then reads a compile-time constant.
    bool HasLane = false;
    uint32_t Lane = 0;
    uint32_t Component = 0;
    uint32_t ByteOffset = 0;
    uint32_t ByteSize = 0;

    // The declaration that won, for diagnostics.
    std::string SourceFile;
    uint32_t SourceLine = 0;

    uint32_t ComponentCount() const;
    // GLSL type of the Props member (color -> vec3/vec4, enum -> int).
    const char* GlslType() const;
};

struct ShaderPropertyDiagnostic
{
    std::string File;
    uint32_t Line = 0;
    std::string Message;

    // "<file>:<line>: <severity>: <message>" — the shape the Shader Errors
    // panel parses into a file:line row.
    std::string Format(const char* severity) const;
};

struct ShaderPropertyTable
{
    std::vector<ShaderProperty> Properties; // declaration order
    std::vector<ShaderPropertyDiagnostic> Errors;
    std::vector<ShaderPropertyDiagnostic> Warnings;
    // True when the surface or the vertex modifier declared at least one
    // property. False for a legacy surface that still reads lanes by name.
    bool HasSurfaceDeclarations = false;
    uint32_t LanesUsed = 0;

    bool Rejected() const { return !Errors.empty(); }
    const ShaderProperty* Find(std::string_view name) const;
};

// One producer's source text and the file it came from (for diagnostics).
struct ShaderPropertySource
{
    std::string Text;
    std::string File;
    ShaderPropertyOrigin Origin = ShaderPropertyOrigin::Surface;
};

// Parse one producer's `// @property` declarations. Grammar errors land in
// Errors; nothing is packed.
ShaderPropertyTable ParseDeclaredProperties(const ShaderPropertySource& source);

// Parse every producer, merge same-name declarations (same type shares one
// slot at the first declaration's position; a type conflict is an error) and
// pack lanes greedy first-fit in adapter -> surface -> vertex-modifier order:
// vec4 on a lane boundary, vec3 on .xyz (a following scalar may take .w),
// vec2 on an 8-byte pair, scalars in the next free component.
ShaderPropertyTable BuildShaderPropertyTable(const std::vector<ShaderPropertySource>& sources);

} // namespace GameEngine::Rendering
