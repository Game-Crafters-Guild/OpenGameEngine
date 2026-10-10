#include "Editor/Hierarchy/HierarchyLightIconTint.h"

#include "Components/Rendering/Light.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "UI/Controls/TreeView.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <cstdint>

namespace GameEngine::Editor
{
namespace
{
// The uncolored icon tint, rgba(200,200,200) in views.css: colored icons change hue and saturation,
// not perceived brightness.
constexpr float kColoredIconBrightness = 200.0f / 255.0f;
constexpr uint32_t kNeutralIconTint = 0xFFC8C8C8u;

uint32_t UnitToByte(float value)
{
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;
    return static_cast<uint32_t>(value * 255.0f + 0.5f);
}

bool ColoredIconsOn(const UIElement& icon)
{
    UIManager* manager = icon.GetOwnerManager();
    UIElement* root = manager ? manager->GetRootElement() : nullptr;
    return root && root->HasClass("hierarchy-icons-colored");
}
} // namespace

UIElement* FindHierarchyPreviewIcon(const UIElement& row)
{
    for (const auto& child : row.GetChildren())
    {
        if (child && child->HasClass("hierarchy-preview-icon"))
            return child.get();
    }
    return nullptr;
}

void ApplyHierarchyLightIconTint(ECS::World& world, ECS::EntityHandle entity, UIElement& icon)
{
    if (!ColoredIconsOn(icon))
    {
        icon.Overrides().Reset(Style::BackgroundTint);
        icon.Overrides().Reset(Style::BackgroundImageSaturation);
        icon.MarkDirty(UIElement::VisualDirty);
        return;
    }

    icon.Overrides().Set(Style::BackgroundImageSaturation, 1.0f);
    const auto* light = world.GetComponent<Components::Light>(entity);
    if (!light)
    {
        icon.Overrides().Set(Style::BackgroundTint, kNeutralIconTint);
        icon.MarkDirty(UIElement::VisualDirty);
        return;
    }

    const uint32_t r = UnitToByte(light->Color[0] * kColoredIconBrightness);
    const uint32_t g = UnitToByte(light->Color[1] * kColoredIconBrightness);
    const uint32_t b = UnitToByte(light->Color[2] * kColoredIconBrightness);
    icon.Overrides().Set(Style::BackgroundTint, (0xFFu << 24) | (r << 16) | (g << 8) | b);
    icon.MarkDirty(UIElement::VisualDirty);
}

void RetintHeldHierarchyLightRow(ECS::World& world, ECS::EntityHandle entity, const TreeView& tree,
                                 std::uint64_t treeId)
{
    UIElement* row = tree.FindBoundRow(treeId);
    if (!row)
        return;
    if (UIElement* icon = FindHierarchyPreviewIcon(*row))
        ApplyHierarchyLightIconTint(world, entity, *icon);
}

} // namespace GameEngine::Editor
