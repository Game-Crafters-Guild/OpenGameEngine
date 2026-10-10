// The AOT project a ship-optimized build publishes compiles the project scripts
// and runtime package modules with the language settings of the live compile and
// the generated hot-reload csprojs, so code that compiles in the editor also
// compiles in the export. A runtime package module that ships a prebuilt
// assembly and no sources is linked in by reference instead.

#include "Engine/Build/NativeAotProject.h"

#include <gtest/gtest.h>

#include <string>

using namespace GameEngine;

TEST(NativeAotProject, CarriesTheLiveCompileLanguageSettings)
{
    NativeAotProjectDesc desc;
    desc.ScriptSources = "X:/game/Assets";
    const std::string xml = GenerateNativeAotCsprojXml(desc);
    EXPECT_NE(xml.find("<AllowUnsafeBlocks>true</AllowUnsafeBlocks>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<ImplicitUsings>enable</ImplicitUsings>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<Nullable>enable</Nullable>"), std::string::npos) << xml;
}

// A module with no sources has no glob to add: its declared assembly is
// referenced and rooted, so ILC keeps its types and module initializers.
TEST(NativeAotProject, PrebuiltModuleWithoutSourcesIsReferencedAndRooted)
{
    PackageCodeModule module;
    module.AssemblyName = "ClosedPack";
    module.Lang = PackageModuleRecord::ModuleLang::CSharp;
    module.Kind = PackageModuleRecord::ModuleKind::Runtime;
    module.PrebuiltDir = "X:/packages/closed-pack/Binaries";

    NativeAotProjectDesc desc;
    desc.ScriptSources = "X:/game/Assets";
    desc.PackageModules = {module};
    const std::string xml = GenerateNativeAotCsprojXml(desc);
    EXPECT_EQ(xml.find("<Compile Include=\"**"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<Reference Include=\"ClosedPack\">\n"
                       "      <HintPath>X:/packages/closed-pack/Binaries/ClosedPack.dll</HintPath>"),
              std::string::npos)
        << xml;
    EXPECT_NE(xml.find("<TrimmerRootAssembly Include=\"ClosedPack\" />"), std::string::npos) << xml;
}
