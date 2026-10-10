// Cross-DLL registration smoke test — the user-side probe DLL.
//
// This is the stand-in for a Phase-2 native-scripting user DLL: a separately
// linked SHARED module that links PRIVATE the Engine import lib (Engine.lib /
// Engine.dll) and registers a component into the engine's reflection registries.
//
// The whole point of the test is to prove that ComponentFieldRegistry and
// ComponentFactory are a SINGLE instance shared across host EXE + Engine.dll +
// this user DLL — i.e. that the function-local-static storage inside
// ComponentFieldRegistry.cpp / ComponentFactory.cpp (compiled once into the ECS
// OBJECT lib and spliced into Engine.dll) is NOT cloned per linking unit. That
// holds only because this DLL links PRIVATE Engine (the import lib) rather than
// the ECS module object code directly. Linking ECS directly would clone the
// statics — exactly the hazard the host test is designed to catch.
//
// Two probe components exercise the two registration mechanisms the Phase-2 ABI
// must reason about:
//
//   * CrossDllProbeExplicit — registered ONLY when the host calls the exported
//     GE_NativeProbe_RegisterExplicit(). This is the mechanism the spec prefers
//     for the user ABI: an explicit GE_UserModule_Register_v1 entry point, no
//     reliance on static-init order or linker retention.
//
//   * CrossDllProbeStatic — registered via the GE_REGISTER_COMPONENT static-init
//     path (an anonymous-namespace object whose initializer runs at DLL load).
//     The host inspects whether this survived into the DLL image and ran on
//     LoadLibrary. Whether the linker keeps an otherwise-unreferenced static in a
//     DLL is itself a documented finding: if it can be stripped, that is the
//     motivation for Phase 2's `ge_reg` linker-section data model over static
//     constructors.

#include "Components/ComponentRegistration.h"  // GE_REFLECT, GE_REGISTER_COMPONENT,
                                                // RegisterReflectedComponent, the registries,
                                                // GetComponentTypeId, GetReflectedFields,
                                                // ComponentTypeName, World, EntityHandle

#include <cstdint>

namespace gepr  // "game engine probe" — a named namespace keeps the canonical
{               // type name (and thus the consteval id) predictable.

// POD components: trivially copyable + standard layout, so they satisfy the
// ECS::Component concept and GetComponentTypeId<T>() is well-formed. Non-default
// member initializers keep them default-constructible (for the factory path)
// without affecting trivial copyability.
struct CrossDllProbeExplicit
{
    float        Health = 7.0f;
    std::int32_t Mana   = 3;
};

struct CrossDllProbeStatic
{
    float         A = 1.0f;
    std::uint32_t B = 2u;
};

} // namespace gepr

// Field table for the explicit probe. GE_REFLECT only defines the compile-time
// Reflection<T> specialization — it does NOT self-register, so this component is
// invisible to the engine until the host calls the explicit export below. That
// keeps the host's "not registered yet" precondition meaningful.
GE_REFLECT(gepr::CrossDllProbeExplicit, Health, Mana);

// The static-init probe. GE_REGISTER_COMPONENT emits the field table AND an
// anonymous-namespace static whose initializer registers the component at DLL
// load. Nothing else in this TU references that static.
GE_REGISTER_COMPONENT(gepr::CrossDllProbeStatic, A, B)

// ---------------------------------------------------------------------------
// Exports — extern "C" so GetProcAddress resolves the undecorated names.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
#  define GE_NATIVE_PROBE_API extern "C" __declspec(dllexport)
#else
#  define GE_NATIVE_PROBE_API extern "C" __attribute__((visibility("default")))
#endif

namespace ge = GameEngine::ECS;

// Register CrossDllProbeExplicit into both registries by calling straight into
// Engine.dll's exported registry methods (the same calls RegisterReflectedComponent
// makes), and return the consteval type id this DLL computed for the type so the
// host can cross-check identity stability across the DLL boundary.
GE_NATIVE_PROBE_API std::uint64_t GE_NativeProbe_RegisterExplicit()
{
    const ge::ComponentTypeId id = ge::GetComponentTypeId<gepr::CrossDllProbeExplicit>();
    ge::ComponentFieldRegistry::Register(id,
                                         ge::GetReflectedFields<gepr::CrossDllProbeExplicit>(),
                                         ge::ComponentTypeName<gepr::CrossDllProbeExplicit>());
    // A no-op creator is enough to prove the factory registry is shared; it is
    // never invoked in this test (no World is constructed).
    ge::ComponentFactory::Register(id, [](ge::World&, ge::EntityHandle) {});
    return id;
}

// The id this DLL computes for the explicit probe, WITHOUT registering it. Lets
// the host learn the expected id before triggering registration.
GE_NATIVE_PROBE_API std::uint64_t GE_NativeProbe_ExpectedExplicitId()
{
    return ge::GetComponentTypeId<gepr::CrossDllProbeExplicit>();
}

