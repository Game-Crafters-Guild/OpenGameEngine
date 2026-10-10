#include "AssistantGate.h"

#include "AssistantTools.h"

#include "AssetCore/GUID.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Logger/Logger.h"
#include "UndoRedo/UndoRedoService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cwctype>
#include <utility>

namespace GameEngine
{
namespace
{
using Editor::DebugRequestDecision;
using Editor::DebugRequestGateContext;
using Editor::DebugRequestVerdict;

constexpr std::string_view kBindMethod = "assistant_bind";
constexpr std::string_view kAuthorizeMethod = "assistant_authorize";
// The arguments through which a tool names a file or a directory it reads or writes
// (mcp/src/tools/ecs-authoring.ts, game-graph.ts; save_scene's path).
constexpr std::string_view kPathArguments[] = {"path", "directory", "outputPath", "outputDir"};

// The answers a waiting call ends with when it never runs.
constexpr const char* kSessionEnded = "Cancelled: the session ended";
constexpr const char* kNoAnswer = "The user did not answer within 10 minutes";
constexpr const char* kEditNotFinished = "The user's own edit in the editor did not end within 10 minutes";
static_assert(AssistantGate::kWaitLimit == std::chrono::minutes(10), "the waiting messages name the limit");

// The undo step each undoable tool records (the editor's own name for the edit, with
// the subject), "{argument}" replaced by the call's value.
struct UndoStepPattern
{
    std::string_view Tool;
    std::string_view Pattern;
};
constexpr UndoStepPattern kUndoSteps[] = {
    {"create_entity", "Create Entity ({name})"},     {"set_component", "Set {component} ({entityId})"},
    {"delete_entity", "Delete Entity ({entityId})"}, {"markup_create", "Create Mark-up ({title})"},
    {"markup_update", "Update Mark-up ({entityId})"}, {"markup_comment", "Comment on Mark-up ({entityId})"},
};

// What a mode does with a call of a class.
enum class Admission
{
    Runs,
    Asks,
    Refused,
};

Admission AdmissionOf(AssistantMode mode, AssistantToolClass toolClass)
{
    switch (toolClass)
    {
    case AssistantToolClass::Read:
        return Admission::Runs;
    case AssistantToolClass::View:
        return mode == AssistantMode::ReadOnly ? Admission::Refused : Admission::Runs;
    case AssistantToolClass::UndoableEdit:
    case AssistantToolClass::Input:
        if (mode == AssistantMode::ReadOnly)
            return Admission::Refused;
        return mode == AssistantMode::AskBeforeEveryEdit ? Admission::Asks : Admission::Runs;
    case AssistantToolClass::Gated:
        if (mode == AssistantMode::ReadOnly)
            return Admission::Refused;
        return mode == AssistantMode::EditFreely ? Admission::Runs : Admission::Asks;
    case AssistantToolClass::Denied:
        return Admission::Refused;
    }
    return Admission::Refused;
}

DebugRequestVerdict Refuse(std::string reason)
{
    return {DebugRequestDecision::Refuse, std::move(reason)};
}

// The string member `key` of `params`, empty when absent.
std::string StringParam(const nlohmann::json* params, const char* key)
{
    if (!params || !params->is_object())
        return {};
    const auto it = params->find(key);
    return it != params->end() && it->is_string() ? it->get<std::string>() : std::string();
}

// `path` lexically normalized, and on Windows lowercased, where paths compare without case.
std::filesystem::path Comparable(const std::filesystem::path& path)
{
    std::filesystem::path normal = path.lexically_normal();
#if defined(_WIN32)
    std::wstring text = normal.wstring();
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return std::towlower(c); });
    normal = text;
#endif
    return normal;
}

// `value` resolved as the MCP server resolves it (relative to the project, an absolute
// path as given) lies inside `projectRoot`.
bool IsInsideProject(const std::filesystem::path& projectRoot, const std::string& value)
{
    if (projectRoot.empty())
        return false;
    std::filesystem::path full = std::filesystem::path(value);
    if (!full.is_absolute())
        full = projectRoot / full;
    const std::filesystem::path relative = Comparable(full).lexically_relative(Comparable(projectRoot));
    return !relative.empty() && *relative.begin() != "..";
}

