using System;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Managed façade over the native InputSystem.
    ///
    /// Designed for gameplay scripts:
    /// - Users define their own contexts/actions by hashing strings (InputIds.HashInput).
    /// - The Editor can gate input by enabling/disabling a chosen context (e.g. "Editor.GameView").
    /// </summary>
    public static class Input
    {
        private const string kLib = "GameEngine.Native";

        public enum DeviceType : int
        {
            Keyboard = 0,
            Mouse = 1,
            Gamepad = 2
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct ActionState
        {
            public byte pressed;
            public byte justPressed;
            public byte justReleased;
            public byte _pad0;
            public float value;
        }

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_GetActionState(ulong actionId, out ActionState state);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_IsKeyDown(int key, out int down);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_WasKeyPressed(int key, out int pressed);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_WasKeyReleased(int key, out int released);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_GetMousePosition(out float x, out float y);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_IsPointerInWindow(out int inWindow);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_IsWindowFocused(out int focused);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_IsMouseButtonDown(int button, out int down);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_WasMouseButtonPressed(int button, out int pressed);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_WasMouseButtonReleased(int button, out int released);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_RegisterAction(ulong contextId, ulong actionId, int isAxis);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_RemoveAction(ulong contextId, ulong actionId);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_ClearBindings(ulong contextId, ulong actionId);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Input_Bind(ulong contextId, ulong actionId, int deviceType, int code, float scale, int requiredMods, int forbiddenMods);

        // NOTE: Context stack / enable control is intentionally not exposed to gameplay scripts.
        // The Editor owns focus gating (e.g. only enable gameplay input when GameView is focused).

        // -----------------------------------------------------------------
        // High-level helpers (string -> id)
        // -----------------------------------------------------------------

        public static ulong Id(string name) => InputIds.HashInput(name);

        public static int RegisterAction(string contextName, string actionName, bool isAxis)
            => RegisterAction(Id(contextName), Id(actionName), isAxis);

        public static int RegisterAction(ulong contextId, ulong actionId, bool isAxis)
            => GE_Input_RegisterAction(contextId, actionId, isAxis ? 1 : 0);

        public static int RemoveAction(string contextName, string actionName)
            => RemoveAction(Id(contextName), Id(actionName));

        public static int RemoveAction(ulong contextId, ulong actionId)
            => GE_Input_RemoveAction(contextId, actionId);

        public static int ClearBindings(string contextName, string actionName)
            => ClearBindings(Id(contextName), Id(actionName));

        public static int ClearBindings(ulong contextId, ulong actionId)
            => GE_Input_ClearBindings(contextId, actionId);

        public static int Bind(string contextName,
                               string actionName,
                               DeviceType device,
                               int code,
                               float scale = 1.0f,
                               int requiredMods = 0,
                               int forbiddenMods = 0)
            => Bind(Id(contextName), Id(actionName), device, code, scale, requiredMods, forbiddenMods);

        public static int Bind(ulong contextId,
                               ulong actionId,
                               DeviceType device,
                               int code,
                               float scale = 1.0f,
                               int requiredMods = 0,
                               int forbiddenMods = 0)
            => GE_Input_Bind(contextId, actionId, (int)device, code, scale, requiredMods, forbiddenMods);

        // -----------------------------------------------------------------
        // Polling
        // -----------------------------------------------------------------

        public static int GetActionState(string actionName, out ActionState state) => GetActionState(Id(actionName), out state);
        public static int GetActionState(ulong actionId, out ActionState state) => GE_Input_GetActionState(actionId, out state);

        public static int IsKeyDown(int key, out bool down)
        {
            down = false;
            int rc = GE_Input_IsKeyDown(key, out int v);
            down = (v != 0);
            return rc;
        }

        public static int WasKeyPressed(int key, out bool pressed)
        {
            pressed = false;
            int rc = GE_Input_WasKeyPressed(key, out int v);
            pressed = (v != 0);
            return rc;
        }

        public static int WasKeyReleased(int key, out bool released)
        {
            released = false;
            int rc = GE_Input_WasKeyReleased(key, out int v);
            released = (v != 0);
            return rc;
        }

        /// <summary>
        /// Where the last move placed the pointer, in window pixels: (0,0) before any
        /// move, and standing after the pointer leaves the window or the window loses
        /// focus. Gate on <see cref="IsPointerInWindow"/> and <see cref="IsWindowFocused"/>
        /// rather than on the position.
        /// </summary>
        public static int GetMousePosition(out float x, out float y) => GE_Input_GetMousePosition(out x, out y);

        /// <summary>
        /// True from the first move that reaches the window until the pointer leaves
        /// it, and again from the next move. Moves keep arriving while a button is
        /// held, so a drag that crosses the window edge stays in. Hover-style reads
        /// want this alone.
        /// </summary>
        public static int IsPointerInWindow(out bool inWindow)
        {
            inWindow = false;
            int rc = GE_Input_IsPointerInWindow(out int v);
            inWindow = (v != 0);
            return rc;
        }

        /// <summary>
        /// Whether the window holds focus, as the platform reports it. Gestures that act
        /// on the world (edge scrolling, drags, wheel zoom) want this as well as the
        /// pointer in the window: a window behind another still receives moves and the
        /// wheel for a pointer that merely crosses it.
        /// </summary>
        public static int IsWindowFocused(out bool focused)
        {
            focused = false;
            int rc = GE_Input_IsWindowFocused(out int v);
            focused = (v != 0);
            return rc;
        }

        public static int IsMouseButtonDown(int button, out bool down)
        {
            down = false;
            int rc = GE_Input_IsMouseButtonDown(button, out int v);
            down = (v != 0);
            return rc;
        }

        // Frame-snapshot edge queries (mouse mirror of WasKeyPressed/WasKeyReleased).
        // Both report true for a press+release pair completing inside one frame —
        // the case an IsMouseButtonDown poll silently loses.
        public static int WasMouseButtonPressed(int button, out bool pressed)
        {
            pressed = false;
            int rc = GE_Input_WasMouseButtonPressed(button, out int v);
            pressed = (v != 0);
            return rc;
        }

        public static int WasMouseButtonReleased(int button, out bool released)
        {
            released = false;
            int rc = GE_Input_WasMouseButtonReleased(button, out int v);
            released = (v != 0);
            return rc;
        }
    }
}

