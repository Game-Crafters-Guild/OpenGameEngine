#pragma once

#include "UI/UiPostTarget.h"

#include <functional>
#include <memory>

namespace GameEngine
{
class UIElement;

namespace UI
{

/// The state an element shares with its UiPostHandles. The element keeps Route in step with its
/// owning UIManager and, in its destructor, clears Route and ElementAlive. Route is read from any
/// thread under its own lock; ElementAlive is written and read on the UI thread only (by the
/// element's destructor and by posted actions as they are drained).
struct UiPostLink
{
    UiPostTarget Route;
    bool ElementAlive = true;
};

/// Posts work to an element's UI thread from any thread without touching the element.
///
/// Take it on the UI thread with UIElement::GetPostHandle and copy it into the job, file-watch,
/// VCS or download callback that has to get back. Post never dereferences the element, so the
/// handle stays safe to use after the element is destroyed, and it follows the element when it
/// moves to another UIManager (a panel torn off into its own window, then re-docked).
///
/// An accepted action runs on the UI thread at the next drain of the manager that owned the
/// element when Post was called, provided the element still exists then; if it was destroyed in
/// between, the action is skipped. Post returns false and drops the action when the element has
/// been destroyed, has no owning UIManager, or that manager is gone. A caller that latched state
/// on the action running must release the latch on false. A default-constructed handle refuses
/// everything.
class UiPostHandle
{
  public:
    UiPostHandle() = default;

    /// Any thread.
    bool Post(std::function<void()> action) const;

  private:
    friend class GameEngine::UIElement;
    explicit UiPostHandle(std::shared_ptr<UiPostLink> link) : m_Link(std::move(link)) {}

    std::shared_ptr<UiPostLink> m_Link;
};

} // namespace UI
} // namespace GameEngine
