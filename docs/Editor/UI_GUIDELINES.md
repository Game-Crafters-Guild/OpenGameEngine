# Editor UI Guidelines

These guidelines describe how to author new editor UI against the current native UI system and theme. They are intentionally practical: prefer existing controls, classes, tokens, and panel patterns before introducing new visual language.

## Design Foundations

Build editor UI as dense, predictable tooling. The editor is an operational workspace, so prioritize scanability, stable layout, and fast repeated action over decorative composition.

- Use existing registered controls and CSS classes before adding a new component.
- Use global tokens from `Apps/Editor/Assets/UI/theme/tokens.css` for shared color, type, spacing, radius, icon tint, and tree sizing.
- Use panel-local CSS only for layout or behavior that is genuinely specific to that panel.
- Avoid nested card-like containers. Use unframed full-width sections, rows, toolbars, scroll areas, repeated items, or modals.
- Keep text inside controls short. Buttons and toolbar controls should not grow or shift between states.
- Prefer icons for compact tool actions and text buttons for explicit commands.

## Tokens

Current token groups:

- Colors:
  - Backgrounds: `--ui_color_bg_root`, `--ui_color_bg_panel`, `--ui_color_bg_tabbar`, `--ui_color_bg_toolbar`, `--ui_color_bg_grid`, `--ui_color_bg_hover`, `--ui_color_bg_selected`.
  - Borders: `--ui_color_border`, `--ui_color_border_subtle`, `--ui_color_border_panel`.
  - Text: `--ui_color_text`, `--ui_color_text_muted`, `--ui_color_text_bright`, `--ui_color_text_dim`.
  - Accents: `--ui_color_accent`, `--ui_color_accent_blue`, `--ui_color_accent_blue_hover`, `--ui_color_accent_blue_pressed`, `--ui_color_accent_green`.
- Status colors not yet formalized as shared tokens:
  - Warning/amber text: `#dcdcaa`, used by log warnings and Script Errors warnings.
  - Profiler orange: `#f08040`, used by Visual Profiler GPU-bound labels and chart borders.
  - VCS warning orange/yellow: `#FFAA00`.
  - Animation yellow tags: `#f0c850`.
- Typography:
  - `--ui_font_family`, `--ui_font_family_default`, `--ui_font_mono`, `--ui_font_script`.
  - `--ui_font_size_base`, `--ui_font_size_list`, `--ui_font_size_small`, `--ui_font_size_header`.
- Spacing:
  - `--ui_spacing_xs`, `--ui_spacing_sm`, `--ui_spacing_md`, `--ui_spacing_lg`, `--ui_spacing_xl`.
- Radius:
  - `--ui_radius_sm`, `--ui_radius_md`.
- Icon tint:
  - `--ui_icon_tint_default`, `--ui_icon_tint_active`, `--ui_icon_tint_hover`.
- Tree sizing:
  - `--ui_tree_icon_size`, `--ui_tree_foldout_size`, `--ui_tree_icon_padding`, `--ui_tree_icon_padding_rtl`.

When a value is shared across panels, add or reuse a token. When a value is local to one panel, keep it in that panel stylesheet.

## Layout Rules

- Every flexible panel root should use `min-width: 0` and `min-height: 0` so children can shrink and scroll correctly.
- Scrollable content needs a constrained parent. Do not rely on content height to create scroll ranges.
- Virtualized grids must keep `.grid` as a single-column flex container; the internal virtual content owns cell placement.
- Split panes should use `WeightedPane` plus `Splitter`; splitter drags must mark layout dirty and keep adjacent panes shrink-safe.
- Mounted panel subtrees should be treated like portal content: style, focus, hit testing, and lifecycle must work through the `Mount` host.
- Toolbar rows should have fixed height and stable icon hit areas.
- Inspector rows should use a two-part pattern: label cell plus field cell. Field cells must be allowed to shrink.
- Fixed-format controls such as icon buttons, numeric cells, vector components, toolbar buttons, and grid thumbnails should have explicit dimensions or min/max constraints.
- Do not place panels, scroll views, or tool surfaces inside decorative cards. Use borders/backgrounds only where they communicate a real boundary.

## Styling Rules

