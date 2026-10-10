# metal-cpp (vendored)

- Source: https://developer.apple.com/metal/cpp/ (`metal-cpp_macOS15_iOS18.zip`)
- Version: macOS 15 / iOS 18 release
- License: Apache 2.0 (see LICENSE.txt)
- Vendored unmodified. Used by the Rendering module's Metal backend
  (`Engine/Modules/Rendering/Source/Metal/`). The `NS_PRIVATE_IMPLEMENTATION`
  defines live in exactly one TU: `MetalCppImpl.cpp`.
- vcpkg has no metal-cpp port, hence the vendored copy.
