#pragma once

#include <nlohmann/json.hpp>
#include "Rendering/Materials/ShaderMeta.h"

namespace GameEngine { namespace Rendering {

using nlohmann::json;

// Declarations for nlohmann::json ADL
void to_json(json& j, const TypeDesc& t);
void from_json(const json& j, TypeDesc& t);

void to_json(json& j, const Member& m);
void from_json(const json& j, Member& m);

void to_json(json& j, const BlockLayout& b);
void from_json(const json& j, BlockLayout& b);

void to_json(json& j, const PushConstantRangeMeta& p);
void from_json(const json& j, PushConstantRangeMeta& p);

void to_json(json& j, const DescriptorBindingMeta& b);
void from_json(const json& j, DescriptorBindingMeta& b);

void to_json(json& j, const DescriptorSetMeta& s);
void from_json(const json& j, DescriptorSetMeta& s);

void to_json(json& j, const StageIO& io);
void from_json(const json& j, StageIO& io);

void to_json(json& j, const StageMeta& s);
void from_json(const json& j, StageMeta& s);

void to_json(json& j, const SpecConstantMeta& s);
void from_json(const json& j, SpecConstantMeta& s);

void to_json(json& j, const ShaderProperty& p);
void from_json(const json& j, ShaderProperty& p);

void to_json(json& j, const ShaderMeta& m);
void from_json(const json& j, ShaderMeta& m);

}} // namespace GameEngine::Rendering

