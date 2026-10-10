# UI System Design (Draft 0.1)

Author: Augment Agent

## User guide
For practical usage and API examples, see [docs/UI_API_GUIDE.md](../../../docs/UI_API_GUIDE.md).

## Goals and scope
- XML + CSS-driven UI that integrates with the engine’s AssetManager, Rendering (geometry + text), and input systems
- High performance: batched draw calls, minimal CPU overhead, incremental layout and visual updates
- Flexible: custom events, bubbling/capturing, pseudo-classes (hover, active, focus) and custom selectors
- Hot reload: .xml and .css edits reflected at runtime with minimal disruption
- Modern layout: flexbox with a reliable implementation

## Recommended dependencies (via vcpkg)
- XML: pugixml
  - Rationale: small, fast, robust DOM; XPath support; widely used; vcpkg port `pugixml`
- CSS parsing + selectors: Lexbor (required)
  - Lexbor is the only supported CSS engine in this codebase; no internal fallback or env/flag switches.
  - Implementation uses lxb_css_stylesheet_parse() API.
- Flexbox layout: Facebook Yoga (`yoga`)
  - Rationale: production-tested, fast, single-purpose flex implementation; maps well to CSS flex properties

Note: we’ll wrap these behind thin interfaces so we can swap if needed.

> Implementation note: The current codebase uses Lexbor exclusively for CSS (no internal fallback and no env/flag switches).

## Current status
- Data-driven UI: XML layouts and CSS styles are loaded from external files; demo app wires them through UIManager.
- CSS parsing: Lexbor-only; fallback code removed; no env/flag switches. Using lxb_css_stylesheet_parse() for stability.
- Selectors: type, #id, .class, attribute selectors, pseudo-classes (:hover, :active, :focus); combinators (descendant, child).
- Properties implemented (subset): margin/padding/gap; border width/color/radius; flex (direction, grow/shrink/basis); width/height/min/max (px and %); colors (named, hex, rgb/rgba, hsl/hsla); text color, font-family/size/line-height with inheritance; background-image + background-size/position/repeat (GUID or project-relative path).
- Background-image paths: url(path) resolves relative to AssetRoot; url(/path) is treated as AssetRoot-anchored; url(asset:my/path) is also supported and treated as AssetRoot-relative.

- Layout: Yoga integration (percentages supported for width/height); positions are content-box relative; root padding verified working.
- Rendering: unified SDF instanced renderer (single draw call) for backgrounds, borders, images, and text; text via Text module (FontAtlas/TextLayout, Slug direct Bezier curve evaluation); clip rects in SSBO.
- Controls: Label, Button, TextField (string), FloatField, IntField, Vector3Field, TextArea, ScrollView, Scrollbar, DockspaceElement, WeightedPane, DockPanel (registered via RegisterBuiltInControls()).
- Diagnostics: noisy triage logs removed by default; optional root padding diagnostics disabled.
- Tests: CSS parser unit tests pass; more coverage planned.

## Next steps (near-term)
- CSS robustness & perf: extend selector coverage (sibling combinators +/~ as needed), verify attribute operator variants; cache match results, optimize dynamic pseudo-state invalidation.
- Cascade/invalidation: refine split of static vs dynamic style data for fast :hover/:active/:focus toggles and class changes.
- Layout polish: overflow/scroll clipping correctness; multi-line text measurement and wrapping (white-space modes), baseline alignment.
- Rendering polish: batching/material sorting review; DPI/HiDPI correctness for scissors; document/align text vs shape coordinate conventions.
- Input & focus: keyboard navigation, IME/text input path, cursor feedback.
- Hot reload UX: debounce and atomic swaps for large stylesheets/layouts; improve error reporting on parse failures.
- Editor integration: persist/restore dock layouts; wire panels through UI XML; resource/inspector panels backed by real data.
- Testing: expand parser/selector/property tests; golden-style tests; layout snapshot tests for regression coverage.



