#include "Markups/MarkupInspector.h"

#include "Components/Markup/Markup.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/ContextMenuLabels.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/MarkupCommentText.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupNotesEditCommand.h"
#include "Markups/MarkupPathInspector.h"
#include "Markups/MarkupPresentation.h"
#include "Markups/MarkupRegionInspector.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/TextField.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <functional>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

using Components::Markup;
using Components::MarkupAuthor;
using Components::MarkupVolume;
using Components::MarkupVolumeShape;
using MarkupECS::MarkupService;

// Runs a notes edit as one undo step (CommitMarkupNotesEdit announces it, so the bridge
// touches the mark-up for the user and the panel rebuilds), then rebuilds this inspector.
// Refused in play mode: mark-ups are edited in the edit world only.
void CommitEdit(const InspectorContext& ctx, MarkupEditorBridge& bridge, const char* label,
                const std::function<bool()>& edit)
{
    if (bridge.IsInPlayMode())
        return;
    if (CommitMarkupNotesEdit(*ctx.World, ctx.Entity, ctx.Undo, ctx.ChangeNotifications, label, edit,
                              [&bridge]() { bridge.SaveVocabulary(); }) &&
        ctx.RequestInspectorRefresh)
        ctx.RequestInspectorRefresh();
}

std::string JoinTags(const MarkupECS::MarkupNotes* notes)
{
    std::string joined;
    if (!notes)
        return joined;
    for (const uint32 tag : notes->Tags)
    {
        // The field is typed back into names, so it shows the vocabulary's own.
        if (const MarkupECS::MarkupTag* named = MarkupService::Get().GetTag(tag))
            joined += (joined.empty() ? "" : ", ") + named->Name;
    }
    return joined;
}

std::vector<std::string> SplitTags(const std::string& text)
{
    std::vector<std::string> names;
    std::stringstream stream(text);
    std::string name;
    while (std::getline(stream, name, ','))
    {
        const size_t first = name.find_first_not_of(" \t");
        const size_t last = name.find_last_not_of(" \t");
        if (first != std::string::npos)
            names.push_back(name.substr(first, last - first + 1));
    }
    return names;
}

// The status choices; a status outside the vocabulary shows as an "Unknown" entry that
// stays selected until the user picks a status.
Dropdown* AddStatusRow(const InspectorContext& ctx, MarkupEditorBridge& bridge, const Markup& markup)
{
    const MarkupService& service = MarkupService::Get();
    std::vector<Dropdown::Option> options;
    int selected = -1;
    std::vector<uint32> ids;
    for (uint32 id = 0; id < service.GetTagCount(); ++id)
    {
        if (!service.IsStatusTag(id))
            continue;
        if (id == markup.Status)
            selected = static_cast<int>(ids.size());
        ids.push_back(id);
        Dropdown::Option option;
        option.value = std::to_string(id);
        option.label = MarkupTagLabel(id);
        options.push_back(std::move(option));
    }
    if (selected < 0)
    {
        selected = static_cast<int>(ids.size());
        ids.push_back(markup.Status);
        options.push_back(Dropdown::Option{std::to_string(markup.Status), "Unknown"});
    }
    Dropdown* dropdown =
        InspectorUI::AddDropdownRow(ctx.Parent, "Status", options, selected, "Where the work on this place stands");
    dropdown->RegisterEventHandler(kEventValueChanged, [ctx, &bridge, dropdown, ids](UIEvent&) {
        const int index = dropdown->GetSelectedIndex();
        if (index < 0 || index >= static_cast<int>(ids.size()))
            return;
        const uint32 status = ids[static_cast<size_t>(index)];
        CommitEdit(ctx, bridge, "Set Mark-up Status", [&]() {
            return MarkupService::Get().SetStatus(*ctx.World, ctx.Entity, status, bridge.CurrentAuthor(),
                                                  bridge.Now());
        });
    });
    return dropdown;
}

