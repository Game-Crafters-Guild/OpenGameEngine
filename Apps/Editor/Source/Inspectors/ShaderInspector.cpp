#include "Inspectors/ShaderInspector.h"
#include "AssetCore/SharedFileRead.h"

#include "InspectorRegistry.h"

#include "Assets/ShaderProgramAsset.h"
#include "Assets/ShaderSourceAsset.h"

#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderCapabilityDetector.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Materials/ShaderMetaValidation.h"
#include "Rendering/ShaderGraph/SgTagParser.h"

#include "Editor/Assets/ShaderGlslOpen.h"
#include "Editor/EditorPaths.h"
#include "ExternalScriptEditorLauncher.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Types/StringUtils.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>

namespace GameEngine {

using InspectorUI::AddTextBlock;
using InspectorUI::AddSelectableTextBlock;

namespace {

constexpr float kShaderInspectorSectionGapPx = 10.0f;

static void ConfigureShaderInspectorLayout(UIElement& root)
{
    root.Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::Gap, StyleLength::Px(kShaderInspectorSectionGapPx));
}

static const char* DescriptorTypeToString(uint32_t t)
{
    using GameEngine::Rendering::DescriptorType;
    switch (static_cast<DescriptorType>(t))
    {
    case DescriptorType::UniformBuffer: return "UniformBuffer";
    case DescriptorType::StorageBuffer: return "StorageBuffer";
    case DescriptorType::Texture: return "Texture";
    case DescriptorType::Sampler: return "Sampler";
    case DescriptorType::CombinedImageSampler: return "CombinedImageSampler";
    case DescriptorType::StorageImage: return "StorageImage";
    case DescriptorType::AccelerationStructure: return "AccelerationStructure";
    default: return "Unknown";
    }
}

static const char* BaseTypeToString(GameEngine::Rendering::BaseType b)
{
    using GameEngine::Rendering::BaseType;
    switch (b)
    {
    case BaseType::Float: return "float";
    case BaseType::Int: return "int";
    case BaseType::UInt: return "uint";
    case BaseType::Bool: return "bool";
    default: return "unknown";
    }
}

static std::string TypeToString(const GameEngine::Rendering::TypeDesc& t)
{
    using GameEngine::Rendering::TypeKind;

    std::ostringstream oss;
    switch (t.Kind)
    {
    case TypeKind::Scalar:
        oss << BaseTypeToString(t.Base);
        break;
    case TypeKind::Vector:
        oss << BaseTypeToString(t.Base) << t.VecSize;
        break;
    case TypeKind::Matrix:
        oss << BaseTypeToString(t.Base) << t.Cols << "x" << t.Rows;
        break;
    case TypeKind::Struct:
        oss << "struct";
        break;
    default:
        oss << "unknown";
        break;
    }
    for (uint32_t d : t.ArrayDims)
    {
        oss << "[";
        if (d != GameEngine::Rendering::kRuntimeArrayDim)
            oss << d;
        oss << "]";
    }
    return oss.str();
}

static std::string FormatShaderPkgSummary(const GameEngine::Rendering::ShaderPackage& pkg)
{
    using namespace GameEngine::Rendering;

    std::ostringstream oss;
    oss << "shaderpkgVersion=" << pkg.version << "\n";

    if (!pkg.stageBytes.empty())
    {
        oss << "\nStages (SPV bytes):\n";
        for (const auto& kv : pkg.stageBytes)
        {
            oss << " - " << kv.first << ": " << kv.second.size() << " bytes\n";
        }
    }

    const ShaderMeta& meta = pkg.meta;
    if (!meta.Stages.empty())
    {
        oss << "\nStage IO:\n";
        auto itVs = meta.Stages.find("vs");
        if (itVs != meta.Stages.end())
        {
            oss << "Vertex inputs:\n";
            for (const auto& io : itVs->second.Inputs)
            {
                oss << "  loc " << io.Location << " " << io.Name << " : " << TypeToString(io.Type) << "\n";
            }
        }
        auto itFs = meta.Stages.find("fs");
        if (itFs != meta.Stages.end())
        {
            oss << "Fragment outputs:\n";
            for (const auto& io : itFs->second.Outputs)
            {
                oss << "  loc " << io.Location << " " << io.Name << " : " << TypeToString(io.Type) << "\n";
            }
        }
    }

    if (!meta.PushConstants.empty())
    {
        oss << "\nPush constants:\n";
        for (const auto& pc : meta.PushConstants)
        {
            oss << " - " << pc.Name << " size=" << pc.Size << " stagesMask=0x" << std::hex << pc.StagesMask << std::dec << "\n";
        }
    }

    if (!meta.Sets.empty())
    {
        oss << "\nDescriptor sets:\n";
        for (const auto& s : meta.Sets)
        {
            oss << "Set " << s.Set << ":\n";
            for (const auto& b : s.Bindings)
            {
                oss << "  binding " << b.Binding << " " << b.Name
                    << " type=" << DescriptorTypeToString(b.Type)
                    << " count=" << b.Count
                    << " stagesMask=0x" << std::hex << b.StagesMask << std::dec << "\n";
            }
        }
    }

    auto report = ValidateShaderMeta(meta, 128);
    if (!report.Issues.empty())
    {
        oss << "\nValidation:\n";
        for (const auto& i : report.Issues)
        {
            const char* sev = (i.Severity == IssueSeverity::Error) ? "ERROR" : (i.Severity == IssueSeverity::Warning) ? "WARN" : "INFO";
            oss << " - [" << sev << "] " << i.Code << ": " << i.Message << "\n";
        }
    }

    return oss.str();
}

static void ClearChildren(UIElement* root)
{
    if (!root)
        return;
    root->RemoveAllChildren();
}

static std::string LoadShaderSourceText(ShaderSourceAsset* src)
{
    if (!src)
        return {};
    if (!src->GetSourceText().empty())
        return src->GetSourceText();

    GameEngine::String text;
    if (!GameEngine::ReadFileTextShared(src->GetPath(), text))
        return {};
    return text;
}

static void AddShaderOpenButtons(UIElement* root,
                                 const InspectorContext& ctx,
                                 const std::filesystem::path& shaderPath,
                                 bool isShaderGraph)
{
    auto buttonRow = std::make_unique<UIElement>();
    buttonRow->AddClass("inspector-asset-actions");
    buttonRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(6.0f))
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::AlignSelf, AlignItems::Stretch);

    if (ctx.OpenScript)
    {
        auto editBtn = std::make_unique<Button>();
        editBtn->SetText("Open in Text Editor");
        editBtn->AddClass("inspector-text");
        editBtn->AddClass("small");
        editBtn->Overrides().Set(Style::Width, StyleLength::Px(150.0f));
        editBtn->RegisterEventHandler(kEventButtonClick, [openScript = ctx.OpenScript, shaderPath](UIEvent&) { openScript(shaderPath); });
        buttonRow->AddChild(std::move(editBtn));
    }

    {
        auto ideBtn = std::make_unique<Button>();
        ideBtn->SetText("Open in IDE");
        ideBtn->AddClass("inspector-text");
        ideBtn->AddClass("small");
        ideBtn->Overrides().Set(Style::Width, StyleLength::Px(100.0f));
        ideBtn->RegisterEventHandler(kEventButtonClick, [shaderPath](UIEvent&) { ExternalScriptEditorLauncher::OpenScript(shaderPath); });
        buttonRow->AddChild(std::move(ideBtn));
    }

    if (ctx.OpenMaterialGraph)
    {
        auto graphBtn = std::make_unique<Button>();
        graphBtn->SetText("Open in Material Graph");
        graphBtn->AddClass("inspector-text");
        graphBtn->AddClass("small");
        graphBtn->Overrides().Set(Style::Width, StyleLength::Px(170.0f));
        if (!isShaderGraph)
            graphBtn->SetEnabled(false);
        graphBtn->RegisterEventHandler(kEventButtonClick, [openGraph = ctx.OpenMaterialGraph, shaderPath](UIEvent&)
                             { openGraph(shaderPath); });
        buttonRow->AddChild(std::move(graphBtn));
    }

    root->AddChild(std::move(buttonRow));
}

