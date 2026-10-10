#include "FileSystem/TreeRemoval.h"

#include "JobSystem/JobChannel.h"
#include "Logger/Logger.h"

namespace GameEngine::FileSystem
{

TreeRemoval::TreeRemoval(::JobSystem::WorkStealingThreadPool& jobSystem)
    : m_Channel(std::make_unique<::JobSystem::JobChannel>(
          jobSystem, ::JobSystem::JobChannelDesc{.Name = "Tree removals", .MaxRunning = 1}))
{
}

TreeRemoval::~TreeRemoval() = default;

void TreeRemoval::Remove(const std::filesystem::path& root)
{
    m_Channel->Enqueue(
        [root]()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            if (ec)
                LOG_WARNING("FileSystem: failed to remove '{}': {}", root.string(), ec.message());
            else
                LOG_INFO("FileSystem: removed '{}'", root.string());
        });
}

} // namespace GameEngine::FileSystem
