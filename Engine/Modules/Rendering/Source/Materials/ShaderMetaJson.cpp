#include "Rendering/Materials/ShaderMetaJson.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace Rendering
{

using nlohmann::json;

// TypeDesc (declarations are provided in ShaderMetaJson.h)

void to_json(json& j, const Member& m)
{
    j = json::object();
    j["name"] = m.Name;
    j["type"] = m.Type;
    j["offset"] = m.Offset;
    j["size"] = m.Size;
    if (m.ArrayStride)
        j["arrayStride"] = *m.ArrayStride;
    if (m.MatrixStride)
        j["matrixStride"] = *m.MatrixStride;
    if (m.RowMajor)
        j["rowMajor"] = true;
}
void from_json(const json& j, Member& m)
{
    j.at("name").get_to(m.Name);
    j.at("type").get_to(m.Type);
    j.at("offset").get_to(m.Offset);
    j.at("size").get_to(m.Size);
    if (j.contains("arrayStride"))
        m.ArrayStride = j.at("arrayStride").get<uint32_t>();
    if (j.contains("matrixStride"))
        m.MatrixStride = j.at("matrixStride").get<uint32_t>();
    m.RowMajor = j.value("rowMajor", false);
}

void to_json(json& j, const TypeDesc& t)
{
    j = json::object();
    j["kind"] = static_cast<uint32_t>(t.Kind);
    j["base"] = static_cast<uint32_t>(t.Base);
    j["rows"] = t.Rows;
    j["cols"] = t.Cols;
    j["vecSize"] = t.VecSize;
    j["arrayDims"] = t.ArrayDims;
    if (!t.StructMembers.empty())
        j["structMembers"] = t.StructMembers;
}
void from_json(const json& j, TypeDesc& t)
{
    t.Kind = static_cast<TypeKind>(j.value("kind", 0u));
    t.Base = static_cast<BaseType>(j.value("base", 0u));
    t.Rows = j.value("rows", 1u);
    t.Cols = j.value("cols", 1u);
    t.VecSize = j.value("vecSize", 1u);
    t.ArrayDims = j.value("arrayDims", std::vector<uint32_t>{});
    t.StructMembers = j.value("structMembers", std::vector<Member>{});
}

void to_json(json& j, const BlockLayout& b)
{
    j = json::object();
    j["size"] = b.Size;
    j["members"] = b.Members;
}
void from_json(const json& j, BlockLayout& b)
{
    b.Size = j.value("size", 0u);
    b.Members = j.value("members", std::vector<Member>{});
}

void to_json(json& j, const PushConstantRangeMeta& p)
{
    j = json::object();
    j["id"] = p.Id;
    j["name"] = p.Name;
    j["size"] = p.Size;
    j["stagesMask"] = p.StagesMask;
    j["block"] = p.Block;
}
void from_json(const json& j, PushConstantRangeMeta& p)
{
    p.Id = j.value("id", 0u);
    j.at("name").get_to(p.Name);
    p.Size = j.value("size", 0u);
    p.StagesMask = j.value("stagesMask", 0u);
    p.Block = j.value("block", BlockLayout{});
}

void to_json(json& j, const DescriptorBindingMeta& b)
{
    j = json::object();
    j["binding"] = b.Binding;
    j["name"] = b.Name;
    j["type"] = b.Type;
    j["count"] = b.Count;
    j["stagesMask"] = b.StagesMask;
    if (b.Block)
        j["block"] = *b.Block;
    if (b.ReadOnly)
        j["readOnly"] = true;
    if (b.Image)
    {
        json imgJson = json::object();
        imgJson["dim"] = b.Image->Dim;
        imgJson["arrayed"] = b.Image->Arrayed;
        imgJson["multisample"] = b.Image->Multisample;
        if (b.Image->Depth)
            imgJson["depth"] = true;
        if (b.Image->Cube)
            imgJson["cube"] = true;
        if (b.Image->Filtered)
            imgJson["filtered"] = true;
        if (b.Image->UnsignedInteger)
            imgJson["unsignedInteger"] = true;
        if (b.Image->Format != 0)
            imgJson["format"] = b.Image->Format;
        j["image"] = imgJson;
    }
}
void from_json(const json& j, DescriptorBindingMeta& b)
{
    b.Binding = j.value("binding", 0u);
    b.Name = j.value("name", std::string{});
    b.Type = j.value("type", 0u);
    b.Count = j.value("count", 1u);
    b.StagesMask = j.value("stagesMask", 0u);
    if (j.contains("block"))
        b.Block = j.at("block").get<BlockLayout>();
    b.ReadOnly = j.value("readOnly", false);
    if (j.contains("image"))
    {
        DescriptorBindingMeta::ImageInfo info{};
        auto ji = j.at("image");
        info.Dim = ji.value("dim", 2u);
        info.Arrayed = ji.value("arrayed", false);
        info.Multisample = ji.value("multisample", false);
        info.Cube = ji.value("cube", false);
        info.Depth = ji.value("depth", false);
        info.Filtered = ji.value("filtered", false);
        info.UnsignedInteger = ji.value("unsignedInteger", false);
        info.Format = ji.value("format", 0u);
        b.Image = info;
    }
}

void to_json(json& j, const DescriptorSetMeta& s)
{
    j = json::object();
    j["set"] = s.Set;
    j["bindings"] = s.Bindings;
}
void from_json(const json& j, DescriptorSetMeta& s)
{
    s.Set = j.value("set", 0u);
    s.Bindings = j.value("bindings", std::vector<DescriptorBindingMeta>{});
    // Precompute layout hash so MaterialBinder doesn't pay for it per draw.
    FinalizeSetLayout(s);
}

void to_json(json& j, const StageIO& io)
{
    j = json::object();
    j["location"] = io.Location;
    // Emitted only when non-default, so a package whose interface uses no
    // component packing serialises exactly as it did before the field existed.
    if (io.Component != 0u)
        j["component"] = io.Component;
    j["name"] = io.Name;
    j["type"] = io.Type;
    if (io.Builtin)
        j["builtin"] = *io.Builtin;
}
void from_json(const json& j, StageIO& io)
{
    io.Location = j.value("location", 0u);
    io.Component = j.value("component", 0u);
    io.Name = j.value("name", std::string{});
    io.Type = j.value("type", TypeDesc{});
    if (j.contains("builtin"))
        io.Builtin = j.at("builtin").get<std::string>();
}

void to_json(json& j, const StageMeta& s)
{
    j = json::object();
    j["inputs"] = s.Inputs;
    j["outputs"] = s.Outputs;
    j["entryPoint"] = s.EntryPoint;
    if (s.ComputeLocalSize)
    {
        json lsJson = json::object();
        lsJson["x"] = s.ComputeLocalSize->X;
        lsJson["y"] = s.ComputeLocalSize->Y;
        lsJson["z"] = s.ComputeLocalSize->Z;
        j["computeLocalSize"] = lsJson;
    }
    if (s.MeshLocalSize)
    {
        json lsJson = json::object();
        lsJson["x"] = s.MeshLocalSize->X;
        lsJson["y"] = s.MeshLocalSize->Y;
        lsJson["z"] = s.MeshLocalSize->Z;
        j["meshLocalSize"] = lsJson;
    }
}
void from_json(const json& j, StageMeta& s)
{
    s.Inputs = j.value("inputs", std::vector<StageIO>{});
    s.Outputs = j.value("outputs", std::vector<StageIO>{});
    s.EntryPoint = j.value("entryPoint", std::string("main"));
    if (j.contains("computeLocalSize"))
    {
        auto o = j.at("computeLocalSize");
        s.ComputeLocalSize = StageMeta::LocalSize{o.value("x", 1u), o.value("y", 1u), o.value("z", 1u)};
    }
    if (j.contains("meshLocalSize"))
    {
        auto o = j.at("meshLocalSize");
        s.MeshLocalSize = StageMeta::LocalSize{o.value("x", 1u), o.value("y", 1u), o.value("z", 1u)};
    }
}

void to_json(json& j, const SpecConstantMeta& s)
{
    j = json::object();
    j["id"] = s.Id;
    j["name"] = s.Name;
    j["type"] = s.Type;
    if (s.DefaultValue)
        j["defaultValue"] = *s.DefaultValue;
}
void from_json(const json& j, SpecConstantMeta& s)
{
    s.Id = j.value("id", 0u);
    s.Name = j.value("name", std::string{});
    s.Type = j.value("type", TypeDesc{});
    if (j.contains("defaultValue"))
        s.DefaultValue = j.at("defaultValue").get<std::string>();
}

void to_json(json& j, const ShaderProperty& p)
{
    j = json::object();
    j["name"] = p.Name;
    j["displayName"] = p.DisplayName;
    j["type"] = static_cast<uint32_t>(p.Type);
    j["origin"] = static_cast<uint32_t>(p.Origin);
    j["default"] = std::vector<float>(p.Default.begin(), p.Default.end());
    if (p.HasRange)
    {
        j["rangeMin"] = p.RangeMin;
        j["rangeMax"] = p.RangeMax;
    }
    if (!p.Group.empty())
        j["group"] = p.Group;
    if (!p.VisibleIf.empty())
        j["visibleIf"] = p.VisibleIf;
    if (!p.Tooltip.empty())
        j["tooltip"] = p.Tooltip;
    if (!p.EnumValues.empty())
        j["values"] = p.EnumValues;
    if (p.Hdr)
        j["hdr"] = true;
    if (p.Hidden)
        j["hidden"] = true;
    if (p.HasAlpha)
        j["hasAlpha"] = true;
    if (p.HasLane)
    {
        j["lane"] = p.Lane;
        j["component"] = p.Component;
        j["byteOffset"] = p.ByteOffset;
        j["byteSize"] = p.ByteSize;
    }
    j["sourceFile"] = p.SourceFile;
    j["sourceLine"] = p.SourceLine;
}
void from_json(const json& j, ShaderProperty& p)
{
    p.Name = j.value("name", std::string{});
    p.DisplayName = j.value("displayName", std::string{});
    p.Type = static_cast<ShaderPropertyType>(j.value("type", 0u));
    p.Origin = static_cast<ShaderPropertyOrigin>(j.value("origin", 1u));
    const auto def = j.value("default", std::vector<float>{});
    for (size_t i = 0; i < p.Default.size() && i < def.size(); ++i)
        p.Default[i] = def[i];
    p.HasRange = j.contains("rangeMin") && j.contains("rangeMax");
    if (p.HasRange)
    {
        p.RangeMin = j.at("rangeMin").get<float>();
        p.RangeMax = j.at("rangeMax").get<float>();
    }
    p.Group = j.value("group", std::string{});
    p.VisibleIf = j.value("visibleIf", std::string{});
    p.Tooltip = j.value("tooltip", std::string{});
    p.EnumValues = j.value("values", std::vector<std::string>{});
    p.Hdr = j.value("hdr", false);
    p.Hidden = j.value("hidden", false);
    p.HasAlpha = j.value("hasAlpha", false);
    p.HasLane = j.contains("byteOffset");
    if (p.HasLane)
    {
        p.Lane = j.value("lane", 0u);
        p.Component = j.value("component", 0u);
        p.ByteOffset = j.value("byteOffset", 0u);
        p.ByteSize = j.value("byteSize", 0u);
    }
    p.SourceFile = j.value("sourceFile", std::string{});
    p.SourceLine = j.value("sourceLine", 0u);
}

void to_json(json& j, const ShaderMeta& m)
{
    j = json::object();
    j["version"] = m.Version;
    j["entryPoints"] = m.EntryPoints;
    j["pushConstants"] = m.PushConstants;
    j["sets"] = m.Sets;
    j["stages"] = m.Stages;
    j["specConstants"] = m.SpecConstants;
    j["requirements"] = m.Requirements;
    j["requiredDefines"] = m.RequiredDefines;
    if (!m.DeclaredProperties.empty())
        j["declaredProperties"] = m.DeclaredProperties;
}
void from_json(const json& j, ShaderMeta& m)
{
    m.Version = j.value("version", 1u);
    m.EntryPoints = j.value("entryPoints", std::unordered_map<std::string, std::string>{});
    m.PushConstants = j.value("pushConstants", std::vector<PushConstantRangeMeta>{});
    m.Sets = j.value("sets", std::vector<DescriptorSetMeta>{});
    m.Stages = j.value("stages", std::unordered_map<std::string, StageMeta>{});
    m.SpecConstants = j.value("specConstants", std::vector<SpecConstantMeta>{});
    m.Requirements = j.value("requirements", std::vector<std::string>{});
    m.RequiredDefines = j.value("requiredDefines", std::vector<std::string>{});
    m.DeclaredProperties = j.value("declaredProperties", std::vector<ShaderProperty>{});
}

} // namespace Rendering
} // namespace GameEngine
