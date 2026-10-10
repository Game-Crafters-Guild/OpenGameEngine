// Declared properties in the composed shader: ShaderComposer replaces the
// adapters' `// GE_DECLARED_PROPERTIES` marker with the program's GE_Props struct,
// the Props accessor and GE_LoadDeclaredProperties(), which reads every packed
// lane (or a compile-time constant for an adapter read no producer stores). A
// declaration error is a compose failure whose lines carry file:line.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/Materials/ShaderPropertyTable.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "TestUtils.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

void WriteFile(const fs::path& p, const std::string& contents)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << contents;
}

struct TempMaterialDir
{
    fs::path Root;
    explicit TempMaterialDir(const char* prefix)
    {
        static std::atomic<uint32_t> counter{0};
        Root = fs::temp_directory_path() /
               (std::string(prefix) + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "_" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(Root);
    }
    ~TempMaterialDir()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
};

ComposedShaderSource ComposeSurface(const TempMaterialDir& dir, const std::string& surfaceSource,
                                    std::vector<std::string>& errors, const std::string& vertexModifier = {})
{
    WriteFile(dir.Root / "props_surface.glsl", surfaceSource);
    if (!vertexModifier.empty())
        WriteFile(dir.Root / "props_modifier.glsl", vertexModifier);

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "props_surface.glsl";
    if (!vertexModifier.empty())
        doc.vertexModifier = "props_modifier.glsl";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;
    if (!vertexModifier.empty())
        key.materialKeywords = key.materialKeywords | MaterialKeyword::HasVertexMod;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = Tests::GetAdapterShaderDir();
    return ShaderComposer::Compose(doc, key, dir.Root, ctx, &errors);
}

const char* kDeclaredSurface =
    "// @property color tint \"Tint\" default=1,0.55,0.2 hdr\n"
    "// @property float power default=2 range=0,8\n"
    "// @property vec2 scroll default=0,0.2\n"
    "// @property bool flag default=true\n"
    "// @property int count default=3\n"
    "// @property enum axis values=UV,WorldY,Radial default=WorldY\n"
    "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
    "    SurfaceOutput o = DefaultSurfaceOutput();\n"
    "    o.baseColor = Props.tint * Props.power * (Props.flag ? 1.0 : 0.5) * float(Props.count) * float(Props.axis);\n"
    "    o.baseColor.xy += Props.scroll;\n"
    "    return o;\n"
    "}\n";

} // namespace

