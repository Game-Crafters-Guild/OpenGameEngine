namespace GameEngine.Scripting
{
    /// <summary>
    /// Key code constants matching GLFW key codes.
    /// Use with Input.IsKeyDown(), Input.WasKeyPressed(), etc.
    /// </summary>
    public static class KeyCode
    {
        // Letters
        public const int A = 65;
        public const int B = 66;
        public const int C = 67;
        public const int D = 68;
        public const int E = 69;
        public const int F = 70;
        public const int G = 71;
        public const int H = 72;
        public const int I = 73;
        public const int J = 74;
        public const int K = 75;
        public const int L = 76;
        public const int M = 77;
        public const int N = 78;
        public const int O = 79;
        public const int P = 80;
        public const int Q = 81;
        public const int R = 82;
        public const int S = 83;
        public const int T = 84;
        public const int U = 85;
        public const int V = 86;
        public const int W = 87;
        public const int X = 88;
        public const int Y = 89;
        public const int Z = 90;

        // Digits
        public const int Alpha0 = 48;
        public const int Alpha1 = 49;
        public const int Alpha2 = 50;
        public const int Alpha3 = 51;
        public const int Alpha4 = 52;
        public const int Alpha5 = 53;
        public const int Alpha6 = 54;
        public const int Alpha7 = 55;
        public const int Alpha8 = 56;
        public const int Alpha9 = 57;

        // Function keys
        public const int F1 = 290;
        public const int F2 = 291;
        public const int F3 = 292;
        public const int F4 = 293;
        public const int F5 = 294;
        public const int F6 = 295;
        public const int F7 = 296;
        public const int F8 = 297;
        public const int F9 = 298;
        public const int F10 = 299;
        public const int F11 = 300;
        public const int F12 = 301;

        // Navigation
        public const int Up = 265;
        public const int Down = 264;
        public const int Left = 263;
        public const int Right = 262;
        public const int Home = 268;
        public const int End = 269;
        public const int PageUp = 266;
        public const int PageDown = 267;

        // Control keys
        public const int Space = 32;
        public const int Escape = 256;
        public const int Enter = 257;
        public const int Tab = 258;
        public const int Backspace = 259;
        public const int Insert = 260;
        public const int Delete = 261;

        // Modifiers
        public const int LeftShift = 340;
        public const int LeftControl = 341;
        public const int LeftAlt = 342;
        public const int RightShift = 344;
        public const int RightControl = 345;
        public const int RightAlt = 346;
    }

    /// <summary>
    /// Mouse button constants.
    /// </summary>
    public static class MouseButton
    {
        public const int Left = 0;
        public const int Right = 1;
        public const int Middle = 2;
    }
}
