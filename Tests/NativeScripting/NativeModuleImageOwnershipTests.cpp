// Cross-image ownership of UI event-handler callables.
//
// A native user module's std::function is code and constant data inside a
// hot-swappable image. Once that image is unmapped, DispatchEvent faults on the
// handler-vector COPY, before it ever consults `active`. Revoking those callables
// at unload therefore needs one thing the handler table never recorded: WHICH
// IMAGE a stored callable came from.
//
// This file is the mechanism proof for the answer the design picked: ask the
// callable. std::function::target_type() names the stored callable's type, and
// the type_info object that names it is emitted by the translation unit that
// instantiated the erasure — i.e. it lives inside the image whose code the
// callable actually is. If that holds, an address-range test against the
// loader's mapped user images discriminates user-owned callables from
// engine-owned ones STRUCTURALLY: an engine handler's identity object is in
// Engine.dll, which is never in the hot-swappable set, so it cannot be stamped
// and therefore cannot be revoked. No tree walk, no predicate to loosen.
//
// Topology (the same one CrossDllRegistrationSmokeTest uses):
//   host EXE  ── links PRIVATE Engine (import lib) ──┐
//                                                     ├──> one Engine.dll
//   probe DLL ── links PRIVATE Engine (import lib) ──┘
//
// The probe DLL stands in for UserScripts.dll: separately linked, loaded by
// LoadLibrary at runtime, and — decisively — it compiles its OWN copy of the
// header-inline RegisterEventHandler, exactly as a native user system does.

#include "Core/Application.h" // PathUtils::GetExecutableDirectory
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/EngineBuildIdentity.h"
#include "NativeScripting/NativeScriptManager.h"
#include "UI/Controls/AxisHeaderBar.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TreeView.h"
#include "UI/ModuleOwnedHandlers.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace GameEngine
{
// The handler table is protected. Friended by UIElement (see UIElement.h) so a
// test can read what the public API deliberately hides. Each test binary defines
// its own; they never coexist in one image.
struct UIEventHandlerAccess
{
    // The identifying address of an entry's stored callable — the type_info the
    // erasure emitted. Zero if the entry is absent or already released.
    static std::uint64_t CallableIdentityAddr(const UIElement& el, EventId id, std::uint64_t key)
    {
        const auto it = el.m_EventHandlers.find(id);
        if (it == el.m_EventHandlers.end())
            return 0;
        for (const auto& he : it->second)
            if (he.key == key && he.handler)
                return reinterpret_cast<std::uint64_t>(&he.handler.target_type());
        return 0;
    }

    // Same, for the first live entry of `id` — used where the key is not
    // observable (handlers an engine control registers on itself).
    static std::uint64_t FirstCallableIdentityAddr(const UIElement& el, EventId id)
    {
        const auto it = el.m_EventHandlers.find(id);
        if (it == el.m_EventHandlers.end())
            return 0;
        for (const auto& he : it->second)
            if (he.handler)
                return reinterpret_cast<std::uint64_t>(&he.handler.target_type());
        return 0;
    }
};
} // namespace GameEngine

namespace
{

using namespace GameEngine;

using MintFn = std::uint64_t (*)();
using RegisterFn = std::uint64_t (*)(void*, std::uint64_t, int*);
using SetOnMouseDownFn = void (*)(void*, int*);
using SetOnTreeSelectionChangedFn = void (*)(void*, int*);

std::filesystem::path ProbeModulePath()
{
    const std::filesystem::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    return exeDir / "NativeScriptingProbe.dll";
}

// A mapped image's address range. On Windows the base IS the HMODULE and the
// span comes straight out of the mapped PE header — no loader call, no lock,
// which is what lets the production resolver run from inside DLL_PROCESS_ATTACH.
struct ImageRange
{
    std::uint64_t Base = 0;
    std::uint64_t Size = 0;
    bool Contains(std::uint64_t addr) const { return Base != 0 && addr >= Base && addr < Base + Size; }
};

#if defined(_WIN32)
ImageRange RangeOf(HMODULE mod)
{
    ImageRange r;
    if (!mod)
        return r;
    const auto* base = reinterpret_cast<const unsigned char*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return r;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return r;
    r.Base = reinterpret_cast<std::uint64_t>(base);
    r.Size = nt->OptionalHeader.SizeOfImage;
    return r;
}

// The loader's own answer, used only to cross-check the range test. The
// production resolver does NOT call this (it takes the loader lock).
HMODULE ModuleFromAddress(std::uint64_t addr)
{
    HMODULE mod = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(addr), &mod);
    return mod;
}