## Asset integration and hot reload
We will introduce two asset types (already anticipated in AssetTypes enum):
- UILayout (.xml) — parsed tree of UI elements
- UIStyle (.css) — parsed stylesheet with rules

Integration:
- Define UISystem/Assets/UIAssets.h with:
  - class UILayoutAsset : public Asset
  - class UIStyleAsset  : public Asset
- AssetManager ParserRegistry: register “.xml” → AssetType::UILayout and “.css” → AssetType::UIStyle
- Hot reload: subscribe to AssetEventDispatcher for AssetLoaded/AssetChanged for these types; on style changes, re-run cascade; on layout changes, rebuild the subtree (initially full rebuild; later, diffing)

## Core runtime types
	- UIManager
	  - Owns root(s), style sheets, and batching/render submission
		  - APIs: LoadLayout(GUID), AttachStyle(GUID), SetHotReloadEnabled(true/false), Update(dt, bool interactive = true), Render(RenderGraph&)
		  - Implementation status: `Update(deltaTime, bool interactive)` now owns time advancement, scheduler/dispatcher processing, input dispatch (keyboard/text/pointer), focus/hover/capture, and Yoga layout. `Render(RenderGraph&)` is geometry-only: it consumes the current element tree and layout/state to build background, border, image, and text geometry and record render passes. The `interactive` parameter on `Update` controls whether a frame is interactive or passive (layout + hover only, no side-effectful input or dockspace changes); `Render` itself has no `passive` flag.
- UIElement (base of all controls)
  - Identity: tagName (e.g., Label, Button), id (from name attribute), classes set
  - Tree: parent, children
  - Style: resolved style (`ResolvedStyle` composed of `LayoutInputs` + `VisualStyle`), inline overrides, and per-element programmatic overrides (`StyleOverrides`)
  - Layout: Yoga node + computed world rects (layout, margin, padding, content)
  - Rendering: virtual `OnGeneratePrimitives(PrimitiveEmitContext& ctx, ...)`
    - Base draws background, border, clipping (if overflow hidden) as SDF primitives
    - Derived elements emit additional primitives (e.g., Label emits Slug text glyphs)
  - Input: hit-testing helpers and event dispatch entry points
  - Dirty flags: StyleDirty, LayoutDirty, VisualDirty, ChildrenDirty
  - Programmatic overrides: `Styles()` for background image overrides and `Overrides()` (typed `StyleOverrides`) for full style overrides with automatic dirty flags
  - API: AddClass/RemoveClass, SetState, MarkDirty(kind), FindById, QuerySelector
- Label (example leaf)
  - Emits Slug glyph primitives via `FontAtlas::ShapeText()` / `TextLayout::ShapeMultiline()`
- Button (example composite)
  - States map to pseudo-classes; raises click/press/release events; focusable

### Programmatic style overrides
- `UIElement::Overrides()` returns a `StyleOverrides&` with typed `Set<T>`/`Get<T>`/`Reset<T>` methods. Dirty flags (layout vs visual) are tracked automatically via `StyleProp<T, StyleImpact>`.
- `UIElement::Styles()` is a narrow helper for background-image overrides and live textures.

## CSS model
- Stylesheet representation
  - Rule: selectors[] + declarations{}
  - Selector AST supports: type, #id, .class, [attr=value], pseudo-classes (:hover, :active, :focus, :disabled), combinators (descendant, child ‘>’, sibling ‘+’, ‘~’ — initially descendant + child)
    - Attribute selectors read authored attributes stored on the element; `id`/`name` match `UIElement::GetId()`.
    - Attribute names are normalized to lowercase and matched case-insensitively.
  - Custom pseudo-classes via registry (e.g., :checked, :selected, :hot, engine-defined)
- Cascade and specificity
  - Compute specificity (ids, classes/attrs, types); stable sort by source order; cache computed style per element
  - Split static (from CSS) vs dynamic (pseudo-state + inline overrides) to allow fast toggling and hot reload
