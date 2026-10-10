# CLAUDE.md

@AGENTS.md

Guidance beyond the shared entry point imported above. `CONTRIBUTING.md` is the short form of the rules for people; this file wins where they disagree. Area conventions load on demand from `.claude/rules/` when you open matching files. Team workflow content (the review ledger, the test map, evidence checklists, designs, the coordination protocol) lives in the sibling repository `GameEngine-Workbench` (`GE_WORKBENCH`, default `../GameEngine-Workbench`, added with `--add-dir`).

## Engineering values

The bar for every change: high performance, high code quality, clean architecture, high usability. Aim for the best end state for the engine, not the fastest thing that works.

- **Fix causes, not symptoms.** When a bug exposes a design flaw, the design is the bug. Prefer the scoped refactor that leaves the engine simpler; if the right fix is bigger than the ask, say so and propose it.
- **No legacy by default.** There are no users or released projects to preserve. For every API, saved format and removal: update all current callers and committed assets, delete the obsolete path, keep only the supported implementation. No backward compatibility, saved-data migrations, deprecated aliases, fallback readers, or checks whose only purpose is detecting removed legacy data; ordinary validation of supported data stays. Preserve compatibility only when explicitly requested for a concrete consumer.
- **Performance is a feature.** Hot paths get data-oriented designs (SoA, contiguous iteration, flat maps over node-based containers). Performance claims are measured before and after, in Release; "should be neutral" is a hypothesis.
- **Code lives where it belongs.** Before adding to a file, ask whether that file owns the responsibility; extract a controller, service or system with a clear owner instead of appending to whatever file is open. Feature code never lands in a hub (`EditorApplication`, `EditorContext`, `SettingsPanel`, `InspectorPanel`, `HierarchyPanel`, `SceneViewPanel`, `RenderServices`, `UIManager`, `UIElement`, `core.css`); the feature pulls what it needs through the app's API and the hub never learns its name.
- **Register, don't accrete.** Extensible surfaces take entries by registration from the owning module: settings categories through `Editor::EditorSettingsRegistry`, inspectors, post effects, dockables and search items through their registries. Migrate legacy hand-built pages on contact.
- **Know what exists before you write.** A second implementation of anything the engine has is a defect. Every subsystem is a module under `Engine/Modules/` (list the directory); engine services live under `Engine/Include/` and `Engine/Source/`; the editor's shared pieces under `Apps/Editor/Include/`. Before writing math, noise, hashing, a container, a string or time utility, a drawing helper or a UI control, look in `Mathematics`, `Noise`, `Types`, `Foundation`, `Platform`, `UI/Include/UI/Controls`, the gizmo API (`Apps/Editor/Include/SceneView/SceneViewGizmos.h`) and the inspector building blocks (`Apps/Editor/Include/UI/InspectorSection.h`). If the shared piece is missing or lacking, add it there in its own change, then use it.
- **Usability counts as quality.** APIs that are hard to misuse, defaults that work, error messages that state the fix, editor UX that does not fight the user. Few settings with high-quality defaults: a dial whose non-default values are all worse is deleted, not shipped.
- **One engine, many kinds of games.** The engine serves first-person and third-person games, role-playing and open-world games, strategy, flight and space simulation, and their cutscenes at close-up quality. A game built on it is a customer and a source of test content, never the specification: design a system for the range, expose the difference as presets and tuning a game chooses, and do not cap quality or scope at what one game's camera needs. A measurement taken in one game's scene says so and names the scenes that would represent the others.

## Debugging

Get facts from a debugger, a capture or the live editor before theorizing, and confirm the instrument is reachable before reporting what it said. Method: the `root-cause-debugging` skill; layered "is X enabled" questions: `trace-the-chain`.

