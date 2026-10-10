#pragma once

#include "Animation/HumanoidRig.h"
#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Animation
{

// Per-chain FK rotation transport mode. OneToOne is the same-profile fast
// path; SlerpAlongArc is used for long chains (e.g., spine) where
// transporting per-bone rotation to a different proportions/orientation
// produces unstable results.
enum class FKRotationMode : uint8
{
    OneToOne,
    Transport,
    SlerpAlongArc
};

// Per-chain translation transport mode.
enum class FKTranslationMode : uint8
{
    None,
    PerBoneScale,
    UniformChainScale
};

const char* FKRotationModeToString(FKRotationMode mode);
FKRotationMode FKRotationModeFromString(std::string_view name);
const char* FKTranslationModeToString(FKTranslationMode mode);
FKTranslationMode FKTranslationModeFromString(std::string_view name);

struct FKSettings
{
    FKRotationMode RotationMode = FKRotationMode::OneToOne;
    float RotationAlpha = 1.0f;
    FKTranslationMode TranslationMode = FKTranslationMode::None;
};

struct IKSettings
{
    bool Enabled = false;
    float BlendToSource = 1.0f;
    float Extension = 0.0f;
};

struct FootLockSettings
{
    bool Enabled = false;
    float SpeedThreshold = 0.05f; // m/s; below this the foot is considered planted
    float LockBlend = 1.0f;       // 0 = ignore lock, 1 = full lock
};

// One source-chain -> target-chain pairing with all settings.
struct ChainPairing
{
    ChainKind Kind = ChainKind::Other;
    FKSettings FK;
    IKSettings IK;
    FootLockSettings FootLock;
};

// One op-stack entry. Free-form params; the op runtime resolves Params at
// schedule time. Stored as nlohmann::json so unknown ops still round-trip.
struct OpStackEntry
{
    std::string OpName;
    nlohmann::json Params;
};

// RetargetMap: source rig -> target rig pairing + chain settings + op stack.
// IsBroken() returns true if either rig reference is null. AssetManager-side
// validation (Phase 1 sub-task 6) sets the broken state by clearing the
// RigRef GUIDs when a referenced asset is missing.
class RetargetMap : public ::GameEngine::Asset
{
  public:
    static constexpr int32 kSchemaVersion = 1;

    RetargetMap(const ::GameEngine::GUID& guid, const std::filesystem::path& path)
        : ::GameEngine::Asset(guid, ::GameEngine::AssetType::RetargetMap, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    bool SaveToData(Vector<uint8>& outData) const;
    bool SaveToPath(const std::filesystem::path& path) const;

    int32 Version() const { return m_Version; }

    const ::GameEngine::GUID& SourceRigRef() const { return m_SourceRigRef; }
    const ::GameEngine::GUID& TargetRigRef() const { return m_TargetRigRef; }
    void SetSourceRigRef(const ::GameEngine::GUID& guid) { m_SourceRigRef = guid; }
    void SetTargetRigRef(const ::GameEngine::GUID& guid) { m_TargetRigRef = guid; }

    const std::vector<ChainPairing>& ChainMap() const { return m_ChainMap; }
    std::vector<ChainPairing>& ChainMapMutable() { return m_ChainMap; }

    const std::vector<OpStackEntry>& OpStack() const { return m_OpStack; }
    std::vector<OpStackEntry>& OpStackMutable() { return m_OpStack; }

    // Returns true if either RigRef GUID is null. Used by inspectors to
    // surface a "BROKEN" banner. Phase 1 only checks null GUIDs;
    // future phases will extend this with asset-load validation.
    bool IsBroken() const
    {
        return m_SourceRigRef.IsNull() || m_TargetRigRef.IsNull();
    }

  private:
    bool ParseJson(const std::string& text);
    std::string SerializeJson() const;

    int32 m_Version = 0;
    ::GameEngine::GUID m_SourceRigRef;
    ::GameEngine::GUID m_TargetRigRef;
    std::vector<ChainPairing> m_ChainMap;
    std::vector<OpStackEntry> m_OpStack;
};

} // namespace Animation
} // namespace GameEngine