- Properties
  - Layout: display, flex-direction, flex-wrap, flex, flex-grow/shrink/basis, align-items, align-self, justify-content, gap, width/height/min/max, margin/padding/border
  - Visual: background-color/gradient, background-image (later), border-color/width/radius (per-edge/corner), opacity, box-shadow (later), overflow
  - Text: color, font-family/size/weight/style, line-height, white-space (initially single-line clip), text-align
  - Custom properties: `--vars` are stored by `StringId` keys; use `HashStringId()` or the `_sid` literal for code access

## XML model
- Element hierarchy maps directly to UIElement instances
- Attributes:
  - id (or name), class (space-separated), inline style (parsed into inline StyleOverrides), element-specific attributes (Label.text, Button.text)
  - Authored attributes are preserved on UIElement for selector matching and hot reload
- Namespacing: all within <UIElement> … with tag names for derived types (Label, Button)

Example:
```
<UIElement id="MyFrame" class="Window">
	    <Label text="Hello World" />
	    <Button id="MyButton" class="DemoButton" text="Press Here" />
</UIElement>
```

Field controls are exposed as dedicated elements backed by a shared TextInput editor:

```
<UIElement id="Inspector" class="InspectorRoot">
		    <!-- Simple string field backed by TextInput -->
		    <TextField id="NameField" class="InspectorTextField" value="MyObject" />
		
		    <!-- Numeric fields with expression support (e.g., "2+2", "3.5 * 2") -->
		    <FloatField id="PosX" class="InspectorNumeric" value="0.0" />
		    <IntField id="Count" class="InspectorNumeric" value="0" />
	</UIElement>
```

`<textfield>`, `<floatfield>`, `<intfield>`, and `<input>` remain supported as
lowercase/legacy aliases for these controls.

Notes:
- `TextField`, `FloatField`, `IntField` and `Vector3Field` use PascalCase tag
  names in the element tree (matching their class names). Lowercase tags are
  preserved purely as aliases for existing layouts.
- `TextInput` is an internal building block used by these field controls. It is
  not intended to be instantiated directly from XML; instead, use the
  higher-level field elements above.

### Boolean controls: Checkbox and Toggle

Checkbox and Toggle are boolean controls built on a shared `ToggleBase : Field<bool>`.
They expose a single logical value and map it to both:

- a `checked` attribute (used by `:checked` and read by the layout/draw pipeline), and
- a `.checked` class on the element (for simple class-based theming).

Basic XML usage:

```
<UIElement id="SettingsPanel" class="panel">
	    <!-- Initial state can be provided via the checked attribute -->
	    <Checkbox id="MusicEnabled" class="ui-toggle" checked="true" text="Music" />
	    <Toggle   id="Fullscreen"   class="ui-toggle" checked="false" />
</UIElement>
```

Details:

- Canonical tag names are `Checkbox` and `Toggle`; lowercase aliases
  `<checkbox>` and `<toggle>` are also supported.
- When user interaction toggles the value, the control updates:
  - its underlying `Field<bool>` value,
  - the `checked` attribute (set to `"true"` / `"false"`), and
  - a `.checked` class on the element.
- The `:checked` pseudo-class uses `ElementState.checked`, which in turn is
  derived from the `checked` attribute. Attribute truthiness matches the
  existing `:enabled` / `:disabled` handling:
  - empty `checked` or values like `"true"`, `"yes"`, `"on"` are treated as true,
  - values like `"false"`, `"0"`, `"no"`, `"off"` are treated as false.

Example styling for a checkbox-style row:

```css
Checkbox,
checkbox {
	    display: flex;
	    flex-direction: row;
	    align-items: center;
	    gap: 6px;
	    padding: 2px 6px;
	    border-width: 1px;
	    border-radius: 4px;
	    border-color: #333333;
	    background-color: #202020;
	    cursor: pointer;
}

Checkbox:checked,
checkbox:checked,
Checkbox.checked,
checkbox.checked {
	    background-color: #2B4C7E;
	    border-color: #3A8FFF;
}
```

