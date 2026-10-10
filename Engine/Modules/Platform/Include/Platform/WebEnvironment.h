#pragma once

namespace GameEngine::Platform::Web
{

/// Import `GE_`-prefixed URL query parameters into the process environment.
///
/// A page has no shell to inherit an environment from, so the query string is
/// where a wasm host's diagnostic switches come from: loading
/// `?GE_WEBGPU_FLUSH_ALL=1` makes `std::getenv("GE_WEBGPU_FLUSH_ALL")` return
/// `"1"` for the rest of the run, and every engine-side `GE_*` diagnostic keeps
/// its single, portable spelling.
///
/// Call once from a web host's `main`, before the engine initializes. Only the
/// `GE_` prefix is imported — other parameters are the host's own arguments.
void ImportEnvironmentFromUrl();

} // namespace GameEngine::Platform::Web
