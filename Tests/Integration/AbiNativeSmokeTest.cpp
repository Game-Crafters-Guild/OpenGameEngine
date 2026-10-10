#include "Scripting/ScriptingABI.h"
#include "Scripting/ModelABI.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Logger/Logger.h"
#include "Core/Engine.h"
#include "Engine/GameUI/GameplayUI.h"
#include "Scripting/ScriptManager.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

using namespace GameEngine;

TEST(ScriptingAbi, CanLoadInterfaceAndCallBasics)
{
    const void* table = nullptr;
    uint32_t size = 0;
    ASSERT_EQ(GE_GetInterface(GE_ABI_VERSION_CURRENT, &table, &size), GE_Result_Ok);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(size, sizeof(GE_Interface_v1));

    auto* iface = reinterpret_cast<const GE_Interface_v1*>(table);
    ASSERT_EQ(iface->abiVersion, GE_ABI_VERSION_CURRENT);

    // Log
    ASSERT_NE(iface->Log, nullptr);
    EXPECT_EQ(iface->Log(GE_Log_Info, "ABI smoke: log", 14), GE_Result_Ok);

    // ECS getters require initialized engine
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";

        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);

        ASSERT_TRUE(engine.Initialize(cfg));
    }

    // World handle and entity count
    GE_Handle world = 0;
    ASSERT_NE(iface->ECS_GetWorldHandle, nullptr);
    ASSERT_EQ(iface->ECS_GetWorldHandle(&world), GE_Result_Ok);
    ASSERT_NE(world, 0ull);

    int32_t count = -1;
    ASSERT_NE(iface->ECS_GetEntityCount, nullptr);
    ASSERT_EQ(iface->ECS_GetEntityCount(world, &count), GE_Result_Ok);
    EXPECT_GE(count, 0);
}

// Negative-path guards to ensure ECS helpers fail predictably and do not
// corrupt out parameters on error.

TEST(ScriptingAbi, EcsGetWorldHandle_NullOutParam_ReturnsInvalidArg)
{
	// Passing a null outWorld pointer should be rejected without crashing.
	EXPECT_EQ(GE_ECS_GetWorldHandle(nullptr), GE_Result_InvalidArg);
}

TEST(ScriptingAbi, EcsGetEntityCount_InvalidArgs_ReturnInvalidArg)
{
	int32_t count = 123;
	// world == 0 should be treated as invalid and must not overwrite the out param.
	EXPECT_EQ(GE_ECS_GetEntityCount(0, &count), GE_Result_InvalidArg);
	EXPECT_EQ(count, 123);

	// Null outCount should also be rejected.
	EXPECT_EQ(GE_ECS_GetEntityCount(0, nullptr), GE_Result_InvalidArg);
}

static void GE_CDECL AbiTestNoopEventCb(const GE_UIEventData*, void*) {}

