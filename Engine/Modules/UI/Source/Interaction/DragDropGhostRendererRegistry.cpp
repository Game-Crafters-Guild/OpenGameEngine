#include "UI/Interaction/DragDropGhostRendererRegistry.h"

#include <unordered_map>

namespace GameEngine::UI::Interaction
{
static std::unordered_map<PayloadTypeId, DragGhostRendererEntry>& Registry()
{
    static std::unordered_map<PayloadTypeId, DragGhostRendererEntry> s;
    return s;
}

void RegisterDragGhostRenderer(PayloadTypeId typeId, DragGhostRendererFn fn, void* userData)
{
    if (typeId == 0)
        return;
    if (!fn)
    {
        UnregisterDragGhostRenderer(typeId);
        return;
    }
    Registry()[typeId] = DragGhostRendererEntry{fn, userData};
}

void UnregisterDragGhostRenderer(PayloadTypeId typeId)
{
    Registry().erase(typeId);
}

const DragGhostRendererEntry* FindDragGhostRenderer(PayloadTypeId typeId)
{
    auto& r = Registry();
    auto it = r.find(typeId);
    if (it == r.end())
        return nullptr;
    return &it->second;
}
} // namespace GameEngine::UI::Interaction

