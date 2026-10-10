#pragma once

#include "Types/Types.h"

namespace GameEngine {

/**
 * @brief Memory management system (placeholder)
 */
class MemoryManager {
public:
    MemoryManager();
    ~MemoryManager();

    /**
     * @brief Initialize memory manager
     */
    bool Initialize();

    /**
     * @brief Shutdown memory manager
     */
    void Shutdown();

private:
    bool m_Initialized;

    DISALLOW_COPY_AND_ASSIGN(MemoryManager);
};

} // namespace GameEngine
