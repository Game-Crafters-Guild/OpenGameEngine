// The scene lines of a mark-up: the Markup component and the notes MarkupService holds
// beside it, as indexed lines in the scene file, so a scene diff shows the conversation.
//
//   Markup.status = "Requested"
//   Markup.author = User
//   Markup.updatedBy = Agent
//   Markup.created = 1759750000
//   Markup.updated = 1759750420
//   Markup.revision = 12
//   Markup.color = (0.2, 0.6, 1, 1)                     only when set
//   Markup.description = "Line one\nLine \"two\""       only when set
//   Markup.tag0 = "Bridge"
//   Markup.entry0 = Created|User|1759750000|"Requested"|""
//   Markup.entry1 = Comment|Agent|1759750420|""|"Moved it \\ check"
//   Markup.entry2 = Edit|User|1759750600|"The Docks"|"moved renamed"
//
// An Edit entry's text field holds its changes (MarkupEditChange) as lowercase names
// separated by spaces, in the enum's order, a recolor with the color it gave ("recolored=#F28C26"); its status field holds the name a rename gave the
// mark-up, "" when it renamed nothing.
//
// Every text, tag and status name is written in the escaped string grammar
// (FormatEscaped). Indexed lines are numbered in order from 0: a line may restate an
// index already read or add the next one, never skip ahead, so a scene cannot make the
// load allocate more than it holds. A line that cannot be read is refused whole and
// changes nothing. Tags are written by name, statuses included: a scene read in a
// project whose vocabulary lacks a name adds it (a status to the status group, a free
// tag with no group), so no scene loses a tag. The revision is restored as saved, and
// the world's revision rises to the largest one loaded, so a reader's `since` from
// before the scene was reopened orders what comes after, unless the mark-up touched
// last was deleted before the save. A line that cannot be read at the next index still
// takes its slot, as an Unreadable entry or a kInvalidTag tag that readers skip, so the
// lines after it keep their numbers and a save writes it back as authored.

