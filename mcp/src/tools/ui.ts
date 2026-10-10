import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { proxyTool } from "./proxy.js";

// Mirrors kDefaultUITreeMaxTextLength (Apps/Editor/Source/DebugServer/UITreeDump.h). The editor
// applies its own default when the parameter is absent; this keeps the two tools that advertise it
// from drifting apart from each other.
const kDefaultMaxTextLength = 512;
const kMaxTextLengthHelp =
  `Cut text values longer than this (default ${kDefaultMaxTextLength}; 0 = whole text). A cut node ` +
  `carries textTruncated:true, so a substring you do not find is genuinely absent unless that flag is set`;

export const tools: ToolDef[] = [
  proxyTool({
    name: "get_ui_tree",
    category: "ui",
    description:
      "Get the editor UI element tree for debugging the editor interface. Two defaults cut the walk: maxDepth (default 5), far above most panel content — the scene-view toolbar sits below depth 12 — and includeHidden (default false), which drops hidden subtrees entirely. So a dump that does not contain an id does NOT mean the id is absent. Read searchIsComplete in the reply: absence only means something when it is true, and completenessNote says which limit applied. To look one id up regardless of depth, pass it as rootId, which searches the whole tree.",
    schema: {
      maxDepth: z.coerce.number().optional().describe("Max tree depth (default 5). Raise it, or use rootId, before concluding an element is missing"),
      rootId: z.string().optional().describe("Start from this element ID. Found by an unbounded search, so this locates elements at any depth"),
      includeLayout: z.coerce.boolean().optional().describe("Include x/y/w/h layout data (default true)"),
      includeHidden: z.coerce.boolean().optional().describe("Include hidden (display:none) elements, marked with hidden:true (default false)"),
      maxTextLength: z.coerce.number().optional().describe(kMaxTextLengthHelp),
    },
    params: ({ maxDepth, rootId, includeLayout, includeHidden, maxTextLength }) => ({
      maxDepth: maxDepth ?? 5,
      rootId,
      includeLayout: includeLayout ?? true,
      includeHidden: includeHidden ?? false,
      maxTextLength: maxTextLength ?? kDefaultMaxTextLength,
    }),
  }),

  proxyTool({
    name: "get_panel_tree",
    category: "ui",
    description:
      "Get a docked panel's UI subtree by panel name (e.g. 'Hierarchy', 'Inspector', 'Assets'). Unlike get_ui_tree with rootId, this resolves the actual rendered panel content regardless of its position in the dock layout. Same completeness contract as get_ui_tree: the reply carries searchIsComplete, and absence of an id means nothing unless it is true — this walk is cut both by maxDepth and by the default that omits hidden subtrees.",
    schema: {
      panelId: z.string().describe("Panel name (e.g. 'Hierarchy', 'Inspector', 'Assets', 'SceneView')"),
      maxDepth: z.coerce.number().optional().describe("Max tree depth (default 5). Raise it before concluding an element is missing"),
      includeLayout: z.coerce.boolean().optional().describe("Include x/y/w/h layout data (default true)"),
      includeHidden: z.coerce.boolean().optional().describe("Include hidden (display:none) elements, marked with hidden:true (default false)"),
      maxTextLength: z.coerce.number().optional().describe(kMaxTextLengthHelp),
    },
    params: ({ panelId, maxDepth, includeLayout, includeHidden, maxTextLength }) => ({
      panelId,
      maxDepth: maxDepth ?? 5,
      includeLayout: includeLayout ?? true,
      includeHidden: includeHidden ?? false,
      maxTextLength: maxTextLength ?? kDefaultMaxTextLength,
    }),
  }),

  proxyTool({
    name: "click_element",
    category: "ui",
    description:
      "Click a UI element by ID or by UI coordinates. Takes the same route real mouse input does, so in play mode with a Game View focused it also reaches the running game's input (position remapped to viewport-local). 'clicked' only reports that pointer input was injected at the returned coordinates. 'hit' names the element the press resolved onto, as information rather than a verdict: it is sampled after the press, so on a click that changes UI state it is nondeterministic and can come back different or empty for a click that worked. Treat a mismatch as a reason to inspect, never as proof the click missed. An elementId whose layout box has collapsed on both axes (hidden, not yet laid out, or a host whose content is mounted under another id) is refused rather than aimed at its origin.",
    schema: {
      elementId: z.string().optional().describe("Element ID to click. Must have a layout box with extent on at least one axis"),
      x: z.coerce.number().optional().describe("X in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
      y: z.coerce.number().optional().describe("Y in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
      button: z.enum(["left", "right", "middle"]).optional().describe("Mouse button (default left). right opens real context menus — on macOS the popup is deferred past the IPC reply, so this does not stall; on Windows the native popup's nested message loop blocks the debug server until the menu closes, so prefer open_context_menu there"),
    },
  }),

  proxyTool({
    name: "perform_drop",
    category: "ui",
    description:
      "Drop asset files at a point in an editor window, as a drag from the Assets panel would end there. The window's drag-and-drop manager resolves the drop target under the point (the Scene View viewport, the Hierarchy tree, an inspector field), and that target's own CanDrop and PerformDrop run, so it exercises the same code a drop with the mouse does, without a pointer gesture to start the drag. 'accepted' says the target took the payload and its PerformDrop ran; PerformDrop reports no result, so read the effect (get_scene_hierarchy, get_undo_stack, the Assets panel). 'reason' says why the target did not take it; 'target' names the drop target element. Refused before any drop: a path outside the project's asset sources (the Assets panel's drop moves files, so copy an outside file into the project first), a path with no file, and a drag-and-drop session in progress. Moves the editor's pointer to the point (the desktop cursor stays where it is). A drop the target defers until an asset loads returns at once with accepted true; read the result later.",
    schema: {
      paths: z.array(z.string()).min(1).describe("Asset files to drop, absolute or project-relative; each must exist inside one of the project's asset sources"),
      elementId: z.string().optional().describe("Drop at this element's center, e.g. SceneViewViewport. Must have a layout box"),
      x: z.coerce.number().optional().describe("X in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
      y: z.coerce.number().optional().describe("Y in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
      windowIndex: z.coerce.number().optional().describe("Editor window (default 0, the main window)"),
    },
  }),

  proxyTool({
    name: "move_pointer",
    category: "ui",
    description:
      "Move the editor's pointer without clicking and report the resolved hover target, for hover-state testing and screenshots. Only the editor's pointer moves: the desktop cursor stays where it is. Takes the same route real mouse input does, so in play mode with a Game View focused it also drives the running game's mouse (position remapped to viewport-local). An elementId whose layout box has collapsed on both axes (hidden, not yet laid out, or a host whose content is mounted under another id) is refused rather than aimed at its origin.",
    schema: {
      elementId: z.string().optional().describe("Element ID whose center should be hovered. Must have a layout box with extent on at least one axis"),
      x: z.coerce.number().optional().describe("X in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
      y: z.coerce.number().optional().describe("Y in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
    },
  }),

  proxyTool({
    name: "send_key",
    category: "ui",
    description: "Inject a keyboard event into the editor along the path a real key takes: global editor shortcuts first, then the focused element, then a running game's HUD and input, then editor actions. Use it for keyboard-driven behavior like Tab focus traversal, Enter commit and Escape cancel, and for typing into a game HUD while play mode runs. Pair with take_screenshot to observe focus highlights.",
    schema: {
      key: z.union([z.string(), z.number()]).describe("Key name (Tab, Enter, Return, NumpadEnter, Escape, Space, Backspace, Delete, Home, End, Up, Down, Left, Right, A-Z, 0-9) or a raw GLFW keycode integer"),
      mods: z.union([z.string(), z.array(z.string())]).optional().describe("Modifier(s): shift | control | alt | super (string or array). Shift+Tab tabs backwards."),
      action: z.enum(["tap", "press", "release", "repeat"]).optional().describe("tap (default: press + release next frame), press, release, or repeat (OS key-repeat edge, e.g. held Tab)"),
      count: z.coerce.number().optional().describe("tap only: number of taps to issue (e.g. Tab x3). Default 1."),
    },
  }),

  proxyTool({
    name: "input_text",
    category: "ui",
    description: "Type text into the focused editor field through the path real typing takes: one character event per codepoint, so the field's per-keystroke callbacks run as they do for a user. Pass elementId to focus a field first (the same focus a click gives it). Refused before anything is typed: text that is not valid UTF-8, or that holds a control character (use commit for Enter and send_key for Tab or Escape), and a call with no field focused. The keystrokes are applied on the editor's next frame.",
    schema: {
      text: z.string().describe("The text to type (UTF-8, no control characters)"),
      elementId: z.string().optional().describe("Element to focus before typing; omitted, the text goes to the focused element"),
      clear: z.coerce.boolean().optional().describe("Select the field's content first so the text replaces it (default false)"),
      commit: z.coerce.boolean().optional().describe("Press Enter after the text, which commits numeric fields and runs Enter-bound actions (default false)"),
      windowIndex: z.coerce.number().optional().describe("Editor window (default 0, the main window)"),
    },
  }),
];
