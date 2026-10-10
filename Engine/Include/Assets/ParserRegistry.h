#pragma once

#include "Types/Types.h"
#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "AssetCore/DepEdge.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

namespace GameEngine {

    // Forward declarations
    class AssetManager;
    struct AssetMetadata;

    /**
     * @brief Result of asset parsing operation
     */
    enum class AssetParseStatus {
        Success,
        NotForMe,
        Error
    };

    struct AssetParseResult {
        AssetParseStatus Status = AssetParseStatus::Error;
        bool Success = false;
        std::string ErrorMessage;
        std::shared_ptr<Asset> ParsedAsset;

        AssetParseResult() = default;
        AssetParseResult(bool success, const std::string& error = "")
            : Status(success ? AssetParseStatus::Success : AssetParseStatus::Error),
              Success(success),
              ErrorMessage(error) {}
        AssetParseResult(std::shared_ptr<Asset> asset)
            : Status(AssetParseStatus::Success),
              Success(true),
              ParsedAsset(std::move(asset)) {}

        static AssetParseResult NotForMe(const std::string& reason = "")
        {
            AssetParseResult r;
            r.Status = AssetParseStatus::NotForMe;
            r.Success = false;
            r.ErrorMessage = reason;
            return r;
        }
    };

    /**
     * @brief Abstract base class for asset parsers
     */
    class AssetParser {
    public:
        virtual ~AssetParser() = default;

        /**
         * @brief Get the asset type this parser handles
         */
        virtual AssetType GetAssetType() const = 0;

        /**
         * @brief Get file extensions this parser supports
         */
        virtual std::vector<std::string> GetSupportedExtensions() const = 0;

        /**
         * @brief Check if this parser can handle the given file
         * @param filePath Path to the file to check
         * @return True if this parser can handle the file
         */
        virtual bool CanParse(const std::filesystem::path& filePath) const;

        /**
         * @brief Parse an asset from file
         * @param metadata Asset metadata containing file path and other info
         * @param assetManager Reference to asset manager for dependencies
         * @return Parse result with success status and asset or error
         */
        virtual AssetParseResult Parse(const AssetMetadata& metadata, AssetManager& assetManager) = 0;

        /// Physical payload to read; metadata.Path remains the logical asset identity.
        virtual std::filesystem::path ResolveReadPath(const AssetMetadata& metadata,
                                                      [[maybe_unused]] const AssetManager& assetManager) const
        {
            return metadata.Path;
        }

        /**
         * @brief Get parser priority (higher = preferred for same extension)
         */
        virtual int GetPriority() const { return 0; }

        /**
         * @brief Get parser name for debugging
         */
        virtual std::string GetName() const = 0;

        /**
         * @brief Compute a format-aware identity hash for file reconciliation.
         *
         * Parsers can override this to provide an efficient, format-specific hash
         * that identifies a file without reading the entire contents. For example:
         * - GLTF/GLB: hash the JSON metadata header
         * - PNG: hash the IHDR chunk
         * - FBX: hash the header block
         *
         * The base implementation returns empty string, which signals the caller
         * to use the generic sparse sampling hash as a fallback.
         *
         * @param filePath Path to the file to hash
         * @param fileSize Size of the file in bytes
         * @return A hex string hash, or empty string if not supported
         */
        virtual std::string ComputeIdentityHash([[maybe_unused]] const std::filesystem::path& filePath,
                                                [[maybe_unused]] int64_t fileSize) const
        {
            return {};
        }

        /**
         * @brief Extract the assets this referrer depends on, into a sink.
         *
         * Called during scan/import. Implementations should walk the referrer's
         * format-specific structure and emit a DepEdge for each outgoing
         * reference (e.g. a material's texture refs, a scene's component
         * refs, a prefab's child refs).
         *
         * @param referrerGuid Asset whose dependencies we want.
         * @param metadata Parsed metadata for the referrer.
         * @param sink Receives one Emit() per outgoing edge.
         * @return true if this parser implements format-aware extraction
         *         (even if no edges were emitted — that means "I scanned and
         *         found none"). false from the default base impl tells the
         *         caller to fall back to AssetDependencyExtractor's
         *         syntactic GUID/path-text scanning.
         *
         * Phase 3 staged migration: parsers opt in by overriding and
         * returning true. Until they do, the syntactic fallback in the
         * registry continues to provide simple (referrer, target) edges
         * with kind=Other and no field locator.
         */
        virtual bool ExtractDependencies([[maybe_unused]] const GUID& referrerGuid,
                                         [[maybe_unused]] const AssetMetadata& metadata,
                                         [[maybe_unused]] DepEdgeSink& sink) const
        {
            // Default: no extraction. Caller falls back to syntactic.
            return false;
        }

