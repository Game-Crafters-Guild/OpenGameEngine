#pragma once

#include "InspectorRegistry.h"
#include "Graph/GraphPortedNode.h"
#include "ShaderGraph/GraphNodePreviewBinding.h"

#include <functional>
#include <string>

namespace GameEngine {

/**
 * The material graph's node element, instantiated by the node factory GraphPanel
 * installs on its canvas for the material kind. Owner of material-only node
 * behavior (color picker, swatch, parameter value rows, texture drop slot,
 * preview binding) as it factors out of the generic graph types.
 */
class MaterialGraphNode : public GraphPortedNode {
public:
    MaterialGraphNode();
    ~MaterialGraphNode() override = default;

    /** The window this node's colour swatch opens, handed down by the material
        controller that made the node. */
    void SetOpenColorPicker(OpenColorPickerWindowFn fn) { m_OpenColorPicker = std::move(fn); }

    /** Material values live in the inspector and the node body, never on port
        rows — ColorConstant included, whose editing is its swatch. */
    bool DrawsInlinePortEditors(const Graph::Node& node) const override
    {
        (void)node;
        return false;
    }

    /** A square preview plate under the detail rows, in expanded view only.
        Texture nodes reserve nothing — their drop slot already shows the
        texture, and stacking a sphere under it made them two squares tall for
        less signal than the texture alone. */
    float ReservedBlockHeight(const Graph::Node& node, bool expandedView) const override;

    /** Parameter nodes surface their VARIABLE's value on the node: one
        synthetic Value row in expanded view (float scrub, X/Y/Z chips for
        float3, raw list for float2/4, a color swatch for ColorParameter),
        editing model->Variables in two-way sync with the Variables board. */
    int SyntheticDetailRowCount(const Graph::Node& node, bool expandedView) const override;

    /** Where a node's rendered preview comes from: the owning panel's atlas,
        looked up by node id. Installed by the factory that builds these nodes,
        so no generic graph type carries preview vocabulary. An unset provider
        (or an empty binding) leaves the reserved plate showing its own tone
        until a cell arrives — the block is reserved from the node model, so
        the plate always fills it. */
    using PreviewLookupFn = std::function<Editor::GraphNodePreviewBinding(const std::string&)>;
    void SetPreviewLookup(PreviewLookupFn lookup) { m_PreviewLookup = std::move(lookup); }

protected:
    /** Texture nodes ARE their texture: the slot shows it and stays clickable
        for reassignment. Every other node paints its atlas cell into the
        reserved plate instead, so the slot is hidden. */
    void BindDetailSlot(const Graph::Node& node, GraphPortDropSlot& slot) override;

    /** The parameter Value row (see SyntheticDetailRowCount). */
    std::string SyntheticDetailRowLabel(const Graph::Node& node, int extraIndex) const override;
    /** ColorConstant's Color row: open the panel's picker for this node. */
    void OpenColorConstantPicker();
    void BindSyntheticDetailRow(const Graph::Node& node, int extraIndex, Label* label,
                            UIElement* host) override;

private:
    OpenColorPickerWindowFn m_OpenColorPicker;
    bool BindTextureSlot(const Graph::Node& node, GraphPortDropSlot& slot);
    void BindPreviewPlate(const Graph::Node& node);
    bool WantsPreviewPlate(const Graph::Node& node, bool expandedView) const;

    PreviewLookupFn m_PreviewLookup;
    UIElement* m_PreviewPlate = nullptr;
};

} // namespace GameEngine