// The refusal for a call of `tool` whose path arguments leave `projectRoot`; empty when
// every path argument stays inside.
std::string PathOutsideProject(std::string_view tool, const std::filesystem::path& projectRoot,
                               const nlohmann::json* arguments)
{
    if (!arguments || !arguments->is_object())
        return {};
    for (std::string_view key : kPathArguments)
    {
        const auto value = arguments->find(key);
        if (value == arguments->end() || !value->is_string() || IsInsideProject(projectRoot, value->get<std::string>()))
            continue;
        return std::string(tool) + ": " + std::string(key) + " '" + value->get<std::string>() +
               "' is outside the project (" + projectRoot.string() +
               "): the assistant writes project files only; use a path inside the project, relative to its root.";
    }
    return {};
}

// The call a request carries. An assistant_authorize names a tool that runs in the MCP
// server's process and the arguments it was called with.
struct Call
{
    const AssistantTool* Tool = nullptr;
    std::string Name;
    const nlohmann::json* Arguments = nullptr;
};

Call CallOf(const DebugRequestGateContext& context)
{
    Call call;
    if (context.Method == kAuthorizeMethod)
    {
        call.Name = StringParam(context.Params, "tool");
        call.Tool = AssistantTools::Find(call.Name);
        if (context.Params && context.Params->is_object())
            if (const auto it = context.Params->find("arguments"); it != context.Params->end())
                call.Arguments = &*it;
        return call;
    }
    call.Tool = AssistantTools::FindByMethod(context.Method);
    call.Name = call.Tool ? std::string(call.Tool->Name) : std::string(context.Method);
    call.Arguments = context.Params;
    return call;
}

// The decision for `call` in `session`'s state: no turn, the mode and class, then the
// project bound on paths.
DebugRequestVerdict DecideInSession(AssistantMode mode, uint64_t turn, const std::filesystem::path& projectRoot,
                                    std::string_view method, const Call& call)
{
    if (turn == 0)
        return Refuse("No turn is running in this conversation.");
    DebugRequestVerdict verdict = DecideAssistantCall(mode, call.Name, call.Tool, call.Arguments);
    if (verdict.Decision == DebugRequestDecision::Refuse)
        return verdict;
    if (method == kAuthorizeMethod && projectRoot.empty())
        return Refuse(call.Name + " reads or writes project files, and no project is open in the editor: open a "
                                  "project, then ask again.");
    // A host-side tool's paths, and an editor method that writes a file (save_scene's
    // path), are bounded alike.
    if (std::string outside = PathOutsideProject(method == kAuthorizeMethod ? std::string_view(call.Name) : method,
                                                 projectRoot, call.Arguments);
        !outside.empty())
        return Refuse(std::move(outside));
    return verdict;
}

// The action in words with its subject, as the user saw it on its row.
std::string ActionText(const AssistantAction& action)
{
    const AssistantTool* tool = AssistantTools::Find(action.Tool);
    std::string text = tool ? std::string(tool->Action) : action.Tool;
    if (!action.Subject.empty())
        text += " (" + action.Subject + ")";
    return text;
}

// An edit the user's open drag would overwrite on commit, or one that cannot be undone,
// waits until the drag ends.
bool WaitsForUsersEdit(AssistantToolClass toolClass)
{
    return toolClass == AssistantToolClass::UndoableEdit || toolClass == AssistantToolClass::Gated;
}

// `text` cut to at most `maxBytes`, on a UTF-8 character boundary.
std::string CutUtf8(std::string text, size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return text;
    size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    text.resize(end);
    return text;
}

// An argument as an undo step's name shows it: an entity id as the entity's name in the
// edit world when it has one (EntityDisplayName), a string bare, anything else as JSON.
std::string ArgumentInName(std::string_view key, const nlohmann::json& value)
{
    const ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (key == "entityId" && value.is_number_unsigned() && world)
    {
        const ECS::EntityHandle entity(value.get<uint32_t>());
        if (world->IsValid(entity))
            return Editor::EntityDisplayName(*world, entity);
    }
    return value.is_string() ? value.get<std::string>() : value.dump();
}

