#pragma once

#include "UI/UIManagerRef.h"

namespace GameEngine
{

class UIElement;
class UIManager;

/// A transient surface that closes when the pointer presses outside it, or
/// when Escape reaches the manager unhandled: dropdowns, popovers, menus,
/// search dialogs.
///
/// The manager owns the dismissal policy, so a popup never needs a transparent
/// full-window click-catcher of its own. That matters because a catcher can
/// only ever cover its own layout parent: a popup parented inside a dock panel
/// leaves every other panel unable to dismiss it, and stretching the catcher
/// past its parent means re-expressing the popup's geometry in window space.
/// Registering here costs neither.
///
/// While any registered popup is open the manager also gates pointer targets
/// to the open popups, so input outside them is swallowed rather than
/// delivered to whatever sits underneath.
///
/// Mix into a UIElement subclass and forward one call:
///
///     class MyPopup : public UIElement, public DismissablePopup
///     {
///       public:
///         MyPopup() : DismissablePopup(this) {}
///         void OnOwnerManagerChanged(UIManager* owner) override
///         {
///             UpdatePopupRegistration(owner);
///         }
///         bool IsPopupOpen() const override { return m_Open; }
///         void DismissPopup() override { Close(); }
///     };
class DismissablePopup
{
  public:
    virtual ~DismissablePopup();

    /// The subtree that counts as "inside": a press on it or any descendant
    /// never dismisses the popup. Defaults to the element itself, which is
    /// right whenever the element is the visible surface. Override only for a
    /// popup whose root is a larger invisible box than the surface it draws —
    /// returning that box would count every press inside it as "inside" and
    /// the popup would never dismiss.
    virtual UIElement* GetPopupRoot();

    /// A press inside a SIBLING surface of the same logical popup also counts
    /// as inside: a context menu is one popup made of several panels (root +
    /// submenus), and a press on a submenu row must not dismiss the root out
    /// from under it — the root's teardown destroys the pressed row before
    /// its click dispatches. Default: no group, only the own subtree counts.
    virtual bool PressWithinPopupGroup(const UIElement* /*pressTarget*/) const { return false; }

    /// Only open popups gate pointer input and answer to Escape.
    ///
    /// Deliberately not a bool on this base: every popup already owns
    /// authoritative open state (a CSS class, a visibility flag), and
    /// mirroring it here would be a second copy free to desync from the one
    /// that decides what is actually on screen.
    virtual bool IsPopupOpen() const = 0;

    /// Close in response to an outside press or Escape. Called only while
    /// IsPopupOpen() reports true, and must leave it reporting false.
    ///
    /// No default: closing means something different per popup — a dropdown
    /// drops its "open" class, a dialog cancels and notifies, a popover hides.
    /// A default that guessed would be overridden by every implementer.
    virtual void DismissPopup() = 0;

  protected:
    /// `self` is the UIElement this popup is mixed into. Stored only, never
    /// dereferenced during construction, so passing `this` from a
    /// member-initializer list is safe.
    ///
    /// Takes a pointer, not a reference: `*this` in a class deriving both
    /// UIElement and DismissablePopup converts to `UIElement&` and to the copy
    /// constructor's `const DismissablePopup&` equally well, which is
    /// ambiguous. A pointer cannot bind to the copy constructor at all.
    explicit DismissablePopup(UIElement* self) : m_Self(self) {}

    DismissablePopup(const DismissablePopup&) = delete;
    DismissablePopup& operator=(const DismissablePopup&) = delete;

    /// Call from OnOwnerManagerChanged. Keeps the registration on exactly one
    /// manager across re-homing, and drops it on destruction.
    void UpdatePopupRegistration(UIManager* owner);

  private:
    UIElement* m_Self = nullptr;
    UIManagerRef m_RegisteredManager;
};

} // namespace GameEngine