- Prefer classes and theme tokens over inline style overrides.
- Use `StyleOverrides` from C++ for dynamic values, measured sizes, live textures, or state that cannot be expressed cleanly in CSS.
- Use `RequestSubtreeStyleAssetPath("UI/controls/<Control>.css")` when a native control owns reusable child structure and local styling.
- Put reusable control styles in `Apps/Editor/Assets/UI/controls`.
- Put global theme-wide editor styles in `Apps/Editor/Assets/UI/theme`.
- Put panel-specific styles in `Apps/Editor/Assets/UI/panels`.
- Use `hidden` for complete removal from layout. The theme intentionally raises specificity with `.hidden.hidden`.
- Use `background-image-tint` for icon color states instead of opacity when the hit area/background must remain visually stable.
- Keep radii small; current editor tokens provide 4 px and 6 px radii.
- Use orange/amber only for warning, caution, validation-in-progress, profiler GPU distinction, or existing category chips. It should not replace blue as the default selection/focus accent.

## Naming

Use predictable CSS naming so classes can be found from C++ and theme files.

- Reusable controls: `<component>`, `<component>-<part>`, `<component>-<state>`.
- Inspector UI: `inspector-*`.
- Toolbar UI: `toolbar-*`, `editor-top-toolbar`, `editor-bottom-toolbar`, or panel-local toolbar names.
- Dock/layout UI: `dock-*`, `Dock*` UXML config nodes, and panel ids that match the panel inventory.
- Panel-specific classes: prefix with the panel or feature name, such as `render-graph-*`, `vram-*`, `node-graph-*`, or `script-errors-*`.
- Icon classes should end in `-icon` and map to shipped assets in `Apps/Editor/Assets/Icons`.

Avoid one-off generic names such as `container`, `box`, `row2`, or `new-style` in editor UI assets.

## Behavior Rules

- `UIManager::Update(deltaTime, interactive)` owns time advancement, dispatcher processing, input dispatch, focus, hover, capture, and layout.
- `UIManager::RenderRG(frame, target, targetSpace)` prepares geometry and declares the UI pass for the current frame. It should not dispatch events or mutate editor data. The host supplies the target's color space and executes the completed graph.
- UI event handlers run during the update phase. They may trigger editor actions, but editor data changes should still use the established editor edit APIs.
- Interactive edits should preview during drag/scrub and commit once at the end of the gesture.
- Inspector refresh code must not overwrite focused field subtrees.
- Prefer posting deferred actions when a control needs to mutate tree structure after layout or during a safe dispatcher drain.
- Use existing drag/drop payload and ghost renderer helpers for editor drag behavior.
- Use existing specialized widgets for scene view, animation/timeline, color picking, charts, and logs before creating new primitives.

## Panel Patterns

Docked panels:

- Add the panel to `DockPanels` in `layout.uxml` when it should be available from the dock inventory.
- Use `layout="panels/<Panel>.uxml"` and `style="panels/<Panel>.css"` for data-driven panel structure when practical.
- Keep panel roots flexible and let the dockspace own outer sizing.

Inspector panels:

- Use `InspectorSection` for component sections.
- Use `inspector-row`, `inspector-label`, and `inspector-field` for properties.
- Pair sliders with numeric fields when precision matters.
- Use `AssetField` for asset references instead of rebuilding picker behavior.

Toolbars:

- Use `icon-button` for compact actions and provide tooltip text from C++ where the icon is not self-explanatory.
- Keep top toolbar sections as left, center, and right flex regions.
- Do not let toolbar overflow overlap neighboring sections; clipping is preferred to layout collision.

Modals:

- Use modal-specific root classes and attach the modal's own stylesheet under `UI/controls/` (e.g. `UI/controls/ProjectPicker.css`), with every rule scoped to that root class.
- Keep initial focus predictable and provide clear cancel/confirm actions.
- Modal body content can be richer, but header/footer control positions should stay stable.

## Asset And Theme Loading

- Global editor styles are imported through `Apps/Editor/Assets/UI/theme.css`.
- Subtree control styles are requested by controls and attached under the relevant subtree.
- Panel styles can be declared in the dock panel inventory or attached by panel code.
- UXML `class` attributes may include multiple classes separated by spaces.
- Inline `style` is acceptable for layout configuration or one-off authored examples, but should not become the default way to theme reusable UI.

## New UI Checklist

- Reuse an existing native control or editor helper where one exists.
- Choose classes from the component catalog before adding new CSS.
- Use tokens for shared colors, spacing, type, radii, and icon tint.
- Ensure flexible roots and scroll areas have `min-width: 0` and `min-height: 0`.
- Verify hover, active, focus, selected, disabled, empty, and drag/drop states where applicable.
- Keep control dimensions stable across state changes.
- Make data changes through editor edit APIs and preserve preview/commit semantics.
- Add new reusable styles to `controls/`, panel-only styles to `panels/`, and cross-editor styles to `theme/`.
