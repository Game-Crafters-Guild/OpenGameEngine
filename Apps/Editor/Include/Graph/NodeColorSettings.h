#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine
{

namespace NodeColorSettings
{
    uint32_t DefaultNodeColorArgb(std::string_view kindId, const std::string& typeId);
    uint32_t GetNodeColorArgb(std::string_view kindId, const std::string& typeId);
    void SetNodeColorArgb(std::string_view kindId, const std::string& typeId, uint32_t argb);
    void ResetNodeColor(std::string_view kindId, const std::string& typeId);

    // Shared fill color of every node's body (the area below the colored header).
    uint32_t DefaultNodeBodyColorArgb();
    uint32_t GetNodeBodyColorArgb();
    /** Value boxes inside nodes: 30% darker than the body. */
    uint32_t GetNodeValueSurfaceColorArgb();
    /** Plates framing a rendered image (preview plate, texture slot, the graph's
        preview panel): 15% darker than the body. */
    uint32_t GetNodePlateColorArgb();
    void SetNodeBodyColorArgb(uint32_t argb);
    void ResetNodeBodyColor();

    // Canvas behind the nodes.
    uint32_t DefaultCanvasColorArgb();
    uint32_t GetCanvasColorArgb();
    void SetCanvasColorArgb(uint32_t argb);
    void ResetCanvasColor();

    void Reload();
}

} // namespace GameEngine
