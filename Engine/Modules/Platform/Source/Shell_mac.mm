#include "Platform/Shell.h"

#import <Cocoa/Cocoa.h>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace Platform
{

namespace
{

void ApplyOpenPanelInitialDirectory(NSOpenPanel* panel, const std::filesystem::path& initialPath)
{
    if (initialPath.empty())
        return;

    std::error_code ec;
    std::filesystem::path pathToUse = initialPath;
    if (std::filesystem::is_directory(pathToUse, ec))
    {
        // Use the directory directly
    }
    else if (std::filesystem::exists(pathToUse, ec))
    {
        pathToUse = pathToUse.parent_path();
    }
    else
    {
        pathToUse = pathToUse.parent_path();
        if (!std::filesystem::exists(pathToUse, ec))
            pathToUse.clear();
    }

    if (pathToUse.empty())
        return;

    NSString* nsPath = [NSString stringWithUTF8String:pathToUse.string().c_str()];
    NSURL* url = [NSURL fileURLWithPath:nsPath isDirectory:YES];
    if (url)
        [panel setDirectoryURL:url];
}

std::vector<std::filesystem::path> CollectOpenPanelPaths(NSOpenPanel* panel)
{
    std::vector<std::filesystem::path> result;
    NSArray<NSURL*>* urls = [panel URLs];
    if (!urls)
        return result;
    result.reserve([urls count]);
    for (NSURL* url in urls)
    {
        NSString* path = [url path];
        if (path)
            result.emplace_back([path UTF8String]);
    }
    return result;
}

} // namespace

// Internal implementation function (called from Shell.cpp)
std::filesystem::path SelectFolder_macOS(const std::filesystem::path& initialPath)
{
    // Use NSOpenPanel for folder selection on macOS
    std::filesystem::path result;
    
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        [panel setCanCreateDirectories:YES]; // Enable "New Folder" button
        [panel setTitle:@"Select Project Folder"];
        [panel setPrompt:@"Select"];
        ApplyOpenPanelInitialDirectory(panel, initialPath);

        if ([panel runModal] == NSModalResponseOK)
        {
            NSURL* url = [[panel URLs] firstObject];
            if (url)
            {
                NSString* path = [url path];
                if (path)
                {
                    result = std::filesystem::path([path UTF8String]);
                }
            }
        }
    }
    
    return result;
}

std::vector<std::filesystem::path> SelectFiles_macOS(const std::filesystem::path& initialPath,
                                                     const char* filterName,
                                                     const char* filterPattern)
{
    (void)filterName;
    (void)filterPattern;

    std::vector<std::filesystem::path> result;

    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:YES];
        [panel setTitle:@"Select Files"];
        [panel setPrompt:@"Select"];
        ApplyOpenPanelInitialDirectory(panel, initialPath);

        if ([panel runModal] == NSModalResponseOK)
            result = CollectOpenPanelPaths(panel);
    }

    return result;
}

std::filesystem::path SelectFile_macOS(const std::filesystem::path& initialPath,
                                       const char* filterName,
                                       const char* filterPattern)
{
    (void)filterName;
    (void)filterPattern;

    std::filesystem::path result;

    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setTitle:@"Select File"];
        [panel setPrompt:@"Select"];
        ApplyOpenPanelInitialDirectory(panel, initialPath);

        if ([panel runModal] == NSModalResponseOK)
        {
            auto files = CollectOpenPanelPaths(panel);
            if (!files.empty())
                result = std::move(files.front());
        }
    }

    return result;
}

std::filesystem::path SaveFile_macOS(const std::filesystem::path& initialPath,
                                     const char* filterName,
                                     const char* filterPattern)
{
    (void)filterName;
    (void)filterPattern;

    std::filesystem::path result;

    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        [panel setCanCreateDirectories:YES];
        [panel setTitle:@"Save File"];
        [panel setPrompt:@"Save"];

        // Set initial directory/filename if provided
        if (!initialPath.empty())
        {
            std::error_code ec;
            std::filesystem::path pathToUse = initialPath;

            if (std::filesystem::is_directory(pathToUse, ec))
            {
                // It's a directory, use it directly
                NSString* nsPath = [NSString stringWithUTF8String:pathToUse.string().c_str()];
                NSURL* url = [NSURL fileURLWithPath:nsPath isDirectory:YES];
                if (url)
                {
                    [panel setDirectoryURL:url];
                }
            }
            else
            {
                // Try to extract directory and filename
                std::filesystem::path dir = pathToUse.parent_path();
                std::filesystem::path filename = pathToUse.filename();

                if (!dir.empty())
                {
                    NSString* nsDir = [NSString stringWithUTF8String:dir.string().c_str()];
                    NSURL* dirUrl = [NSURL fileURLWithPath:nsDir isDirectory:YES];
                    if (dirUrl)
                    {
                        [panel setDirectoryURL:dirUrl];
                    }
                }

                if (!filename.empty())
                {
                    NSString* nsFilename = [NSString stringWithUTF8String:filename.string().c_str()];
                    [panel setNameFieldStringValue:nsFilename];
                }
            }
        }

        if ([panel runModal] == NSModalResponseOK)
        {
            NSURL* url = [panel URL];
            if (url)
            {
                NSString* path = [url path];
                if (path)
                {
                    result = std::filesystem::path([path UTF8String]);
                }
            }
        }
    }

    return result;
}

bool MoveToTrash_macOS(const std::filesystem::path& path)
{
    if (path.empty())
        return false;

    @autoreleasepool {
        NSString* nsPath = [NSString stringWithUTF8String:path.string().c_str()];
        if (!nsPath)
            return false;
        NSURL* url = [NSURL fileURLWithPath:nsPath];
        if (!url)
            return false;
        NSError* error = nil;
        const BOOL ok = [[NSFileManager defaultManager] trashItemAtURL:url
                                                      resultingItemURL:nil
                                                                 error:&error];
        return ok == YES;
    }
}

} // namespace Platform
} // namespace GameEngine
