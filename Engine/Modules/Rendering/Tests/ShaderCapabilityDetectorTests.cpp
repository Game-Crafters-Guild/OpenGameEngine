// Tests for ShaderCapabilityDetector: scanning GLSL source for composition hooks.

#include <gtest/gtest.h>

#include "Rendering/Materials/ShaderCapabilityDetector.h"

using namespace GameEngine::Rendering;

TEST(ShaderCapabilityDetectorTest, DetectsNothing_EmptySource)
{
    auto caps = ShaderCapabilityDetector::Detect("");
    EXPECT_EQ(caps, ShaderCapability::None);
}

TEST(ShaderCapabilityDetectorTest, DetectsNothing_StandaloneShader)
{
    const std::string src =
        "#version 450\n"
        "layout(location = 0) out vec4 oColor;\n"
        "void main() { oColor = vec4(1.0); }\n";

    auto caps = ShaderCapabilityDetector::Detect(src);
    EXPECT_EQ(caps, ShaderCapability::None);
}

TEST(ShaderCapabilityDetectorTest, DetectsSurface)
{
    const std::string src =
        "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
        "{\n"
        "    SurfaceOutput o = DefaultSurfaceOutput();\n"
        "    o.baseColor = vec3(1.0);\n"
        "    return o;\n"
        "}\n";

    auto caps = ShaderCapabilityDetector::Detect(src);
    EXPECT_TRUE(HasCapability(caps, ShaderCapability::Surface));
    EXPECT_FALSE(HasCapability(caps, ShaderCapability::VertexModifier));
}

TEST(ShaderCapabilityDetectorTest, DetectsVertexModifier)
{
    const std::string src =
        "vec3 ModifyVertex(vec3 position, InstanceData inst)\n"
        "{\n"
        "    return position + vec3(0.0, sin(inst.instanceIndex), 0.0);\n"
        "}\n";

    auto caps = ShaderCapabilityDetector::Detect(src);
    EXPECT_FALSE(HasCapability(caps, ShaderCapability::Surface));
    EXPECT_TRUE(HasCapability(caps, ShaderCapability::VertexModifier));
}

TEST(ShaderCapabilityDetectorTest, DetectsBothCapabilities)
{
    const std::string src =
        "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
        "{\n"
        "    SurfaceOutput o = DefaultSurfaceOutput();\n"
        "    o.baseColor = Mat.uBaseColor.rgb;\n"
        "    return o;\n"
        "}\n"
        "\n"
        "vec3 ModifyVertex(vec3 position, InstanceData inst)\n"
        "{\n"
        "    return position;\n"
        "}\n";

    auto caps = ShaderCapabilityDetector::Detect(src);
    EXPECT_TRUE(HasCapability(caps, ShaderCapability::Surface));
    EXPECT_TRUE(HasCapability(caps, ShaderCapability::VertexModifier));
}

TEST(ShaderCapabilityDetectorTest, CommentOnlyOutputSignatureIsSimpleModifier)
{
    const std::string src =
        "//      void ModifyVertex(inout VertexOutput v, InstanceData inst);\n"
        "vec3 ModifyVertex(vec3 position, InstanceData inst)\n"
        "{\n"
        "    return position;\n"
        "}\n";

    auto caps = ShaderCapabilityDetector::Detect(src);
    EXPECT_TRUE(HasCapability(caps, ShaderCapability::VertexModifier));
    EXPECT_FALSE(HasCapability(caps, ShaderCapability::VertexOutputModifier));
}

TEST(ShaderCapabilityDetectorTest, DoesNotMatchWithoutParen)
{
    const std::string src =
        "// This shader mentions EvaluateSurface and ModifyVertex in comments\n"
        "// but never has the opening paren immediately after the name.\n"
        "void main() {}\n";

    auto caps = ShaderCapabilityDetector::Detect(src);
    EXPECT_EQ(caps, ShaderCapability::None);
}

TEST(ShaderCapabilityDetectorTest, MatchesWithWhitespaceBeforeParen)
{
    // The detector looks for "EvaluateSurface(" — no whitespace tolerance needed
    // since our convention uses no space before the paren. This test verifies that
    // a space before the paren does NOT false-positive match.
    const std::string src =
        "SurfaceOutput EvaluateSurface (SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n";

    auto caps = ShaderCapabilityDetector::Detect(src);
    // "EvaluateSurface (" has a space — the detector looks for "EvaluateSurface(" which
    // is not present, so this should NOT match.
    EXPECT_FALSE(HasCapability(caps, ShaderCapability::Surface));
}