// Game-UI ABI boundary: the document-layer GE_GameUI_FindElement and the element-layer
// GE_UIElement_* exports link, and the safety contract no-ops when no GameUIHost is
// registered — an unresolvable handle must report not-found, never dereference. (The
// positive find + register + drive path is covered at the host level in UITests
// GameUIHostTests, which has the headless device harness; this pins the C ABI surface and
// the null-host safety the managed side relies on.)
TEST(ScriptingAbi, GameUiExportsAreSafeWithNoHost)
{
	auto& engine = EngineCore::GetInstance();
	if (!engine.IsInitialized()) {
		ApplicationConfig cfg; cfg.AssetDirectory = "Assets";
		ScriptsConfig scriptsConfig{};
		scriptsConfig.enableHotReload = false;
		engine.SetScriptsConfig(scriptsConfig);
		ASSERT_TRUE(engine.Initialize(cfg));
	}
	// Guarantee no active gameplay host for this test (#1051's accessor owns the slot).
	GameUI::SetHost(nullptr);

	// Document layer: with no host nothing resolves, and the out-param is zeroed rather than
	// left holding a stale id a caller might then use.
	uint64_t instanceId = 12345;
	EXPECT_EQ(GE_GameUI_FindElement(0, "hp-fill", &instanceId), GE_Result_NotFound);
	EXPECT_EQ(instanceId, 0ull) << "outInstanceId must be zeroed even when nothing resolves";
	EXPECT_EQ(GE_GameUI_FindElement(0, "hp-fill", nullptr), GE_Result_InvalidArg);
	// A null or empty id is a caller error, rejected BEFORE the lookup — an empty id
	// would otherwise match the first element that has no id, i.e. most of a tree, and
	// mint a durable handle to it. Rejected either way, host or no host.
	instanceId = 5;
	EXPECT_EQ(GE_GameUI_FindElement(0, nullptr, &instanceId), GE_Result_InvalidArg);
	EXPECT_EQ(instanceId, 0ull) << "outInstanceId must be zeroed even on the reject path";
	instanceId = 5;
	EXPECT_EQ(GE_GameUI_FindElement(0, "", &instanceId), GE_Result_InvalidArg);
	EXPECT_EQ(instanceId, 0ull);

	// Element layer: a null callback is rejected up front (before any host lookup) and the
	// out-param is initialized.
	uint64_t token = 12345;
	EXPECT_EQ(GE_UIElement_RegisterEvent(1, "UI.ButtonClick", nullptr, nullptr, &token), GE_Result_InvalidArg);
	EXPECT_EQ(token, 0ull) << "outToken must be zeroed even on the reject path";

	// An unknown or empty event name is a caller error, rejected BEFORE any host lookup —
	// a name nothing dispatches would otherwise mint a subscription that can never fire.
	token = 7;
	EXPECT_EQ(GE_UIElement_RegisterEvent(1, "UI.NotAnEvent", &AbiTestNoopEventCb, nullptr, &token),
	          GE_Result_InvalidArg);
	EXPECT_EQ(token, 0ull);
	EXPECT_EQ(GE_UIElement_RegisterEvent(1, nullptr, &AbiTestNoopEventCb, nullptr, &token), GE_Result_InvalidArg);
	EXPECT_EQ(GE_UIElement_RegisterEvent(1, "", &AbiTestNoopEventCb, nullptr, &token), GE_Result_InvalidArg);
	EXPECT_EQ(GE_UIElement_UnregisterEvent(1, "UI.NotAnEvent", 1), GE_Result_InvalidArg);

	// With no host, no instance id resolves — including one that was live in an earlier
	// session, since ids are never reused.
	EXPECT_EQ(GE_UIElement_SetWidthPercent(1, 50.0f), GE_Result_NotFound);
	EXPECT_EQ(GE_UIElement_SetLabelText(1, "100 / 100"), GE_Result_NotFound);
	EXPECT_EQ(GE_UIElement_SetClass(1, "low-health", 1), GE_Result_NotFound);

	// 0 is never a live element, on any export.
	EXPECT_EQ(GE_UIElement_SetWidthPercent(0, 50.0f), GE_Result_NotFound);
	EXPECT_EQ(GE_UIElement_SetLabelText(0, nullptr), GE_Result_NotFound);

	// An empty/null class name is a caller error, not a lookup failure — and it is rejected
	// before the lookup, so the result does not depend on whether a host exists.
	EXPECT_EQ(GE_UIElement_SetClass(1, nullptr, 1), GE_Result_InvalidArg);
	EXPECT_EQ(GE_UIElement_SetClass(1, "", 0), GE_Result_InvalidArg);

	// A real callback still resolves to not-found (no host) and is never invoked; token stays 0.
	token = 7;
	EXPECT_EQ(GE_UIElement_RegisterEvent(1, "UI.ButtonClick", &AbiTestNoopEventCb, nullptr, &token),
	          GE_Result_NotFound);
	EXPECT_EQ(token, 0ull) << "outToken stays 0 when nothing was registered";

	// Unregister with no host is a safe no-op (element already gone), not a failure.
	EXPECT_EQ(GE_UIElement_UnregisterEvent(1, "UI.ButtonClick", 123), GE_Result_Ok);
	EXPECT_EQ(GE_UIElement_UnregisterEvent(0, "UI.MouseMove", 0), GE_Result_Ok);

	// Liveness is a QUERY: with nothing resolvable the answer is Ok/false, not an error —
	// a binding must be able to tell "dead" from "the engine is not up".
	int32_t alive = 42;
	EXPECT_EQ(GE_UIElement_IsAlive(1, &alive), GE_Result_Ok);
	EXPECT_EQ(alive, 0) << "an unresolvable id must report not-alive, not alive";
	EXPECT_EQ(GE_UIElement_IsAlive(0, &alive), GE_Result_Ok);
	EXPECT_EQ(alive, 0);
	EXPECT_EQ(GE_UIElement_IsAlive(1, nullptr), GE_Result_InvalidArg);

	// The element-scoped tag id is an operation on an element, so an unresolvable id is
	// not-found — the asymmetry with IsAlive above is deliberate.
	uint64_t tagId = 99;
	EXPECT_EQ(GE_UIElement_GetTagId(1, &tagId), GE_Result_NotFound);
	EXPECT_EQ(tagId, 0ull) << "outTagId must be zeroed even when nothing resolves";
	EXPECT_EQ(GE_UIElement_GetTagId(1, nullptr), GE_Result_InvalidArg);

	// Tag NAME -> id needs no host at all: the factory registry is process-wide. That the
	// answer AGREES with the registry's own type->id cache is asserted in UITests, which
	// links the UI module; here the C surface's contract is what is pinned.
	EXPECT_EQ(GE_UI_GetTagId("button", &tagId), GE_Result_Ok);
	EXPECT_NE(tagId, 0ull) << "built-in controls must be registered by the time the ABI is callable";
	EXPECT_EQ(GE_UI_GetTagId("not-a-registered-tag", &tagId), GE_Result_Ok);
	EXPECT_EQ(tagId, 0ull) << "an unknown tag answers 0, it does not fail";
	EXPECT_EQ(GE_UI_GetTagId(nullptr, &tagId), GE_Result_InvalidArg);
	EXPECT_EQ(GE_UI_GetTagId("", &tagId), GE_Result_InvalidArg);
	EXPECT_EQ(GE_UI_GetTagId("button", nullptr), GE_Result_InvalidArg);

	// The event allowlist, reconciled against the REAL export rather than against a copy of
	// itself. Each name a binding may send must be KNOWN here — with no host the answer is
	// not-found (the element did not resolve), which is exactly what distinguishes it from
	// the InvalidArg an unknown name gets before any lookup. The managed binding and the
	// test double both carry hand-mirrored copies of this list; this is the only place that
	// can catch either of them drifting from the engine.
	for (const char* eventName : {"UI.ButtonClick", "UI.MouseEnter", "UI.MouseMove", "UI.FocusIn",
	                              "UI.ValueChanging", "UI.ValueChanged", "UI.ScrollOffsetChanged",
	                              "UI.AttachedToPanel", "UI.DetachedFromPanel"}) {
		token = 7;
		EXPECT_EQ(GE_UIElement_RegisterEvent(1, eventName, &AbiTestNoopEventCb, nullptr, &token),
		          GE_Result_NotFound)
		    << "event name '" << eventName << "' is not on the ABI's allowlist, so a binding that "
		       "sends it would get InvalidArg — one of the three copies of this list has drifted";
		EXPECT_EQ(token, 0ull);
		EXPECT_EQ(GE_UIElement_UnregisterEvent(1, eventName, 1), GE_Result_Ok);
	}

	// The typed value accessors: same safety contract as every element export — a handle that
	// does not resolve is not-found with the out-param zeroed, a null out-param is a caller
	// error caught before any lookup. The type rules and delivery need a live tree and are
	// pinned in UITests (GameUIElementHandleTests).
	float floatValue = 99.0f;
	EXPECT_EQ(GE_UIElement_GetValueFloat(1, &floatValue), GE_Result_NotFound);
	EXPECT_EQ(floatValue, 0.0f) << "out-params must be zeroed even when nothing resolves";
	EXPECT_EQ(GE_UIElement_GetValueFloat(1, nullptr), GE_Result_InvalidArg);
	EXPECT_EQ(GE_UIElement_SetValueFloat(1, 1.0f, 1), GE_Result_NotFound);
	int32_t boolValue = 99;
	EXPECT_EQ(GE_UIElement_GetValueBool(1, &boolValue), GE_Result_NotFound);
	EXPECT_EQ(boolValue, 0);
	EXPECT_EQ(GE_UIElement_SetValueBool(1, 1, 0), GE_Result_NotFound);
	char textBuf[8] = {};
	int32_t textLen = 99;
	EXPECT_EQ(GE_UIElement_GetValueText(1, textBuf, sizeof(textBuf), &textLen), GE_Result_NotFound);
	EXPECT_EQ(textLen, 0);
	EXPECT_EQ(GE_UIElement_SetValueText(1, "x", 1), GE_Result_NotFound);
	float scrollX = 99.0f;
	float scrollY = 99.0f;
	EXPECT_EQ(GE_UIElement_GetScrollOffset(1, &scrollX, &scrollY), GE_Result_NotFound);
	EXPECT_EQ(scrollX, 0.0f);
	EXPECT_EQ(scrollY, 0.0f);
	EXPECT_EQ(GE_UIElement_GetScrollOffset(1, nullptr, &scrollY), GE_Result_InvalidArg);
	EXPECT_EQ(GE_UIElement_SetScrollX(1, 1.0f), GE_Result_NotFound);
	EXPECT_EQ(GE_UIElement_SetScrollY(1, 1.0f), GE_Result_NotFound);
	int32_t index = 99;
	EXPECT_EQ(GE_UIElement_GetDropdownSelectedIndex(1, &index), GE_Result_NotFound);
	EXPECT_EQ(GE_UIElement_SetDropdownSelectedIndex(1, 0, 1), GE_Result_NotFound);
	EXPECT_EQ(GE_UIElement_GetDropdownSelectedLabel(1, textBuf, sizeof(textBuf), &textLen),
	          GE_Result_NotFound);
}


