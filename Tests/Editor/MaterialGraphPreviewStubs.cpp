// Leaf-dep stubs for EditorTests.
//
// MaterialGraphController owns the node preview atlas (MaterialGraphPreviews)
// and drives the panel's preview sphere (MaterialGraphPreviewHost). Their real
// TUs reach GraphCanvas, the render-graph tick and the preview material
// runtime, none of which the controller tests exercise: the tests never hand
// the controller an EditorContext or a preview host, so every call below is
// unreachable and a no-op at link time is sufficient.

#include "ShaderGraph/MaterialGraphPreviewHost.h"
#include "ShaderGraph/MaterialGraphPreviews.h"

namespace GameEngine
{
    void MaterialGraphPreviewHost::SetVisibleForMaterialGraph(bool) {}
    void MaterialGraphPreviewHost::RefreshFromModel(const Graph::Model&, const std::string&,
                                                    const std::filesystem::path&)
    {
    }
    void MaterialGraphPreviewHost::RefreshMaterialProperties(const Graph::Model&) {}
    void MaterialGraphPreviewHost::ApplyPreviewBinding(const Editor::GraphNodePreviewBinding&) {}
    void MaterialGraphPreviewHost::OnParentLayoutReady() {}
    void MaterialGraphPreviewHost::ClearPreview() {}
    void MaterialGraphPreviewHost::SetIblEnabled(bool) {}

    namespace Editor
    {
        void MaterialGraphPreviews::Initialize(const EditorContext*, bool) {}
        void MaterialGraphPreviews::Shutdown() {}
        void MaterialGraphPreviews::Sync(const EditorContext*, const Graph::Model&, bool,
                                         GraphCanvas*, bool)
        {
        }
        void MaterialGraphPreviews::SetMainPreviewMaterial(const GUID&) {}
        bool MaterialGraphPreviews::TryGetMainPreviewBinding(GraphNodePreviewBinding&) const
        {
            return false;
        }
        void MaterialGraphPreviews::SetMainPreviewYaw(float) {}
        void MaterialGraphPreviews::SyncValues(const EditorContext*, const Graph::Model&) {}
    }
}
