#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Input/GamepadCodes.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <unordered_map>

namespace GameEngine {
namespace Input {

namespace {

// GLFW key codes range 0..348 (GLFW_KEY_LAST). Mouse buttons 0..7.
constexpr int kMaxKeyCode = 349;
constexpr int kMaxMouseButton = 8;

// A poll claims the code it reads for this many frames past the frame it was
// made in. Wider than a single frame so a caller that polls at less than frame
// rate, or misses a tick, keeps its claim; every poll refreshes the stamp.
constexpr std::uint64_t kPollClaimWindowFrames = 2;

struct KeyState {
    bool down         = false;
    bool pressedThis  = false;
    bool releasedThis = false;
    // Modifier state at the moment this key was pressed.
    // This avoids "last event wins" issues when multiple input events happen in the same frame
    // (e.g. Shift+F press, then Shift release before Update()).
    int modsAtPress   = 0;
};

struct ContextData {
    bool enabled = true;
    // Registered actions for this context.
    std::unordered_map<ActionId, ActionDesc> actions;
    // Current state per action.
    std::unordered_map<ActionId, ActionState> states;
};

struct ListenerRecord {
    uint32_t       handleId;
    ActionId       actionId;
    ActionCallback callback;
};

inline bool IsValidKeyCode(int key) { return key >= 0 && key < kMaxKeyCode; }
inline bool IsValidMouseButton(int btn) { return btn >= 0 && btn < kMaxMouseButton; }

inline bool IsShiftKey(KeyCode key)
{
    return key == kKeyCode_LeftShift || key == kKeyCode_RightShift;
}

inline bool IsControlKey(KeyCode key)
{
    return key == kKeyCode_LeftControl || key == kKeyCode_RightControl;
}

inline bool IsAltKey(KeyCode key)
{
    return key == kKeyCode_LeftAlt || key == kKeyCode_RightAlt;
}

inline bool IsSuperKey(KeyCode key)
{
    return key == kKeyCode_LeftSuper || key == kKeyCode_RightSuper;
}

// A binding applies only when every required modifier is held and no forbidden
// one is. Shared by action evaluation and by claim matching so a sink never
// claims a combination its action would then ignore.
inline bool MatchesMods(int mods, const ActionBinding& b)
{
    if ((mods & b.requiredMods) != b.requiredMods)
        return false;
    if ((mods & b.forbiddenMods) != 0)
        return false;
    return true;
}

} // namespace

class InputSystem::Impl {
public:
    // Flat arrays for key state -- cache-friendly, zero-allocation.
    KeyState m_Keys[kMaxKeyCode] = {};

    // Frame snapshot: survives from Update() until the next Update() so
    // WasKeyPressed/WasKeyReleased can read last frame's transitions.
    bool m_FramePressed[kMaxKeyCode] = {};
    bool m_FrameReleased[kMaxKeyCode] = {};

    // Keys whose transient flags (pressedThis/releasedThis) need clearing.
    // Typically 0-5 entries per frame. Duplicates are harmless.
    std::vector<int> m_DirtyKeys;

    // Keys pressed this frame -- subset of dirty keys, used for mod latching.
    std::vector<int> m_PressedThisFrame;

    // Mouse state -- flat array for buttons, vectors for position/deltas.
    KeyState m_MouseButtons[kMaxMouseButton] = {};
    // Where each button was last pressed. Outlives the hold and the release so a
    // gesture can be measured from its origin however late the reader polls.
    std::optional<Mathematics::Vector2> m_MousePressPositions[kMaxMouseButton];
    // Frame snapshot, same contract as m_FramePressed/m_FrameReleased: survives
    // from Update() until the next Update() so WasMouseButtonPressed/Released
    // see a press+release pair that completed inside a single frame — the pair
    // a live IsMouseButtonDown poll can never observe.
    bool m_FrameMousePressed[kMaxMouseButton] = {};
    bool m_FrameMouseReleased[kMaxMouseButton] = {};
    std::vector<int> m_DirtyMouseButtons;
    Mathematics::Vector2 m_MousePosition;
    bool m_PointerInWindow = false;
    bool m_WindowFocused = false;
    // Accumulates only between two positions inside the window: a pointer
    // arriving from outside has not travelled from anywhere this sink saw.
    Mathematics::Vector2 m_MouseDelta;
    Mathematics::Vector2 m_Scroll;

    struct GamepadState
    {
        bool connected = false;
        float axes[kGamepadAxisCount] = {};
        bool buttons[kGamepadButtonCount] = {};
    };
    GamepadState m_Gamepads[kMaxGamepads] = {};

    int m_CurrentMods = 0;

    // Contexts and stack (higher index = higher priority).
    std::unordered_map<ContextId, ContextData> m_Contexts;
    std::vector<ContextId>                     m_ContextStack;

    std::unordered_map<ActionId, std::vector<ListenerRecord>> m_Listeners; // by action id
    uint32_t                                                  m_NextListenerId = 1;

