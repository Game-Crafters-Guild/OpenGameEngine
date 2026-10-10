#include "Inspectors/Particles/ParticleStackInspector.h"

#include "AssetCore/Asset.h"
#include "Core/Engine.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/InspectorComponentSection.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/Particles/ParticleParameterFields.h"
#include "Inspectors/Particles/ParticleProcessorSearchProvider.h"
#include "Inspectors/Particles/ParticleStackEditor.h"
#include "Particles/Assets/ParticleStackAsset.h"
#include "Particles/ParticleProcessorRegistry.h"
#include "Particles/ParticleStackDocument.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/InspectorActionButton.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ReorderableSectionList.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Controls/TextField.h"
#include "UI/EditorIcons.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <any>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace
{
using namespace Particles;
using ParticleInspectors::ParticleProcessorSearchProvider;
using ParticleInspectors::ParticleStackEditor;
using EditorPointer = std::shared_ptr<ParticleStackEditor>;

constexpr const char* kStackStyleAssetPath = "UI/inspectors/ParticleStack/ParticleStack.css";
constexpr float kPickerRowHeight = 36.0f;
constexpr float kPickerHalfWidth = 180.0f;

size_t ProcessorTypeCount()
{
    size_t count = 0;
    ParticleProcessorRegistry::ForEach([&count](const ParticleProcessorDescriptor&)
                                       { ++count; });
    return count;
}

// Foldout keys persist expansion across rebuilds: a section is keyed by its parent's key and its
// own identity, never by its position.
struct StackLayout
{
    std::string ViewKey;
    Platform::Window* Window = nullptr;
    std::unordered_map<UIElement*, std::string> Paths;
    std::unordered_map<UIElement*, Foldout*> Sections;

    UIElement* AddSection(UIElement* parent, const std::string& title, bool expanded, const std::string& identity,
                          std::string_view iconClass)
    {
        const std::string key = Paths[parent] + "/" + identity;
        auto* section = InspectorUI::AddComponentSection(parent, ViewKey + key, title, expanded);
        if (!iconClass.empty())
            section->SetIconClass(std::string(iconClass));
        auto* content = section->GetContentContainer();
        Paths[content] = key;
        Sections[content] = section;
        return content;
    }

    void AddMenu(UIElement* content, std::vector<InspectorUI::SectionMenuItem> items)
    {
        InspectorUI::AddSectionHeaderMenu(Sections.at(content), Window, std::move(items));
    }
};

using ProcessorEdit = std::function<void(ParticleProcessorInstance&)>;

void CommitProcessor(const EditorPointer& editor, uint32 id, const std::string& label, bool rebuild,
                     const ProcessorEdit& edit)
{
    editor->Commit(label, [id, edit](StackDocument& document)
                   {
        if (auto* processor = FindProcessor(document, id))
            edit(*processor); }, rebuild);
}

void PreviewProcessor(const EditorPointer& editor, uint32 id, const std::string& label, const ProcessorEdit& edit)
{
    editor->Preview(label, [id, edit](StackDocument& document)
                    {
        if (auto* processor = FindProcessor(document, id))
            edit(*processor); });
}

void SetStart(ParticleProcessorInstance& processor, float start)
{
    processor.Start = start;
}

void AddChosenProcessor(StackDocument& document, uint32 phaseId, const std::string& type)
{
    const auto* descriptor = ParticleProcessorRegistry::Find(type);
    if (!descriptor)
        return;
    auto processor = MakeProcessorInstance(*descriptor);
    uint32 id = 0;
    std::string error;
    AddProcessor(document, phaseId, std::move(processor), id, error);
}

void CommitProcessorChoice(UIElement* root, SearchDialog* dialog, const EditorPointer& editor, uint32 phaseId,
                           const std::shared_ptr<bool>& open, const std::string& type)
{
    *open = false;
    root->RemoveChild(dialog);
    editor->Commit("Add Particle Processor", [phaseId, type](StackDocument& document)
                   { AddChosenProcessor(document, phaseId, type); }, true);
}

void ShowProcessorPicker(const EditorPointer& editor, uint32 phaseId, const std::shared_ptr<bool>& open, UIEvent& event)
{
    auto& source = *event.CurrentTarget;
    auto* manager = source.GetOwnerManager();
    auto* root = manager ? manager->GetRootElement() : nullptr;
    if (*open || !root)
        return;
    *open = true;
    auto provider = std::make_shared<ParticleProcessorSearchProvider>();
    auto dialog = std::make_unique<SearchDialog>();
    ParticleInspectors::ApplyParticleStackStyle(dialog.get());
    dialog->SetProvider(provider.get());
    dialog->SetFixedHeight(true);
    dialog->SetMaxListHeight(static_cast<float>(ProcessorTypeCount()) * kPickerRowHeight + 2.0f);
    auto* dialogPointer = dialog.get();
    dialog->SetOnResult([editor, phaseId, open, provider, dialogPointer, root](const SearchResultItem& item)
                        {
        const auto* choice = std::any_cast<std::string>(&item.UserData);
        if (!choice)
            return;
        root->PostSafeAction([editor, phaseId, open, dialogPointer, root, type = *choice]
                             { CommitProcessorChoice(root, dialogPointer, editor, phaseId, open, type); }); });
    dialog->SetOnCancel([open, provider, dialogPointer, root]
                        {
        *open = false;
        root->RemoveChild(dialogPointer); });
    const float x = source.GetLayoutX() + source.GetLayoutWidth() * 0.5f - kPickerHalfWidth;
    const float y = source.GetLayoutY() + source.GetLayoutHeight();
    root->AddChild(std::move(dialog));
    dialogPointer->SetAnchorPosition(x, y, SearchDialogHorizontalAnchor::LeadingLeft, source.GetLayoutHeight());
    dialogPointer->Show();
}

std::vector<Dropdown::Option> StageOptions(ParticleStageMask stages, ParticleStage current, int& selected)
{
    std::vector<Dropdown::Option> options;
    for (uint32 index = 0; index < kParticleStageCount; ++index)
    {
        const auto stage = static_cast<ParticleStage>(index);
        if ((stages & StageBit(stage)) == 0)
            continue;
        if (stage == current)
            selected = static_cast<int>(options.size());
        options.push_back({std::string(StageWireName(stage)), std::string(StageDisplayName(stage))});
    }
    return options;
}

// The rows every processor shares: its name, the stage it runs in, and when it is active.
void AddCommonFields(UIElement* fields, const EditorPointer& editor, const ParticleProcessorInstance& processor)
{
    const uint32 id = processor.Id;
    const auto& descriptor = *processor.Descriptor;
    InspectorUI::AddTextRow(fields, "Name", processor.Label)
        ->SetOnValueChanged([editor, id](const std::string& label)
                            { CommitProcessor(editor, id, "Rename Particle Processor", false,
                                              [label](ParticleProcessorInstance& edited)
                                              { edited.Label = label; }); });
    int selected = 0;
    const auto stages = StageOptions(descriptor.Stages, processor.Stage, selected);
    if (stages.size() > 1)
        InspectorUI::AddDropdownRow(fields, "Stage", stages, selected, "When the processor runs for each particle")
            ->SetOnValueChanged([editor, id](const std::string& wireName)
                                {
                ParticleStage stage;
                if (TryParseStage(wireName, stage))
                    CommitProcessor(editor, id, "Change Particle Processor Stage", true,
                                    [stage](ParticleProcessorInstance& edited) { edited.Stage = stage; }); });
    const bool interval = processor.Stage == ParticleStage::Update || processor.Stage == ParticleStage::Emission;
    if (!interval)
        return;
    if (processor.Stage == ParticleStage::Update)
        InspectorUI::AddDropdownRow(fields, "Active On", {{"phaseAge", "Phase Age"}, {"age", "Particle Age"}},
                                    processor.Clock == ParticleClock::Age ? 1 : 0,
                                    "The clock the active interval is measured on")
            ->SetOnValueChanged([editor, id](const std::string& clock)
                                { CommitProcessor(editor, id, "Change Particle Processor Clock", false,
                                                  [age = clock == "age"](ParticleProcessorInstance& edited)
                                                  { edited.Clock = age ? ParticleClock::Age : ParticleClock::PhaseAge; }); });
    InspectorDrag::AddFloatRowWithDrag(
        fields, "Active From", processor.Start,
        [editor, id](float start)
        { PreviewProcessor(editor, id, "Change Particle Processor Interval",
                           [start](ParticleProcessorInstance& edited) { SetStart(edited, start); }); },
        [editor, id](float start)
        { CommitProcessor(editor, id, "Change Particle Processor Interval", false,
                          [start](ParticleProcessorInstance& edited) { SetStart(edited, start); }); },
        0.0f, "Seconds after which the processor starts", 0.0f);
    InspectorDrag::AddToggleRow(fields, "Stops", processor.End.has_value(), [editor, id](bool stops)
                                { CommitProcessor(editor, id, "Change Particle Processor Interval", true,
                                                  [stops](ParticleProcessorInstance& edited)
                                                  {
                                                      if (!stops)
                                                          edited.End.reset();
                                                      else if (!edited.End)
                                                          edited.End = edited.Start + 1.0f;
                                                  }); }, "Stop the processor after a while instead of running for the rest of the life");
    if (processor.End)
        InspectorDrag::AddFloatRowWithDrag(
            fields, "Active Until", *processor.End, [](float) {},
            [editor, id](float end)
            { CommitProcessor(editor, id, "Change Particle Processor Interval", false,
                              [end](ParticleProcessorInstance& edited)
                              { edited.End = std::max(end, edited.Start); }); },
            *processor.End, "Seconds at which the processor stops", 0.0f);
}

void SwitchProcessor(const EditorPointer& editor, uint32 id, bool on)
{
    CommitProcessor(editor, id, "Switch Particle Processor", false,
                    [on](ParticleProcessorInstance& edited) { edited.Enabled = on; });
}

void AddProcessorSection(const InspectorContext& context, const EditorPointer& editor, const StackDocument& document,
                         const StackPhase& phase, size_t index, EditorUI::ReorderableSectionList* list,
                         StackLayout& layout)
{
    const auto& processor = phase.Processors[index];
    const auto& descriptor = *processor.Descriptor;
    const uint32 id = processor.Id;
    const std::string title = std::string(StageDisplayName(processor.Stage)) + " · " +
                              (processor.Label.empty() ? std::string(descriptor.DisplayName) : processor.Label);
    auto* fields = layout.AddSection(list, title, false, "processor-" + std::to_string(id), descriptor.IconClass);
    auto* section = layout.Sections.at(fields);
    section->AddClass("particle-stack-section");
    if (!descriptor.Description.empty())
        section->GetHeader()->SetTooltip(std::string(descriptor.Description));
    list->AddEntry(id, section, "Drag to reorder this processor.");
    const auto neighbour = [&phase](size_t at)
    { return phase.Processors[at].Id; };
    layout.AddMenu(fields, {{"Move Up", EditorIcons::kArrowUp, index > 0, "first processor",
                             [editor, phaseId = phase.Id, id, target = index > 0 ? neighbour(index - 1) : 0u]
                             { editor->Commit("Move Particle Processor", [phaseId, id, target](StackDocument& document)
                                              { ReorderProcessor(document, phaseId, id, target, false); }, true); }},
                            {"Move Down", EditorIcons::kArrowDown, index + 1 < phase.Processors.size(), "last processor",
                             [editor, phaseId = phase.Id, id,
                              target = index + 1 < phase.Processors.size() ? neighbour(index + 1) : 0u]
                             { editor->Commit("Move Particle Processor", [phaseId, id, target](StackDocument& document)
                                              { ReorderProcessor(document, phaseId, id, target, true); }, true); }},
                            {"Remove Processor", EditorIcons::kTrash, true, {}, [editor, id]
                             { editor->Commit("Remove Particle Processor", [id](StackDocument& document)
                                              { RemoveProcessor(document, id); }, true); }}});
    InspectorUI::AddSectionEnableToggle(section, processor.Enabled, "Switch this processor on or off",
                                        [editor, id](bool on) { SwitchProcessor(editor, id, on); });
    AddCommonFields(fields, editor, processor);
    ParticleInspectors::AddParticleParameterFields(fields, context, editor, document, processor);
    if (descriptor.UsesGeometry && !processor.Geometry.Vertices.empty())
        InspectorUI::AddTextBlock(fields, std::to_string(processor.Geometry.Vertices.size()) + " vertices, " +
                                              std::to_string(processor.Geometry.Indices.size() / 3) + " triangles");
}

void AddPhaseSection(const InspectorContext& context, const EditorPointer& editor, const StackDocument& document,
                     const StackPhase& phase, UIElement* parent, StackLayout& layout)
{
    const uint32 phaseId = phase.Id;
    const bool entry = phaseId == document.EntryPhase;
    auto* content = layout.AddSection(parent, phase.Label + (entry ? " (entry)" : ""), true,
                                      "phase-" + std::to_string(phaseId), "particle-phase-icon");
    InspectorUI::AddTextRow(content, "Phase Name", phase.Label)
        ->SetOnValueChanged([editor, phaseId](const std::string& label)
                            { editor->Commit("Rename Particle Phase", [phaseId, label](StackDocument& document)
                                             {
                                                 if (auto* target = FindPhase(document, phaseId))
                                                     target->Label = label; }, true); });
    auto removalCheck = document;
    std::string removalReason;
    const bool canRemove = RemovePhase(removalCheck, phaseId, removalReason);
    layout.AddMenu(content, {{"Set Entry Phase", EditorIcons::kPlay, !entry, "already the entry phase",
                              [editor, phaseId]
                              { editor->Commit("Set Entry Phase", [phaseId](StackDocument& document)
                                               { document.EntryPhase = phaseId; }, true); }},
                             {"Remove Phase", EditorIcons::kTrash, canRemove, removalReason,
                              [editor, phaseId]
                              { editor->Commit("Remove Particle Phase", [phaseId](StackDocument& document)
                                               {
                                                   std::string error;
                                                   RemovePhase(document, phaseId, error); }, true); }}});
    auto* addField = InspectorUI::AddActionRow(content, "particle-stack-add-row");
    addField->GetParent()->AddClass("particle-stack-add-processor-row");
    auto addProcessor = std::make_unique<EditorUI::InspectorActionButton>("Add Processor", "inspector-action-add");
    addProcessor->SetId("particle-add-processor-" + std::to_string(phaseId));
    auto pickerOpen = std::make_shared<bool>(false);
    addProcessor->SetOnClick([editor, phaseId, pickerOpen](UIEvent& event)
                             { ShowProcessorPicker(editor, phaseId, pickerOpen, event); });
    addField->AddChild(std::move(addProcessor));
    auto list = std::make_unique<EditorUI::ReorderableSectionList>(
        layout.ViewKey + "/" + std::to_string(phaseId),
        [editor, phaseId](uint64_t source, uint64_t target, bool after)
        { editor->Commit("Reorder Particle Processors",
                         [phaseId, source = static_cast<uint32>(source), target = static_cast<uint32>(target),
                          after](StackDocument& document) { ReorderProcessor(document, phaseId, source, target, after); },
                         true); });
    auto* processors = list.get();
    layout.Paths[processors] = layout.Paths[content];
    content->AddChild(std::move(list));
    for (size_t index = 0; index < phase.Processors.size(); ++index)
        AddProcessorSection(context, editor, document, phase, index, processors, layout);
}

void BuildParticleStackAssetInspector(const InspectorContext& context)
{
    auto* asset = static_cast<Asset*>(context.Object);
    if (!context.Parent || !asset || asset->GetType() != AssetType::ParticleStack)
        return;
    ParticleInspectors::ApplyParticleStackStyle(context.Parent);
    ParticleInspectors::AddParticleStackRows(context, *static_cast<ParticleStackAsset*>(asset));
}
} // namespace

