#pragma once

// Host side of the GE_ToolchainFingerprint handshake (NativeScriptingABI.h): the
// engine's own fingerprint plus the field-by-field comparison LoadModule runs
// against a user DLL's exported fingerprint before Register_v1.

#include "NativeScripting/NativeScriptingABI.h"

#include <string>

namespace GameEngine
{
namespace NativeScripting
{

// The engine host's fingerprint — GE_FillToolchainFingerprint captured with the
// ENGINE's compiler / CRT / STL macros (this module is part of the engine build).
GE_ToolchainFingerprint HostToolchainFingerprint();

// Empty string when every field matches; otherwise a diagnostic naming each
// mismatching field with host vs module values, e.g.
// "IteratorDebugLevel mismatch (host=0, module=2)". Padding is not compared.
std::string DescribeToolchainFingerprintMismatch(const GE_ToolchainFingerprint& host,
                                                 const GE_ToolchainFingerprint& module);

} // namespace NativeScripting
} // namespace GameEngine