And a simple slider-like toggle:

```css
Toggle,
toggle {
	    width: 34px;
	    height: 18px;
	    border-radius: 9px;
	    border-width: 1px;
	    border-color: #555555;
	    background-color: #333333;
	    cursor: pointer;
}

Toggle:checked,
toggle:checked,
Toggle.checked,
toggle.checked {
	    background-color: #3A8FFF;
	    border-color: #3A8FFF;
}
```

These examples rely purely on background/border styling; more advanced visuals
can be built by composing additional child elements.

### Dropdown control (UI and OS modes)

The `Dropdown` control is a string-valued field (`Field<std::string>`) that
supports two presentation modes:

- **UI mode** (default): a pure-UI popup list built from child elements.
- **Native/OS mode**: delegates menu display to a host-provided callback
  (intended for editor/desktop environments).

XML skeleton (UI-mode):

```
<Dropdown id="QualityDropdown" class="ui-dropdown" />
```

Options are typically supplied from code via the field API:

```cpp
// After layout has been loaded
if (UIElement* root = ui->GetRootElement())
{
	    if (auto* el = root->FindById("QualityDropdown"))
	    {
		        if (auto* dd = dynamic_cast<Dropdown*>(el))
		        {
			            dd->SetOptionsFromString("Low, Medium, High");
		        }
	    }
}
```

The control will create an internal header label (class `dropdown-header`) and
an items container (class `dropdown-items`) that holds one `Label` per option
with class `dropdown-item`.

Example CSS for a simple popup list:

```css
.dropdown {
	    display: flex;
	    flex-direction: column;
	    min-width: 120px;
}

.dropdown-header {
	    padding: 4px 8px;
	    border-radius: 4px;
	    border-width: 1px;
	    border-color: #444444;
	    background-color: #252525;
	    cursor: pointer;
}

.dropdown-items {
	    display: none;
	    margin-top: 2px;
	    border-radius: 4px;
	    border-width: 1px;
	    border-color: #444444;
	    background-color: #202020;
}

/* Header click toggles the "open" class on the root dropdown element */
.dropdown.open .dropdown-items {
	    display: flex;
	    flex-direction: column;
}

.dropdown-item {
	    padding: 4px 8px;
}

.dropdown-item:hover {
	    background-color: #303030;
}
```

Mode selection is controlled via the `mode` attribute:

- `mode="ui"` (default) – always uses the pure-UI popup list above.
- `mode="os"` or `mode="native"` – requests OS-hosted behavior. The host
  environment (e.g., the editor) can install a native menu invoker via
  `Dropdown::SetNativeMenuInvoker`. When no invoker is present, the control
  gracefully falls back to UI mode.

Example combining Checkbox, Toggle, and both Dropdown modes in a single
settings-like panel:

```xml
<UIElement id="UiControlsDemo" class="panel ui-demo-panel">
	    <Label    id="UiDemoHeader"   class="ui-demo-header" text="UI Controls Demo" />
	    <Checkbox id="UiDemoCheckbox" class="ui-toggle"       checked="true"  text="Enable debug flag" />
	    <UIElement class="ui-demo-row">
		        <Label  class="ui-demo-label" text="Toggle sample" />
		        <Toggle id="UiDemoToggle"     class="ui-demo-toggle" checked="false" />
	    </UIElement>
	    <Dropdown id="UiDemoDropdownUi"     class="ui-dropdown"        />
	    <Dropdown id="UiDemoDropdownNative" class="ui-dropdown-native" mode="os" />
</UIElement>
```

The host can then populate options and, for the native dropdown, install a
`NativeMenuInvoker` that maps options to an OS menu.


## Layout (Flexbox via Yoga)
- Each UIElement owns a Yoga node
- Map CSS properties to Yoga styles during cascade application
- After marking LayoutDirty, perform layout pass from root: Yoga::CalculateLayout()
- Store resulting rects on elements: worldLayout, paddingRect, contentRect
- Relayout triggers VisualDirty if any element’s rect changed (to rebuild geometry)

