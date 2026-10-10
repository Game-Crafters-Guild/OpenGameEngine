#pragma once

#include "Assets/AssetManager.h"
#include "AssetCore/GUID.h"

#include <functional>
#include <optional>
#include <string>

namespace GameEngine
{
class UIManager;

struct WindowUiBootstrapRequest
{
    UIManager* ui = nullptr;
    AssetManager* assetManager = nullptr;
    GUID layoutGuid{};
    GUID styleGuid{};
    std::string contextLabel;
    std::function<bool(UIManager&)> fallbackRootBuilder;
};

struct WindowUiBootstrapResult
{
    bool layoutGuidProvided = false;
    bool styleGuidProvided = false;
    bool layoutLoaded = false;
    bool styleLoaded = false;
    bool fallbackApplied = false;
    bool hasRoot = false;
};

struct WindowUiBootstrapAsyncState
{
    WindowUiBootstrapRequest request{};
    WindowUiBootstrapResult result{};
    std::optional<AssetLoadHandle> layoutLoad;
    std::optional<AssetLoadHandle> styleLoad;
    bool complete = false;
};

class WindowUiBootstrap
{
  public:
    static WindowUiBootstrapResult Run(const WindowUiBootstrapRequest& request);
    static WindowUiBootstrapAsyncState BeginAsync(const WindowUiBootstrapRequest& request);
    static WindowUiBootstrapResult TickAsync(WindowUiBootstrapAsyncState& state);
    static bool IsAsyncComplete(const WindowUiBootstrapAsyncState& state) { return state.complete; }
};

} // namespace GameEngine
