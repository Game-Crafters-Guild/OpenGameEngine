#include "Particles/ParticleParameterAccess.h"
#include "Particles/ParticleStackDocument.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string>

namespace GameEngine::Particles
{
namespace
{
constexpr uint32 kMaximumProcessorsPerPhase = 64;

struct Reporter
{
    std::vector<StackDiagnostic>& Diagnostics;
    std::string Path;

    void Error(std::string message) const { Diagnostics.push_back({Path, std::move(message)}); }
};

bool InBounds(float value, const ParticleParameterField& field)
{
    return std::isfinite(value) && value >= field.Minimum && value <= field.Maximum;
}

std::string Describe(const ParticleParameterField& field)
{
    return std::string(field.Label.empty() ? field.Name : field.Label);
}

void ValidateCurve(const Math::Curve& curve, const ParticleParameterField& field, const Reporter& reporter)
{
    if (curve.KeyCount == 0 || curve.KeyCount > Math::Curve::Capacity)
    {
        reporter.Error(Describe(field) + " needs 1 to 32 curve keys");
        return;
    }
    float previous = -std::numeric_limits<float>::infinity();
    for (uint32 index = 0; index < curve.KeyCount; ++index)
    {
        const auto& key = curve.Keys[index];
        if (!std::isfinite(key.Time) || !std::isfinite(key.InTangent) || !std::isfinite(key.OutTangent) ||
            !InBounds(key.Value, field) || key.Time <= previous ||
            static_cast<uint8>(key.Interp) > static_cast<uint8>(Math::CurveInterp::Step) ||
            static_cast<uint8>(key.TangentMode) > static_cast<uint8>(Math::CurveTangentMode::Mirrored))
        {
            reporter.Error(Describe(field) + " has an invalid, out-of-range or unordered curve key");
            return;
        }
        previous = key.Time;
    }
}

void ValidateValue(const ParticleValue& value, const ParticleParameterField& field, const Reporter& reporter)
{
    if (!FindParticleOption(ParticleValueModeOptions(), static_cast<uint32>(value.Mode)))
    {
        reporter.Error(Describe(field) + " has an unsupported value mode");
        return;
    }
    if (!InBounds(value.Minimum, field) || !InBounds(value.Maximum, field))
        reporter.Error(Describe(field) + " is outside " + std::to_string(field.Minimum) + " to " +
                       std::to_string(field.Maximum));
    if (value.UsesCurve())
        ValidateCurve(value.MinimumCurve, field, reporter);
    if (value.Mode == ParticleValueMode::CurveRange)
        ValidateCurve(value.MaximumCurve, field, reporter);
}

void ValidateParameter(const void* parameters, const ParticleParameter& parameter, const StackDocument& document,
                       const Reporter& reporter)
{
    const auto& field = *parameter.Presentation;
    switch (parameter.Kind())
    {
    case ParticleParameterKind::Bool:
    {
        const uint8 value = *ParticleParameterPointer<uint8>(parameters, *parameter.Field);
        if (value > 1)
            reporter.Error(Describe(field) + " is not a boolean");
        break;
    }
    case ParticleParameterKind::Float:
        if (!InBounds(*ParticleParameterPointer<float>(parameters, *parameter.Field), field))
            reporter.Error(Describe(field) + " is outside " + std::to_string(field.Minimum) + " to " +
                           std::to_string(field.Maximum));
        break;
    case ParticleParameterKind::UInt:
    {
        const float value = static_cast<float>(ReadParticleParameterInteger(parameters, parameter));
        if (value < field.Minimum || value > field.Maximum)
            reporter.Error(Describe(field) + " is outside its range");
        break;
    }
    case ParticleParameterKind::Enum:
        if (!FindParticleOption(field.Options, ReadParticleParameterInteger(parameters, parameter)))
            reporter.Error(Describe(field) + " has an unsupported value");
        break;
    case ParticleParameterKind::Flags:
    {
        uint32 known = 0;
        for (const auto& option : field.Options)
            known |= option.Value;
        if ((ReadParticleParameterInteger(parameters, parameter) & ~known) != 0)
            reporter.Error(Describe(field) + " sets an unknown flag");
        break;
    }
    case ParticleParameterKind::Phase:
    {
        const uint32 phase = ReadParticleParameterInteger(parameters, parameter);
        const bool shown = !field.Visible || field.Visible(parameters);
        if (shown && !FindPhase(document, phase))
            reporter.Error(Describe(field) + " names a phase that does not exist");
        break;
    }
    case ParticleParameterKind::Vector3:
    {
        const auto& vector = *ParticleParameterPointer<Mathematics::Vector3>(parameters, *parameter.Field);
        if (!InBounds(vector.x, field) || !InBounds(vector.y, field) || !InBounds(vector.z, field))
            reporter.Error(Describe(field) + " is not finite or outside its range");
        break;
    }
    case ParticleParameterKind::Value:
        for (uint32 element = 0; element < parameter.ValueCount(); ++element)
            ValidateValue(*ParticleParameterPointer<ParticleValue>(parameters, *parameter.Field, element), field,
                          reporter);
        break;
    case ParticleParameterKind::ValueInput:
    {
        const auto& input = *ParticleParameterPointer<ParticleValueInput>(parameters, *parameter.Field);
        if (!FindParticleOption(ParticleDriverOptions(), static_cast<uint32>(input.Driver)) ||
            !FindParticleOption(ParticleWrapOptions(), static_cast<uint32>(input.Wrap)) ||
            !std::isfinite(input.Minimum) || !std::isfinite(input.Maximum) || !(input.Maximum > input.Minimum))
            reporter.Error(Describe(field) + " needs a supported input and a range that increases");
        break;
    }
    case ParticleParameterKind::Text:
    {
        const char* text = ParticleParameterPointer<char>(parameters, *parameter.Field);
        if (std::memchr(text, '\0', parameter.Field->Size) == nullptr)
            reporter.Error(Describe(field) + " is longer than " + std::to_string(parameter.Field->Size - 1) +
                           " bytes");
        break;
    }
    }
}

void ValidateGeometry(const ParticleProcessorInstance& processor, const Reporter& reporter)
{
    const auto& geometry = processor.Geometry;
    if (!processor.Descriptor->UsesGeometry)
    {
        if (!geometry.Vertices.empty() || !geometry.Indices.empty())
            reporter.Error("This processor type holds no geometry");
        return;
    }
    if (geometry.Vertices.size() > kMaxGeometryVertices || geometry.Indices.size() > 3 * kMaxGeometryVertices)
        reporter.Error("Geometry exceeds " + std::to_string(kMaxGeometryVertices) + " vertices");
    for (const auto& vertex : geometry.Vertices)
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z))
        {
            reporter.Error("A geometry vertex is not finite");
            break;
        }
    for (const uint32 index : geometry.Indices)
        if (index >= geometry.Vertices.size())
        {
            reporter.Error("A geometry index is out of range");
            break;
        }
}

