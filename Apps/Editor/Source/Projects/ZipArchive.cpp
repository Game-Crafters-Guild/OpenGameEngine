#include "Projects/ZipArchive.h"

#include "Logger/Logger.h"

#include <miniz.h>

#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

namespace GameEngine::Editor
{

namespace fs = std::filesystem;

namespace
{

// Zip entry names use '/' separators. Reject anything that could escape the
// destination (zip-slip) or that this extractor doesn't model.
bool IsSafeEntryName(const std::string& name)
{
    if (name.empty() || name.front() == '/' ||
        name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= name.size())
    {
        const size_t end = name.find('/', start);
        const std::string segment =
            name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment == ".." || segment == ".")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

FILE* OpenZipFile(const fs::path& zipFile)
{
#if defined(_WIN32)
    // fopen takes ACP-encoded chars on Windows; go wide so non-ASCII user
    // paths (cache lives under the user profile) open correctly.
    return _wfopen(zipFile.c_str(), L"rb");
#else
    return std::fopen(zipFile.c_str(), "rb");
#endif
}

} // namespace

bool ExtractZipArchive(const fs::path& zipFile, const fs::path& destination,
                       const std::string& innerRoot, std::string* outError)
{
    FILE* file = OpenZipFile(zipFile);
    if (!file)
    {
        if (outError)
            *outError = "Could not open archive: " + zipFile.generic_string();
        return false;
    }

    mz_zip_archive archive{};
    if (!mz_zip_reader_init_cfile(&archive, file, 0, 0))
    {
        std::fclose(file);
        if (outError)
            *outError = "Not a valid .zip archive.";
        return false;
    }

    auto fail = [&](std::string message) {
        mz_zip_reader_end(&archive);
        std::fclose(file);
        if (outError)
            *outError = std::move(message);
        return false;
    };

    const std::string prefix = innerRoot.empty() ? std::string{} : innerRoot + "/";
    size_t extractedCount = 0;

    const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
    for (mz_uint index = 0; index < fileCount; ++index)
    {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat))
            return fail("Could not read the archive's file table.");

        std::string name = stat.m_filename;
        if (!IsSafeEntryName(name))
        {
            Logger::Log::Warning("Zip extraction skipped unsafe entry '{}'", name);
            continue;
        }
        if (!prefix.empty())
        {
            if (name.rfind(prefix, 0) != 0)
                continue;
            name = name.substr(prefix.size());
            if (name.empty())
                continue;
        }

        const fs::path target = destination / fs::path(name);
        std::error_code ec;
        if (mz_zip_reader_is_file_a_directory(&archive, index))
        {
            fs::create_directories(target, ec);
            continue;
        }

        fs::create_directories(target.parent_path(), ec);

        size_t uncompressedSize = 0;
        void* data = mz_zip_reader_extract_to_heap(&archive, index, &uncompressedSize, 0);
        if (!data)
            return fail("Could not extract '" + name + "' — the archive may be corrupt.");

        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        const bool written =
            out && out.write(static_cast<const char*>(data),
                             static_cast<std::streamsize>(uncompressedSize))
                       .good();
        mz_free(data);
        if (!written)
            return fail("Could not write '" + target.generic_string() + "'.");
        ++extractedCount;
    }

    mz_zip_reader_end(&archive);
    std::fclose(file);

    if (extractedCount == 0)
    {
        if (outError)
            *outError = innerRoot.empty() ? "The archive is empty."
                                          : "The archive has no folder '" + innerRoot + "'.";
        return false;
    }
    return true;
}

} // namespace GameEngine::Editor
