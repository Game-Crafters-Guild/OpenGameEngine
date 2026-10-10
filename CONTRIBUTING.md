# Contributing

This file is the short form of the rules every change to this repository follows, for people and for agents alike. `AGENTS.md` is the agents' entry point (how to build, test and land a change, and a map of the rest); the full working guidance for agents is `CLAUDE.md` at the repository root, with area conventions under `.claude/rules/`; the coding conventions are `CodingStyle.md`; the build is `BUILD.md`. The owner's review rules in full are `GameEngine-Workbench/agent/pr-review-rules.md`; walk them before opening or reviewing a pull request. When this file and `CLAUDE.md` disagree, `CLAUDE.md` wins.

## Two repositories

- **This repository holds the engine**: source, tests, shipped assets, packages, build system, and design canonicals under `docs/`. Nothing else belongs here.
- **GameEngine-Workbench** holds the team workflow content: the review ledger (`review-tier-log/`), the test map (`test-map/`), follow-up briefs, agent scripts, measurements, triage tables, and the design archive. It is private: maintainers check it out next to this repository as `../GameEngine-Workbench` (override with `GE_WORKBENCH`); nothing in the engine build reads it. A rule in this file that names a workbench document is applied by the maintainers in review; a contributor without access follows this file and `BUILD.md`. Review verdicts, evidence files, screenshots, run logs and reports go there or into the pull request body, never into this tree.

## What does not get committed here

Screenshots and comparison images, generated `.assetdb` files, build and test logs, hash lists, benchmark JSON, dated validation records, per-review notes, agent scratch directories, standalone test projects nothing runs, roadmap or TODO files. If a pull request needs evidence, the numbers go in its body and the files go in the workbench.

## Documentation

- `docs/` is for users of the engine: how to use a feature, an authored format they write, or a concept they need to understand. Development documentation — designs, decisions, investigations, measurements, plans, post-mortems, review records — lives in the workbench repository (`GameEngine-Workbench/designs/`, `measurements/`, `notes/`), not here. A code comment states the invariant, unit, convention or trap itself; it may name the workbench document for the long story, it never depends on it.
- New documentation is HTML: one canonical file per topic, undated filename, a changelog section, the repository's dark style (references in the workbench under `doc-style/`). Existing Markdown stays Markdown when merely edited; the root files (`README.md`, `BUILD.md`, `CodingStyle.md`, `CLAUDE.md`, this file) are the exception by convention.
- A change to documented behaviour edits the canonical. A dated sibling file for "the same topic, another iteration" is a defect.
- No engine names (Unity, Unreal) in user-facing strings, tooltips, enum display names or comments; import features keep their names. User-facing text uses American spelling.

## Tests

