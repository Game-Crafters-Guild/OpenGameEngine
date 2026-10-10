/**
 * @file Utils.h
 * @brief Utility functions for the rendering library
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>

namespace GameEngine {
namespace Rendering {

    enum class ShaderSourceKind : uint8_t;

    /**
     * @brief Utility functions for rendering
     */
    namespace Utils {

        // Host-provided shader bytecode loader (SPIR-V, etc).
        // Higher-level code (Engine/Editor) routes shader loads through
        // AssetManager; tests and examples wire their own loaders.
        using ShaderFileLoaderFunc = std::vector<uint8_t> (*)(const char* name);
        void SetShaderFileLoader(ShaderFileLoaderFunc loader);
        /// Current host callback, for preserving a scoped host's predecessor.
        ShaderFileLoaderFunc GetShaderFileLoader();

        // Host-provided shader path resolver. Given a relative path, returns an
        // absolute filesystem path. Without one, ResolveShaderPath can only answer
        // for absolute paths: it reports the miss and returns empty.
        using ShaderPathResolverFunc = std::filesystem::path (*)(const std::filesystem::path& relativePath);
        void SetShaderPathResolver(ShaderPathResolverFunc resolver);
        /// Current host callback, for preserving a scoped host's predecessor.
        ShaderPathResolverFunc GetShaderPathResolver();

        // Whether a host has installed a resolver yet. The two ways ResolveShaderPath
        // returns empty mean opposite things: with a resolver the shader is genuinely
        // absent and asking again cannot help; without one nobody has been able to look
        // yet, and an early frame must ask again. A caller that latches a missing shader
        // has to tell them apart.
        [[nodiscard]] bool HasShaderPathResolver();

        // The shader form the active device ingests. A stage shader outside the
        // .shaderpkg path is asked for by its .spv name; where the device ingests
        // WGSL, LoadShaderFile serves the cooked ".wgsl" sibling of that name first.
        // Set by RenderServices when it takes a device; SPIR-V until then.
        void SetPreferredShaderSource(ShaderSourceKind kind);

        // String utilities
        std::string FormatString(const char* format, ...);
        std::vector<std::string> SplitString(const std::string& str, char delimiter);
        std::string ToUpper(const std::string& str);

        // File utilities
        bool FileExists(const std::string& path) noexcept;
        [[nodiscard]] std::vector<uint8_t> ReadFile(const std::string& path);
        [[nodiscard]] bool WriteFile(const std::string& path, const std::vector<uint8_t>& data);

        // Shader / binary utilities. An absent shader is reported as an empty
        // blob and an error log, never a throw: callers treat empty as "this
        // shader is unavailable" and degrade.
        [[nodiscard]] std::vector<uint8_t> LoadShaderFile(const char* name);
        // Resolve a shader file path (SPIR-V or source) from common runtime locations.
        [[nodiscard]] std::string ResolveShaderPath(const char* name);


        // Memory utilities
        template<typename T>
        std::unique_ptr<T> MakeUnique(T&& value) {
            return std::make_unique<T>(std::forward<T>(value));
        }

        // Alignment utilities
        constexpr size_t AlignUp(size_t value, size_t alignment) {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        constexpr size_t AlignDown(size_t value, size_t alignment) {
            return value & ~(alignment - 1);
        }
        // Note: AlignUp/AlignDown require power-of-two alignment. Use these for general alignments.
        constexpr size_t AlignUpTo(size_t value, size_t alignment) {
            return alignment ? ((value + alignment - 1) / alignment) * alignment : value;
        }
        constexpr size_t AlignDownTo(size_t value, size_t alignment) {
            return alignment ? (value / alignment) * alignment : value;
        }


        // Hash utilities
        [[nodiscard]] size_t HashString(const std::string& str) noexcept;
        [[nodiscard]] size_t HashBytes(const void* data, size_t size) noexcept;

        // Time utilities
        double GetCurrentTimeSeconds() noexcept;
        uint64_t GetCurrentTimeMilliseconds() noexcept;
        uint64_t GetCurrentTimeMicroseconds() noexcept;
    }

} // namespace Rendering
} // namespace GameEngine
