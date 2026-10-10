#include "Particles/ParticleStackAuthoring.h"

#include "Particles/ParticleParameterAccess.h"
#include "Particles/ParticleStackBinary.h"
#include "Serialization/JsonMath.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace GameEngine::Particles
{
namespace
{
using Json = nlohmann::json;

// Deepest nesting a stack document may use. The deepest authored path (document, phases, phase,
// processors, processor, parameters, value array, value, curve, key) is ten levels; anything far
// past that is not a stack. Copying and serializing a JSON value recurse once per level, so an
// unbounded document would exhaust the thread's stack.
constexpr int kMaxStackDocumentDepth = 32;
constexpr size_t kMaxStackDocumentBytes = 16u * 1024u * 1024u;

[[noreturn]] void Refuse(const std::string& message)
{
    throw std::runtime_error(message);
}

Json ParseBoundedDocument(std::string_view source)
{
    const auto refuseDeepNesting = [](int depth, Json::parse_event_t, Json&)
    {
        if (depth > kMaxStackDocumentDepth)
            Refuse("The stack is nested deeper than " + std::to_string(kMaxStackDocumentDepth) + " levels");
        return true;
    };
    return Json::parse(source, refuseDeepNesting);
}

const Json& Member(const Json& object, const char* key)
{
    if (!object.is_object() || !object.contains(key))
        Refuse(std::string("Missing '") + key + "'");
    return object.at(key);
}

float ReadFloat(const Json& value, std::string_view what)
{
    if (!value.is_number())
        Refuse("Expected a number for " + std::string(what));
    return value.get<float>();
}

std::string_view ReadString(const Json& value, std::string_view what)
{
    if (!value.is_string())
        Refuse("Expected text for " + std::string(what));
    return value.get_ref<const std::string&>();
}

// Every key of `object` must be one of `known`: a misspelled field is an error, not silently lost.
void RequireKnownKeys(const Json& object, std::initializer_list<std::string_view> known, std::string_view where)
{
    for (const auto& [key, value] : object.items())
        if (std::find(known.begin(), known.end(), key) == known.end())
            Refuse("Unknown field '" + key + "' in " + std::string(where));
}

Json WriteCurve(const Math::Curve& curve)
{
    Json keys = Json::array();
    for (uint32 index = 0; index < curve.KeyCount; ++index)
    {
        const auto& key = curve.Keys[index];
        Json out{{"time", key.Time}, {"value", key.Value}};
        if (key.Interp != Math::CurveInterp::Linear)
            out["interpolation"] = key.Interp == Math::CurveInterp::Smooth ? "smooth" : "step";
        if (key.TangentMode != Math::CurveTangentMode::Auto)
            out["tangentMode"] = static_cast<uint32>(key.TangentMode);
        if (key.InTangent != 0.0f)
            out["inTangent"] = key.InTangent;
        if (key.OutTangent != 0.0f)
            out["outTangent"] = key.OutTangent;
        keys.push_back(std::move(out));
    }
    return keys;
}

void ReadCurve(const Json& source, Math::Curve& curve)
{
    if (!source.is_array() || source.size() > Math::Curve::Capacity)
        Refuse("A curve is a list of at most 32 keys");
    curve = {};
    for (const auto& item : source)
    {
        RequireKnownKeys(item, {"time", "value", "interpolation", "tangentMode", "inTangent", "outTangent"},
                         "a curve key");
        Math::CurveKey key;
        key.Time = ReadFloat(Member(item, "time"), "a key time");
        key.Value = ReadFloat(Member(item, "value"), "a key value");
        if (item.contains("interpolation"))
        {
            const auto interpolation = ReadString(item.at("interpolation"), "a key interpolation");
            if (interpolation == "smooth")
                key.Interp = Math::CurveInterp::Smooth;
            else if (interpolation == "step")
                key.Interp = Math::CurveInterp::Step;
            else if (interpolation != "linear")
                Refuse("Unknown key interpolation '" + std::string(interpolation) + "'");
        }
        if (item.contains("tangentMode"))
            key.TangentMode = static_cast<Math::CurveTangentMode>(Serialization::ReadUInt32(item.at("tangentMode")));
        key.InTangent = item.contains("inTangent") ? ReadFloat(item.at("inTangent"), "a tangent") : 0.0f;
        key.OutTangent = item.contains("outTangent") ? ReadFloat(item.at("outTangent"), "a tangent") : 0.0f;
        curve.Keys[curve.KeyCount++] = key;
    }
}

Json WriteValue(const ParticleValue& value)
{
    const auto* mode = FindParticleOption(ParticleValueModeOptions(), static_cast<uint32>(value.Mode));
    Json out{{"mode", mode ? mode->WireName : "constant"}};
    switch (value.Mode)
    {
    case ParticleValueMode::Range:
        out["minimum"] = value.Minimum;
        out["maximum"] = value.Maximum;
        break;
    case ParticleValueMode::Curve:
        out["curve"] = WriteCurve(value.MinimumCurve);
        break;
    case ParticleValueMode::CurveRange:
        out["minimumCurve"] = WriteCurve(value.MinimumCurve);
        out["maximumCurve"] = WriteCurve(value.MaximumCurve);
        break;
    case ParticleValueMode::Constant:
    default:
        out["value"] = value.Minimum;
        break;
    }
    return out;
}

ParticleValue ReadValue(const Json& source)
{
    if (source.is_number())
        return ParticleValue(source.get<float>());
    RequireKnownKeys(source, {"mode", "value", "minimum", "maximum", "curve", "minimumCurve", "maximumCurve"},
                     "a value");
    const auto modeName = ReadString(Member(source, "mode"), "a value mode");
    const auto* mode = FindParticleOption(ParticleValueModeOptions(), modeName);
    if (!mode)
        Refuse("Unknown value mode '" + std::string(modeName) + "'");
    ParticleValue value;
    value.Mode = static_cast<ParticleValueMode>(mode->Value);
    switch (value.Mode)
    {
    case ParticleValueMode::Range:
        value.Minimum = ReadFloat(Member(source, "minimum"), "a range minimum");
        value.Maximum = ReadFloat(Member(source, "maximum"), "a range maximum");
        break;
    case ParticleValueMode::Curve:
        ReadCurve(Member(source, "curve"), value.MinimumCurve);
        value.MaximumCurve = value.MinimumCurve;
        break;
    case ParticleValueMode::CurveRange:
        ReadCurve(Member(source, "minimumCurve"), value.MinimumCurve);
        ReadCurve(Member(source, "maximumCurve"), value.MaximumCurve);
        break;
    case ParticleValueMode::Constant:
    default:
        value.Minimum = value.Maximum = ReadFloat(Member(source, "value"), "a constant");
        break;
    }
    return value;
}

std::string_view WireName(std::span<const ParticleEnumOption> options, uint32 value)
{
    const auto* option = FindParticleOption(options, value);
    return option ? option->WireName : std::string_view{};
}

uint32 ReadOption(std::span<const ParticleEnumOption> options, const Json& source, std::string_view what)
{
    const auto name = ReadString(source, what);
    const auto* option = FindParticleOption(options, name);
    if (!option)
        Refuse("Unknown " + std::string(what) + " '" + std::string(name) + "'");
    return option->Value;
}

Json WriteParameter(const void* parameters, const ParticleParameter& parameter)
{
    const auto& field = *parameter.Field;
    const auto& presentation = *parameter.Presentation;
    switch (parameter.Kind())
    {
    case ParticleParameterKind::Bool:
        return *ParticleParameterPointer<bool>(parameters, field);
    case ParticleParameterKind::Float:
        return *ParticleParameterPointer<float>(parameters, field);
    case ParticleParameterKind::UInt:
    case ParticleParameterKind::Phase:
        return ReadParticleParameterInteger(parameters, parameter);
    case ParticleParameterKind::Enum:
        return WireName(presentation.Options, ReadParticleParameterInteger(parameters, parameter));
    case ParticleParameterKind::Flags:
    {
        Json flags = Json::array();
        const uint32 bits = ReadParticleParameterInteger(parameters, parameter);
        for (const auto& option : presentation.Options)
            if ((bits & option.Value) != 0)
                flags.push_back(option.WireName);
        return flags;
    }
    case ParticleParameterKind::Vector3:
    {
        const auto& vector = *ParticleParameterPointer<Mathematics::Vector3>(parameters, field);
        return Json::array({vector.x, vector.y, vector.z});
    }
    case ParticleParameterKind::Value:
    {
        if (parameter.ValueCount() == 1)
            return WriteValue(*ParticleParameterPointer<ParticleValue>(parameters, field));
        Json values = Json::array();
        for (uint32 element = 0; element < parameter.ValueCount(); ++element)
            values.push_back(WriteValue(*ParticleParameterPointer<ParticleValue>(parameters, field, element)));
        return values;
    }
    case ParticleParameterKind::ValueInput:
    {
        const auto& input = *ParticleParameterPointer<ParticleValueInput>(parameters, field);
        return Json{{"driver", WireName(ParticleDriverOptions(), static_cast<uint32>(input.Driver))},
                    {"minimum", input.Minimum},
                    {"maximum", input.Maximum},
                    {"wrap", WireName(ParticleWrapOptions(), static_cast<uint32>(input.Wrap))}};
    }
    case ParticleParameterKind::Text:
    default:
        return std::string(ParticleParameterPointer<char>(parameters, field));
    }
}

void ReadParameter(const Json& source, void* parameters, const ParticleParameter& parameter)
{
    const auto& field = *parameter.Field;
    const auto& presentation = *parameter.Presentation;
    const std::string what = ParticleParameterKey(presentation.Name);
    switch (parameter.Kind())
    {
    case ParticleParameterKind::Bool:
        if (!source.is_boolean())
            Refuse("Expected true or false for " + what);
        *ParticleParameterPointer<bool>(parameters, field) = source.get<bool>();
        break;
    case ParticleParameterKind::Float:
        *ParticleParameterPointer<float>(parameters, field) = ReadFloat(source, what);
        break;
    case ParticleParameterKind::UInt:
    case ParticleParameterKind::Phase:
        WriteParticleParameterInteger(parameters, parameter, Serialization::ReadUInt32(source));
        break;
    case ParticleParameterKind::Enum:
        WriteParticleParameterInteger(parameters, parameter, ReadOption(presentation.Options, source, what));
        break;
    case ParticleParameterKind::Flags:
    {
        if (!source.is_array())
            Refuse("Expected a list for " + what);
        uint32 bits = 0;
        for (const auto& item : source)
            bits |= ReadOption(presentation.Options, item, what);
        WriteParticleParameterInteger(parameters, parameter, bits);
        break;
    }
    case ParticleParameterKind::Vector3:
        *ParticleParameterPointer<Mathematics::Vector3>(parameters, field) = Serialization::ReadVector3(source);
        break;
    case ParticleParameterKind::Value:
    {
        const uint32 count = parameter.ValueCount();
        if (count == 1)
        {
            *ParticleParameterPointer<ParticleValue>(parameters, field) = ReadValue(source);
            break;
        }
        if (!source.is_array() || source.size() > count)
            Refuse("Expected at most " + std::to_string(count) + " values for " + what);
        for (uint32 element = 0; element < source.size(); ++element)
            *ParticleParameterPointer<ParticleValue>(parameters, field, element) = ReadValue(source.at(element));
        break;
    }
    case ParticleParameterKind::ValueInput:
    {
        RequireKnownKeys(source, {"driver", "minimum", "maximum", "wrap"}, what);
        auto& input = *ParticleParameterPointer<ParticleValueInput>(parameters, field);
        if (source.contains("driver"))
            input.Driver = static_cast<ParticleDriver>(ReadOption(ParticleDriverOptions(), source.at("driver"), "driver"));
        if (source.contains("minimum"))
            input.Minimum = ReadFloat(source.at("minimum"), "an input minimum");
        if (source.contains("maximum"))
            input.Maximum = ReadFloat(source.at("maximum"), "an input maximum");
        if (source.contains("wrap"))
            input.Wrap = static_cast<ParticleWrap>(ReadOption(ParticleWrapOptions(), source.at("wrap"), "wrap"));
        break;
    }
    case ParticleParameterKind::Text:
    {
        const auto text = ReadString(source, what);
        if (text.size() >= field.Size || text.find('\0') != std::string_view::npos)
            Refuse(what + " is longer than " + std::to_string(field.Size - 1) + " bytes");
        char* destination = ParticleParameterPointer<char>(parameters, field);
        std::memset(destination, 0, field.Size);
        std::memcpy(destination, text.data(), text.size());
        break;
    }
    }
}

bool FieldEqualsDefault(const void* parameters, const void* defaults, const ECS::FieldInfo& field)
{
    return std::memcmp(static_cast<const std::byte*>(parameters) + field.Offset,
                       static_cast<const std::byte*>(defaults) + field.Offset, field.Size) == 0;
}

Json WriteProcessor(const ParticleProcessorInstance& processor)
{
    const auto& descriptor = *processor.Descriptor;
    Json out{{"type", descriptor.Id}, {"id", processor.Id}, {"label", processor.Label}};
    if (!processor.Enabled)
        out["enabled"] = false;
    out["stage"] = StageWireName(processor.Stage);
    if (processor.Clock != ParticleClock::PhaseAge)
        out["clock"] = "age";
    if (processor.Start != 0.0f)
        out["start"] = processor.Start;
    if (processor.End)
        out["end"] = *processor.End;
    const auto defaults = MakeProcessorInstance(descriptor);
    Json parameters = Json::object();
    ForEachParticleParameter(descriptor, [&](const ParticleParameter& parameter)
                             {
        if (!FieldEqualsDefault(processor.Parameters.data(), defaults.Parameters.data(), *parameter.Field))
            parameters[ParticleParameterKey(parameter.Presentation->Name)] =
                WriteParameter(processor.Parameters.data(), parameter); });
    if (!parameters.empty())
        out["parameters"] = std::move(parameters);
    if (descriptor.UsesGeometry && !processor.Geometry.Vertices.empty())
    {
        Json vertices = Json::array();
        for (const auto& vertex : processor.Geometry.Vertices)
            vertices.push_back(Json::array({vertex.x, vertex.y, vertex.z}));
        out["vertices"] = std::move(vertices);
        out["indices"] = processor.Geometry.Indices;
    }
    return out;
}

ParticleProcessorInstance ReadProcessor(const Json& source)
{
    if (!source.is_object())
        Refuse("A processor is an object");
    RequireKnownKeys(source, {"type", "id", "label", "enabled", "stage", "clock", "start", "end", "parameters", "vertices", "indices"},
                     "a processor");
    const auto type = ReadString(Member(source, "type"), "a processor type");
    const auto* descriptor = ParticleProcessorRegistry::Find(type);
    if (!descriptor)
        Refuse("Unknown processor type '" + std::string(type) + "'");
    auto processor = MakeProcessorInstance(*descriptor);
    processor.Id = Serialization::ReadUInt32(Member(source, "id"));
    const std::string where = "processor " + std::to_string(processor.Id);
    if (source.contains("label"))
        processor.Label = std::string(ReadString(source.at("label"), "a label"));
    if (source.contains("enabled"))
    {
        if (!source.at("enabled").is_boolean())
            Refuse("Expected true or false for enabled in " + where);
        processor.Enabled = source.at("enabled").get<bool>();
    }
    if (source.contains("stage") && !TryParseStage(ReadString(source.at("stage"), "a stage"), processor.Stage))
        Refuse("Unknown stage in " + where);
    if (source.contains("clock"))
    {
        const auto clock = ReadString(source.at("clock"), "a clock");
        if (clock == "age")
            processor.Clock = ParticleClock::Age;
        else if (clock != "phaseAge")
            Refuse("Unknown clock '" + std::string(clock) + "' in " + where);
    }
    if (source.contains("start"))
        processor.Start = ReadFloat(source.at("start"), "start");
    if (source.contains("end"))
        processor.End = ReadFloat(source.at("end"), "end");
    if (source.contains("parameters"))
    {
        const auto& parameters = source.at("parameters");
        if (!parameters.is_object())
            Refuse("Parameters of " + where + " are an object");
        size_t matched = 0;
        ForEachParticleParameter(*descriptor, [&](const ParticleParameter& parameter)
                                 {
            const auto key = ParticleParameterKey(parameter.Presentation->Name);
            if (!parameters.contains(key))
                return;
            ++matched;
            ReadParameter(parameters.at(key), processor.Parameters.data(), parameter); });
        if (matched != parameters.size())
            for (const auto& [key, value] : parameters.items())
            {
                bool known = false;
                ForEachParticleParameter(*descriptor, [&](const ParticleParameter& parameter)
                                         { known |= ParticleParameterKey(parameter.Presentation->Name) == key; });
                if (!known)
                    Refuse("Unknown parameter '" + key + "' of " + std::string(descriptor->Id));
            }
    }
    if (source.contains("vertices") || source.contains("indices"))
    {
        if (!descriptor->UsesGeometry)
            Refuse(std::string(descriptor->Id) + " holds no geometry");
        const auto& vertices = Member(source, "vertices");
        const auto& indices = Member(source, "indices");
        if (!vertices.is_array() || !indices.is_array() || vertices.size() > kMaxGeometryVertices ||
            indices.size() > 3 * kMaxGeometryVertices)
            Refuse("Geometry of " + where + " is not a bounded list of vertices and indices");
        processor.Geometry.Vertices.reserve(vertices.size());
        for (const auto& vertex : vertices)
            processor.Geometry.Vertices.push_back(Serialization::ReadVector3(vertex));
        processor.Geometry.Indices.reserve(indices.size());
        for (const auto& index : indices)
            processor.Geometry.Indices.push_back(Serialization::ReadUInt32(index));
    }
    return processor;
}
} // namespace

std::string SerializeParticleStack(const StackDocument& document, int indent)
{
    Json phases = Json::array();
    for (const auto& phase : document.Phases)
    {
        Json processors = Json::array();
        for (const auto& processor : phase.Processors)
            processors.push_back(WriteProcessor(processor));
        phases.push_back(Json{{"id", phase.Id}, {"label", phase.Label}, {"processors", std::move(processors)}});
    }
    const Json out{{"version", document.Version},
                   {"entryPhase", document.EntryPhase},
                   {"lifetime", document.Lifetime},
                   {"phases", std::move(phases)}};
    return out.dump(indent);
}

bool ParseParticleStack(std::string_view source, StackDocument& output, std::vector<StackDiagnostic>& diagnostics)
{
    diagnostics.clear();
    try
    {
        if (source.size() > kMaxStackDocumentBytes)
            Refuse("The stack exceeds " + std::to_string(kMaxStackDocumentBytes) + " bytes");
        const Json json = ParseBoundedDocument(source);
        if (!json.is_object())
            Refuse("A stack is an object");
        RequireKnownKeys(json, {"version", "entryPhase", "lifetime", "phases"}, "the stack");
        StackDocument document;
        document.Version = Serialization::ReadUInt32(Member(json, "version"));
        document.EntryPhase = Serialization::ReadUInt32(Member(json, "entryPhase"));
        if (json.contains("lifetime"))
            document.Lifetime = ReadFloat(json.at("lifetime"), "lifetime");
        const auto& phases = Member(json, "phases");
        if (!phases.is_array() || phases.size() > kMaxStackPhases)
            Refuse("A stack needs a list of 1 to " + std::to_string(kMaxStackPhases) + " phases");
        for (const auto& phaseJson : phases)
        {
            RequireKnownKeys(phaseJson, {"id", "label", "processors"}, "a phase");
            StackPhase phase;
            phase.Id = Serialization::ReadUInt32(Member(phaseJson, "id"));
            if (phaseJson.contains("label"))
                phase.Label = std::string(ReadString(phaseJson.at("label"), "a phase label"));
            const auto& processors = Member(phaseJson, "processors");
            if (!processors.is_array() || processors.size() > kMaxStackProcessors)
                Refuse("A phase holds a list of at most " + std::to_string(kMaxStackProcessors) + " processors");
            for (const auto& processor : processors)
                phase.Processors.push_back(ReadProcessor(processor));
            document.Phases.push_back(std::move(phase));
        }
        if (!ValidateStack(document, diagnostics))
            return false;
        output = std::move(document);
        return true;
    }
    catch (const std::exception& error)
    {
        diagnostics = {{"stack", error.what()}};
        return false;
    }
}

bool LoadParticleStack(std::span<const uint8> bytes, StackDocument& output, std::vector<StackDiagnostic>& diagnostics)
{
    if (IsParticleStackBinary(bytes))
        return ReadParticleStackBinary(bytes, output, diagnostics);
    return ParseParticleStack(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), output,
                              diagnostics);
}

} // namespace GameEngine::Particles