- A test lives with the module it tests, registered under `BUILD_TESTING`, visible to `ctest -N`, staged to run from the build output.
- Gate a change with its focused suites: the suites that compile or exercise the touched code (`BUILD.md`, *Tests*, says how to find them; maintainers take them from the workbench test map), each run whole in one process, with declared = ran + disabled reported and every skip named and justified. A filter that matches nothing is not a pass; an all-skips suite is not green. `AGENTS.md` (Test) has the full rule: steps, folds and the daily run of the full set.
- A behavioural claim needs a red control: revert the production change, keep the test, show it fails.
- Merge `origin/main` into the branch and build the merged tree before calling a change ready. A clean textual merge does not mean the result compiles.
- Build every target that compiles a touched file (the focused suites' targets, and `Editor` or `Player` for an application source that no test target compiles, live check or not); never the whole tree. The gtest XML of each gate run goes into the change's evidence.

## Know what exists

The engine already has most of the vocabulary a change needs. Every subsystem is a module under `Engine/Modules/`; list that directory first (it is the full set, and `CLAUDE.md` only summarises it), then `Engine/Include/` for engine-level services and `Apps/Editor/Include/` for the editor's shared pieces. Before writing math, noise, hashing, a container, a string or time utility, a drawing helper or a UI control, look in the module that owns the concept; the ones hit most often are `Engine/Modules/Mathematics`, `Engine/Modules/Types`, `Engine/Modules/Foundation`, `Engine/Modules/Platform`, `Engine/Modules/UI/Include/UI/Controls`, the gizmo API in `Apps/Editor/Include/SceneView/SceneViewGizmos.h`, and the inspector building blocks under `Apps/Editor/Include/UI/`. A second implementation of any of them is a defect. If the shared piece is missing or lacking, add it there in its own change, then use it.

Code lives where its concept lives, as a named, reusable piece with one job: drawing in the gizmo API, noise in `Engine/Modules/Noise`, pickers in the UI controls, presentation in the inspector. A feature delivered as one large file of private helpers and local lambdas is reshaped before it merges.

## Pull requests

- Direction and behaviour come before correctness. Say whether the change is the right thing for the engine at all: the subsystem that owns the concept, the shape the design agreed, and what a user sees at the end. A change that follows its instructions to a bad end result is wrong, and the review says so; decline or defer is the owner's call.
- Every user-facing change is judged as a user meets it: the workflow, the number of steps, what the defaults do, what an error tells them, how the editor looks and feels. Editor UX that fights the user fails review even when the code is clean. Anything visible passes the visual critic (recorded-pose screenshots, before and after, the authoring workflow used); its rejection is binding.
- The reviewer states the end-state judgement separately from the findings: "right change, right shape", or "works, wrong direction", with the reason.
- A pull request from the macOS or the asset-team agent carries its own reviewer verdict and ledger row before it is marked ready; the Windows side then does a pass (`GameEngine-Workbench/agent/coordination.md`).
- Visual work ships with its evidence set captured at the final head against the workbench evidence checklist (`GameEngine-Workbench/agent/evidence-checklists.md`); the review and both visual critics run on that head, and their findings fold in one round, nits included.
- One purpose per pull request and per commit. Nothing unrelated rides along.
- Opening a pull request certifies the contribution under the Developer Certificate of Origin 1.1 (the checkbox in the pull request template): the author has the right to submit it, keeps the copyright in it and licenses it under the repository's MIT licence. No per-commit sign-off line is needed; pull requests from outside the maintainers are squash-merged, so the merged commit carries the pull request and credits its author. A first contribution adds one line to `AUTHORS.md`; contributors keep the copyright in their contributions.
- The body explains the user's problem first, then the change, then the verification, then what is not verified. Retitle when the branch no longer does what the title says.
- Plain commits after review; never force-push a shared branch. No attribution lines or tool signatures in commits or bodies. Impersonal phrasing.
- No feature code in hubs (`EditorApplication`, `SettingsPanel`, `InspectorPanel`, `RenderServices`, `UIManager`, `core.css`). Extensible surfaces take registrations: settings through `EditorSettingsRegistry`, inspectors, post effects, dockables and search items through their registries.
- Few settings, high-quality defaults. A dial whose non-default values are all worse is deleted, not shipped.
- Reuse the engine's types: `GUID` or `StringId` for identity, the maths types, `Handle`, the job system, the event module. Every added function has a production caller in the same change; no compatibility shims, no dead code.
- No legacy by default: there are no users or released projects to preserve. For every API, saved format, and removal, update current callers and committed assets and delete the obsolete path. No backward compatibility, saved-data migrations, deprecated aliases, fallback readers, or legacy-only warnings and checks unless explicitly requested for a concrete consumer. Keep ordinary validation of supported data.
- Performance claims are measured in Release on a quiet machine against a rule stated before measuring.
- Editor UI is declared, not assembled: static layout in `.uxml`, style in the control's own `.css`, one selector per rule, no styling from C++, icons by class; inspectors compose `InspectorSection` and the shared field controls. No text below 12 px.
- Named functions over lambdas: a lambda is a short adapter handed to an API; anything longer, reused, or defined-then-called-once becomes a named function in the owning file.

## Platform and build

- Never resolve a runtime path back into the repository; stage the file and load it from the executable directory.
- Third-party changes are overlay ports under `cmake/ports/` (`cmake/macos-ports/` for a change macOS alone needs) with a bumped port-version and no prose inside the hashed port directory (see `cmake/ports/README.md`).
- Web-only or compat paths are compiled out of the builds that do not need them; state the effect on the other platform.
- No platform-specific exceptions for hardware nobody on the team tests. Fix the cause.

## Commit messages

Every commit has a concise, imperative subject and a body separated by a blank line that says what changed and why. No subject-only commits. The body describes the final committed state, not the editing process; avoid "shortened", "changed from" or "now" unless the commit modifies behaviour that existed in its parent.
