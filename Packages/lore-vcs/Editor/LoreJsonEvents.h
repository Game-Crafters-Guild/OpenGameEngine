#pragma once

#include "VCSIntegration/IVCSIntegration.h"
#include "VCSIntegration/VCSFileStatus.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

// Decoders for the `lore --json` event stream: one object per stdout line,
// shaped `{"tagName": "<camelCaseEvent>", "data": {...}}`. Hashes are hex
// text, byte flags are booleans, enums are camelCase strings.

std::vector<nlohmann::json> ParseLoreJsonEvents(std::string_view output);

struct LoreFileStatusEntry
{
    std::string Path; // repository-root relative, forward slashes
    VCSFileStatus Status = VCSFileStatus::Clean;
};

// `repositoryStatusFile` payload -> editor status. Returns false for a payload
// that names no path. Action `keep` is dirty content (the CLI prints it as M);
// an unstaged `add` is an untracked file.
bool ParseLoreStatusFileEvent(const nlohmann::json& data, LoreFileStatusEntry& outEntry);

struct LoreRevisionHeader
{
    std::string BranchName;
    std::string Revision; // hex signature
    uint64_t RevisionNumber = 0;
};

// `repositoryStatusRevision` payload. Returns false when the branch name is missing.
bool ParseLoreRevisionHeader(const nlohmann::json& data, LoreRevisionHeader& outHeader);

// A `lore --json history` run: each `revisionHistoryEntry` opens an entry and
// the `metadata` events that follow it fill message, author and date.
std::vector<VCSLogEntry> ParseLoreHistoryEvents(const std::vector<nlohmann::json>& events);

} // namespace GameEngine
