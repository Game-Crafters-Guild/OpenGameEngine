#pragma once

#include "Animation/AnimationPose.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <string>
#include <vector>

namespace GameEngine
{
namespace Animation
{

enum class SkeletonModifierKind : uint8
{
    LookAt,
    Aim,
    CopyTransform,
    TwoBoneIK,
    FABRIK,
    CCDIK,
    SpringBone,
    TwistDispersion,
    PhysicalBone
};

struct SkeletonModifier
{
    SkeletonModifierKind Kind = SkeletonModifierKind::CopyTransform;
    std::string Name;
    bool Enabled = true;
    float32 Weight = 1.0f;
    uint32 SourceBone = 0;
    uint32 TargetBone = 0;
    uint32 MidBone = 0;
    uint32 EndBone = 0;
    Mathematics::Vector3 TargetPosition;
};

class SkeletonModifierStack
{
public:
    void Add(SkeletonModifier modifier);
    void Clear();
    size_t Size() const { return m_Modifiers.size(); }
    const std::vector<SkeletonModifier>& Modifiers() const { return m_Modifiers; }

    void Apply(AnimationPose& pose) const;

private:
    std::vector<SkeletonModifier> m_Modifiers;
};

const char* SkeletonModifierKindToString(SkeletonModifierKind kind);

} // namespace Animation
} // namespace GameEngine