// A triangle whose node "Socket" carries extras.
constexpr std::string_view kSocketExtras = R"({"offset": [0, 1.5, 0]})";
constexpr std::string_view kTriangleWithSocketExtras = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "Socket", "mesh": 0, "extras": {"offset": [0, 1.5, 0]}}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";

// GE_Model_GetExtras reads what a loaded model kept, under the text-out contract ModelApi.GetExtras
// relies on: a length query, a full read, a short buffer that still reports the whole length. It refuses a
// thread other than the engine's main thread, and bad buffer arguments before it looks the model up.
TEST(ScriptingAbi, ModelGetExtrasReadsWhatTheLoadedModelKept)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) {
        ApplicationConfig cfg; cfg.AssetDirectory = "Assets";
        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);
        ASSERT_TRUE(engine.Initialize(cfg));
    }
    AssetManager* assetManager = engine.TryGetAssetManager();
    ASSERT_NE(assetManager, nullptr);

    const GUID modelGuid = GUID::Generate();
    auto model = std::make_shared<ModelAsset>(modelGuid, std::filesystem::path("abi_socket_extras.gltf"));
    ASSERT_TRUE(model->LoadFromData(Vector<uint8>(kTriangleWithSocketExtras.begin(), kTriangleWithSocketExtras.end())));
    assetManager->RegisterLoadedAsset(modelGuid, model);
    uint8 guidBytes[GUID::kSize];
    modelGuid.WriteBytes(guidBytes);
    const std::string_view socket = "Socket";
    const auto socketLength = static_cast<uint32_t>(socket.size());

    int32_t length = -1;
    ASSERT_EQ(GE_Model_GetExtras(guidBytes, GE_ModelObjectKind_Node, socket.data(), socketLength, nullptr, 0, &length),
              GE_Result_Ok);
    ASSERT_EQ(length, static_cast<int32_t>(kSocketExtras.size()));
    std::string text(static_cast<size_t>(length), '\0');
    int32_t written = -1;
    EXPECT_EQ(GE_Model_GetExtras(guidBytes, GE_ModelObjectKind_Node, socket.data(), socketLength, text.data(), length,
                                 &written),
              GE_Result_Ok);
    EXPECT_EQ(written, length);
    EXPECT_EQ(text, kSocketExtras);
    char shortBuffer[4] = {};
    EXPECT_EQ(GE_Model_GetExtras(guidBytes, GE_ModelObjectKind_Node, socket.data(), socketLength, shortBuffer,
                                 sizeof(shortBuffer), &written),
              GE_Result_Ok);
    EXPECT_EQ(written, length) << "a short buffer still reports the whole length";
    EXPECT_EQ(std::string_view(shortBuffer, sizeof(shortBuffer)), kSocketExtras.substr(0, sizeof(shortBuffer)));

    length = -1;
    EXPECT_EQ(GE_Model_GetExtras(guidBytes, GE_ModelObjectKind_Mesh, socket.data(), socketLength, nullptr, 0, &length),
              GE_Result_Ok);
    EXPECT_EQ(length, 0) << "an object with no extras reads back empty";
    EXPECT_EQ(GE_Model_GetExtras(guidBytes, GE_ModelObjectKind_Animation + 1u, socket.data(), socketLength, nullptr, 0,
                                 &length),
              GE_Result_InvalidArg);

    // Initialize marked this thread as the engine's main thread; any other thread is refused.
    ASSERT_TRUE(engine.GetScriptManager().HasMarkedMainThread());
    GE_Result workerResult = GE_Result_Ok;
    int32_t workerLength = -1;
    std::thread worker([&] {
        workerResult = GE_Model_GetExtras(guidBytes, GE_ModelObjectKind_Node, socket.data(), socketLength, nullptr, 0,
                                          &workerLength);
    });
    worker.join();
    EXPECT_EQ(workerResult, GE_Result_Fail) << "a call off the main thread is refused";
    EXPECT_EQ(workerLength, 0);

    assetManager->UnregisterLoadedAsset(modelGuid);
    length = -1;
    EXPECT_EQ(GE_Model_GetExtras(guidBytes, GE_ModelObjectKind_Node, socket.data(), socketLength, nullptr, 0, &length),
              GE_Result_NotFound);
    EXPECT_EQ(length, 0);
    EXPECT_EQ(GE_Model_GetExtras(guidBytes, GE_ModelObjectKind_Node, socket.data(), socketLength, nullptr, 5, &length),
              GE_Result_InvalidArg)
        << "a null buffer with a nonzero length is refused before the model is looked up";
}


