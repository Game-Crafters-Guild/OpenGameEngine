// Cross-DLL registration smoke test — the gtest host.
//
// Proves the Phase-2 prerequisite: the engine's component reflection registries
// (ComponentFieldRegistry, ComponentFactory) are a SINGLE process-wide instance
// shared across the host EXE, Engine.dll, and a separately-linked user DLL.
//
// Topology under test:
//   host EXE   ── links PRIVATE Engine (import lib) ──┐
//                                                      ├──> the ONE registry
//   probe DLL  ── links PRIVATE Engine (import lib) ──┘    living in Engine.dll
//
// The registry storage is a function-local static inside ComponentFieldRegistry.cpp
// / ComponentFactory.cpp, compiled once into the ECS OBJECT lib and spliced into
// Engine.dll. Because both the host and the probe link the Engine *import lib*
// (not the ECS object code directly), every Register / FindByName / Has call in
// either binary dispatches to the same exported Engine.dll function and therefore
// the same static. If that sharing held only by accident — e.g. if a consumer
// linked the ECS module directly and cloned the statics — the host would query an
// empty local copy and these assertions would fail.
//
// The flow (run once in SetUpTestSuite so the "before registration" snapshot is
// taken before anything touches the explicit probe):
//   1. LoadLibrary the probe DLL (resolved next to THIS exe, not the cwd).
//   2. Ask the DLL for the id it computes for CrossDllProbeExplicit (no register).
//   3. Snapshot the host's view BEFORE the DLL registers anything.
//   4. Call the DLL's explicit-register export.
//   5. The TESTs assert the host now sees what the DLL registered, by id.

#include "Core/Application.h" // PathUtils::GetExecutableDirectory
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentFactory.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace
{

namespace ge = GameEngine::ECS;

using U64Fn = std::uint64_t (*)();
using U32Fn = std::uint32_t (*)();
using IntFn = int (*)();

// The probe DLL is staged next to this test exe; resolve it from the exe dir, never
// the cwd (ctest sets WORKING_DIRECTORY to the build root, not the exe dir).
std::filesystem::path ProbeModulePath()
{
    const std::filesystem::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
#if defined(_WIN32)
    return exeDir / "NativeScriptingProbe.dll";
#elif defined(__APPLE__)
    return exeDir / "libNativeScriptingProbe.dylib";
#else
    return exeDir / "libNativeScriptingProbe.so";
#endif
}

#if defined(_WIN32)
using ModuleHandle = HMODULE;
ModuleHandle LoadModule(const std::filesystem::path& p) { return ::LoadLibraryW(p.c_str()); }
void* FindSymbol(ModuleHandle h, const char* name) { return reinterpret_cast<void*>(::GetProcAddress(h, name)); }
#else
using ModuleHandle = void*;
ModuleHandle LoadModule(const std::filesystem::path& p) { return ::dlopen(p.c_str(), RTLD_NOW | RTLD_GLOBAL); }
void* FindSymbol(ModuleHandle h, const char* name) { return ::dlsym(h, name); }
#endif

} // namespace

// One-time load + snapshot shared by every assertion below.
class CrossDllRegistration : public ::testing::Test
{
protected:
    static ModuleHandle s_module;

    // What the DLL reports about itself.
    static std::uint64_t s_expectedExplicitId;   // id of CrossDllProbeExplicit (pre-register)
    static std::uint64_t s_returnedExplicitId;   // id returned by the register export
    static std::uint64_t s_staticId;             // id of CrossDllProbeStatic
    static std::uint32_t s_probeFieldCount;      // field count the DLL sees
    static int           s_selfSeesExplicit;     // DLL's own post-register Has() result

    // The host's view of the SHARED registries, captured BEFORE the DLL registers
    // the explicit probe.
    static bool   s_fieldHadExplicitBefore;
    static bool   s_factoryHadExplicitBefore;
    static bool   s_fieldHasStaticOnLoad;        // did the DLL-load static initializer run?
    static std::size_t s_engineFactoryCountBefore;

    static bool   s_loaded;
    static std::string s_loadError;

