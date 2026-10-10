#include "Core/WindowUiBootstrap.h"

#include "Assets/AssetManager.h"
#include "Logger/Logger.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/UIManager.h"

#include <chrono>
#include <thread>

namespace GameEngine
{

namespace
{

std::string GetBootstrapLabel(const WindowUiBootstrapRequest& request)
{
    return request.contextLabel.empty() ? std::string("window") : request.contextLabel;
}

void ApplyFallbackIfNeeded(const WindowUiBootstrapRequest& request,
                           WindowUiBootstrapResult& result,
                           const std::string& label)
{
    result.hasRoot = request.ui && request.ui->GetRootElement() != nullptr;
    if (result.hasRoot || !request.fallbackRootBuilder || !request.ui)
        return;

    result.fallbackApplied = request.fallbackRootBuilder(*request.ui);
    result.hasRoot = request.ui->GetRootElement() != nullptr;
    if (result.fallbackApplied && result.hasRoot)
    {
        Logger::Log::Info("[WindowUiBootstrap] {} fallback root applied", label);
    }
    else
    {
        Logger::Log::Warning("[WindowUiBootstrap] {} fallback root failed", label);
    }
}

} // namespace

WindowUiBootstrapResult WindowUiBootstrap::Run(const WindowUiBootstrapRequest& request)
{
    auto MsSince = [](const auto& t) {
        return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count();
    };
    const auto tRun = std::chrono::high_resolution_clock::now();

    WindowUiBootstrapAsyncState state = BeginAsync(request);
    while (!state.complete)
    {
        (void)TickAsync(state);
        if (!state.complete)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Logger::Log::Info("[Startup]     Bootstrap.Run total: {:.1f}ms", MsSince(tRun));
    return state.result;
}

WindowUiBootstrapAsyncState WindowUiBootstrap::BeginAsync(const WindowUiBootstrapRequest& request)
{
    WindowUiBootstrapAsyncState state{};
    state.request = request;
    if (!request.ui || !request.assetManager)
    {
        Logger::Log::Warning("[WindowUiBootstrap] invalid request: ui={} assetManager={}",
                             request.ui ? "set" : "null",
                             request.assetManager ? "set" : "null");
        state.complete = true;
        return state;
    }

    const std::string label = GetBootstrapLabel(request);
    state.result.layoutGuidProvided = !request.layoutGuid.IsNull();
    if (state.result.layoutGuidProvided)
    {
        state.layoutLoad.emplace(
            request.assetManager->LoadAsset(request.layoutGuid, AssetLoadResultCallback{}, AssetLoadPriority::High));
    }
    else
    {
        Logger::Log::Warning("[WindowUiBootstrap] {} layout guid is null", label);
    }

    state.result.styleGuidProvided = !request.styleGuid.IsNull();
    if (state.result.styleGuidProvided)
    {
        state.styleLoad.emplace(
            request.assetManager->LoadAsset(request.styleGuid, AssetLoadResultCallback{}, AssetLoadPriority::High));
    }
    else
    {
        Logger::Log::Warning("[WindowUiBootstrap] {} style guid is null", label);
    }

    ApplyFallbackIfNeeded(request, state.result, label);
    state.complete = !state.layoutLoad.has_value() && !state.styleLoad.has_value();
    if (state.complete && !state.result.hasRoot)
    {
        Logger::Log::Warning("[WindowUiBootstrap] {} has no UI root after bootstrap", label);
    }

    return state;
}

WindowUiBootstrapResult WindowUiBootstrap::TickAsync(WindowUiBootstrapAsyncState& state)
{
    if (state.complete)
    {
        return state.result;
    }

    if (!state.request.ui || !state.request.assetManager)
    {
        state.complete = true;
        return state.result;
    }

    auto MsSince = [](const auto& t) {
        return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count();
    };

    const std::string label = GetBootstrapLabel(state.request);
    auto consumeLoad = [&](std::optional<AssetLoadHandle>& handle, AssetType expectedType, bool isLayout)
    {
        if (!handle.has_value() || !handle->IsComplete())
            return;

        const GUID guid = isLayout ? state.request.layoutGuid : state.request.styleGuid;
        SharedPtr<Asset> asset = handle->GetResult();
        handle.reset();

        const char* kind = isLayout ? "layout" : "style";
        if (!asset)
        {
            Logger::Log::Warning("[WindowUiBootstrap] {} {} load failed (guid={})",
                                 label,
                                 kind,
                                 guid.ToString());
            return;
        }

        if (asset->GetType() != expectedType)
        {
            Logger::Log::Warning("[WindowUiBootstrap] {} {} guid type mismatch (expected {}, got={})",
                                 label,
                                 kind,
                                 isLayout ? "UILayout" : "UIStyle",
                                 static_cast<int>(asset->GetType()));
            return;
        }

        auto tApply = std::chrono::high_resolution_clock::now();
        if (isLayout)
        {
            state.result.layoutLoaded = state.request.ui->LoadLayoutFromAsset(*static_cast<UILayoutAsset*>(asset.get()));
            Logger::Log::Info("[Startup]     LoadLayoutFromAsset: {:.1f}ms", MsSince(tApply));
            if (!state.result.layoutLoaded)
            {
                Logger::Log::Warning("[WindowUiBootstrap] {} failed to apply UILayout asset", label);
            }
        }
        else
        {
            state.result.styleLoaded = state.request.ui->AttachStyleFromAsset(*static_cast<UIStyleAsset*>(asset.get()));
            Logger::Log::Info("[Startup]     AttachStyleFromAsset: {:.1f}ms", MsSince(tApply));
            if (!state.result.styleLoaded)
            {
                Logger::Log::Warning("[WindowUiBootstrap] {} failed to apply UIStyle asset", label);
            }
        }
    };

    consumeLoad(state.layoutLoad, AssetType::UILayout, true);
    consumeLoad(state.styleLoad, AssetType::UIStyle, false);

    state.complete = !state.layoutLoad.has_value() && !state.styleLoad.has_value();
    if (state.complete)
    {
        ApplyFallbackIfNeeded(state.request, state.result, label);
        if (!state.result.hasRoot)
        {
            Logger::Log::Warning("[WindowUiBootstrap] {} has no UI root after bootstrap", label);
        }
    }
    else
    {
        state.result.hasRoot = state.request.ui->GetRootElement() != nullptr;
    }

    return state.result;
}

} // namespace GameEngine
