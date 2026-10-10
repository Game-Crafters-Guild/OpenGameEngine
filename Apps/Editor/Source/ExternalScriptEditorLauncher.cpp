 #include "ExternalScriptEditorLauncher.h"

#include "Core/Engine.h"
#include "Scripting/ScriptManager.h"
#include "Platform/Shell.h"

namespace GameEngine
{

std::filesystem::path ExternalScriptEditorLauncher::ScriptProjectPath(const ScriptManager& scripts)
{
	// The path the generator writes, never a second guess at it: the default
	// layout generates the project in the asset root, apart from the assemblies.
	const std::filesystem::path projectPath = scripts.GeneratedProjectPath();
	if (projectPath.parent_path().empty())
		return {};
	return projectPath;
}

bool ExternalScriptEditorLauncher::OpenScript(const std::filesystem::path& scriptPath)
{
	if (scriptPath.empty())
	{
		return false;
	}

	// If scripting is not compiled in, or the engine is not initialized yet,
	// fall back to the default OS handler for the script file.
	#if !GE_ENABLE_SCRIPTING
		return Platform::OpenPath(scriptPath);
	#else
		EngineCore& engine = EngineCore::GetInstance();
		if (!engine.IsInitialized())
		{
			return Platform::OpenPath(scriptPath);
		}

		const std::filesystem::path projectPath = ScriptProjectPath(engine.GetScriptManager());
		if (!projectPath.empty() && Platform::OpenScriptWithProject(scriptPath, projectPath))
		{
			return true;
		}

		// If anything above fails (no generated project, project missing, or
		// platform-specific launch failing), gracefully fall back to the default
		// shell open for the script file.
		return Platform::OpenPath(scriptPath);
	#endif
}

} // namespace GameEngine

