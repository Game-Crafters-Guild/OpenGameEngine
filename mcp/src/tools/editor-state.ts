import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { proxyTool } from "./proxy.js";

export const tools: ToolDef[] = [
  proxyTool({
    name: "get_build_status",
    category: "editor",
    description: "Get the current game build generation, running/completed state, success receipt and diagnostics. Poll after trigger_build and require the same generation with running=false and succeeded=true before publishing its output.",
    schema: {},
  }),
  proxyTool({
    name: "get_editor_state",
    category: "editor",
    description: "Get current editor state: play mode, scene, selected entity, entity count, plus `session` provenance — the worktree this editor was built from, the branch that tree was on AT LAUNCH, the build config, and any --session-label its launcher gave it. Use `session` to answer 'whose editor is this, and may I touch it?' in one call instead of walking the process table. `session.branchAtLaunch` is what this binary was BUILT from, not what the tree is checked out to now — `session.worktreePath` is the identity that cannot drift. CHECK `sceneLoadError` BEFORE TRUSTING `scenePath` OR `entityCount`: it is null on a healthy editor and an object when the last scene open failed. With `worldCleared` true the entities in the world are a PARTIAL LOAD of `sceneLoadError.path` — not a loaded scene, and not the scene that was open before — and `scenePath` is empty precisely because there is no loaded document left to name. SEPARATELY, `sceneDegraded` is non-null when the scene DID load but this build could not apply every assignment in the file (an enum value, field or component type from a newer build, or a value whose syntax this parser cannot read). Unlike `sceneLoadError`, `scenePath` and `entityCount` ARE trustworthy then — the world really is that scene, minus `sceneDegraded.skips`. It stays non-null for as long as the LOAD-TIME record stands — the same condition the window title's (PARTIAL LOAD) decoration keys on, so the two surfaces cannot disagree — and carries BOTH counts, which answer different questions. `loadTimeCount` is what that load could not apply. `outstandingCount` is what a SAVE still has to answer for, and it drops as overrides retire: the user discarding a preserved value in the Inspector, the component being removed, the entity being destroyed. `droppedCount` is the subset a save would LOSE (the rest round-trips verbatim). `skips` lists ONLY the outstanding rows, so a discarded override is never reported as text a save will write back; each carries entity/component/field/message/line/preserved. save_scene refuses to overwrite such a scene's own file without force=true only while `outstandingCount` is above zero. Unknown fields are null, never guessed. FINALLY, `pendingMaterialTextureBinds` is content readiness the pixels cannot report: 0 on a settled frame, non-zero while that many material texture slots are still waiting on a decode and are rendering bindless defaults (white albedo, flat normal) in place of their authored textures. A frame captured while it is non-zero looks exactly like a material that never binds, and a texture's FIRST material bind also tags its cook usage — so on a cold derived cache the window lasts as long as the block-compression encode, seconds to minutes rather than frames. POLL IT TO ZERO BEFORE MEASURING OR A/BING ANYTHING VISUAL. `pendingUiBackgroundImages` is the same question for UI chrome, which the material counter cannot answer: non-zero means that many background images (icons, panel art) have a load or upload in flight and are drawing nothing where they will draw once it lands. POLL IT TO ZERO TOO before capturing editor UI. An image that fails outright does not sit in this count — it logs 'completed with no texture; it stays blank' and stops.",
    schema: {},
  }),

  proxyTool({
    name: "get_log",
    category: "editor",
    description: "Get the most recent engine log messages (the tail). Returns the newest `count` messages, oldest-first within that window — so a small count shows what just happened.",
    schema: {
      count: z.coerce.number().optional().describe("Max messages to return, taken from the newest end (default 100)"),
      minLevel: z.string().optional().describe("Minimum level: trace/debug/info/warning/error"),
      filter: z.string().optional().describe("Substring filter — only return messages containing this text"),
      offset: z.coerce.number().optional().describe("Skip this many of the newest matching messages before returning (page further back into history; default 0)"),
    },
    params: ({ count, minLevel, filter, offset }) => ({
      count: count ?? 100,
      minLevel: minLevel ?? "info",
      filter: filter ?? "",
      offset: offset ?? 0,
    }),
  }),

  proxyTool({
    name: "get_scene_hierarchy",
    category: "editor",
    description: "Get the entity hierarchy tree of the current scene. Each node carries id, name, children, enabled (the entity's own switch) and enabledInHierarchy (false when the entity or any ancestor is off, one update after the switch).",
    schema: {},
  }),

  proxyTool({
    name: "trigger_build",
    category: "editor",
    description: "Trigger a game build from the Editor's Build panel for a single platform (e.g. \"Mac\", \"Windows\"). The build runs asynchronously on the editor UI thread; poll get_log (filter \"Build:\") to follow progress and find the produced artifact path.",
    schema: {
      platform: z.string().describe("Target platform to build: Mac, Windows, Linux, Steam, Nintendo, Sony"),
    },
  }),

  proxyTool({
    name: "get_build_settings",
    category: "editor",
    description: "Get the Editor's current build settings: workspace/asset roots, SDK path + lib presence, per-platform scenes/icon/config, and the resolved output directories. Use to confirm the SDK is staged and the project is ready before trigger_build.",
    schema: {},
  }),

  proxyTool({
    name: "execute_command",
    category: "editor",
    description: "Execute a named editor command (create_cube, create_terrain, etc.) or by numeric command ID. Play mode is not reachable here — use set_play_mode, which reports the resulting state and refuses an illegal transition.",
    schema: {
      name: z.string().optional().describe("Command name: create_empty, create_cube, create_sphere, create_capsule, create_plane, create_camera, create_light_dir, create_light_point, create_light_spot, create_light_ambient, assets_view_grid, assets_view_list"),
      commandId: z.coerce.number().optional().describe("Numeric command ID (alternative to name)"),
    },
    // commandId wins when both are given — the numeric id is unambiguous.
    params: ({ name, commandId }) => {
      const params: Record<string, unknown> = {};
      if (commandId !== undefined) params.commandId = commandId;
      else if (name !== undefined) params.name = name;
      return params;
    },
  }),

  proxyTool({
    name: "set_play_mode",
    category: "editor",
    description:
      "Enter, exit, pause or resume the editor's play mode, through the same actions the toolbar Play/Pause/Stop buttons drive — and pass the optional activateGameView flag if your session touches game UI, because without it no UI document mounts and every managed lookup reads dead. " +
      "Each action is legal from exactly one state and is refused with an error otherwise (entering while already playing, exiting while already editing), " +
      "and the response always reports the resulting 'playMode' so a caller never has to trust its own intent — the same vocabulary get_editor_state uses. " +
      "SCENE STATE: entering snapshots the ECS world; exiting restores that snapshot, so everything the running game changed in the ECS world is DISCARDED. Terrain reverts zone payloads only (sculpt/heightmap stores are untouched), physics is reset rather than restored, and audio voices are stopped. Nothing is ever saved on your behalf — save first if the pre-play edits matter. " +
      "Exiting can land in 'change_review' rather than 'editing' when editor commands (not game code) ran during play: that state holds those edits pending, and a second 'exit' discards them, exactly as the toolbar Stop button does. " +
      "This never switches the editor to the fullscreen game layout, so the dock layout you set up survives a play session (activateGameView changes which tab is in front, nothing else), " +
      "and it skips the toolbar's pre-play pending-download prompt — playing proceeds even if assets are still downloading. " +
      "IF YOUR SESSION TOUCHES GAME UI, PASS activateGameView — read its description first; without it managed UI resolves nothing and looks broken.",
    schema: {
      action: z.enum(["enter", "exit", "pause", "resume"]).describe(
        "enter: editing -> playing. exit: playing/paused -> editing (or change_review). pause: playing -> paused. resume: paused -> playing. There is no frame-step action in the editor."),
      activateGameView: z.boolean().optional().describe(
        "Bring the Game View tab to the front (and give its viewport UI focus) BEFORE the transition. Default false, which leaves your dock layout untouched. " +
        "PASS true FOR ANY SESSION THAT TOUCHES GAME UI. The Game View's composite is the only thing that publishes the gameplay UI host, so while another tab is in front NO UI DOCUMENT IS MOUNTED: C# Ui.FindElement returns null and [UiElement] fields never bind. " +
        "That failure is indistinguishable from a broken scripting ABI, and the Scene View's HUD preview keeps drawing the HUD while it happens, so the screenshot looks right while every probe reads dead. " +
        "The toolbar Play button only raises the Game View when the toolbar's play-fullscreen toggle is on (off by default), so it is not a workaround. " +
        "If the main editor window is unavailable the transition is refused outright rather than entering play with the caller believing the view is up. " +
        "Otherwise read the result rather than assuming it: the response carries 'gameViewActive' (read back from the dock model — this is the one that governs whether UI mounts) and 'gameViewViewportFocused' (input routing only; it can be false on the first call after the panel was closed, because the viewport element is not mounted the same frame — call again if you need input to reach the game)."),
    },
  }),

  proxyTool({
    name: "focus_view",
    category: "editor",
    description:
      "Bring the Scene View or Game View tab to the front of its dock group in the main editor window and give that view the input focus it expects. " +
      "Use it before a screenshot to choose which view is captured, or before send_key/simulate input to choose which view receives it. " +
      "The two views need opposite focus handling and this applies the right one: the Game View's viewport takes UI focus so input reaches the running game, " +
      "while the Scene View's must have UI focus cleared or UIManager would swallow its W/A/S/D camera keys. " +
      "Reports 'active', 'viewportFocused' and 'focusedElementId' read back from the dock model and UI manager after the change, not assumed from the request. " +
      "If the panel was closed it is re-docked at its default placement — its viewport may not be mounted the same frame, so if 'viewportFocused' comes back false, call again.",
    schema: {
      view: z.enum(["scene", "game"]).describe("Which viewport panel to bring to the front and focus."),
    },
  }),
];