### Display modes and flex-direction defaults

The engine exposes a small `DisplayMode` enum (`None`, `Block`, `Inline`, `Flex`) that
controls how elements participate in layout. All non-`None` modes map to Yoga’s flex
layout, but we layer HTML- and CSS-like semantics on top via `ResolvedStyle`:

- **Block / Inline**
  - When no explicit `flex-direction` has been set on the resolved style
    (`hasFlexDirection == false`), these modes behave like HTML blocks and stack
    children **vertically** (column main axis) by default.
- **Flex**
  - When `display: flex` is set in CSS but `flex-direction` is omitted, the effective
    main axis defaults to **row**, matching standard CSS flexbox semantics.
- **Explicit direction wins**
  - Any explicit `flex-direction` assignment (from CSS or code) sets both
    `flexDirection` and `hasFlexDirection` on `ResolvedStyle`. In this case the
    chosen direction is respected regardless of `DisplayMode`.

This policy gives us HTML-like vertical stacking for default containers while keeping
`display:flex` behavior aligned with the web platform, and is enforced in the
Yoga bridge layer when translating `ResolvedStyle` to Yoga node styles.

## Rendering
- Unified SDF instanced renderer: one pipeline, one SSBO, one draw call.
- Each visible element emits `UIPrimitive` structs (128 bytes, `alignas(16)`) into a flat buffer via `OnGeneratePrimitives(PrimitiveEmitContext& ctx, ...)`.
  - `PrimitiveMode::Rect` for backgrounds, borders, shadows, glow
  - `PrimitiveMode::Slug` for text glyphs
  - `PrimitiveMode::Textured` for background images
  - `PrimitiveMode::Line` for separators and underlines
- Clip rects are stored in a separate SSBO; each primitive references its clip by index.
- GPU draw: `Draw(6, primitiveCount)` — instanced fullscreen quads, fragment shader evaluates SDF per-pixel.
- Textures: bindless descriptor array from `UITextureRegistry` holds all font atlas pages and background images.

## Events and input
- Event phases: Capture → Target → Bubble
- Event types: pointer (enter/leave/move/down/up/click/wheel), keyboard (down/up/char), focus/blur, custom
- Hit-testing by traversing from root, z-order = document order; respect clip regions
- State mapping: pointer over → :hover, pressed on target → :active, focus → :focus
- stopPropagation(), preventDefault() semantics for UI controls

## Dirtiness and incremental work
- StyleDirty → recascade (element + descendants if inheritance used)
- LayoutDirty → request layout pass at root or nearest layout scope; shortcut reflow to affected subtree when possible
- VisualDirty → rebuild geometry for the element (and children if necessary)
- Coalesce updates within a frame; debounce asset hot reload to avoid thrashing

## Hot reload behavior
- UIStyleAsset change → invalidate style cache, re-run cascade; preserve dynamic class toggles and pseudo-state
- UILayoutAsset change → rebuild tree for that asset root (initial), later: DOM diff
- AssetManager event hooks in UIManager

## Materials and extensibility
- Default UI material (solid color/texture) via the SDF shader pair (ui_sdf.vert/ui_sdf.frag)
- Allow per-element material override (future): style property `material: name`, or API setter
- Custom renderers: elements may push custom draw ops with a material id and user data

## File layout
- Engine/Modules/UI/Include/UI/
  - UIManager.h, UIElement.h, UIStyle.h, ResolvedStyle.h, StyleOverrides.h, StyleProp.h, StyleProperties.h
  - UIPrimitive.h, UIFrameBufferRing.h, UITextureRegistry.h
  - Assets/UILayoutAsset.h, Assets/UIStyleAsset.h