#include "Components/Markup/Markup.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "MarkupECS/MarkupService.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Scene
{
namespace
{

using Components::Markup;
using Components::MarkupAuthor;
using MarkupECS::MarkupEditChange;
using MarkupECS::MarkupEntry;
using MarkupECS::MarkupEntryKind;
using MarkupECS::MarkupNotes;
using MarkupECS::MarkupService;

// The color a tag a scene names gets when the vocabulary lacks it.
constexpr float32 kAddedTagColor[4] = {0.6f, 0.6f, 0.6f, 1.0f};

bool Refuse(std::string* outError, std::string why)
{
    if (outError)
        *outError = std::move(why);
    return false;
}

void TrimLeft(std::string_view& text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
}

void TrimRight(std::string_view& text)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
}

// The schema's string grammar: a double-quoted value in which a backslash escapes a
// backslash (\\), a double quote (\"), a line feed (\n) and a carriage return (\r), so
// any text stays on its one scene line.
std::string FormatEscaped(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

// Reads one FormatEscaped string from the front of `text` (after blanks) into `out`
// and leaves `text` just past its closing quote. Refuses a missing opening or closing
// quote and a backslash before anything but \ " n r.
bool TakeEscaped(std::string_view& text, std::string& out, std::string* outError)
{
    TrimLeft(text);
    if (text.empty() || text.front() != '"')
        return Refuse(outError, "Markup text must be double-quoted");
    out.clear();
    for (size_t i = 1; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '"')
        {
            text.remove_prefix(i + 1);
            return true;
        }
        if (c != '\\')
        {
            out.push_back(c);
            continue;
        }
        if (++i == text.size())
            break;
        switch (text[i])
        {
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        default: return Refuse(outError, "Markup text has an unknown escape; only \\\\ \\\" \\n \\r are read");
        }
    }
    return Refuse(outError, "Markup text has no closing quote; write a quote inside it as \\\"");
}

// A whole value in FormatEscaped's grammar: one string and nothing after it.
bool ParseEscaped(std::string_view value, std::string& out, std::string* outError)
{
    if (!TakeEscaped(value, out, outError))
        return false;
    TrimRight(value);
    TrimLeft(value);
    if (!value.empty())
        return Refuse(outError, "Markup text has more after its closing quote; write a quote inside it as \\\"");
    return true;
}

std::string_view AuthorName(MarkupAuthor author)
{
    return author == MarkupAuthor::Agent ? "Agent" : "User";
}

bool ParseAuthor(std::string_view text, MarkupAuthor& out, std::string* outError)
{
    TrimLeft(text);
    TrimRight(text);
    if (text == "User")
        out = MarkupAuthor::User;
    else if (text == "Agent")
        out = MarkupAuthor::Agent;
    else
        return Refuse(outError, "Markup author must be User or Agent");
    return true;
}

std::string_view EntryKindName(MarkupEntryKind kind)
{
    switch (kind)
    {
    case MarkupEntryKind::Created: return "Created";
    case MarkupEntryKind::Comment: return "Comment";
    case MarkupEntryKind::StatusChange: return "StatusChange";
    case MarkupEntryKind::Unreadable: return "Unreadable";
    case MarkupEntryKind::Edit: return "Edit";
    }
    return "Comment";
}

std::string FormatEditChanges(uint8 changes, uint32 colorArgb)
{
    std::string text;
    for (const MarkupEditChange change : MarkupECS::kMarkupEditChangeOrder)
    {
        if ((changes & static_cast<uint8>(change)) == 0)
            continue;
        if (!text.empty())
            text += ' ';
        text += MarkupECS::MarkupEditChangeName(change);
        if (change == MarkupEditChange::Recolored && colorArgb != 0)
        {
            char hex[9];
            std::snprintf(hex, sizeof(hex), "=#%06X", static_cast<unsigned>(colorArgb & 0x00FFFFFFu));
            text += hex;
        }
    }
    return text;
}

// "#RRGGBB" as opaque 0xAARRGGBB; false for anything else.
bool ParseHexColor(std::string_view text, uint32& out)
{
    if (text.size() != 7 || text[0] != '#')
        return false;
    uint32 rgb = 0;
    for (const char c : text.substr(1))
    {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0)
            return false;
        rgb = rgb * 16 + static_cast<uint32>(digit);
    }
    out = 0xFF000000u | rgb;
    return true;
}

bool ParseEditChanges(std::string_view text, uint8& out, uint32& outColorArgb, std::string* outError)
{
    uint8 changes = 0;
    uint32 colorArgb = 0;
    while (!text.empty())
    {
        const size_t space = text.find(' ');
        std::string_view word = text.substr(0, space);
        text = space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
        if (word.empty())
            continue;
        if (const size_t equals = word.find('='); equals != std::string_view::npos)
        {
            if (word.substr(0, equals) != MarkupECS::MarkupEditChangeName(MarkupEditChange::Recolored) ||
                !ParseHexColor(word.substr(equals + 1), colorArgb))
                return Refuse(outError, "Markup Edit entry change '" + std::string(word) +
                                            "' is not a color; write recolored=#RRGGBB");
            word = word.substr(0, equals);
        }
        bool known = false;
        for (const MarkupEditChange change : MarkupECS::kMarkupEditChangeOrder)
        {
            if (word == MarkupECS::MarkupEditChangeName(change))
            {
                changes = static_cast<uint8>(changes | static_cast<uint8>(change));
                known = true;
            }
        }
        if (!known)
            return Refuse(outError, "Markup Edit entry change '" + std::string(word) +
                                        "' is not one of moved, resized, rotated, renamed, reshaped, recolored, described");
    }
    if (changes == 0)
        return Refuse(outError, "A Markup Edit entry names at least one change, such as \"moved\"");
    out = changes;
    outColorArgb = colorArgb;
    return true;
}

bool ParseEntryKind(std::string_view text, MarkupEntryKind& out, std::string* outError)
{
    TrimLeft(text);
    TrimRight(text);
    if (text == "Created")
        out = MarkupEntryKind::Created;
    else if (text == "Comment")
        out = MarkupEntryKind::Comment;
    else if (text == "StatusChange")
        out = MarkupEntryKind::StatusChange;
    else if (text == "Edit")
        out = MarkupEntryKind::Edit;
    else
        return Refuse(outError, "Markup entry kind must be Created, Comment, StatusChange or Edit");
    return true;
}

