#pragma once

namespace GameEngine {

class UIElement;

/** True when canvas Delete / Cmd+C / Cmd+V would steal keys from an inline node field. */
bool GraphCanvasShouldIgnoreEditKeys(UIElement* focused);

} // namespace GameEngine
