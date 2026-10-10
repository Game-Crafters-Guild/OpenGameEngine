#pragma once

#include "UI/UiPostHandle.h"

#include <functional>
#include <memory>

namespace GameEngine::UI
{

/// Turns a burst of requests from any thread into one run of an action on an element's UI
/// thread: the first Request posts the action, and later ones are absorbed until it starts.
///
/// Built for listeners that fire on another thread (VCS status polls, file-watch events) and
/// want "refresh once, soon". The listener captures a copy and calls Request; the copy shares
/// its state by ownership, so it touches neither the element nor the object that owns the
/// action, and stays safe after both are gone.
///
/// The owner calls Cancel before anything the action touches is destroyed; after that the action
/// never runs again, including a run already queued. The action also does not run if the
/// element behind the post handle has been destroyed.
class UiCoalescedPost
{
  public:
    /// Requests do nothing.
    UiCoalescedPost() = default;

    /// UI thread. `action` runs on the UI thread of `target`'s element.
    UiCoalescedPost(UiPostHandle target, std::function<void()> action);

    /// Any thread.
    void Request() const;

    /// UI thread.
    void Cancel() const;

  private:
    struct State;
    static void RunRequested(const std::shared_ptr<State>& state);

    std::shared_ptr<State> m_State;
};

} // namespace GameEngine::UI
