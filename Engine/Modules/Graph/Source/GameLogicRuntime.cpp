#include "Graph/GameLogicRuntime.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{

bool GameLogicRuntimeContext::CompareFloat(float a, float b, std::string_view comparison)
{
    if (comparison == "less")
        return a < b;
    if (comparison == "less_or_equal")
        return a <= b;
    if (comparison == "equal")
        return a == b;
    if (comparison == "not_equal")
        return a != b;
    if (comparison == "greater_or_equal")
        return a >= b;
    return a > b;
}

bool GameLogicRuntimeContext::CompareInt(int a, int b, std::string_view comparison)
{
    if (comparison == "less")
        return a < b;
    if (comparison == "less_or_equal")
        return a <= b;
    if (comparison == "equal")
        return a == b;
    if (comparison == "not_equal")
        return a != b;
    if (comparison == "greater_or_equal")
        return a >= b;
    return a > b;
}

bool GameLogicRuntimeContext::BoolOperator(bool a, bool b, std::string_view operation)
{
    if (operation == "and")
        return a && b;
    if (operation == "or")
        return a || b;
    if (operation == "xor")
        return a != b;
    if (operation == "equal")
        return a == b;
    if (operation == "not_equal")
        return a != b;
    return a && b;
}

float GameLogicRuntimeContext::FloatOperator(float a, float b, std::string_view operation)
{
    if (operation == "subtract")
        return a - b;
    if (operation == "multiply")
        return a * b;
    if (operation == "divide")
        return b == 0.0f ? 0.0f : a / b;
    if (operation == "min")
        return std::min(a, b);
    if (operation == "max")
        return std::max(a, b);
    return a + b;
}

float GameLogicRuntimeContext::FloatClamp(float value, float minValue, float maxValue)
{
    if (minValue > maxValue)
        std::swap(minValue, maxValue);
    return std::clamp(value, minValue, maxValue);
}

float GameLogicRuntimeContext::FloatLerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

float GameLogicRuntimeContext::FloatAbs(float value)
{
    return std::fabs(value);
}

float GameLogicRuntimeContext::FloatRound(float value)
{
    return std::round(value);
}

float GameLogicRuntimeContext::FloatSign(float value)
{
    if (value > 0.0f)
        return 1.0f;
    if (value < 0.0f)
        return -1.0f;
    return 0.0f;
}

float GameLogicRuntimeContext::RandomFloat(float minValue, float maxValue)
{
    return minValue + (maxValue - minValue) * 0.5f;
}

int GameLogicRuntimeContext::IntOperator(int a, int b, std::string_view operation)
{
    if (operation == "subtract")
        return a - b;
    if (operation == "multiply")
        return a * b;
    if (operation == "divide")
        return b == 0 ? 0 : a / b;
    if (operation == "modulo")
        return b == 0 ? 0 : a % b;
    if (operation == "min")
        return std::min(a, b);
    if (operation == "max")
        return std::max(a, b);
    return a + b;
}

int GameLogicRuntimeContext::RandomInt(int minValue, int maxValue)
{
    if (minValue > maxValue)
        std::swap(minValue, maxValue);
    return minValue;
}

GameLogicVec3 GameLogicRuntimeContext::Vector3Operator(GameLogicVec3 a, GameLogicVec3 b, std::string_view operation)
{
    if (operation == "subtract")
        return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
    if (operation == "multiply")
        return {a.X * b.X, a.Y * b.Y, a.Z * b.Z};
    if (operation == "divide")
        return {b.X == 0.0f ? 0.0f : a.X / b.X, b.Y == 0.0f ? 0.0f : a.Y / b.Y, b.Z == 0.0f ? 0.0f : a.Z / b.Z};
    if (operation == "min")
        return {std::min(a.X, b.X), std::min(a.Y, b.Y), std::min(a.Z, b.Z)};
    if (operation == "max")
        return {std::max(a.X, b.X), std::max(a.Y, b.Y), std::max(a.Z, b.Z)};
    return {a.X + b.X, a.Y + b.Y, a.Z + b.Z};
}

GameLogicVec3 GameLogicRuntimeContext::Vector3Scale(GameLogicVec3 value, float scale)
{
    return {value.X * scale, value.Y * scale, value.Z * scale};
}

GameLogicVec3 GameLogicRuntimeContext::Vector3Normalize(GameLogicVec3 value)
{
    const float magnitude = Vector3Magnitude(value);
    if (magnitude == 0.0f)
        return {};
    return Vector3Scale(value, 1.0f / magnitude);
}

float GameLogicRuntimeContext::Vector3Magnitude(GameLogicVec3 value)
{
    return std::sqrt(value.X * value.X + value.Y * value.Y + value.Z * value.Z);
}

float GameLogicRuntimeContext::Vector3Distance(GameLogicVec3 a, GameLogicVec3 b)
{
    return Vector3Magnitude({a.X - b.X, a.Y - b.Y, a.Z - b.Z});
}

float GameLogicRuntimeContext::Vector3Dot(GameLogicVec3 a, GameLogicVec3 b)
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

GameLogicVec3 GameLogicRuntimeContext::Vector3Cross(GameLogicVec3 a, GameLogicVec3 b)
{
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}

GameLogicVec3 GameLogicRuntimeContext::Vector3Lerp(GameLogicVec3 a, GameLogicVec3 b, float t)
{
    return {FloatLerp(a.X, b.X, t), FloatLerp(a.Y, b.Y, t), FloatLerp(a.Z, b.Z, t)};
}

void GameLogicRuntimeContext::PlaySound(std::string_view clipGuid, float volume, float pitch, bool loop,
                                        bool spatialized, GameLogicVec3 position, int worldId, int bus)
{
    (void)clipGuid;
    (void)volume;
    (void)pitch;
    (void)loop;
    (void)spatialized;
    (void)position;
    (void)worldId;
    (void)bus;
}

bool GameLogicRuntimeContext::Raycast(GameLogicVec3 origin, GameLogicVec3 direction, float distance)
{
    (void)origin;
    (void)direction;
    (void)distance;
    return false;
}

bool GameLogicRuntimeContext::CollisionEvent(GameLogicEntityId entity, std::string_view phase, std::string_view tag)
{
    (void)entity;
    (void)phase;
    (void)tag;
    return false;
}

bool GameLogicRuntimeContext::TriggerEvent(GameLogicEntityId entity, std::string_view phase, std::string_view tag)
{
    (void)entity;
    (void)phase;
    (void)tag;
    return false;
}

void GameLogicRuntimeContext::PlayAnimation(GameLogicEntityId entity, std::string_view clipGuid, bool loop)
{
    (void)entity;
    (void)clipGuid;
    (void)loop;
}

void GameLogicRuntimeContext::SetAnimatorBool(GameLogicEntityId entity, std::string_view name, bool value)
{
    (void)entity;
    (void)name;
    (void)value;
}

void GameLogicRuntimeContext::SetAnimatorFloat(GameLogicEntityId entity, std::string_view name, float value)
{
    (void)entity;
    (void)name;
    (void)value;
}

void GameLogicRuntimeContext::SetAnimatorTrigger(GameLogicEntityId entity, std::string_view name)
{
    (void)entity;
    (void)name;
}

void GameLogicRuntimeContext::DrawDebugLine(GameLogicVec3 from, GameLogicVec3 to, float duration)
{
    (void)from;
    (void)to;
    (void)duration;
}

} // namespace GameEngine
