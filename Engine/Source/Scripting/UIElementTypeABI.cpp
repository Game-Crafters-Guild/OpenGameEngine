// UIElementTypeABI.cpp — the TYPE-LIFECYCLE layer of the game-UI scripting ABI: owner minting,
// tag registration, the orphan sweep, and the reload verdict. Nothing here addresses an element.
// That is the split from UIElementABI.cpp, which is the per-ELEMENT layer and takes an element
// instance id in every export: this file's subjects are the TYPE and the load context that
// defines it, and an export here that took an instance id would be in the wrong file.
//
// THE OWNER-VALIDATION CONTRACT, which is the whole reason this layer is not just a thin
// forwarder. ManagedElementTypes takes an owner id as a plain number and trusts it, which is
// correct for C++ callers — native code is inside the trust boundary and could call
// UnregisterFactoriesOwnedBy directly anyway. Managed code is NOT: a script can declare its own
// DllImport and pass any number it likes, and naming another context's owner would let it orphan
// that context's elements. So the id is minted by ManagedTypeOwners and never chosen by the
// caller, and EVERY export here that takes one validates it through
// ManagedTypeOwners::ValidateOwner BEFORE touching ManagedElementTypes. A refusal is
// GE_Result_InvalidArg and is also LOGGED, because a return code alone leaves a forged id
// indistinguishable from a call that legitimately had nothing to do.
//
// GE_UI_SetElementTypeCallbacks is the one export that takes no owner and so is not
// owner-validated. It installs process-wide function pointers from the pre-loaded default
// context, which is never unloaded — there is no per-context state for an owner to scope, and
// nothing for one context to take from another by calling it.
//
// The three-state machine has a required ORDER the managed side already depends on: orphan at
// the unload seam, then NotifyReloadCompleted with the SAME id once the reload finishes, and
// only then ReleaseTypeOwner. Notify does not release, and must not — retiring the id before the
// verdict would make a validating ABI refuse the very call that closes the machine.
//
// That announced verdict is not the only one. A context that owned no element type unloads
// without announcing anything, so restoring a deleted type would reconcile nothing; the engine
// therefore closes each registration window with a verdict for every owner that claimed inside
// it. Nothing is exported for that — it is a property of the window, not a call the caller makes.
//
// Mirrors UIElementABI.cpp: extern "C", GE_API/GE_CDECL, every body try/catch -> GE_Result_Fail.
//
// Threading, and it is not uniform across these exports:
//
//   Any thread   AcquireTypeOwner, ReleaseTypeOwner (ManagedTypeOwners is locked), and
//                RequestTypeRegistrationWindow, whose whole purpose is to be callable from a
//                reload thread.
//   Main only    RegisterElementType, NotifyReloadCompleted and OrphanElementTypes. Claiming a
//                tag writes the factory registry that document builds read; the verdict
//                constructs instances and edits classes on live elements; and the orphan sweep
//                performs the symmetric, larger write of that same registry — unregistering a
//                whole owner's factories, aliases and attribute handlers. None can marshal
//                itself: registration has to return the tag id synchronously (*outTagId below),
//                an id cannot be computed off-thread because it is read from the map the claim
//                writes, and the orphan seam's handle release is only valid while the unload
//                callback is on the stack. So all three are REFUSED off-main instead.
//
//                ORPHANING IS A CONTRACT ON THE CALLER, not a best effort. Refused, nothing is
//                swept, and both consequences last the whole process: the unloading context can
//                never be collected (the runtime's own detector reports that half), and its TAGS
//                stay claimed, so those types can never be registered again. That is the intended
//                failure, because the alternative is mutating a registry the UI thread is
//                reading. The editor's reload already satisfies it (the unload runs inside the
//                main-thread swap task).
//
// This file installs both halves of that contract onto ManagedElementTypes — the marshaller and
// the main-thread predicate, both backed by ScriptManager — alongside the callbacks.

#include "Scripting/ScriptingABI.h" // GE_API, GE_CDECL, GE_Result

#include "AbiMainThread.h"
#include "DllEngineBootstrap.h"
#include "Scripting/ManagedElementTypes.h"
#include "Scripting/ManagedTypeOwners.h"
#include "Scripting/ScriptManager.h"

#include <functional>
#include <utility>

