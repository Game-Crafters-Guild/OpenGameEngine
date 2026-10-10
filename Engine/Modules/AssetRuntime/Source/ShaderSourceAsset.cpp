#include "Assets/ShaderSourceAsset.h"

#include "AssetCore/SharedFileRead.h"

namespace GameEngine {

bool ShaderSourceAsset::Load()
{
    try
    {
        Vector<uint8> bytes;
        if (!ReadFileBytesShared(GetPath(), bytes))
        {
            SetState(AssetState::Failed);
            return false;
        }
        return LoadFromData(bytes);
    }
    catch (...)
    {
        SetState(AssetState::Failed);
        return false;
    }
}

bool ShaderSourceAsset::LoadFromData(const Vector<uint8>& data)
{
    try
    {
        m_Text.assign(reinterpret_cast<const char*>(data.data()), data.size());
        SetState(AssetState::Loaded);
        return true;
    }
    catch (...)
    {
        SetState(AssetState::Failed);
        return false;
    }
}

void ShaderSourceAsset::Unload()
{
    m_Text.clear();
    SetState(AssetState::Unloaded);
}

} // namespace GameEngine

