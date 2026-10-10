/**
 * @file SpecializationConstants.cpp
 * @brief Enhanced Specialization Constants implementation
 */

#include "Rendering/Core/SpecializationConstants.h"
#include <iostream>
#include <algorithm>
#include <cstring>

namespace GameEngine {
namespace Rendering {

    void SpecializationConstants::RemoveConstant(uint32_t constantId) {
        auto it = m_Entries.find(constantId);
        if (it == m_Entries.end()) {
            return;
        }

        m_Entries.erase(it);
    }

    void SpecializationConstants::Clear() {
        m_Entries.clear();
        m_Data.clear();
    }

    SpecializationConstants SpecializationConstants::CreateVariant() const {
        SpecializationConstants variant;
        variant.m_Entries = m_Entries;
        variant.m_Data = m_Data;
        return variant;
    }

    void SpecializationConstants::Merge(const SpecializationConstants& other, bool overwrite) {
        for (const auto& [constantId, entry] : other.m_Entries) {
            if (!overwrite && m_Entries.find(constantId) != m_Entries.end()) {
                continue; // Skip existing constants if not overwriting
            }

            // Copy the constant data
            Entry newEntry = entry;
            newEntry.Offset = static_cast<uint32_t>(m_Data.size());

            // Add data to our buffer
            size_t oldSize = m_Data.size();
            m_Data.resize(oldSize + entry.Size);
            std::memcpy(m_Data.data() + oldSize, other.m_Data.data() + entry.Offset, entry.Size);

            m_Entries[constantId] = newEntry;
        }
    }

    uint64_t SpecializationConstants::GetHash() const {
        // Walk constants in sorted-by-id order so two SpecializationConstants
        // with identical content but different insertion order produce the
        // same hash. Required for the pipeline cache: two call sites that
        // build the same constants in different orders must intern to the
        // same id, otherwise the cache fragments per call site.
        std::vector<uint32_t> sortedIds;
        sortedIds.reserve(m_Entries.size());
        for (const auto& [constantId, entry] : m_Entries) {
            (void)entry;
            sortedIds.push_back(constantId);
        }
        std::sort(sortedIds.begin(), sortedIds.end());

        uint64_t hash = 0;
        for (uint32_t constantId : sortedIds) {
            const Entry& entry = m_Entries.at(constantId);
            hash ^= std::hash<uint32_t>{}(constantId) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<uint32_t>{}(static_cast<uint32_t>(entry.Size))
                    + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            for (size_t i = 0; i < entry.Size; ++i) {
                uint8_t byte = m_Data[entry.Offset + i];
                hash ^= std::hash<uint8_t>{}(byte) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
        }
        return hash;
    }

    void SpecializationConstants::DebugPrint() const {
        std::cout << "\n=== Specialization Constants ===" << std::endl;
        std::cout << "Total constants: " << m_Entries.size() << std::endl;
        std::cout << "Total data size: " << m_Data.size() << " bytes" << std::endl;
        
        for (const auto& [constantId, entry] : m_Entries) {
            std::cout << "  Constant " << constantId << " (" << entry.Name << "): ";
            std::cout << "offset=" << entry.Offset << ", size=" << entry.Size << ", value=";
            
            // Print value based on size (simple interpretation)
            if (entry.Size == 4) {
                uint32_t value;
                std::memcpy(&value, m_Data.data() + entry.Offset, 4);
                std::cout << value;
            } else if (entry.Size == 8) {
                uint64_t value;
                std::memcpy(&value, m_Data.data() + entry.Offset, 8);
                std::cout << value;
            } else if (entry.Size == 1) {
                uint8_t value;
                std::memcpy(&value, m_Data.data() + entry.Offset, 1);
                std::cout << static_cast<uint32_t>(value);
            } else {
                std::cout << "[" << entry.Size << " bytes]";
            }
            std::cout << std::endl;
        }
        std::cout << "=================================" << std::endl;
    }


} // namespace Rendering
} // namespace GameEngine