// A process that has run the engine owns it for good. A GE_* export reached after this
// harness's Shutdown() — a managed finalizer, a stale callback — must find no engine, not
// start a second one under the bootstrap's own ApplicationConfig (GE_NATIVE_DIR or a bare
// "Assets", plus a changed working directory).
TEST(ScriptingAbi, ExportAfterHostShutdownDoesNotStartAnotherEngine)
{
	auto& engine = EngineCore::GetInstance();
	if (!engine.IsInitialized()) {
		ApplicationConfig cfg; cfg.AssetDirectory = "Assets";
		ScriptsConfig scriptsConfig{};
		scriptsConfig.enableHotReload = false;
		engine.SetScriptsConfig(scriptsConfig);
		ASSERT_TRUE(engine.Initialize(cfg));
	}
	engine.Shutdown();
	ASSERT_FALSE(engine.IsInitialized());

	// An export that needs the engine must be refused by the bootstrap — GE_Result_Fail, the
	// export's mapping of that refusal; a bootstrapped engine would answer NotInitialized for
	// its null RuntimeInput instead — and the engine must stay down.
	int32_t down = 42;
	EXPECT_EQ(GE_Input_IsKeyDown(0, &down), GE_Result_Fail);
	EXPECT_FALSE(engine.IsInitialized()) << "the ABI started an engine this process had already shut down";
}
