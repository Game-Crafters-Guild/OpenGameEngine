#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace GameEngine
{

// Authoring-facing blend factors and operators. These mirror the GPU-side
// Rendering::BlendFactor / Rendering::BlendOp (Core/Device.h) value-for-value,
// but live in the material/asset layer so the .material parser never pulls the
// heavy device abstraction. They are mapped to the device enums at PSO build
// (DeriveMaterialBlendState, MaterialBlendDerive.h), which static_asserts the
// value correspondence so any drift is a compile error.
enum class MaterialBlendFactor : uint8_t
{
    Zero = 0,
    One = 1,
    SrcColor = 2,
    OneMinusSrcColor = 3,
    DstColor = 4,
    OneMinusDstColor = 5,
    SrcAlpha = 6,
    OneMinusSrcAlpha = 7,
    DstAlpha = 8,
    OneMinusDstAlpha = 9,
    ConstantColor = 10,
    OneMinusConstantColor = 11,
    ConstantAlpha = 12,
    OneMinusConstantAlpha = 13,
    AlphaSaturate = 14
};

enum class MaterialBlendOp : uint8_t
{
    Add = 0,
    Subtract = 1,
    ReverseSubtract = 2,
    Min = 3,
    Max = 4
};

// Explicit blend-state authoring (T1, "full blend controls"). Every field
// defaults to the engine's historical hardwired Blend equation, so a
// default-constructed MaterialBlendState reproduces pre-T1 behavior exactly:
// straight-alpha color (SrcAlpha, 1-SrcAlpha, Add) with premultiplied-style
// alpha accumulation (One, 1-SrcAlpha, Add). This is why an existing Blend
// material that authors no `blend` block stays byte-identical.
//
// Order-dependence caveat (see amendment 11): additive (One,One) and multiply
// (DstColor,Zero) are commutative and therefore order-independent — correct even
// without a depth sort. The default straight-alpha and any premultiplied-over-
// premultiplied blend are order-DEPENDENT and only correct over opaque geometry
// until the sorted transparent pass (T2) lands. T1 alone does not claim general
// transparency correctness.
struct MaterialBlendState
{
    MaterialBlendFactor SrcColorFactor = MaterialBlendFactor::SrcAlpha;
    MaterialBlendFactor DstColorFactor = MaterialBlendFactor::OneMinusSrcAlpha;
    MaterialBlendOp ColorOp = MaterialBlendOp::Add;
    MaterialBlendFactor SrcAlphaFactor = MaterialBlendFactor::One;
    MaterialBlendFactor DstAlphaFactor = MaterialBlendFactor::OneMinusSrcAlpha;
    MaterialBlendOp AlphaOp = MaterialBlendOp::Add;

    bool operator==(const MaterialBlendState&) const = default;
};

// True when the blend equation's result depends on draw order — the sorted
// transparent pass (T2) peels ONLY these. Additive (One,One,Add), multiply
// (DstColor,Zero,Add / Zero,SrcColor,Add), and Min/Max are commutative in the
// destination and stay on the instanced batched path: sorting them wastes the
// per-draw budget and the 256 ceiling for zero visual gain. Unauthored (nullopt)
// is the default straight-alpha equation — order-dependent. Anything not
// provably commutative is treated as order-dependent (conservative). The peel
// predicate and the sort collection MUST both use this function — a disagreement
// makes peeled draws vanish.
inline bool IsOrderDependentBlendState(const std::optional<MaterialBlendState>& authored)
{
    if (!authored.has_value())
        return true; // default straight-alpha
    const MaterialBlendState& b = *authored;
    if (b.ColorOp == MaterialBlendOp::Min || b.ColorOp == MaterialBlendOp::Max)
        return false;
    if (b.ColorOp != MaterialBlendOp::Add)
        return true; // Subtract/ReverseSubtract are order-dependent
    const bool additive = b.SrcColorFactor == MaterialBlendFactor::One &&
                          b.DstColorFactor == MaterialBlendFactor::One;
    const bool multiplySrc = b.SrcColorFactor == MaterialBlendFactor::DstColor &&
                             b.DstColorFactor == MaterialBlendFactor::Zero;
    const bool multiplyDst = b.SrcColorFactor == MaterialBlendFactor::Zero &&
                             b.DstColorFactor == MaterialBlendFactor::SrcColor;
    return !(additive || multiplySrc || multiplyDst);
}

inline std::string_view MaterialBlendFactorToString(MaterialBlendFactor f)
{
    switch (f)
    {
    case MaterialBlendFactor::Zero: return "Zero";
    case MaterialBlendFactor::One: return "One";
    case MaterialBlendFactor::SrcColor: return "SrcColor";
    case MaterialBlendFactor::OneMinusSrcColor: return "OneMinusSrcColor";
    case MaterialBlendFactor::DstColor: return "DstColor";
    case MaterialBlendFactor::OneMinusDstColor: return "OneMinusDstColor";
    case MaterialBlendFactor::SrcAlpha: return "SrcAlpha";
    case MaterialBlendFactor::OneMinusSrcAlpha: return "OneMinusSrcAlpha";
    case MaterialBlendFactor::DstAlpha: return "DstAlpha";
    case MaterialBlendFactor::OneMinusDstAlpha: return "OneMinusDstAlpha";
    case MaterialBlendFactor::ConstantColor: return "ConstantColor";
    case MaterialBlendFactor::OneMinusConstantColor: return "OneMinusConstantColor";
    case MaterialBlendFactor::ConstantAlpha: return "ConstantAlpha";
    case MaterialBlendFactor::OneMinusConstantAlpha: return "OneMinusConstantAlpha";
    case MaterialBlendFactor::AlphaSaturate: return "AlphaSaturate";
    }
    return "SrcAlpha";
}

inline std::optional<MaterialBlendFactor> MaterialBlendFactorFromString(std::string_view s)
{
    if (s == "Zero") return MaterialBlendFactor::Zero;
    if (s == "One") return MaterialBlendFactor::One;
    if (s == "SrcColor") return MaterialBlendFactor::SrcColor;
    if (s == "OneMinusSrcColor") return MaterialBlendFactor::OneMinusSrcColor;
    if (s == "DstColor") return MaterialBlendFactor::DstColor;
    if (s == "OneMinusDstColor") return MaterialBlendFactor::OneMinusDstColor;
    if (s == "SrcAlpha") return MaterialBlendFactor::SrcAlpha;
    if (s == "OneMinusSrcAlpha") return MaterialBlendFactor::OneMinusSrcAlpha;
    if (s == "DstAlpha") return MaterialBlendFactor::DstAlpha;
    if (s == "OneMinusDstAlpha") return MaterialBlendFactor::OneMinusDstAlpha;
    if (s == "ConstantColor") return MaterialBlendFactor::ConstantColor;
    if (s == "OneMinusConstantColor") return MaterialBlendFactor::OneMinusConstantColor;
    if (s == "ConstantAlpha") return MaterialBlendFactor::ConstantAlpha;
    if (s == "OneMinusConstantAlpha") return MaterialBlendFactor::OneMinusConstantAlpha;
    if (s == "AlphaSaturate") return MaterialBlendFactor::AlphaSaturate;
    return std::nullopt;
}

inline std::string_view MaterialBlendOpToString(MaterialBlendOp op)
{
    switch (op)
    {
    case MaterialBlendOp::Add: return "Add";
    case MaterialBlendOp::Subtract: return "Subtract";
    case MaterialBlendOp::ReverseSubtract: return "ReverseSubtract";
    case MaterialBlendOp::Min: return "Min";
    case MaterialBlendOp::Max: return "Max";
    }
    return "Add";
}

inline std::optional<MaterialBlendOp> MaterialBlendOpFromString(std::string_view s)
{
    if (s == "Add") return MaterialBlendOp::Add;
    if (s == "Subtract") return MaterialBlendOp::Subtract;
    if (s == "ReverseSubtract") return MaterialBlendOp::ReverseSubtract;
    if (s == "Min") return MaterialBlendOp::Min;
    if (s == "Max") return MaterialBlendOp::Max;
    return std::nullopt;
}

} // namespace GameEngine
