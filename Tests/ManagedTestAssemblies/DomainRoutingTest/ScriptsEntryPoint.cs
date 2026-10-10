namespace GameEngine.Scripts;

public static class ScriptsEntryPoint
{
    // returns domain-specific value via static state for test validation
    private static int s_value;

    // Simple lifecycle entrypoint used by ScriptingAbi tests; non-negative means success
    public static int OnAssemblyLoaded()
    {
        s_value = 0;
        return 0;
    }

    // Used to exercise negative-return handling paths in GE_Invoke
    public static int ReturnNegative()
    {
        return -5;
    }

    public static int SetValueTo(int v)
    {
        s_value = v; return v;
    }

    // Parameterless ops for ABI convenience
    public static int Reset() { s_value = 0; return 0; }
    public static int IncrementAndGet() { s_value++; return s_value; }

    public static int GetValue()
    {
        return s_value;
    }
}