    static void SetUpTestSuite()
    {
        const std::filesystem::path probe = ProbeModulePath();
        s_module = LoadModule(probe);
        if (!s_module)
        {
            s_loadError = "LoadLibrary failed for " + probe.string();
            return;
        }

        auto expectedIdFn  = reinterpret_cast<U64Fn>(FindSymbol(s_module, "GE_NativeProbe_ExpectedExplicitId"));
        auto staticIdFn    = reinterpret_cast<U64Fn>(FindSymbol(s_module, "GE_NativeProbe_StaticId"));
        auto registerFn    = reinterpret_cast<U64Fn>(FindSymbol(s_module, "GE_NativeProbe_RegisterExplicit"));
        auto fieldCountFn  = reinterpret_cast<U32Fn>(FindSymbol(s_module, "GE_NativeProbe_ExplicitFieldCount"));
        auto selfSeesFn    = reinterpret_cast<IntFn>(FindSymbol(s_module, "GE_NativeProbe_SelfSeesExplicit"));

        if (!expectedIdFn || !staticIdFn || !registerFn || !fieldCountFn || !selfSeesFn)
        {
            s_loadError = "one or more probe exports failed to resolve";
            return;
        }

        s_expectedExplicitId = expectedIdFn();
        s_staticId           = staticIdFn();
        s_probeFieldCount    = fieldCountFn();

        // Host's view of the SHARED registry, before the DLL registers the explicit
        // probe. The explicit probe must be absent; the static-init probe's presence
        // tells us whether the DLL-load initializer ran.
        s_fieldHadExplicitBefore   = ge::ComponentFieldRegistry::Has(s_expectedExplicitId);
        s_factoryHadExplicitBefore = ge::ComponentFactory::Has(s_expectedExplicitId);
        s_fieldHasStaticOnLoad     = ge::ComponentFieldRegistry::Has(s_staticId);
        s_engineFactoryCountBefore = ge::ComponentFactory::RegisteredTypes().size();

        // Trigger the explicit, cross-DLL registration.
        s_returnedExplicitId = registerFn();
        s_selfSeesExplicit   = selfSeesFn();

        s_loaded = true;
    }

    // Intentionally NO TearDownTestSuite that unloads the probe.
    //
    // The probe registered a std::function creator into ComponentFactory and
    // reflected-name string_views into ComponentFieldRegistry — both pointing into
    // the probe's image. Calling FreeLibrary here unmaps that image while
    // Engine.dll's process-lifetime registries still hold those references; the
    // registries' static destructors then dereference the unmapped code at process
    // exit and SIGSEGV (observed: exit 139 AFTER all tests pass).
    //
    // Invalidating those engine-side references before unload is exactly the
    // symmetric unregister / OnBeforeUnload protocol that Phase 2 (commit C12)
    // builds; it does not exist yet. This test's scope is registration *sharing*,
    // not safe *unload*, so it leaves the probe mapped for the process lifetime.
    // The crash-on-unload is recorded as the empirical motivation for the unload
    // protocol — see the cross-dll-registration finding doc.
};

ModuleHandle  CrossDllRegistration::s_module = {};
std::uint64_t CrossDllRegistration::s_expectedExplicitId = 0;
std::uint64_t CrossDllRegistration::s_returnedExplicitId = 0;
std::uint64_t CrossDllRegistration::s_staticId = 0;
std::uint32_t CrossDllRegistration::s_probeFieldCount = 0;
int           CrossDllRegistration::s_selfSeesExplicit = -1;
bool          CrossDllRegistration::s_fieldHadExplicitBefore = false;
bool          CrossDllRegistration::s_factoryHadExplicitBefore = false;
bool          CrossDllRegistration::s_fieldHasStaticOnLoad = false;
std::size_t   CrossDllRegistration::s_engineFactoryCountBefore = 0;
bool          CrossDllRegistration::s_loaded = false;
std::string   CrossDllRegistration::s_loadError;

// Guard: every other assertion is meaningless if the DLL didn't load.
TEST_F(CrossDllRegistration, ProbeDllLoadsAndResolves)
{
    ASSERT_TRUE(s_loaded) << s_loadError;
    EXPECT_NE(s_module, ModuleHandle{});
}

// The host shares Engine.dll's *populated* registry, not an empty local clone:
// every engine component registered its factory into Engine.dll at Engine.dll
// load, and the host — linking only the import lib — sees them. A cloned,
// host-local ComponentFactory would be empty here.
TEST_F(CrossDllRegistration, HostSharesEnginePopulatedFactory)
{
    ASSERT_TRUE(s_loaded) << s_loadError;
    EXPECT_GT(s_engineFactoryCountBefore, 0u)
        << "host's ComponentFactory view is empty — it is NOT sharing Engine.dll's instance";
}

