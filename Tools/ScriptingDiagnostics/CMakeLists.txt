add_executable(ScriptingDiag
    ScriptingDiag.cpp
)

# Link against the native scripting shim which exports the C ABI
# The global include dirs are set in the root CMakeLists, so just link
# This also ensures proper build order
target_link_libraries(ScriptingDiag PRIVATE GameEngine.Native)

# Group for IDEs
set_target_properties(ScriptingDiag PROPERTIES FOLDER "Tools")

# Output layout: bin/<Config>/Tools
ge_set_output_tools(ScriptingDiag)

# Stage runtime DLL dependencies next to the tool (e.g. GameEngine.Native.dll)
ge_stage_runtime_dependencies(ScriptingDiag)

