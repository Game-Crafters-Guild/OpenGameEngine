#pragma once

namespace GameEngine {

// Register the native C/C++ source inspector for AssetType::NativeSource assets
// (.cpp/.cc/.cxx/.c/.h/.hpp/.hxx/.hh): a read-only source preview plus "Open In Editor"
// (internal Script Editor panel) and "Open In IDE" buttons.
void RegisterNativeSourceInspector();

} // namespace GameEngine
