#pragma once

#include "JobSystem/Types.h"
#include <cstdint>

namespace JobSystem {

/**
 * @brief Task execution status enumeration
 */
enum class TaskStatus : uint8 {
    Pending = 0,    // Task is waiting to be executed
    Running = 1,    // Task is currently being executed
    Completed = 2,  // Task completed successfully
    Failed = 3,     // Task failed during execution
    Cancelled = 4   // Task was cancelled before completion
};

/**
 * @brief Pool scheduling class for Submit(F&&) / EnqueueWork / EnqueueWorkBatch.
 *
 * Orders dequeue between the pool's two lanes: a worker takes Background work
 * only when it finds no Normal work. Normal is the default and the hot path.
 * Background rides a dedicated global lane that every consumer polls last; it
 * runs on idle capacity, and a saturating Normal flood postpones it
 * indefinitely (there is no aging). The class orders dequeue only: shutdown,
 * cancellation and the never-drop contracts are identical for both classes
 * (see WorkStealingThreadPool's class comment).
 */
enum class JobPriority : uint8 {
    Normal = 0,
    Background = 1
};

} // namespace JobSystem