uint64_t TopEntryId(const Editor::UndoRedoService& undo)
{
    const size_t count = undo.GetUndoCount();
    return count == 0 ? 0 : undo.GetUndoEntryIdAt(count - 1);
}

// The most of a call's subject a log line carries, in bytes.
constexpr size_t kLogSubjectBytes = 120;

// `text` cut for a log line: at most kLogSubjectBytes, an ellipsis where it was cut, so a
// refusal that quotes a path argument carries no more of it than a subject would.
std::string LogWords(const std::string& text)
{
    std::string cut = CutUtf8(text, kLogSubjectBytes);
    if (cut.size() < text.size())
        cut += "\xE2\x80\xA6";
    return cut;
}

std::string LogOutcome(const AssistantAction& action)
{
    switch (action.State)
    {
    case AssistantActionState::Waiting:
        return "parked";
    case AssistantActionState::Done:
        return "ran";
    case AssistantActionState::Failed:
        return "failed: " + LogWords(action.Message);
    case AssistantActionState::Refused:
        return "refused: " + LogWords(action.Message);
    case AssistantActionState::Declined:
        return "declined";
    case AssistantActionState::Cancelled:
        break;
    }
    return "cancelled";
}
} // namespace

// One admitted edit's undo step: a compound open from Before to Handled, so the edits its
// handler makes become one entry; an empty compound pushes nothing. The destructor closes
// a step that was never closed.
class AssistantGate::UndoStep
{
public:
    UndoStep(Editor::UndoRedoService& undo, uint64_t requestId, std::string name)
        : m_Undo(undo)
        , m_RequestId(requestId)
        , m_Name(std::move(name))
        , m_TopBefore(TopEntryId(undo))
    {
        m_Undo.BeginCompound(m_Name);
    }
    ~UndoStep()
    {
        if (!m_Closed)
            m_Undo.EndCompound();
    }
    UndoStep(const UndoStep&) = delete;
    UndoStep& operator=(const UndoStep&) = delete;

    uint64_t RequestId() const { return m_RequestId; }
    const std::string& Name() const { return m_Name; }
    /// Ends the step: the id of the entry it pushed, 0 when the call changed nothing.
    uint64_t Close()
    {
        m_Closed = true;
        m_Undo.EndCompound();
        const uint64_t top = TopEntryId(m_Undo);
        // Ids only grow: a newer top is the step this call pushed.
        return top > m_TopBefore ? top : 0;
    }

private:
    Editor::UndoRedoService& m_Undo;
    uint64_t m_RequestId;
    std::string m_Name;
    uint64_t m_TopBefore;
    bool m_Closed = false;
};

DebugRequestVerdict DecideAssistantCall(AssistantMode mode, std::string_view requested, const AssistantTool* tool,
                                        const nlohmann::json* arguments)
{
    if (!tool)
        return Refuse(std::string(requested) + " is not one of the assistant's tools.");
    const std::string name(tool->Name);
    if (tool->Class == AssistantToolClass::Denied)
        return Refuse(name + " is not available to the assistant: " + std::string(tool->DeniedReason) + ".");
    if (tool->Name == "take_screenshot" && arguments && arguments->is_object() &&
        arguments->value("allowWindowCapture", false))
        return Refuse("Window captures are not available to the assistant, only the editor's own rendered "
                      "frame: call take_screenshot without allowWindowCapture.");
    switch (AdmissionOf(mode, tool->Class))
    {
    case Admission::Runs:
        return {};
    case Admission::Asks:
        return {DebugRequestDecision::Park, {}};
    case Admission::Refused:
        break;
    }
    // Every class but Denied runs or asks in Auto.
    return Refuse(name + " is not available in " + std::string(DescribeAssistantMode(mode).Label) +
                  ": ask the user to switch the conversation to " +
                  std::string(DescribeAssistantMode(AssistantMode::Auto).Label) + ".");
}

