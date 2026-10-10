#include "Particles/ParticleStackBinary.h"

#include "Particles/ParticleParameterAccess.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace GameEngine::Particles
{
namespace
{
constexpr uint32 kHeaderLength = 24;
constexpr uint32 kLengthOffset = 8;
constexpr uint32 kMaximumStringLength = 0xffff;
constexpr size_t kEncodedKeySize = 4 * sizeof(float) + 2;

class Writer
{
  public:
    std::vector<uint8> Bytes;

    void U8(uint8 value) { Bytes.push_back(value); }
    void U32(uint32 value)
    {
        for (uint32 shift = 0; shift < 32; shift += 8)
            Bytes.push_back(static_cast<uint8>(value >> shift));
    }
    void F32(float value)
    {
        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        U32(bits);
    }
    void Text(std::string_view text)
    {
        U32(static_cast<uint32>(text.size()));
        Bytes.insert(Bytes.end(), text.begin(), text.end());
    }
    void Vector(const Mathematics::Vector3& vector)
    {
        F32(vector.x);
        F32(vector.y);
        F32(vector.z);
    }
    void Curve(const Math::Curve& curve)
    {
        U8(curve.KeyCount);
        for (uint32 index = 0; index < curve.KeyCount; ++index)
        {
            const auto& key = curve.Keys[index];
            F32(key.Time);
            F32(key.Value);
            F32(key.InTangent);
            F32(key.OutTangent);
            U8(static_cast<uint8>(key.Interp));
            U8(static_cast<uint8>(key.TangentMode));
        }
    }
};

class Reader
{
  public:
    explicit Reader(std::span<const uint8> bytes) : m_Bytes(bytes) {}

    bool Failed() const { return !m_Error.empty(); }
    const std::string& Error() const { return m_Error; }
    size_t Offset() const { return m_Offset; }
    void Seek(size_t offset) { m_Offset = offset; }
    void Fail(std::string message)
    {
        if (m_Error.empty())
            m_Error = std::move(message);
    }

    bool Need(size_t count)
    {
        if (Failed() || m_Offset > m_Bytes.size() || count > m_Bytes.size() - m_Offset)
        {
            Fail("Truncated particle stack");
            return false;
        }
        return true;
    }
    // Checks a declared element count against the bytes left before anything is allocated.
    bool NeedElements(uint32 count, size_t elementSize)
    {
        if (Failed() || m_Offset > m_Bytes.size() || count > (m_Bytes.size() - m_Offset) / elementSize)
        {
            Fail("Truncated particle stack array");
            return false;
        }
        return true;
    }
    uint8 U8()
    {
        if (!Need(1))
            return 0;
        return m_Bytes[m_Offset++];
    }
    uint32 U32()
    {
        if (!Need(4))
            return 0;
        uint32 value = 0;
        for (uint32 shift = 0; shift < 32; shift += 8)
            value |= static_cast<uint32>(m_Bytes[m_Offset++]) << shift;
        return value;
    }
    float F32()
    {
        const uint32 bits = U32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    bool Bool()
    {
        const uint8 value = U8();
        if (value > 1)
            Fail("Invalid boolean in particle stack");
        return value != 0;
    }
    std::string Text()
    {
        const uint32 length = U32();
        if (length > kMaximumStringLength || !Need(length))
        {
            Fail("Invalid text in particle stack");
            return {};
        }
        std::string text(reinterpret_cast<const char*>(m_Bytes.data() + m_Offset), length);
        m_Offset += length;
        return text;
    }
    Mathematics::Vector3 Vector()
    {
        const float x = F32();
        const float y = F32();
        const float z = F32();
        return {x, y, z};
    }
    void Curve(Math::Curve& curve)
    {
        const uint8 count = U8();
        if (count > Math::Curve::Capacity || !NeedElements(count, kEncodedKeySize))
        {
            Fail("Invalid curve in particle stack");
            return;
        }
        curve = {};
        curve.KeyCount = count;
        for (uint32 index = 0; index < count; ++index)
        {
            auto& key = curve.Keys[index];
            key.Time = F32();
            key.Value = F32();
            key.InTangent = F32();
            key.OutTangent = F32();
            key.Interp = static_cast<Math::CurveInterp>(U8());
            key.TangentMode = static_cast<Math::CurveTangentMode>(U8());
        }
    }

  private:
    std::span<const uint8> m_Bytes;
    size_t m_Offset = 0;
    std::string m_Error;
};

void WriteParameter(Writer& writer, const void* parameters, const ParticleParameter& parameter)
{
    const auto& field = *parameter.Field;
    switch (parameter.Kind())
    {
    case ParticleParameterKind::Bool:
    case ParticleParameterKind::Enum:
        writer.U8(*ParticleParameterPointer<uint8>(parameters, field));
        break;
    case ParticleParameterKind::Float:
        writer.F32(*ParticleParameterPointer<float>(parameters, field));
        break;
    case ParticleParameterKind::UInt:
    case ParticleParameterKind::Phase:
    case ParticleParameterKind::Flags:
        writer.U32(ReadParticleParameterInteger(parameters, parameter));
        break;
    case ParticleParameterKind::Vector3:
        writer.Vector(*ParticleParameterPointer<Mathematics::Vector3>(parameters, field));
        break;
    case ParticleParameterKind::Value:
        for (uint32 element = 0; element < parameter.ValueCount(); ++element)
        {
            const auto& value = *ParticleParameterPointer<ParticleValue>(parameters, field, element);
            writer.U8(static_cast<uint8>(value.Mode));
            writer.F32(value.Minimum);
            writer.F32(value.Maximum);
            writer.Curve(value.MinimumCurve);
            writer.Curve(value.MaximumCurve);
        }
        break;
    case ParticleParameterKind::ValueInput:
    {
        const auto& input = *ParticleParameterPointer<ParticleValueInput>(parameters, field);
        writer.U8(static_cast<uint8>(input.Driver));
        writer.F32(input.Minimum);
        writer.F32(input.Maximum);
        writer.U8(static_cast<uint8>(input.Wrap));
        break;
    }
    case ParticleParameterKind::Text:
        writer.Text(ParticleParameterPointer<char>(parameters, field));
        break;
    }
}

void ReadParameter(Reader& reader, void* parameters, const ParticleParameter& parameter)
{
    const auto& field = *parameter.Field;
    switch (parameter.Kind())
    {
    case ParticleParameterKind::Bool:
        *ParticleParameterPointer<bool>(parameters, field) = reader.Bool();
        break;
    case ParticleParameterKind::Enum:
        *ParticleParameterPointer<uint8>(parameters, field) = reader.U8();
        break;
    case ParticleParameterKind::Float:
        *ParticleParameterPointer<float>(parameters, field) = reader.F32();
        break;
    case ParticleParameterKind::UInt:
    case ParticleParameterKind::Phase:
    case ParticleParameterKind::Flags:
        WriteParticleParameterInteger(parameters, parameter, reader.U32());
        break;
    case ParticleParameterKind::Vector3:
        *ParticleParameterPointer<Mathematics::Vector3>(parameters, field) = reader.Vector();
        break;
    case ParticleParameterKind::Value:
        for (uint32 element = 0; element < parameter.ValueCount(); ++element)
        {
            auto& value = *ParticleParameterPointer<ParticleValue>(parameters, field, element);
            value.Mode = static_cast<ParticleValueMode>(reader.U8());
            value.Minimum = reader.F32();
            value.Maximum = reader.F32();
            reader.Curve(value.MinimumCurve);
            reader.Curve(value.MaximumCurve);
        }
        break;
    case ParticleParameterKind::ValueInput:
    {
        auto& input = *ParticleParameterPointer<ParticleValueInput>(parameters, field);
        input.Driver = static_cast<ParticleDriver>(reader.U8());
        input.Minimum = reader.F32();
        input.Maximum = reader.F32();
        input.Wrap = static_cast<ParticleWrap>(reader.U8());
        break;
    }
    case ParticleParameterKind::Text:
    {
        const std::string text = reader.Text();
        if (text.size() >= field.Size)
        {
            reader.Fail("Text parameter is too long");
            break;
        }
        char* destination = ParticleParameterPointer<char>(parameters, field);
        std::memset(destination, 0, field.Size);
        std::memcpy(destination, text.data(), text.size());
        break;
    }
    }
}

void WriteProcessor(Writer& writer, const ParticleProcessorInstance& processor)
{
    const auto& descriptor = *processor.Descriptor;
    writer.Text(descriptor.Id);
    writer.U32(processor.Id);
    writer.Text(processor.Label);
    writer.U8(processor.Enabled ? 1 : 0);
    writer.U8(static_cast<uint8>(processor.Stage));
    writer.U8(static_cast<uint8>(processor.Clock));
    writer.F32(processor.Start);
    writer.U8(processor.End ? 1 : 0);
    writer.F32(processor.End.value_or(0.0f));
    writer.U32(descriptor.ParameterSize);
    ForEachParticleParameter(descriptor, [&](const ParticleParameter& parameter)
                             { WriteParameter(writer, processor.Parameters.data(), parameter); });
    if (descriptor.UsesGeometry)
    {
        writer.U32(static_cast<uint32>(processor.Geometry.Vertices.size()));
        for (const auto& vertex : processor.Geometry.Vertices)
            writer.Vector(vertex);
        writer.U32(static_cast<uint32>(processor.Geometry.Indices.size()));
        for (const uint32 index : processor.Geometry.Indices)
            writer.U32(index);
    }
}

bool ReadProcessor(Reader& reader, ParticleProcessorInstance& processor)
{
    const std::string type = reader.Text();
    const auto* descriptor = reader.Failed() ? nullptr : ParticleProcessorRegistry::Find(type);
    if (!descriptor)
    {
        reader.Fail("Unknown processor type '" + type + "'");
        return false;
    }
    processor = MakeProcessorInstance(*descriptor);
    processor.Id = reader.U32();
    processor.Label = reader.Text();
    processor.Enabled = reader.Bool();
    processor.Stage = static_cast<ParticleStage>(reader.U8());
    if (static_cast<uint32>(processor.Stage) >= kParticleStageCount)
        reader.Fail("Unknown processor stage");
    processor.Clock = static_cast<ParticleClock>(reader.U8());
    if (processor.Clock != ParticleClock::Age && processor.Clock != ParticleClock::PhaseAge)
        reader.Fail("Unknown processor clock");
    processor.Start = reader.F32();
    const bool hasEnd = reader.Bool();
    const float end = reader.F32();
    if (hasEnd)
        processor.End = end;
    if (reader.U32() != descriptor->ParameterSize)
    {
        reader.Fail("Parameters of '" + type + "' do not match this build");
        return false;
    }
    ForEachParticleParameter(*descriptor, [&](const ParticleParameter& parameter)
                             { ReadParameter(reader, processor.Parameters.data(), parameter); });
    if (descriptor->UsesGeometry)
    {
        const uint32 vertices = reader.U32();
        if (vertices > kMaxGeometryVertices || !reader.NeedElements(vertices, 3 * sizeof(float)))
            reader.Fail("Invalid geometry vertex count");
        else
        {
            processor.Geometry.Vertices.resize(vertices);
            for (auto& vertex : processor.Geometry.Vertices)
                vertex = reader.Vector();
        }
        const uint32 indices = reader.U32();
        if (indices > 3 * kMaxGeometryVertices || !reader.NeedElements(indices, sizeof(uint32)))
            reader.Fail("Invalid geometry index count");
        else
        {
            processor.Geometry.Indices.resize(indices);
            for (auto& index : processor.Geometry.Indices)
                index = reader.U32();
        }
    }
    return !reader.Failed();
}
} // namespace

bool IsParticleStackBinary(std::span<const uint8> bytes)
{
    return bytes.size() >= 4 && std::memcmp(bytes.data(), kParticleStackBinaryMagic, 4) == 0;
}

bool WriteParticleStackBinary(const StackDocument& document, std::vector<uint8>& out, std::string& error)
{
    std::vector<StackDiagnostic> diagnostics;
    if (!ValidateStack(document, diagnostics))
    {
        error = diagnostics.front().Path + ": " + diagnostics.front().Message;
        return false;
    }
    Writer writer;
    writer.Bytes.insert(writer.Bytes.end(), kParticleStackBinaryMagic, kParticleStackBinaryMagic + 4);
    writer.U32(document.Version);
    writer.U32(0); // total length, patched below
    writer.U32(document.EntryPhase);
    writer.F32(document.Lifetime);
    writer.U32(static_cast<uint32>(document.Phases.size()));
    for (const auto& phase : document.Phases)
    {
        writer.U32(phase.Id);
        writer.Text(phase.Label);
        writer.U32(static_cast<uint32>(phase.Processors.size()));
        for (const auto& processor : phase.Processors)
            WriteProcessor(writer, processor);
    }
    if (writer.Bytes.size() > kMaxParticleStackBinaryBytes)
    {
        error = "The cooked stack exceeds " + std::to_string(kMaxParticleStackBinaryBytes) + " bytes";
        return false;
    }
    const auto length = static_cast<uint32>(writer.Bytes.size());
    for (uint32 byte = 0; byte < 4; ++byte)
        writer.Bytes[kLengthOffset + byte] = static_cast<uint8>(length >> (8 * byte));
    out = std::move(writer.Bytes);
    error.clear();
    return true;
}

bool ReadParticleStackBinary(std::span<const uint8> bytes, StackDocument& output,
                             std::vector<StackDiagnostic>& diagnostics)
{
    diagnostics.clear();
    if (!IsParticleStackBinary(bytes) || bytes.size() < kHeaderLength)
    {
        diagnostics.push_back({"stack", "Not a cooked particle stack"});
        return false;
    }
    if (bytes.size() > kMaxParticleStackBinaryBytes)
    {
        diagnostics.push_back({"stack", "The cooked stack is too large"});
        return false;
    }
    Reader reader(bytes);
    reader.Seek(4);
    StackDocument document;
    document.Version = reader.U32();
    if (reader.U32() != bytes.size())
    {
        diagnostics.push_back({"stack", "The cooked stack length does not match its contents"});
        return false;
    }
    document.EntryPhase = reader.U32();
    document.Lifetime = reader.F32();
    const uint32 phases = reader.U32();
    if (phases == 0 || phases > kMaxStackPhases)
        reader.Fail("A stack needs 1 to " + std::to_string(kMaxStackPhases) + " phases");
    uint32 total = 0;
    for (uint32 phaseIndex = 0; phaseIndex < phases && !reader.Failed(); ++phaseIndex)
    {
        StackPhase phase;
        phase.Id = reader.U32();
        phase.Label = reader.Text();
        const uint32 processors = reader.U32();
        if (processors > kMaxStackProcessors - total)
        {
            reader.Fail("A stack holds at most " + std::to_string(kMaxStackProcessors) + " processors");
            break;
        }
        total += processors;
        phase.Processors.resize(processors);
        for (auto& processor : phase.Processors)
            if (!ReadProcessor(reader, processor))
                break;
        document.Phases.push_back(std::move(phase));
    }
    if (!reader.Failed() && reader.Offset() != bytes.size())
        reader.Fail("Trailing bytes after the cooked stack");
    if (reader.Failed())
    {
        diagnostics.push_back({"stack", reader.Error()});
        return false;
    }
    if (!ValidateStack(document, diagnostics))
        return false;
    output = std::move(document);
    return true;
}

} // namespace GameEngine::Particles