        /**
         * @brief Rewrite a staged copy of an asset into the form a packaged game loads.
         *
         * The build calls this for every staged asset after the copy and before the package is
         * published; the parser then loads the cooked form in the packaged game. The default
         * ships the file as authored.
         *
         * @param stagedFile The copy in the staging tree, rewritten in place.
         * @param error Receives why the asset cannot ship.
         * @return false when the asset cannot ship; the build then fails.
         */
        virtual bool CookForPackage([[maybe_unused]] const std::filesystem::path& stagedFile,
                                    [[maybe_unused]] std::string& error) const
        {
            return true;
        }
    };

    /**
     * @brief Parser registration information
     */
    struct ParserRegistration {
        std::shared_ptr<AssetParser> Parser;
        std::vector<std::string> Extensions;
        int Priority;

        ParserRegistration(std::shared_ptr<AssetParser> p, const std::vector<std::string>& ext, int Prio = 0)
            : Parser(std::move(p)), Extensions(ext), Priority(Prio) {}
    };

    /**
     * @brief Registry for asset parsers with pluggable architecture
     */
    class ParserRegistry {
    public:
        ParserRegistry();
        ~ParserRegistry() = default;

        /**
         * @brief Initialize the parser registry with default parsers
         */
        bool Initialize();

        /**
         * @brief Shutdown the parser registry
         */
        void Shutdown();

        /**
         * @brief Register a parser for the extensions it reports in GetSupportedExtensions()
         * @param parser The parser to register
         * @param priority Parser priority (higher = preferred for same extension); 0 uses parser->GetPriority()
         * @return True if registration was successful
         */
        bool RegisterParser(std::shared_ptr<AssetParser> parser, int priority = 0);

        /**
         * @brief Unregister a parser
         * @param parser The parser to unregister
         */
        void UnregisterParser(std::shared_ptr<AssetParser> parser);

        /**
         * @brief Get asset type for a file extension
         * @param extension File extension (e.g., ".xml")
         * @return Asset type or AssetType::Unknown if no parser found
         */
        AssetType GetAssetTypeFromExtension(const std::string& extension) const;

        /**
         * @brief Find the best parser for a file
         * @param filePath Path to the file
         * @return Best parser or nullptr if none found
         */
        std::shared_ptr<AssetParser> FindParser(const std::filesystem::path& filePath) const;

        /// Resolve the parser-owned payload before an asynchronous read.
        std::filesystem::path ResolveReadPath(const AssetMetadata& metadata, const AssetManager& assetManager) const;

        /**
         * @brief Find all parsers for a file extension
         * @param extension File extension
         * @return Vector of parsers sorted by priority (highest first)
         */
        std::vector<std::shared_ptr<AssetParser>> FindParsers(const std::string& extension) const;

        /**
         * @brief Parse an asset using the best available parser
         * @param metadata Asset metadata
         * @param assetManager Asset manager reference
         * @return Parse result
         */
        AssetParseResult ParseAsset(const AssetMetadata& metadata, AssetManager& assetManager) const;

        /**
         * @brief Get all registered parsers
         */
        std::vector<ParserRegistration> GetAllParsers() const;

        /**
         * @brief Get number of registered parsers
         */
        size_t GetParserCount() const;

        /**
         * @brief Check if a parser is registered for the given extension
         */
        bool HasParserForExtension(const std::string& extension) const;

        /**
         * @brief Load parser plugins from a directory.
         *
         * A plugin is a shared library that exports:
         *   extern "C" void GE_RegisterAssetParsers(GameEngine::ParserRegistry* registry);
         *
         * The plugin is expected to call RegisterParser(...) on the provided registry.
         *
         * @param directory Directory containing plugin shared libraries
         * @return True if all plugins loaded successfully (missing directory is treated as success)
         */
        bool LoadPluginsFromDirectory(const std::filesystem::path& directory);

        /**
         * @brief Load parser plugins from environment variable GE_ASSET_PARSER_PLUGINS_DIR
         *
         * Multiple directories can be separated by ';'.
         */
        void LoadPluginsFromEnvironment();

    private:
        bool m_Initialized = false;
        
        // Map from file extension to list of parsers (sorted by priority).
        struct ParserEntry
        {
            std::shared_ptr<AssetParser> Parser;
            int Priority = 0;
        };
        std::unordered_map<std::string, std::vector<ParserEntry>> m_ExtensionParsers;
        
        // All registered parsers
        std::vector<ParserRegistration> m_Registrations;

        // Loaded parser plugin library handles (kept alive for the lifetime of the registry).
        std::vector<void*> m_PluginHandles;

        /**
         * @brief Register default parsers for built-in asset types
         */
        void RegisterDefaultParsers();

        /**
         * @brief Normalize file extension (lowercase, ensure starts with '.')
         */
        std::string NormalizeExtension(const std::string& extension) const;

        /**
         * @brief Sort parsers by priority (highest first)
         */
        void SortParsersByPriority(std::vector<ParserEntry>& parsers) const;
    };

} // namespace GameEngine
