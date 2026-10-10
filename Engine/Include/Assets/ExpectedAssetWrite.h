#pragma once

#include "Assets/ExpectedWriteLedger.h"

#include <filesystem>

namespace GameEngine
{

class AssetManager;

/**
 * @brief A write this process is about to make, announced before the file is touched.
 *
 * Obtained from AssetManager::ExpectWrite or ExpectWritesUnder, held across the write,
 * and dropped once the bytes are reported:
 *
 * @code
 * auto write = assets.ExpectWrite(scenePath);
 * if (!SaveTo(scenePath))
 *     return false;
 * write.Report(scenePath);
 * @endcode
 *
 * Report is the change drive — it runs the same registry sync, asset-event dispatch and
 * reload mark a file-watcher event runs, on the calling thread, on every host. The
 * announcement is what tells the watcher's report of the same write that the write has
 * already been delivered, so the pipeline runs once per save whether or not the host has
 * a watcher at all.
 *
 * Destroying the handle without reporting says the write did not happen: the
 * announcement is dropped and the next change to that path is delivered normally.
 */
class ExpectedAssetWrite
{
  public:
    ~ExpectedAssetWrite();

    ExpectedAssetWrite(const ExpectedAssetWrite&) = delete;
    ExpectedAssetWrite& operator=(const ExpectedAssetWrite&) = delete;
    ExpectedAssetWrite(ExpectedAssetWrite&&) = delete;
    ExpectedAssetWrite& operator=(ExpectedAssetWrite&&) = delete;

    /**
     * @brief The bytes are on disk: drive the asset pipeline for @p written now.
     *
     * Called once per file the write produced — once for a saved file, once per copied
     * file for a tree announced with ExpectWritesUnder.
     */
    void Report(const std::filesystem::path& written);

  private:
    friend class AssetManager;
    ExpectedAssetWrite(AssetManager& assets, ExpectedWriteLedger::Registration id);

    AssetManager& m_Assets;
    ExpectedWriteLedger::Registration m_Id;
};

} // namespace GameEngine
