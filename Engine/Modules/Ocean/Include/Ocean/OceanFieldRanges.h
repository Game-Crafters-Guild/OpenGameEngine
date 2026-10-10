#pragma once

namespace GameEngine::Ocean
{

// Bind inspector slider bounds to the ocean components' reflected float fields so
// each value is clamped to a sensible range. Call ONCE at engine init, after the
// component reflection has been registered (static init of the generated
// reflection runs before any runtime call). Idempotent.
void RegisterOceanFieldRanges();

} // namespace GameEngine::Ocean