- Engine/Modules/UI/Source/
  - UIManager.cpp, UIManager_Update.cpp, UIManager_RenderPrep.cpp, UIManager_PrimitiveGen.cpp
  - UIManager_StyleResolve.cpp, UIManager_Layout.cpp, UIManager_Fonts.cpp, UIManager_Assets.cpp
  - UIManager_Input.cpp, UIManager_Internal.h
  - Controls/ (30+ control implementations)
- Engine/Modules/UI/UISystemDesign.md (this doc)

## Milestones
1) Approve dependencies (pugixml, lexbor or libcss, yoga); add to vcpkg + CMake toolchain
2) Create module skeleton: headers, CMake, minimal UIManager/UIElement with dirtiness + basic rendering of a solid rect
3) Implement UILayoutAsset + UIStyleAsset; register in ParserRegistry; wire Asset events → UIManager hot reload
4) XML loader: construct element tree; CSS loader: parse rules; cascade + computed style for a subset (id, class, tag)
5) Yoga integration: map flex properties; store computed rects; relayout on changes
6) Base rendering: background/border/rounded corners via ShapeBuilder; default material
7) Text Label: integrate Text::FontAtlas/ShapeText; resolve text-related styles
8) Input + events: hit-testing, hover/active/focus, keyboard focus; event bubbling/capturing
9) Batching + clipping: per-material buckets, scissors from clip regions
10) Hot reload polish + performance passes; add custom pseudo-classes and selectors as needed


## Right-to-Left (RTL) Support

Status: Enabled (opt-in); default UI remains LTR. No controls switched by default.

- Engine support
  - CSS `direction` is parsed and mapped to Yoga’s `YGDirection` (LTR/RTL/Inherit).
  - In RTL, flex “start/end” semantics mirror automatically; gaps work the same.
- Theme hooks (opt-in via `[dir="rtl"]`)
  - Scope mirroring: `[dir="rtl"] .grid { direction: rtl; }`, `[dir="rtl"] .tree { direction: rtl; }`
  - Example left/right overrides when needed: `[dir="rtl"] .tree .tree-item .tree-title { background-position: right center; padding-right: 18px; padding-left: 0; }`
  - Keep AssetPanel LTR even under global RTL: `.assets, .assets .grid, .assets .tree { direction: ltr; }`
- Usage policy (initial)
  - Default is LTR. Apply `dir="rtl"` on a container to mirror that subtree only.
  - Favor logical alignment (start/end). Until logical properties are fully supported, use `[dir]`-scoped overrides for left/right paddings, margins, and icon positions.
- Testing (initial plan)
  - Parser: unit test that `direction: rtl` sets `ResolvedStyle.layout.direction`.
  - Layout: snapshot tests comparing LTR vs RTL for row/column flex containers (justify/align start/end; gap spacing).
  - Integration: a theme-level test where a root `[dir="rtl"]` mirrors GridView/TreeView, but the AssetPanel subtree remains LTR.
- Gap vs row-gap/column-gap
  - When both axes use the same spacing, `gap` is equivalent to `row-gap`+`column-gap`. Prefer `gap` for brevity; use per-axis props when asymmetric spacing is desired.
- Future work
  - Add CSS logical properties (margin-inline-*, padding-inline-*), `text-align: start/end`, and validate keyboard navigation in RTL. Expand bidi text coverage in Text module.

## Open questions
- CSS engine choice: Resolved — Lexbor only (required).
- Material system hooks: how to expose engine materials to UI safely (name registry vs handles)
- Theming & variables: Resolved — CSS custom properties (`--vars`) are implemented and keyed by `StringId`.



## Flex item ordering (`order`)

Status: Implemented via stable child sorting before building the Yoga tree.

- Rationale: Upstream Yoga does not expose `YGNodeStyleSetOrder`. We emulate CSS semantics by sorting each element’s direct children by `order` prior to calling `YGNodeInsertChild`.
- Stability and DOM order:
  - Sorting is stable with original DOM index as the tiebreaker.
  - Unsetting `order` (default 0) restores original DOM order automatically.
