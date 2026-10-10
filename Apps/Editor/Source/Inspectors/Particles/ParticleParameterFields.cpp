#include "Inspectors/Particles/ParticleParameterFields.h"

#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/Particles/ParticleStackEditor.h"
#include "Particles/ParticleParameterAccess.h"
#include "Types/Color.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/Controls/Vector3Field.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::ParticleInspectors
{
namespace
{
using namespace Particles;

constexpr float kCurveHeadroom = 0.1f;
constexpr float kColorPickerMaxIntensity = 8.0f;

// The field of one processor a row edits: writes go through the editor as a function of the
// document, so an undo or a rebuild never holds a pointer into it.
struct FieldBinding
{
    std::shared_ptr<ParticleStackEditor> Editor;
    uint32 ProcessorId = 0;
    ParticleParameter Parameter;
    std::string Label;
    InspectorContext Context;
};

using ParameterWrite = std::function<void(void* parameters)>;

const ParticleProcessorInstance* CurrentProcessor(const FieldBinding& binding)
{
    const auto* document = binding.Editor->Document();
    return document ? FindProcessor(*document, binding.ProcessorId) : nullptr;
}

// Whether a write changes which fields the processor shows (or how a value array is labelled),
// so the inspector must rebuild after the commit.
bool ChangesLayout(const ParticleProcessorInstance& processor, const ParameterWrite& write)
{
    auto updated = processor.Parameters;
    write(updated.data());
    bool changed = false;
    ForEachParticleParameter(*processor.Descriptor, [&](const ParticleParameter& parameter)
                             {
        const auto& field = *parameter.Presentation;
        if (field.Visible && field.Visible(processor.Parameters.data()) != field.Visible(updated.data()))
            changed = true;
        if (field.ComponentLabelsFor &&
            field.ComponentLabelsFor(processor.Parameters.data()).size() != field.ComponentLabelsFor(updated.data()).size())
            changed = true;
        if (field.IsColor && field.IsColor(processor.Parameters.data()) != field.IsColor(updated.data()))
            changed = true; });
    return changed;
}

ParticleStackEditor::Edit ParameterEdit(uint32 processorId, ParameterWrite write)
{
    return [processorId, write = std::move(write)](StackDocument& document)
    {
        if (auto* processor = FindProcessor(document, processorId))
            write(processor->Parameters.data());
    };
}

void CommitParameter(const FieldBinding& binding, ParameterWrite write, bool forceRebuild = false)
{
    const auto* processor = CurrentProcessor(binding);
    if (!processor)
        return;
    const bool rebuild = forceRebuild || ChangesLayout(*processor, write);
    binding.Editor->Commit("Edit " + binding.Label, ParameterEdit(binding.ProcessorId, std::move(write)), rebuild);
}

void PreviewParameter(const FieldBinding& binding, ParameterWrite write)
{
    binding.Editor->Preview("Edit " + binding.Label, ParameterEdit(binding.ProcessorId, std::move(write)));
}

// A bound as the inspector prints it: "0", "0.5", "4096".
std::string BoundText(float bound)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%g", static_cast<double>(bound));
    return text;
}

// The field's tooltip and the range a typed value is clamped to, so a value that lands on a bound says why.
std::string Tooltip(const ParticleParameterField& field)
{
    const bool bounded = field.Minimum > std::numeric_limits<float>::lowest();
    const bool capped = field.Maximum < std::numeric_limits<float>::max();
    std::string text(field.Tooltip);
    if (!bounded && !capped)
        return text;
    const std::string range = bounded && capped ? BoundText(field.Minimum) + " to " + BoundText(field.Maximum)
                              : bounded         ? BoundText(field.Minimum) + " or more"
                                                : BoundText(field.Maximum) + " or less";
    return text + (text.empty() ? "" : ". ") + "Values are clamped to " + range;
}

std::string Joined(std::string_view label, std::string_view component)
{
    if (component.empty())
        return std::string(label);
    return std::string(label) + " " + std::string(component);
}

std::vector<Dropdown::Option> Options(std::span<const ParticleEnumOption> options)
{
    std::vector<Dropdown::Option> entries;
    entries.reserve(options.size());
    for (const auto& option : options)
        entries.push_back({std::string(option.WireName), std::string(option.DisplayName), {}, std::string(option.IconClass)});
    return entries;
}

int SelectedOption(std::span<const ParticleEnumOption> options, uint32 value)
{
    for (size_t index = 0; index < options.size(); ++index)
        if (options[index].Value == value)
            return static_cast<int>(index);
    return 0;
}

void AddFloatField(UIElement* parent, const FieldBinding& binding, float initial)
{
    const auto& field = *binding.Parameter.Presentation;
    const auto* info = binding.Parameter.Field;
    auto write = [info](float value)
    { return [info, value](void* parameters)
      { *ParticleParameterPointer<float>(parameters, *info) = value; }; };
    InspectorDrag::AddFloatRowWithDrag(
        parent, binding.Label, initial, [binding, write](float value)
        { PreviewParameter(binding, write(value)); },
        [binding, write](float value)
        { CommitParameter(binding, write(value)); }, initial, Tooltip(field).c_str(),
        field.Minimum, field.Maximum);
}

void AddUIntField(UIElement* parent, const FieldBinding& binding, uint32 initial)
{
    const auto& field = *binding.Parameter.Presentation;
    const auto parameter = binding.Parameter;
    const auto clamp = [minimum = field.Minimum, maximum = field.Maximum](int value)
    { return static_cast<uint32>(std::clamp(static_cast<float>(value), std::max(0.0f, minimum), maximum)); };
    auto write = [parameter, clamp](int value)
    { return [parameter, clamp, value](void* parameters)
      { WriteParticleParameterInteger(parameters, parameter, clamp(value)); }; };
    InspectorDrag::AddIntRowWithDrag(
        parent, binding.Label, static_cast<int>(initial),
        [binding, write](int value)
        { PreviewParameter(binding, write(value)); },
        [binding, write](int value)
        { CommitParameter(binding, write(value)); }, static_cast<int>(initial),
        Tooltip(field).c_str());
}

void AddEnumField(UIElement* parent, const FieldBinding& binding, uint32 initial)
{
    const auto& field = *binding.Parameter.Presentation;
    auto* dropdown = InspectorUI::AddDropdownRow(parent, binding.Label, Options(field.Options),
                                                   SelectedOption(field.Options, initial), Tooltip(field).c_str());
    dropdown->SetOnValueChanged([binding](const std::string& wireName)
                                {
        const auto* option = FindParticleOption(binding.Parameter.Presentation->Options, wireName);
        if (!option)
            return;
        const auto parameter = binding.Parameter;
        CommitParameter(binding, [parameter, value = option->Value](void* parameters)
                        { WriteParticleParameterInteger(parameters, parameter, value); }); });
}

void AddFlagsField(UIElement* parent, const FieldBinding& binding, uint32 initial)
{
    const auto& field = *binding.Parameter.Presentation;
    for (const auto& option : field.Options)
    {
        const uint32 bit = option.Value;
        InspectorDrag::AddToggleRow(
            parent, Joined(binding.Label, option.DisplayName), (initial & bit) != 0,
            [binding, bit](bool on)
            {
                const auto parameter = binding.Parameter;
                CommitParameter(binding, [parameter, bit, on](void* parameters)
                                {
                    const uint32 bits = ReadParticleParameterInteger(parameters, parameter);
                    WriteParticleParameterInteger(parameters, parameter, on ? bits | bit : bits & ~bit); });
            },
            Tooltip(field).c_str());
    }
}

void AddPhaseField(UIElement* parent, const FieldBinding& binding, const StackDocument& document, uint32 initial)
{
    std::vector<Dropdown::Option> options;
    int selected = 0;
    for (const auto& phase : document.Phases)
    {
        if (phase.Id == initial)
            selected = static_cast<int>(options.size());
        options.push_back({std::to_string(phase.Id), phase.Label});
    }
    auto* dropdown = InspectorUI::AddDropdownRow(parent, binding.Label, options, selected,
                                                   Tooltip(*binding.Parameter.Presentation).c_str());
    dropdown->SetOnValueChanged([binding](const std::string& value)
                                {
        const auto parameter = binding.Parameter;
        const auto phase = static_cast<uint32>(std::stoul(value));
        CommitParameter(binding, [parameter, phase](void* parameters)
                        { WriteParticleParameterInteger(parameters, parameter, phase); }); });
}

void AddVectorField(UIElement* parent, const FieldBinding& binding, const Mathematics::Vector3& initial)
{
    const auto& field = *binding.Parameter.Presentation;
    auto* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, binding.Label, Tooltip(field).c_str());
    auto vector = std::make_unique<Vector3Field>();
    vector->AddClass("inspector-vector3");
    vector->SetDefaultValue(Rendering::Vector3(initial.x, initial.y, initial.z));
    vector->SetValue(Rendering::Vector3(initial.x, initial.y, initial.z));
    if (field.Minimum > std::numeric_limits<float>::lowest() || field.Maximum < std::numeric_limits<float>::max())
        vector->SetComponentValueRange(field.Minimum, field.Maximum);
    const auto* info = binding.Parameter.Field;
    auto write = [info](const Rendering::Vector3& value)
    {
        return [info, value](void* parameters)
        { *ParticleParameterPointer<Mathematics::Vector3>(parameters, *info) = {value.x, value.y, value.z}; };
    };
    vector->SetOnValueChanging([binding, write](const Rendering::Vector3& value)
                               { PreviewParameter(binding, write(value)); });
    vector->SetOnValueChanged([binding, write](const Rendering::Vector3& value)
                              { CommitParameter(binding, write(value)); });
    InspectorUI::AddFieldContainer(row)->AddChild(std::move(vector));
}

