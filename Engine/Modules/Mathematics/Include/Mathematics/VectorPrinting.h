#pragma once

// Test-facing printers for the vector types.
//
// Kept out of Vector2.h and its siblings so the math headers every hot path
// includes never pull in <ostream>. Include this from a test that compares
// vectors: GoogleTest finds PrintTo by argument-dependent lookup and reports
// the components instead of a byte dump.

#include "Mathematics/Vector2.h"

#include <ostream>

namespace GameEngine {
namespace Mathematics {

inline void PrintTo(const Vector2& value, std::ostream* out)
{
    *out << "(" << value.x << ", " << value.y << ")";
}

} // namespace Mathematics
} // namespace GameEngine
