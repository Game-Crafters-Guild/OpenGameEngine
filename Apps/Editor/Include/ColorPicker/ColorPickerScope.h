#pragma once

#include "InspectorRegistry.h" // ColorPickerCallbacks

#include <functional>
#include <memory>
#include <vector>

namespace GameEngine
{
namespace Editor
{

/// Closes the colour pickers opened under it. An opener whose picker callbacks
/// capture state that is torn down while a picker can still be open (a settings
/// page and its rows) owns a scope, sets ColorPickerCallbacks::scope when it
/// opens a picker, and calls CloseAll when that state goes. Destroying the scope
/// closes its pickers too. A picker closed this way calls none of its callbacks.
class ColorPickerScope
{
  public:
    ColorPickerScope() = default;
    ~ColorPickerScope();
    ColorPickerScope(const ColorPickerScope&) = delete;
    ColorPickerScope& operator=(const ColorPickerScope&) = delete;

    void CloseAll();

    /// Called by the presenter that opens the picker: callbacks that run only
    /// until the next CloseAll or the scope's destruction.
    ColorPickerCallbacks Bind(ColorPickerCallbacks callbacks) const;
    /// Called by the presenter that opens the picker: closes that picker. It
    /// must do nothing when the picker has already closed.
    void AddCloseAction(std::function<void()> close);

  private:
    struct Generation
    {
    };

    void RunCloseActions();

    // Replaced on every CloseAll, so callbacks bound before it hold an expired
    // reference and do nothing.
    std::shared_ptr<Generation> m_Generation = std::make_shared<Generation>();
    std::vector<std::function<void()>> m_CloseActions;
};

} // namespace Editor
} // namespace GameEngine
