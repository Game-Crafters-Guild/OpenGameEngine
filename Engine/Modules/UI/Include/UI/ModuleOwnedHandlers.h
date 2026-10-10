#pragma once

#include <cstddef>
#include <cstdint>
#include <typeinfo>

namespace GameEngine
{
class UIElement;

namespace UI
{

// Ownership of UI event-handler callables by hot-swappable module images.
//
// A native user module's std::function is code and constant data inside that
// module's image. Stored in UIElement::m_EventHandlers it outlives the image, and
// nothing in the table notices: DispatchToHandlers walks the entries in place and
// calls straight through the live one, so the first dispatch after the unmap calls
// into unmapped code — and the entry does not even have to be dispatched to be
// fatal, since destroying the std::function runs the type-erasure code that the
// callable's own translation unit emitted, which lives in that same image.
// Revoking those callables while their image is still mapped needs one fact the
// handler table never recorded: which image a stored callable came from.
//
// The answer comes from the callable, never from the caller and never from the
// tree: std::function::target_type() names the stored callable, and the
// type_info naming it is emitted by the translation unit that instantiated the
// erasure — i.e. it lives in the image whose code the callable is. Testing that
// address against the loader's mapped hot-swappable ranges is therefore exact.
// (Proven per-image under this build by NativeModuleImageOwnershipTests.)
//
// Why that satisfies the constraint STRUCTURALLY: an engine handler's callable
// lives in Engine.dll or Editor.exe, and neither is ever in the hot-swappable
// set. A Button's internal handlers, scrollbar drag, dock chrome and tooltip
// overlays cannot be stamped, so they cannot be revoked — not by policy, not by
// a predicate a later change could loosen. Built-in elements registering
// handlers is expressly fine.
//
// Main thread only, like every other module-registration protocol in the engine
// — and note that this RAISES the cost of breaking that rule. UIElement was
// already main-thread-only, but an off-thread registration used to touch one
// element's own table; it now also touches the process-wide image list and
// element index below. A registration racing a module load would race
// `CloseImageAttribution`'s append to the image list, and racing a revocation
// would race the index the pass is copying. The failure is container corruption,
// not a wrong attribution — but the blast radius is the whole UI, not one
// element. Every entry point that writes either table asserts the rule against
// the thread that first wrote; the check compiles out with NDEBUG.
//
// Every entry point is a no-op while no hot-swappable image is mapped, which is
// every host that never loads a user module (unit tests, tools, the Player
// before its prebuilt load) — those keep today's behaviour exactly.

// ---------------------------------------------------------------------------
// Loader side — called by EngineCore's module-image observer.
// ---------------------------------------------------------------------------

// Bracket the raw image map. A module's static initializers run INSIDE
// LoadLibrary, before the image base exists to test against, so registrations
// they make cannot be attributed when they happen. Between these two calls
// every registration records its callable's address provisionally;
// CloseImageAttribution resolves the provisional set against the now-known
// range and drops everything outside it — so an engine handler registered
// during the window is dropped rather than stamped. `base == 0` means the map
// failed and every provisional stamp is dropped.
void OpenImageAttribution();
void CloseImageAttribution(std::uint64_t base, std::uint64_t size);

// Retract a range at unmap. After this the image's addresses attribute to
// nothing, which is correct: there is no image left to own them.
void RetractHotSwappableImage(std::uint64_t base);

// Handlers still holding a callable inside [base, base + size). Read by the
// unload quiesce ledger AFTER revocation ran, so a non-zero answer means
// revocation missed one and the image must stay mapped.
std::size_t CountHandlersOwnedByImage(std::uint64_t base, std::uint64_t size);

// Release every handler whose callable lives in [base, base + size), through
// the same release path an explicit unregister uses. Returns how many were
// revoked. Must run while the image is still mapped — destroying a
// std::function calls back into the image that built it.
std::size_t RevokeHandlersOwnedByImage(std::uint64_t base, std::uint64_t size);

// ---------------------------------------------------------------------------
// Registration side — called from UIElement's header-inline registration path,
// so these run in every consuming image and must resolve to Engine.dll's single
// copy (the double-link disease; see UIElement::SetInEventDispatch).
// ---------------------------------------------------------------------------

// The identity address of a callable owned by a mapped hot-swappable image (or
// by an image currently being mapped); 0 for engine-owned code and for every
// host with no such image mapped.
std::uint64_t AttributeCallableOwner(const std::type_info& ti);

// True if addr lies in a currently published hot-swappable image. Member-slot
// revocation uses it to drop a provisional stamp that resolved outside every
// mapped image, the same way CloseImageAttribution does for table entries.
bool AddressIsInsideMappedImage(std::uint64_t addr);

// Track/untrack an element holding at least one stamped handler. The index is
// what keeps revocation off the element tree: it holds only elements a user
// module actually registered on, which is nothing at all for the editor's tens
// of thousands of chrome registrations.
void NoteElementOwnsModuleHandler(UIElement* el);
void ForgetElementOwnsModuleHandler(UIElement* el);

// Elements currently carrying at least one stamped handler. Test seam.
std::size_t IndexedElementCount();

} // namespace UI
} // namespace GameEngine
