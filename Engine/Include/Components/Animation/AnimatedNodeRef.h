#pragma once

#include "Types/Types.h"

namespace GameEngine
{
namespace Components
{

// Identifies which imported model node drives this rigid mesh entity.
// @ge-no-add  data helper, not user-addable in the editor
struct AnimatedNodeRef
{
    uint32 nodeIndex = 0;
};

} // namespace Components
} // namespace GameEngine