std::string ModuleFileName(HMODULE mod)
{
    char buf[MAX_PATH] = {};
    ::GetModuleFileNameA(mod, buf, MAX_PATH);
    return buf;
}
#endif

// Loaded once for the whole binary: LoadLibrary/FreeLibrary churn is not what is
// under test here, and the probe's static initializers must run exactly once.
class ModuleImageOwnership : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
#if defined(_WIN32)
        s_Probe = ::LoadLibraryW(ProbeModulePath().c_str());
        ASSERT_NE(s_Probe, nullptr) << "LoadLibrary(" << ProbeModulePath().string()
                                    << ") failed: " << ::GetLastError();
        s_ProbeRange = RangeOf(s_Probe);
        // By NAME, not by taking the address of an Engine.dll function: a
        // function address formed in THIS image is the import thunk, which lives
        // in the host exe. Reading Engine.dll's base off a thunk silently aliases
        // the engine range onto the host range and makes every arm below
        // meaningless while still looking like it ran.
        s_EngineRange = RangeOf(::GetModuleHandleW(L"Engine.dll"));
        ASSERT_NE(s_EngineRange.Base, 0u) << "Engine.dll is not mapped by that name";
        s_HostRange = RangeOf(::GetModuleHandleW(nullptr));

        s_Mint = reinterpret_cast<MintFn>(
            ::GetProcAddress(s_Probe, "GE_NativeProbe_MintHandlerIdentityAddr"));
        s_Register = reinterpret_cast<RegisterFn>(
            ::GetProcAddress(s_Probe, "GE_NativeProbe_RegisterUiHandler"));
        s_SetOnMouseDown = reinterpret_cast<SetOnMouseDownFn>(
            ::GetProcAddress(s_Probe, "GE_NativeProbe_SetOnMouseDown"));
        s_SetOnTreeSelectionChanged = reinterpret_cast<SetOnTreeSelectionChangedFn>(
            ::GetProcAddress(s_Probe, "GE_NativeProbe_SetOnTreeSelectionChanged"));
        ASSERT_NE(s_Mint, nullptr);
        ASSERT_NE(s_Register, nullptr);
        ASSERT_NE(s_SetOnMouseDown, nullptr);
        ASSERT_NE(s_SetOnTreeSelectionChanged, nullptr);
#endif
    }

    static HMODULE s_Probe;
    static ImageRange s_ProbeRange;
    static ImageRange s_EngineRange;
    static ImageRange s_HostRange;
    static MintFn s_Mint;
    static RegisterFn s_Register;
    static SetOnMouseDownFn s_SetOnMouseDown;
    static SetOnTreeSelectionChangedFn s_SetOnTreeSelectionChanged;
};

HMODULE ModuleImageOwnership::s_Probe = nullptr;
ImageRange ModuleImageOwnership::s_ProbeRange{};
ImageRange ModuleImageOwnership::s_EngineRange{};
ImageRange ModuleImageOwnership::s_HostRange{};
MintFn ModuleImageOwnership::s_Mint = nullptr;
RegisterFn ModuleImageOwnership::s_Register = nullptr;
SetOnMouseDownFn ModuleImageOwnership::s_SetOnMouseDown = nullptr;
SetOnTreeSelectionChangedFn ModuleImageOwnership::s_SetOnTreeSelectionChanged = nullptr;

// Records the numbers the design's Slice 0 verdict rests on. Not an assertion of
// policy — the assertions are the tests below; this one exists so a failure
// anywhere in the file can be read against the actual image layout.
TEST_F(ModuleImageOwnership, ReportsImageLayout)
{
    EXPECT_NE(s_ProbeRange.Base, 0u);
    EXPECT_NE(s_EngineRange.Base, 0u);
    EXPECT_NE(s_HostRange.Base, 0u);
    // Three genuinely distinct images — the precondition for every test below.
    EXPECT_NE(s_ProbeRange.Base, s_EngineRange.Base);
    EXPECT_NE(s_ProbeRange.Base, s_HostRange.Base);
    EXPECT_NE(s_EngineRange.Base, s_HostRange.Base);

    std::printf("[image] probe  base=0x%llx size=0x%llx %s\n",
                (unsigned long long)s_ProbeRange.Base, (unsigned long long)s_ProbeRange.Size,
                ModuleFileName(s_Probe).c_str());
    std::printf("[image] engine base=0x%llx size=0x%llx %s\n",
                (unsigned long long)s_EngineRange.Base, (unsigned long long)s_EngineRange.Size,
                ModuleFileName(::GetModuleHandleW(L"Engine.dll")).c_str());
    std::printf("[image] host   base=0x%llx size=0x%llx %s\n",
                (unsigned long long)s_HostRange.Base, (unsigned long long)s_HostRange.Size,
                ModuleFileName(::GetModuleHandleW(nullptr)).c_str());
}

