#pragma once

namespace GameEngine::Editor
{
class DebugRequestGateRegistry;

// Registers the mark-ups' attribution gate ("markups"): every debug-server request runs
// inside an Agent scope of the installed MarkupEditorBridge, closed when its synchronous
// handling ends, so markup_*, set_component, undo and redo over the port stamp Agent. The
// input-injection methods (send_key, input_text, send_text, click_element, move_pointer,
// perform_drop, simulate_mouse_drag, simulate_double_click_drag) run outside it: what the
// injected input does counts as the user's, whether the editor handles it inside the
// request or in a later frame.
void RegisterMarkupRequestGate(DebugRequestGateRegistry& registry);

} // namespace GameEngine::Editor
