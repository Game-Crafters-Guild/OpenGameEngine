#pragma once

namespace GameEngine
{

/// The target framework of every C# assembly the engine generates a project for or
/// asks the compile server to build (project scripts, package modules, the NativeAOT
/// export), and the output folder of the engine's own managed assemblies. It names the
/// .NET major the engine hosts; the compile server resolves its reference assemblies
/// from the matching Microsoft.NETCore.App.Ref pack.
inline constexpr const char kScriptTargetFramework[] = "net10.0";

} // namespace GameEngine
