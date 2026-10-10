#include "Panels/ScriptSyntax.h"

#include "AssetCore/AssetTypes.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_set>

namespace GameEngine::Editor
{

namespace
{

using WordSet = std::unordered_set<std::string_view>;

const WordSet kCSharpKeywords = {
    "abstract", "as", "base", "break", "case", "catch", "checked", "class", "const", "continue",
    "default", "delegate", "do", "else", "enum", "event", "explicit", "extern", "false", "finally",
    "fixed", "for", "foreach", "goto", "if", "implicit", "in", "interface", "internal", "is", "lock",
    "namespace", "new", "null", "operator", "out", "override", "params", "private", "protected",
    "public", "readonly", "ref", "return", "sealed", "sizeof", "stackalloc", "static", "struct",
    "switch", "this", "throw", "true", "try", "typeof", "unchecked", "unsafe", "using", "virtual",
    "volatile", "while", "async", "await", "get", "set", "value", "var", "yield", "partial", "where",
    "select", "from", "group", "into", "orderby", "join", "let", "by", "on", "equals", "record",
    "init", "nameof", "when", "with",
};

const WordSet kCSharpTypes = {
    "bool", "byte", "sbyte", "char", "decimal", "double", "float", "int", "uint", "long", "ulong",
    "short", "ushort", "string", "object", "void", "dynamic", "nint", "nuint",
    "Component", "Transform", "Vector2", "Vector3", "Vector4", "Quaternion", "Color", "Rect",
    "Bounds", "Matrix4x4",
};

const WordSet kGlslKeywords = {
    "attribute", "const", "uniform", "varying", "buffer", "shared", "coherent", "volatile",
    "restrict", "readonly", "writeonly", "layout", "centroid", "flat", "smooth", "noperspective",
    "patch", "sample", "invariant", "precise", "break", "continue", "do", "for", "while", "switch",
    "case", "default", "if", "else", "subroutine", "in", "out", "inout", "true", "false", "discard",
    "return", "struct", "precision", "highp", "mediump", "lowp",
};

const WordSet kGlslTypes = {
    "void", "bool", "int", "uint", "float", "double",
    "vec2", "vec3", "vec4", "dvec2", "dvec3", "dvec4", "bvec2", "bvec3", "bvec4",
    "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4",
    "mat2", "mat3", "mat4", "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4",
    "mat4x2", "mat4x3", "mat4x4", "dmat2", "dmat3", "dmat4",
    "sampler1D", "sampler2D", "sampler3D", "samplerCube", "sampler2DShadow", "samplerCubeShadow",
    "sampler1DArray", "sampler2DArray", "sampler2DArrayShadow", "samplerCubeArray",
    "samplerCubeArrayShadow", "sampler2DMS", "sampler2DMSArray", "samplerBuffer",
    "isampler2D", "isampler3D", "isamplerCube", "isampler2DArray", "usampler2D", "usampler3D",
    "usamplerCube", "usampler2DArray", "image1D", "image2D", "image3D", "imageCube", "image2DArray",
    "iimage2D", "iimage3D", "uimage2D", "uimage3D", "imageBuffer", "atomic_uint",
    "texture1D", "texture2D", "texture3D", "textureCube", "texture2DArray", "textureCubeArray",
    "sampler", "samplerShadow", "subpassInput", "subpassInputMS",
    // Built-in variables.
    "gl_Position", "gl_PointSize", "gl_ClipDistance", "gl_CullDistance", "gl_VertexIndex",
    "gl_InstanceIndex", "gl_VertexID", "gl_InstanceID", "gl_BaseVertex", "gl_BaseInstance",
    "gl_DrawID", "gl_FragCoord", "gl_FrontFacing", "gl_PointCoord", "gl_FragDepth", "gl_SampleID",
    "gl_SamplePosition", "gl_SampleMask", "gl_SampleMaskIn", "gl_HelperInvocation", "gl_Layer",
    "gl_ViewportIndex", "gl_PrimitiveID", "gl_InvocationID", "gl_NumWorkGroups", "gl_WorkGroupSize",
    "gl_WorkGroupID", "gl_LocalInvocationID", "gl_GlobalInvocationID", "gl_LocalInvocationIndex",
    "gl_in", "gl_out", "gl_PerVertex",
    // Built-in functions.
    "radians", "degrees", "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
    "asinh", "acosh", "atanh", "pow", "exp", "log", "exp2", "log2", "sqrt", "inversesqrt", "abs",
    "sign", "floor", "trunc", "round", "roundEven", "ceil", "fract", "mod", "modf", "min", "max",
    "clamp", "mix", "step", "smoothstep", "isnan", "isinf", "floatBitsToInt", "floatBitsToUint",
    "intBitsToFloat", "uintBitsToFloat", "fma", "frexp", "ldexp", "packUnorm2x16", "packSnorm2x16",
    "unpackUnorm2x16", "unpackSnorm2x16", "packUnorm4x8", "unpackUnorm4x8", "packHalf2x16",
    "unpackHalf2x16", "length", "distance", "dot", "cross", "normalize", "faceforward", "reflect",
    "refract", "matrixCompMult", "outerProduct", "transpose", "determinant", "inverse", "lessThan",
    "lessThanEqual", "greaterThan", "greaterThanEqual", "equal", "notEqual", "any", "all", "not",
    "texture", "textureLod", "textureGrad", "textureProj", "textureOffset", "textureLodOffset",
    "textureProjLod", "textureGradOffset", "texelFetch", "texelFetchOffset", "textureSize",
    "textureQueryLod", "textureQueryLevels", "textureSamples", "textureGather",
    "textureGatherOffset", "imageLoad", "imageStore", "imageSize", "imageAtomicAdd",
    "imageAtomicExchange", "atomicAdd", "atomicMin", "atomicMax", "atomicAnd", "atomicOr",
    "atomicXor", "atomicExchange", "atomicCompSwap", "atomicCounter", "atomicCounterIncrement",
    "atomicCounterDecrement", "dFdx", "dFdy", "dFdxFine", "dFdyFine", "dFdxCoarse", "dFdyCoarse",
    "fwidth", "fwidthFine", "fwidthCoarse", "barrier", "memoryBarrier", "memoryBarrierBuffer",
    "memoryBarrierShared", "memoryBarrierImage", "groupMemoryBarrier", "subpassLoad", "EmitVertex",
    "EndPrimitive", "bitfieldExtract", "bitfieldInsert", "bitfieldReverse", "bitCount", "findLSB",
    "findMSB", "uaddCarry", "usubBorrow", "umulExtended", "imulExtended", "interpolateAtCentroid",
    "interpolateAtSample", "interpolateAtOffset", "noise1", "noise2", "noise3", "noise4",
};

struct LanguageRules
{
    const WordSet* Keywords;
    const WordSet* Types;
    bool CharLiterals;        // 'c' is a character literal (C#), not an error to skip (GLSL)
    bool CommentAnnotations;  // `@tag` inside a line comment is an Annotation token
};

const LanguageRules& RulesFor(ScriptLanguage language)
{
    static const LanguageRules csharp{&kCSharpKeywords, &kCSharpTypes, /*CharLiterals=*/true,
                                      /*CommentAnnotations=*/false};
    static const LanguageRules glsl{&kGlslKeywords, &kGlslTypes, /*CharLiterals=*/false,
                                    /*CommentAnnotations=*/true};
    return language == ScriptLanguage::Glsl ? glsl : csharp;
}

bool IsWordStart(unsigned char c) { return std::isalpha(c) || c == '_'; }
bool IsWordChar(unsigned char c) { return std::isalnum(c) || c == '_'; }
// Digits, radix/exponent letters, suffixes and the decimal point: `0x1F`, `1.5e-3f`, `10u`.
bool IsNumberChar(unsigned char c) { return std::isalnum(c) || c == '.' || c == '_'; }

// One past the literal that starts at `start`. A sign directly after the exponent
// letter belongs to the literal (`1e-3`); in a hex literal `e` is a digit, so
// `0x1E-3` stays two operands.
std::size_t EndOfNumber(const std::string& text, std::size_t start)
{
    const std::size_t n = text.size();
    const bool hex = start + 1 < n && text[start] == '0' && (text[start + 1] == 'x' || text[start + 1] == 'X');
    std::size_t end = start + 1;
    while (end < n)
    {
        const unsigned char c = static_cast<unsigned char>(text[end]);
        if (IsNumberChar(c))
        {
            ++end;
            continue;
        }
        const bool exponentSign = !hex && (c == '+' || c == '-') && (text[end - 1] == 'e' || text[end - 1] == 'E') &&
                                  end + 1 < n && std::isdigit(static_cast<unsigned char>(text[end + 1]));
        if (!exponentSign)
            break;
        ++end;
    }
    return end;
}

std::size_t EndOfLine(const std::string& text, std::size_t from)
{
    const std::size_t nl = text.find('\n', from);
    return nl == std::string::npos ? text.size() : nl;
}

bool OnlyWhitespaceBeforeOnLine(const std::string& text, std::size_t pos)
{
    while (pos > 0)
    {
        const char c = text[pos - 1];
        if (c == '\n')
            return true;
        if (c != ' ' && c != '\t' && c != '\r')
            return false;
        --pos;
    }
    return true;
}

// A line comment as one token, or — when the language uses them — split around each
// `@tag` so the annotation reads as a declaration rather than prose.
void EmitLineComment(const std::string& text, std::size_t start, std::size_t end, bool annotations,
                     std::vector<SyntaxToken>& tokens)
{
    if (!annotations)
    {
        tokens.push_back({SyntaxTokenType::Comment, start, end - start});
        return;
    }
    std::size_t runStart = start;
    std::size_t i = start;
    while (i < end)
    {
        const bool tagStart = text[i] == '@' && i + 1 < end && IsWordStart(static_cast<unsigned char>(text[i + 1])) &&
                              (i == start || std::isspace(static_cast<unsigned char>(text[i - 1])));
        if (!tagStart)
        {
            ++i;
            continue;
        }
        if (i > runStart)
            tokens.push_back({SyntaxTokenType::Comment, runStart, i - runStart});
        std::size_t j = i + 1;
        while (j < end && IsWordChar(static_cast<unsigned char>(text[j])))
            ++j;
        tokens.push_back({SyntaxTokenType::Annotation, i, j - i});
        runStart = j;
        i = j;
    }
    if (end > runStart)
        tokens.push_back({SyntaxTokenType::Comment, runStart, end - runStart});
}

// Closing quote index + 1, honouring backslash escapes; the string ends at the line
// break (an unterminated literal must not swallow the rest of the file).
std::size_t EndOfQuoted(const std::string& text, std::size_t openQuote)
{
    const char quote = text[openQuote];
    std::size_t i = openQuote + 1;
    while (i < text.size())
    {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size())
        {
            i += 2;
            continue;
        }
        if (c == quote)
            return i + 1;
        if (c == '\n')
            return i;
        ++i;
    }
    return text.size();
}

} // namespace