bool ParseInt64(std::string_view text, int64& out, std::string* outError)
{
    if (ParseIntegerToken(text, out) == IntegerTokenResult::Ok)
        return true;
    return Refuse(outError, "Markup time must be whole Unix seconds");
}

// `<prefix><N>` with N decimal, as the indexed lines name their slot.
bool ParseIndex(std::string_view property, std::string_view prefix, size_t& out)
{
    if (property.size() <= prefix.size() || property.substr(0, prefix.size()) != prefix)
        return false;
    uint32 index = 0;
    if (ParseIntegerToken(property.substr(prefix.size()), index) != IntegerTokenResult::Ok)
        return false;
    out = index;
    return true;
}

// An indexed line may restate a read index or add the next one: `index` <= `size`.
bool CheckIndexInOrder(std::string_view property, size_t index, size_t size, std::string* outError)
{
    if (index <= size)
        return true;
    return Refuse(outError, "Markup." + std::string(property) + " comes before index " + std::to_string(size) +
                                "; indexed lines are numbered in order from 0");
}

std::string TagName(const MarkupService& service, uint32 id)
{
    const MarkupECS::MarkupTag* tag = service.GetTag(id);
    return tag ? tag->Name : std::string();
}

// The status named `name`: a status-group tag, added to the group when the vocabulary
// lacks the name. Refuses an empty name and a name the vocabulary has as a free tag.
bool ResolveStatus(MarkupService& service, const std::string& name, uint32& out, std::string* outError)
{
    const uint32 id = service.AddTag(name, MarkupECS::kMarkupStatusGroup, kAddedTagColor);
    if (id == MarkupService::kInvalidTag)
        return Refuse(outError, "Markup status names no status; write one such as \"Requested\"");
    if (!service.IsStatusTag(id))
        return Refuse(outError, "Markup status '" + name + "' is a free tag, not a status");
    out = id;
    return true;
}

// `Kind|Author|Time|"status"|"text"`; a comment's status is ""; an edit's status is the name a
// rename gave the mark-up and its text its changes.
std::string FormatEntry(const MarkupService& service, const MarkupEntry& entry)
{
    std::string line(EntryKindName(entry.Kind));
    line += '|';
    line += AuthorName(entry.Author);
    line += '|';
    line += std::to_string(entry.TimeUnix);
    line += '|';
    if (entry.Kind == MarkupEntryKind::Edit)
        line += FormatEscaped(entry.Text);
    else
        line += FormatEscaped(entry.Kind == MarkupEntryKind::Comment ? std::string() : TagName(service, entry.Status));
    line += '|';
    line += FormatEscaped(entry.Kind == MarkupEntryKind::Edit ? FormatEditChanges(entry.Changes, entry.ColorArgb) : entry.Text);
    return line;
}

// Reads FormatEntry's line into `out` only when the whole line is valid.
bool ParseEntry(MarkupService& service, std::string_view value, MarkupEntry& out, std::string* outError)
{
    std::string_view fields[3];
    for (std::string_view& field : fields)
    {
        const size_t bar = value.find('|');
        if (bar == std::string_view::npos)
            return Refuse(outError, "Markup entry must be Kind|Author|Time|\"status\"|\"text\"");
        field = value.substr(0, bar);
        value.remove_prefix(bar + 1);
    }

    MarkupEntry entry;
    std::string status;
    if (!ParseEntryKind(fields[0], entry.Kind, outError) || !ParseAuthor(fields[1], entry.Author, outError) ||
        !ParseInt64(fields[2], entry.TimeUnix, outError) || !TakeEscaped(value, status, outError))
        return false;
    TrimLeft(value);
    if (value.empty() || value.front() != '|')
        return Refuse(outError, "Markup entry must be Kind|Author|Time|\"status\"|\"text\"");
    value.remove_prefix(1);
    if (!ParseEscaped(value, entry.Text, outError))
        return false;

    if (entry.Kind == MarkupEntryKind::Edit)
    {
        if (!ParseEditChanges(entry.Text, entry.Changes, entry.ColorArgb, outError))
            return false;
        entry.Text = std::move(status); // the name a rename gave it
    }
    else if (entry.Kind == MarkupEntryKind::Comment)
    {
        if (!status.empty())
            return Refuse(outError, "A Comment entry carries no status; write \"\" in its status field");
    }
    else if (!ResolveStatus(service, status, entry.Status, outError))
    {
        return false;
    }
    out = std::move(entry);
    return true;
}

