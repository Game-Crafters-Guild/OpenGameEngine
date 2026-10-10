#pragma once

#include "Types/Types.h"

// Test components for ECS tests
// These components are used only for testing and are not part of the core ECS or Engine

namespace GameEngine::ECS::test {

// Transform components for testing
struct Position {
    float32 x = 0.0f;
    float32 y = 0.0f;
    float32 z = 0.0f;
    
    Position() = default;
    Position(float32 x_, float32 y_, float32 z_) : x(x_), y(y_), z(z_) {}
};

struct Velocity {
    float32 x = 0.0f;
    float32 y = 0.0f;
    float32 z = 0.0f;
    
    Velocity() = default;
    Velocity(float32 x_, float32 y_, float32 z_) : x(x_), y(y_), z(z_) {}
};

struct Rotation {
    float32 x = 0.0f;
    float32 y = 0.0f;
    float32 z = 0.0f;
    float32 w = 1.0f; // Quaternion
    
    Rotation() = default;
    Rotation(float32 x_, float32 y_, float32 z_, float32 w_) : x(x_), y(y_), z(z_), w(w_) {}
};

struct Scale {
    float32 x = 1.0f;
    float32 y = 1.0f;
    float32 z = 1.0f;

    Scale() = default;
    Scale(float32 uniform) : x(uniform), y(uniform), z(uniform) {}
    Scale(float32 x_, float32 y_, float32 z_) : x(x_), y(y_), z(z_) {}
};

// Unified Transform component (4x4 matrix)
struct Transform {
    float32 matrix[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,  // Column 0
        0.0f, 1.0f, 0.0f, 0.0f,  // Column 1
        0.0f, 0.0f, 1.0f, 0.0f,  // Column 2
        0.0f, 0.0f, 0.0f, 1.0f   // Column 3 (translation)
    };

    bool IsDirty = true;
    Transform() = default;
};

// Game logic components for testing
struct Health {
    int32 current = 100;
    int32 maximum = 100;
    
    Health() = default;
    Health(int32 max) : current(max), maximum(max) {}
    Health(int32 curr, int32 max) : current(curr), maximum(max) {}
    
    bool isAlive() const { return current > 0; }
    float32 getPercentage() const { return static_cast<float32>(current) / static_cast<float32>(maximum); }
};

struct Damage {
    int32 amount = 10;
    float32 range = 1.0f;
    
    Damage() = default;
    Damage(int32 dmg) : amount(dmg) {}
    Damage(int32 dmg, float32 rng) : amount(dmg), range(rng) {}
};

struct Timer {
    float32 duration = 1.0f;
    float32 elapsed = 0.0f;
    bool repeat = false;
    
    Timer() = default;
    Timer(float32 dur) : duration(dur) {}
    Timer(float32 dur, bool rep) : duration(dur), repeat(rep) {}
    
    bool isFinished() const { return elapsed >= duration; }
    float32 getProgress() const { return elapsed / duration; }
    void reset() { elapsed = 0.0f; }
};

// Marker components for testing
struct Temporary {
    float32 timeToLive = 1.0f;
    
    Temporary() = default;
    Temporary(float32 ttl) : timeToLive(ttl) {}
};

// Rendering components for testing
struct Camera {
    float32 fov = 60.0f;
    float32 aspectRatio = 16.0f / 9.0f;
    float32 nearPlane = 0.1f;
    float32 farPlane = 1000.0f;
    bool isActive = true;
    
    Camera() = default;
    Camera(float32 fovDegrees, float32 aspect, float32 near, float32 far)
        : fov(fovDegrees), aspectRatio(aspect), nearPlane(near), farPlane(far) {}
};

struct Light {
    enum Type : uint8 {
        Directional = 0,
        Point = 1,
        Spot = 2
    };
    
    Type type = Directional;
    float32 intensity = 1.0f;
    float32 color[3] = {1.0f, 1.0f, 1.0f}; // RGB
    float32 range = 10.0f;
    bool castsShadows = true;
    
    Light() = default;
    Light(Type t, float32 intensity_, float32 r, float32 g, float32 b)
        : type(t), intensity(intensity_) {
        color[0] = r; color[1] = g; color[2] = b;
    }
};

struct Material {
    uint32 materialId = 0;
    float32 albedo[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // RGBA
    float32 metallic = 0.0f;
    float32 roughness = 0.5f;
    
    Material() = default;
    Material(uint32 id) : materialId(id) {}
};

struct Mesh {
    uint32 meshId = 0;
    uint32 vertexBufferId = 0;
    uint32 indexBufferId = 0;
    uint32 vertexCount = 0;
    uint32 indexCount = 0;
    bool isVisible = true;
    
