// Decider parity (transparency-scale Risk 3 / review F7). Locks the case table of
// IsOrderDependentBlendMaterial — the single predicate that decides sorted-set
// membership in all three places (extraction class bit, collection filter, opaque
// batch peel). A one-case drift here double-draws or vanishes an instance, the
// exact T2 burn class, so the table is pinned by a test.

#include <gtest/gtest.h>

#include "Engine/Rendering/Material.h"

#include "Rendering/Materials/MaterialBlend.h"

#include <optional>

using namespace GameEngine;
namespace R = GameEngine::Engine::Renderer;

namespace
{
bool Decide(MaterialAlphaMode mode, const std::optional<MaterialBlendState>& blend)
{
    R::Material mat = R::Material::TestFactory::Create(GUID{}, "decider", 256);
    R::Material::TestFactory::SetAlphaMode(mat, mode);
    R::Material::TestFactory::SetBlendState(mat, blend);
    return R::IsOrderDependentBlendMaterial(mat);
}

MaterialBlendState Bs(MaterialBlendFactor src, MaterialBlendFactor dst,
                      MaterialBlendOp op = MaterialBlendOp::Add)
{
    MaterialBlendState b{}; // alpha fields keep defaults; the predicate reads only color op/factors
    b.SrcColorFactor = src;
    b.DstColorFactor = dst;
    b.ColorOp = op;
    return b;
}
} // namespace

// Order-dependent Blend => PEELED into the sorted pass (true).
TEST(SortedTransparentDecider, OrderDependentBlendIsPeeled)
{
    EXPECT_TRUE(Decide(MaterialAlphaMode::Blend, std::nullopt));         // unauthored = default straight-alpha
    EXPECT_TRUE(Decide(MaterialAlphaMode::Blend, MaterialBlendState{})); // explicit default straight-alpha
    // premultiplied over premultiplied (One, 1-SrcAlpha, Add) is still order-dependent.
    EXPECT_TRUE(Decide(MaterialAlphaMode::Blend,
                       Bs(MaterialBlendFactor::One, MaterialBlendFactor::OneMinusSrcAlpha)));
    // Subtract / ReverseSubtract are order-dependent (not commutative).
    EXPECT_TRUE(Decide(MaterialAlphaMode::Blend,
                       Bs(MaterialBlendFactor::SrcAlpha, MaterialBlendFactor::OneMinusSrcAlpha,
                          MaterialBlendOp::Subtract)));
    EXPECT_TRUE(Decide(MaterialAlphaMode::Blend,
                       Bs(MaterialBlendFactor::SrcAlpha, MaterialBlendFactor::OneMinusSrcAlpha,
                          MaterialBlendOp::ReverseSubtract)));
}

// Commutative Blend equations => stay on the batched path (false).
TEST(SortedTransparentDecider, CommutativeBlendIsExcluded)
{
    EXPECT_FALSE(Decide(MaterialAlphaMode::Blend,
                        Bs(MaterialBlendFactor::One, MaterialBlendFactor::One))); // additive
    EXPECT_FALSE(Decide(MaterialAlphaMode::Blend,
                        Bs(MaterialBlendFactor::DstColor, MaterialBlendFactor::Zero))); // multiply (src)
    EXPECT_FALSE(Decide(MaterialAlphaMode::Blend,
                        Bs(MaterialBlendFactor::Zero, MaterialBlendFactor::SrcColor))); // multiply (dst)
    EXPECT_FALSE(Decide(MaterialAlphaMode::Blend,
                        Bs(MaterialBlendFactor::SrcAlpha, MaterialBlendFactor::OneMinusSrcAlpha,
                           MaterialBlendOp::Min)));
    EXPECT_FALSE(Decide(MaterialAlphaMode::Blend,
                        Bs(MaterialBlendFactor::SrcAlpha, MaterialBlendFactor::OneMinusSrcAlpha,
                           MaterialBlendOp::Max)));
}

// Non-Blend alpha modes are opaque-class regardless of any (ignored) blend state.
// Transmission is coerced to Opaque before m_AlphaMode is set (MaterialRegistry),
// so it lands here and is never peeled.
TEST(SortedTransparentDecider, NonBlendAlphaModesNeverPeeled)
{
    EXPECT_FALSE(Decide(MaterialAlphaMode::Opaque, std::nullopt));
    EXPECT_FALSE(Decide(MaterialAlphaMode::Mask, std::nullopt));
    EXPECT_FALSE(Decide(MaterialAlphaMode::Opaque, MaterialBlendState{}));
    EXPECT_FALSE(Decide(MaterialAlphaMode::Mask,
                        Bs(MaterialBlendFactor::One, MaterialBlendFactor::OneMinusSrcAlpha)));
}