TEST(ComposeDeclaredProperties, EmitsStructAccessorAndLaneReadsPerType)
{
    TempMaterialDir dir("ge_props_emit");
    std::vector<std::string> errors;
    const auto result = ComposeSurface(dir, kDeclaredSurface, errors);
    for (const auto& e : errors)
        ADD_FAILURE() << e;
    ASSERT_TRUE(result.IsValid());
    const std::string& fs = result.fragmentSource;

    EXPECT_NE(fs.find("struct GE_Props\n{\n"), std::string::npos);
    EXPECT_NE(fs.find("    vec3 tint;\n"), std::string::npos);
    EXPECT_NE(fs.find("    float power;\n"), std::string::npos);
    EXPECT_NE(fs.find("    vec2 scroll;\n"), std::string::npos);
    EXPECT_NE(fs.find("    bool flag;\n"), std::string::npos);
    EXPECT_NE(fs.find("    int count;\n"), std::string::npos);
    EXPECT_NE(fs.find("    int axis;\n"), std::string::npos);
    EXPECT_NE(fs.find("#define Props ge_Props\n"), std::string::npos);
    EXPECT_NE(fs.find("void GE_LoadDeclaredProperties()\n"), std::string::npos);

    // Greedy first-fit: tint .xyz of lane 0, power in its .w, scroll on lane 1 .xy,
    // flag .z, count .w, axis on lane 2 .x. Bool compares, int/enum bit-cast.
    EXPECT_NE(fs.find("ge_Props.tint = ge_MatData.uParams[0].xyz;"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.power = ge_MatData.uParams[0].w;"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.scroll = ge_MatData.uParams[1].xy;"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.flag = (ge_MatData.uParams[1].z != 0.0);"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.count = floatBitsToInt(ge_MatData.uParams[1].w);"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.axis = floatBitsToInt(ge_MatData.uParams[2].x);"), std::string::npos);

    // The marker is gone and the adapter's own lines stay addressable.
    EXPECT_EQ(fs.find("// GE_DECLARED_PROPERTIES\n"), std::string::npos);
    EXPECT_NE(fs.find("#line "), std::string::npos);

    ASSERT_NE(result.declaredProperties, nullptr);
    EXPECT_TRUE(result.declaredProperties->HasSurfaceDeclarations);
    EXPECT_NE(result.declaredProperties->Find("tint"), nullptr);
}

TEST(ComposeDeclaredProperties, AdapterReadsUndeclaredByTheSurfaceBecomeConstants)
{
    TempMaterialDir dir("ge_props_const");
    std::vector<std::string> errors;
    const auto result = ComposeSurface(dir, kDeclaredSurface, errors);
    ASSERT_TRUE(result.IsValid()) << (errors.empty() ? "" : errors.front());
    const std::string& fs = result.fragmentSource;
    EXPECT_NE(fs.find("ge_Props.alphaCutoff = 0.5;"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.specularIor = 1.5;"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.transmissionColor = vec3(1.0, 1.0, 1.0);"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.transmissionWeight = 0.0;"), std::string::npos);
    EXPECT_EQ(fs.find("ge_Props.alphaCutoff = ge_MatData"), std::string::npos);
}

TEST(ComposeDeclaredProperties, SurfaceRedeclaringAnAdapterReadSharesTheFirstLane)
{
    TempMaterialDir dir("ge_props_share");
    std::vector<std::string> errors;
    const auto result = ComposeSurface(dir,
                                       "// @property float alphaCutoff default=0.3\n"
                                       "// @property color tint\n"
                                       "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
                                       "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                                       "    o.baseColor = Props.tint; o.opacity = Props.alphaCutoff; return o; }\n",
                                       errors);
    ASSERT_TRUE(result.IsValid()) << (errors.empty() ? "" : errors.front());
    EXPECT_NE(result.fragmentSource.find("ge_Props.alphaCutoff = ge_MatData.uParams[0].x;"), std::string::npos);
    EXPECT_NE(result.fragmentSource.find("ge_Props.tint = ge_MatData.uParams[1].xyz;"), std::string::npos);
}

TEST(ComposeDeclaredProperties, UndeclaredSurfaceBindsAdapterReadsToTheirLegacyLanes)
{
    // A surface without declarations is still wired through the registry's
    // hand-typed table: alphaCutoff at uParams3.y, specularIor at uParams14.x,
    // transmission at uParams15 — lanes 4, 15 and 16 of the array.
    TempMaterialDir dir("ge_props_legacy");
    std::vector<std::string> errors;
    const auto result = ComposeSurface(dir,
                                       "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
                                       "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                                       "    o.baseColor = Mat.uBaseColor.rgb; o.metallic = Mat.uParams0.x; return o; }\n",
                                       errors);
    ASSERT_TRUE(result.IsValid()) << (errors.empty() ? "" : errors.front());
    const std::string& fs = result.fragmentSource;
    EXPECT_NE(fs.find("ge_Props.alphaCutoff = ge_MatData.uParams[4].y;"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.specularIor = ge_MatData.uParams[15].x;"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.transmissionColor = ge_MatData.uParams[16].xyz;"), std::string::npos);
    EXPECT_NE(fs.find("ge_Props.transmissionWeight = ge_MatData.uParams[16].w;"), std::string::npos);
    ASSERT_NE(result.declaredProperties, nullptr);
    EXPECT_FALSE(result.declaredProperties->HasSurfaceDeclarations);
}

TEST(ComposeDeclaredProperties, VertexModifierDeclarationsReachBothStages)
{
    TempMaterialDir dir("ge_props_vm");
    std::vector<std::string> errors;
    const auto result = ComposeSurface(dir,
                                       "// @property float a\n"
                                       "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
                                       "    SurfaceOutput o = DefaultSurfaceOutput(); o.metallic = Props.a; return o; }\n",
                                       errors,
                                       "// @property vec4 wind default=0,0,0,0\n"
                                       "vec3 ModifyVertex(vec3 p) { return p + Props.wind.xyz; }\n");
    ASSERT_TRUE(result.IsValid()) << (errors.empty() ? "" : errors.front());
    EXPECT_NE(result.fragmentSource.find("ge_Props.wind = ge_MatData.uParams[1];"), std::string::npos);
    EXPECT_NE(result.vertexSource.find("ge_Props.wind = ge_MatData.uParams[1];"), std::string::npos);
    EXPECT_NE(result.vertexSource.find("ge_Props.a = ge_MatData.uParams[0].x;"), std::string::npos);
}

// The unread-property warning reads the whole program: a surface that
// redeclares an adapter read (alphaCutoff, to expose the slider) never spells
// Props.alphaCutoff itself, and must not be told it is unread.
TEST(ComposeDeclaredProperties, ANameTheAdapterReadsIsNotWarnedAsUnread)
{
    Logger::Log::Initialize({});
    auto sink = Logger::MakeUnique<Logger::CallbackSink>();
    auto* sinkPtr = sink.get();
    struct Captured
    {
        std::mutex Mutex;
        std::vector<std::string> Lines;
    };
    auto captured = std::make_shared<Captured>();
    const Logger::uint64 callbackId = sinkPtr->RegisterCallback(
        [captured](const Logger::LogMessage& msg)
        {
            if (msg.Message.find("is never read") != Logger::String::npos)
            {
                std::lock_guard lock(captured->Mutex);
                captured->Lines.emplace_back(msg.Message.c_str());
            }
        });
    Logger::Log::AddSink(std::move(sink));

    TempMaterialDir dir("ge_props_unread");
    std::vector<std::string> errors;
    const auto result = ComposeSurface(dir,
                                       "// @property float alphaCutoff default=0.5 range=0,1\n"
                                       "// @property float leftover default=1\n"
                                       "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n",
                                       errors);
    // Delivery to sinks is asynchronous; Flush is the barrier.
    Logger::Log::Flush();
    sinkPtr->UnregisterCallback(callbackId);
    ASSERT_TRUE(result.IsValid()) << (errors.empty() ? "" : errors.front());

    std::lock_guard lock(captured->Mutex);
    const auto mentions = [&](const char* name)
    {
        return std::any_of(captured->Lines.begin(), captured->Lines.end(),
                           [&](const std::string& l) { return l.find(std::string("'") + name + "'") != std::string::npos; });
    };
    EXPECT_TRUE(mentions("leftover")) << "the instrument: a name nothing reads is warned";
    EXPECT_FALSE(mentions("alphaCutoff")) << "the adapter reads it";
}

TEST(ComposeDeclaredProperties, GrammarErrorFailsComposeWithFileAndLine)
{
    TempMaterialDir dir("ge_props_err");
    std::vector<std::string> errors;
    const auto result = ComposeSurface(dir,
                                       "// header\n"
                                       "// @property flaot roughness default=0.5\n"
                                       "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n",
                                       errors);
    EXPECT_FALSE(result.IsValid());
    ASSERT_FALSE(errors.empty());
    const std::string expected = (dir.Root / "props_surface.glsl").generic_string() + ":2: error: ";
    EXPECT_NE(errors.front().find(expected), std::string::npos) << errors.front();
    EXPECT_NE(errors.front().find("flaot"), std::string::npos);
}

TEST(ComposeDeclaredProperties, ConflictingTypeAcrossProducersFailsCompose)
{
    TempMaterialDir dir("ge_props_conflict");
    std::vector<std::string> errors;
    const auto result = ComposeSurface(dir,
                                       "// @property vec3 alphaCutoff\n"
                                       "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n",
                                       errors);
    EXPECT_FALSE(result.IsValid());
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("alphaCutoff"), std::string::npos);
    EXPECT_NE(errors.front().find("adapter_forward.glsl"), std::string::npos);
}

TEST(ComposeDeclaredProperties, BudgetOverflowFailsCompose)
{
    std::string src;
    for (int i = 0; i < 31; ++i)
        src += "// @property vec4 v" + std::to_string(i) + "\n";
    src += "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n";
    TempMaterialDir dir("ge_props_budget");
    std::vector<std::string> errors;
    const auto result = ComposeSurface(dir, src, errors);
    EXPECT_FALSE(result.IsValid());
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("120 floats"), std::string::npos) << errors.front();
}

