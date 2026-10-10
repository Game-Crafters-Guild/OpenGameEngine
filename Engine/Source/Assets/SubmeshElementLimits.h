#pragma once

// The most vertices and indices one submesh holds. The HLOD cache treats a blob that claims more as
// corrupt, and the glTF loader refuses an accessor without a buffer view that declares more.

#include "Types/Types.h"

namespace GameEngine
{

constexpr uint32 kMaxVerticesPerSubmesh = 1u << 26;
constexpr uint32 kMaxIndicesPerSubmesh = 1u << 27;

} // namespace GameEngine
