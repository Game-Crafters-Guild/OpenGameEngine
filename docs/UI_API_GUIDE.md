# UI API Guide

This guide covers how to build UI with XML/CSS and C++, how to register controls,
and how to update styles and properties at runtime.

## Quick start

```cpp
#include "UI/UIManager.h"
#include "UI/UITargetSpace.h"

GameEngine::UIManager ui(device, assetManager);
ui.LoadLayoutFromFile("Assets/UI/MyLayout.xml");
ui.AttachStyleFromFile("Assets/UI/MyTheme.css");

while (running) {
    ui.Update(dt);
    // The host begins this frame and supplies its target and target color space.
    if (!ui.RenderRG(frame, target, targetSpace)) {
        // No pass was declared: skip encoding/presenting this target.
        continue;
    }
}
```

`frame` is a `Rendering::RenderGraph::RGFrame`, `target` is a texture from that frame,
and `targetSpace` is the host's explicit `UI::UITargetSpace` for the attachment.
The three-argument overload clears the target. To composite a HUD over existing
scene content, pass `Rendering::RenderGraph::RGLoadOp::Load` as the fourth argument.
The host executes the completed graph after declaration; do not retain frame texture IDs.

Manual root setup:

```cpp
auto root = std::make_unique<GameEngine::UIElement>("UIElement");
root->SetId("Root");
root->AddClass("root");
ui.SetRoot(std::move(root));
```

## Loading layouts and styles

- File-based: `LoadLayoutFromFile`, `AttachStyleFromFile` (and async variants).
- Asset-based: `LoadLayoutFromAsset`, `AttachStyleFromAsset`.
- Subtree styles: `AttachStyleToSubtreeFromAsset` / `AttachStyleToSubtreeFromAssetPath`.

## Authoring layouts (XML)

Tag names map to registered controls (see `UI/Controls` and `UI/Registration`).

Common attributes:
- `id` (or `name`) -> `UIElement::SetId`
- `class` -> `AddClass` for each token
- `style` -> parsed once into inline `StyleOverrides` (inline CSS declarations)

Example:

```xml
<UIElement id="SettingsPanel" class="panel">
  <Label text="Audio" />
  <Checkbox id="MusicEnabled" checked="true" text="Music" />
  <Slider id="Volume" min="0" max="1" value="0.8" step="0.01" />
</UIElement>
```

Notes:
- Controls that register `Text()` bindings can use inner text as well as a `text`
  attribute (e.g. `<Button>Ok</Button>`).
- Unknown tags fall back to `UIElement` with a warning.

## Styling (CSS)

Every UI manager loads the UI module's default stylesheet under your sheets. It gives unstyled
engine controls a visible track, thumb, knob and box, and any rule of yours wins over it. Its
color properties, including the slider track's `--slider-track-fill`, `--slider-track` and
`--slider-tick`, are listed in [UI control defaults](ui-control-defaults.html).

Selectors supported include tag, `.class`, `#id`, `[attr=value]`, and pseudo-classes
like `:hover`, `:active`, `:focus`, `:focus-visible` and `:checked`.

`:focus-visible` matches keyboard focus only: focus that Tab moved, or that your code moved
(`UIManager::FocusElement`, `SetFocusById`) while a key event was being dispatched, such as a
shortcut that takes the user to a button. Focus moved by a click matches `:focus` alone, so
use `:focus-visible` for focus rings.

Attribute selectors read authored attributes stored on the element. `id` and `name`
are special-cased to `UIElement::GetId()`. Attribute names are normalized to
lowercase and matched case-insensitively; prefer lowercase attribute names in
XML and CSS for consistency.

Custom properties (`--vars`) can be read from code:

```cpp
const auto* style = el.GetResolvedStyle();
if (style) {
    float pad = style->GetCustomNumber("--pad", 0.0f);
    uint32_t accent = style->GetCustomColor("--accent", 0xFF000000u);
}
```

For hot paths, pre-hash with `StringId`:

```cpp
using namespace GameEngine;
constexpr StringId kPad = "--pad"_sid;
float pad = style->GetCustomNumber(kPad, 0.0f);
```

## Programmatic UI construction

```cpp
auto root = std::make_unique<UIElement>("UIElement");
auto label = std::make_unique<Label>();
label->SetText("Hello");
label->AddClass("title");
root->AddChild(std::move(label));

ui.SetRoot(std::move(root));
```

If you add children after the root is attached to a `UIManager`, ownership
propagates automatically.

## Style overrides (code)

Use `StyleOverrides` for programmatic overrides. Typed property keys provide
compile-time safety; dirty flags are tracked automatically via the
`StyleProp<T, StyleImpact>` template.

```cpp
el.Overrides()
  .Set(Style::Width, StyleLength::Px(200.0f))
  .Set(Style::Height, StyleLength::Px(40.0f))
  .Set(Style::BackgroundColor, 0xFF202020u);
el.Overrides().SetCustomNumber("--pad"_sid, 4.0f);
```

Reading back an override:
```cpp
if (auto bg = el.Overrides().Get(Style::BackgroundColor))
    LOG_INFO("Background color: 0x{:08X}", *bg);
```

Reset a property to inherit from CSS:
```cpp
el.Overrides().Reset(Style::BackgroundColor);
```

For live background textures, use `Styles()`:

```cpp
el.Styles()
  .SetBackgroundTexture(rgTextureRef)
  .SetBackgroundSizeCover()
  .SetBackgroundTint(0xFFFFFFFF);
```

## Events

Event IDs are hashed `StringId` values (see `UI/UIEvents.h`).

```cpp
using namespace GameEngine;

auto token = el.RegisterEventHandler(kEventMouseDown, [](UIEvent& e) {
    e.Stop();
});

el.RegisterEventHandler(HashEvent("My.CustomEvent"), [](UIEvent& e) {
    // handle custom event
});
```

## Creating new controls

1. Subclass `UIElement` (optionally implement `ITextMeasurable`).
2. Override `OnGeneratePrimitives` to emit custom SDF primitives.
3. Override `OnPostLayout` if you need layout-dependent setup.
4. Register the control and its attributes with `UIRegistration`.

Example:

```cpp
class Badge : public UIElement {
public:
    Badge() : UIElement("Badge") {}
    void SetText(const std::string& text) { m_text = text; MarkDirty(VisualDirty); }

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    std::string m_text;
};
```

Registration and attribute binding:

```cpp
using namespace GameEngine::UIRegistration;

static auto s_reg_badge = Register<Badge>("Badge")
    .Text(&Badge::SetText)
    .Attr("text", &Badge::SetText)
    .TagAlias("badge");
```

For custom attribute types, specialize `Parser<T>`:

```cpp
template<>
struct Parser<Badge::Mode> {
    static Badge::Mode Parse(std::string_view s) { /* ... */ }
};
```

## Modifying attributes and properties

Prefer dedicated APIs:
- `SetId`, `AddClass`, `RemoveClass`
- `SetEnabled`, `SetFocusable`, `SetTabIndex`
- `Overrides()` for style changes (typed `StyleOverrides` API)

Inline `style` and arbitrary attributes are intended for XML/CSS parsing rather
than runtime code.