// The color the mark-up draws in. Its default is its status's color: the swatch shows that
// color and the row says "Status color". A picked color (shown as its hex value) is one undo
// step, and the reset glyph beside the swatch takes it back to the status color (Markup::Color
// all zero). The picker previews on the volume while it is open.
void AddColorRow(const InspectorContext& ctx, MarkupEditorBridge& bridge, const Markup& markup)
{
    const bool custom = MarkupHasOwnColor(markup);
    InspectorUI::ColorSwatchRow row = InspectorUI::AddColorSwatchRow(
        ctx.Parent, "Color", "The color the volume, its label and its row draw in; its status color unless set",
        MarkupDisplayArgb(markup),
        custom ? ContextMenuLabels::ArgbToHexRGB(MarkupDisplayArgb(markup)) : std::string("Status color"));
    if (!row.Swatch || !row.Value)
        return;
    const auto open = [ctx, &bridge](UIEvent& event) {
        if (event.Button != 0 || bridge.IsInPlayMode() || !ctx.OpenColorPickerWindow)
            return;
        event.Stop();
        const Markup* current = ctx.World->GetComponent<Markup>(ctx.Entity);
        if (!current)
            return;
        using Edit = UndoRedoService::InteractiveEdit;
        auto edit = std::make_shared<Edit>();
        if (ctx.Undo)
            *edit = ctx.Undo->BeginInteractiveEdit(
                "Set Mark-up Color",
                InspectorDrag::MakeComponentSnapshotTarget<Markup>(ctx.World, ctx.Entity, ctx.ChangeNotifications, "Mark-up Color"));
        const auto write = [ctx](uint32_t argb) {
            if (Markup* edited = ctx.World->GetComponentForWrite<Markup>(ctx.Entity))
                SetMarkupColorArgb(*edited, argb);
        };
        ColorPickerCallbacks callbacks;
        callbacks.onValueChanging = [edit, write](uint32_t argb, float) {
            if (*edit)
                edit->Preview([&] { write(argb); });
        };
        callbacks.onApply = [ctx, edit, write](uint32_t argb, float) {
            if (*edit)
            {
                edit->Preview([&] { write(argb); });
                edit->Commit();
            }
            else
            {
                InspectorDrag::CommitComponentWithUndo<Markup>(ctx.World, ctx.Entity, ctx.ChangeNotifications, nullptr,
                                                               "Set Mark-up Color",
                                                               [argb](Markup& edited) { SetMarkupColorArgb(edited, argb); });
            }
            if (ctx.RequestInspectorRefresh)
                ctx.RequestInspectorRefresh();
        };
        callbacks.onCancel = [edit]() {
            if (*edit)
                edit->Cancel();
        };
        ctx.OpenColorPickerWindow(MarkupDisplayArgb(*current), 1.0f, std::move(callbacks));
    };
    row.Swatch->RegisterEventHandler(kEventMouseDown, open);
    row.Value->RegisterEventHandler(kEventMouseDown, open);
    if (!custom || bridge.IsInPlayMode())
        return;
    // The theme's icon button with the reset glyph, as the inspector's other glyph buttons are.
    auto reset = std::make_unique<Button>();
    reset->AddClass("icon-button");
    reset->AddClass("markup-inspector-color-reset");
    reset->SetTooltip("Use the status color again");
    reset->RegisterEventHandler(kEventButtonClick, [ctx](UIEvent&) {
        InspectorDrag::CommitComponentWithUndo<Markup>(ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo,
                                                       "Use Mark-up Status Color", [](Markup& edited) {
                                                           for (float32& channel : edited.Color)
                                                               channel = 0.0f;
                                                       });
        if (ctx.RequestInspectorRefresh)
            ctx.RequestInspectorRefresh();
    });
    row.Swatch->GetParent()->AddChild(std::move(reset));
}

// A new name joins the project's tags in the same undo step as the edit, so undoing the
// edit takes a mistyped name out of the vocabulary again.
TextField* AddTagsRow(const InspectorContext& ctx, MarkupEditorBridge& bridge, const MarkupECS::MarkupNotes* notes)
{
    TextField* field = InspectorUI::AddTextRow(ctx.Parent, "Tags", JoinTags(notes),
                                               "Free tags, separated by commas; a new name joins the project's tags");
    field->SetOnCommit([ctx, &bridge, field]() {
        CommitEdit(ctx, bridge, "Set Mark-up Tags", [&]() {
            MarkupService& service = MarkupService::Get();
            constexpr float32 kNewTagColor[4] = {0.6f, 0.6f, 0.6f, 1.0f};
            std::vector<uint32> tags;
            for (const std::string& name : SplitTags(field->GetValue()))
            {
                const uint32 tag = service.AddTag(name, {}, kNewTagColor);
                if (tag != MarkupService::kInvalidTag && !service.IsStatusTag(tag))
                    tags.push_back(tag);
            }
            return service.SetTags(*ctx.World, ctx.Entity, tags, bridge.CurrentAuthor(), bridge.Now());
        });
    });
    return field;
}