    SinkRole m_Role = SinkRole::Application;
    bool m_ClaimsSuspended = false;

    // Frame in which each code was last polled; 0 = never. Update() advances
    // m_Frame, so a claim ages out on its own once nothing reads the code.
    // m_Frame starts at 1 so a poll made before the first Update is still a
    // stamp rather than "never".
    std::uint64_t m_Frame = 1;
    std::uint64_t m_KeyPollClaim[kMaxKeyCode] = {};
    std::uint64_t m_MousePollClaim[kMaxMouseButton] = {};
    std::uint64_t m_GamepadPollClaim[kMaxGamepads][kGamepadButtonCount] = {};

    void UpdateActionsFromKeys();
    int  DeriveShortcutModsFromKeys() const;

    // Stamped by the polling queries and read by the claim checks, all on the
    // thread that owns the frame — the same thread the platform delivers events
    // on, which is where game systems and editor panels poll from.
    void NoteKeyPoll(int key) { m_KeyPollClaim[key] = m_Frame; }
    void NoteMousePoll(int button) { m_MousePollClaim[button] = m_Frame; }
    void NoteGamepadPoll(int gamepadIndex, int button) { m_GamepadPollClaim[gamepadIndex][button] = m_Frame; }

    bool HasPollClaim(std::uint64_t stamp) const
    {
        return stamp != 0 && (m_Frame - stamp) <= kPollClaimWindowFrames;
    }

    // Does this sink claim the event — i.e. is it something the game reads?
    // Bindings answer for action-mapped code, poll stamps for poll-style code.
    bool ClaimsKey(int key, int mods) const;
    bool ClaimsMouseButton(int button) const;
    bool ClaimsScroll(Mathematics::Vector2 delta) const;
    bool ClaimsGamepadButton(int gamepadIndex, int button) const;

    // Does this sink take the event? An Application sink ends its chain and takes
    // everything; a Gameplay sink takes what it claims, and nothing at all while
    // suspended. A key or button it already holds stays its own, so a hold cannot
    // be split from the repeats and release that end it.
    bool TakesKeyPress(int key, int mods, bool alreadyHeld) const
    {
        if (m_Role == SinkRole::Application)
            return true;
        if (m_ClaimsSuspended)
            return false;
        return alreadyHeld || ClaimsKey(key, ClaimMods(mods));
    }

    bool TakesMousePress(int button, bool alreadyHeld) const
    {
        if (m_Role == SinkRole::Application)
            return true;
        if (m_ClaimsSuspended)
            return false;
        return alreadyHeld || ClaimsMouseButton(button);
    }

    bool TakesScroll(Mathematics::Vector2 delta) const
    {
        if (m_Role == SinkRole::Application)
            return true;
        if (m_ClaimsSuspended)
            return false;
        return ClaimsScroll(delta);
    }

    // A gamepad button has no repeats and no modifiers, so there is nothing to
    // carry over from a press this sink already took.
    bool TakesGamepadPress(int gamepadIndex, int button) const
    {
        if (m_Role == SinkRole::Application)
            return true;
        if (m_ClaimsSuspended)
            return false;
        return ClaimsGamepadButton(gamepadIndex, button);
    }

    // Modifier state a claim is judged against: the modifier keys this sink
    // holds, widened by the mask the platform reported with the event. The mask
    // is what covers a modifier pressed before this sink started receiving keys.
    int ClaimMods(int eventMods) const { return DeriveShortcutModsFromKeys() | (eventMods & kModShortcutMask); }