std::string AssistantUndoStepName(const AssistantTool& tool, const nlohmann::json* arguments)
{
    const auto pattern = std::find_if(std::begin(kUndoSteps), std::end(kUndoSteps),
                                      [&tool](const UndoStepPattern& step) { return step.Tool == tool.Name; });
    if (pattern == std::end(kUndoSteps))
        return "Assistant: " + std::string(tool.Action);
    std::string name;
    const std::string_view text = pattern->Pattern;
    for (size_t at = 0; at < text.size(); ++at)
    {
        const size_t close = text[at] == '{' ? text.find('}', at) : std::string_view::npos;
        if (close == std::string_view::npos)
        {
            name += text[at];
            continue;
        }
        const std::string key(text.substr(at + 1, close - at - 1));
        if (arguments && arguments->is_object())
            if (const auto value = arguments->find(key); value != arguments->end() && !value->is_null())
                name += ArgumentInName(key, *value);
        at = close;
    }
    // A subject whose argument is absent leaves nothing in its parentheses.
    if (name.ends_with(" ()"))
        name.resize(name.size() - 3);
    return "Assistant: " + name;
}

std::string AssistantActionLogLine(AssistantGate::SessionId session, const AssistantAction& action)
{
    const std::string subject = LogWords(action.Subject);
    std::string line = "AI Assistant: conversation " + std::to_string(session) + " turn " +
                       std::to_string(action.Turn) + ": " + action.Tool + (subject.empty() ? "" : " " + subject) +
                       " -> " + LogOutcome(action);
    if (action.UndoEntryId != 0)
        line += " undo #" + std::to_string(action.UndoEntryId);
    return line;
}

AssistantGate& AssistantGate::Get()
{
    static AssistantGate s_Gate;
    return s_Gate;
}

AssistantGate::AssistantGate()
    : AssistantGate([] { return std::chrono::steady_clock::now(); })
{
}

AssistantGate::AssistantGate(Clock now)
    : m_Now(std::move(now))
{
}

AssistantGate::~AssistantGate() = default;

AssistantGate::SessionId AssistantGate::Open(AssistantMode mode, std::filesystem::path projectRoot)
{
    const SessionId id = m_NextSession++;
    Session& session = m_Sessions[id];
    session.Id = id;
    session.Token = GUID::Generate().ToCompactString();
    session.Mode = mode;
    session.ProjectRoot = std::move(projectRoot);
    return id;
}

void AssistantGate::Close(SessionId session)
{
    m_Sessions.erase(session);
}

std::string AssistantGate::Token(SessionId session) const
{
    const auto it = m_Sessions.find(session);
    return it == m_Sessions.end() ? std::string() : it->second.Token;
}

void AssistantGate::SetMode(SessionId session, AssistantMode mode)
{
    if (const auto it = m_Sessions.find(session); it != m_Sessions.end())
        it->second.Mode = mode;
}

void AssistantGate::StartTurn(SessionId session, uint64_t turn)
{
    if (const auto it = m_Sessions.find(session); it != m_Sessions.end())
    {
        it->second.Turn = turn;
        it->second.AllowedForTurn.clear();
    }
}

void AssistantGate::EndTurn(SessionId session)
{
    StartTurn(session, 0);
}

void AssistantGate::Answer(SessionId session, uint64_t requestId, AssistantAnswer answer)
{
    const auto it = m_Sessions.find(session);
    if (it == m_Sessions.end() || answer == AssistantAnswer::None)
        return;
    Session& state = it->second;
    const AssistantAction* action = state.Ledger.Find(requestId);
    // A row left from an earlier turn is answered by nothing: its call ends as cancelled.
    if (!action || action->State != AssistantActionState::Waiting || action->WaitingFor != AssistantWaitReason::Answer ||
        action->Turn != state.Turn)
        return;
    const std::string tool = action->Tool;
    state.Ledger.Edit(requestId)->Answer = answer;
    if (answer == AssistantAnswer::AllowForTurn && !IsAllowedForTurn(state, tool))
        state.AllowedForTurn.push_back(tool);
}

const AssistantActionLedger* AssistantGate::Ledger(SessionId session) const
{
    const auto it = m_Sessions.find(session);
    return it == m_Sessions.end() ? nullptr : &it->second.Ledger;
}

