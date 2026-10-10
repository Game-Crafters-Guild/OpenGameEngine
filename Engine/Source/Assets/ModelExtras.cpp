#include "Assets/ModelExtras.h"

#include <cassert>

namespace GameEngine {

ModelExtras::RetainResult ModelExtras::Retain(ModelObjectKind kind, std::string_view name, std::string_view text)
{
    assert(static_cast<size_t>(kind) < kModelObjectKindCount);
    if (m_ModelBoundReached)
        return RetainResult::OverModelBound;
    if (text.size() > kObjectBoundBytes)
        return RetainResult::OverObjectBound;
    auto& blocks = m_BlocksByKind[static_cast<size_t>(kind)];
    if (blocks.contains(name))
        return RetainResult::NameTaken;
    const size_t charge = name.size() + text.size();
    if (charge > kModelBoundBytes - m_RetainedBytes) {
        m_ModelBoundReached = true;
        return RetainResult::OverModelBound;
    }
    blocks.emplace(String(name), String(text));
    m_RetainedBytes += charge;
    return RetainResult::Retained;
}

std::string_view ModelExtras::Find(ModelObjectKind kind, std::string_view name) const
{
    assert(static_cast<size_t>(kind) < kModelObjectKindCount);
    const auto& blocks = m_BlocksByKind[static_cast<size_t>(kind)];
    const auto block = blocks.find(name);
    return block != blocks.end() ? std::string_view(block->second) : std::string_view();
}

} // namespace GameEngine