// The id of the static-init probe, so the host can ask the registry whether the
// DLL-load static initializer actually ran.
GE_NATIVE_PROBE_API std::uint64_t GE_NativeProbe_StaticId()
{
    return ge::GetComponentTypeId<gepr::CrossDllProbeStatic>();
}

// Number of reflected fields the DLL sees for the explicit probe — a fixed-point
// the host can compare its own ComponentFieldRegistry::Get(id).size() against.
GE_NATIVE_PROBE_API std::uint32_t GE_NativeProbe_ExplicitFieldCount()
{
    return static_cast<std::uint32_t>(ge::GetReflectedFields<gepr::CrossDllProbeExplicit>().size());
}

// Whether THIS DLL, after registering, observes the explicit probe through the
// (shared) registry. Sanity that the DLL's own view and the host's view coincide.
GE_NATIVE_PROBE_API int GE_NativeProbe_SelfSeesExplicit()
{
    return ge::ComponentFieldRegistry::Has(
               ge::GetComponentTypeId<gepr::CrossDllProbeExplicit>())
               ? 1
               : 0;
}

// ---------------------------------------------------------------------------
// UI handler ownership probes.
//
// A hot-swappable module's std::function is code and constant data inside the
// module image; the engine has to be able to tell such a callable apart from an
// engine-owned one BEFORE the image is unmapped. These exports mint callables
// from lambdas compiled into THIS DLL so the host can check which image the
// erasure's identifying object was emitted into.
//
// RegisterEventHandler is header-inline, so the call below compiles into this
// DLL exactly as it does in a native user system (HpBarSystem.h registers this
// way) — this is the production call shape, not a stand-in for it.
// ---------------------------------------------------------------------------

#include "UI/Controls/Button.h"
#include "UI/Controls/TreeView.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

namespace
{
// The identifying address of a std::function's stored callable: the type_info
// the type erasure emits, which lives in the image that instantiated it.
std::uint64_t CallableIdentityAddr(const GameEngine::UIElement::EventHandler& fn)
{
    return fn ? reinterpret_cast<std::uint64_t>(&fn.target_type()) : 0u;
}
} // namespace

// A callable minted here, never registered — isolates the erasure from the
// handler table.
GE_NATIVE_PROBE_API std::uint64_t GE_NativeProbe_MintHandlerIdentityAddr()
{
    int captured = 41;
    GameEngine::UIElement::EventHandler fn = [captured](GameEngine::UIEvent&) mutable { ++captured; };
    return CallableIdentityAddr(fn);
}

// Register a handler on a HOST-owned element through the ordinary public API.
// Returns the token key so the host can find the entry it created.
GE_NATIVE_PROBE_API std::uint64_t GE_NativeProbe_RegisterUiHandler(void* element, std::uint64_t eventId,
                                                                  int* firedCounter)
{
    auto* el = static_cast<GameEngine::UIElement*>(element);
    const auto token = el->RegisterEventHandler(
        static_cast<GameEngine::EventId>(eventId),
        [firedCounter](GameEngine::UIEvent&) { if (firedCounter) ++*firedCounter; });
    return token.Key;
}

// A handler that is NOT a lambda. Closure types are unique per translation unit,
// so a lambda's type descriptor can only be this DLL's. A plain function pointer
// names a type Engine.dll could equally name, which is the one shape where a
// shared type descriptor would be conceivable — and would attribute this DLL's
// callable to Engine.dll, i.e. fail in the UNSAFE direction (never revoked).
static void ProbeFreeFunctionHandler(GameEngine::UIEvent&) {}

GE_NATIVE_PROBE_API std::uint64_t GE_NativeProbe_RegisterFunctionPointerHandler(void* element,
                                                                                std::uint64_t eventId)
{
    auto* el = static_cast<GameEngine::UIElement*>(element);
    GameEngine::UIElement::EventHandler fn = &ProbeFreeFunctionHandler;
    return el->RegisterEventHandler(static_cast<GameEngine::EventId>(eventId), std::move(fn)).Key;
}

// A member callback slot rather than a handler-table entry. SetOnMouseDown is
// compiled in Engine.dll, but the callable is still this image's: the lambda is
// minted here, so target_type() names the type_info this TU emitted. That is the
// bind shape a native HUD module uses for a control callback.
GE_NATIVE_PROBE_API void GE_NativeProbe_SetOnMouseDown(void* button, int* firedCounter)
{
    auto* btn = static_cast<GameEngine::Button*>(button);
    btn->SetOnMouseDown([firedCounter](GameEngine::Button&) {
        if (firedCounter)
            ++*firedCounter;
    });
}

// The same bind shape on TreeView, whose fifteen slots are the bulk of the stamped
// member surface. Button alone would leave every TreeView slot proven only against
// the in-process fake image, which cannot exercise a real FreeLibrary.
GE_NATIVE_PROBE_API void GE_NativeProbe_SetOnTreeSelectionChanged(void* treeView, int* firedCounter)
{
    auto* tv = static_cast<GameEngine::TreeView*>(treeView);
    tv->SetOnSelectionChanged([firedCounter](GameEngine::TreeId) {
        if (firedCounter)
            ++*firedCounter;
    });
}
