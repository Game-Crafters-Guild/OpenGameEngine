# FileWatchingService

Centralized file watching with subscription/observer pattern. Consolidates multiple watchers, provides debouncing and pattern-based filtering, and reduces resource usage.

## Usage
```cpp
auto& fws = GameEngine::FileWatchingService::GetInstance();
GameEngine::FilePattern pattern(dir, ".*", {".dll"}, /*recursive=*/true);
auto sub = fws.Subscribe(pattern, [](const FileChangeEvent& evt){ /* handle */ });
fws.StartWatching();
// ... later
fws.StopWatching();
```

## FilePattern fields
- directory: root directory to watch
- filenamePattern: std::regex applied to the filename (no path)
- extensions: optional set of extensions to include (e.g., {".dll", ".txt"}); empty = all
- recursive: when false, only matches files directly under directory

## Match semantics
- Matches() returns true only if:
  - filePath is inside directory (lexically),
  - recursive is true or the file is not in a subdirectory,
  - extensions is empty or file extension is contained in extensions,
  - filename regex matches the filename.

See Tests/Assets/FilePatternTests.cpp for examples.