// SLICE 0, arm A — the erasure's identity object is emitted per image.
// A std::function built from a lambda compiled into the probe DLL identifies
// itself with an address inside the probe DLL's mapped range.
TEST_F(ModuleImageOwnership, ProbeMintedCallableIdentifiesTheProbeImage)
{
    const std::uint64_t addr = s_Mint();
    ASSERT_NE(addr, 0u);
    std::printf("[slice0] probe-minted callable identity addr=0x%llx (probe base + 0x%llx)\n",
                (unsigned long long)addr, (unsigned long long)(addr - s_ProbeRange.Base));

    EXPECT_TRUE(s_ProbeRange.Contains(addr))
        << "callable minted in the probe DLL did not resolve to the probe image";
    EXPECT_FALSE(s_EngineRange.Contains(addr));
    EXPECT_FALSE(s_HostRange.Contains(addr));

    // Cross-check the range test against the loader's own answer.
    EXPECT_EQ(ModuleFromAddress(addr), s_Probe);
}

// SLICE 0, arm B — the PRODUCTION call shape, not a stand-in.
// The probe registers on a host-owned element through the ordinary public
// RegisterEventHandler (which it compiles itself, being header-inline). The
// entry the engine now holds identifies the probe image.
TEST_F(ModuleImageOwnership, HandlerRegisteredFromTheProbeIdentifiesTheProbeImage)
{
    UIElement el;
    int fired = 0;
    const std::uint64_t key = s_Register(&el, kEventMouseUp, &fired);
    ASSERT_NE(key, 0u);

    const std::uint64_t addr = UIEventHandlerAccess::CallableIdentityAddr(el, kEventMouseUp, key);
    ASSERT_NE(addr, 0u) << "the entry the probe registered holds no callable";
    std::printf("[slice0] probe-registered handler identity addr=0x%llx (probe base + 0x%llx)\n",
                (unsigned long long)addr, (unsigned long long)(addr - s_ProbeRange.Base));

    EXPECT_TRUE(s_ProbeRange.Contains(addr));
    EXPECT_FALSE(s_EngineRange.Contains(addr));
    EXPECT_FALSE(s_HostRange.Contains(addr));

    // The handler is live and belongs to the probe, which is still mapped.
    UIEvent e{};
    e.Id = kEventMouseUp;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
    EXPECT_EQ(fired, 1);
}

// SLICE 0, arm C — the arm that pins the constraint.
// An ENGINE-owned handler must never be attributable to a hot-swappable image.
// AxisHeaderBar's constructor registers two handlers on itself from
// AxisHeaderBar.cpp, which is compiled into Engine.dll.
TEST_F(ModuleImageOwnership, EngineRegisteredHandlerDoesNotIdentifyAnyUserImage)
{
    AxisHeaderBar bar;
    const std::uint64_t addr = UIEventHandlerAccess::FirstCallableIdentityAddr(bar, kEventMouseMove);
    ASSERT_NE(addr, 0u) << "AxisHeaderBar's constructor no longer registers a MouseMove handler; "
                           "pick another engine-owned registration for this arm";
    std::printf("[slice0] engine-registered handler identity addr=0x%llx (engine base + 0x%llx)\n",
                (unsigned long long)addr, (unsigned long long)(addr - s_EngineRange.Base));

    EXPECT_TRUE(s_EngineRange.Contains(addr))
        << "an engine-registered handler did not resolve to Engine.dll";
    EXPECT_FALSE(s_ProbeRange.Contains(addr))
        << "an engine-registered handler resolved INTO the hot-swappable image — the "
           "discrimination is not structural and the design is void";
    EXPECT_EQ(ModuleFromAddress(addr), reinterpret_cast<HMODULE>(s_EngineRange.Base));
}