void ValidateProcessor(const StackDocument& document, const StackPhase& phase,
                       const ParticleProcessorInstance& processor, std::vector<StackDiagnostic>& diagnostics)
{
    const Reporter reporter{diagnostics, "processor." + std::to_string(processor.Id)};
    const auto* descriptor = processor.Descriptor;
    if (!descriptor)
    {
        reporter.Error("Unknown processor type");
        return;
    }
    if (processor.Parameters.size() != descriptor->ParameterSize)
    {
        reporter.Error("Parameters do not match the processor type");
        return;
    }
    if ((descriptor->Stages & StageBit(processor.Stage)) == 0)
        reporter.Error(std::string(descriptor->DisplayName) + " cannot run in the " +
                       std::string(StageDisplayName(processor.Stage)) + " stage");
    if (descriptor->Role == ParticleProcessorRole::Emission && phase.Id != document.EntryPhase)
        reporter.Error("Emission processors belong to the entry phase");
    if (!std::isfinite(processor.Start) || processor.Start < 0.0f ||
        (processor.End && (!std::isfinite(*processor.End) || *processor.End < processor.Start)))
        reporter.Error("The active interval must start at 0 or later and end after it starts");
    if (processor.Label.size() > 0xffff)
        reporter.Error("The label is too long");
    ForEachParticleParameter(*descriptor, [&](const ParticleParameter& parameter)
                             { ValidateParameter(processor.Parameters.data(), parameter, document, reporter); });
    ValidateGeometry(processor, reporter);
    if (descriptor->Validate)
    {
        ParticleValidationContext context;
        context.Parameters = processor.Parameters.data();
        context.Document = &document;
        context.Stage = processor.Stage;
        context.Geometry = &processor.Geometry;
        context.Path = reporter.Path;
        context.Diagnostics = &diagnostics;
        descriptor->Validate(context);
    }
}
} // namespace

bool ValidateStack(const StackDocument& document, std::vector<StackDiagnostic>& diagnostics)
{
    diagnostics.clear();
    const Reporter reporter{diagnostics, "stack"};
    if (document.Version != kStackSchemaVersion)
        reporter.Error("Unsupported stack version " + std::to_string(document.Version));
    if (document.Phases.empty() || document.Phases.size() > kMaxStackPhases)
        reporter.Error("A stack needs 1 to " + std::to_string(kMaxStackPhases) + " phases");
    if (!std::isfinite(document.Lifetime) || document.Lifetime <= 0.0f)
        reporter.Error("The particle lifetime must be positive");
    std::set<uint32> ids;
    size_t total = 0;
    for (const auto& phase : document.Phases)
    {
        if (phase.Id == 0 || !ids.insert(phase.Id).second)
            Reporter{diagnostics, "phase." + std::to_string(phase.Id)}.Error("Phase ids must be unique and nonzero");
        if (phase.Processors.size() > kMaximumProcessorsPerPhase)
            Reporter{diagnostics, "phase." + std::to_string(phase.Id)}.Error("A phase holds at most 64 processors");
    }
    if (!FindPhase(document, document.EntryPhase))
        reporter.Error("The entry phase does not exist");
    for (const auto& phase : document.Phases)
        for (const auto& processor : phase.Processors)
        {
            ++total;
            if (processor.Id == 0 || !ids.insert(processor.Id).second)
                Reporter{diagnostics, "processor." + std::to_string(processor.Id)}.Error(
                    "Processor ids must be unique and nonzero");
            ValidateProcessor(document, phase, processor, diagnostics);
        }
    if (total > kMaxStackProcessors)
        reporter.Error("A stack holds at most " + std::to_string(kMaxStackProcessors) + " processors");
    return diagnostics.empty();
}

} // namespace GameEngine::Particles
