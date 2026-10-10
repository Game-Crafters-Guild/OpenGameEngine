#include "AgentCallRows.h"

#include "AgentCallPieces.h"
#include "AssistantActionLedger.h"
#include "AssistantTools.h"
#include "AssistantTurnUndo.h"
#include "ConversationMessage.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <string_view>

namespace GameEngine
{
namespace
{
using Mark = AgentCallRowModel::Mark;

// The thread entry markup_comment adds is not an undo step, by design.
constexpr std::string_view kCommentTool = "markup_comment";

// A tool's bare name, as the ledger records it; a name the table does not have as given.
std::string_view BareName(std::string_view name)
{
    const AssistantTool* tool = AssistantTools::Find(name);
    return tool ? tool->Name : name;
}

std::string ActionWords(std::string_view name)
{
    const AssistantTool* tool = AssistantTools::Find(name);
    return std::string(tool ? tool->Action : name);
}

Mark MarkOf(const AssistantAction& action)
{
    switch (action.State)
    {
    case AssistantActionState::Waiting:
        return Mark::Waiting;
    case AssistantActionState::Done:
        return action.Class == AssistantToolClass::Read || action.Class == AssistantToolClass::View ? Mark::ViewOnly
                                                                                                  : Mark::Done;
    case AssistantActionState::Failed:
    case AssistantActionState::Refused:
    case AssistantActionState::Declined:
    case AssistantActionState::Cancelled:
        break;
    }
    return Mark::Refused;
}

// What the row adds after the subject: why a call did not run, what a waiting call is, or
// that it ran because the user allowed it.
// "(refused)" when the gate said no, "(failed)" when the editor answered with an error.
std::string Outcome(const AssistantAction& action)
{
    switch (action.State)
    {
    case AssistantActionState::Failed:
        return "(failed) " + action.Message;
    case AssistantActionState::Refused:
        return "(refused) " + action.Message;
    case AssistantActionState::Cancelled:
        return action.Message;
    case AssistantActionState::Declined:
        return "you declined";
    case AssistantActionState::Waiting:
        return action.Class == AssistantToolClass::Gated ? "not an undo step" : std::string();
    case AssistantActionState::Done:
        break;
    }
    // A call that ran because the user allowed it says so, as a declined one does.
    switch (action.Answer)
    {
    case AssistantAnswer::Allow:
        return "you allowed";
    case AssistantAnswer::AllowForTurn:
        return "allowed for this turn";
    case AssistantAnswer::None:
    case AssistantAnswer::Deny:
        break;
    }
    return {};
}

// The words a row adds for a call the editor never received.
constexpr std::string_view kRefusedByCli = "(refused) Not allowed for the assistant";

// Where a call's undo step is now.
enum class StepPlace
{
    None,
    Undo,
    Redo,
    Gone,
};

StepPlace PlaceOf(const AssistantAction& action, const AssistantUndoSnapshot& history)
{
    if (action.UndoEntryId == 0)
        return StepPlace::None;
    const auto holds = [&action](const std::vector<uint64_t>& stack) {
        return std::find(stack.begin(), stack.end(), action.UndoEntryId) != stack.end();
    };
    if (holds(history.Undo))
        return StepPlace::Undo;
    return holds(history.Redo) ? StepPlace::Redo : StepPlace::Gone;
}

std::string UndoState(const AssistantAction& action, const AssistantUndoSnapshot& history)
{
    if (action.State == AssistantActionState::Waiting)
        return {};
    if (action.State != AssistantActionState::Done)
        return "nothing changed";
    switch (PlaceOf(action, history))
    {
    case StepPlace::Undo:
        return "Undo step: " + action.UndoName;
    case StepPlace::Redo:
        return "undone";
    case StepPlace::Gone:
        return "no longer in the undo history";
    case StepPlace::None:
        break;
    }
    if (action.Tool == kCommentTool)
        return "a comment stays in the thread";
    switch (action.Class)
    {
    case AssistantToolClass::Read:
        return {};
    case AssistantToolClass::UndoableEdit:
        return "nothing changed";
    case AssistantToolClass::View:
    case AssistantToolClass::Input:
    case AssistantToolClass::Gated:
    case AssistantToolClass::Denied:
        break;
    }
    return "not an undo step";
}

std::string Join(const std::string& first, const std::string& second, std::string_view separator)
{
    if (first.empty())
        return second;
    if (second.empty())
        return first;
    return first + std::string(separator) + second;
}

// Appends `text` pretty-printed (AgentCallJsonLines), or as it is when it is not JSON (a
// response cut at the ledger's limit).
void AppendJsonLines(std::vector<AgentCallPieces>& lines, std::string_view text)
{
    const nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
    if (value.is_discarded())
    {
        lines.push_back({AgentCallPiece::Words(std::string(text))});
        return;
    }
    for (AgentCallPieces& line : AgentCallJsonLines(value))
        lines.push_back(std::move(line));
}

// The raw call and the editor's answer, pretty-printed.
std::vector<AgentCallPieces> DetailLines(std::string_view tool, std::string_view arguments, const AssistantAction* action)
{
    std::vector<AgentCallPieces> lines{{AgentCallPiece::MutedWords("Call"), AgentCallPiece::Words(std::string(tool))}};
    AppendJsonLines(lines, arguments);
    if (!action)
        return lines;
    if (!action->Result.empty())
    {
        lines.push_back({AgentCallPiece::MutedWords("Result")});
        AppendJsonLines(lines, action->Result);
    }
    else if (!action->Message.empty())
    {
        lines.push_back({AgentCallPiece::MutedWords("Answer"), AgentCallPiece::Words(action->Message)});
    }
    return lines;
}

// The row's summary: the call's subject, then why it did not run or what it waits as.
AgentCallPieces SummaryOf(const AssistantAction& action)
{
    AgentCallPieces summary = AgentCallSummary(action);
    if (std::string outcome = Outcome(action); !outcome.empty())
    {
        if (!summary.empty())
            summary.push_back(AgentCallPiece::MutedWords("·"));
        summary.push_back(AgentCallPiece::Words(std::move(outcome)));
    }
    return summary;
}

AgentCallRowModel RowOf(const AssistantAction& action, AgentCallMedia media, const AssistantUndoSnapshot& history)
{
    AgentCallRowModel row;
    row.Glyph = MarkOf(action);
    row.Action = ActionWords(action.Tool);
    row.Summary = SummaryOf(action);
    row.UndoState = UndoState(action, history);
    row.Details = DetailLines(action.Tool, action.Arguments, &action);
    row.RequestId = action.RequestId;
    row.Image = std::move(media.Image);
    row.Resources = std::move(media.Resources);
    if (action.State == AssistantActionState::Waiting)
    {
        row.Asks = action.WaitingFor == AssistantWaitReason::Answer;
        row.Waiting = row.Asks ? "Waiting for your answer" : "Waiting for your edit to finish";
    }
    return row;
}

// The same arguments, compared as JSON (key order and spacing aside); as text when either
// side is not JSON.
bool SameArguments(std::string_view reported, std::string_view recorded)
{
    const nlohmann::json left = nlohmann::json::parse(reported, nullptr, false);
    const nlohmann::json right = nlohmann::json::parse(recorded, nullptr, false);
    if (left.is_discarded() || right.is_discarded())
        return reported == recorded;
    return left == right;
}

// How a reported call is matched to a record of its tool (MatchRecords), strictest first.
enum class MatchBy : uint8_t
{
    // The same arguments and the same outcome: a call that failed takes a failed record.
    ArgumentsAndOutcome,
    Arguments,
    Order,
};

// The record ended as the reported call did: failed both, or neither failed.
bool SameOutcome(const ToolCall& call, const AssistantAction& record)
{
    return (call.End == ToolCallEnd::Failed) == (record.State == AssistantActionState::Failed);
}

// A record no reported call matched.
constexpr size_t kNoCall = static_cast<size_t>(-1);

// Gives each call of `calls` not yet matched and not refused by the CLI the first record of
// its tool in `records` not yet taken that `by` allows; `callOfRecord` holds each taken
// record's call.
void MatchRecords(std::span<const ToolCall> calls, std::span<const AssistantAction* const> records,
                  std::vector<bool>& matched, std::vector<size_t>& callOfRecord, MatchBy by)
{
    for (size_t call = 0; call < calls.size(); ++call)
    {
        if (matched[call] || calls[call].End == ToolCallEnd::Refused)
            continue;
        const std::string_view name = BareName(calls[call].Name);
        for (size_t record = 0; record < records.size(); ++record)
        {
            if (callOfRecord[record] != kNoCall || records[record]->Tool != name)
                continue;
            if (by != MatchBy::Order && !SameArguments(calls[call].InputJson, records[record]->Arguments))
                continue;
            if (by == MatchBy::ArgumentsAndOutcome && !SameOutcome(calls[call], *records[record]))
                continue;
            callOfRecord[record] = call;
            matched[call] = true;
            break;
        }
    }
}

// Which of the turn's calls the editor has a record of. A call the CLI refused never
// reached the editor. Any other call takes the turn's first record of its tool not yet
// taken with the same arguments and outcome, then with the same arguments, then the first
// one not yet taken: a call that failed before the editor (an argument the server rejected,
// a dropped connection) leaves the record to its retry, even one with the same arguments.
struct CallMatch
{
    /// Turn `turn`'s records, in the order the editor made them.
    std::vector<const AssistantAction*> Records;
    /// Per record, the index in `streamCalls` of the call it shows; kNoCall for a record no
    /// reported call matched.
    std::vector<size_t> CallOfRecord;
    /// Per call of `streamCalls`, whether a record shows it.
    std::vector<bool> Matched;
};

CallMatch MatchCalls(std::span<const ToolCall> streamCalls, std::span<const AssistantAction> actions, uint64_t turn)
{
    CallMatch match;
    for (const AssistantAction& action : actions)
        if (action.Turn == turn)
            match.Records.push_back(&action);
    match.CallOfRecord.assign(match.Records.size(), kNoCall);
    match.Matched.assign(streamCalls.size(), false);
    for (MatchBy by : {MatchBy::ArgumentsAndOutcome, MatchBy::Arguments, MatchBy::Order})
        MatchRecords(streamCalls, match.Records, match.Matched, match.CallOfRecord, by);
    return match;
}

// The calls of `streamCalls` the editor has no record of in turn `turn` (MatchCalls).
std::vector<const ToolCall*> UnreceivedCalls(std::span<const ToolCall> streamCalls,
                                             std::span<const AssistantAction> actions, uint64_t turn)
{
    const CallMatch match = MatchCalls(streamCalls, actions, turn);
    std::vector<const ToolCall*> unreceived;
    for (size_t call = 0; call < streamCalls.size(); ++call)
        if (!match.Matched[call])
            unreceived.push_back(&streamCalls[call]);
    return unreceived;
}

// A row for a call the editor never received: the CLI refused it, or it failed before or
// outside the editor ("(failed)" and the first line of its error).
AgentCallRowModel RowOfUnreceived(const ToolCall& call)
{
    const std::string_view name = BareName(call.Name);
    AgentCallRowModel row;
    row.Action = ActionWords(name);
    row.Summary = AgentCallSummary(name, call.InputJson);
    if (call.End != ToolCallEnd::Unknown)
    {
        if (!row.Summary.empty())
            row.Summary.push_back(AgentCallPiece::MutedWords("·"));
        const std::string outcome = call.End == ToolCallEnd::Refused ? std::string(kRefusedByCli)
                                    : call.Error.empty()                ? std::string("(failed)")
                                                                        : "(failed) " + call.Error;
        row.Summary.push_back(AgentCallPiece::Words(outcome));
        row.Glyph = Mark::Refused;
        row.UndoState = "nothing changed";
    }
    row.Details = DetailLines(call.Name, call.InputJson, nullptr);
    if (!call.Error.empty())
        row.Details.push_back({AgentCallPiece::MutedWords("Answer"), AgentCallPiece::Words(call.Error)});
    return row;
}

// The names of a call's arguments: calls of one tool fold only when they name the same.
std::string ArgumentShape(const AssistantAction& action)
{
    const nlohmann::json arguments = nlohmann::json::parse(action.Arguments, nullptr, false);
    std::string shape;
    if (arguments.is_object())
        for (const auto& [key, value] : arguments.items())
            shape += key + ",";
    return shape;
}

// `next` folds into the row that shows `last`: both ran, one tool, the same arguments
// named, and their steps (if any) in the same place. A call with an image or assets
// (AgentCallMedia) keeps a row of its own; the caller checks that.
bool Folds(const AssistantAction& last, const AssistantAction& next, const AssistantUndoSnapshot& history)
{
    return last.State == AssistantActionState::Done && next.State == AssistantActionState::Done &&
           last.Tool == next.Tool && last.Answer == next.Answer && PlaceOf(last, history) == PlaceOf(next, history) &&
           ArgumentShape(last) == ArgumentShape(next);
}

// Shows `calls` consecutive calls in `row`, the last of them `next`; `subjects` holds
// the summaries of the calls before it, comma-separated.
void Fold(AgentCallRowModel& row, AgentCallPieces& subjects, AgentCallPieces& lastSubject, const AssistantAction& next,
          size_t calls, const AssistantUndoSnapshot& history)
{
    // A subject already listed (the same file saved twice) is not repeated.
    const AgentCallPieces summary = AgentCallSummary(next);
    if (summary != lastSubject)
    {
        if (!subjects.empty() && !summary.empty())
            subjects.push_back(AgentCallPiece::MutedWords(","));
        subjects.insert(subjects.end(), summary.begin(), summary.end());
        lastSubject = summary;
    }
    row.Summary = subjects;
    row.Summary.push_back(AgentCallPiece::MutedWords("(" + std::to_string(calls) + " calls)"));
    if (const std::string outcome = Outcome(next); !outcome.empty())
    {
        row.Summary.push_back(AgentCallPiece::MutedWords("·"));
        row.Summary.push_back(AgentCallPiece::Words(outcome));
    }
    if (PlaceOf(next, history) == StepPlace::Undo)
        row.UndoState = std::to_string(calls) + " undo steps";
    std::vector<AgentCallPieces> details = DetailLines(next.Tool, next.Arguments, &next);
    row.Details.push_back({});
    row.Details.insert(row.Details.end(), std::make_move_iterator(details.begin()),
                       std::make_move_iterator(details.end()));
}
} // namespace

std::vector<AgentCallRowModel> AgentCallRowModel::For(std::span<const ToolCall> streamCalls,
                                                      std::span<const AssistantAction> actions, uint64_t turn,
                                                      const AssistantUndoSnapshot& history)
{
    std::vector<AgentCallRowModel> rows;
    // Rows follow call order: a call the editor never received sits before the record of a
    // later call (a failed call above its retry). Records no reported call matched keep their
    // place among the others.
    const CallMatch match = MatchCalls(streamCalls, actions, turn);
    size_t nextCall = 0;
    // The call whose row takes folds; null after a row with media or a call the editor never
    // received, which take none.
    const AssistantAction* folding = nullptr;
    AgentCallPieces foldedSubjects;
    AgentCallPieces lastSubject;
    size_t folded = 0;
    for (size_t record = 0; record < match.Records.size(); ++record)
    {
        const AssistantAction& action = *match.Records[record];
        if (const size_t call = match.CallOfRecord[record]; call != kNoCall)
        {
            for (; nextCall < call; ++nextCall)
            {
                if (match.Matched[nextCall])
                    continue;
                rows.push_back(RowOfUnreceived(streamCalls[nextCall]));
                folding = nullptr;
            }
            nextCall = std::max(nextCall, call + 1);
        }
        AgentCallMedia media = AgentCallMediaOf(action);
        const bool showsMedia = !media.Empty();
        if (folding && !showsMedia && Folds(*folding, action, history))
        {
            Fold(rows.back(), foldedSubjects, lastSubject, action, ++folded, history);
            continue;
        }
        rows.push_back(RowOf(action, std::move(media), history));
        folding = showsMedia ? nullptr : &action;
        foldedSubjects = AgentCallSummary(action);
        lastSubject = foldedSubjects;
        folded = 1;
    }
    for (; nextCall < streamCalls.size(); ++nextCall)
        if (!match.Matched[nextCall])
            rows.push_back(RowOfUnreceived(streamCalls[nextCall]));
    return rows;
}

std::string AgentCallRowModel::Counts(std::span<const ToolCall> streamCalls, std::span<const AssistantAction> actions,
                                      uint64_t turn)
{
    size_t steps = 0;
    size_t refused = 0;
    size_t failed = 0;
    size_t declined = 0;
    size_t cancelled = 0;
    for (const AssistantAction& action : actions)
    {
        if (action.Turn != turn)
            continue;
        steps += action.UndoEntryId != 0 ? 1 : 0;
        refused += action.State == AssistantActionState::Refused ? 1 : 0;
        failed += action.State == AssistantActionState::Failed ? 1 : 0;
        declined += action.State == AssistantActionState::Declined ? 1 : 0;
        cancelled += action.State == AssistantActionState::Cancelled ? 1 : 0;
    }
    for (const ToolCall* call : UnreceivedCalls(streamCalls, actions, turn))
    {
        refused += call->End == ToolCallEnd::Refused ? 1 : 0;
        failed += call->End == ToolCallEnd::Failed ? 1 : 0;
    }
    std::string counts;
    if (steps > 0)
        counts = std::to_string(steps) + (steps == 1 ? " undo step" : " undo steps");
    if (refused > 0)
        counts = Join(counts, std::to_string(refused) + " refused", " · ");
    if (failed > 0)
        counts = Join(counts, std::to_string(failed) + " failed", " · ");
    if (declined > 0)
        counts = Join(counts, std::to_string(declined) + " declined", " · ");
    if (cancelled > 0)
        counts = Join(counts, std::to_string(cancelled) + " cancelled", " · ");
    return counts;
}
} // namespace GameEngine
