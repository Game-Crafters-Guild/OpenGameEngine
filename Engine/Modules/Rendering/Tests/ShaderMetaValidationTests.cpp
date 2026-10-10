#include <gtest/gtest.h>
#include "Rendering/Materials/ShaderMetaValidation.h"

using namespace GameEngine::Rendering;

TEST(ShaderMetaValidation, PushConstantLimitExceededIsError) {
    ShaderMeta m{};
    {
        PushConstantRangeMeta pc{};
        pc.Name = "A";
        pc.Size = 96;
        pc.StagesMask = 0;
        pc.Block = {};
        m.PushConstants.push_back(pc);
    }
    {
        PushConstantRangeMeta pc{};
        pc.Name = "B";
        pc.Size = 64;
        pc.StagesMask = 0;
        pc.Block = {};
        m.PushConstants.push_back(pc);
    }
    auto rep = ValidateShaderMeta(m, 128);
    EXPECT_TRUE(rep.HasErrors());
    bool found=false; for (auto& i : rep.Issues) if (i.Code=="PushConstantsTotalBytesExceedLimit") found=true; EXPECT_TRUE(found);
}

namespace {

StageIO MakeIO(uint32_t location, uint32_t component, uint32_t vecSize, const char* name) {
    StageIO io{};
    io.Location = location;
    io.Component = component;
    io.Name = name;
    io.Type.Kind = vecSize > 1 ? TypeKind::Vector : TypeKind::Scalar;
    io.Type.Base = BaseType::UInt;
    io.Type.VecSize = vecSize;
    return io;
}

bool HasDuplicateStageIOIssue(const ValidationReport& rep) {
    for (const auto& i : rep.Issues)
        if (i.Code == "DuplicateStageIOLocation")
            return true;
    return false;
}

} // namespace

// The LOD-crossfade interface shape: two flat uints packed into one location.
// Legal SPIR-V — the interface slot is (location, component).
TEST(ShaderMetaValidation, DisjointComponentsOnOneLocationAreNotDuplicates) {
    ShaderMeta m{};
    StageMeta vs{};
    vs.Outputs.push_back(MakeIO(14, 0, 1, "vInstanceFlags"));
    vs.Outputs.push_back(MakeIO(14, 1, 1, "vLodFadeCode"));
    m.Stages["vs"] = vs;
    StageMeta fs{};
    fs.Inputs.push_back(MakeIO(14, 0, 1, "vInstanceFlags"));
    fs.Inputs.push_back(MakeIO(14, 1, 1, "vLodFadeCode"));
    m.Stages["fs"] = fs;

    const auto rep = ValidateShaderMeta(m);
    EXPECT_FALSE(HasDuplicateStageIOIssue(rep));
    EXPECT_FALSE(rep.HasErrors());
}

TEST(ShaderMetaValidation, SameLocationAndComponentIsStillDuplicate) {
    ShaderMeta m{};
    StageMeta fs{};
    fs.Inputs.push_back(MakeIO(14, 0, 1, "a"));
    fs.Inputs.push_back(MakeIO(14, 0, 1, "b"));
    m.Stages["fs"] = fs;

    const auto rep = ValidateShaderMeta(m);
    EXPECT_TRUE(HasDuplicateStageIOIssue(rep));
}

// A wide variable covers the components a narrow one would sit in, so packing
// behind it overlaps even though the component indices differ.
TEST(ShaderMetaValidation, OverlappingComponentSpansAreDuplicates) {
    ShaderMeta m{};
    StageMeta vs{};
    vs.Outputs.push_back(MakeIO(3, 0, 3, "vNormalWS"));
    vs.Outputs.push_back(MakeIO(3, 1, 1, "vPacked"));
    m.Stages["vs"] = vs;

    const auto rep = ValidateShaderMeta(m);
    EXPECT_TRUE(HasDuplicateStageIOIssue(rep));
}

TEST(ShaderMetaValidation, DuplicateSpecConstantIdIsError) {
    ShaderMeta m{};
    m.SpecConstants.push_back(SpecConstantMeta{ 5, "SC5", TypeDesc{}, std::nullopt });
    m.SpecConstants.push_back(SpecConstantMeta{ 5, "SC5_dup", TypeDesc{}, std::nullopt });
    auto rep = ValidateShaderMeta(m);
    EXPECT_TRUE(rep.HasErrors());
    bool found=false; for (auto& i : rep.Issues) if (i.Code=="DuplicateSpecConstantId") found=true; EXPECT_TRUE(found);
}

