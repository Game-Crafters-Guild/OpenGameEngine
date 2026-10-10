#pragma once

#include "UI/Internal/AttachStateAccess.h"

namespace GameEngine
{
class UIElement;
class UIManager;

namespace UI::Detail
{

// NOT PUBLIC API. The attach/detach machinery's internals, declared here rather than on
// UIElement.h because every caller is a .cpp inside this module — only the enqueue entry
// point (UIElement.h) is reachable from header-inline code and has to stay there.
//
// Leave the element's settle queue, from ~UIElement. A no-op while the manager is being
// destroyed.
void ForgetAttachSettle(UIManager* owner, UIElement* el);

// True while `el` is inside a destruction-detach dispatch. The tree-edit refusals consult
// it so a handler cannot resurrect or re-parent something already condemned.
//
// Inline, and deliberately so: this is on the front of AddChild / InsertChild / TakeChild,
// which run per tree mutation — a document build or a virtualized list rebind calls them in
// bulk. As a cross-TU call it cost a call and a load on every one of them to answer "no"
// in every case but a teardown; inline it is the load and a predictable branch.
inline bool IsDoomed(const UIElement* el)
{
    return el != nullptr && UIAttachStateAccess::Doomed(*el);
}

} // namespace UI::Detail
} // namespace GameEngine