void AssistantGate::RequestUndoTurn(SessionId session, uint64_t turn)
{
    m_UndoTurnRequest = UndoTurnRequest{session, turn};
}

void AssistantGate::Update(Editor::UndoRedoService* undo)
{
    if (undo && m_UndoTurnRequest)
    {
        ReadUndoHistory(undo);
        UndoRequestedTurn(*undo);
    }
    m_UndoTurnRequest.reset();
    ReadUndoHistory(undo);
}

void AssistantGate::ReadUndoHistory(const Editor::UndoRedoService* undo)
{
    AssistantUndoSnapshot history;
    if (undo)
    {
        const size_t undoCount = undo->GetUndoCount();
        const size_t redoCount = undo->GetRedoCount();
        // Every push, undo, redo and trim moves a count or the top of a stack.
        const bool same = undoCount == m_UndoHistory.Undo.size() && redoCount == m_UndoHistory.Redo.size() &&
                          (undoCount == 0 || m_UndoHistory.Undo.back() == undo->GetUndoEntryIdAt(undoCount - 1)) &&
                          (redoCount == 0 || m_UndoHistory.Redo.front() == undo->GetRedoEntryIdAt(0)) &&
                          (undoCount == 0 || m_UndoHistory.Undo.front() == undo->GetUndoEntryIdAt(0));
        if (same)
            return;
        history.Undo.reserve(undoCount);
        for (size_t index = 0; index < undoCount; ++index)
            history.Undo.push_back(undo->GetUndoEntryIdAt(index));
        for (size_t index = 0; index < redoCount; ++index)
            history.Redo.push_back(undo->GetRedoEntryIdAt(index));
        if (const char* top = undo->PeekUndoName())
            history.TopName = top;
    }
    if (history == m_UndoHistory)
        return;
    m_UndoHistory = std::move(history);
    ++m_UndoHistoryRevision;
}

void AssistantGate::UndoRequestedTurn(Editor::UndoRedoService& undo)
{
    const auto session = m_Sessions.find(m_UndoTurnRequest->Session);
    if (session == m_Sessions.end())
        return;
    const AssistantTurnUndo turn =
        AssistantTurnUndo::Describe(session->second.Ledger.Actions(), m_UndoTurnRequest->Turn, m_UndoHistory);
    // The rule is checked again here: an edit may have landed since the button was enabled.
    if (turn.Status != AssistantTurnUndo::State::Ready)
        return;
    for (size_t step = 0; step < turn.Steps.size(); ++step)
        undo.Undo();
}

void AssistantGate::Log(const Session& session, uint64_t requestId) const
{
    if (const AssistantAction* action = session.Ledger.Find(requestId))
        Logger::Log::Info("{}", AssistantActionLogLine(session.Id, *action));
}

DebugRequestVerdict AssistantGate::Bind(uint32_t clientId, const nlohmann::json* params)
{
    const std::string token = StringParam(params, "token");
    for (const auto& [id, session] : m_Sessions)
    {
        if (!token.empty() && session.Token == token)
        {
            m_Bindings[clientId] = id;
            return {};
        }
    }
    return Refuse("no open AI Assistant conversation has this token; the conversation was closed or the editor "
                  "restarted");
}

bool AssistantGate::IsAllowedForTurn(const Session& session, std::string_view tool)
{
    return std::find(session.AllowedForTurn.begin(), session.AllowedForTurn.end(), tool) !=
           session.AllowedForTurn.end();
}

AssistantGate::Session* AssistantGate::BoundSession(uint32_t clientId)
{
    const auto binding = m_Bindings.find(clientId);
    if (binding == m_Bindings.end())
        return nullptr;
    const auto session = m_Sessions.find(binding->second);
    return session == m_Sessions.end() ? nullptr : &session->second;
}