void AddTextField(UIElement* parent, const FieldBinding& binding, const char* initial)
{
    auto* text = InspectorUI::AddTextRow(parent, binding.Label, initial, Tooltip(*binding.Parameter.Presentation).c_str());
    text->SetOnValueChanged([binding](const std::string& value)
                            {
        const auto* info = binding.Parameter.Field;
        if (value.size() >= info->Size)
            return;
        CommitParameter(binding, [info, value](void* parameters)
                        {
            char* destination = ParticleParameterPointer<char>(parameters, *info);
            std::memset(destination, 0, info->Size);
            std::memcpy(destination, value.data(), value.size()); }); });
}

// The value range a curve graph shows: the keys with some headroom, and at least [0, 1].
void CurveBounds(const Math::Curve& curve, const ParticleParameterField& field, float& minimum, float& maximum)
{
    minimum = 0.0f;
    maximum = 1.0f;
    for (uint32 index = 0; index < curve.KeyCount; ++index)
    {
        minimum = std::min(minimum, curve.Keys[index].Value);
        maximum = std::max(maximum, curve.Keys[index].Value);
    }
    const float headroom = (maximum - minimum) * kCurveHeadroom;
    minimum = std::max(field.Minimum, minimum - headroom);
    maximum = std::min(field.Maximum, maximum + headroom);
}

