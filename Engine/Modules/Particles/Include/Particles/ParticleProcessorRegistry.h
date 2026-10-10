#pragma once

#include "ECS/Reflection.h"
#include "Particles/ParticleChannels.h"
#include "Particles/ParticleValue.h"
#include "Types/Types.h"

#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace GameEngine::Particles
{

class ParticleEventSink;
class ParticleTrailStore;
struct ParticleEmitterFrame;
struct ParticleProcessorGeometry;
struct ParticleCollisionWorld;
struct StackDiagnostic;
struct StackDocument;

/// When a processor runs. Birth, Enter, Update and Exit run over particles; Emission runs once per
/// tick for the emitter; Event holds event rules the runtime evaluates at their trigger.
enum class ParticleStage : uint8
{
    Emission,
    Birth,
    Enter,
    Update,
    Exit,
    Event
};

inline constexpr uint32 kParticleStageCount = 6;
using ParticleStageMask = uint8;

constexpr ParticleStageMask StageBit(ParticleStage stage)
{
    return static_cast<ParticleStageMask>(1u << static_cast<uint32>(stage));
}

/// Wire name ("birth") and display name ("Birth") of a stage.
std::string_view StageWireName(ParticleStage stage);
std::string_view StageDisplayName(ParticleStage stage);
bool TryParseStage(std::string_view wireName, ParticleStage& stage);

/// Whose semantics a processor's parameters feed. Particle processors execute through their own
/// Execute function; the other roles are evaluated by the runtime from the processor's parameters.
enum class ParticleProcessorRole : uint8
{
    Particle,
    Emission,
    Event,
    Trail
};

/// Update-stage processors run either before the runtime integrates positions (forces and
/// properties) or after it (collisions, constraints and trail recording).
enum class ParticleUpdateOrder : uint8
{
    BeforeIntegration,
    AfterIntegration
};

/// How the inspector, the codecs and validation treat one reflected parameter field.
enum class ParticleParameterKind : uint8
{
    Bool,
    Float,
    UInt,
    Enum,
    Vector3,
    /// A ParticleValue, or a C array of them edited as one vector (for example X, Y, Z).
    Value,
    /// The ParticleValueInput every curve of the processor is evaluated at.
    ValueInput,
    /// A null-terminated char array.
    Text,
    /// A uint32 phase id of the same stack.
    Phase,
    /// A uint32 bit mask with one named bit per option.
    Flags
};

/// One option of an Enum or Flags parameter. WireName is the saved identifier, DisplayName what the
/// inspector shows, IconClass an optional CSS class for the dropdown entry.
struct ParticleEnumOption
{
    std::string_view WireName;
    std::string_view DisplayName;
    uint32 Value = 0;
    std::string_view IconClass{};
};

/// Saved and displayed names of the value modes, drivers and wraps every Value and ValueInput field
/// shares.
std::span<const ParticleEnumOption> ParticleValueModeOptions();
std::span<const ParticleEnumOption> ParticleDriverOptions();
std::span<const ParticleEnumOption> ParticleWrapOptions();

/// Presentation and validation of one reflected parameter field. Name matches the field's
/// GE_REFLECT name; the saved key is the name with a lowercase first letter.
struct ParticleParameterField
{
    std::string_view Name;
    std::string_view Label;
    std::string_view Tooltip{};
    ParticleParameterKind Kind = ParticleParameterKind::Float;
    std::span<const ParticleEnumOption> Options{};
    /// Inclusive bounds for Float, UInt and Value fields (every constant and key of a Value).
    float Minimum = std::numeric_limits<float>::lowest();
    float Maximum = std::numeric_limits<float>::max();
    /// Labels of the components of a Vector3 or a Value array ("X", "Y", "Z").
    std::span<const std::string_view> ComponentLabels{};
    /// For a Value array whose used components depend on other parameters (a Property's target):
    /// the labels of the components in use. Overrides ComponentLabels when set.
    std::span<const std::string_view> (*ComponentLabelsFor)(const void* parameters) = nullptr;
    /// For a Value array of four components: true when it is a linear RGBA color, which the
    /// inspector edits with a color picker while every component is constant.
    bool (*IsColor)(const void* parameters) = nullptr;
    /// Shown only when this returns true for the processor's parameters; null shows it always.
    bool (*Visible)(const void* parameters) = nullptr;
};

/// What a Validate function may report on, for one processor instance.
struct ParticleValidationContext
{
    const void* Parameters = nullptr;
    const StackDocument* Document = nullptr;
    ParticleStage Stage = ParticleStage::Update;
    const ParticleProcessorGeometry* Geometry = nullptr;
    std::string Path;
    std::vector<StackDiagnostic>* Diagnostics = nullptr;

    void Error(std::string message) const;
};

/// Per-tick scratch arrays a processor may fill for its batch; each holds the emitter's capacity.
class ParticleScratch
{
  public:
    static constexpr uint32 kArrayCount = 8;
    void Configure(uint32 capacity);
    std::span<float> Floats(uint32 index);

  private:
    std::vector<float> m_Floats[kArrayCount];
};

/// The emitter a batch belongs to, as the runtime resolved it for this tick. Particles live in
/// simulation space: world space, or emitter-local space when the emitter simulates locally.
struct ParticleEmitterFrame
{
    Mathematics::Vector3 Origin{};
    Mathematics::Vector3 Velocity{};
    /// Columns of the emitter's world transform (linear part) and its inverse.
    Mathematics::Vector3 Axis[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Mathematics::Vector3 InverseAxis[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    bool LocalSpace = false;
    bool Planar = false;
    bool Invertible = true;
    double Elapsed = 0.0;
    uint32 Seed = 1;

    Mathematics::Vector3 LocalToWorldVector(const Mathematics::Vector3& vector) const;
    Mathematics::Vector3 WorldToLocalVector(const Mathematics::Vector3& vector) const;
    Mathematics::Vector3 LocalToWorldPoint(const Mathematics::Vector3& point) const;
    Mathematics::Vector3 WorldToLocalPoint(const Mathematics::Vector3& point) const;
    Mathematics::Vector3 SimulationToWorldPoint(const Mathematics::Vector3& point) const;
    Mathematics::Vector3 SimulationToWorldVector(const Mathematics::Vector3& vector) const;
    Mathematics::Vector3 WorldToSimulationPoint(const Mathematics::Vector3& point) const;
    Mathematics::Vector3 WorldToSimulationVector(const Mathematics::Vector3& vector) const;
    Mathematics::Vector3 LocalToSimulationVector(const Mathematics::Vector3& vector) const;
    Mathematics::Vector3 LocalToSimulationPoint(const Mathematics::Vector3& point) const;
};

/// One processor invocation over a batch of particles in one phase. Particles holds indices into
/// the channel arrays; the function loops over them and touches only the channels it declared.
struct ParticleProcessorContext
{
    ParticleChannels& Channels;
    std::span<const uint32> Particles;
    const ParticleEmitterFrame& Emitter;
    float DeltaTime = 0.0f;
    uint32 ProcessorId = 0;
    /// Position of the processor among its phase's processors, below 64.
    uint32 ProcessorOrdinal = 0;
    const void* Parameters = nullptr;
    const void* Compiled = nullptr;
    ParticleScratch& Scratch;
    ParticleEventSink* Events = nullptr;
    const ParticleCollisionWorld* Collision = nullptr;
    ParticleTrailStore* Trails = nullptr;

    template <typename T>
    const T& Params() const
    {
        return *static_cast<const T*>(Parameters);
    }

    /// Driver values of `input` for every particle of the batch, remapped to [0, 1]. Skips the
    /// work (and returns an empty span) when no value of the processor uses a curve.
    std::span<const float> SampleInputs(const ParticleValueInput& input, bool needed, uint32 scratchIndex);

    /// `value` for every particle of the batch into scratch array `scratchIndex`, given the inputs
    /// SampleInputs produced. `stream` separates the random picks of a processor's values.
    std::span<const float> SampleValue(const ParticleValue& value, std::span<const float> inputs, uint32 stream,
                                       uint32 scratchIndex);
};

/// One emission processor evaluated for the emitter this tick.
struct ParticleEmissionContext
{
    const void* Parameters = nullptr;
    /// Emitter time at the start of the tick and the tick length, in seconds.
    double Begin = 0.0;
    double Duration = 0.0;
    /// Per-processor state the runtime keeps across ticks and clears on restart.
    double* State = nullptr;
    uint32 Seed = 1;
    uint32 ProcessorId = 0;
    float EmitterSpeed = 0.0f;
};

/// Input to a Compile function: the processor's parameters and geometry, from which it derives
/// load-time data such as a cumulative area table.
struct ParticleProcessorCompileInput
{
    const void* Parameters = nullptr;
    const ParticleProcessorGeometry* Geometry = nullptr;
};

/// A processor type. Registered once; stacks refer to it by Id. The spans must point at static
/// storage. Particle-role processors provide Execute, emission processors Emit.
struct ParticleProcessorDescriptor
{
    /// Stable saved identifier ("acceleration"); a game uses its own prefix ("game.wind").
    std::string_view Id;
    std::string_view DisplayName;
    std::string_view Description{};
    /// CSS class of the icon shown in the processor header and the Add Processor picker.
    std::string_view IconClass{};
    std::string_view Category{};

    ParticleProcessorRole Role = ParticleProcessorRole::Particle;
    ParticleStageMask Stages = 0;
    ParticleStage DefaultStage = ParticleStage::Update;
    ParticleUpdateOrder Order = ParticleUpdateOrder::BeforeIntegration;

    /// Channels every instance reads and writes, and a function adding the ones that depend on
    /// the instance's parameters (for example the attribute a Property processor targets).
    ParticleChannelMask Reads = 0;
    ParticleChannelMask Writes = 0;
    ParticleChannelMask (*ParameterChannels)(const void* parameters) = nullptr;

    /// The reflected parameter struct (GE_REFLECT) and its presentation, one entry per field.
    std::span<const ECS::FieldInfo> Fields{};
    std::span<const ParticleParameterField> Parameters{};
    uint32 ParameterSize = 0;
    uint32 ParameterAlignment = 0;
    void (*Construct)(void* parameters) = nullptr;

    /// Instances carry vertices and triangle indices (mesh emission shapes).
    bool UsesGeometry = false;

    void (*Validate)(const ParticleValidationContext& context) = nullptr;
    std::shared_ptr<const void> (*Compile)(const ParticleProcessorCompileInput& input) = nullptr;
    void (*Execute)(ParticleProcessorContext& context) = nullptr;
    double (*Emit)(const ParticleEmissionContext& context) = nullptr;
};

/// Builds a descriptor's parameter plumbing for struct `T` reflected with GE_REFLECT.
template <typename T>
void BindParticleParameters(ParticleProcessorDescriptor& descriptor, std::span<const ParticleParameterField> fields)
{
    static_assert(std::is_trivially_copyable_v<T>, "Processor parameters are copied as bytes");
    descriptor.Fields = ECS::GetReflectedFields<T>();
    descriptor.Parameters = fields;
    descriptor.ParameterSize = static_cast<uint32>(sizeof(T));
    descriptor.ParameterAlignment = static_cast<uint32>(alignof(T));
    descriptor.Construct = [](void* parameters)
    { new (parameters) T{}; };
}

/// Every processor type a stack can hold. Built-in processors are registered before the first
/// lookup; a game or package registers its own from its module initialisation.
///
/// Registrations made while a native module loads carry that module and its load generation
/// (ECS/ModuleRegistration.h), so a module reload re-registers its processors in place and the
/// loader keeps a superseded image mapped while stacks may still run its code.
class ParticleProcessorRegistry
{
  public:
    /// Registers `descriptor`. A second registration of an Id replaces the first in place, so stacks
    /// that hold it run the new code; it is refused, with an error naming the fix, when its
    /// parameter struct differs, because stacks hold parameter bytes laid out for the first. Asserts
    /// when the presentation table does not describe every reflected field with a matching kind and
    /// size. Returns whether the descriptor is registered.
    static bool Register(const ParticleProcessorDescriptor& descriptor);

    /// The descriptor registered under `id`, or null. The pointer stays valid for the process.
    static const ParticleProcessorDescriptor* Find(std::string_view id);

    /// Visits every descriptor in registration order (built-ins first).
    static void ForEach(const std::function<void(const ParticleProcessorDescriptor&)>& visit);

    /// Module load abort: undoes what the aborted load {moduleId, generation} registered before its
    /// image is unmapped. A processor it replaced gets the replaced registration back; one it added
    /// is no longer found. Returns the number of processors undone.
    static std::size_t PurgeModuleProcessors(std::string_view moduleId, uint64 generation);

    /// Module unload ledger: processors whose code an older generation of `moduleId` served. Stacks
    /// compiled while it did may hold data that code built and still run it, so a nonzero count keeps
    /// the superseded image mapped.
    static std::size_t CountSupersededModuleProcessors(std::string_view moduleId, uint64 currentGeneration);
};

} // namespace GameEngine::Particles