ScriptLanguage ScriptLanguageForPath(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return GetAssetTypeFromExtension(ext) == AssetType::Shader ? ScriptLanguage::Glsl : ScriptLanguage::CSharp;
}

void TokenizeScript(ScriptLanguage language, const std::string& text, std::vector<SyntaxToken>& tokens)
{
    tokens.clear();
    const LanguageRules& rules = RulesFor(language);
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);

        if (c == '/' && i + 1 < n && text[i + 1] == '/')
        {
            const std::size_t end = EndOfLine(text, i);
            EmitLineComment(text, i, end, rules.CommentAnnotations, tokens);
            i = end;
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*')
        {
            const std::size_t close = text.find("*/", i + 2);
            const std::size_t end = close == std::string::npos ? n : close + 2;
            tokens.push_back({SyntaxTokenType::Comment, i, end - i});
            i = end;
            continue;
        }
        if (c == '#' && OnlyWhitespaceBeforeOnLine(text, i))
        {
            const std::size_t end = EndOfLine(text, i);
            tokens.push_back({SyntaxTokenType::Preprocessor, i, end - i});
            i = end;
            continue;
        }
        if (c == '"' || (c == '\'' && rules.CharLiterals))
        {
            const std::size_t end = EndOfQuoted(text, i);
            tokens.push_back({SyntaxTokenType::String, i, end - i});
            i = end;
            continue;
        }
        if (std::isspace(c))
        {
            ++i;
            continue;
        }
        if (std::isdigit(c) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(text[i + 1]))))
        {
            const std::size_t end = EndOfNumber(text, i);
            tokens.push_back({SyntaxTokenType::Number, i, end - i});
            i = end;
            continue;
        }
        if (IsWordStart(c))
        {
            std::size_t end = i + 1;
            while (end < n && IsWordChar(static_cast<unsigned char>(text[end])))
                ++end;
            const std::string_view word(text.data() + i, end - i);
            SyntaxTokenType type = SyntaxTokenType::Identifier;
            if (rules.Keywords->count(word))
                type = SyntaxTokenType::Keyword;
            else if (rules.Types->count(word))
                type = SyntaxTokenType::Type;
            tokens.push_back({type, i, end - i});
            i = end;
            continue;
        }
        if (std::ispunct(c))
        {
            tokens.push_back({SyntaxTokenType::Operator, i, 1});
            ++i;
            continue;
        }
        ++i;
    }
}

} // namespace GameEngine::Editor
