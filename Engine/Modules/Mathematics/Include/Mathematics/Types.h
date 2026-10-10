#pragma once

// Canonical engine-wide math types.
//
// These are lightweight wrapper types that provide a stable public API for the
// engine and editor while delegating the heavy lifting to GLM under the hood.
//
// Individual types live in their own headers (Vector2.h, Vector3.h, ...). This
// header exists as the single include most code should use.

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Mathematics/Rect.h"
#include "Mathematics/Matrix2x2.h"
#include "Mathematics/Matrix3x3.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Quaternion.h"