namespace
{
using GameEngine::Scripting::ManagedElementTypes;
using GameEngine::Scripting::ManagedTypeOwners;
using GameEngine::Scripting::OwnerEntryPoint;

// Hand a tree-visible transition to the frame loop. Resolved per call rather than captured:
// EngineCore::Initialize and Shutdown replace the ScriptManager, and the one alive at dispatch
// time is the one that must run the task.
void QueueOnMainThread(std::function<void()> task)
{
    GameEngine::EngineCore::GetInstance().GetScriptManager().QueueMainThreadTask(std::move(task));
}
} // namespace

extern "C"
{

// Mint an owner id for one managed load context. Ids are random rather than sequential and are
// never reused once retired; see ManagedTypeOwners.h for what that closes and what it does not.
GE_API GE_Result GE_CDECL GE_UI_AcquireTypeOwner(uint64_t* outOwner)
{
    try
    {
        if (!outOwner)
            return GE_Result_InvalidArg;
        *outOwner = 0;
        if (GameEngine::DllBootstrap::EnsureEngineInitialized() != GE_Result_Ok)
            return GE_Result_Fail;
        *outOwner = ManagedTypeOwners::Instance().Acquire();
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Retire an owner id. GE_Result_InvalidArg when it was not live — a double release, or an id
// that was never minted. Deliberately does NOT sweep the owner's types: orphaning and retiring
// are separate steps with a required order, and the caller performs them in it.
GE_API GE_Result GE_CDECL GE_UI_ReleaseTypeOwner(uint64_t owner)
{
    try
    {
        if (GameEngine::DllBootstrap::EnsureEngineInitialized() != GE_Result_Ok)
            return GE_Result_Fail;
        ManagedTypeOwners& owners = ManagedTypeOwners::Instance();
        if (!owners.Release(owner))
        {
            // Release answers false rather than reporting, because "not live" is its return
            // value; the refusal record is this layer's to make.
            owners.ReportRejected(owner, OwnerEntryPoint::ReleaseTypeOwner);
            return GE_Result_InvalidArg;
        }
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Install the calls into the defining language, plus the marshaller the orphan transition needs.
//
// No owner, and so no owner validation: these pointers are process-wide and are installed from
// the pre-loaded default context, which never unloads. There is no per-context state here for
// one context to name instead of another's, which is what owner validation exists to prevent
// everywhere else in this file.
//
// The marshaller is installed HERE rather than being reached for by ManagedElementTypes: that
// policy compiles into Engine and is asserted by suites that never stand up an EngineCore, and
// ScriptManager is only reachable through the engine root singleton.
GE_API GE_Result GE_CDECL GE_UI_SetElementTypeCallbacks(
    intptr_t (GE_CDECL* create)(uint64_t tagId, uint64_t instanceId),
    void (GE_CDECL* applyAttribute)(intptr_t handle, const char* name, const char* value),
    void (GE_CDECL* release)(intptr_t handle),
    void (GE_CDECL* performRegistrations)())
{
    try
    {
        if (GameEngine::DllBootstrap::EnsureEngineInitialized() != GE_Result_Ok)
            return GE_Result_Fail;

        // ManagedElementCallbacks spells these as plain function pointers, which is the same
        // type as a GE_CDECL one under every toolchain this builds on (GE_CDECL is __cdecl on
        // Windows, where __cdecl is also the default, and empty elsewhere). A toolchain that
        // changed the default calling convention would fail this assignment rather than
        // silently mismatch the managed `delegate* unmanaged[Cdecl]`.
        GameEngine::Scripting::ManagedElementCallbacks callbacks{};
        callbacks.Create = create;
        callbacks.ApplyAttribute = applyAttribute;
        callbacks.Release = release;
        callbacks.PerformRegistrations = performRegistrations;

        // ORDER IS LOAD-BEARING: the callbacks go LAST.
        //
        // Installing them is what opens every gate. RequestRegistrationWindow returns early
        // unless PerformRegistrations is set, and the window it then queues reads the marshaller;
        // a proxy built by a document reads Create. So publishing the callbacks first would leave
        // an interval in which another thread can be inside RunOnMainThread reading m_Marshaller
        // while this thread is still assigning it — a torn read of a std::function, not a stale
        // one. Installing the marshaller and the predicate first closes that interval by
        // construction rather than by trusting the caller's ordering, which matters because these
        // are public C exports that a script can call with its own DllImport.
        ManagedElementTypes& types = ManagedElementTypes::Instance();
        types.SetMainThreadMarshaller(&QueueOnMainThread);
        types.SetMainThreadPredicate(&GameEngine::ScriptingAbi::OnEngineMainThread);
        types.SetCallbacks(callbacks);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Ask for a main-thread slot in which to register types and close reload verdicts.
//
// THE ONE EXPORT HERE A RELOAD THREAD MAY CALL. The defining language stages its batch wherever
// its reload handler happens to run — which is the thread pool whenever the managed pump's
// watchdog fires — and calls this; the engine then calls PerformRegistrations back on the main
// thread, and the language does its registering and notifying synchronously from inside that.
//
// No owner and no owner validation: it names no context and carries no payload. All it can do is
// cause a callback the caller installed itself to run a frame earlier than never.
GE_API GE_Result GE_CDECL GE_UI_RequestTypeRegistrationWindow(void)
{
    try
    {
        if (GameEngine::DllBootstrap::EnsureEngineInitialized() != GE_Result_Ok)
            return GE_Result_Fail;
        ManagedElementTypes::Instance().RequestRegistrationWindow();
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Claim one tag for this owner. It touches no element: what happens to elements already orphaned
// under the tag is decided by a verdict, which the window issues for this owner once the whole
// batch has returned.
//
// *outTagId is 0 when the tag was REFUSED — it belongs to the engine or to another live
// context — and that is GE_Result_Ok, not an error. The caller registers a whole assembly's
// types in a loop and already treats a 0 tag id as a per-type skip; failing the call would
// turn one colliding tag into a silent non-registration of every type that followed it. The
// engine has already logged which tag and why.
GE_API GE_Result GE_CDECL GE_UI_RegisterElementType(uint64_t owner, const char* tagName, uint64_t* outTagId)
{
    try
    {
        if (!outTagId)
            return GE_Result_InvalidArg;
        *outTagId = 0;
        if (GameEngine::DllBootstrap::EnsureEngineInitialized() != GE_Result_Ok)
            return GE_Result_Fail;
        // Before the tag-name check, so a forged owner is recorded whatever else the call got
        // wrong.
        if (!ManagedTypeOwners::Instance().ValidateOwner(owner, OwnerEntryPoint::RegisterElementType))
            return GE_Result_InvalidArg;
        if (!tagName || *tagName == '\0')
            return GE_Result_InvalidArg;
        *outTagId = ManagedElementTypes::Instance().RegisterType(tagName, owner);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// The owner's context is unloading. Its elements drop their instances and go Pending while
// staying in the tree, and its tags are unregistered. Call it from the unload seam, before the
// context actually goes: the handles have to be released while they are still valid.
GE_API GE_Result GE_CDECL GE_UI_OrphanElementTypes(uint64_t owner, uint64_t* outOrphaned)
{
    try
    {
        if (!outOrphaned)
            return GE_Result_InvalidArg;
        *outOrphaned = 0;
        if (GameEngine::DllBootstrap::EnsureEngineInitialized() != GE_Result_Ok)
            return GE_Result_Fail;
        if (!ManagedTypeOwners::Instance().ValidateOwner(owner, OwnerEntryPoint::OrphanElementTypes))
            return GE_Result_InvalidArg;
        *outOrphaned = static_cast<uint64_t>(ManagedElementTypes::Instance().OrphanTypesOwnedBy(owner));
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// The reload of this owner's context finished. Whatever of its elements is still Pending did not
// come back and is now Faulted.
//
// The owner must still be LIVE here, which is why this does not release it: the id is what names
// the elements whose fate is being decided, and retiring it first would make this call refuse
// itself. The caller releases it immediately after.
GE_API GE_Result GE_CDECL GE_UI_NotifyReloadCompleted(uint64_t owner)
{
    try
    {
        if (GameEngine::DllBootstrap::EnsureEngineInitialized() != GE_Result_Ok)
            return GE_Result_Fail;
        if (!ManagedTypeOwners::Instance().ValidateOwner(owner, OwnerEntryPoint::NotifyReloadCompleted))
            return GE_Result_InvalidArg;
        ManagedElementTypes::Instance().NotifyReloadCompleted(owner);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

} // extern "C"
