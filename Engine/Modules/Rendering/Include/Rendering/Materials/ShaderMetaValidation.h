#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "Rendering/Materials/ShaderMeta.h"

namespace GameEngine { namespace Rendering {

enum class IssueSeverity : uint32_t { Info = 0, Warning = 1, Error = 2 };

struct ValidationIssue {
    IssueSeverity Severity = IssueSeverity::Error;
    std::string Code;     // e.g., "DescriptorConflict", "PushConstantsTotalBytesExceedLimit"
    std::string Message;  // human readable details
};

struct ValidationReport {
    std::vector<ValidationIssue> Issues;
    bool HasErrors() const {
        for (const auto& i : Issues) if (i.Severity == IssueSeverity::Error) return true;
        return false;
    }
};

ValidationReport ValidateShaderMeta(const ShaderMeta& meta, uint32_t pushConstantMaxBytes = 128);

}} // namespace GameEngine::Rendering