- Scope: Ordering applies per flex container (not global). Nested containers each sort their own children independently.
- Wrapping: With `flex-wrap: wrap`, ordering is evaluated across all children, then Yoga lays them into rows/columns. Equal-order stability ensures predictable placement across lines.
- Direction/axes:
  - Row vs Column: `order` determines sequence; the main-axis (X for row, Y for column) changes accordingly.
  - RTL: Only mirrors main-axis positions; the item sequence from `order` is unchanged.
- Tests (unit + UI-level):
  - Stable reordering and restore-on-unset
  - Order respected under RTL (positions mirrored)
  - Wrapping + order across multiple lines
  - Nested containers (independent sorts per level)
  - Column direction (vertical reordering)

## Mount element (portal)

Status: Implemented. `Mount` displays a non-owned UIElement target inside its own subtree.

- Ownership: `Mount` holds a raw pointer to the target; it never deletes it. The creator retains ownership and lifetime management.
- Yoga integration: After inserting normal children (already sorted by `order`), `Mount` appends its target as the last Yoga child so it participates in layout and styling.
- Styling: The mounted target receives full computed style based on its own id/classes/selectors, independent of the `Mount` container’s identity.
- Rendering: `Mount` renders its own background/border first; the mounted subtree is rendered by the UIManager’s traversal of the Yoga tree.
- Events & hit-testing: Input hit-testing and event routing work across the `Mount` portal. Capture prefers storing a pointer to the element to work across portals.
- Tests (UI-level):
  - Mounted element receives style and is laid out as expected
  - Click events bubble to the mounted target

## Notes and limitations
- The `order` behavior is implemented in our bridge code, not Yoga. Future Yoga updates won’t affect it unless their child order assumptions change.
- `Mount` requires the target to outlive the UI frame(s) that reference it. Callers must ensure target lifetime and clear the target before destroying it.
- CSS `word-break` is not implemented yet in the parser/style system; add support when needed.


- Spacing rounding (tests): For `justify-content: space-evenly` and `align-content: space-evenly`, Yoga may produce integer-rounded positions (e.g., {33, 167} vs analytical {33.333, 166.667}). Our tests use a relaxed tolerance (~1.0 px) for these cases to avoid false negatives.


## Dispatcher/Scheduler routing via TLS UiContext (and diagnostics)

Status: Enabled. The UI runtime now routes immediate defers (PostAction) and scheduling via a per-thread TLS UiContext that is bound by UIManager entry points and select Editor integration sites.

- TLS context
  - UiContextScope(dispatcher, scheduler) temporarily binds per-thread pointers; nested scopes restore the previous value on destruction.
  - UIManager binds UiContextScope in Render(), input handlers (OnKey/OnChar/OnMouseMove/OnMouseButton/OnScroll), and asset APIs (LoadLayout*/AttachStyle*).
  - Rationale: avoids global dispatcher, supports multiple UIManager instances on the same thread without cross-window misrouting.

- Ownership propagation
  - UIManager::SetRoot() calls root->SetOwnerManager(this) and ownership propagates down the subtree.
  - UIElement::AddChild() propagates owner from parent to newly attached children.
  - Programmatic element creation: create freely; owner is assigned when attached (AddChild/SetRoot) and then PostAction will route correctly.

- PostAction fallback and diagnostics
  - PostAction first tries TLS dispatcher, then owner’s dispatcher; an element with neither drops the action and returns false (logged once), never runs it inline.
  - PostAction is callable from any thread. The owner's dispatcher is reached through the element's post route (UI::UiPostTarget), which the UI thread updates on every owner change and which holds the dispatcher by shared ownership. ~UIManager closes its dispatcher, so a post from a worker thread to an element whose manager has been destroyed returns false instead of touching freed memory.

- Editor integration note
  - DockspaceElement::RebuildFromModel() runs outside UIManager; Editor binds a UiContextScope around those calls only.
  - Initialization order: AssetsPanel::SetContext() is invoked after the docking model is bound so Grid/Tree are already attached to a UIManager before any PostAction.