std::vector<Math::CurveKey> CurveKeys(const Math::Curve& curve)
{
    return {curve.Keys, curve.Keys + curve.KeyCount};
}

Math::Curve CurveOf(const std::vector<Math::CurveKey>& keys)
{
    Math::Curve curve;
    for (const auto& key : keys)
        if (!curve.TryInsert(key))
            break;
    return curve;
}

void AddCurveGraph(UIElement* parent, const FieldBinding& binding, uint32 element, const Math::Curve& curve, bool maximum)
{
    const auto& field = *binding.Parameter.Presentation;
    auto* row = InspectorUI::AddRow(parent);
    auto* label = InspectorUI::AddLabel(row, maximum ? "  Upper Curve" : "  Curve");
    label->AddClass("inspector-label-no-drag");
    auto* container = InspectorUI::AddFieldContainer(row);
    container->AddClass("particle-curve-container");
    auto graph = std::make_unique<CurveField>();
    graph->AddClass("particle-curve-field");
    CurveField::Config config;
    CurveBounds(curve, field, config.ValueMin, config.ValueMax);
    config.AllowTangentEditing = true;
    config.MinKeys = 1;
    config.MaxKeys = Math::Curve::Capacity;
    graph->SetConfig(config);
    graph->SetKeys(CurveKeys(curve));
    const auto* info = binding.Parameter.Field;
    auto write = [info, element, maximum](const std::vector<Math::CurveKey>& keys)
    {
        return [info, element, maximum, curve = CurveOf(keys)](void* parameters)
        {
            auto& value = *ParticleParameterPointer<ParticleValue>(parameters, *info, element);
            (maximum ? value.MaximumCurve : value.MinimumCurve) = curve;
        };
    };
    graph->SetOnChanging([binding, write](const std::vector<Math::CurveKey>& keys)
                         { PreviewParameter(binding, write(keys)); });
    graph->SetOnChanged([binding, write](const std::vector<Math::CurveKey>& keys)
                        { CommitParameter(binding, write(keys)); });
    container->AddChild(std::move(graph));
}