TextArea* AddDescriptionRow(const InspectorContext& ctx, MarkupEditorBridge& bridge,
                           const MarkupECS::MarkupNotes* notes)
{
    // An inspector row, so the row's disabled look reaches its label in play mode.
    UIElement* row = InspectorUI::AddRow(ctx.Parent);
    if (Label* label = InspectorUI::AddLabel(row, "Description"); label && label->GetParent())
        label->GetParent()->AddClass("markup-inspector-description-label-cell");
    auto area = std::make_unique<TextArea>();
    area->AddClass("markup-inspector-description");
    area->SetValue(notes ? notes->Description : std::string());
    TextArea* areaPtr = area.get();
    // Committed when the field loses focus, so typing is one edit, not one per key.
    area->RegisterEventHandler(kEventFocusOut, [ctx, &bridge, areaPtr](UIEvent&) {
        const MarkupECS::MarkupNotes* current = MarkupService::Get().FindNotes(*ctx.World, ctx.Entity);
        const std::string& text = areaPtr->GetValue();
        if (current && current->Description == text)
            return;
        CommitEdit(ctx, bridge, "Edit Mark-up Description", [&]() {
            return MarkupService::Get().SetDescription(*ctx.World, ctx.Entity, bridge.CurrentAuthor(), bridge.Now(),
                                                       text);
        });
    });
    InspectorUI::AddFieldContainer(row)->AddChild(std::move(area));
    return areaPtr;
}

// One thread entry: a comment as a header (author, time) over its text; an action as one
// muted line ("Agent set it to In progress · 5 min ago"). The kind classes are the ones the
// Activity tab's rows carry (MarkupEntryKindClass), styled alike in MarkupsPanel.css.
std::unique_ptr<UIElement> BuildThreadEntry(const MarkupECS::MarkupEntry& entry, int64 now, MarkupEditorBridge& bridge,
                                            ECS::World& world)
{
    auto item = std::make_unique<UIElement>();
    item->AddClass("markup-inspector-entry");
    item->AddClass(MarkupEntryKindClass(entry));
    const std::string author = MarkupAuthorText(entry.Author);
    const std::string when = RelativeTimeText(entry.TimeUnix, now);
    if (MarkupEntryIsAction(entry))
    {
        item->AddClass("markup-entry-action");
        auto line = std::make_unique<UIElement>();
        line->AddClass("markup-inspector-entry-action-row");
        line->AddChild(BuildMarkupEntryMarker(entry));
        auto text = std::make_unique<Label>();
        text->AddClass("markup-inspector-entry-action-text");
        std::unique_ptr<UIElement> swatch = BuildMarkupEntryColorSwatch(entry);
        text->SetText(author + " " + MarkupEntryText(entry) + (swatch ? "" : " · " + when));
        line->AddChild(std::move(text));
        if (swatch)
        {
            // The color the line names, then the time after it.
            line->AddChild(std::move(swatch));
            auto time = std::make_unique<Label>();
            time->AddClass("markup-inspector-entry-action-text");
            time->SetText("· " + when);
            line->AddChild(std::move(time));
        }
        item->AddChild(std::move(line));
        return item;
    }
    // The comment's marker beside its header, at the action rows' marker column.
    auto headerRow = std::make_unique<UIElement>();
    headerRow->AddClass("markup-inspector-entry-action-row");
    headerRow->AddChild(BuildMarkupEntryMarker(entry));
    auto header = std::make_unique<Label>();
    header->AddClass("markup-inspector-entry-header");
    header->SetText(author + " · " + when);
    headerRow->AddChild(std::move(header));
    item->AddChild(std::move(headerRow));
    // The comment's links and entity links work in its text (BuildMarkupCommentText).
    item->AddChild(BuildMarkupCommentText(entry.Text, "markup-inspector-entry-body", bridge, world,
                                          SelectThroughBridge(bridge, world)));
    return item;
}

