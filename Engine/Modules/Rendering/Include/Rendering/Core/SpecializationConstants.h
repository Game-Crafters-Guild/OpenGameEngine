#pragma once

#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>
#include <type_traits>
#include <cstring>

namespace GameEngine {
namespace Rendering {

    /**
     * @brief Enhanced Specialization Constants Manager
     * 
     * Provides type-safe, high-performance specialization constant management
     * for Vulkan shaders. Reduces shader variants and improves performance.
     * 
     * Features:
     * - Type-safe constant definition
     * - Automatic layout management
     * - Shader variant optimization
     * - Runtime constant updates
     * - Template-based API for ease of use
     */
    class SpecializationConstants {
    public:
        /**
         * @brief Specialization constant entry
         */
        struct Entry {
            uint32_t ConstantId;
            uint32_t Offset;
            size_t Size;
            std::string Name; // For debugging
        };

        SpecializationConstants() = default;
        ~SpecializationConstants() = default;

        /**
         * @brief Add a specialization constant
         * @tparam T Type of the constant (must be POD)
         * @param constantId Shader constant ID
         * @param value Initial value
         * @param name Debug name for the constant
         */
        template<typename T>
        void AddConstant(uint32_t constantId, const T& value, const std::string& name = "") {
            static_assert(std::is_trivially_copyable_v<T>, "Specialization constants must be trivially copyable");
            
            Entry entry;
            entry.ConstantId = constantId;
            entry.Offset = static_cast<uint32_t>(m_Data.size());
            entry.Size = sizeof(T);
            entry.Name = name.empty() ? ("Constant_" + std::to_string(constantId)) : name;

            // Add to data buffer
            size_t oldSize = m_Data.size();
            m_Data.resize(oldSize + sizeof(T));
            std::memcpy(m_Data.data() + oldSize, &value, sizeof(T));

            // Store entry
            m_Entries[constantId] = entry;
        }

        /**
         * @brief Update a specialization constant value
         * @tparam T Type of the constant
         * @param constantId Shader constant ID
         * @param value New value
         */
        template<typename T>
        bool UpdateConstant(uint32_t constantId, const T& value) {
            auto it = m_Entries.find(constantId);
            if (it == m_Entries.end()) {
                return false;
            }

            const Entry& entry = it->second;
            if (entry.Size != sizeof(T)) {
                return false; // Type mismatch
            }

            std::memcpy(m_Data.data() + entry.Offset, &value, sizeof(T));
            return true;
        }

        /**
         * @brief Get a specialization constant value
         * @tparam T Type of the constant
         * @param constantId Shader constant ID
         * @param outValue Output value
         * @return true if constant exists and type matches
         */
        template<typename T>
        bool GetConstant(uint32_t constantId, T& outValue) const {
            auto it = m_Entries.find(constantId);
            if (it == m_Entries.end()) {
                return false;
            }

            const Entry& entry = it->second;
            if (entry.Size != sizeof(T)) {
                return false; // Type mismatch
            }

            std::memcpy(&outValue, m_Data.data() + entry.Offset, sizeof(T));
            return true;
        }

        /**
         * @brief Remove a specialization constant
         * @param constantId Shader constant ID
         */
        void RemoveConstant(uint32_t constantId);

        /**
         * @brief Clear all specialization constants
         */
        void Clear();

        // Backend-agnostic accessors should be implemented in backend-specific layers.

        /**
         * @brief Check if there are any constants defined
         */
        bool IsEmpty() const { return m_Entries.empty(); }

        /**
         * @brief Get number of constants
         */
        size_t GetConstantCount() const { return m_Entries.size(); }

        /**
         * @brief Get total data size
         */
        size_t GetDataSize() const { return m_Data.size(); }

        /**
         * @brief Get raw data pointer (may be null if empty)
         */
        const uint8_t* GetDataPtr() const { return m_Data.empty() ? nullptr : m_Data.data(); }

        /**
         * @brief Get all constant entries (for debugging)
         */
        const std::unordered_map<uint32_t, Entry>& GetEntries() const { return m_Entries; }

        /**
         * @brief Create a copy with different values
         */
        SpecializationConstants CreateVariant() const;

        /**
         * @brief Merge constants from another set
         * @param other Other specialization constants
         * @param overwrite Whether to overwrite existing constants
         */
        void Merge(const SpecializationConstants& other, bool overwrite = false);

        /**
         * @brief Generate a hash for shader variant management
         */
        uint64_t GetHash() const;

        /**
         * @brief Debug print all constants
         */
        void DebugPrint() const;

    private:
        std::unordered_map<uint32_t, Entry> m_Entries;
        std::vector<uint8_t> m_Data;
    };

    /**
     * @brief Specialization Constants Builder
     * 
     * Fluent interface for building specialization constants.
     */
    class SpecializationConstantsBuilder {
    public:
        SpecializationConstantsBuilder() = default;

        template<typename T>
        SpecializationConstantsBuilder& Add(uint32_t constantId, const T& value, const std::string& name = "") {
            m_Constants.AddConstant(constantId, value, name);
            return *this;
        }

        SpecializationConstants Build() { return std::move(m_Constants); }

    private:
        SpecializationConstants m_Constants;
    };

    /**
     * @brief Common specialization constants for convenience
     */
    namespace CommonConstants {
        constexpr uint32_t kWorkgroupSizeX = 0;
        constexpr uint32_t kWorkgroupSizeY = 1;
        constexpr uint32_t kWorkgroupSizeZ = 2;
        constexpr uint32_t kMaxLights = 10;
        constexpr uint32_t kShadowMapSize = 11;
        constexpr uint32_t kEnableShadows = 12;
        constexpr uint32_t kEnableNormalMapping = 13;
        constexpr uint32_t kEnablePbr = 14;
        constexpr uint32_t kMaxBones = 15;
    }

} // namespace Rendering
} // namespace GameEngine
