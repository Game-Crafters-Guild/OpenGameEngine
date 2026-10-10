#include "UndoRedo/SetAssetMetaValueCommand.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"

#include <utility>

namespace GameEngine::Editor
{

SetAssetMetaValueCommand::SetAssetMetaValueCommand(
    const std::filesystem::path& assetPath,
    std::string metaKey,
    std::string newValue,
    std::function<void(const std::filesystem::path&, const std::string&)> write,
    std::function<void()> onWritten)
    : m_AssetGuid(EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(assetPath)),
      m_MetaKey(std::move(metaKey)),
      m_NewValue(std::move(newValue)),
      m_Write(std::move(write)),
      m_OnWritten(std::move(onWritten))
{
}

void SetAssetMetaValueCommand::Do()
{
    if (!m_PreviousValueCaptured)
    {
        std::filesystem::path path;
        if (TryResolveCurrentPath(path))
        {
            EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetMetaValue(
                path, m_MetaKey, m_PreviousValue);
        }
        m_PreviousValueCaptured = true;
    }
    Write(m_NewValue);
}

void SetAssetMetaValueCommand::Undo()
{
    Write(m_PreviousValue);
}

void SetAssetMetaValueCommand::Write(const std::string& value)
{
    std::filesystem::path path;
    if (!TryResolveCurrentPath(path))
        return;
    if (m_Write)
        m_Write(path, value);
    if (m_OnWritten)
        m_OnWritten();
}

bool SetAssetMetaValueCommand::TryResolveCurrentPath(std::filesystem::path& outPath) const
{
    if (m_AssetGuid.IsNull())
        return false;
    AssetMetadata metadata{};
    if (!EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(m_AssetGuid,
                                                                                       metadata))
        return false;
    if (metadata.Path.empty())
        return false;
    outPath = metadata.Path;
    return true;
}

} // namespace GameEngine::Editor