// The thread, boxed under its title, then the comment field and its Add comment button;
// returns the two.
std::pair<TextArea*, Button*> AddThread(const InspectorContext& ctx, MarkupEditorBridge& bridge, const MarkupECS::MarkupNotes* notes)
{
    auto group = std::make_unique<UIElement>();
    group->AddClass("markup-inspector-thread-group");
    auto title = std::make_unique<Label>();
    title->AddClass("markup-inspector-thread-title");
    title->SetText("Thread");
    group->AddChild(std::move(title));
    auto thread = std::make_unique<UIElement>();
    thread->AddClass("markup-inspector-thread");
    const int64 now = bridge.Now();
    if (notes)
    {
        for (const MarkupECS::MarkupEntry& entry : notes->Entries)
        {
            if (entry.Kind != MarkupECS::MarkupEntryKind::Unreadable)
                thread->AddChild(BuildThreadEntry(entry, now, bridge, *ctx.World));
        }
    }
    group->AddChild(std::move(thread));
    ctx.Parent->AddChild(std::move(group));

    // A full-width, several-line field under the Thread, as wide as its entries: a comment is
    // written as a message, line breaks included.
    // An empty field shows a hint in the theme's placeholder look, gone once it holds text.
    auto host = std::make_unique<UIElement>();
    host->AddClass("markup-inspector-comment-host");
    auto field = std::make_unique<TextArea>();
    field->AddClass("markup-inspector-comment");
    field->SetTooltip("Write a comment for the agent: Enter adds it, Shift+Enter starts a new line");
    TextArea* fieldPtr = field.get();
    auto placeholder = std::make_unique<Label>();
    placeholder->AddClass("markup-inspector-comment-placeholder");
    placeholder->SetText("Add a comment: Enter sends, Shift+Enter for a new line");
    Label* placeholderPtr = placeholder.get();
    const auto showPlaceholder = [placeholderPtr](const std::string& value) {
        value.empty() ? placeholderPtr->RemoveClass("hidden") : placeholderPtr->AddClass("hidden");
    };
    fieldPtr->SetOnValueChanging(showPlaceholder);
    fieldPtr->SetOnValueChanged(showPlaceholder);
    host->AddChild(std::move(field));
    host->AddChild(std::move(placeholder));
    UIElement* hostPtr = host.get();
    ctx.Parent->AddChild(std::move(host));
    auto addComment = [ctx, &bridge, fieldPtr]() {
        const std::string text = fieldPtr->GetValue();
        if (text.empty())
            return;
        CommitEdit(ctx, bridge, "Add Mark-up Comment", [&]() {
            return MarkupService::Get().AddComment(*ctx.World, ctx.Entity, bridge.CurrentAuthor(), bridge.Now(), text);
        });
    };
    // Enter adds the comment and Shift+Enter breaks the line (the text area's submit mode).
    fieldPtr->SetOnSubmit(addComment);
    // The send button under the field, flush with its right edge: the field and the button read
    // as one block.
    auto row = std::make_unique<UIElement>();
    row->AddClass("markup-inspector-comment-actions");
    auto add = std::make_unique<Button>();
    add->SetText("Add comment");
    add->AddClass("small");
    add->AddClass("secondary");
    add->RegisterEventHandler(kEventButtonClick, [addComment](UIEvent&) { addComment(); });
    Button* addPtr = add.get();
    row->AddChild(std::move(add));
    hostPtr->AddChild(std::move(row));
    return {fieldPtr, addPtr};
}

// "Created now by the agent · changed 5 min ago by you".
std::string CreatedChangedText(const Markup& markup, int64 now)
{
    const auto who = [](MarkupAuthor author) { return author == MarkupAuthor::Agent ? "the agent" : "you"; };
    return "Created " + RelativeTimeText(markup.CreatedUnix, now) + " by " + who(markup.Author) + " · changed " +
           RelativeTimeText(markup.UpdatedUnix, now) + " by " + who(markup.UpdatedBy);
}

// In play mode the section shows the notes read-only, under the notice the Mark-up tool
// shows; the edits refuse in CommitEdit as well. `firstChild` is where the section begins.
void ShowReadOnlyInPlay(const InspectorContext& ctx, size_t firstChild, std::initializer_list<UIElement*> controls)
{
    auto notice = std::make_unique<EditorUI::InspectorNotice>(std::string(MarkupEditorBridge::kPlayModeNotice),
                                                              EditorUI::InspectorNotice::Kind::Information);
    ctx.Parent->InsertChild(firstChild, std::move(notice));
    for (UIElement* control : controls)
        InspectorUI::SetRowOfControlEnabled(control, false);
}

