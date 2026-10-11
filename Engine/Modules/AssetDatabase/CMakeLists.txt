cmake_minimum_required(VERSION 3.20)

project(AssetDatabase LANGUAGES CXX)

set(ASSETDATABASE_SOURCES
    Source/AssetDatabasePaths.cpp
    Source/AssetStore_TextJsonl.cpp
    Source/AssetSourceSnapshot.cpp
    Source/AssetStoreReconciler.cpp
    Source/RedirectChain.cpp
)

set(ASSETDATABASE_HEADERS
    Include/AssetDatabase/AssetDatabasePaths.h
    Include/AssetDatabase/IAssetDbCache.h
    Include/AssetDatabase/AssetRecord.h
    Include/AssetDatabase/IAssetStore.h
    Include/AssetDatabase/AssetStore_TextJsonl.h
    Include/AssetDatabase/AssetSourceSnapshot.h
    Include/AssetDatabase/AssetStoreReconciler.h
    Include/AssetDatabase/RedirectChain.h
)

add_library(AssetDatabase OBJECT ${ASSETDATABASE_SOURCES} ${ASSETDATABASE_HEADERS})

set_target_properties(AssetDatabase PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_features(AssetDatabase PUBLIC cxx_std_20)

target_include_directories(AssetDatabase
    PUBLIC
        Include
    PRIVATE
        Source
)

target_link_libraries(AssetDatabase
    PUBLIC
        Logger
        AssetCore
        Types
    PRIVATE
        Platform
        FileSystem
        # Large store and snapshot loads parse on the pool; objects link once into Engine.
        JobSystem
)

# JSON is used for the authoritative JSONL store (implementation-only).
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(AssetDatabase PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(AssetDatabase PRIVATE /W4)
else()
    target_compile_options(AssetDatabase PRIVATE -Wall -Wextra -Wpedantic)
endif()

message(STATUS "AssetDatabase: Configured as static library")

# ----------------------------------------------------------------------------
# AssetDatabaseSqlite: the concrete SQLite implementation of IAssetDbCache.
#
# Kept in a SEPARATE library so the shared Engine.dll (and the standalone
# Player) link NO SQL engine. Only editor/tooling/test targets link this
# library and inject the factory via AssetRegistry::SetAssetDbCacheFactory.
# ----------------------------------------------------------------------------
add_library(AssetDatabaseSqlite STATIC
    Source/AssetDbCache_Sqlite.cpp
    Include/AssetDatabase/AssetDbCache_Sqlite.h
)

set_target_properties(AssetDatabaseSqlite PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_features(AssetDatabaseSqlite PUBLIC cxx_std_20)

target_include_directories(AssetDatabaseSqlite
    PUBLIC
        Include
    PRIVATE
        Source
)

# Mirrors AssetDatabase's own public deps (AssetCore brings GUID + DepEdge;
# AssetDatabase's headers provide AssetRecord + IAssetDbCache). We deliberately
# do NOT link the AssetDatabase OBJECT library here — that would splice its
# .obj files (and their file-scope statics) into this static lib, and since
# editor targets link BOTH Engine.dll (which already owns those objects) and
# AssetDatabaseSqlite, that produces duplicate-symbol / split-singleton bugs.
# The implementation only needs AssetDatabase's HEADERS, so propagate just its
# include dirs via the OBJECT target's INTERFACE_INCLUDE_DIRECTORIES.
target_link_libraries(AssetDatabaseSqlite
    PUBLIC
        Logger
        AssetCore
        Types
)
target_include_directories(AssetDatabaseSqlite
    PUBLIC
        $<TARGET_PROPERTY:AssetDatabase,INTERFACE_INCLUDE_DIRECTORIES>
)

find_package(SQLite3 REQUIRED)
target_link_libraries(AssetDatabaseSqlite PRIVATE SQLite::SQLite3 nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(AssetDatabaseSqlite PRIVATE /W4)
else()
    target_compile_options(AssetDatabaseSqlite PRIVATE -Wall -Wextra -Wpedantic)
endif()

message(STATUS "AssetDatabaseSqlite: Configured as static library")