// Precondition: the explicit probe was unknown until the DLL's export ran. This
// makes the post-registration visibility attributable to the cross-DLL Register
// call rather than to some incidental prior registration.
TEST_F(CrossDllRegistration, ExplicitProbeAbsentBeforeRegister)
{
    ASSERT_TRUE(s_loaded) << s_loadError;
    EXPECT_NE(s_expectedExplicitId, 0u);
    EXPECT_FALSE(s_fieldHadExplicitBefore);
    EXPECT_FALSE(s_factoryHadExplicitBefore);
}

// THE CORE PROOF. The probe DLL called ComponentFieldRegistry::Register; the host
// — a different binary — now finds that entry by id and by name. That can only
// happen if both binaries touch the same registry instance inside Engine.dll.
TEST_F(CrossDllRegistration, FieldRegistryIsSharedAcrossDll)
{
    ASSERT_TRUE(s_loaded) << s_loadError;

    // Identity is consteval-stable across the DLL boundary: the id the DLL returned
    // from Register equals the id it computed independently, and equals what the
    // host resolves by name.
    EXPECT_EQ(s_returnedExplicitId, s_expectedExplicitId);

    EXPECT_TRUE(ge::ComponentFieldRegistry::Has(s_expectedExplicitId))
        << "host cannot see a component the probe DLL registered — registries are cloned, not shared";

    const auto fields = ge::ComponentFieldRegistry::Get(s_expectedExplicitId);
    EXPECT_EQ(fields.size(), s_probeFieldCount);
    EXPECT_EQ(fields.size(), 2u);  // CrossDllProbeExplicit has Health + Mana

    const ge::ComponentTypeId byName =
        ge::ComponentFieldRegistry::FindByName("CrossDllProbeExplicit");
    EXPECT_EQ(byName, s_expectedExplicitId);

    const std::string_view canon =
        ge::ComponentFieldRegistry::GetCanonicalName(s_expectedExplicitId);
    EXPECT_TRUE(canon.ends_with("CrossDllProbeExplicit")) << "canonical name: " << canon;
}

// Same proof for the second registry: the factory creator the DLL registered is
// visible to the host by id.
TEST_F(CrossDllRegistration, FactoryRegistryIsSharedAcrossDll)
{
    ASSERT_TRUE(s_loaded) << s_loadError;
    EXPECT_TRUE(ge::ComponentFactory::Has(s_expectedExplicitId))
        << "host cannot see a factory the probe DLL registered — ComponentFactory is cloned, not shared";
    EXPECT_GT(s_engineFactoryCountBefore, 0u);
}

// Sanity: the DLL's own view of the registry coincides with the host's. After it
// registered, it can read its own entry back through the shared registry.
TEST_F(CrossDllRegistration, ProbeDllSeesItsOwnRegistration)
{
    ASSERT_TRUE(s_loaded) << s_loadError;
    EXPECT_EQ(s_selfSeesExplicit, 1);
}

// FINDING — does an anonymous-namespace static initializer survive into the DLL
// image and run on LoadLibrary?
//
// CrossDllProbeStatic registers only via GE_REGISTER_COMPONENT's static-init path,
// with no other reference to it in the DLL. If the host sees it immediately after
// LoadLibrary (before any explicit export ran), the initializer survived and fired.
//
// In a single-TU DLL built Debug this is expected to hold. The hazard the Phase-2
// `ge_reg` linker-section model addresses is the harder case: a registrar that
// lives in a static/object library aggregated into the DLL, where the linker may
// drop the whole object file (and its CRT init entry) when nothing references it.
// A failure here would be a strong, concrete argument for that data-section model.
TEST_F(CrossDllRegistration, StaticInitRegistrationSurvivesDllLoad)
{
    ASSERT_TRUE(s_loaded) << s_loadError;
    RecordProperty("staticId", std::to_string(s_staticId));
    RecordProperty("staticInitVisibleOnLoad", s_fieldHasStaticOnLoad ? "true" : "false");
    GTEST_LOG_(INFO) << "Static-init probe visible immediately after LoadLibrary: "
                     << (s_fieldHasStaticOnLoad ? "YES" : "NO");
    EXPECT_NE(s_staticId, 0u);
    EXPECT_TRUE(s_fieldHasStaticOnLoad)
        << "the DLL-load static initializer did NOT run/survive — the linker likely "
           "stripped the unreferenced registrar. This is the case Phase 2's ge_reg "
           "data-section registration model is designed to make robust.";
}
