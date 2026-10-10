#pragma once

#include "AgentCallMedia.h"
#include "AgentCallPieces.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
struct AssistantAction;
struct AssistantUndoSnapshot;
struct ToolCall;

/// One row under a reply for a call its assistant made, or for consecutive calls of one
/// tool with the same shape and outcome, folded ("Lantern_02, Lantern_03 (2 calls)").
struct AgentCallRowModel
{
    /// The glyph before the action.
    enum class Mark : uint8_t
    {
        /// A call the editor never received (the CLI reported it, and refused or ran it
        /// itself).
        None,
        /// Ran and changed something: an edit, injected input, a gated action.
        Done,
        /// Ran and changed nothing but, at most, the view: a read or a view change.
        ViewOnly,
        /// Did not run: refused, declined, cancelled, or the editor refused it.
        Refused,
        /// Waits for the user's answer or for the user's own edit to end.
        Waiting,
    };

    Mark Glyph = Mark::None;
    /// The action in words ("Set component").
    std::string Action;
    /// What it acted on (AgentCallSummary: entities as links, vectors with their axes),
    /// and for a call that did not run, why: "(refused)" and the reason when the editor's
    /// gate said no, "(failed)" and the error when the editor failed it.
    AgentCallPieces Summary;
    /// What it left in the undo history, shown at the row's end: "Undo step: Assistant:
    /// Create Entity (Lantern_01)", "3 undo steps", "undone", "no longer in the undo
    /// history", "not an undo step", "nothing changed", "a comment stays in the thread";
    /// empty for a read and while waiting.
    std::string UndoState;
    /// The raw call and the editor's response, pretty-printed one line each, behind the
    /// row's Details.
    std::vector<AgentCallPieces> Details;
    /// The call waits for the user's answer: the row offers Allow, Allow for this turn and
    /// Deny for RequestId.
    bool Asks = false;
    /// While waiting, what for: "Waiting for your answer", "Waiting for your edit to finish".
    std::string Waiting;
    /// The first call the row shows (AssistantAction::RequestId); 0 for a call the editor
    /// never received.
    uint64_t RequestId = 0;
    /// The image the call's result names (a screenshot, a captured render-graph texture),
    /// shown inline under the row.
    std::optional<AgentCallImage> Image;
    /// The assets the call names (AgentCallMedia::Resources), shown under the row as their
    /// thumbnails with Open.
    std::vector<std::string> Resources;

    bool operator==(const AgentCallRowModel&) const = default;

    /// The rows for the calls of turn `turn`: each call the editor recorded in `actions`
    /// (the conversation's ledger), in arrival order and folded (a call with an image or
    /// assets keeps a row of its own), its step read in
    /// `history`, then each call of `streamCalls` (the provider's report) the ledger has no
    /// record of (a call the CLI refused itself, one that failed before the editor, or a
    /// provider without the editor's tools). A call the CLI refused matches no record;
    /// any other matches a record of its tool with the same arguments and outcome first,
    /// then with the same arguments, then by order.
    static std::vector<AgentCallRowModel> For(std::span<const ToolCall> streamCalls,
                                              std::span<const AssistantAction> actions, uint64_t turn,
                                              const AssistantUndoSnapshot& history);
    /// The counts a reply's footer shows for turn `turn` ("5 undo steps · 1 refused · 1
    /// failed"): refused and failed apart, as the rows word them, the calls only the
    /// provider reported (`streamCalls`) included; empty when the turn made no undo step
    /// and every call ran.
    static std::string Counts(std::span<const ToolCall> streamCalls, std::span<const AssistantAction> actions,
                              uint64_t turn);
};
} // namespace GameEngine
