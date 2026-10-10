#pragma once

#include <filesystem>

namespace GameEngine
{
class PolyhavenDownloadManager;

namespace Editor::Automation
{
/// Environment-driven startup actions for headless gates. Nothing here runs
/// unless the gate asked for it through the environment, and nothing here
/// touches the scene: each hook exercises one transfer or service path so a
/// run can be judged from the log alone.
///
/// GE_SMOKE_POLYHAVEN=<slug>[:<type>] starts one online-library download
/// (type defaults to "textures"); the gate reads 'Polyhaven: downloaded' or
/// 'download failed' from the log.
void RunStartupSmokeHooks(PolyhavenDownloadManager& downloads, const std::filesystem::path& assetsRoot);
} // namespace Editor::Automation
} // namespace GameEngine
