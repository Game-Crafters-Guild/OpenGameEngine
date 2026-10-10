#include "Animation/RetargetAssetWatcher.h"

namespace GameEngine
{
namespace Animation
{

void RetargetAssetWatcher::Attach(::GameEngine::AssetEventDispatcher& dispatcher)
{
    Detach();

    m_ProfileWatcher = ::GameEngine::AssetReloadInvalidator(
        dispatcher, ::GameEngine::AssetType::SkeletonProfile,
        [this](const ::GameEngine::GUID& g) { DispatchProfile(g); });
    m_RigWatcher = ::GameEngine::AssetReloadInvalidator(
        dispatcher, ::GameEngine::AssetType::HumanoidRig,
        [this](const ::GameEngine::GUID& g) { DispatchRig(g); });
    m_MapWatcher = ::GameEngine::AssetReloadInvalidator(
        dispatcher, ::GameEngine::AssetType::RetargetMap,
        [this](const ::GameEngine::GUID& g) { DispatchMap(g); });

    m_Attached = true;
}

void RetargetAssetWatcher::Detach()
{
    m_ProfileWatcher.Reset();
    m_RigWatcher.Reset();
    m_MapWatcher.Reset();
    m_Attached = false;
}

void RetargetAssetWatcher::OnProfileReloaded(Callback cb)
{
    if (cb)
        m_ProfileCallbacks.push_back(std::move(cb));
}

void RetargetAssetWatcher::OnRigReloaded(Callback cb)
{
    if (cb)
        m_RigCallbacks.push_back(std::move(cb));
}

void RetargetAssetWatcher::OnMapReloaded(Callback cb)
{
    if (cb)
        m_MapCallbacks.push_back(std::move(cb));
}

void RetargetAssetWatcher::FireProfileReloadedForTest(const ::GameEngine::GUID& guid)
{
    DispatchProfile(guid);
}

void RetargetAssetWatcher::FireRigReloadedForTest(const ::GameEngine::GUID& guid)
{
    DispatchRig(guid);
}

void RetargetAssetWatcher::FireMapReloadedForTest(const ::GameEngine::GUID& guid)
{
    DispatchMap(guid);
}

void RetargetAssetWatcher::DispatchProfile(const ::GameEngine::GUID& guid)
{
    for (const auto& cb : m_ProfileCallbacks)
        cb(guid);
}

void RetargetAssetWatcher::DispatchRig(const ::GameEngine::GUID& guid)
{
    for (const auto& cb : m_RigCallbacks)
        cb(guid);
}

void RetargetAssetWatcher::DispatchMap(const ::GameEngine::GUID& guid)
{
    for (const auto& cb : m_MapCallbacks)
        cb(guid);
}

} // namespace Animation
} // namespace GameEngine