static void BuildShaderGraphInspector(UIElement* root,
                                      ShaderSourceAsset* src,
                                      const InspectorContext& ctx,
                                      const std::string& sourceText)
{
    const std::filesystem::path srcPath = src->GetPath();

    // Asset title is shown in the panel's fixed header strip (see ScriptInspector).
    AddTextBlock(root, srcPath.string(), "inspector-asset-path");
    AddShaderOpenButtons(root, ctx, srcPath, true);
    InspectorUI::AddSourceFileRow(root, srcPath);

    const auto parsed = ShaderGraph::ParseShaderGraphSource(sourceText);
    if (parsed.IsGraphFile)
    {
        const auto doc = ShaderGraph::ParseGraphDocumentFromTags(parsed.TagBlock);
        if (!doc.GraphName.empty())
        {
            std::ostringstream info;
            info << "Graph: " << doc.GraphName << "\n";
            info << "Nodes: " << doc.Nodes.size() << "  Edges: " << doc.Edges.size();
            AddTextBlock(root, info.str());
        }
    }
}

static const char* StageKeyToPretty(const std::string& stageKey)
{
    const std::string s = ToLowerAscii(stageKey);
    if (s == "vs") return "Vertex";
    if (s == "fs") return "Fragment";
    if (s == "cs") return "Compute";
    if (s == "gs") return "Geometry";
    if (s == "ms") return "Mesh";
    return "Unknown";
}

