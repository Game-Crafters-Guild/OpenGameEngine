#pragma once

#include "AssetCore/Types.h"
#include <array>

namespace GameEngine {

/**
 * @brief 128-bit Globally Unique Identifier for assets
 */
class GUID {
public:
    static constexpr size_t kSize = 16; // 128 bits
    using Data = std::array<uint8, kSize>;

    /**
     * @brief Default constructor - creates null GUID
     */
    GUID();

    /**
     * @brief Constructor from raw data
     */
    explicit GUID(const Data& data);

    /**
     * @brief Construct from a 16-byte C array (component-field storage).
     */
    static GUID FromBytes(const uint8 (&bytes)[kSize]);

    /**
     * @brief Write the 16 raw bytes into a C array (component-field storage).
     */
    void WriteBytes(uint8 (&out)[kSize]) const;

    /**
     * @brief Constructor from string representation
     */
    explicit GUID(const String& str);

    /**
     * @brief Generate a new random (RFC 4122 version 4) GUID: 122 random bits.
     * Thread-safe without locking: every thread draws from its own generator.
     */
    static GUID Generate();

    // Deterministically derive a child GUID from a parent GUID and a sub-key (e.g., "animations/Walk")
    static GUID Derive(const GUID& parent, const String& subKey);

    /**
     * @brief Create null GUID (all zeros)
     */
    static GUID Null();

    /**
     * @brief Check if GUID is null
     */
    bool IsNull() const;

    /**
     * @brief Convert to string representation
     * Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
     */
    String ToString() const;

    /**
     * @brief Convert to compact string (no dashes)
     */
    String ToCompactString() const;

    /**
     * @brief Get raw data
     */
    const Data& GetData() const { return m_Data; }

    /**
     * @brief Comparison operators
     */
    bool operator==(const GUID& other) const;
    bool operator!=(const GUID& other) const;
    bool operator<(const GUID& other) const;

private:
    Data m_Data;
};

} // namespace GameEngine

// Hash specialization for std::unordered_map.
//
// Defined inline so every translation unit that includes this header
// instantiates it locally — Engine.dll's SHARED export filter aggressively
// drops template-class members from the export table (see
// cmake/GenerateEngineExportsDef.cmake), so out-of-line specializations would
// only live inside Engine.dll's binary and be unresolvable from Editor.exe /
// GameEngine.Native.dll. Inline keeps it linker-local in every consumer.
namespace std {
    template<>
    struct hash<GameEngine::GUID> {
        size_t operator()(const GameEngine::GUID& guid) const noexcept {
            const auto& data = guid.GetData();
            size_t h = 0;
            // FNV-1a over the 16 GUID bytes.
            for (auto byte : data) {
                h ^= static_cast<size_t>(byte);
                h *= 1099511628211ULL;
            }
            return h;
        }
    };
}