TEST(ComposeDeclaredProperties, MatReadOfADeclaredNameFailsWithAFixIt)
{
    TempMaterialDir dir("ge_props_mat");
    std::vector<std::string> errors;
    const auto result = ComposeSurface(dir,
                                       "// @property color tint\n"
                                       "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
                                       "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                                       "    o.baseColor = Mat.tint; // wrong accessor\n"
                                       "    return o; }\n",
                                       errors);
    EXPECT_FALSE(result.IsValid());
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("props_surface.glsl:4: error:"), std::string::npos) << errors.front();
    EXPECT_NE(errors.front().find("Props.tint"), std::string::npos);
}

TEST(ComposeDeclaredProperties, DeclaredTypesCompileThroughShaderc)
{
    TempMaterialDir dir("ge_props_compile");
    const fs::path materialDir = dir.Root / "Materials";
    WriteFile(materialDir / "typed_surface.glsl", kDeclaredSurface);

    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TypedProps";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "typed_surface.glsl";
    doc.properties["count"] = static_cast<int32_t>(4);

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = Tests::GetAdapterShaderDir();
    ctx.CacheRoot = dir.Root / "Cache";
    const auto result = BuildMaterialToShaderPackage(doc, materialDir / "TypedProps.material", "TypedProps", ctx,
                                                     ShaderSourceKind::SpirV, MaterialKeyword::Instanced);
    for (const auto& e : result.errors)
        if (e.find("shaderc is not available") != std::string::npos)
            GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& e : result.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);
    ASSERT_EQ(result.package->meta.DeclaredProperties.size(), 10u) << "4 adapter reads + 6 surface declarations";
    const auto& props = result.package->meta.DeclaredProperties;
    const auto tint = std::find_if(props.begin(), props.end(), [](const ShaderProperty& p) { return p.Name == "tint"; });
    ASSERT_NE(tint, props.end());
    EXPECT_TRUE(tint->HasLane);
    EXPECT_EQ(tint->ByteOffset, 0u);
    EXPECT_TRUE(tint->Hdr);
}
