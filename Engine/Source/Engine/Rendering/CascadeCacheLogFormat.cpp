#include "Engine/Rendering/CascadeCacheLogFormat.h"

#include <format>

namespace GameEngine
{
namespace Engine::Renderer
{

std::string FormatCascadeCacheCauseHistogram(const CascadeShadowCache::Stats& window,
                                             uint64_t casterContentVersion)
{
    const auto count = [&window](CascadeCacheDirtyCause c)
    { return window.CauseCounts[static_cast<size_t>(c)]; };
    return std::format("first {} contrib {} contribchg {} underdraw {} phys {} casters {}@v{} "
                       "camera {} fit {} config {} settle {}",
                       count(CascadeCacheDirtyCause::FirstRender),
                       count(CascadeCacheDirtyCause::ContributorPresent),
                       count(CascadeCacheDirtyCause::ContributorChanged),
                       count(CascadeCacheDirtyCause::ExecUnderDraw),
                       count(CascadeCacheDirtyCause::PhysicalChanged),
                       count(CascadeCacheDirtyCause::CasterContentChanged), casterContentVersion,
                       count(CascadeCacheDirtyCause::CameraChanged),
                       count(CascadeCacheDirtyCause::CascadeFitChanged),
                       count(CascadeCacheDirtyCause::ConfigChanged),
                       count(CascadeCacheDirtyCause::CullNotSettled));
}

} // namespace Engine::Renderer
} // namespace GameEngine