// Switches a value to `mode`, seeding what the new mode needs from what the value holds.
void ChangeMode(ParticleValue& value, ParticleValueMode mode)
{
    if (mode == value.Mode)
        return;
    if (value.MinimumCurve.KeyCount == 0)
    {
        Math::CurveKey key;
        key.Value = value.Minimum;
        value.MinimumCurve.TryInsert(key);
        key.Time = 1.0f;
        value.MinimumCurve.TryInsert(key);
    }
    if (value.MaximumCurve.KeyCount == 0)
        value.MaximumCurve = value.MinimumCurve;
    if (mode == ParticleValueMode::Constant)
        value.Maximum = value.Minimum;
    value.Mode = mode;
}

// `showsSwatch`: the row is one channel of a colour whose swatch row sits above it, and a committed
// channel rebuilds the rows so the swatch shows the stored colour.
void AddValueEditor(UIElement* parent, const FieldBinding& binding, uint32 element, std::string_view component,
                    const ParticleValue& value, bool showsSwatch)
{
    const auto& field = *binding.Parameter.Presentation;
    const auto* info = binding.Parameter.Field;
    const std::string label = Joined(binding.Label, component);
    auto* row = InspectorUI::AddRow(parent);
    auto* rowLabel = InspectorUI::AddLabel(row, label, Tooltip(field).c_str());
    auto* container = InspectorUI::AddFieldContainer(row);
    container->AddClass("particle-value-fields");
    const auto writeMinimum = [info, element](float number)
    {
        return [info, element, number](void* parameters)
        {
            auto& target = *ParticleParameterPointer<ParticleValue>(parameters, *info, element);
            target.Minimum = number;
            if (target.Mode == ParticleValueMode::Constant)
                target.Maximum = number;
        };
    };
    const auto writeMaximum = [info, element](float number)
    {
        return [info, element, number](void* parameters)
        { ParticleParameterPointer<ParticleValue>(parameters, *info, element)->Maximum = number; };
    };
    if (value.Mode == ParticleValueMode::Constant || value.Mode == ParticleValueMode::Range)
    {
        auto* minimum = InspectorUI::AddFloat(container, value.Minimum);
        minimum->SetValueRange(field.Minimum, field.Maximum);
        minimum->SetOnValueChanging([binding, writeMinimum](const float& number)
                                    { PreviewParameter(binding, writeMinimum(number)); });
        minimum->SetOnValueChanged([binding, writeMinimum, showsSwatch](const float& number)
                                   { CommitParameter(binding, writeMinimum(number), showsSwatch); });
        InspectorDrag::SetupLabelDragFloat(
            rowLabel, minimum, [binding, writeMinimum, minimum]
            { PreviewParameter(binding, writeMinimum(minimum->GetValue())); },
            [binding, writeMinimum, minimum, showsSwatch]
            { CommitParameter(binding, writeMinimum(minimum->GetValue()), showsSwatch); }, value.Minimum,
            field.Minimum, field.Maximum);
        if (value.Mode == ParticleValueMode::Range)
        {
            auto* maximum = InspectorUI::AddFloat(container, value.Maximum);
            maximum->SetValueRange(field.Minimum, field.Maximum);
            maximum->SetOnValueChanging([binding, writeMaximum](const float& number)
                                        { PreviewParameter(binding, writeMaximum(number)); });
            maximum->SetOnValueChanged([binding, writeMaximum](const float& number)
                                       { CommitParameter(binding, writeMaximum(number)); });
        }
    }
    else
        rowLabel->AddClass("inspector-label-no-drag");
    auto mode = std::make_unique<Dropdown>();
    mode->AddClass("inspector-dropdown");
    mode->AddClass("particle-value-mode");
    mode->SetOptions(Options(ParticleValueModeOptions()),
                     SelectedOption(ParticleValueModeOptions(), static_cast<uint32>(value.Mode)));
    mode->SetTooltip("How the value varies: one number, a random pick per particle, or a curve over the input");
    mode->SetOnValueChanged([binding, info, element](const std::string& wireName)
                            {
        const auto* option = FindParticleOption(ParticleValueModeOptions(), wireName);
        if (!option)
            return;
        CommitParameter(binding, [info, element, mode = static_cast<ParticleValueMode>(option->Value)](void* parameters)
                        { ChangeMode(*ParticleParameterPointer<ParticleValue>(parameters, *info, element), mode); },
                        true); });
    container->AddChild(std::move(mode));
    if (value.UsesCurve())
        AddCurveGraph(parent, binding, element, value.MinimumCurve, false);
    if (value.Mode == ParticleValueMode::CurveRange)
        AddCurveGraph(parent, binding, element, value.MaximumCurve, true);
}

