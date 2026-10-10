#include <gtest/gtest.h>

#include "Animation/RetargetAssetWatcher.h"

#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"

#include <atomic>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Animation;

namespace
{

AssetEvent MakeReloadedEvent(AssetType type, const GUID& guid)
{
    return AssetEvent(AssetEventType::AssetReloaded, guid, type, std::string("test://x"));
}

} // namespace

TEST(RetargetAssetWatcherTest, FiresProfileCallbackOnDispatch)
{
    AssetEventDispatcher dispatcher;
    RetargetAssetWatcher watcher;
    watcher.Attach(dispatcher);

    std::atomic<int> profileCount{0};
    std::atomic<int> rigCount{0};
    std::atomic<int> mapCount{0};
    GUID lastProfile;

    watcher.OnProfileReloaded([&](const GUID& g) { ++profileCount; lastProfile = g; });
    watcher.OnRigReloaded([&](const GUID&) { ++rigCount; });
    watcher.OnMapReloaded([&](const GUID&) { ++mapCount; });

    const GUID profileGuid("11111111-1111-1111-1111-111111111111");
    dispatcher.DispatchEvent(MakeReloadedEvent(AssetType::SkeletonProfile, profileGuid));

    EXPECT_EQ(profileCount.load(), 1);
    EXPECT_EQ(rigCount.load(), 0);
    EXPECT_EQ(mapCount.load(), 0);
    EXPECT_EQ(lastProfile.ToString(), profileGuid.ToString());
}

TEST(RetargetAssetWatcherTest, FiresRigCallbackOnlyForRigEvents)
{
    AssetEventDispatcher dispatcher;
    RetargetAssetWatcher watcher;
    watcher.Attach(dispatcher);

    std::atomic<int> rigCount{0};
    watcher.OnRigReloaded([&](const GUID&) { ++rigCount; });

    dispatcher.DispatchEvent(MakeReloadedEvent(AssetType::HumanoidRig, GUID()));
    dispatcher.DispatchEvent(MakeReloadedEvent(AssetType::SkeletonProfile, GUID()));
    dispatcher.DispatchEvent(MakeReloadedEvent(AssetType::Texture, GUID())); // unrelated

    EXPECT_EQ(rigCount.load(), 1);
}

TEST(RetargetAssetWatcherTest, FiresMapCallbackOnReload)
{
    AssetEventDispatcher dispatcher;
    RetargetAssetWatcher watcher;
    watcher.Attach(dispatcher);

    std::atomic<int> mapCount{0};
    watcher.OnMapReloaded([&](const GUID&) { ++mapCount; });

    dispatcher.DispatchEvent(MakeReloadedEvent(AssetType::RetargetMap, GUID()));
    EXPECT_EQ(mapCount.load(), 1);
}

TEST(RetargetAssetWatcherTest, MultipleCallbacksAllFire)
{
    AssetEventDispatcher dispatcher;
    RetargetAssetWatcher watcher;
    watcher.Attach(dispatcher);

    std::atomic<int> a{0}, b{0};
    watcher.OnProfileReloaded([&](const GUID&) { ++a; });
    watcher.OnProfileReloaded([&](const GUID&) { ++b; });

    dispatcher.DispatchEvent(MakeReloadedEvent(AssetType::SkeletonProfile, GUID()));

    EXPECT_EQ(a.load(), 1);
    EXPECT_EQ(b.load(), 1);
}

TEST(RetargetAssetWatcherTest, DetachStopsCallbacks)
{
    AssetEventDispatcher dispatcher;
    RetargetAssetWatcher watcher;
    watcher.Attach(dispatcher);

    std::atomic<int> count{0};
    watcher.OnProfileReloaded([&](const GUID&) { ++count; });

    dispatcher.DispatchEvent(MakeReloadedEvent(AssetType::SkeletonProfile, GUID()));
    EXPECT_EQ(count.load(), 1);

    watcher.Detach();
    EXPECT_FALSE(watcher.IsAttached());

    dispatcher.DispatchEvent(MakeReloadedEvent(AssetType::SkeletonProfile, GUID()));
    EXPECT_EQ(count.load(), 1); // still 1; watcher detached
}

TEST(RetargetAssetWatcherTest, TestSeamFiresWithoutDispatcher)
{
    RetargetAssetWatcher watcher;
    std::atomic<int> count{0};
    watcher.OnProfileReloaded([&](const GUID&) { ++count; });
    watcher.FireProfileReloadedForTest(GUID());
    EXPECT_EQ(count.load(), 1);
}
