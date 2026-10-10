#pragma once

#include "Types/Types.h"

namespace JobSystem {

// The Types module's aliases under the JobSystem namespace: one definition,
// so a translation unit that opens both namespaces sees one entity per name.
using GameEngine::int8;
using GameEngine::int16;
using GameEngine::int32;
using GameEngine::int64;
using GameEngine::uint8;
using GameEngine::uint16;
using GameEngine::uint32;
using GameEngine::uint64;
using GameEngine::float32;
using GameEngine::float64;

using GameEngine::String;
using GameEngine::Vector;

using GameEngine::UniquePtr;
using GameEngine::SharedPtr;
using GameEngine::WeakPtr;
using GameEngine::MakeUnique;
using GameEngine::MakeShared;

using GameEngine::Function;

using TaskId = uint64;

// The most blocking threads a pool may be given for its JobChannels: the
// desktop budget (Platform::BlockingThreadBudget()) and the ceiling the pool
// asserts in Debug.
inline constexpr size_t kMaxBlockingThreadBudget = 64;

class Task;
class TaskHandle;
class WorkStealingThreadPool;
class TaskDependencyGraph;

} // namespace JobSystem