void OpenColorPicker(const FieldBinding& binding, uint32 argb, float intensity, UIEvent& event)
{
    if (event.Button != 0 || !binding.Context.OpenColorPickerWindow)
        return;
    event.Stop();
    const auto* info = binding.Parameter.Field;
    auto write = [info](uint32 color, float gain)
    {
        const ColorLinear picked = InspectorUI::PickerStateToHdrColor(color, gain, kColorPickerMaxIntensity);
        return [info, picked](void* parameters)
        {
            const float channels[4] = {picked.r, picked.g, picked.b, picked.a};
            for (uint32 channel = 0; channel < 4; ++channel)
            {
                auto& value = *ParticleParameterPointer<ParticleValue>(parameters, *info, channel);
                value.Minimum = value.Maximum = channels[channel];
            }
        };
    };
    ColorPickerCallbacks callbacks;
    callbacks.onValueChanging = [binding, write](uint32 color, float gain)
    { PreviewParameter(binding, write(color, gain)); };
    callbacks.onCancel = [binding]
    { binding.Editor->CancelPreview(); };
    callbacks.onApply = [binding, write, argb, intensity](uint32 color, float gain)
    {
        if (color == argb && gain == intensity)
        {
            binding.Editor->CancelPreview();
            return;
        }
        CommitParameter(binding, write(color, gain), true);
    };
    binding.Context.OpenColorPickerWindow(argb, intensity, std::move(callbacks));
}

void AddColorSwatch(UIElement* parent, const FieldBinding& binding, const ParticleValue* values)
{
    const ColorLinear color{values[0].Minimum, values[1].Minimum, values[2].Minimum, values[3].Minimum};
    const auto state = InspectorUI::HdrColorToPickerState(color, kColorPickerMaxIntensity);
    char alpha[24];
    std::snprintf(alpha, sizeof(alpha), " · Alpha %.2f", color.a);
    auto row = InspectorUI::AddColorSwatchRow(parent, binding.Label, "Pick the color; Apply records one undo step",
                                              state.Argb,
                                              InspectorUI::FormatColorRgb(color.r, color.g, color.b) + alpha);
    auto click = [binding, state](UIEvent& event) { OpenColorPicker(binding, state.Argb, state.Intensity, event); };
    row.Swatch->RegisterEventHandler(kEventMouseDown, click);
    row.Value->RegisterEventHandler(kEventMouseDown, click);
}

void AddValueField(UIElement* parent, const FieldBinding& binding, const void* parameters)
{
    const auto& field = *binding.Parameter.Presentation;
    const auto* info = binding.Parameter.Field;
    const auto labels = field.ComponentLabelsFor ? field.ComponentLabelsFor(parameters) : field.ComponentLabels;
    const uint32 count = std::min<uint32>(binding.Parameter.ValueCount(),
                                          labels.empty() ? binding.Parameter.ValueCount() : static_cast<uint32>(labels.size()));
    const auto* values = ParticleParameterPointer<ParticleValue>(parameters, *info);
    const bool color = field.IsColor && field.IsColor(parameters) && count == 4 &&
                       std::all_of(values, values + 4, [](const ParticleValue& value)
                                   { return value.Mode == ParticleValueMode::Constant; });
    if (color)
        AddColorSwatch(parent, binding, values);
    for (uint32 element = 0; element < count; ++element)
        AddValueEditor(parent, binding, element, labels.empty() || count == 1 ? std::string_view{} : labels[element],
                       values[element], color);
}