// SLICE 0, arm D — a third image behaves the same way, so arm A is per-image
// emission and not a probe-DLL peculiarity.
TEST_F(ModuleImageOwnership, HostMintedCallableIdentifiesTheHostImage)
{
    int captured = 7;
    UIElement::EventHandler fn = [captured](UIEvent&) mutable { ++captured; };
    const auto addr = reinterpret_cast<std::uint64_t>(&fn.target_type());

    EXPECT_TRUE(s_HostRange.Contains(addr));
    EXPECT_FALSE(s_ProbeRange.Contains(addr));
    EXPECT_FALSE(s_EngineRange.Contains(addr));
}

// SLICE 0, arm E — an empty std::function has no owner. typeid(void) resolves
// outside every user image, so "unowned" is the natural answer rather than a
// special case the resolver has to remember.
TEST_F(ModuleImageOwnership, EmptyCallableIdentifiesNoUserImage)
{
    UIElement::EventHandler fn;
    EXPECT_FALSE(static_cast<bool>(fn));
    const auto addr = reinterpret_cast<std::uint64_t>(&fn.target_type());
    EXPECT_FALSE(s_ProbeRange.Contains(addr));
}

// ---------------------------------------------------------------------------
// Revocation, against a genuinely separate image.
//
// Everything above establishes that the ownership question can be answered.
// These run the answer through the production seam: publish the probe's range as
// a hot-swappable image, register from the probe, revoke, and — in the last test
// — actually unmap and dispatch, which is the defect itself.
// ---------------------------------------------------------------------------

// Publishes/retracts the probe image around a test body.
class ProbeAsHotSwappableImage
{
  public:
    ProbeAsHotSwappableImage(std::uint64_t base, std::uint64_t size)
        : m_Base(base), m_Size(size)
    {
        UI::OpenImageAttribution();
        UI::CloseImageAttribution(base, size);
    }
    ~ProbeAsHotSwappableImage()
    {
        UI::RevokeHandlersOwnedByImage(m_Base, m_Size);
        UI::RetractHotSwappableImage(m_Base);
    }
    ProbeAsHotSwappableImage(const ProbeAsHotSwappableImage&) = delete;
    ProbeAsHotSwappableImage& operator=(const ProbeAsHotSwappableImage&) = delete;

  private:
    std::uint64_t m_Base;
    std::uint64_t m_Size;
};