    Mesh() = default;
    Mesh(uint32 id) : meshId(id) {}
};

struct Renderable {
    bool isVisible = true;
    uint32 renderLayer = 0;

    Renderable() = default;
    Renderable(bool visible, uint32 layer = 0) : isVisible(visible), renderLayer(layer) {}
};

struct Color {
    float32 r = 1.0f;
    float32 g = 1.0f;
    float32 b = 1.0f;
    float32 a = 1.0f;

    Color() = default;
    Color(float32 red, float32 green, float32 blue, float32 alpha = 1.0f)
        : r(red), g(green), b(blue), a(alpha) {}
};

// RPG components for testing
struct Inventory {
    static constexpr size_t MAX_ITEMS = 16;
    uint32 itemIds[MAX_ITEMS];
    uint32 itemCounts[MAX_ITEMS];
    uint32 itemCount = 0;

    Inventory() {
        for (size_t i = 0; i < MAX_ITEMS; ++i) {
            itemIds[i] = 0;
            itemCounts[i] = 0;
        }
    }

    bool AddItem(uint32 id, uint32 count) {
        for (size_t i = 0; i < itemCount; ++i) {
            if (itemIds[i] == id) {
                itemCounts[i] += count;
                return true;
            }
        }
        if (itemCount >= MAX_ITEMS) return false;
        itemIds[itemCount] = id;
        itemCounts[itemCount] = count;
        ++itemCount;
        return true;
    }

    bool RemoveItem(uint32 id, uint32 count) {
        for (size_t i = 0; i < itemCount; ++i) {
            if (itemIds[i] == id) {
                if (itemCounts[i] < count) return false;
                itemCounts[i] -= count;
                return true;
            }
        }
        return false;
    }

    uint32 GetItemCount(uint32 id) const {
        for (size_t i = 0; i < itemCount; ++i) {
            if (itemIds[i] == id) return itemCounts[i];
        }
        return 0;
    }
};

struct Experience {
    uint32 currentXP = 0;
    uint32 level = 1;
    uint32 xpToNextLevel = 100;

    Experience() = default;
    Experience(uint32 startLevel) : level(startLevel) {
        xpToNextLevel = calculateXPForLevel(startLevel + 1);
    }

    uint32 calculateXPForLevel(uint32 targetLevel) const {
        return targetLevel * 100;
    }

    void AddXP(uint32 amount) {
        currentXP += amount;
        while (currentXP >= xpToNextLevel) {
            currentXP -= xpToNextLevel;
            ++level;
            xpToNextLevel = calculateXPForLevel(level + 1);
        }
    }
};

struct Stats {
    int32 strength = 10;
    int32 dexterity = 10;
    int32 intelligence = 10;
    int32 vitality = 10;
    int32 luck = 10;

    Stats() = default;
    Stats(int32 str, int32 dex, int32 intel, int32 vit, int32 lck)
        : strength(str), dexterity(dex), intelligence(intel), vitality(vit), luck(lck) {}

    int32 GetAttackPower() const { return strength * 2 + dexterity; }
    int32 GetDefense() const { return vitality * 2 + strength / 2; }
};

// AI components for testing
struct AIState {
    enum State : uint8 {
        Idle = 0,
        Patrol = 1,
        Chase = 2,
        Attack = 3,
        Flee = 4
    };

    State currentState = Idle;
    State previousState = Idle;
    float32 stateTimer = 0.0f;

    AIState() = default;
    AIState(State initial) : currentState(initial), previousState(initial) {}

    void ChangeState(State newState) {
        previousState = currentState;
        currentState = newState;
        stateTimer = 0.0f;
    }

    bool HasStateChanged() const { return currentState != previousState; }
};

struct Pathfinding {
    static constexpr size_t MAX_PATH_NODES = 32;

    struct PathNode {
        float32 x, y;
        PathNode() : x(0), y(0) {}
        PathNode(float32 x_, float32 y_) : x(x_), y(y_) {}
    };

    PathNode path[MAX_PATH_NODES];
    uint32 pathLength = 0;
    uint32 currentNode = 0;
    bool hasPath = false;

    Pathfinding() = default;

    void SetPath(const PathNode* nodes, uint32 count) {
        pathLength = (count > MAX_PATH_NODES) ? MAX_PATH_NODES : count;
        for (uint32 i = 0; i < pathLength; ++i) {
            path[i] = nodes[i];
        }
        currentNode = 0;
        hasPath = true;
    }

    PathNode GetCurrentTarget() const {
        if (!hasPath || currentNode >= pathLength) return PathNode{};
        return path[currentNode];
    }

    bool AdvanceToNextNode() {
        if (!hasPath || currentNode + 1 >= pathLength) return false;
        ++currentNode;
        return true;
    }
};

} // namespace GameEngine::ECS::test
