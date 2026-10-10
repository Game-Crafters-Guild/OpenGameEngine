// GraphCanvas / GraphPanel statics referenced by GraphSettingsPage.cpp.
// EditorSdkRegistryTests does not compile those TUs; the closures are not
// invoked by GraphSettingsPageTests, but the linker still needs the symbols.
//
// Invariant: every GraphCanvas/GraphPanel static that GraphSettingsPage.cpp
// names has a mirror here returning the real default. Adding a row to that
// page without its mirror breaks this target's link.

#include "Graph/GraphCanvas.h"
#include "Panels/GraphPanel.h"

#include <string>

namespace GameEngine
{

float GraphCanvas::GetNodeCornerRadius()
{
    return kDefaultNodeCornerRadius;
}

void GraphCanvas::SetNodeCornerRadius(float)
{
}

bool GraphCanvas::GetConnectionRoundedCorners()
{
    return true;
}

void GraphCanvas::SetConnectionRoundedCorners(bool)
{
}

bool GraphCanvas::GetNodeDropShadows()
{
    return true;
}

void GraphCanvas::SetNodeDropShadows(bool)
{
}

void GraphCanvas::SetNodeDropShadowStyle(float, float, float, float)
{
}

std::string GraphCanvas::GetNodeHeaderAlignment()
{
    return "left";
}

void GraphCanvas::SetNodeHeaderAlignment(const std::string&)
{
}

std::string GraphCanvas::GetSelectedNodeWireEmphasis()
{
    return "glow";
}

void GraphCanvas::SetSelectedNodeWireEmphasis(const std::string&)
{
}

void GraphCanvas::MarkLiveCanvasesDirty()
{
}

bool GraphPanel::GetPanelScrollbarsPreference()
{
    return false;
}

void GraphPanel::SetPanelScrollbarsPreference(bool)
{
}

bool GraphPanel::GetVariablesScrollbarsPreference()
{
    return true;
}

void GraphPanel::SetVariablesScrollbarsPreference(bool)
{
}

} // namespace GameEngine
