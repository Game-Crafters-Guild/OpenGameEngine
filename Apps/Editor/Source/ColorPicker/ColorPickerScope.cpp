#include "ColorPicker/ColorPickerScope.h"

#include <utility>

namespace GameEngine
{
namespace Editor
{

ColorPickerScope::~ColorPickerScope()
{
    RunCloseActions();
}

void ColorPickerScope::CloseAll()
{
    m_Generation = std::make_shared<Generation>();
    RunCloseActions();
}

ColorPickerCallbacks ColorPickerScope::Bind(ColorPickerCallbacks callbacks) const
{
    const std::weak_ptr<Generation> generation = m_Generation;
    ColorPickerCallbacks bound;
    if (callbacks.onApply)
    {
        bound.onApply = [generation, fn = std::move(callbacks.onApply)](uint32_t argb, float intensity)
        {
            if (!generation.expired())
                fn(argb, intensity);
        };
    }
    if (callbacks.onCancel)
    {
        bound.onCancel = [generation, fn = std::move(callbacks.onCancel)]()
        {
            if (!generation.expired())
                fn();
        };
    }
    if (callbacks.onValueChanging)
    {
        bound.onValueChanging = [generation, fn = std::move(callbacks.onValueChanging)](uint32_t argb, float intensity)
        {
            if (!generation.expired())
                fn(argb, intensity);
        };
    }
    return bound;
}

void ColorPickerScope::AddCloseAction(std::function<void()> close)
{
    m_CloseActions.push_back(std::move(close));
}

void ColorPickerScope::RunCloseActions()
{
    // Taken out first, so a close action that re-enters this scope sees an empty list.
    std::vector<std::function<void()>> closeActions = std::exchange(m_CloseActions, {});
    for (const std::function<void()>& close : closeActions)
        close();
}

} // namespace Editor
} // namespace GameEngine
