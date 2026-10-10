#include "UI/UiPostHandle.h"

#include "UI/UiDispatcher.h"

#include <utility>

namespace GameEngine::UI
{

bool UiPostHandle::Post(std::function<void()> action) const
{
    if (!m_Link)
        return false;
    const UiPostTarget::Route route = m_Link->Route.Load();
    if (!route.Dispatcher)
        return false;
    // The drain runs on the UI thread, where ElementAlive is written, so reading it there is safe.
    return route.Dispatcher->Post([link = m_Link, action = std::move(action)]() {
        if (link->ElementAlive)
            action();
    });
}

} // namespace GameEngine::UI
