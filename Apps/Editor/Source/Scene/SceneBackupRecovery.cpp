#include "Scene/SceneBackupRecovery.h"

#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"

#include <string>
#include <system_error>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kStagedSuffix = ".backup";
constexpr const char* kAtomicTempSuffix = ".tmp";

bool EndsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void RemoveQuiet(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

} // namespace

std::filesystem::path SceneBackupPathFor(const std::filesystem::path& scenePath)
{
    return scenePath.parent_path()
           / (scenePath.stem().string() + kStagedSuffix + scenePath.extension().string());
}

std::filesystem::path SceneZonesFolderFor(const std::filesystem::path& scenePath)
{
    return scenePath.parent_path() / (scenePath.stem().string() + "_Zones");
}

std::filesystem::path StagedZonePayloadPathFor(const std::filesystem::path& livePayloadPath)
{
    return livePayloadPath.string() + kStagedSuffix;
}

SceneRecoveryScan ScanForSceneRecovery(const std::filesystem::path& scenePath)
{
    SceneRecoveryScan scan;
    scan.ScenePath = scenePath;
    scan.BackupScenePath = SceneBackupPathFor(scenePath);

    std::error_code ec;
    if (std::filesystem::exists(scan.BackupScenePath, ec) && std::filesystem::exists(scenePath, ec))
    {
        const auto backupTime = std::filesystem::last_write_time(scan.BackupScenePath, ec);
        if (!ec)
        {
            const auto sceneTime = std::filesystem::last_write_time(scenePath, ec);
            scan.BackupSceneUsable = !ec && backupTime > sceneTime;
        }
    }

    const std::filesystem::path zonesDir = SceneZonesFolderFor(scenePath);
    if (std::filesystem::is_directory(zonesDir, ec))
    {
        for (const auto& entry : std::filesystem::directory_iterator(zonesDir, ec))
        {
            if (!entry.is_regular_file(ec))
                continue;
            const std::string name = entry.path().filename().string();
            if (EndsWith(name, std::string(".tzone") + kStagedSuffix)
                || EndsWith(name, std::string(".tsculpt") + kStagedSuffix))
                scan.StagedPayloads.push_back(entry.path());
        }
    }
    return scan;
}

bool PromoteStagedZonePayloads(const std::vector<std::filesystem::path>& stagedPayloads)
{
    bool allOk = true;
    for (const auto& staged : stagedPayloads)
    {
        const std::string s = staged.string();
        if (!EndsWith(s, kStagedSuffix))
        {
            Logger::Log::Warning("Scene recovery: '{}' is not a staged payload — skipped", s);
            allOk = false;
            continue;
        }
        const std::filesystem::path live = s.substr(0, s.size() - std::string(kStagedSuffix).size());
        if (!GameEngine::FileSystem::PublishFile(staged, live))
        {
            Logger::Log::Warning("Scene recovery: promote {} -> {} failed", s, live.string());
            allOk = false;
        }
    }
    return allOk;
}

void DiscardSceneBackups(const SceneRecoveryScan& scan)
{
    for (const auto& staged : scan.StagedPayloads)
    {
        RemoveQuiet(staged);
        RemoveQuiet(staged.string() + kAtomicTempSuffix);
    }
    if (!scan.BackupScenePath.empty())
        RemoveQuiet(scan.BackupScenePath);
}

SceneRecoveryResolution ResolveRecoveryDecision(const SceneRecoveryScan& scan, bool restore)
{
    SceneRecoveryResolution r;
    r.DocumentPath = scan.ScenePath;
    if (restore)
    {
        if (!PromoteStagedZonePayloads(scan.StagedPayloads))
            Logger::Log::Warning("Scene recovery: some staged payloads could not be promoted");
        r.OpenedFromBackup = scan.BackupSceneUsable;
        r.ContentPathToOpen = scan.BackupSceneUsable ? scan.BackupScenePath : scan.ScenePath;
        // The staged payloads were consumed by promotion; the backup scene stays
        // until the recovered session closes cleanly (it is the only copy of the
        // recovered structure until the user saves).
        return r;
    }
    DiscardSceneBackups(scan);
    r.OpenedFromBackup = false;
    r.ContentPathToOpen = scan.ScenePath;
    return r;
}

} // namespace GameEngine::Editor