void Send(UIElement& el, EventId id)
{
    UIEvent e{};
    e.Id = id;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

TEST_F(ModuleImageOwnership, RevocationReleasesAProbeOwnedHandler)
{
    ProbeAsHotSwappableImage mapped(s_ProbeRange.Base, s_ProbeRange.Size);

    UIElement el;
    int fired = 0;
    const std::uint64_t key = s_Register(&el, kEventMouseUp, &fired);
    ASSERT_NE(key, 0u);
    ASSERT_EQ(UI::CountHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 1u)
        << "a handler registered from the probe DLL was not attributed to it";

    Send(el, kEventMouseUp);
    ASSERT_EQ(fired, 1);

    EXPECT_EQ(UI::RevokeHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 1u);
    EXPECT_EQ(UI::CountHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 0u);

    Send(el, kEventMouseUp);
    EXPECT_EQ(fired, 1) << "a revoked handler still fired";
}

TEST_F(ModuleImageOwnership, EngineHandlersAreInvisibleToProbeRevocation)
{
    ProbeAsHotSwappableImage mapped(s_ProbeRange.Base, s_ProbeRange.Size);

    AxisHeaderBar bar; // two Engine.dll-minted handlers
    UIElement userElement;
    int fired = 0;
    ASSERT_NE(s_Register(&userElement, kEventMouseUp, &fired), 0u);

    // Only the probe's handler is even visible to the pass.
    EXPECT_EQ(UI::CountHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 1u);
    EXPECT_EQ(UI::RevokeHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 1u);

    // The engine control is untouched and still dispatches.
    int before = 0;
    (void)before;
    Send(bar, kEventMouseMove);
    EXPECT_GT(UIEventHandlerAccess::FirstCallableIdentityAddr(bar, kEventMouseMove), 0u)
        << "revocation released an Engine.dll-owned handler";
}

// The defect end to end: a handler whose code is inside an image that is then
// unmapped. Without revocation the dispatch below faults while COPYING the
// handler vector. A separate on-disk copy is loaded so this test owns an image it
// can really unmap (LoadLibrary on the already-loaded probe path would just bump
// a refcount).
// The member-slot half of the mechanism, against the real probe DLL: a callable
// stored in Button::m_OnMouseDown rather than in the handler table must attribute
// to the probe and be released by the same pre-unmap revocation.
TEST_F(ModuleImageOwnership, MemberSlotFromTheProbeIsClearedByRevoke)
{
    ProbeAsHotSwappableImage mapped(s_ProbeRange.Base, s_ProbeRange.Size);

    Button btn;
    int fired = 0;
    s_SetOnMouseDown(&btn, &fired);
    ASSERT_EQ(UI::CountHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 1u)
        << "SetOnMouseDown from the probe DLL was not attributed to it";

    Send(btn, kEventMouseDown);
    ASSERT_EQ(fired, 1);

    EXPECT_EQ(UI::RevokeHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 1u);
    EXPECT_EQ(UI::CountHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 0u);

    Send(btn, kEventMouseDown);
    EXPECT_EQ(fired, 1) << "a revoked SetOnMouseDown still fired";
}

// TreeView's slots across the same real image boundary. Button's arm above proves the
// mechanism for a control with two slots declared in its own header; TreeView is where
// the bulk of the stamped surface lives, and its slots are reached through a different
// control's virtuals, so "Button works" does not imply "TreeView works".
TEST_F(ModuleImageOwnership, TreeViewMemberSlotFromTheProbeIsClearedByRevoke)
{
    ProbeAsHotSwappableImage mapped(s_ProbeRange.Base, s_ProbeRange.Size);

    TreeView tv;
    int fired = 0;
    s_SetOnTreeSelectionChanged(&tv, &fired);
    ASSERT_EQ(UI::CountHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 1u)
        << "SetOnSelectionChanged from the probe DLL was not attributed to it";

    EXPECT_EQ(UI::RevokeHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 1u);
    EXPECT_EQ(UI::CountHandlersOwnedByImage(s_ProbeRange.Base, s_ProbeRange.Size), 0u)
        << "a TreeView slot survived revocation and would dangle past the unmap";
}

// The member-slot equivalent of DispatchIsSafeAfterTheOwningImageIsUnmapped, with
// a real FreeLibrary: the slot is released while the image is still mapped, so
// neither the later event nor the Button's destruction reaches into dead code.
TEST_F(ModuleImageOwnership, MemberSlotIsSafeAfterTheOwningImageIsUnmapped)
{
    const std::filesystem::path copy =
        std::filesystem::temp_directory_path() /
        ("GeProbeSlotUnmap_" + std::to_string(::GetCurrentProcessId()) + ".dll");
    std::error_code ec;
    std::filesystem::copy_file(ProbeModulePath(), copy,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "copying the probe failed: " << ec.message();

    HMODULE second = ::LoadLibraryW(copy.c_str());
    ASSERT_NE(second, nullptr) << "LoadLibrary(copy) failed: " << ::GetLastError();
    ASSERT_NE(second, s_Probe) << "the copy resolved to the same image; nothing to unmap";
    const ImageRange secondRange = RangeOf(second);
    ASSERT_NE(secondRange.Base, 0u);

    auto bind =
        reinterpret_cast<SetOnMouseDownFn>(::GetProcAddress(second, "GE_NativeProbe_SetOnMouseDown"));
    ASSERT_NE(bind, nullptr);

    auto btn = std::make_unique<Button>();
    int fired = 0;
    {
        UI::OpenImageAttribution();
        UI::CloseImageAttribution(secondRange.Base, secondRange.Size);

        bind(btn.get(), &fired);
        ASSERT_EQ(UI::CountHandlersOwnedByImage(secondRange.Base, secondRange.Size), 1u);
        Send(*btn, kEventMouseDown);
        ASSERT_EQ(fired, 1) << "the slot did not run while its image was mapped";

        EXPECT_EQ(UI::RevokeHandlersOwnedByImage(secondRange.Base, secondRange.Size), 1u);
        EXPECT_EQ(UI::CountHandlersOwnedByImage(secondRange.Base, secondRange.Size), 0u)
            << "the quiesce ledger would refuse this unmap";
    }

    ASSERT_TRUE(::FreeLibrary(second));
    UI::RetractHotSwappableImage(secondRange.Base);
    EXPECT_EQ(ModuleFromAddress(secondRange.Base + 0x1000), nullptr);

    Send(*btn, kEventMouseDown);
    EXPECT_EQ(fired, 1);

    btn.reset();

    std::filesystem::remove(copy, ec);
}

TEST_F(ModuleImageOwnership, DispatchIsSafeAfterTheOwningImageIsUnmapped)
{
    const std::filesystem::path copy =
        std::filesystem::temp_directory_path() /
        ("GeProbeUnmap_" + std::to_string(::GetCurrentProcessId()) + ".dll");
    std::error_code ec;
    std::filesystem::copy_file(ProbeModulePath(), copy,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "copying the probe failed: " << ec.message();

    HMODULE second = ::LoadLibraryW(copy.c_str());
    ASSERT_NE(second, nullptr) << "LoadLibrary(copy) failed: " << ::GetLastError();
    ASSERT_NE(second, s_Probe) << "the copy resolved to the same image; nothing to unmap";
    const ImageRange secondRange = RangeOf(second);
    ASSERT_NE(secondRange.Base, 0u);

    auto reg = reinterpret_cast<RegisterFn>(::GetProcAddress(second, "GE_NativeProbe_RegisterUiHandler"));
    ASSERT_NE(reg, nullptr);

    auto el = std::make_unique<UIElement>();
    int fired = 0;
    {
        UI::OpenImageAttribution();
        UI::CloseImageAttribution(secondRange.Base, secondRange.Size);

        ASSERT_NE(reg(el.get(), kEventMouseUp, &fired), 0u);
        ASSERT_EQ(UI::CountHandlersOwnedByImage(secondRange.Base, secondRange.Size), 1u);
        Send(*el, kEventMouseUp);
        ASSERT_EQ(fired, 1) << "the handler did not run while its image was mapped";

        // The pre-unmap hook, in the order the loader uses it.
        EXPECT_EQ(UI::RevokeHandlersOwnedByImage(secondRange.Base, secondRange.Size), 1u);
        EXPECT_EQ(UI::CountHandlersOwnedByImage(secondRange.Base, secondRange.Size), 0u)
            << "the quiesce ledger would refuse this unmap";
    }

    ASSERT_TRUE(::FreeLibrary(second));
    UI::RetractHotSwappableImage(secondRange.Base);
    // The image really is gone.
    EXPECT_EQ(ModuleFromAddress(secondRange.Base + 0x1000), nullptr);

    // THIS is the line that faults at main: DispatchEvent copies every
    // HandlerEntry, and hence every std::function, before it consults `active`.
    Send(*el, kEventMouseUp);
    EXPECT_EQ(fired, 1);

    // Destroying the element must not reach into the dead image either. Done
    // explicitly rather than at scope exit so a fault here is attributed to this
    // line instead of to the test's teardown.
    el.reset();

    std::filesystem::remove(copy, ec);
}

// The residual risk in the mechanism, probed rather than argued.
//
// A lambda's closure type is unique to the translation unit that wrote it, so its
// type descriptor can only be the probe's. A plain FUNCTION POINTER names a type
// Engine.dll could name too — the one shape where a single shared descriptor is
// conceivable. It would fail in the unsafe direction (a user callable attributed
// to Engine.dll is never revoked, i.e. an AV), so it gets an assertion, not a
// paragraph.
TEST_F(ModuleImageOwnership, AFunctionPointerHandlerStillIdentifiesTheProbeImage)
{
    auto reg = reinterpret_cast<std::uint64_t (*)(void*, std::uint64_t)>(
        ::GetProcAddress(s_Probe, "GE_NativeProbe_RegisterFunctionPointerHandler"));
    ASSERT_NE(reg, nullptr);

    UIElement el;
    const std::uint64_t key = reg(&el, kEventMouseUp);
    ASSERT_NE(key, 0u);

    const std::uint64_t addr = UIEventHandlerAccess::CallableIdentityAddr(el, kEventMouseUp, key);
    ASSERT_NE(addr, 0u);
    std::printf("[slice0] probe function-pointer handler identity addr=0x%llx (probe base + 0x%llx)\n",
                (unsigned long long)addr, (unsigned long long)(addr - s_ProbeRange.Base));

    EXPECT_TRUE(s_ProbeRange.Contains(addr))
        << "a non-lambda callable built in the probe DLL was attributed elsewhere — such a "
           "handler would survive its module's unload un-revoked";
    EXPECT_FALSE(s_EngineRange.Contains(addr));
}

// The load-abort seam, driven through the real manager.
//
// A DLL refused by the load handshake has already run its static initializers and
// is unmapped on the way out (the handle is a local). If anything it handed to an
// engine-owned object survives that, the next dispatch faults exactly as after a
// superseded-image unmap — so the abort path needs the same revocation the unload
// path gets. The probe has no GE_UserModule_AbiVersion_v1 export, which is
// precisely how a refused module behaves.
TEST_F(ModuleImageOwnership, LoadAbortRevokesHandlersFromTheRefusedImage)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("GeAbortRevoke_" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / ".Cache" / "NativeScripts" / "build", ec);
    const auto dll = root / "RefusedModule.dll";
    std::filesystem::copy_file(ProbeModulePath(), dll,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(GameEngine::NativeScripting::WriteBuildCacheRecord(
        root / ".Cache" / "NativeScripts" / "build",
        // Stamped with THIS engine's identity so the dev-cache gate passes it through:
        // the subject here is the load-abort seam, which only runs once the image is
        // mapped and the ABI-export handshake then refuses it.
        GameEngine::NativeScripting::BuildCacheRecord{
            "d", dll.generic_string(), "", GameEngine::NativeScripting::EngineBuildIdentity()}));

    UIElement el;
    int fired = 0;
    std::uint64_t mappedBase = 0, mappedSize = 0;
    int mapBegins = 0, mapEnds = 0, unmappings = 0, unmapped = 0;
    std::size_t revokedAtUnmap = 0;
    std::string pinsAfterRevoke = "unset";

    GameEngine::NativeScripting::NativeScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(nullptr));

    GameEngine::NativeScripting::NativeScriptManager::ModuleImageObserver observer;
    observer.ImageMapBegin = [&] { ++mapBegins; UI::OpenImageAttribution(); };
    observer.ImageMapEnd = [&](std::uint64_t base, std::uint64_t size) {
        ++mapEnds;
        mappedBase = base;
        mappedSize = size;
        UI::CloseImageAttribution(base, size);
        // Stand in for what the refused module's own code would have done: hand a
        // callable built INSIDE the freshly mapped image to an engine-owned element.
        if (!base)
            return;
        auto reg = reinterpret_cast<RegisterFn>(
            ::GetProcAddress(reinterpret_cast<HMODULE>(base), "GE_NativeProbe_RegisterUiHandler"));
        ASSERT_NE(reg, nullptr);
        ASSERT_NE(reg(&el, kEventMouseUp, &fired), 0u);
    };
    observer.DescribeImagePins = [&](std::uint64_t base, std::uint64_t size) -> std::string {
        const std::size_t n = UI::CountHandlersOwnedByImage(base, size);
        return n == 0 ? std::string{} : std::to_string(n) + " UI event handler(s)";
    };
    observer.ImageUnmapping = [&](std::uint64_t base, std::uint64_t size) {
        ++unmappings;
        revokedAtUnmap = UI::RevokeHandlersOwnedByImage(base, size);
        pinsAfterRevoke = observer.DescribeImagePins(base, size);
    };
    observer.ImageUnmapped = [&](std::uint64_t base) {
        ++unmapped;
        UI::RetractHotSwappableImage(base);
    };
    mgr.SetModuleImageObserver(observer);

    // Refused: no ABI export. The return value is "no prebuilt module loaded".
    EXPECT_FALSE(mgr.LoadPrebuiltUserModule(root, "RefusedModule"));

    EXPECT_EQ(mapBegins, 1) << "the map window was never opened";
    EXPECT_EQ(mapEnds, 1) << "the map window was never closed";
    EXPECT_NE(mappedBase, 0u) << "the refused image reported no base";
    EXPECT_NE(mappedSize, 0u) << "the refused image reported no span";
    EXPECT_EQ(unmappings, 1) << "the abort path did not run the pre-unmap hook";
    EXPECT_EQ(unmapped, 1) << "the abort path did not retract the image range";
    EXPECT_EQ(revokedAtUnmap, 1u)
        << "the refused image was unmapped with a live handler still pointing into it";
    EXPECT_EQ(pinsAfterRevoke, std::string{})
        << "revocation left a pin the ledger would report";

    // The image is gone; this is the dispatch that would fault.
    Send(el, kEventMouseUp);
    EXPECT_EQ(fired, 0);

    mgr.Shutdown();
    std::filesystem::remove_all(root, ec);
}

} // namespace
