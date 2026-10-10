# Overlay ports

These directories override the upstream vcpkg registry ports of the same name
(wired via `vcpkg-configuration.json` → `overlay-ports: ["cmake/ports"]`).
They exist to carry engine-specific patches.

`cmake/macos-ports` holds overlays that only macOS triplets use (see
[below](#macos-only-overlays-cmakemacos-ports)). The rules on this page apply
to them as well.

## The one rule: every behavioural edit bumps `port-version`

When you change **anything a consumer can observe** — a patch file, the
portfile, build flags, features — increment `"port-version"` in that port's
`vcpkg.json` (add the field if absent; it defaults to 0) in the **same
commit**.

Why this is load-bearing, not bookkeeping:

- vcpkg decides whether to reinstall a port by comparing the declared
  `version` + `port-version` against its installed database. An edit without a
  bump is **invisible**: every already-populated `vcpkg_installed` keeps the
  old binaries and every build linking them silently ships your patch's
  absence — and no layer, the freshness guard included, can detect it.
- A correct bump is necessary but not sufficient: the yoga flexbox patch
  (commit `1c20a2662`, 2026-07-31) landed with `"port-version": 1` and still
  linked nowhere for days. The trees that missed it were redirected at a shared
  `vcpkg_installed` — a shape configure now refuses (`cmake/VcpkgInstallGuard.cmake`)
  — and ran with `VCPKG_MANIFEST_INSTALL=OFF`, so no reconfigure ever reinstalled
  there, and nothing compared the declared version against the installed one. The
  configure-time freshness guard (`cmake/OverlayPortGuard.cmake`) closes that
  second gap by making the comparison and hard-failing on a stale install —
  but it reads the same two fields, so it only sees what the bump made
  visible. Both requirements stand on their own.

When the guard fails a configure, its message names the port, both versions,
and the reconfigure command that fixes the install root. Do not work around it
by editing the declared version downward; reinstall instead.

## Escape hatch: `GE_ALLOW_STALE_OVERLAY_PORTS`

If you are mid-task and need one build through before fixing the install
root:

```
cmake --preset <preset> -DGE_ALLOW_STALE_OVERLAY_PORTS=ON
```

The stale-port failure downgrades to a warning that repeats on **every**
configure, and the build links the pre-patch library — treat any result
touching the patched behaviour as unmeasured. It is a cache variable and
stays on until you reconfigure with `-DGE_ALLOW_STALE_OVERLAY_PORTS=OFF`; do
that as soon as the install root is current.

Updating a port to a new **upstream** version resets `port-version` to 0 (or
omit it) — same as vcpkg's registry convention.

## Port notes live here, not inside the port directory

vcpkg hashes every file inside `cmake/ports/<port>/` into that port's ABI
(`share/<port>/vcpkg_abi_info.txt` lists them), and the install-set guard
(`cmake/VcpkgInstallGuard.cmake`) treats an ABI change as grounds to wipe the
build directory. A README inside a port directory therefore turns a wording
edit into a rebuild of the port and a full rebuild of every tree that
reconfigures. Keep a port's prose in this file; keep only portfile, manifest,
patches and build inputs in the port directory.

### glfw3 — Win32 active-window ownership and button coordinates

This overlay retains the baseline registry's GLFW **3.5.1** source and port
settings (registry tree `983a9c6ea042da294d61cb405600725dd7c22bbd`) and carries
two Win32 patches: `0001` for polling's modifier-release repair (below) and
`0002` for button-event coordinates (after it).

`AttachThreadInput` shares active-window state between input queues, including
across processes. `GetActiveWindow()` can then return a foreign HWND whose
`"GLFW"` property contains an address in another process. The old polling code
dereferences that address as `_GLFWwindow`, causing an access violation. A
process-ID check alone also cannot establish ownership by this GLFW instance.

Resolve the active HWND through GLFW's existing window list instead. This adds
no API or registry; the short scan runs once per poll when an active HWND exists.
Only the matching owned window receives modifier-release repair. Unmatched or
null active windows still reach the existing disabled-cursor handling. GLFW's
main-thread/event-polling restrictions govern list lifetime as before.

The defect also exists in GLFW 3.4; this is not attributed to the 3.5.1 upgrade.
Upstream closed the report of this lookup as external
([glfw/glfw#2803](https://github.com/glfw/glfw/issues/2803)), so expect the
patch to persist across updates. When updating the dependency, check whether
the lookup changed, rebase or remove the patch, bump `port-version` in
`vcpkg.json` for any change to what this port builds, and keep the ownership
regression `GlfwWindowOwnership.*` (`Engine/Modules/Platform/Tests`) green.

References: [shared input state](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-attachthreadinput),
[active window lookup](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getactivewindow).

**Button-event coordinates (`0002`).** A Win32 button message carries its own
client point in `lParam`, but GLFW's button callback has no position, so a
consumer hit-tests at the last cursor callback's point or queries the cursor.
Querying at dispatch returns wherever the pointer is *now*, which is later than
the press when movement follows it in the same poll; the last callback's point
is missing after a consumer cleared its position (cursor leave, focus loss),
because GLFW suppresses a cursor callback whose point did not change. The patch
reports the message's point through the existing cursor callback before every
button callback, including an unchanged point, and leaves a disabled cursor
(raw or not) alone so relative motion never receives an absolute point. It adds
no API. `Platform::Window::MouseButtonThunk` relies on it and does not re-query
the cursor on Win32. When updating GLFW, rebase or remove the patch if upstream
reports button positions itself, and keep `GlfwButtonCoordinates.*` and
`PlatformWindowButtonTests` (`Engine/Modules/Platform/Tests`) green.

## macOS-only overlays (`cmake/macos-ports`)

An overlay in `cmake/ports` reaches every triplet, and vcpkg hashes its files
into the port's ABI wherever it applies, so even a copy of a registry port that
changes only `supports` reinstalls that port on Windows and Linux and makes
`cmake/VcpkgInstallGuard.cmake` refuse every existing build directory there. A
port changed for macOS alone therefore lives in `cmake/macos-ports`, which
`cmake/Dependencies.cmake` hands to vcpkg for `*-osx` triplets only, through
the `VCPKG_OVERLAY_PORTS` environment variable that the toolchain's manifest
install and the installs `install_required_packages()` and
`verify_dependencies()` run all inherit. Windows and Linux keep the registry
ports and their installed ABIs. The freshness guard and the configure
dependencies cover this directory on macOS, so a port edit there bumps
`port-version` like any other.

`cmake/steamdeck-ports` is also a platform-only directory, but the Steam Deck
presets pass it through the toolchain's `VCPKG_OVERLAY_PORTS` cache variable.
`cmake/macos-ports` cannot depend on a preset: `vcpkg.json` requests
`directxtex` on `osx`, so every macOS configure needs the overlays, including
one without a preset, where `cmake/Dependencies.cmake` works out the triplet
itself. The cache variable also stays in `CMakeCache.txt`, and the build
directory then fails to configure a checkout without the directory ("Overlay
path ... must be an existing directory").

### directxtex and directx-headers — the BC encoder on macOS hosts

The registry lists DirectXTex (`may2026`) and DirectX-Headers (`v1.619.5`) for
Windows and Linux only. Without these overlays a macOS configure has no BC
encoder, and every texture the Editor or an export cooks resolves to
uncompressed RGBA8. These overlays are the baseline registry's ports (registry
trees `e9f89c9f75ab81ba10bf86dca60c5f202f881690` and
`ae5de268690a6a9216cf835574d1da1b5ffacf87`) with three edits: `supports` gains
`osx` in both, DirectXTex's `directx-headers` dependency gains `osx`, and both
declare `port-version` 1. Off Windows, DirectXTex builds against
DirectX-Headers' WSL adapter headers and DirectXMath, as on Linux. Macs sample
BC, so a macOS host cooks the same BC artifacts as the other desktop hosts.
When the registry adds `osx` to both ports, delete these overlays.

**A baseline bump checks these two ports.** vcpkg always takes an overlay over
the registry, so these overlays keep macOS on DirectXTex `2026-05-07` and
DirectX-Headers `1.619.5` after any bump of the `baseline` in
`vcpkg-configuration.json`, while Windows and Linux move to the new registry
versions. Nothing reports that: the freshness guard compares the install with
the overlay, not with the registry. DirectXMath is not overlaid, so it moves
under the old DirectXTex, and `DIRECTX_TEX_VERSION` is part of the texture-cook
key (`Engine/Source/Assets/TextureCook.cpp`), so the same project would key its
cooks differently on a Mac and on Windows. So whoever bumps the baseline looks
up both ports at the new baseline (`git rev-parse <baseline>:ports/directxtex`
and `:ports/directx-headers` in a vcpkg clone that has the commit) and compares
them with the registry trees recorded above:

- If the new registry lists `osx` in both ports' `supports`, delete both
  overlays, and this section with them.
- Otherwise, if either tree moved, copy both ports again from the new registry
  trees in the same commit as the bump, with the same three edits (`osx` in
  `supports`, `osx` in DirectXTex's `directx-headers` dependency, and a
  `port-version` one above the registry's), and record the new trees here.
  Copy both even when only one moved, so the pair always comes from one
  baseline.
- If neither tree moved, the overlays stay as they are.

### thorvg — no global operator new/delete

This overlay retains the baseline registry's ThorVG **1.1.2** port (registry tree
`0eb333ce0ef1d78c4e211f94a64727dcd971fc1b`) and removes the four global
allocation functions `src/renderer/tvgInitializer.cpp` defines (`operator new`,
`operator new[]`, `operator delete`, `operator delete[]`, each a forwarder to
`malloc`/`free`). Linked statically, they become `Engine`'s own global
allocation functions: a third-party library decides `Engine`'s allocator, and
any replacement the engine defines in `Engine` is a duplicate symbol. Without
them ThorVG allocates through whatever the image's `operator new` is: the
default library one (also `malloc`/`free`) or the engine's replacement.
Behaviour is unchanged except on allocation failure: ThorVG's operator new
returned null, the standard one throws `std::bad_alloc`.

`vcpkg_replace_string` only warns when nothing matches, so the portfile first
searches the source for the exact block and stops the port build with
`FATAL_ERROR` when it is missing. A ThorVG update that changes the block fails
the build and has to re-check it; it cannot silently bring the four
definitions back into `Engine`.

ThorVG is a separate project; once it stops defining these functions in its
static library (or offers a build option that leaves them out), delete this
overlay and this section.