DebugRequestVerdict AssistantGate::Offer(Session& session, const DebugRequestGateContext& context)
{
    const Call call = CallOf(context);
    AssistantAction action;
    action.RequestId = context.RequestId;
    action.ClientId = context.ClientId;
    action.Turn = session.Turn;
    action.Tool = call.Name;
    action.Class = call.Tool ? call.Tool->Class : AssistantToolClass::Denied;
    action.Arguments =
        call.Arguments ? call.Arguments->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) : "{}";
    action.Subject = call.Arguments ? AssistantTools::Subject(call.Name, *call.Arguments) : std::string();
    action.Requested = std::chrono::system_clock::now();

    DebugRequestVerdict verdict = DecideInSession(session.Mode, session.Turn, session.ProjectRoot, context.Method, call);
    if (verdict.Decision == DebugRequestDecision::Park && IsAllowedForTurn(session, call.Name))
    {
        // It runs on the user's earlier Allow for this turn; its row says so.
        verdict = {};
        action.Answer = AssistantAnswer::AllowForTurn;
    }
    if (verdict.Decision == DebugRequestDecision::Refuse)
    {
        action.State = AssistantActionState::Refused;
        action.Message = verdict.Refusal;
        action.Answered = action.Requested;
        session.Ledger.Add(std::move(action));
        Log(session, context.RequestId);
        return verdict;
    }
    if (verdict.Decision == DebugRequestDecision::Park)
    {
        action.State = AssistantActionState::Waiting;
        action.WaitingFor = AssistantWaitReason::Answer;
        action.WaitingSince = m_Now();
        session.Ledger.Add(std::move(action));
        Log(session, context.RequestId);
        return verdict;
    }
    action.State = AssistantActionState::Done;
    session.Ledger.Add(action);
    return Admit(session, action, context);
}

DebugRequestVerdict AssistantGate::OfferAgain(Session& session, const AssistantAction& action,
                                              const DebugRequestGateContext& context)
{
    if (session.Turn == 0 || action.Turn != session.Turn)
        return Settle(session, action.RequestId, AssistantActionState::Cancelled, kSessionEnded);
    if (action.WaitingFor == AssistantWaitReason::Answer)
    {
        if (action.Answer == AssistantAnswer::Deny)
            return Settle(session, action.RequestId, AssistantActionState::Declined,
                          "The user declined: " + ActionText(action));
        if (action.Answer == AssistantAnswer::None && !IsAllowedForTurn(session, action.Tool))
        {
            if (m_Now() - action.WaitingSince >= kWaitLimit)
                return Settle(session, action.RequestId, AssistantActionState::Refused, kNoAnswer);
            return {DebugRequestDecision::Park, {}};
        }
    }
    return Admit(session, action, context);
}

DebugRequestVerdict AssistantGate::Admit(Session& session, const AssistantAction& action,
                                         const DebugRequestGateContext& context)
{
    const uint64_t requestId = action.RequestId;
    if (WaitsForUsersEdit(action.Class) && context.Undo && context.Undo->HasOpenInteractiveEdit())
    {
        if (action.WaitingFor == AssistantWaitReason::Edit)
        {
            if (m_Now() - action.WaitingSince >= kWaitLimit)
                return Settle(session, requestId, AssistantActionState::Refused, kEditNotFinished);
            return {DebugRequestDecision::Park, {}};
        }
        // One budget per call: an allowed call that now waits for the drag keeps the time
        // it already waited for the answer, so it is never refused after the MCP server
        // gave up on it and then run.
        const bool firstWait = action.WaitingFor == AssistantWaitReason::None;
        AssistantAction* waiting = session.Ledger.Edit(requestId);
        waiting->State = AssistantActionState::Waiting;
        waiting->WaitingFor = AssistantWaitReason::Edit;
        if (firstWait)
            waiting->WaitingSince = m_Now();
        return {DebugRequestDecision::Park, {}};
    }
    if (action.State != AssistantActionState::Done || action.WaitingFor != AssistantWaitReason::None)
    {
        AssistantAction* admitted = session.Ledger.Edit(requestId);
        admitted->State = AssistantActionState::Done;
        admitted->WaitingFor = AssistantWaitReason::None;
    }
    if (action.Class == AssistantToolClass::UndoableEdit && context.Undo)
    {
        const Call call = CallOf(context);
        if (call.Tool)
            m_OpenStep = std::make_unique<UndoStep>(*context.Undo, requestId,
                                                    AssistantUndoStepName(*call.Tool, call.Arguments));
    }
    return {};
}

