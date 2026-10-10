using System;
using System.Runtime.InteropServices;
using System.Text;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Managed façade over the native PlatformServices facade.
    ///
    /// Platform-blind by design: the same API works against whichever
    /// provider backend the Player build linked (Local, or a native platform
    /// SDK later). Branch on <see cref="GetCapabilities"/>, never on provider
    /// identity.
    /// </summary>
    public static class PlatformServices
    {
        private const string kLib = "GameEngine.Native";
        private const int kMaxAccountFieldBytes = 256;

        public enum Result : int
        {
            Ok = 0,
            Unsupported = 1,
            NotInitialized = 2,
            AlreadyInitialized = 3,
            InvalidArgument = 4,
            NotFound = 5,
            IOError = 6,
            ProviderError = 7,
            FileTooLarge = 8
        }

        // Lowest-common-denominator save limits across shipping backends.
        // A title that stays within these cannot exceed a stricter backend.
        public const long MaxSaveFileBytes = 16L * 1024 * 1024; // 16 MiB
        public const int MaxSlotNameLength = 15;
        public const int MaxFileNameLength = 64;

        [Flags]
        public enum Capability : uint
        {
            None = 0,
            Accounts = 1u << 0,
            LocalSaves = 1u << 1,
            CloudSaves = 1u << 2,
            Achievements = 1u << 3,
            Entitlements = 1u << 4,
            PlatformUI = 1u << 5,
            OfflineMode = 1u << 6
        }

        public struct Account
        {
            public string Id;
            public string DisplayName;
            public bool SignedIn;
            public bool GuestOrOffline;
        }

        public struct AchievementState
        {
            public bool Unlocked;
            public uint Current;
            public uint Target;
        }

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_Initialize(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string appName,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? saveRoot);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern void GE_Platform_Shutdown();
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern void GE_Platform_Tick();
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_IsInitialized();
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern uint GE_Platform_GetCapabilities();
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_GetPrimaryAccount(
            byte[] idBuffer, int idBufferSize, out int idLength,
            byte[] nameBuffer, int nameBufferSize, out int nameLength,
            out int signedIn, out int guestOrOffline);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_RequestSignIn();
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_RequestSignOut();
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_WriteSaveFile(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string slot,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            byte[] data, int size);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_ReadSaveFile(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string slot,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            byte[]? buffer, int bufferSize, out int size);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_DeleteSaveFile(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string slot,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_ListSlots(byte[]? buffer, int bufferSize, out int length);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_ListFiles(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string slot,
            byte[]? buffer, int bufferSize, out int length);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_ListEntitlements(byte[]? buffer, int bufferSize, out int length);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_CommitSave(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string slot);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_UnlockAchievement(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string id);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_SetAchievementProgress(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string id, uint current, uint target);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_GetAchievementState(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string id,
            out int unlocked, out uint current, out uint target);
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_Platform_HasEntitlement(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string id, out int has);

        public static Result Initialize(string appName, string? saveRoot = null)
            => (Result)GE_Platform_Initialize(appName, saveRoot);

        public static void Shutdown() => GE_Platform_Shutdown();
        public static void Tick() => GE_Platform_Tick();
        public static bool IsInitialized => GE_Platform_IsInitialized() != 0;

        public static Capability GetCapabilities() => (Capability)GE_Platform_GetCapabilities();
        public static bool Supports(Capability capability)
            => (GetCapabilities() & capability) == capability && capability != Capability.None;

        public static Result GetPrimaryAccount(out Account account)
        {
            var idBuffer = new byte[kMaxAccountFieldBytes];
            var nameBuffer = new byte[kMaxAccountFieldBytes];
            var result = (Result)GE_Platform_GetPrimaryAccount(
                idBuffer, idBuffer.Length, out var idLength,
                nameBuffer, nameBuffer.Length, out var nameLength,
                out var signedIn, out var guest);
            account = default;
            if (result != Result.Ok)
                return result;
            account.Id = Encoding.UTF8.GetString(idBuffer, 0, Math.Min(idLength, idBuffer.Length));
            account.DisplayName = Encoding.UTF8.GetString(nameBuffer, 0, Math.Min(nameLength, nameBuffer.Length));
            account.SignedIn = signedIn != 0;
            account.GuestOrOffline = guest != 0;
            return Result.Ok;
        }

        public static Result RequestSignIn() => (Result)GE_Platform_RequestSignIn();
        public static Result RequestSignOut() => (Result)GE_Platform_RequestSignOut();

        public static Result WriteSaveFile(string slot, string name, byte[] data)
            => (Result)GE_Platform_WriteSaveFile(slot, name, data, data.Length);

        public static Result ReadSaveFile(string slot, string name, out byte[] data)
        {
            data = Array.Empty<byte>();
            // Size query + read; the file can change between the two calls
            // (native returns InvalidArgument when it grew), so retry with
            // the fresh size and trim if it shrank.
            for (var attempt = 0; attempt < 4; ++attempt)
            {
                var result = (Result)GE_Platform_ReadSaveFile(slot, name, null, 0, out var size);
                if (result != Result.Ok)
                    return result;
                if (size == 0)
                    return Result.Ok;
                var buffer = new byte[size];
                result = (Result)GE_Platform_ReadSaveFile(slot, name, buffer, buffer.Length, out var actualSize);
                if (result == Result.Ok)
                {
                    if (actualSize != buffer.Length)
                        Array.Resize(ref buffer, actualSize);
                    data = buffer;
                    return Result.Ok;
                }
                if (result != Result.InvalidArgument)
                    return result;
            }
            return Result.IOError;
        }

        public static Result DeleteSaveFile(string slot, string name)
            => (Result)GE_Platform_DeleteSaveFile(slot, name);

        public static Result ListSlots(out string[] slots)
            => ReadLineList(GE_Platform_ListSlots, out slots);

        public static Result ListFiles(string slot, out string[] files)
            => ReadLineList((byte[]? buffer, int size, out int length)
                => GE_Platform_ListFiles(slot, buffer, size, out length), out files);

        public static Result ListEntitlements(out string[] entitlements)
            => ReadLineList(GE_Platform_ListEntitlements, out entitlements);

        private delegate int ListCall(byte[]? buffer, int bufferSize, out int length);

        private static Result ReadLineList(ListCall call, out string[] values)
        {
            values = Array.Empty<string>();
            var result = (Result)call(null, 0, out var length);
            if (result != Result.Ok)
                return result;
            if (length == 0)
                return Result.Ok;
            var buffer = new byte[length];
            result = (Result)call(buffer, buffer.Length, out var readLength);
            if (result != Result.Ok)
                return result;
            var joined = Encoding.UTF8.GetString(buffer, 0, Math.Min(readLength, buffer.Length));
            values = joined.Split('\n', StringSplitOptions.RemoveEmptyEntries);
            return Result.Ok;
        }

        public static Result CommitSave(string slot) => (Result)GE_Platform_CommitSave(slot);

        public static Result UnlockAchievement(string id) => (Result)GE_Platform_UnlockAchievement(id);

        public static Result SetAchievementProgress(string id, uint current, uint target)
            => (Result)GE_Platform_SetAchievementProgress(id, current, target);

        public static Result GetAchievementState(string id, out AchievementState state)
        {
            var result = (Result)GE_Platform_GetAchievementState(id, out var unlocked, out var current, out var target);
            state = default;
            if (result != Result.Ok)
                return result;
            state.Unlocked = unlocked != 0;
            state.Current = current;
            state.Target = target;
            return Result.Ok;
        }

        public static Result HasEntitlement(string id, out bool has)
        {
            var result = (Result)GE_Platform_HasEntitlement(id, out var hasValue);
            has = hasValue != 0;
            return result;
        }
    }
}
