#include "Assets/SceneAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"

#include <filesystem>

namespace GameEngine
{

bool SceneAsset::Load()
{
    const std::filesystem::path& p = GetPath();
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
    {
        Logger::Log::Error("SceneAsset: file does not exist '{}'", p.string());
        return false;
    }

    std::string text;
    if (!ReadFileTextShared(p, text))
    {
        Logger::Log::Error("SceneAsset: failed to open/read '{}'", p.string());
        return false;
    }
    m_Text = std::move(text);
    return true;
}

bool SceneAsset::LoadFromData(const Vector<uint8>& data)
{
    m_Text.assign(reinterpret_cast<const char*>(data.data()), data.size());
    return true;
}

void SceneAsset::Unload()
{
    m_Text.clear();
    m_Text.shrink_to_fit();
}

} // namespace GameEngine

