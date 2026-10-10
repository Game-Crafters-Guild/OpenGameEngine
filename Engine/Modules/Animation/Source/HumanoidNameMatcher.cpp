#include "Animation/HumanoidNameMatcher.h"

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace GameEngine
{
namespace Animation
{

namespace
{

enum class Side
{
    Any,
    Left,
    Right,
};

struct PatternRow
{
    HumanBone Bone;
    bool Required;
    Side RequiredSide;
    bool AllowUnmarkedRight;
    std::vector<std::string_view> Aliases;
};

// Priority-ordered table. First match wins; per-bone matching applies
// the table in order, so e.g. Shoulder/Clavicle (canonical LeftShoulder)
// claims its source bone before LeftUpperArm tries to (which would otherwise
// match Synty's "Shoulder_L" naming).
//
// Coverage:
//   - Mixamo:        mixamorig:Hips, mixamorig:LeftArm, mixamorig:LeftHandThumb1
//   - Synty:         Hips, Spine_01, Shoulder_L, UpperArm_L, Hand_R, Foot_L
//   - MetaHuman:     spine_01, clavicle_l, upperarm_l, lowerarm_l, hand_l,
//                    thigh_l, calf_l, foot_l (lowercase, _l/_r side suffix)
//   - Maya / Adobe:  L_UpperArm, R_Forearm
//   - VRM / glTF:    LeftUpperArm, RightLowerArm
const PatternRow kRows[] = {
    // --- Body root + spine ---
    // Hips: Mixamo/Synty/MetaHuman ("hips"/"pelvis") plus BlenRig centroid
    // bones ("hipcenter", "spine" + "hips" → candidate "hips"). Side tokens
    // stripped first, so left/right limbs cannot satisfy center rows.
    {HumanBone::Hips,        true,  Side::Any,   false, {"hips", "hip", "pelvis", "roothip", "spineroot", "hipcenter"}},
    {HumanBone::Head,        true,  Side::Any,   false, {"head"}},
    {HumanBone::Neck,        false, Side::Any,   false, {"neck", "neck01", "neck1"}},
    {HumanBone::UpperChest,  false, Side::Any,   false, {"upperchest", "spine03", "spine3"}},
    {HumanBone::Chest,       false, Side::Any,   false, {"chest", "spine02", "spine2"}},
    // Lowest spine only (spine / spine01 / spine1). Spine_02 / Spine_03 map
    // to Chest / UpperChest via dedicated rows. End-anchored matching is what
    // keeps this safe on BlenRig: "MSTR-Spine_Hips" emits suffixes
    // {"spinehips", "hips"} — the bare alias "spine" does NOT match, so
    // this row leaves MSTR-Spine_Hips alone and FK-Spine wins as the first
    // bone whose body suffix is exactly "spine".
    {HumanBone::Spine,       true,  Side::Any,   false, {"spine", "spine01", "spine1"}},

    // --- Arms (Left) ---
    // Shoulder/clavicle FIRST, so Synty's "Shoulder_L" (canonical
    // LeftShoulder) doesn't get claimed by LeftUpperArm's "shoulder" alt.
    {HumanBone::LeftShoulder,  false, Side::Left, false, {"shoulder", "clav", "clavicle", "collar"}},
    // UpperArm/Bicep/Arm. "shoulder" alt covers Synty rigs where Clavicle =
    // canonical LeftShoulder and Shoulder_X = upper arm; first-match-wins
    // ordering above guarantees LeftShoulder claims Clavicle/Shoulder first,
    // so this row picks up only the leftover upper-arm bone. Without
    // "shoulder" here the upper-arm bone falls through unmapped, the LeftArm
    // chain becomes [Shoulder, LowerArm, Hand] (3 bones), and the upper-arm
    // bone stays at bind during retargeting — the visible "T-pose arms while
    // body animates" symptom on Synty cross-rig.
    {HumanBone::LeftUpperArm,  true,  Side::Left, false, {"upperarm", "bicep", "shoulder", "arm"}},
    {HumanBone::LeftLowerArm,  true,  Side::Left, false, {"lowerarm", "forearm", "elbow"}},
    {HumanBone::LeftHand,      true,  Side::Left, false, {"hand", "wrist"}},

    // --- Arms (Right) ---
    {HumanBone::RightShoulder, false, Side::Right, false, {"shoulder", "clav", "clavicle", "collar"}},
    {HumanBone::RightUpperArm, true,  Side::Right, false, {"upperarm", "bicep", "shoulder", "arm"}},
    {HumanBone::RightLowerArm, true,  Side::Right, false, {"lowerarm", "forearm", "elbow"}},
    {HumanBone::RightHand,     true,  Side::Right, false, {"hand", "wrist"}},

    // --- Legs (Left) ---
    {HumanBone::LeftUpperLeg,  true,  Side::Left, false, {"upperleg", "upleg", "thigh", "hipjoint"}},
    {HumanBone::LeftLowerLeg,  true,  Side::Left, false, {"lowerleg", "leg", "shin", "calf", "knee"}},
    {HumanBone::LeftFoot,      true,  Side::Left, false, {"foot", "ankle"}},
    {HumanBone::LeftToes,      false, Side::Left, false, {"toe", "toes", "toebase", "toesbase", "ball"}},

    // --- Legs (Right) ---
    {HumanBone::RightUpperLeg, true,  Side::Right, false, {"upperleg", "upleg", "thigh", "hipjoint"}},
    {HumanBone::RightLowerLeg, true,  Side::Right, false, {"lowerleg", "leg", "shin", "calf", "knee"}},
    {HumanBone::RightFoot,     true,  Side::Right, false, {"foot", "ankle"}},
    {HumanBone::RightToes,     false, Side::Right, false, {"toe", "toes", "toebase", "toesbase", "ball"}},

    // --- Face ---
    {HumanBone::LeftEye,  false, Side::Left,  false, {"eye"}},
    {HumanBone::RightEye, false, Side::Right, false, {"eye"}},
    {HumanBone::Jaw,      false, Side::Any,   false, {"jaw"}},

    // --- Left fingers ---
    // Each pattern accepts a side-explicit form (Mixamo / VRM / Maya: "L_" or
    // "Left" prefix or suffix) AND a ufbx duplicate-name disambiguation form
    // (Synty: when two bones share a name, the second occurrence is suffixed
    // with `_1` — which by convention marks the LEFT-side bone).
    {HumanBone::LeftThumbProximal, false, Side::Left, false, {"thumb", "thumbproximal", "thumb01", "thumb1"}},
    {HumanBone::LeftThumbIntermediate, false, Side::Left, false, {"thumbintermediate", "thumb02", "thumb2"}},
    {HumanBone::LeftThumbDistal, false, Side::Left, false, {"thumbdistal", "thumb03", "thumb3"}},

    {HumanBone::LeftIndexProximal, false, Side::Left, false, {"index", "indexfinger", "indexproximal", "indexfingerproximal", "index01", "indexfinger01", "index1", "indexfinger1"}},
    {HumanBone::LeftIndexIntermediate, false, Side::Left, false, {"indexintermediate", "indexfingerintermediate", "index02", "indexfinger02", "index2", "indexfinger2"}},
    {HumanBone::LeftIndexDistal, false, Side::Left, false, {"indexdistal", "indexfingerdistal", "index03", "indexfinger03", "index3", "indexfinger3"}},

    {HumanBone::LeftMiddleProximal, false, Side::Left, false, {"middle", "middlefinger", "finger", "middleproximal", "middlefingerproximal", "fingerproximal", "middle01", "middlefinger01", "finger01", "middle1", "middlefinger1", "finger1"}},
    {HumanBone::LeftMiddleIntermediate, false, Side::Left, false, {"middleintermediate", "middlefingerintermediate", "fingerintermediate", "middle02", "middlefinger02", "finger02", "middle2", "middlefinger2", "finger2"}},
    {HumanBone::LeftMiddleDistal, false, Side::Left, false, {"middledistal", "middlefingerdistal", "fingerdistal", "middle03", "middlefinger03", "finger03", "middle3", "middlefinger3", "finger3"}},

    {HumanBone::LeftRingProximal, false, Side::Left, false, {"ring", "ringfinger", "ringproximal", "ringfingerproximal", "ring01", "ringfinger01", "ring1", "ringfinger1"}},
    {HumanBone::LeftRingIntermediate, false, Side::Left, false, {"ringintermediate", "ringfingerintermediate", "ring02", "ringfinger02", "ring2", "ringfinger2"}},
    {HumanBone::LeftRingDistal, false, Side::Left, false, {"ringdistal", "ringfingerdistal", "ring03", "ringfinger03", "ring3", "ringfinger3"}},

    {HumanBone::LeftLittleProximal, false, Side::Left, false, {"little", "littlefinger", "pinky", "pinkyfinger", "littleproximal", "littlefingerproximal", "pinkyproximal", "pinkyfingerproximal", "little01", "littlefinger01", "pinky01", "pinkyfinger01", "little1", "littlefinger1", "pinky1", "pinkyfinger1"}},
    {HumanBone::LeftLittleIntermediate, false, Side::Left, false, {"littleintermediate", "littlefingerintermediate", "pinkyintermediate", "pinkyfingerintermediate", "little02", "littlefinger02", "pinky02", "pinkyfinger02", "little2", "littlefinger2", "pinky2", "pinkyfinger2"}},
    {HumanBone::LeftLittleDistal, false, Side::Left, false, {"littledistal", "littlefingerdistal", "pinkydistal", "pinkyfingerdistal", "little03", "littlefinger03", "pinky03", "pinkyfinger03", "little3", "littlefinger3", "pinky3", "pinkyfinger3"}},

    // --- Right fingers ---
    // Right side accepts explicit "R" prefix/suffix or bare numeric form
    // (Synty's un-disambiguated "Thumb_01" with NO `_1` trailing).
    {HumanBone::RightThumbProximal, false, Side::Right, true, {"thumb", "thumbproximal", "thumb01", "thumb1"}},
    {HumanBone::RightThumbIntermediate, false, Side::Right, true, {"thumbintermediate", "thumb02", "thumb2"}},
    {HumanBone::RightThumbDistal, false, Side::Right, true, {"thumbdistal", "thumb03", "thumb3"}},

    {HumanBone::RightIndexProximal, false, Side::Right, true, {"index", "indexfinger", "indexproximal", "indexfingerproximal", "index01", "indexfinger01", "index1", "indexfinger1"}},
    {HumanBone::RightIndexIntermediate, false, Side::Right, true, {"indexintermediate", "indexfingerintermediate", "index02", "indexfinger02", "index2", "indexfinger2"}},
    {HumanBone::RightIndexDistal, false, Side::Right, true, {"indexdistal", "indexfingerdistal", "index03", "indexfinger03", "index3", "indexfinger3"}},

    {HumanBone::RightMiddleProximal, false, Side::Right, true, {"middle", "middlefinger", "finger", "middleproximal", "middlefingerproximal", "fingerproximal", "middle01", "middlefinger01", "finger01", "middle1", "middlefinger1", "finger1"}},
    {HumanBone::RightMiddleIntermediate, false, Side::Right, true, {"middleintermediate", "middlefingerintermediate", "fingerintermediate", "middle02", "middlefinger02", "finger02", "middle2", "middlefinger2", "finger2"}},
    {HumanBone::RightMiddleDistal, false, Side::Right, true, {"middledistal", "middlefingerdistal", "fingerdistal", "middle03", "middlefinger03", "finger03", "middle3", "middlefinger3", "finger3"}},

    {HumanBone::RightRingProximal, false, Side::Right, true, {"ring", "ringfinger", "ringproximal", "ringfingerproximal", "ring01", "ringfinger01", "ring1", "ringfinger1"}},
    {HumanBone::RightRingIntermediate, false, Side::Right, true, {"ringintermediate", "ringfingerintermediate", "ring02", "ringfinger02", "ring2", "ringfinger2"}},
    {HumanBone::RightRingDistal, false, Side::Right, true, {"ringdistal", "ringfingerdistal", "ring03", "ringfinger03", "ring3", "ringfinger3"}},

    {HumanBone::RightLittleProximal, false, Side::Right, true, {"little", "littlefinger", "pinky", "pinkyfinger", "littleproximal", "littlefingerproximal", "pinkyproximal", "pinkyfingerproximal", "little01", "littlefinger01", "pinky01", "pinkyfinger01", "little1", "littlefinger1", "pinky1", "pinkyfinger1"}},
    {HumanBone::RightLittleIntermediate, false, Side::Right, true, {"littleintermediate", "littlefingerintermediate", "pinkyintermediate", "pinkyfingerintermediate", "little02", "littlefinger02", "pinky02", "pinkyfinger02", "little2", "littlefinger2", "pinky2", "pinkyfinger2"}},
    {HumanBone::RightLittleDistal, false, Side::Right, true, {"littledistal", "littlefingerdistal", "pinkydistal", "pinkyfingerdistal", "little03", "littlefinger03", "pinky03", "pinkyfinger03", "little3", "littlefinger3", "pinky3", "pinkyfinger3"}},
};

constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

struct NormalizedName
{
    // Suffix candidates joined from body tokens. Matches mimic the original
    // regex `$` anchor: an alias must equal one of these suffixes, never a
    // prefix or interior fragment. For body = [a, b, c] the candidates are
    // {"abc", "bc", "c"} — enough to match aliases that anchor to the
    // trailing tokens (e.g. "Hip_Center" → "hipcenter" ; "FK-Spine" → "spine")
    // without falsely matching interior fragments (e.g. "MSTR-Spine_Hips"
    // must NOT match the "spine" alias for Spine — it only emits suffixes
    // "spinehips" and "hips").
    std::vector<std::string> BodyCandidates;
    bool HasLeft = false;
    bool HasRight = false;
};

bool IsSeparator(char c)
{
    return c == '.' || c == '_' || c == '-' || c == ':' || std::isspace(static_cast<unsigned char>(c));
}

bool IsLeftToken(std::string_view token)
{
    return token == "l" || token == "left";
}

bool IsRightToken(std::string_view token)
{
    return token == "r" || token == "right";
}

// Control-rig namespace tokens that should be stripped wherever they appear,
// not just at the leading position. BlenRig / Auto-Rig Pro emit names like
// `IK-MSTR-Wrist.L` where the body bone is buried behind multiple namespace
// tokens; `FK-W-Foot.L` further wraps the body with a `W` (world-space)
// helper. A bone literally named just `W` is the only edge case this
// over-filters — extremely unlikely in real rigs.
bool IsIgnorableToken(std::string_view token)
{
    return token == "mixamorig" || token == "fk" || token == "ik" ||
           token == "def" || token == "ctr" || token == "mstr" ||
           token == "str" || token == "w";
}

void PushToken(std::vector<std::string>& tokens, std::string& current)
{
    if (current.empty())
        return;
    tokens.push_back(current);
    current.clear();
}

bool EndsInDigit(std::string_view token)
{
    return !token.empty() && std::isdigit(static_cast<unsigned char>(token.back())) != 0;
}

NormalizedName Normalize(std::string_view boneName)
{
    NormalizedName out;

    // 1) Split into tokens by explicit separators AND camel-case boundaries.
    std::vector<std::string> tokens;
    std::string current;
    current.reserve(boneName.size());

    char prevOriginal = '\0';
    for (const char ch : boneName)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (IsSeparator(ch))
        {
            PushToken(tokens, current);
            prevOriginal = '\0';
            continue;
        }

        if (!current.empty() && std::isupper(uch) &&
            (std::islower(static_cast<unsigned char>(prevOriginal)) ||
             std::isdigit(static_cast<unsigned char>(prevOriginal))))
        {
            PushToken(tokens, current);
        }
        current.push_back(static_cast<char>(std::tolower(uch)));
        prevOriginal = ch;
    }
    PushToken(tokens, current);

    // 2) Classify side, drop namespace tokens, keep the rest as the body.
    std::vector<std::string> body;
    body.reserve(tokens.size());
    for (const auto& token : tokens)
    {
        if (IsLeftToken(token)) { out.HasLeft = true; continue; }
        if (IsRightToken(token)) { out.HasRight = true; continue; }
        if (IsIgnorableToken(token)) continue;
        body.push_back(token);
    }

    // 3) Synty duplicate disambiguation. ufbx splits two bones that share a
    // name by suffixing the second with `_1` (e.g. `Thumb_01_1`). Treat a
    // lone trailing `"1"` token as a left-side marker AND drop it from the
    // body so suffix matching against aliases like `"thumb01"` succeeds.
    // Guarded by the preceding token ending in a digit so we don't strip
    // arbitrary `Foo_1` bones (which should remain unmatched / Side::Any).
    if (body.size() >= 2 && body.back() == "1" && EndsInDigit(body[body.size() - 2]))
    {
        if (!out.HasLeft && !out.HasRight)
            out.HasLeft = true;
        body.pop_back();
    }

    // 4) Generate end-anchored suffix candidates. Equivalent to the original
    // regex `$` anchor: a row's alias must equal one of these suffixes.
    out.BodyCandidates.reserve(body.size());
    for (size_t first = 0; first < body.size(); ++first)
    {
        std::string joined;
        for (size_t i = first; i < body.size(); ++i)
            joined += body[i];
        out.BodyCandidates.push_back(std::move(joined));
    }

    return out;
}

uint32 RequiredBoneCount()
{
    static const uint32 s_Count = []
    {
        uint32 c = 0;
        for (const auto& row : kRows)
            if (row.Required) ++c;
        return c;
    }();
    return s_Count;
}

uint32 OptionalBoneCount()
{
    static const uint32 s_Count = []
    {
        uint32 c = 0;
        for (const auto& row : kRows)
            if (!row.Required) ++c;
        return c;
    }();
    return s_Count;
}

bool SideMatches(const PatternRow& row, const NormalizedName& name)
{
    if (row.RequiredSide == Side::Any)
        return !name.HasLeft && !name.HasRight;
    if (row.RequiredSide == Side::Left)
        return name.HasLeft && !name.HasRight;
    return (name.HasRight && !name.HasLeft) ||
           (row.AllowUnmarkedRight && !name.HasLeft && !name.HasRight);
}

bool AliasMatches(const NormalizedName& name, const std::vector<std::string_view>& aliases)
{
    for (std::string_view candidate : name.BodyCandidates)
    {
        for (std::string_view alias : aliases)
        {
            if (candidate == alias)
                return true;
        }
    }
    return false;
}

bool RowMatches(const PatternRow& row, const NormalizedName& name)
{
    return SideMatches(row, name) && AliasMatches(name, row.Aliases);
}

} // namespace

const HumanoidNameMatcher::Mapping*
HumanoidNameMatcher::Result::Find(HumanBone canonical) const
{
    const auto idx = static_cast<size_t>(canonical);
    if (idx >= ByCanonical.size()) return nullptr;
    const uint32 mapIdx = ByCanonical[idx];
    if (mapIdx == ~0u || mapIdx >= Mappings.size()) return nullptr;
    return &Mappings[mapIdx];
}

bool HumanoidNameMatcher::IsRequiredBone(HumanBone bone)
{
    for (const auto& row : kRows)
        if (row.Bone == bone)
            return row.Required;
    return false;
}

HumanBone HumanoidNameMatcher::Match(std::string_view boneName) const
{
    if (boneName.empty()) return HumanBone::None;

    const NormalizedName name = Normalize(boneName);
    for (size_t i = 0; i < kRowCount; ++i)
    {
        if (RowMatches(kRows[i], name))
            return kRows[i].Bone;
    }
    return HumanBone::None;
}

HumanoidNameMatcher::Result
HumanoidNameMatcher::MatchSkeleton(const std::vector<std::string>& boneNames) const
{
    Result result;
    result.RequiredTotal = RequiredBoneCount();
    result.OptionalTotal = OptionalBoneCount();

    if (boneNames.empty())
        return result;

    std::vector<NormalizedName> normalized;
    normalized.reserve(boneNames.size());
    for (const auto& boneName : boneNames)
        normalized.push_back(Normalize(boneName));

    // Walk the priority-ordered table. For each canonical bone, find the
    // first un-claimed source bone whose name matches the pattern. This
    // ensures Shoulder/Clavicle wins before UpperArm even when both could
    // match (Synty case).
    std::unordered_set<uint32> claimedSources;
    claimedSources.reserve(boneNames.size());

    for (size_t row = 0; row < kRowCount; ++row)
    {
        const auto& info = kRows[row];

        for (uint32 bone = 0; bone < boneNames.size(); ++bone)
        {
            if (claimedSources.count(bone)) continue;
            const std::string& name = boneNames[bone];
            if (name.empty()) continue;
            if (!RowMatches(info, normalized[bone])) continue;

            Mapping m;
            m.Canonical = info.Bone;
            m.SourceBoneName = name;
            m.CachedSourceIndex = bone;
            const uint32 mapIdx = static_cast<uint32>(result.Mappings.size());
            result.Mappings.push_back(std::move(m));
            result.ByCanonical[static_cast<size_t>(info.Bone)] = mapIdx;
            claimedSources.insert(bone);
            if (info.Required) ++result.RequiredMatched;
            else ++result.OptionalMatched;
            break;
        }
    }

    result.Coverage = (result.RequiredTotal > 0)
        ? (static_cast<float>(result.RequiredMatched) / static_cast<float>(result.RequiredTotal))
        : 0.0f;

    return result;
}

} // namespace Animation
} // namespace GameEngine
