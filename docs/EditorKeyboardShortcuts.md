# Editor Keyboard Shortcuts

## Gizmo Tools (Scene View)

| Key | Tool | Description |
|-----|------|-------------|
| **Q** | Selection | Arrow tool - select objects |
| **W** | Move | Translate gizmo - move objects |
| **E** | Rotate | Rotation gizmo - rotate objects |
| **R** | Scale | Scale gizmo - resize objects |

## Camera Navigation (Scene View)

### Mouse Camera Controls

| Input | Action | Description |
|-------|--------|-------------|
| **RMB (Hold & Drag)** | FPS Look | Free-look camera (first-person style) |
| **Alt + LMB** | Orbit | Rotate camera around pivot point |
| **Alt + Ctrl + LMB** (PC/Linux) | Pan | Move camera horizontally/vertically |
| **Alt + Cmd + LMB** (macOS) | Pan | Move camera horizontally/vertically |
| **Alt + RMB** | Dolly | Zoom in/out (move left-right) |

### Keyboard Camera Controls

| Input | Action | Description |
|-------|--------|-------------|
| **W** (+ RMB) | Move Forward | Move camera forward |
| **A** (+ RMB) | Move Left | Move camera left |
| **S** (+ RMB) | Move Backward | Move camera backward |
| **D** (+ RMB) | Move Right | Move camera right |
| **Shift** (+ WASD + RMB) | Fast Move | 3x faster camera movement |

### Camera Framing

| Key | Action | Description |
|-----|--------|-------------|
| **F** | Frame Selection | Focus camera on selected object |
| **Alt + F** | Frame All | Focus camera on all objects in scene |

## Global Editor Shortcuts

| Key | Action | Description |
|-----|--------|-------------|
| **G** | Toggle Gizmos | Show/hide transformation gizmos |
| **P** | Toggle FPS Counter | Show/hide FPS counter in Scene View |
| **Cmd + W** (macOS) | Close Tab/Window | Close active tab or floating window |
| **Ctrl + W** (PC/Linux) | Close Tab/Window | Close active tab or floating window |
| **F11** | Toggle Debug Zones | Show/hide dock debug overlay |
| **F3** | UI Demo Panel | Open UI demo/test panel |

---

## View Navigation

| Input | Action | Description |
|-------|--------|-------------|
| **Ctrl + Scroll** (PC/Linux), **Cmd + Scroll** (macOS) | Resize Items | Over the Hierarchy or the Assets panel's items, or over the panel's size slider: wheel up makes the items bigger, wheel down smaller |

Resize Items can be rebound under **Keyboard Shortcuts > View Navigation > Resize Items**. The
binding must include a modifier, so a plain wheel always scrolls. The size slider's tooltip names
the current binding. In the Assets grid, sizes step on a 16 px grid (32, 48, 64, ... px), the same
sizes a drag on the slider reaches. See [UI control defaults](ui-control-defaults.html) for how
the gesture reaches game UI grids.

---

## Platform Differences

All shortcuts work cross-platform. The only difference is:
- **macOS** uses **Cmd** (Command) as the primary modifier
- **Windows/Linux** uses **Ctrl** (Control) as the primary modifier

---

## Tab Management

| Action | Method | Description |
|--------|--------|-------------|
| **Close Tab** | Right-click on tab | Close a specific panel tab |
| **Close Tab** | **Cmd/Ctrl + W** | Close the currently active tab |
| **Close Window** | **Cmd/Ctrl + W** (when no tabs) | Close floating window |

**Note**: The hidden close button on tabs has been removed. Use right-click or keyboard shortcuts to close tabs.

---

## Tips

- **WASD navigation** requires holding the right mouse button (RMB)
- **Alt-based camera controls** work independently and don't require RMB
- Press **Shift** while moving to triple your movement speed
- Press **F** to quickly frame and zoom to a selected object
- Press **Alt + F** to frame all objects in the scene
- **Right-click on any tab** to access the context menu with close option
- **Cmd/Ctrl + W** closes the active tab; if no tabs remain in a floating window, it closes the window