static bool InferStageFromExtension(const std::string& extLower, std::string& outStageKey)
{
    if (extLower == ".vert") { outStageKey = "vs"; return true; }
    if (extLower == ".frag") { outStageKey = "fs"; return true; }
    if (extLower == ".comp") { outStageKey = "cs"; return true; }
    if (extLower == ".geom") { outStageKey = "gs"; return true; }
    return false;
}

static std::vector<std::string> SplitDefinesCsv(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]()
    {
        std::string t = cur;
        // trim
        auto isws = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!t.empty() && isws((unsigned char)t.front())) t.erase(t.begin());
        while (!t.empty() && isws((unsigned char)t.back())) t.pop_back();
        if (!t.empty())
            out.push_back(t);
        cur.clear();
    };
    for (char c : text)
    {
        if (c == ',' || c == ';' || c == '\n')
        {
            flush();
            continue;
        }
        cur.push_back(c);
    }
    flush();
    return out;
}

static std::optional<std::string> GetMetaString(AssetRegistry& reg,
                                                const std::filesystem::path& assetPath,
                                                const char* key)
{
    std::string v;
    if (reg.TryGetMetaValue(assetPath, key, v))
    {
        return v;
    }
    return std::nullopt;
}

static void BuildCompositionShaderInspector(UIElement* root,
                                            ShaderSourceAsset* src,
                                            const InspectorContext& ctx,
                                            Rendering::ShaderCapability caps)
{
    using Rendering::ShaderCapability;
    using Rendering::HasCapability;

    const std::filesystem::path srcPath = src->GetPath();

    AddTextBlock(root, srcPath.string(), "inspector-asset-path");
    AddShaderOpenButtons(root, ctx, srcPath, false);
    InspectorUI::AddSourceFileRow(root, srcPath);

    std::ostringstream desc;
    desc << "This is a composition shader snippet. It is compiled as part of a material\n"
            "via ShaderComposer, not as a standalone shader.\n\n"
            "Detected capabilities:\n";

    if (HasCapability(caps, ShaderCapability::Surface))
        desc << "  - Surface Function (EvaluateSurface)\n";
    if (HasCapability(caps, ShaderCapability::VertexModifier))
        desc << "  - Vertex Modifier (ModifyVertex)\n";

    desc << "\nAssign this file to a material's shader slot to use it.";

    AddTextBlock(root, desc.str());
}

