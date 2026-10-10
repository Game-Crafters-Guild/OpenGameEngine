#pragma once

#include <string>
#include <string_view>

namespace GameEngine
{

struct GameLogicVec3
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

using GameLogicEntityId = unsigned long long;

class GameLogicRuntimeContext
{
public:
    virtual ~GameLogicRuntimeContext() = default;

    virtual GameLogicEntityId Self() const { return 0; }

    virtual bool GetBoolVariable(std::string_view name, bool fallback = false) { (void)name; return fallback; }
    virtual float GetFloatVariable(std::string_view name, float fallback = 0.0f) { (void)name; return fallback; }
    virtual std::string GetStringVariable(std::string_view name, std::string_view fallback = {}) { (void)name; return std::string(fallback); }
    virtual void SetVariable(std::string_view name, std::string_view value) { (void)name; (void)value; }

    virtual bool CompareFloat(float a, float b, std::string_view comparison);
    virtual bool CompareInt(int a, int b, std::string_view comparison);
    virtual bool BoolOperator(bool a, bool b, std::string_view operation);
    virtual bool BoolNot(bool value) { return !value; }

    virtual float FloatOperator(float a, float b, std::string_view operation);
    virtual float FloatClamp(float value, float minValue, float maxValue);
    virtual float FloatLerp(float a, float b, float t);
    virtual float FloatAbs(float value);
    virtual float FloatRound(float value);
    virtual float FloatSign(float value);
    virtual float RandomFloat(float minValue, float maxValue);
    virtual int IntOperator(int a, int b, std::string_view operation);
    virtual int RandomInt(int minValue, int maxValue);

    virtual GameLogicVec3 MakeVector3(float x, float y, float z) { return {x, y, z}; }
    virtual GameLogicVec3 Vector3Operator(GameLogicVec3 a, GameLogicVec3 b, std::string_view operation);
    virtual GameLogicVec3 Vector3Scale(GameLogicVec3 value, float scale);
    virtual GameLogicVec3 Vector3Normalize(GameLogicVec3 value);
    virtual float Vector3Magnitude(GameLogicVec3 value);
    virtual float Vector3Distance(GameLogicVec3 a, GameLogicVec3 b);
    virtual float Vector3Dot(GameLogicVec3 a, GameLogicVec3 b);
    virtual GameLogicVec3 Vector3Cross(GameLogicVec3 a, GameLogicVec3 b);
    virtual GameLogicVec3 Vector3Lerp(GameLogicVec3 a, GameLogicVec3 b, float t);

    virtual float GetTime() { return 0.0f; }
    virtual float GetDeltaTime() { return 0.0f; }
    virtual void SetTimeScale(float scale) { (void)scale; }
    virtual void QuitApplication() {}
    virtual void SetSkyTimeOfDay(float hours, bool animate, float cycleSeconds)
    {
        (void)hours;
        (void)animate;
        (void)cycleSeconds;
    }

    virtual void Delay(float seconds) { (void)seconds; }
    virtual void SendEvent(std::string_view eventName) { (void)eventName; }

    virtual void PlaySound(std::string_view clipGuid, float volume, float pitch, bool loop,
                           bool spatialized, GameLogicVec3 position, int worldId, int bus);
    virtual void StopAllSounds() {}
    virtual void SetBusVolume(int bus, float volume) { (void)bus; (void)volume; }

    virtual GameLogicEntityId CreateEntity(std::string_view name) { (void)name; return 0; }
    virtual void DestroyEntity(GameLogicEntityId entity) { (void)entity; }
    virtual void SetEntityEnabled(GameLogicEntityId entity, bool enabled) { (void)entity; (void)enabled; }
    virtual GameLogicEntityId FindEntityByName(std::string_view name) { (void)name; return 0; }

    virtual void SetPosition(GameLogicEntityId entity, GameLogicVec3 position) { (void)entity; (void)position; }
    virtual GameLogicVec3 GetPosition(GameLogicEntityId entity) { (void)entity; return {}; }
    virtual void Translate(GameLogicEntityId entity, GameLogicVec3 delta) { (void)entity; (void)delta; }
    virtual void Rotate(GameLogicEntityId entity, GameLogicVec3 euler) { (void)entity; (void)euler; }
    virtual void LookAt(GameLogicEntityId entity, GameLogicVec3 target) { (void)entity; (void)target; }

    virtual void AddForce(GameLogicEntityId entity, GameLogicVec3 force) { (void)entity; (void)force; }
    virtual void SetVelocity(GameLogicEntityId entity, GameLogicVec3 velocity) { (void)entity; (void)velocity; }
    virtual bool Raycast(GameLogicVec3 origin, GameLogicVec3 direction, float distance);
    virtual bool CollisionEvent(GameLogicEntityId entity, std::string_view phase, std::string_view tag);
    virtual bool TriggerEvent(GameLogicEntityId entity, std::string_view phase, std::string_view tag);

    virtual void PlayAnimation(GameLogicEntityId entity, std::string_view clipGuid, bool loop);
    virtual void SetAnimatorBool(GameLogicEntityId entity, std::string_view name, bool value);
    virtual void SetAnimatorFloat(GameLogicEntityId entity, std::string_view name, float value);
    virtual void SetAnimatorTrigger(GameLogicEntityId entity, std::string_view name);

    virtual bool GetInputAction(std::string_view action) { (void)action; return false; }
    virtual float GetInputAxis(std::string_view axis) { (void)axis; return 0.0f; }

    virtual void LoadScene(std::string_view scene) { (void)scene; }
    virtual void ReloadScene() {}

    virtual void ShowPanel(std::string_view panel) { (void)panel; }
    virtual void HidePanel(std::string_view panel) { (void)panel; }
    virtual void SetText(std::string_view element, std::string_view text) { (void)element; (void)text; }

    virtual void SetLightIntensity(GameLogicEntityId entity, float intensity) { (void)entity; (void)intensity; }
    virtual void SetCameraActive(GameLogicEntityId entity, bool active) { (void)entity; (void)active; }
    virtual void ThirdPersonCameraFollow(GameLogicEntityId camera,
                                         GameLogicEntityId target,
                                         GameLogicVec3 offset,
                                         GameLogicVec3 lookOffset,
                                         float positionSmoothing,
                                         float rotationSmoothing)
    {
        (void)camera;
        (void)target;
        (void)offset;
        (void)lookOffset;
        (void)positionSmoothing;
        (void)rotationSmoothing;
    }

    virtual void MoveTo(GameLogicEntityId entity, GameLogicVec3 target) { (void)entity; (void)target; }
    virtual void StopMove(GameLogicEntityId entity) { (void)entity; }

    virtual void Log(std::string_view message) { (void)message; }
    virtual void DrawDebugLine(GameLogicVec3 from, GameLogicVec3 to, float duration);
    virtual void TraceAction(std::string_view actionType) { (void)actionType; }
};

} // namespace GameEngine