    // Take an event's modifier mask as ambient state. Modifiers are not claimed
    // like other input: a sink that declines a bare Ctrl — which a gameplay sink
    // does whenever nothing is bound to Ctrl alone — never records it as held, so
    // its own key state can never report the chord its Ctrl+Click binding names.
    // The platform mask is the authority for the modifiers actually down, and it
    // arrives on every pointer event, so adopting it is what makes a chorded
    // pointer binding both claimable and matchable.
    void AdoptEventMods(int eventMods)
    {
        // Pointer masks carry shortcut bits only (Window::GetModifierKeyMask
        // synthesizes Shift/Ctrl/Alt/Super; GLFW's lock-key mods are never
        // enabled), so the lock bits stay as the last key event reconciled them.
        m_CurrentMods = ClaimMods(eventMods) | (m_CurrentMods & (kModCapsLock | kModNumLock));
    }
};

// --- InputSystem public API ---

InputSystem::InputSystem(SinkRole role)
    : m_Impl(std::make_unique<Impl>())
{
    m_Impl->m_Role = role;
}

InputSystem::~InputSystem() = default;

void InputSystem::Update(float /*deltaTime*/) {
    // Frame boundary for poll claims: polls made from here until the next Update
    // belong to this frame, and the events that arrive before that next Update
    // see them as the current frame's claims.
    ++m_Impl->m_Frame;

    // Recompute action states from current key state.
    m_Impl->UpdateActionsFromKeys();

    // Snapshot transitions into frame arrays, then clear transient flags.
    // Only touch keys that were actually dirtied this frame.
    std::memset(m_Impl->m_FramePressed, 0, sizeof(m_Impl->m_FramePressed));
    std::memset(m_Impl->m_FrameReleased, 0, sizeof(m_Impl->m_FrameReleased));
    for (int key : m_Impl->m_DirtyKeys) {
        KeyState& ks = m_Impl->m_Keys[key];
        // OR: a key dirtied multiple times (press+release same frame) must
        // preserve both transitions even though the second iteration sees
        // cleared flags from the first.
        m_Impl->m_FramePressed[key]  |= ks.pressedThis;
        m_Impl->m_FrameReleased[key] |= ks.releasedThis;
        ks.pressedThis  = false;
        ks.releasedThis = false;
    }
    m_Impl->m_DirtyKeys.clear();
    m_Impl->m_PressedThisFrame.clear();

    // Snapshot mouse button transitions, then clear transient flags (same
    // OR-over-dirty-list pattern as keys: press+release in one frame must
    // preserve both transitions).
    std::memset(m_Impl->m_FrameMousePressed, 0, sizeof(m_Impl->m_FrameMousePressed));
    std::memset(m_Impl->m_FrameMouseReleased, 0, sizeof(m_Impl->m_FrameMouseReleased));
    for (int btn : m_Impl->m_DirtyMouseButtons) {
        KeyState& bs = m_Impl->m_MouseButtons[btn];
        m_Impl->m_FrameMousePressed[btn]  |= bs.pressedThis;
        m_Impl->m_FrameMouseReleased[btn] |= bs.releasedThis;
        bs.pressedThis  = false;
        bs.releasedThis = false;
    }
    m_Impl->m_DirtyMouseButtons.clear();

    // Clear per-frame mouse deltas and scroll accumulators after actions have been computed.
    m_Impl->m_MouseDelta = {};
    m_Impl->m_Scroll     = {};

    // Dispatch action events for actions that changed this frame.
    for (auto &listenerPair : m_Impl->m_Listeners) {
        const ActionId actionId = listenerPair.first;
        ActionState    st       = GetActionState(actionId);
        if (!st.justPressed && !st.justReleased)
            continue;

        ActionEvent evt{actionId, st, false};
        for (ListenerRecord &rec : listenerPair.second) {
            rec.callback(evt);
            if (evt.consumed)
                break;
        }
    }
}

void InputSystem::ResetState()
{
    for (int i = 0; i < kMaxKeyCode; ++i)
    {
        KeyState& ks = m_Impl->m_Keys[i];
        if (ks.down || ks.pressedThis || ks.releasedThis)
        {
            if (ks.down)
                ks.releasedThis = true;
            ks.down = false;
            ks.pressedThis = false;
            m_Impl->m_DirtyKeys.push_back(i);
        }
    }
    for (int i = 0; i < kMaxMouseButton; ++i)
    {
        // The gesture that recorded the origin is over; a reader that polls
        // after the reset must see "no press" rather than measure a drag from
        // where the pointer was before focus was lost.
        m_Impl->m_MousePressPositions[i].reset();
        KeyState& bs = m_Impl->m_MouseButtons[i];
        if (bs.down || bs.pressedThis || bs.releasedThis)
        {
            if (bs.down)
                bs.releasedThis = true;
            bs.down = false;
            bs.pressedThis = false;
            m_Impl->m_DirtyMouseButtons.push_back(i);
        }
    }
    m_Impl->m_MouseDelta = {};
    m_Impl->m_Scroll     = {};
    m_Impl->m_CurrentMods = 0;

    // A pad carries no per-frame edge snapshot to unwind, so forgetting every
    // slot outright is the whole release: its buttons read up, the slot reads
    // empty, and the next poll re-delivers connection and axes. A hold that
    // outlives the reset is adopted as a baseline rather than pressed again.
    for (auto& pad : m_Impl->m_Gamepads)
        pad = {};
}

void InputSystem::ClearRegistrations()
{
    m_Impl->m_Contexts.clear();
    m_Impl->m_ContextStack.clear();
    m_Impl->m_Listeners.clear();
    m_Impl->m_NextListenerId = 1;

    // Poll claims belong to the session that made them: the code that was
    // reading these keys is gone with the registrations.
    std::memset(m_Impl->m_KeyPollClaim, 0, sizeof(m_Impl->m_KeyPollClaim));
    std::memset(m_Impl->m_MousePollClaim, 0, sizeof(m_Impl->m_MousePollClaim));
    std::memset(m_Impl->m_GamepadPollClaim, 0, sizeof(m_Impl->m_GamepadPollClaim));
}

void InputSystem::SetClaimsSuspended(bool suspended)
{
    m_Impl->m_ClaimsSuspended = suspended;
}

// The polling queries record the claim they imply. They are const because
// reading state is what they do; the claim is bookkeeping about the reader, and
// m_Impl is a unique_ptr, which does not propagate constness to what it owns.
bool InputSystem::IsKeyDown(KeyCode key) const {
    if (!IsValidKeyCode(key))
        return false;
    m_Impl->NoteKeyPoll(key);
    return m_Impl->m_Keys[key].down;
}

bool InputSystem::WasKeyPressed(KeyCode key) const {
    if (!IsValidKeyCode(key))
        return false;
    m_Impl->NoteKeyPoll(key);
    return m_Impl->m_FramePressed[key];
}

bool InputSystem::WasKeyReleased(KeyCode key) const {
    if (!IsValidKeyCode(key))
        return false;
    m_Impl->NoteKeyPoll(key);
    return m_Impl->m_FrameReleased[key];
}

Mathematics::Vector2 InputSystem::GetMousePosition() const {
    return m_Impl->m_MousePosition;
}

bool InputSystem::IsPointerInWindow() const {
    return m_Impl->m_PointerInWindow;
}

bool InputSystem::IsWindowFocused() const {
    return m_Impl->m_WindowFocused;
}

std::optional<Mathematics::Vector2> InputSystem::GetMouseButtonPressPosition(MouseCode button) const {
    if (!IsValidMouseButton(button))
        return std::nullopt;
    m_Impl->NoteMousePoll(button);
    return m_Impl->m_MousePressPositions[button];
}

bool InputSystem::IsMouseButtonDown(MouseCode button) const {
    if (!IsValidMouseButton(button))
        return false;
    m_Impl->NoteMousePoll(button);
    return m_Impl->m_MouseButtons[button].down;
}

bool InputSystem::WasMouseButtonPressed(MouseCode button) const {
    if (!IsValidMouseButton(button))
        return false;
    m_Impl->NoteMousePoll(button);
    return m_Impl->m_FrameMousePressed[button];
}

bool InputSystem::WasMouseButtonReleased(MouseCode button) const {
    if (!IsValidMouseButton(button))
        return false;
    m_Impl->NoteMousePoll(button);
    return m_Impl->m_FrameMouseReleased[button];
}

void InputSystem::OnGamepadState(int gamepadIndex, const float* axes, int axisCount, bool connected)
{
    if (gamepadIndex < 0 || gamepadIndex >= kMaxGamepads)
        return;

    auto& pad = m_Impl->m_Gamepads[gamepadIndex];
    pad.connected = connected && axes != nullptr;

    const int axisCopy = pad.connected ? std::min(axisCount, kGamepadAxisCount) : 0;
    for (int i = 0; i < axisCopy; ++i)
        pad.axes[i] = axes[i];
    for (int i = axisCopy; i < kGamepadAxisCount; ++i)
        pad.axes[i] = 0.0f;

    // Buttons are not cleared here: a pad that leaves releases the buttons it
    // held first, so each sink clears exactly the buttons it was holding and one
    // that declined a press has nothing to forget.
}

bool InputSystem::OnGamepadButton(int gamepadIndex, int button, bool down)
{
    if (gamepadIndex < 0 || gamepadIndex >= kMaxGamepads)
        return false;
    if (button < 0 || button >= kGamepadButtonCount)
        return false;

    auto& pad = m_Impl->m_Gamepads[gamepadIndex];
    if (!down)
    {
        // Releases reach every stage and clear only state this sink holds, so a
        // release for a press it declined is a no-op rather than a phantom edge.
        pad.buttons[button] = false;
        return false;
    }

    if (!m_Impl->TakesGamepadPress(gamepadIndex, button))
        return false;

    pad.buttons[button] = true;
    return true;
}

bool InputSystem::IsGamepadConnected(int gamepadIndex) const
{
    if (gamepadIndex < 0 || gamepadIndex >= kMaxGamepads)
        return false;
    return m_Impl->m_Gamepads[gamepadIndex].connected;
}

float InputSystem::GetGamepadAxis(int gamepadIndex, GamepadAxis axis) const
{
    if (gamepadIndex < 0 || gamepadIndex >= kMaxGamepads)
        return 0.0f;
    const auto& pad = m_Impl->m_Gamepads[gamepadIndex];
    if (!pad.connected)
        return 0.0f;
    const int idx = static_cast<int>(axis);
    if (idx < 0 || idx >= kGamepadAxisCount)
        return 0.0f;
    return pad.axes[idx];
}

bool InputSystem::IsGamepadButtonDown(int gamepadIndex, GamepadButton button) const
{
    if (gamepadIndex < 0 || gamepadIndex >= kMaxGamepads)
        return false;
    const int idx = static_cast<int>(button);
    if (idx < 0 || idx >= kGamepadButtonCount)
        return false;
    // The claim is stamped before the connection is judged: a game that reads a
    // button while nothing is plugged in is still declaring interest, so the pad
    // it plugs in next is claimed from its first press.
    m_Impl->NoteGamepadPoll(gamepadIndex, idx);
    const auto& pad = m_Impl->m_Gamepads[gamepadIndex];
    if (!pad.connected)
        return false;
    return pad.buttons[idx];
}

void InputSystem::RegisterAction(const ContextId &context, const ActionDesc &desc) {
    ContextData &ctx = m_Impl->m_Contexts[context];
    ctx.actions[desc.id] = desc;
    ctx.states[desc.id]  = ActionState{};
}

bool InputSystem::MatchesKeyBinding(const ContextId &context, const ActionId &action, KeyCode key, int mods) const {
    const auto ctxIt = m_Impl->m_Contexts.find(context);
    if (ctxIt == m_Impl->m_Contexts.end() || !ctxIt->second.enabled)
        return false;
    const auto actionIt = ctxIt->second.actions.find(action);
    if (actionIt == ctxIt->second.actions.end())
        return false;
    for (const auto &binding : actionIt->second.bindings) {
        if (binding.device == DeviceType::Keyboard && binding.code == key && MatchesMods(mods, binding))
            return true;
    }
    return false;
}

bool InputSystem::IsHostReservedKey(KeyCode key, int mods) const {
    for (const auto &[contextId, ctx] : m_Impl->m_Contexts) {
        if (!ctx.enabled)
            continue;
        for (const auto &[actionId, action] : ctx.actions) {
            if (!action.hostReserved)
                continue;
            for (const auto &binding : action.bindings) {
                if (binding.device == DeviceType::Keyboard && binding.code == key && MatchesMods(mods, binding))
                    return true;
            }
        }
    }
    return false;
}

void InputSystem::RemoveAction(const ContextId &context, const ActionId &id) {
    auto ctxIt = m_Impl->m_Contexts.find(context);
    if (ctxIt == m_Impl->m_Contexts.end())
        return;
    ctxIt->second.actions.erase(id);
    ctxIt->second.states.erase(id);
}

void InputSystem::BindKey(const ContextId &context, const ActionId &id, const ActionBinding &binding) {
    ContextData &ctx = m_Impl->m_Contexts[context];
    auto actIt       = ctx.actions.find(id);
    if (actIt == ctx.actions.end()) {
        ActionDesc desc{};
        desc.id = id;
        desc.bindings.push_back(binding);
        ctx.actions[id] = desc;
        ctx.states[id]  = ActionState{};
    } else {
        actIt->second.bindings.push_back(binding);
    }
}

void InputSystem::ClearBindings(const ContextId &context, const ActionId &id) {
    auto ctxIt = m_Impl->m_Contexts.find(context);
    if (ctxIt == m_Impl->m_Contexts.end())
        return;
    auto actIt = ctxIt->second.actions.find(id);
    if (actIt == ctxIt->second.actions.end())
        return;
    actIt->second.bindings.clear();
}

void InputSystem::PushContext(const ContextId &context) {
    // Avoid duplicates; treat this as raising priority if already present.
    auto it = std::find(m_Impl->m_ContextStack.begin(), m_Impl->m_ContextStack.end(), context);
    if (it != m_Impl->m_ContextStack.end()) {
        m_Impl->m_ContextStack.erase(it);
    }
    m_Impl->m_ContextStack.push_back(context);
}

void InputSystem::PopContext(const ContextId &context) {
    auto it = std::find(m_Impl->m_ContextStack.begin(), m_Impl->m_ContextStack.end(), context);
    if (it != m_Impl->m_ContextStack.end()) {
        m_Impl->m_ContextStack.erase(it);
    }
}

void InputSystem::SetContextEnabled(const ContextId &context, bool enabled) {
    ContextData &ctx = m_Impl->m_Contexts[context];
    ctx.enabled      = enabled;
}

ActionState InputSystem::GetActionState(const ActionId &id) const {
    // Walk contexts from highest to lowest priority.
    for (auto it = m_Impl->m_ContextStack.rbegin(); it != m_Impl->m_ContextStack.rend(); ++it) {
        auto ctxIt = m_Impl->m_Contexts.find(*it);
        if (ctxIt == m_Impl->m_Contexts.end() || !ctxIt->second.enabled)
            continue;
        auto stIt = ctxIt->second.states.find(id);
        if (stIt != ctxIt->second.states.end())
            return stIt->second;
    }

    // Nothing on the stack owns this action, so answer from the enabled contexts
    // instead of reporting a dead action. UpdateActionsFromKeys computes state for
    // every enabled context and a claim is judged the same way, so without this an
    // action can be claimed — swallowing its key — while reading zeros forever.
    // That is what a binding registered through the scripting ABI looks like: the
    // managed surface authors actions and bindings but never pushes a context.
    // The stack still decides priority; contexts that were never pushed carry no
    // relative order among themselves.
    for (const auto &ctxPair : m_Impl->m_Contexts) {
        const ContextData &ctx = ctxPair.second;
        if (!ctx.enabled)
            continue;
        auto stIt = ctx.states.find(id);
        if (stIt != ctx.states.end())
            return stIt->second;
    }
    return ActionState{};
}

bool InputSystem::IsActionActive(const ActionId &id) const {
    ActionState st = GetActionState(id);
    return st.pressed;
}

bool InputSystem::WasActionTriggered(const ActionId &id) const {
    ActionState st = GetActionState(id);
    return st.justPressed;
}

InputSystem::ListenerHandle InputSystem::AddActionListener(const ActionId &id, ActionCallback cb) {
    ListenerHandle handle{m_Impl->m_NextListenerId++};
    m_Impl->m_Listeners[id].push_back(ListenerRecord{handle.id, id, std::move(cb)});
    return handle;
}

void InputSystem::RemoveActionListener(ListenerHandle handle) {
    for (auto &pair : m_Impl->m_Listeners) {
        auto &vec = pair.second;
        auto it = std::find_if(vec.begin(), vec.end(),
            [handle](const ListenerRecord &r) { return r.handleId == handle.id; });
        if (it != vec.end()) {
            vec.erase(it);
            return;
        }
    }
}

bool InputSystem::SerializeBindingsToJson(std::string &/*outJson*/) const {
    // Not implemented yet.
    return false;
}

bool InputSystem::DeserializeBindingsFromJson(const std::string &/*json*/) {
    // Not implemented yet.
    return false;
}

bool InputSystem::OnKey(int key, int action, int mods) {
    if (!IsValidKeyCode(key))
        return false;

    KeyState& st = m_Impl->m_Keys[key];
    const bool isPressEvent = (action != kKeyActionRelease);

    // Releases are never consumed: whoever saw the press must see the release or
    // the key stays down forever, and a release for a key this sink does not hold
    // is a no-op below.
    const bool consumed = isPressEvent && m_Impl->TakesKeyPress(key, mods, st.down);
    if (isPressEvent && !consumed)
        return false;

    const bool isNewPress = isPressEvent && !st.down;

    if (isPressEvent)
    {
        if (isNewPress)
        {
            st.down = true;
            st.pressedThis = true;
            m_Impl->m_PressedThisFrame.push_back(key);
        }
    }
    else
    {
        if (st.down)
        {
            st.down = false;
            st.releasedThis = true;
        }
    }
    m_Impl->m_DirtyKeys.push_back(key);

    // Reconcile modifier state:
    // - Prefer actual modifier key state when we have it.
    // - Fall back to the platform-provided mods mask.
    // - IMPORTANT: for the modifier key being processed, force the bit to match
    //   the derived post-event key state. This avoids "release event still has Shift bit"
    //   issues on some platforms.
    const int derivedShortcut = m_Impl->DeriveShortcutModsFromKeys();
    const int eventShortcut = (mods & kModShortcutMask);
    int shortcutMods = (derivedShortcut | eventShortcut);
    if (IsShiftKey(key))
        shortcutMods = (shortcutMods & ~kModShift) | (derivedShortcut & kModShift);
    if (IsControlKey(key))
        shortcutMods = (shortcutMods & ~kModControl) | (derivedShortcut & kModControl);
    if (IsAltKey(key))
        shortcutMods = (shortcutMods & ~kModAlt) | (derivedShortcut & kModAlt);
    if (IsSuperKey(key))
        shortcutMods = (shortcutMods & ~kModSuper) | (derivedShortcut & kModSuper);

    const int lockMods = (mods & (kModCapsLock | kModNumLock));
    m_Impl->m_CurrentMods = shortcutMods | lockMods;

    if (isNewPress)
    {
        st.modsAtPress = m_Impl->m_CurrentMods;
    }

    // Latch any observed modifier bits into keys pressed this frame.
    // Only iterates the small list of keys pressed this frame (typically 1-3).
    for (int k : m_Impl->m_PressedThisFrame)
    {
        m_Impl->m_Keys[k].modsAtPress |= m_Impl->m_CurrentMods;
    }

    return consumed;
}

void InputSystem::OnChar(unsigned int /*codepoint*/) {
    // Text input will likely be routed directly to UI/text widgets; InputSystem can
    // optionally expose this later.
}

bool InputSystem::OnMouseButton(int button, bool down, int mods) {
    if (!IsValidMouseButton(button))
        return false;

    m_Impl->AdoptEventMods(mods);

    KeyState &st = m_Impl->m_MouseButtons[button];

    // Same contract as OnKey: releases always land, a press is taken or declined.
    const bool consumed = down && m_Impl->TakesMousePress(button, st.down);
    if (down && !consumed)
        return false;

    if (down) {
        if (!st.down) {
            st.down        = true;
            st.pressedThis = true;
            // The chord this press belongs to: its edges are judged by these
            // modifiers even if one of them leaves before the frame is evaluated.
            st.modsAtPress = m_Impl->m_CurrentMods;
            // A press while the pointer is outside the window has no origin a
            // gesture could be measured from.
            if (m_Impl->m_PointerInWindow)
                m_Impl->m_MousePressPositions[button] = m_Impl->m_MousePosition;
            else
                m_Impl->m_MousePressPositions[button].reset();
        }
    } else {
        if (st.down) {
            st.down         = false;
            st.releasedThis = true;
        }
    }
    m_Impl->m_DirtyMouseButtons.push_back(button);
    return consumed;
}

void InputSystem::OnMouseMove(Mathematics::Vector2 position) {
    if (m_Impl->m_PointerInWindow)
        m_Impl->m_MouseDelta = m_Impl->m_MouseDelta + (position - m_Impl->m_MousePosition);
    m_Impl->m_MousePosition = position;
    m_Impl->m_PointerInWindow = true;
}

void InputSystem::OnMouseLeave() {
    m_Impl->m_PointerInWindow = false;
}

void InputSystem::OnWindowFocus(bool focused) {
    // The window system synthesizes releases on focus loss only for the keys it
    // saw pressed on this window; held state adopted from an event's mods mask
    // has no release at all, so the sink lets go of everything itself — on
    // every loss report, whether or not it saw the gain.
    if (!focused)
        ResetState();
    m_Impl->m_WindowFocused = focused;
}

bool InputSystem::OnMouseScroll(Mathematics::Vector2 delta, int mods) {
    m_Impl->AdoptEventMods(mods);

    // A wheel has no press/release pair, so there is no hold to preserve and no
    // polling query that reads scroll: an axis binding of an enabled context is
    // the only thing that can claim one for a Gameplay sink.
    if (!m_Impl->TakesScroll(delta))
        return false;

    m_Impl->m_Scroll = m_Impl->m_Scroll + delta;
    return true;
}

// --- Impl helpers ---

int InputSystem::Impl::DeriveShortcutModsFromKeys() const
{
    int mods = 0;
    if (m_Keys[kKeyCode_LeftShift].down || m_Keys[kKeyCode_RightShift].down)
        mods |= kModShift;
    if (m_Keys[kKeyCode_LeftControl].down || m_Keys[kKeyCode_RightControl].down)
        mods |= kModControl;
    if (m_Keys[kKeyCode_LeftAlt].down || m_Keys[kKeyCode_RightAlt].down)
        mods |= kModAlt;
    if (m_Keys[kKeyCode_LeftSuper].down || m_Keys[kKeyCode_RightSuper].down)
        mods |= kModSuper;
    return mods;
}

bool InputSystem::Impl::ClaimsKey(int key, int mods) const
{
    if (HasPollClaim(m_KeyPollClaim[key]))
        return true;

    // Enabled contexts, not the priority stack: these are exactly the contexts
    // whose action states UpdateActionsFromKeys computes, so a claimed key is a
    // key that can still move an action.
    for (const auto& ctxPair : m_Contexts)
    {
        if (!ctxPair.second.enabled)
            continue;
        for (const auto& actPair : ctxPair.second.actions)
        {
            for (const ActionBinding& b : actPair.second.bindings)
            {
                if (b.device != DeviceType::Keyboard || b.code != key)
                    continue;
                if (MatchesMods(mods, b))
                    return true;
            }
        }
    }
    return false;
}

bool InputSystem::Impl::ClaimsMouseButton(int button) const
{
    if (HasPollClaim(m_MousePollClaim[button]))
        return true;

    for (const auto& ctxPair : m_Contexts)
    {
        if (!ctxPair.second.enabled)
            continue;
        for (const auto& actPair : ctxPair.second.actions)
        {
            // Axis bindings carry synthetic negative codes, never a button.
            if (actPair.second.isAxis)
                continue;
            for (const ActionBinding& b : actPair.second.bindings)
            {
                if (b.device != DeviceType::Mouse || b.code != button)
                    continue;
                if (MatchesMods(m_CurrentMods, b))
                    return true;
            }
        }
    }
    return false;
}

bool InputSystem::Impl::ClaimsGamepadButton(int gamepadIndex, int button) const
{
    if (HasPollClaim(m_GamepadPollClaim[gamepadIndex][button]))
        return true;

    // A gamepad binding names a button, not a pad, and action evaluation reads
    // the first pad — so a binding claims that pad's button and no other's,
    // rather than consuming a press no action can act on.
    if (gamepadIndex != 0)
        return false;

    for (const auto& ctxPair : m_Contexts)
    {
        if (!ctxPair.second.enabled)
            continue;
        for (const auto& actPair : ctxPair.second.actions)
        {
            // Axis bindings carry axis codes, never a button.
            if (actPair.second.isAxis)
                continue;
            for (const ActionBinding& b : actPair.second.bindings)
            {
                if (b.device != DeviceType::Gamepad || b.code != button)
                    continue;
                if (MatchesMods(m_CurrentMods, b))
                    return true;
            }
        }
    }
    return false;
}

bool InputSystem::Impl::ClaimsScroll(Mathematics::Vector2 delta) const
{
    for (const auto& ctxPair : m_Contexts)
    {
        if (!ctxPair.second.enabled)
            continue;
        for (const auto& actPair : ctxPair.second.actions)
        {
            if (!actPair.second.isAxis)
                continue;
            for (const ActionBinding& b : actPair.second.bindings)
            {
                if (b.device != DeviceType::Mouse)
                    continue;
                // Only the axis that actually moved: a game bound to wheel zoom
                // does not take a horizontal trackpad pan.
                const bool axisMoved = (b.code == kMouseScrollAxisX && delta.x != 0.0f) ||
                                       (b.code == kMouseScrollAxisY && delta.y != 0.0f);
                if (axisMoved && MatchesMods(m_CurrentMods, b))
                    return true;
            }
        }
    }
    return false;
}

void InputSystem::Impl::UpdateActionsFromKeys() {
    // For each context, recompute action states based on bound keys and mouse input.
    for (auto &ctxPair : m_Contexts) {
        ContextData &ctx = ctxPair.second;
        if (!ctx.enabled)
            continue;

        for (auto &actPair : ctx.actions) {
            const ActionId   id   = actPair.first;
            const ActionDesc &desc = actPair.second;
            ActionState &     st   = ctx.states[id];

            bool  anyDown  = false;
            bool  anyPress = false;
            bool  anyRel   = false;
            float value    = 0.0f;

            for (const ActionBinding &b : desc.bindings) {
                if (b.device == DeviceType::Keyboard) {
                    if (!IsValidKeyCode(b.code))
                        continue;

                    const KeyState &ks = m_Keys[b.code];

                    // For transitions, evaluate modifiers as they were at the moment the key was pressed.
                    // For held state, evaluate against the current modifier snapshot.
                    const int modsForTransition = ks.modsAtPress;
                    const int modsForHeld       = m_CurrentMods;

                    if (ks.pressedThis) {
                        if (MatchesMods(modsForTransition, b))
                            anyPress = true;
                    }

                    if (ks.releasedThis) {
                        if (MatchesMods(modsForTransition, b))
                            anyRel = true;
                    }

                    if (ks.down) {
                        if (!MatchesMods(modsForHeld, b))
                            continue;
                        anyDown = true;
                        if (desc.isAxis)
                            value += b.scale;
                    }
                } else if (b.device == DeviceType::Mouse) {
                    if (!desc.isAxis) {
                        // Buttons are judged like keys: the edges by the modifiers
                        // held at the press, the hold by the current snapshot.
                        if (!IsValidMouseButton(b.code))
                            continue;
                        const KeyState &bs = m_MouseButtons[b.code];
                        const bool pressMatches = MatchesMods(bs.modsAtPress, b);
                        if (bs.pressedThis && pressMatches)
                            anyPress = true;
                        if (bs.releasedThis && pressMatches)
                            anyRel = true;
                        if (bs.down && MatchesMods(m_CurrentMods, b))
                            anyDown = true;
                    } else {
                        if (!MatchesMods(m_CurrentMods, b))
                            continue;
                        // Axis-style mouse bindings use synthetic codes for deltas/scroll.
                        switch (b.code) {
                        case kMouseAxisX:       value += m_MouseDelta.x * b.scale; break;
                        case kMouseAxisY:       value += m_MouseDelta.y * b.scale; break;
                        case kMouseScrollAxisX: value += m_Scroll.x * b.scale;     break;
                        case kMouseScrollAxisY: value += m_Scroll.y * b.scale;     break;
                        default:
                            // For completeness: allow treating a held mouse button as a 1.0f axis.
                            if (IsValidMouseButton(b.code) && m_MouseButtons[b.code].down)
                                value += b.scale;
                            break;
                        }
                    }
                } else if (b.device == DeviceType::Gamepad) {
                    if (!MatchesMods(m_CurrentMods, b))
                        continue;
                    const GamepadState& pad = m_Gamepads[0];
                    if (!pad.connected)
                        continue;
                    if (desc.isAxis) {
                        const int axisIdx = b.code;
                        if (axisIdx >= 0 && axisIdx < kGamepadAxisCount)
                            value += pad.axes[axisIdx] * b.scale;
                    } else if (b.code >= 0 && b.code < kGamepadButtonCount && pad.buttons[b.code]) {
                        anyDown = true;
                    }
                }
            }

            st.justPressed  = anyPress;
            st.justReleased = anyRel;
            st.pressed      = anyDown;
            if (desc.isAxis)
                st.value = value;
        }
    }
}

} // namespace Input
} // namespace GameEngine
