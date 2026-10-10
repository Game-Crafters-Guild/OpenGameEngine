namespace GameEngine.CoreBridge
{
    /// <summary>
    /// Result of a managed scripting operation. The codes mirror native GE_Result
    /// (Engine/Include/Scripting/ScriptingABI.h) value for value.
    /// </summary>
    public enum ScriptingOpResult
    {
        /// <summary>Operation completed successfully. GE_Result_Ok.</summary>
        Ok = 0,
        /// <summary>General failure (unexpected error or unclassified). GE_Result_Fail.</summary>
        Fail = -1,
        /// <summary>Invalid argument passed to the API (null/empty/length mismatch, etc.). GE_Result_InvalidArg.</summary>
        InvalidArg = -2,
        /// <summary>Requested capability or resource is not available (e.g., HRM missing). GE_Result_NotFound.</summary>
        NotFound = -3,
        /// <summary>The subsystem has not been initialized. GE_Result_NotInitialized.</summary>
        NotInitialized = -4,
        /// <summary>The subsystem was already initialized. GE_Result_AlreadyInitialized.</summary>
        AlreadyInitialized = -5,
        /// <summary>The destination buffer is full. GE_Result_BufferFull.</summary>
        BufferFull = -6,
    }

    internal static class ScriptingOp
    {
        /// <summary>
        /// Maps a HotReloadManager return code to ScriptingOpResult, with the same
        /// table as native MapManagedRcToGeResult (ScriptingABI.cpp); any other
        /// code is Fail.
        /// </summary>
        /// <remarks>
        /// -5 and -6 stay Fail: HotReloadManager returns -5 for a script method
        /// that threw, which is not GE_Result_AlreadyInitialized.
        /// </remarks>
        public static ScriptingOpResult MapCommon(int code)
        {
            return code switch
            {
                0 => ScriptingOpResult.Ok,
                -1 => ScriptingOpResult.Fail,
                -2 => ScriptingOpResult.InvalidArg,
                -3 => ScriptingOpResult.NotFound,
                -4 => ScriptingOpResult.NotInitialized,
                _ => ScriptingOpResult.Fail,
            };
        }
    }
}