static void BuildShaderSourceInspector(UIElement* root, ShaderSourceAsset* src, const InspectorContext& ctx)
{
    if (!root || !src)
        return;

    const std::filesystem::path srcPath = src->GetPath();
    const std::string sourceText = LoadShaderSourceText(src);
    std::string ext = ToLowerAscii(srcPath.extension().string());

    if (ShaderGraph::IsShaderGraphSource(sourceText))
    {
        BuildShaderGraphInspector(root, src, ctx, sourceText);
        return;
    }

    const auto caps = Rendering::ShaderCapabilityDetector::Detect(sourceText);
    if (caps != Rendering::ShaderCapability::None)
    {
        BuildCompositionShaderInspector(root, src, ctx, caps);
        return;
    }

    AddTextBlock(root, srcPath.string(), "inspector-asset-path");
    AddShaderOpenButtons(root, ctx, srcPath, false);
    InspectorUI::AddSourceFileRow(root, srcPath);

    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    auto& registry = assetManager.GetRegistry();

    // Stage selection / inference
    std::string stageKey;
    const bool stageFromExt = InferStageFromExtension(ext, stageKey);
    if (!stageFromExt)
    {
        auto metaStage = GetMetaString(registry, srcPath, "shader_stage");
        stageKey = metaStage.has_value() ? ToLowerAscii(*metaStage) : "vs";
    }

    {
        auto l = std::make_unique<Label>();
        l->AddClass("inspector-text");
        l->SetText(std::string("Stage: ") + StageKeyToPretty(stageKey) + " (" + stageKey + ")");
        root->AddChild(std::move(l));
    }

    // Allow stage picking for .glsl and .hlsl (generic)
    const bool allowPickStage = (ext == ".glsl" || ext == ".hlsl");
    if (allowPickStage)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("inspector-row");
        UIElement* rowRaw = row.get();
        root->AddChild(std::move(row));

        auto requestRebuild = [root, src, ctx]()
        {
            ClearChildren(root);
            BuildShaderSourceInspector(root, src, ctx);
        };

        auto addStageButton = [&](const char* label, const char* key)
        {
            auto b = std::make_unique<Button>();
            b->AddClass("inspector-text");
            b->SetText(label);
            b->RegisterEventHandler(kEventButtonClick, [root, src, srcPath, key, &registry, ctx](UIEvent&)
                          {
                              registry.SetMetaValue(srcPath, "shader_stage", key);
                              if (UIElement::IsInEventDispatch())
                                  root->PostAction([root, src, ctx]()
                                                   { ClearChildren(root); BuildShaderSourceInspector(root, src, ctx); });
                              else
                              {
                                  ClearChildren(root);
                                  BuildShaderSourceInspector(root, src, ctx);
                              }
                          });
            rowRaw->AddChild(std::move(b));
        };

        addStageButton("Vertex", "vs");
        addStageButton("Fragment", "fs");
        addStageButton("Compute", "cs");
        addStageButton("Geometry", "gs");
    }

    // Entry point / defines (HLSL only for entry point; both can use defines)
    std::string entryPoint = "main";
    if (ext == ".hlsl")
    {
        auto metaEntry = GetMetaString(registry, srcPath, "shader_entry");
        entryPoint = metaEntry.has_value() && !metaEntry->empty() ? *metaEntry : "main";

        auto l = std::make_unique<Label>();
        l->AddClass("inspector-text");
        l->SetText("Entry point (HLSL)");
        root->AddChild(std::move(l));

        auto f = std::make_unique<TextField>();
        f->AddClass("inspector-text");
        f->SetValue(entryPoint);
        f->SetOnValueChanged([root, src, srcPath, &registry, ctx](const std::string& v)
                             {
                                 registry.SetMetaValue(srcPath, "shader_entry", v.empty() ? "main" : v);
                                 if (UIElement::IsInEventDispatch())
                                     root->PostAction([root, src, ctx]()
                                                      { ClearChildren(root); BuildShaderSourceInspector(root, src, ctx); });
                                 else
                                 {
                                     ClearChildren(root);
                                     BuildShaderSourceInspector(root, src, ctx);
                                 }
                             });
        root->AddChild(std::move(f));
    }
    else
    {
        // GLSL entry point is always main in our current compiler path.
        entryPoint = "main";
    }

    std::string definesCsv;
    {
        auto metaDefines = GetMetaString(registry, srcPath, "shader_defines");
        definesCsv = metaDefines.has_value() ? *metaDefines : std::string();
    }
    {
        auto l = std::make_unique<Label>();
        l->AddClass("inspector-text");
        l->SetText("Defines (comma-separated)");
        root->AddChild(std::move(l));

        auto f = std::make_unique<TextField>();
        f->AddClass("inspector-text");
        f->SetValue(definesCsv);
        f->SetOnValueChanged([root, src, srcPath, &registry, ctx](const std::string& v)
                             {
                                 registry.SetMetaValue(srcPath, "shader_defines", v);
                                 if (UIElement::IsInEventDispatch())
                                     root->PostAction([root, src, ctx]()
                                                      { ClearChildren(root); BuildShaderSourceInspector(root, src, ctx); });
                                 else
                                 {
                                     ClearChildren(root);
                                     BuildShaderSourceInspector(root, src, ctx);
                                 }
                             });
        root->AddChild(std::move(f));
    }

    // Compile + reflect (cache-backed)
    {
        Rendering::ShaderProgramCompileRequest creq{};
        creq.debugName = src->GetName() + std::string("_") + stageKey;

        const auto p = GameEngine::Editor::GetCurrentEditorProjectPaths();
        if (!p.projectCacheRoot.empty())
        {
            creq.cacheRoot = (p.projectCacheRoot / "Shaders").lexically_normal();
        }
        else
        {
            // Fallback: derive from asset root when workspace is unavailable (tests/tools).
            const std::filesystem::path workspaceRoot = assetManager.GetAssetRoot().parent_path();
            creq.cacheRoot = (workspaceRoot / ".Cache" / "Shaders").lexically_normal();
        }

        creq.baseDirectory = srcPath.parent_path();
        creq.includeDirs = {};

        Rendering::ShaderStageCompileSpec s{};
        s.stage = stageKey;
        s.sourcePath = srcPath; // absolute ok
        s.entryPoint = entryPoint;
        s.defines = SplitDefinesCsv(definesCsv);
        creq.stages.push_back(std::move(s));

        Rendering::ShaderProgramCompileResult cres{};
        std::string err;
        // SPIR-V: the result is summarised for the inspector (reflection, byte
        // counts) and never fed to pipeline creation.
        if (!Rendering::ShaderCompileService::CompileProgramToCache(
                creq, Rendering::ShaderSourceKind::SpirV, cres, &err))
        {
            // Ensure the user sees useful diagnostics even when shaderc returns an empty message.
            std::string shown = err;
            auto isws = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!shown.empty() && isws((unsigned char)shown.front())) shown.erase(shown.begin());
            while (!shown.empty() && isws((unsigned char)shown.back())) shown.pop_back();
            if (shown.empty())
            {
                shown = "No diagnostics provided.\n"
                        "Source: " + srcPath.string() + "\n"
                        "Stage: " + stageKey + "\n"
                        "Entry: " + entryPoint + "\n"
                        "Defines: " + definesCsv;
            }
            AddSelectableTextBlock(root, "Compile/reflect failed:\n" + shown);
        }
        else
        {
            AddTextBlock(root, "Generated shaderpkg:\n" + cres.shaderPkgPath.string());
            GameEngine::Rendering::ShaderPackage pkg{};
            pkg.version = 1;
            pkg.meta = cres.meta;
            pkg.stageBytes = cres.stageBytes;
            pkg.cacheInfoJson = cres.cacheInfoJson;
            AddTextBlock(root, FormatShaderPkgSummary(pkg));
        }
    }
}

} // namespace

void RegisterShaderInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.Object)
            return;

        auto* base = static_cast<Asset*>(ctx.Object);

        // Shader Program (.shader)
        if (auto* prog = dynamic_cast<ShaderProgramAsset*>(base))
        {
            auto header = std::make_unique<Label>();
            header->AddClass("inspector-header");
            header->SetText("Shader Program: " + prog->GetName());
            ctx.Parent->AddChild(std::move(header));

            AddTextBlock(ctx.Parent, prog->GetPath().string());

            if (!prog->GetErrors().empty())
            {
                std::ostringstream oss;
                oss << "Errors:\n";
                for (const auto& e : prog->GetErrors())
                {
                    oss << " - " << e << "\n";
                }
                AddSelectableTextBlock(ctx.Parent, oss.str());
            }

            if (!prog->GetGeneratedShaderPkgPath().empty())
            {
                AddTextBlock(ctx.Parent, "Generated shaderpkg:\n" + prog->GetGeneratedShaderPkgPath());
            }

            if (const auto* pkg = prog->GetPackage())
            {
                AddTextBlock(ctx.Parent, FormatShaderPkgSummary(*pkg));
            }
            return;
        }

        // Shader source text (.glsl/.hlsl/.vert/.frag/.comp)
        if (auto* src = dynamic_cast<ShaderSourceAsset*>(base))
        {
            auto container = std::make_unique<UIElement>();
            UIElement* root = container.get();
            ConfigureShaderInspectorLayout(*container);
            ctx.Parent->AddChild(std::move(container));
            BuildShaderSourceInspector(root, src, ctx);
            return;
        }

        // Fallback
        {
            auto header = std::make_unique<Label>();
            header->AddClass("inspector-header");
            header->SetText("Shader: " + base->GetName());
            ctx.Parent->AddChild(std::move(header));

            AddTextBlock(ctx.Parent, base->GetPath().string());
        }
    };

    InspectorRegistry::Get().RegisterAssetInspector(AssetType::Shader, std::move(fn));
}

} // namespace GameEngine