// A tag line's value: a free tag, added to the vocabulary when it lacks the name.
// Refuses a status name and a name that names no tag.
bool ReadTagLine(MarkupService& service, std::string_view value, uint32& outTag, std::string* outError)
{
    std::string text;
    if (!ParseEscaped(value, text, outError))
        return false;
    const uint32 existing = service.FindTag(text);
    if (existing != MarkupService::kInvalidTag && service.IsStatusTag(existing))
        return Refuse(outError, "Markup tag '" + text + "' is a status; a mark-up's status is its Markup.status line");
    const uint32 tag = service.AddTag(text, {}, kAddedTagColor);
    if (tag == MarkupService::kInvalidTag)
        return Refuse(outError, "Markup tag names no tag; write a name or drop the line");
    outTag = tag;
    return true;
}

bool HasColor(const Markup& markup)
{
    for (const float32 channel : markup.Color)
    {
        if (channel != 0.0f)
            return true;
    }
    return false;
}

class MarkupSceneSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "Markup"; }

    bool IsPresent(const ECS::World& world, ECS::EntityHandle entity) const override
    {
        return world.GetComponent<Markup>(entity) != nullptr;
    }

    // The component's own lines need no service; the status (a tag name) and the notes do,
    // so a world saved without MarkupService keeps the component and its switch state.
    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* markup = world.GetComponent<Markup>(entity);
        if (!markup)
            return;
        const MarkupService* service = MarkupService::TryGet();

        if (service)
            outLines.push_back("Markup.status = " + FormatEscaped(TagName(*service, markup->Status)));
        outLines.push_back("Markup.author = " + std::string(AuthorName(markup->Author)));
        outLines.push_back("Markup.updatedBy = " + std::string(AuthorName(markup->UpdatedBy)));
        outLines.push_back("Markup.created = " + std::to_string(markup->CreatedUnix));
        outLines.push_back("Markup.updated = " + std::to_string(markup->UpdatedUnix));
        outLines.push_back("Markup.revision = " + std::to_string(markup->Revision));
        if (HasColor(*markup))
            outLines.push_back("Markup.color = " +
                               FormatFloat4(markup->Color[0], markup->Color[1], markup->Color[2], markup->Color[3]));

        const MarkupNotes* notes = service ? service->FindNotes(world, entity) : nullptr;
        if (!notes)
            return;
        if (!notes->Description.empty())
            outLines.push_back("Markup.description = " + FormatEscaped(notes->Description));
        // An unreadable slot: the loader preserved its authored text under the slot's key
        // (SetTags moves it when it renumbers the slot) and the save puts it back in place
        // of the line written here.
        for (size_t i = 0; i < notes->Tags.size(); ++i)
            outLines.push_back("Markup.tag" + std::to_string(i) + " = " +
                               (notes->Tags[i] == MarkupService::kInvalidTag
                                    ? std::string("\"\"")
                                    : FormatEscaped(TagName(*service, notes->Tags[i]))));
        for (size_t i = 0; i < notes->Entries.size(); ++i)
            outLines.push_back("Markup.entry" + std::to_string(i) + " = " +
                               (notes->Entries[i].Kind == MarkupEntryKind::Unreadable
                                    ? std::string("Unreadable")
                                    : FormatEntry(*service, notes->Entries[i])));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                       std::string_view property, std::string_view value, std::string* outError) const override
    {
        // A mark-up's notes are not a component, and an instance overrides components only,
        // so a mark-up inside a blueprint or a subscene could not keep its thread.
        if (!ctx.EntityIdPrefix.empty() || (ctx.SceneFile && ctx.SceneFile->extension() == ".blueprint"))
        {
            if (outError)
                *outError = "A mark-up belongs to a scene, not to a blueprint or a subscene; move it into the "
                            "scene that places the instance";
            return false;
        }
        Markup markup{};
        if (const auto* existing = world.GetComponent<Markup>(entity))
            markup = *existing;

        bool applied = false;
        if (!ApplyComponentField(markup, property, value, applied, outError))
            return false;
        MarkupService* service = MarkupService::TryGet();
        if (!applied)
        {
            if (!service)
                return Refuse(outError, "MarkupService not initialized");
            if (!ApplyServiceProperty(*service, world, entity, markup, property, value, outError))
                return false;
        }
        if (property == "revision" && service)
            service->RestoreRevision(world, markup.Revision);

        world.AddComponentImmediate(entity, markup);
        return true;
    }

  private:
    // The lines that set a component field directly. `applied` is false for any other property.
    static bool ApplyComponentField(Markup& markup, std::string_view property, std::string_view value,
                                    bool& applied, std::string* outError)
    {
        applied = true;
        if (property == "author")
            return ParseAuthor(value, markup.Author, outError);
        if (property == "updatedby")
            return ParseAuthor(value, markup.UpdatedBy, outError);
        if (property == "created")
            return ParseInt64(value, markup.CreatedUnix, outError);
        if (property == "updated")
            return ParseInt64(value, markup.UpdatedUnix, outError);
        if (property == "revision")
        {
            if (ParseIntegerToken(value, markup.Revision) == IntegerTokenResult::Ok)
                return true;
            return Refuse(outError, "Markup revision must be a whole number");
        }
        if (property == "color")
        {
            Float4 color{};
            if (!ParseFloat4(value, color))
                return Refuse(outError, "Markup color must be (r, g, b, a)");
            markup.Color[0] = color.X;
            markup.Color[1] = color.Y;
            markup.Color[2] = color.Z;
            markup.Color[3] = color.W;
            return true;
        }
        applied = false;
        return true;
    }

    // The lines that name a tag or carry the notes. Each reads its whole value before it
    // writes, so a refused line changes nothing.
    static bool ApplyServiceProperty(MarkupService& service, const ECS::World& world, ECS::EntityHandle entity,
                                     Markup& markup, std::string_view property, std::string_view value,
                                     std::string* outError)
    {
        std::string text;
        size_t index = 0;
        if (property == "status")
        {
            uint32 status = 0;
            if (!ParseEscaped(value, text, outError) || !ResolveStatus(service, text, status, outError))
                return false;
            markup.Status = status;
            return true;
        }
        if (property == "description")
        {
            if (!ParseEscaped(value, text, outError))
                return false;
            service.EnsureNotes(world, entity).Description = std::move(text);
            return true;
        }
        if (ParseIndex(property, "tag", index))
        {
            const MarkupNotes* notes = service.FindNotes(world, entity);
            if (!CheckIndexInOrder(property, index, notes ? notes->Tags.size() : 0, outError))
                return false;
            uint32 tag = MarkupService::kInvalidTag;
            const bool read = ReadTagLine(service, value, tag, outError);
            std::vector<uint32>& tags = service.EnsureNotes(world, entity).Tags;
            if (index == tags.size())
                tags.push_back(read ? tag : MarkupService::kInvalidTag);
            else if (read)
                tags[index] = tag;
            return read;
        }
        if (ParseIndex(property, "entry", index))
        {
            const MarkupNotes* notes = service.FindNotes(world, entity);
            if (!CheckIndexInOrder(property, index, notes ? notes->Entries.size() : 0, outError))
                return false;
            MarkupEntry entry;
            const bool read = ParseEntry(service, value, entry, outError);
            if (!read)
                entry = MarkupEntry{MarkupEntryKind::Unreadable};
            std::vector<MarkupEntry>& entries = service.EnsureNotes(world, entity).Entries;
            if (index == entries.size())
                entries.push_back(std::move(entry));
            else if (read)
                entries[index] = std::move(entry);
            return read;
        }
        return Refuse(outError, "Unknown Markup property");
    }
};

GE_REGISTER_SCENE_SCHEMA(MarkupSceneSchema)

} // namespace
} // namespace GameEngine::Scene
