using GameEngine.Interop;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Stable 64-bit input id hashing matching native Input::HashInput (FNV-1a over the name's UTF-8 bytes).
    /// </summary>
    public static class InputIds
    {
        public static ulong HashInput(string text) => StringIds.Hash(text);
    }
}

