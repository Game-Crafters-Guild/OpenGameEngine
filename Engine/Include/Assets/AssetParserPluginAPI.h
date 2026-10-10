#pragma once

// Minimal C ABI for parser plugins.
//
// A parser plugin is a shared library (.dll/.so/.dylib) that exports:
//
//   extern "C" void GE_RegisterAssetParsers(GameEngine::ParserRegistry* registry);
//
// The plugin should call registry->RegisterParser(...) for each parser it provides.

namespace GameEngine
{
class ParserRegistry;
}

#if defined(_WIN32)
#if defined(GE_ASSET_PARSER_PLUGIN_BUILD)
#define GE_ASSET_PARSER_PLUGIN_API extern "C" __declspec(dllexport)
#else
#define GE_ASSET_PARSER_PLUGIN_API extern "C" __declspec(dllimport)
#endif
#else
#define GE_ASSET_PARSER_PLUGIN_API extern "C"
#endif

GE_ASSET_PARSER_PLUGIN_API void GE_RegisterAssetParsers(GameEngine::ParserRegistry* registry);
