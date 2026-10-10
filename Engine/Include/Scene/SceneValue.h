#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Scene
{

enum class SceneValueKind
{
    Bool,
    Int,
    Float,
    String,
    Tuple,
    Array,
    FileRef,     // @"path"
    ResourceRef, // #id
    EntityRef,   // $id
    GuidRef,     // &{guid}
    AssetRef,    // [path="<p>" guid="<g>"] — either field may be omitted, at least one required
    Identifier   // bare identifier
};

struct SceneValue
{
    SceneValueKind Kind = SceneValueKind::Identifier;

    bool BoolValue = false;
    long long IntValue = 0;
    double FloatValue = 0.0;
    std::string StringValue;       // String / Identifier / FileRef / ResourceRef / EntityRef / GuidRef payload
    // Tuple / Array elements. AssetRef stores Items[0]=FileRef(path) and
    // Items[1]=GuidRef(guid) — consumers should use AssetRefPath /
    // AssetRefGuid below rather than indexing this directly.
    std::vector<SceneValue> Items;
};

// Accessors for SceneValueKind::AssetRef. Asset references are encoded as
// `[path="..." guid="..."]`; either field may be empty but at least one is
// non-empty (the parser rejects fully-empty AssetRefs). Both helpers return
// an empty string_view when the value is not an AssetRef so callers can
// branch on `Kind` once and read both fields without re-checking.
std::string_view AssetRefPath(const SceneValue& v);
std::string_view AssetRefGuid(const SceneValue& v);

struct Float3
{
    float X = 0, Y = 0, Z = 0;
};

struct Float4
{
    float X = 0, Y = 0, Z = 0, W = 1;
};

// True for canonical GUID text — 8-4-4-4-12 lowercase/uppercase hex, the form
// GUID::ToString emits. GUID's string constructor accepts anything and silently
// yields a NULL guid on garbage, so schema parsers gate on this first to reject a
// malformed reference instead of quietly clearing the field.
bool IsGuidText(std::string_view s);

// Why a decimal-integer token was refused. OutOfRange separates "this is not an integer" from
// "this is an integer the destination cannot hold" — different things to tell the user.
enum class IntegerTokenResult
{
    Ok,
    Malformed,
    OutOfRange
};

// The scene file's decimal-integer token grammar, in one place so every reader agrees on it:
// surrounding scene whitespace (space, tab, CR, LF), one optional leading '+' — or '-' for a
// signed destination — then decimal digits, to the end of the token and no further. Leading
// zeroes are accepted; hexadecimal spellings, exponents, digit separators and trailing text are
// not.
//
// Exact or nothing: `value` is written only on Ok, so a token that overflows T or breaks the
// grammar leaves the destination alone and the caller reports the assignment instead of storing a
// plausible wrong number. Parsing in T's own domain is what lets an unsigned 64-bit field keep its
// top bit, which a signed intermediate cannot represent.
//
// Defined for the eight fixed-width integer types.
template <class T>
IntegerTokenResult ParseIntegerToken(std::string_view text, T& value);

// Parse helpers for the INI-style scene schema.
bool ParseQuotedString(std::string_view s, std::string& out);
bool ParseFloat(std::string_view s, float& out);
bool ParseFloat3(std::string_view s, Float3& out); // "(x, y, z)"
bool ParseFloat4(std::string_view s, Float4& out); // "(x, y, z, w)"

// Parse a generic value of the scene schema:
// bool/number/string/tuple/array and references: @"path", #id, $id, &{guid}
bool ParseValue(std::string_view s, SceneValue& out, std::string* outError = nullptr);
bool ParseIdentifier(std::string_view s, std::string& out);

std::string FormatQuoted(std::string_view s);
// Shortest round-trip form (std::to_chars): exact on reload, unlike ostream's 6-significant-digit
// default. The one scalar float serializer the scene schemas share with FormatFloat2/3/4.
std::string FormatFloat(float value);
std::string FormatFloat2(float x, float y);
std::string FormatFloat3(float x, float y, float z);
std::string FormatFloat4(float x, float y, float z, float w);

// Format an asset reference. Either argument may be empty; at least one
// must be non-empty. Output forms:
//   [path="<p>" guid="<g>"]  — both present
//   [guid="<g>"]              — guid only
//   [path="<p>"]              — path only
std::string FormatAssetRef(std::string_view path, std::string_view guid);

} // namespace GameEngine::Scene