- Native crashes and hangs: cdb from the Windows SDK Debugging Tools (usually `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\`). Snapshot a running process without stopping it: `cdb -pv -p <pid> -c "~*k; qd"`. Reproduce under the debugger: `cdb -g -G <exe> <args>`, then `!analyze -v`, `~*k`, `.frame N` and `dv /t`. Post-mortem: `cdb -z <dump> -c "!analyze -v; ~*k; q"`. PDBs sit beside the staged binaries. Several editors are often alive: verify PID and path before attaching, and never kill an editor you did not launch.
- Memory bugs: the `vs2026-x64-local-asan` preset, Debug configuration, ASan runtime DLLs staged automatically, `GE_ENABLE_ASAN=1` defined so perf-sensitive tests can relax thresholds.
- GPU: RenderDoc and the Vulkan instruments, in `.claude/rules/rendering.md` and the `renderdoc-gpu-debug` skill.
- Live editor: the MCP tools (`mcp__gameengine__*`, setup in `docs/MCP_SETUP.md`) and the CLI `node mcp/ge.mjs <tool>` (`--help` lists all; `--port` or `GE_EDITOR_DEBUG_PORT` targets a non-default editor; `ge raw <method> '{...}'` reaches methods with no tool). Adding a `RegisterHandler` without exposing it as a tool or listing a reason in `mcp/ipc-method-coverage.json` fails `EditorIpcMethodDrift`. A backgrounded or occluded editor window may render black or throttled; bring it to the foreground before drawing visual conclusions.

## Architecture in one screen

```
Engine/Include, Engine/Source   engine-level services           Apps/Editor    the editor (C++, CSS/XML UI)
Engine/Modules/<Name>           one library per subsystem       Apps/Player    the runtime
Engine/ECSModules               shared ECS components           Managed/       C# (CoreBridge, HotReload, Scripting.ABI, CompileServerHost)
Engine/Tests, Tests/            engine-level and integration    docs/          user documentation only
```

Modules are layered: Foundation (`Logger`, `GenerationalVector`, `Types`), core services (`AssetCore`, `JobSystem`, `Memory`, `Platform`, `FileWatcher`, `Mathematics`), content (`AssetDatabase`, `AssetRuntime`, `Audio`, `Input`, `Text`), world (`ECS`, `Physics`, `PhysicsECS`, `Rendering`, `UI`, `Scheduler`), integration (`Scripting`, `Graph`, the VCS modules). Assets are GUID-identified with pluggable parsers, a dependency graph for targeted hot reload and async streaming. `Engine` is a shared library: the Editor, the Player and `GameEngine.Native.dll` share one `Engine.dll` and its engine singleton; whoever calls `EngineCore::Initialize` first owns the lifecycle (`Engine/Source/Scripting/DllEngineBootstrap.h`).

Convention traps that invert habits from other engines, always called out when in play: left-handed, +Z forward, +Y up; reverse-Z with a D32 depth buffer, clear 0.0 and `GreaterOrEqual`; negative shadow bias; negative viewport height; descriptor indexing and image-layout transitions. The full list is in `.claude/rules/rendering.md`.

## Code style

`CodingStyle.md` is canonical: functions and types PascalCase, private members `m_PascalCase`, public data PascalCase, locals camelCase, constants `kPascalCase`, full words, `enum class`, fixed-width integers for layout, `std::chrono` for time, early returns over nesting. The rules that bite most:

- Minimal API surface: private by default, no convenience overloads, and every added function has a production caller in the same change. An API that a game or a script calls is judged by what a game developer needs, not by who calls it inside the engine: it ships with its user documentation and a test, and "nothing subscribes yet" is not a reason to delete it.
- No speculative abstraction: no interface, virtual dispatch, template or registry for a consumer that does not exist; the second concrete use tells you where the seam belongs. This is not licence to skip design: decomposition, naming and single responsibility are structure, expected for one consumer.
- Named functions over lambdas: a lambda is a short adapter handed to an API; anything longer, captured across a screen, defined then called once, or needed twice becomes a named function in the owning file.
- One class per file; a file that outgrows its responsibility is split in the change that grows it. No dead code, no commented-out code, no vague TODOs, no magic numbers. Migrate old-style code on contact, never in unprompted bulk.
- Headers: forward declarations in headers, includes in the .cpp; order own header, module headers, stdlib, third-party.
- Comments describe the code as it is now: the invariant, the unit or coordinate convention, the trap and the rule. Never narrate history, never address reviewers, no author or date stamps. Doxygen on public APIs.

## Landing a change

Commit verified work to a feature branch early; main is strict. A clean compile plus green unit tests is not proof for rendering, GPU or runtime changes: confirm in a running editor first, say what to look for (fps, flicker, the interaction), and verify builds from exit status and fresh binaries, never from a wrapper's output. Never push an unverified rendering, GPU or runtime change to main; when verification is hard, stage and ask.

Review: direction fit comes before correctness: say whether the change is right for the engine at all, in the subsystem that owns the concept, in the shape the design agreed, and state that judgement separately from the findings. Security, input parsing, threading, lifetime, GPU resources and user-visible quality get the full adversarial review (`adversarial-review` skill: probes, mutants, a live check where owed); mechanical classes get the same skill at the depth the change needs. Whoever coordinates lanes, reviews or merges reads `GameEngine-Workbench/agent/coordination.md` first: the batched review round, review trees, the ledger, pull requests from the other platform's agents, and when a timing measurement may take the machine.

## Working discipline

- The brief is a hypothesis: verify the premise (reproduce the defect, trace the cited paths at their current state) before implementing; if evidence contradicts it, stop and report rather than improvise.
- Comments are claims; verify against code and fix any comment your change falsifies.
- "Is X enabled" and "did we fix Y" are chains, not values: enumerate every layer with file:line, date it with `git log`, present the table before the verdict. A reversed answer means the chain was incomplete; enumerate, don't re-sample.
- Tier every claim: verified, inferred, assumed. Other agents' reports, the owner's claims and your own under challenge are all hypotheses: re-verify, never retract on weaker evidence than you asserted on, never accept an unchecked correction. Absence of evidence is not evidence of absence.
- Relays are verbatim: quote a decision and its optionality; carry the tier of a lane's finding ("lane X reports Y, unverified by me") or verify first. Ownership, provenance and version claims (whose process, whose worktree, which file version) are checked in one command before they are stated, above all when they would justify not acting; the session-start git snapshot is not a check.
- Never reason from timestamps; verify by exit code, fresh files and running the result. Missing test scaffolding is work, not an excuse. Debug to root cause: reproduce, capture the failing state before rebuilding, verify the instrument, attribute before fixing, rule out the environment, change one variable at a time, prove the fix before/after under the same measurement.
- A fold brief names the fold's checks as a list before the lane starts: the suites that hold a changed test (whole, one process each), the gate steps the fold touches (a subset, never the full gate), one focused capture pair where the look can change, or none. A brief without that list is not sent. A text, comment or documentation fold owes no gate, no capture and no check round. Main is merged into a branch once, at the end of its last fold, never mid-stream; the fold check and the critics start together on the same head. Every extra check a fold runs is time the owner sees as the process creeping back.
- Finish the whole task. A turn that ends with a plan, a question the request already answers, or "the build will wake me" leaves the work undone: wait in session for builds and suites, do the next step, and stop only when blocked on input only the owner can give or before a destructive action. Keep a checklist (`STATUS.md` in the evidence directory) that survives compaction.
- A script other processes may be running is never edited in place: write the new file and move it over the old one. Evidence another team needs is published to the workbench, never cited by a path on one machine.

## Reporting

End every long run under three headings: **Blocked on me** (decisions or input only the owner can give), **Changed** (files, commits, pushes), **Found** (defects, gaps or risks that were not the point of the task). Omit an empty heading. When compacting, preserve verbatim: the owner's decisions in their wording, live lane ids and worktree paths, branch heads and SHAs, process ids of anything still running, and every open item.

## Working with the owner

The owner is a senior graphics engineer (Unity URP/HDRP and SRP internals, UE5 engine-side rendering, and extensive hands-on work in this engine's Vulkan renderer, terrain and ECS). Skip the basics, lead with conclusions and evidence, be direct about trade-offs and about verified versus assumed. A one-sentence Unity or UE cross-reference is the fastest bridge for a genuinely new concept; anchor, don't lecture. Ask only for visual or quality judgement, product direction, or an editor-level GPU run; everything else is decided and split autonomously.

## Documentation

`docs/` is for users of the engine (usage, authored formats, concepts). Designs, decisions, investigations, measurements, plans, post-mortems and review records live in the workbench and are cited from code by name only. The format and the one-canonical-file rule load from `.claude/rules/documentation.md` when you touch `docs/`.