void AddValueInputField(UIElement* parent, const FieldBinding& binding, const ParticleValueInput& input)
{
    const auto* info = binding.Parameter.Field;
    auto* driver = InspectorUI::AddDropdownRow(parent, binding.Label, Options(ParticleDriverOptions()),
                                                 SelectedOption(ParticleDriverOptions(), static_cast<uint32>(input.Driver)),
                                                 Tooltip(*binding.Parameter.Presentation).c_str());
    driver->SetOnValueChanged([binding, info](const std::string& wireName)
                              {
        const auto* option = FindParticleOption(ParticleDriverOptions(), wireName);
        if (!option)
            return;
        CommitParameter(binding, [info, driver = static_cast<ParticleDriver>(option->Value)](void* parameters)
                        { ParticleParameterPointer<ParticleValueInput>(parameters, *info)->Driver = driver; }); });
    const auto writeBound = [info](bool upper, float value)
    {
        return [info, upper, value](void* parameters)
        {
            auto& target = *ParticleParameterPointer<ParticleValueInput>(parameters, *info);
            (upper ? target.Maximum : target.Minimum) = value;
        };
    };
    for (const bool upper : {false, true})
        InspectorDrag::AddFloatRowWithDrag(
            parent, upper ? "  Input To" : "  Input From", upper ? input.Maximum : input.Minimum,
            [binding, writeBound, upper](float value)
            { PreviewParameter(binding, writeBound(upper, value)); },
            [binding, writeBound, upper](float value)
            { CommitParameter(binding, writeBound(upper, value)); },
            upper ? 1.0f : 0.0f, upper ? "Input value at the end of the curves" : "Input value at the start of the curves");
    auto* wrap = InspectorUI::AddDropdownRow(parent, "  Outside the Range", Options(ParticleWrapOptions()),
                                               SelectedOption(ParticleWrapOptions(), static_cast<uint32>(input.Wrap)),
                                               "What the curves read for inputs outside the range");
    wrap->SetOnValueChanged([binding, info](const std::string& wireName)
                            {
        const auto* option = FindParticleOption(ParticleWrapOptions(), wireName);
        if (!option)
            return;
        CommitParameter(binding, [info, value = static_cast<ParticleWrap>(option->Value)](void* parameters)
                        { ParticleParameterPointer<ParticleValueInput>(parameters, *info)->Wrap = value; }); });
}
} // namespace

void AddParticleParameterFields(UIElement* parent, const InspectorContext& context,
                                const std::shared_ptr<ParticleStackEditor>& editor, const StackDocument& document,
                                const ParticleProcessorInstance& processor)
{
    const void* parameters = processor.Parameters.data();
    ForEachParticleParameter(*processor.Descriptor, [&](const ParticleParameter& parameter)
                             {
        const auto& field = *parameter.Presentation;
        if (field.Visible && !field.Visible(parameters))
            return;
        FieldBinding binding{editor, processor.Id, parameter,
                             std::string(field.Label.empty() ? field.Name : field.Label), context};
        const auto& info = *parameter.Field;
        switch (parameter.Kind())
        {
        case ParticleParameterKind::Bool:
            InspectorDrag::AddToggleRow(parent, binding.Label, *ParticleParameterPointer<bool>(parameters, info),
                                        [binding](bool on)
                                        {
                                            const auto* target = binding.Parameter.Field;
                                            CommitParameter(binding, [target, on](void* block)
                                                            { *ParticleParameterPointer<bool>(block, *target) = on; });
                                        },
                                        Tooltip(field).c_str());
            break;
        case ParticleParameterKind::Float:
            AddFloatField(parent, binding, *ParticleParameterPointer<float>(parameters, info));
            break;
        case ParticleParameterKind::UInt:
            AddUIntField(parent, binding, ReadParticleParameterInteger(parameters, parameter));
            break;
        case ParticleParameterKind::Enum:
            AddEnumField(parent, binding, ReadParticleParameterInteger(parameters, parameter));
            break;
        case ParticleParameterKind::Flags:
            AddFlagsField(parent, binding, ReadParticleParameterInteger(parameters, parameter));
            break;
        case ParticleParameterKind::Phase:
            AddPhaseField(parent, binding, document, ReadParticleParameterInteger(parameters, parameter));
            break;
        case ParticleParameterKind::Vector3:
            AddVectorField(parent, binding, *ParticleParameterPointer<Mathematics::Vector3>(parameters, info));
            break;
        case ParticleParameterKind::Value:
            AddValueField(parent, binding, parameters);
            break;
        case ParticleParameterKind::ValueInput:
            AddValueInputField(parent, binding, *ParticleParameterPointer<ParticleValueInput>(parameters, info));
            break;
        case ParticleParameterKind::Text:
            AddTextField(parent, binding, ParticleParameterPointer<char>(parameters, info));
            break;
        } });
}

} // namespace GameEngine::ParticleInspectors
