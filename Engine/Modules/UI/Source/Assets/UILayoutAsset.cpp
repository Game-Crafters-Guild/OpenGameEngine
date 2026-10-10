#include "UI/Assets/UILayoutAsset.h"
#include "UI/Parsers/XMLParser.h"
#include <fstream>

using namespace GameEngine;
using namespace GameEngine::UIParsing;

bool UILayoutAsset::Load() {
    // Read from file path
    std::ifstream in(GetPath());
    if (!in.is_open()) { SetState(AssetState::Failed); return false; }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    // Load as a declarative template tree (no control construction). UIManager will
    // instantiate a live control tree from this template when binding.
    bool ok = XMLParser::ParseLayoutTemplateFromString(text, m_Root, GetPath().string());
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

bool UILayoutAsset::LoadFromData(const Vector<uint8>& data) {
    std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    bool ok = XMLParser::ParseLayoutTemplateFromString(text, m_Root, GetPath().string());
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

void UILayoutAsset::Unload() {
    m_Root.reset();
    SetState(AssetState::Unloaded);
}
