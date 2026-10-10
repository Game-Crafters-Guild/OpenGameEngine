#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "Logger/Backtrace.h"

namespace GameEngine {

class UIElement;

// Builds a multi-span row representing a single backtrace frame, used by
// LogPanel's detail pane. Splitting index/module/symbol/path into separate
// spans (with a break-all source row) keeps long absolute paths from blowing
// up layout — a flat one-Label-per-frame approach stalls on deep paths.
//
// If onOpenSource is set and the frame has a file, the row becomes clickable
// and invokes the callback on mouseup.
std::unique_ptr<UIElement> BuildBacktraceFrameRow(
    std::size_t index,
    const Logger::BacktraceFrame& frame,
    std::function<void(const std::string&, int)> onOpenSource = {});

} // namespace GameEngine