// Rebuilds the section once when play mode starts or stops, so a section open across the
// switch turns read-only (or editable) without a reselect. One compare per frame.
void RebuildOnPlayModeChange(const InspectorContext& ctx, MarkupEditorBridge& bridge)
{
    if (!ctx.FrameRefreshCallbacks || !ctx.RequestInspectorRefresh)
        return;
    auto requested = std::make_shared<bool>(false);
    ctx.FrameRefreshCallbacks->push_back(
        [&bridge, builtInPlay = bridge.IsInPlayMode(), requested, host = ctx.Parent,
         refresh = ctx.RequestInspectorRefresh]() {
            if (*requested || bridge.IsInPlayMode() == builtInPlay)
                return;
            *requested = true;
            host->PostAction(refresh);
        });
}

// The volume's shape. Changing it is a mark-up edit, so play mode refuses it as the
// Markup section's edits are refused, and shows the row disabled.
void BuildMarkupVolumeSection(const InspectorContext& ctx, MarkupEditorBridge& bridge)
{
    if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        return;
    const MarkupVolume* volume = ctx.World->GetComponent<MarkupVolume>(ctx.Entity);
    if (!volume)
        return;
    const std::vector<Dropdown::Option> options{{"0", "Box"}, {"1", "Sphere"}};
    Dropdown* shape = InspectorUI::AddDropdownRow(ctx.Parent, "Shape", options, static_cast<int>(volume->Shape),
                                                  "The unit shape the transform places and scales");
    shape->RegisterEventHandler(kEventValueChanged, [ctx, &bridge, shape](UIEvent&) {
        if (bridge.IsInPlayMode())
            return;
        const auto chosen = static_cast<MarkupVolumeShape>(shape->GetSelectedIndex());
        InspectorDrag::CommitComponentWithUndo<MarkupVolume>(ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo,
                                                             "Set Mark-up Shape",
                                                             [chosen](MarkupVolume& edited) { edited.Shape = chosen; });
    });
    if (bridge.IsInPlayMode())
        InspectorUI::SetRowOfControlEnabled(shape, false);
    AddConvertToRegionRow(ctx, bridge);
    RebuildOnPlayModeChange(ctx, bridge);
}

void RegisterSectionTitle(ECS::ComponentTypeId type, const char* title)
{
    EditorComponentTraits traits;
    EditorComponentTraitsRegistry::Get().TryGet(type, traits);
    traits.DisplayName = title;
    EditorComponentTraitsRegistry::Get().Register(type, std::move(traits));
}

} // namespace

void RegisterMarkupInspector(MarkupEditorBridge& bridge)
{
    RegisterSectionTitle(ECS::GetComponentTypeId<Markup>(), "Mark-up");
    RegisterSectionTitle(ECS::GetComponentTypeId<MarkupVolume>(), "Mark-up Volume");
    InspectorRegistry::Get().RegisterComponentInspector<MarkupVolume>(
        [&bridge](const InspectorContext& ctx) { BuildMarkupVolumeSection(ctx, bridge); });
    RegisterMarkupRegionTraits();
    InspectorRegistry::Get().RegisterComponentInspector<Markup>([&bridge](const InspectorContext& ctx) {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid() || !MarkupService::TryGet())
            return;
        const Markup* markup = ctx.World->GetComponent<Markup>(ctx.Entity);
        if (!markup)
            return;
        bridge.MarkSeen(*ctx.World, ctx.Entity);
        const MarkupECS::MarkupNotes* notes = MarkupService::Get().FindNotes(*ctx.World, ctx.Entity);

        const size_t firstChild = ctx.Parent->GetChildren().size();
        Dropdown* status = AddStatusRow(ctx, bridge, *markup);
        AddColorRow(ctx, bridge, *markup);
        TextField* tags = AddTagsRow(ctx, bridge, notes);
        InspectorUI::AddTextBlock(ctx.Parent, CreatedChangedText(*markup, bridge.Now()), "markup-inspector-history");
        TextArea* description = AddDescriptionRow(ctx, bridge, notes);
        AddMarkupRegionBlock(ctx, bridge);
        AddMarkupPathBlock(ctx, bridge);
        const auto [comment, addComment] = AddThread(ctx, bridge, notes);
        if (bridge.IsInPlayMode())
            ShowReadOnlyInPlay(ctx, firstChild, {status, tags, description, comment, addComment});
        RebuildOnPlayModeChange(ctx, bridge);
    });
}

} // namespace GameEngine::Editor