DebugRequestVerdict AssistantGate::Settle(Session& session, uint64_t requestId, AssistantActionState state,
                                          std::string message)
{
    if (AssistantAction* action = session.Ledger.Edit(requestId))
    {
        action->State = state;
        action->WaitingFor = AssistantWaitReason::None;
        action->Message = message;
        action->Answered = std::chrono::system_clock::now();
        Log(session, requestId);
    }
    return Refuse(std::move(message));
}

DebugRequestVerdict AssistantGate::Before(const DebugRequestGateContext& context)
{
    if (context.Method == kBindMethod)
        return Bind(context.ClientId, context.Params);
    if (!m_Bindings.contains(context.ClientId))
    {
        if (context.Method == kAuthorizeMethod)
            return Refuse("this connection is not bound to an AI Assistant conversation");
        return {};
    }
    Session* session = BoundSession(context.ClientId);
    if (!session)
        return Refuse("The AI Assistant conversation this session served was closed: start a new turn from the "
                      "AI Assistant panel.");
    // A parked request is offered again under its own id.
    if (const AssistantAction* known = session->Ledger.Find(context.RequestId))
        return OfferAgain(*session, *known, context);
    return Offer(*session, context);
}

void AssistantGate::Handled(const DebugRequestGateContext& context)
{
    if (!m_OpenStep || m_OpenStep->RequestId() != context.RequestId)
        return;
    const std::unique_ptr<UndoStep> step = std::move(m_OpenStep);
    const uint64_t entry = step->Close();
    Session* session = BoundSession(context.ClientId);
    if (entry == 0 || !session)
        return;
    if (AssistantAction* action = session->Ledger.Edit(context.RequestId))
    {
        action->UndoEntryId = entry;
        action->UndoName = step->Name();
    }
}

void AssistantGate::After(const DebugRequestGateContext& context, const nlohmann::json* response)
{
    Session* session = BoundSession(context.ClientId);
    if (!session || !session->Ledger.Find(context.RequestId))
        return;
    AssistantAction* action = session->Ledger.Edit(context.RequestId);
    action->Answered = std::chrono::system_clock::now();
    const bool ran = action->State == AssistantActionState::Done;
    if (response)
    {
        action->Result = CutUtf8(response->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                                 AssistantActionLedger::kMaxResultBytes);
        if (ran && !response->value("ok", true))
        {
            action->State = AssistantActionState::Failed;
            action->Message = response->value("error", std::string());
        }
    }
    // A refusal was logged when the gate decided it; a call that ran is logged with its outcome.
    if (ran)
        Log(*session, context.RequestId);
}

void AssistantGate::ClientClosed(uint32_t clientId)
{
    if (Session* session = BoundSession(clientId))
    {
        // Its parked requests are dropped unanswered with the connection.
        for (const AssistantAction& action : session->Ledger.Actions())
            if (action.ClientId == clientId && action.State == AssistantActionState::Waiting)
                (void)Settle(*session, action.RequestId, AssistantActionState::Cancelled, kSessionEnded);
    }
    m_Bindings.erase(clientId);
}

Editor::DebugRequestGate AssistantGate::AsDebugRequestGate()
{
    Editor::DebugRequestGate gate;
    gate.GateId = "aiAssistant";
    gate.Before = [this](const DebugRequestGateContext& context) { return Before(context); };
    gate.Handled = [this](const DebugRequestGateContext& context) { Handled(context); };
    gate.After = [this](const DebugRequestGateContext& context, const nlohmann::json* response) {
        After(context, response);
    };
    gate.ClaimedMethods = {std::string(kBindMethod), std::string(kAuthorizeMethod)};
    gate.ClientClosed = [this](uint32_t clientId) { ClientClosed(clientId); };
    gate.Update = [this](Editor::UndoRedoService* undo) { Update(undo); };
    return gate;
}
} // namespace GameEngine