namespace ParticleInspectors
{
void ApplyParticleStackStyle(UIElement* element)
{
    if (!element)
        return;
    element->AddClass("particle-stack");
    element->RequestSubtreeStyleAssetPath(kStackStyleAssetPath, "editor");
}

void AddParticleStackRows(const InspectorContext& context, Particles::ParticleStackAsset& asset)
{
    if (!context.Parent)
        return;
    auto editor = std::make_shared<ParticleStackEditor>(asset, EngineCore::GetInstance().TryGetAssetManager(),
                                                        context.Undo, context.RequestInspectorRefresh);
    if (!editor->Diagnostics().empty())
    {
        std::string text;
        for (const auto& diagnostic : editor->Diagnostics())
            text += (text.empty() ? "" : "\n") + diagnostic.Path + ": " + diagnostic.Message;
        context.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(text));
    }
    const auto* document = editor->Document();
    if (!document)
        return;
    InspectorDrag::AddFloatRowWithDrag(
        context.Parent, "Particle Lifetime", document->Lifetime,
        [editor](float lifetime)
        { editor->Preview("Change Particle Lifetime", [lifetime](StackDocument& document)
                          { document.Lifetime = lifetime; }); },
        [editor](float lifetime)
        { editor->Commit("Change Particle Lifetime", [lifetime](StackDocument& document)
                         { document.Lifetime = lifetime; }, false); },
        1.0f, "Seconds a particle lives unless a processor sets its lifetime", 0.001f);
    auto* toolbarField = InspectorUI::AddActionRow(context.Parent, "particle-stack-add-row");
    toolbarField->GetParent()->SetId("particle-add-phase-row");
    auto addPhase = std::make_unique<EditorUI::InspectorActionButton>("Add Phase", "inspector-action-add");
    addPhase->SetOnClick([editor](UIEvent&)
                         { editor->Commit("Add Particle Phase", [](StackDocument& document)
                                          {
                                              uint32 id = 0;
                                              std::string error;
                                              AddPhase(document, "Phase", id, error); }, true); });
    toolbarField->AddChild(std::move(addPhase));
    StackLayout layout;
    layout.ViewKey = "ParticleStack/" + asset.GetGUID().ToString();
    layout.Window = context.Window;
    for (const auto& phase : document->Phases)
        AddPhaseSection(context, editor, *document, phase, context.Parent, layout);
}
} // namespace ParticleInspectors

void RegisterParticleStackInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(AssetType::ParticleStack, BuildParticleStackAssetInspector);
}

} // namespace GameEngine
