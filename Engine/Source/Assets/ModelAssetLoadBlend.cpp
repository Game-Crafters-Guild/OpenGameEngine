// .blend file importer.
//
// Mirrors `ModelAssetLoadFbx.cpp` in shape:
//   - One unnamed-namespace block of converters at the top (they take an
//     AxisConversion from MakeEngineConversion).
//   - `ModelAsset::LoadFromBlendData` walks the parsed fbtBlend scene graph,
//     classifies Object datablocks (Mesh / Armature), builds engine
//     `SkeletonData` from the Armature hierarchy, and emits a `Mesh` per
//     material slot.
//
// Coordinate conversion is ModelImport::MakeEngineConversion(
// BlendImport::SourceConversion(), opts.Axis). Blend source is +X right, +Y
// forward, +Z up; default Mirror X is the B_to_E map (x,y,z)->(-x,z,y).
// Bake/mirrors are inspector kv, same as FBX/glTF. All axis fixups go
// through the helpers below.

#include "Assets/ModelAsset.h"
#include "Assets/AnimationClip.h"
#include "Assets/FbxLoaderOptions.h"
#include "ModelAxisConversion.h"
#include "BlendAxisConversion.h"
#include "Logger/Logger.h"

#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Animation/SkeletonData.h"
#include "Types/StringId.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(GE_HAVE_FBTBLEND)
// fbtBlend.h emits MSVC narrowing warnings (size_t -> int, etc.) that would be
// promoted to errors under the engine's default /WX. Suppress at the include
// boundary so this TU stays warnings-clean.
#  if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable: 4244)
#    pragma warning(disable: 4267)
#    pragma warning(disable: 4305)
#    pragma warning(disable: 4309)
#    pragma warning(disable: 4838)
#    pragma warning(disable: 4996)
#  endif
#  include "fbtBlend.h"
#  if defined(_MSC_VER)
#    pragma warning(pop)
#  endif
#endif

namespace GameEngine
{

#if defined(GE_HAVE_FBTBLEND)

namespace
{

// Blender Object.type DNA enum values (stable across versions).
// See `source/blender/makesdna/DNA_object_types.h` upstream.
constexpr short kOB_EMPTY    = 0;
constexpr short kOB_MESH     = 1;
constexpr short kOB_ARMATURE = 25;

// Blender's linear unit is metres natively (no cm->m conversion like FBX).
constexpr float kBlenderUnitScale = 1.0f;

// ---------------------------------------------------------------
// The four canonical converters: one helper per conversion kind, no variants.
// ---------------------------------------------------------------

void ConvertBlendVec3Position(const float src[3], float unitScale,
                              const ModelImport::AxisConversion& c, float* outXYZ)
{
    const ModelImport::Vec3 v = ModelImport::ConvertVec3({src[0], src[1], src[2]}, c, unitScale);
    outXYZ[0] = v.x;
    outXYZ[1] = v.y;
    outXYZ[2] = v.z;
}

// Quaternion: Blender DNA is (W, X, Y, Z); engine is (X, Y, Z, W). Reorder
// first, then the same ConvertQuat LoadFBX / clip keys use.
void ConvertBlendQuat(const float srcWXYZ[4], const ModelImport::AxisConversion& c, float* outXYZW)
{
    const ModelImport::Quat q = ModelImport::ConvertQuat(
        {srcWXYZ[1], srcWXYZ[2], srcWXYZ[3], srcWXYZ[0]}, c);
    outXYZW[0] = q.x;
    outXYZW[1] = q.y;
    outXYZW[2] = q.z;
    outXYZW[3] = q.w;
}

void ConvertBlendVec3Scale(const float src[3], const ModelImport::AxisConversion& c, float* outXYZ)
{
    const ModelImport::Vec3 v = ModelImport::ConvertScale({src[0], src[1], src[2]}, c);
    outXYZ[0] = v.x;
    outXYZ[1] = v.y;
    outXYZ[2] = v.z;
}

// Blender DNA stores Bone::arm_mat as row-major float[4][4] (mat[row][col],
// translation in the last row). Convert the 3x3 and translation through the
// same C R C^T / C t path ConvertAffine uses for FBX.
void StoreBlendMatrixAsColumnMajorTargetSpace(const float src[4][4], float unitScale,
                                              const ModelImport::AxisConversion& c, float* out16)
{
    const float src3x3[3][3] = {
        {src[0][0], src[0][1], src[0][2]},
        {src[1][0], src[1][1], src[1][2]},
        {src[2][0], src[2][1], src[2][2]},
    };
    ModelImport::ConvertAffine(src3x3, src[3][0], src[3][1], src[3][2], c, unitScale, out16);
}

void StoreIdentityColumnMajor(float* out16)
{
    std::memset(out16, 0, 16 * sizeof(float));
    out16[0] = out16[5] = out16[10] = out16[15] = 1.0f;
}

bool InvertColumnMajor4x4(const float* m, float* invOut)
{
    // Adapted from MESA glm gluInvertMatrix; column-major in/out.
    float inv[16];
    inv[0] =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] =  m[4]*m[ 9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[ 9];
    inv[12]= -m[4]*m[ 9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[ 9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[ 9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[ 9];
    inv[13]=  m[0]*m[ 9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[ 9];
    inv[2] =  m[1]*m[ 6]*m[15] - m[1]*m[ 7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[ 7] - m[13]*m[3]*m[ 6];
    inv[6] = -m[0]*m[ 6]*m[15] + m[0]*m[ 7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[ 7] + m[12]*m[3]*m[ 6];
    inv[10]=  m[0]*m[ 5]*m[15] - m[0]*m[ 7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[ 7] - m[12]*m[3]*m[ 5];
    inv[14]= -m[0]*m[ 5]*m[14] + m[0]*m[ 6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[ 6] + m[12]*m[2]*m[ 5];
    inv[3] = -m[1]*m[ 6]*m[11] + m[1]*m[ 7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[ 9]*m[2]*m[ 7] + m[ 9]*m[3]*m[ 6];
    inv[7] =  m[0]*m[ 6]*m[11] - m[0]*m[ 7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[ 8]*m[2]*m[ 7] - m[ 8]*m[3]*m[ 6];
    inv[11]= -m[0]*m[ 5]*m[11] + m[0]*m[ 7]*m[ 9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[ 9] - m[ 8]*m[1]*m[ 7] + m[ 8]*m[3]*m[ 5];
    inv[15]=  m[0]*m[ 5]*m[10] - m[0]*m[ 6]*m[ 9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[ 9] + m[ 8]*m[1]*m[ 6] - m[ 8]*m[2]*m[ 5];
    const float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (std::fabs(det) < 1e-20f)
        return false;
    const float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i)
        invOut[i] = inv[i] * invDet;
    return true;
}

// Decompose a column-major translation+rotation matrix to TRS components.
// Assumes uniform-or-near-uniform scale (Blender bone arm_mat is rigid +
// uniform-scale by construction unless the artist pushes scale into rest).
void DecomposeRestTRS(const float m[16], float outT[3], float outQuatXYZW[4], float outS[3])
{
    outT[0] = m[12];
    outT[1] = m[13];
    outT[2] = m[14];

    // Extract per-axis scale from column lengths.
    float scaleX = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    float scaleY = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    float scaleZ = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
    if (scaleX < 1e-8f) scaleX = 1.0f;
    if (scaleY < 1e-8f) scaleY = 1.0f;
    if (scaleZ < 1e-8f) scaleZ = 1.0f;

    outS[0] = scaleX;
    outS[1] = scaleY;
    outS[2] = scaleZ;

    // Normalize rotation columns.
    const float r00 = m[0]/scaleX, r10 = m[1]/scaleX, r20 = m[2]/scaleX;
    const float r01 = m[4]/scaleY, r11 = m[5]/scaleY, r21 = m[6]/scaleY;
    const float r02 = m[8]/scaleZ, r12 = m[9]/scaleZ, r22 = m[10]/scaleZ;

    // Standard rotation-matrix to quaternion (Shoemake).
    const float trace = r00 + r11 + r22;
    float qx, qy, qz, qw;
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        qw = 0.25f * s;
        qx = (r21 - r12) / s;
        qy = (r02 - r20) / s;
        qz = (r10 - r01) / s;
    }
    else if (r00 > r11 && r00 > r22)
    {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        qw = (r21 - r12) / s;
        qx = 0.25f * s;
        qy = (r01 + r10) / s;
        qz = (r02 + r20) / s;
    }
    else if (r11 > r22)
    {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        qw = (r02 - r20) / s;
        qx = (r01 + r10) / s;
        qy = 0.25f * s;
        qz = (r12 + r21) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        qw = (r10 - r01) / s;
        qx = (r02 + r20) / s;
        qy = (r12 + r21) / s;
        qz = 0.25f * s;
    }
    outQuatXYZW[0] = qx;
    outQuatXYZW[1] = qy;
    outQuatXYZW[2] = qz;
    outQuatXYZW[3] = qw;
}

// Multiply two column-major 4x4 matrices: out = a * b.
void Multiply4x4(const float* a, const float* b, float* out)
{
    float tmp[16];
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a[k*4 + row] * b[col*4 + k];
            tmp[col*4 + row] = s;
        }
    }
    std::memcpy(out, tmp, 16 * sizeof(float));
}

// Strip Blender's two-character ID name prefix ("OB", "ME", "AR", ...).
// The DNA `ID.name` is stored as `<TT><actual-name>\0` where TT is the type
// code and the printable name starts at index 2.
std::string StripIdPrefix(const char* idName)
{
    if (!idName) return {};
    if (std::strlen(idName) < 2) return std::string(idName);
    return std::string(idName + 2);
}

// Walk a fbtList of Bone* and recursively flatten into a parent-indexed
// SkeletonData. Returns the number of bones added to the output arrays.
struct BoneAccumulator
{
    std::vector<int32>& Parent;
    std::vector<float>& RestTranslation;
    std::vector<float>& RestRotation;
    std::vector<float>& RestScale;
    std::vector<float>& RestLocalMatrix;
    std::vector<float>& InverseBind;
    std::vector<float>& BindPose;
    std::vector<std::string>& BoneNames;
    std::unordered_map<const Blender::Bone*, uint32>& BoneToIndex;
    float UnitScale;
    const ModelImport::AxisConversion* Conv = nullptr;
};

void AccumulateBoneSubtree(BoneAccumulator& acc,
                           const Blender::Bone* bone,
                           int32 parentIndex)
{
    if (!bone) return;

    const uint32 index = static_cast<uint32>(acc.Parent.size());
    acc.BoneToIndex.emplace(bone, index);
    acc.Parent.push_back(parentIndex);

    // Bone::arm_mat is the bone's world-space rest transform (armature space)
    // as a row-major float[4][4]. Convert to engine column-major target space
    // and use that as the "world rest" — the inverse is the bind matrix.
    float armColumnMajor[16];
    StoreBlendMatrixAsColumnMajorTargetSpace(bone->arm_mat, acc.UnitScale, *acc.Conv, armColumnMajor);

    // Inverse-bind matrix: GPU palette wants the matrix that takes a
    // mesh-local vertex into bone-local space. Mesh root world = identity in
    // armature local space, so InverseBind = inverse(armWorld).
    float inverseBindColumnMajor[16];
    if (!InvertColumnMajor4x4(armColumnMajor, inverseBindColumnMajor))
        StoreIdentityColumnMajor(inverseBindColumnMajor);

    // BindPose = armWorld (used by the same skinning math as the FBX path).
    acc.BindPose.insert(acc.BindPose.end(), armColumnMajor, armColumnMajor + 16);
    acc.InverseBind.insert(acc.InverseBind.end(), inverseBindColumnMajor, inverseBindColumnMajor + 16);

    // Local rest matrix: relative to parent. local = parent.armWorld^-1 * armWorld.
    float localColumnMajor[16];
    if (parentIndex >= 0)
    {
        const float* parentArm = acc.BindPose.data() + static_cast<size_t>(parentIndex) * 16u;
        float parentInv[16];
        if (!InvertColumnMajor4x4(parentArm, parentInv))
            StoreIdentityColumnMajor(parentInv);
        Multiply4x4(parentInv, armColumnMajor, localColumnMajor);
    }
    else
    {
        std::memcpy(localColumnMajor, armColumnMajor, sizeof(localColumnMajor));
    }
    acc.RestLocalMatrix.insert(acc.RestLocalMatrix.end(), localColumnMajor, localColumnMajor + 16);

    // Decompose local matrix to TRS for sample-time channel application.
    float trs_t[3], trs_q[4], trs_s[3];
    DecomposeRestTRS(localColumnMajor, trs_t, trs_q, trs_s);
    acc.RestTranslation.push_back(trs_t[0]);
    acc.RestTranslation.push_back(trs_t[1]);
    acc.RestTranslation.push_back(trs_t[2]);
    acc.RestRotation.push_back(trs_q[0]);
    acc.RestRotation.push_back(trs_q[1]);
    acc.RestRotation.push_back(trs_q[2]);
    acc.RestRotation.push_back(trs_q[3]);
    acc.RestScale.push_back(trs_s[0]);
    acc.RestScale.push_back(trs_s[1]);
    acc.RestScale.push_back(trs_s[2]);

    acc.BoneNames.emplace_back(bone->name);

    // Recurse children.
    for (const Blender::Bone* child = static_cast<const Blender::Bone*>(bone->childbase.first);
         child != nullptr;
         child = child->next)
    {
        AccumulateBoneSubtree(acc, child, static_cast<int32>(index));
    }
}

// Find the Object that hosts a given Mesh datablock (the mesh-Object that
// has `Object.data == mesh`). Used to resolve mesh -> armature link via the
// mesh-Object's parent.
const Blender::Object* FindMeshOwnerObject(const fbtList& objectList, const Blender::Mesh* mesh)
{
    for (const Blender::Object* obj = reinterpret_cast<const Blender::Object*>(objectList.first);
         obj != nullptr;
         obj = reinterpret_cast<const Blender::Object*>(obj->id.next))
    {
        if (obj->type == kOB_MESH && obj->data == mesh)
            return obj;
    }
    return nullptr;
}

void AddVertexInfluence(uint16_t* jointOut, float* weightOut, uint32 boneIndex, float w)
{
    if (w <= 1e-8f) return;

    int smallest = 0;
    for (int k = 1; k < 4; ++k)
    {
        if (weightOut[k] < weightOut[smallest])
            smallest = k;
    }
    const float sum = weightOut[0] + weightOut[1] + weightOut[2] + weightOut[3];
    if (sum < 1e-6f)
    {
        for (int k = 0; k < 4; ++k)
        {
            if (weightOut[k] < 1e-6f)
            {
                jointOut[k] = static_cast<uint16_t>(boneIndex);
                weightOut[k] = w;
                return;
            }
        }
    }
    if (w > weightOut[smallest])
    {
        jointOut[smallest] = static_cast<uint16_t>(boneIndex);
        weightOut[smallest] = w;
    }
}

void NormalizeWeights(float* w)
{
    const float sum = w[0] + w[1] + w[2] + w[3];
    if (sum > 1e-6f)
    {
        const float inv = 1.0f / sum;
        w[0] *= inv;
        w[1] *= inv;
        w[2] *= inv;
        w[3] *= inv;
    }
    else
    {
        w[0] = 1.0f;
        w[1] = w[2] = w[3] = 0.0f;
    }
}

// ---------------------------------------------------------------
// Action extraction (Phase B3).
//
// Blender stores per-bone animation as a list of `FCurve` records on a
// `bAction` datablock. Each FCurve has an `rna_path` like
// `pose.bones["Hips"].location`, an `array_index` (which channel of the
// vector), and a `bezt[]` array of Bezier triples. The job of this section
// is to walk the FCurves, group them by (boneName, propertyKind), bake the
// keyframe samples to engine-space TRS keys, and emit one AnimChannel per
// (bone, path) tuple.
// ---------------------------------------------------------------

// Blender rotation_mode values from `DNA_action_types.h`. Stable across
// versions; we mirror the canonical enum here so we don't have to drag in
// extra fbtBlend headers.
//
//   ROT_MODE_QUAT      = 0
//   ROT_MODE_XYZ       = 1
//   ROT_MODE_XZY       = 2
//   ROT_MODE_YXZ       = 3
//   ROT_MODE_YZX       = 4
//   ROT_MODE_ZXY       = 5
//   ROT_MODE_ZYX       = 6
//   ROT_MODE_AXISANGLE = -1
constexpr short kRotModeQuat       = 0;
constexpr short kRotModeAxisAngle  = -1;

// What kind of property this FCurve drives.
enum class BlendFCurveProp
{
    Location,
    RotationQuaternion,
    RotationEuler,
    RotationAxisAngle,
    Scale,
    Unsupported,
};

struct BlendFCurveBinding
{
    std::string BoneName;
    BlendFCurveProp Prop = BlendFCurveProp::Unsupported;
};

// Parse `pose.bones["BoneName"].location` and friends. We accept either
// double-quoted or single-quoted bone names (Blender writes double quotes;
// being permissive here costs nothing).
//
// Returns Unsupported when the path doesn't target a pose-bone property
// we know how to handle.
BlendFCurveBinding ParseRnaPath(const char* rnaPath)
{
    BlendFCurveBinding out;
    if (!rnaPath) return out;

    constexpr const char* kPrefix = "pose.bones[";
    constexpr size_t kPrefixLen = 11; // strlen("pose.bones[")
    if (std::strncmp(rnaPath, kPrefix, kPrefixLen) != 0)
        return out;

    const char* cursor = rnaPath + kPrefixLen;
    if (*cursor != '"' && *cursor != '\'')
        return out;
    const char quote = *cursor++;
    const char* nameBegin = cursor;
    while (*cursor && *cursor != quote)
        ++cursor;
    if (*cursor != quote)
        return out;
    out.BoneName.assign(nameBegin, static_cast<size_t>(cursor - nameBegin));
    ++cursor; // consume closing quote
    if (*cursor != ']') return out;
    ++cursor; // consume ']'
    if (*cursor != '.') return out;
    ++cursor; // consume '.'

    if (std::strcmp(cursor, "location") == 0)
        out.Prop = BlendFCurveProp::Location;
    else if (std::strcmp(cursor, "rotation_quaternion") == 0)
        out.Prop = BlendFCurveProp::RotationQuaternion;
    else if (std::strcmp(cursor, "rotation_euler") == 0)
        out.Prop = BlendFCurveProp::RotationEuler;
    else if (std::strcmp(cursor, "rotation_axis_angle") == 0)
        out.Prop = BlendFCurveProp::RotationAxisAngle;
    else if (std::strcmp(cursor, "scale") == 0)
        out.Prop = BlendFCurveProp::Scale;
    else
        out.Prop = BlendFCurveProp::Unsupported;

    return out;
}

// Sample an FCurve at the given frame value. v1 (per plan-blender §3) does
// "baked Bezier" — sample at each existing knot using the knot's stored
// value, with linear interpolation between knots. Proper Bezier handle
// evaluation is a B3.1 follow-up; this avoids inventing a curve solver
// while keeping the imported clip indistinguishable from a CYCLES-baked
// clip on linear segments (the common case in artist files).
struct BezKey
{
    float Time;   // seconds (frame / fps)
    float Value;
};

// Extract the (time, value) of every knot of an FCurve. BezTriple stores
// the knot at vec[1] (vec[0] is the left handle, vec[2] is the right).
void ExtractFCurveKnots(const Blender::FCurve* fc, float fps, std::vector<BezKey>& outKnots)
{
    outKnots.clear();
    if (!fc || !fc->bezt || fc->totvert <= 0 || fps <= 0.0f) return;
    outKnots.reserve(static_cast<size_t>(fc->totvert));
    const float invFps = 1.0f / fps;
    for (int i = 0; i < fc->totvert; ++i)
    {
        const Blender::BezTriple& bt = fc->bezt[i];
        BezKey k;
        k.Time  = bt.vec[1][0] * invFps;
        k.Value = bt.vec[1][1];
        outKnots.push_back(k);
    }
    std::sort(outKnots.begin(), outKnots.end(),
              [](const BezKey& a, const BezKey& b) { return a.Time < b.Time; });
}

float SampleKnotsLinear(const std::vector<BezKey>& knots, float t)
{
    if (knots.empty()) return 0.0f;
    if (t <= knots.front().Time) return knots.front().Value;
    if (t >= knots.back().Time)  return knots.back().Value;
    // Binary search for the segment.
    size_t lo = 0;
    size_t hi = knots.size() - 1;
    while (lo + 1 < hi)
    {
        const size_t mid = (lo + hi) / 2;
        if (knots[mid].Time <= t) lo = mid;
        else hi = mid;
    }
    const float t0 = knots[lo].Time;
    const float t1 = knots[lo + 1].Time;
    const float v0 = knots[lo].Value;
    const float v1 = knots[lo + 1].Value;
    if (t1 <= t0) return v0;
    const float u = (t - t0) / (t1 - t0);
    return v0 + (v1 - v0) * u;
}

// Convert Euler triple to quaternion in Blender's ZXY-default convention.
// Blender's rotation_mode determines the order; we honour the bone's
// rotmode here. AXIS_ANGLE is intentionally not handled — see below.
void EulerToQuatBlender(const float euler[3], short rotMode, float outWXYZ[4])
{
    // Per-axis half-angle sin/cos.
    const float hx = euler[0] * 0.5f;
    const float hy = euler[1] * 0.5f;
    const float hz = euler[2] * 0.5f;
    const float cx = std::cos(hx), sx = std::sin(hx);
    const float cy = std::cos(hy), sy = std::sin(hy);
    const float cz = std::cos(hz), sz = std::sin(hz);

    // Per-axis quats in (W,X,Y,Z) order.
    const float qx[4] = { cx, sx, 0.0f, 0.0f };
    const float qy[4] = { cy, 0.0f, sy, 0.0f };
    const float qz[4] = { cz, 0.0f, 0.0f, sz };

    auto mul = [](const float a[4], const float b[4], float out[4]) {
        // (W,X,Y,Z) Hamilton product.
        const float aw = a[0], ax = a[1], ay = a[2], az = a[3];
        const float bw = b[0], bx = b[1], by = b[2], bz = b[3];
        out[0] = aw*bw - ax*bx - ay*by - az*bz;
        out[1] = aw*bx + ax*bw + ay*bz - az*by;
        out[2] = aw*by - ax*bz + ay*bw + az*bx;
        out[3] = aw*bz + ax*by - ay*bx + az*bw;
    };

    // Order tables: Blender's ROT_MODE_<ABC> means apply A first, then B,
    // then C — i.e. final = Rc * Rb * Ra in matrix form. For quaternion
    // composition that corresponds to qFinal = qC * qB * qA (right-to-left
    // because pose v' = R v applies A first when v is on the right).
    auto compose = [&mul, &qx, &qy, &qz](char a, char b, char c, float out[4]) {
        const float* pa = (a == 'X' ? qx : (a == 'Y' ? qy : qz));
        const float* pb = (b == 'X' ? qx : (b == 'Y' ? qy : qz));
        const float* pc = (c == 'X' ? qx : (c == 'Y' ? qy : qz));
        float ba[4];
        mul(pb, pa, ba);
        mul(pc, ba, out);
    };

    switch (rotMode)
    {
    default:
    case 1: compose('X', 'Y', 'Z', outWXYZ); break;
    case 2: compose('X', 'Z', 'Y', outWXYZ); break;
    case 3: compose('Y', 'X', 'Z', outWXYZ); break;
    case 4: compose('Y', 'Z', 'X', outWXYZ); break;
    case 5: compose('Z', 'X', 'Y', outWXYZ); break;
    case 6: compose('Z', 'Y', 'X', outWXYZ); break;
    }
}

// Group key used while bucketing FCurves. Bone-name is hashed once for
// fast equality + flat lookup; collisions are vanishingly rare (StringId
// is FNV-1a 64-bit) and bone-name uniqueness is enforced by Blender.
struct BoneChannelKey
{
    StringId BoneNameHash;
    GameEngine::Animation::AnimPath Path;

    bool operator==(const BoneChannelKey& other) const
    {
        return BoneNameHash == other.BoneNameHash && Path == other.Path;
    }
};

struct BoneChannelKeyHash
{
    size_t operator()(const BoneChannelKey& k) const noexcept
    {
        return static_cast<size_t>(k.BoneNameHash) * 3u + static_cast<size_t>(k.Path);
    }
};

// Per-(bone, prop) bucket while we collect FCurves before flattening.
// `Curves` is indexed by array_index (0..3); empty entries stay at default.
struct BlendChannelBucket
{
    std::string BoneName;
    BlendFCurveProp Prop = BlendFCurveProp::Unsupported;
    short RotMode = 1; // bone rotmode lookup result; default XYZ
    // For Translation / Euler / Scale: 3 slots; Quaternion: 4 slots.
    std::vector<BezKey> Curves[4];
    bool HasComponent[4] = { false, false, false, false };
};

// Walk the action's pose channels and build a name -> rotmode lookup.
// (Pose channels live on the Object that owns this Action via AnimData,
// not on the action itself; we accept an optional override map as the
// authoritative source. Default rotmode = XYZ when the pchan is absent —
// matches Blender's UI default for new bones and is the documented
// fallback in the FCurve code.)
//
// Note: the action object itself doesn't carry pose-channel info; the
// Object that the action is attached to does. For B3 we synthesize the
// rotmode from the FCurves themselves: if the action has a rotation_euler
// FCurve for a bone, that bone's pchan must have been Euler at authoring
// time; if rotation_quaternion is present, the pchan is Quat. This avoids
// the cross-reference walk (Object::pose::chanbase) at extraction time.
//
// Helpers below use the per-bucket `RotMode` field, which we set to:
//   - kRotModeQuat (0)        when the bucket is RotationQuaternion
//   - X..Z explicit Euler     when the bucket is RotationEuler — the
//                             Euler order isn't recoverable from the
//                             FCurve alone, so we ALSO consult the
//                             optional `boneRotModeMap` argument; default
//                             to ROT_MODE_XYZ (1) when absent. v1 ships
//                             with the default; v2 walks Object pose to
//                             pick up custom orders.
//   - kRotModeAxisAngle (-1)  when the bucket is RotationAxisAngle (warned
//                             and skipped at flatten time)

// Collect every keyframe time across an array of curves into a sorted+
// deduplicated time set. This becomes the AnimChannel sample times.
void CollectUnionTimes(const std::vector<BezKey>* curves, int curveCount,
                       std::vector<float>& outTimes)
{
    outTimes.clear();
    for (int i = 0; i < curveCount; ++i)
    {
        for (const auto& k : curves[i])
            outTimes.push_back(k.Time);
    }
    std::sort(outTimes.begin(), outTimes.end());
    outTimes.erase(std::unique(outTimes.begin(), outTimes.end(),
                              [](float a, float b) { return std::fabs(a - b) < 1e-7f; }),
                  outTimes.end());
}

// Flatten one bucket into 1 AnimChannel (translation/scale) or 1
// AnimChannel (rotation, regardless of euler vs quat input). Coord
// conversion is applied component-wise here.
void FlattenBucketToChannel(const BlendChannelBucket& b,
                            float unitScale,
                            uint32 boneIndex,
                            const ModelImport::AxisConversion& axisConversion,
                            std::vector<GameEngine::Animation::AnimChannel>& outChannels)
{
    using namespace GameEngine::Animation;

    if (b.Prop == BlendFCurveProp::RotationAxisAngle)
        return; // skipped at extraction; rare

    AnimChannel chan{};
    chan.boneIndex = boneIndex;
    chan.targetName = b.BoneName;
    chan.targetNameId = b.BoneName.empty() ? 0u : HashStringId(b.BoneName);
    chan.interp = AnimInterp::Linear;

    int curveCount = 0;
    AnimPath path = AnimPath::Translation;
    switch (b.Prop)
    {
    case BlendFCurveProp::Location:           path = AnimPath::Translation; curveCount = 3; break;
    case BlendFCurveProp::RotationEuler:      path = AnimPath::Rotation;    curveCount = 3; break;
    case BlendFCurveProp::RotationQuaternion: path = AnimPath::Rotation;    curveCount = 4; break;
    case BlendFCurveProp::Scale:              path = AnimPath::Scale;       curveCount = 3; break;
    default: return;
    }
    chan.path = path;

    std::vector<float> times;
    CollectUnionTimes(b.Curves, curveCount, times);
    if (times.empty()) return;

    chan.keys.reserve(times.size());
    for (float t : times)
    {
        AnimKeyframe k{};
        // Match InitializeDefaultKeyframe — explicit rather than dragging
        // the symbol across the .cpp boundary.
        k.rotation[3] = 1.0f;
        k.scale[0] = 1.0f;
        k.scale[1] = 1.0f;
        k.scale[2] = 1.0f;
        k.time = t;

        switch (b.Prop)
        {
        case BlendFCurveProp::Location:
        {
            float src[3] = {
                b.HasComponent[0] ? SampleKnotsLinear(b.Curves[0], t) : 0.0f,
                b.HasComponent[1] ? SampleKnotsLinear(b.Curves[1], t) : 0.0f,
                b.HasComponent[2] ? SampleKnotsLinear(b.Curves[2], t) : 0.0f,
            };
            ConvertBlendVec3Position(src, unitScale, axisConversion, k.translation);
            break;
        }
        case BlendFCurveProp::Scale:
        {
            float src[3] = {
                b.HasComponent[0] ? SampleKnotsLinear(b.Curves[0], t) : 1.0f,
                b.HasComponent[1] ? SampleKnotsLinear(b.Curves[1], t) : 1.0f,
                b.HasComponent[2] ? SampleKnotsLinear(b.Curves[2], t) : 1.0f,
            };
            ConvertBlendVec3Scale(src, axisConversion, k.scale);
            break;
        }
        case BlendFCurveProp::RotationQuaternion:
        {
            // Blender stores (W, X, Y, Z) on disk; the FCurve array_index
            // is 0=W, 1=X, 2=Y, 3=Z. Sample then ConvertBlendQuat.
            const float w = b.HasComponent[0] ? SampleKnotsLinear(b.Curves[0], t) : 1.0f;
            const float x = b.HasComponent[1] ? SampleKnotsLinear(b.Curves[1], t) : 0.0f;
            const float y = b.HasComponent[2] ? SampleKnotsLinear(b.Curves[2], t) : 0.0f;
            const float z = b.HasComponent[3] ? SampleKnotsLinear(b.Curves[3], t) : 0.0f;
            const float srcWXYZ[4] = { w, x, y, z };
            ConvertBlendQuat(srcWXYZ, axisConversion, k.rotation);
            // Renormalize against linear-blend drift between Bezier knots.
            const float lenSq = k.rotation[0]*k.rotation[0] + k.rotation[1]*k.rotation[1]
                              + k.rotation[2]*k.rotation[2] + k.rotation[3]*k.rotation[3];
            if (lenSq > 1e-12f)
            {
                const float invLen = 1.0f / std::sqrt(lenSq);
                k.rotation[0] *= invLen;
                k.rotation[1] *= invLen;
                k.rotation[2] *= invLen;
                k.rotation[3] *= invLen;
            }
            break;
        }
        case BlendFCurveProp::RotationEuler:
        {
            const float euler[3] = {
                b.HasComponent[0] ? SampleKnotsLinear(b.Curves[0], t) : 0.0f,
                b.HasComponent[1] ? SampleKnotsLinear(b.Curves[1], t) : 0.0f,
                b.HasComponent[2] ? SampleKnotsLinear(b.Curves[2], t) : 0.0f,
            };
            float quatWXYZ[4];
            EulerToQuatBlender(euler, b.RotMode, quatWXYZ);
            // Same converter as the quaternion path — keeps the per-datum
            // table single-call-site honest.
            ConvertBlendQuat(quatWXYZ, axisConversion, k.rotation);
            const float lenSq = k.rotation[0]*k.rotation[0] + k.rotation[1]*k.rotation[1]
                              + k.rotation[2]*k.rotation[2] + k.rotation[3]*k.rotation[3];
            if (lenSq > 1e-12f)
            {
                const float invLen = 1.0f / std::sqrt(lenSq);
                k.rotation[0] *= invLen;
                k.rotation[1] *= invLen;
                k.rotation[2] *= invLen;
                k.rotation[3] *= invLen;
            }
            break;
        }
        default:
            break;
        }

        chan.keys.push_back(k);
    }

    if (!chan.keys.empty())
        outChannels.push_back(std::move(chan));
}

// Walk all FCurves in the Action (which can hang off either `groups`
// or the top-level `curves` ListBase, depending on file age) and bucket
// them by (bone, prop). Returns the bucket map keyed for fast lookup.
//
// Bone-name -> rotmode is provided externally when available (e.g. via
// `boneRotModeMap` populated from the Object's bPose). When absent, Euler
// FCurves use the Blender UI default of XYZ (kRotModeXYZ = 1).
//
// Two storage layouts coexist in real .blend files:
//   - Legacy (pre-4.4): FCurves live on action->curves with grouping
//     metadata on action->groups. Each FCurve carries an rna_path string.
//   - Slotted (4.4+):   FCurves live under bAction.layer_array[i] ->
//     ActionStrip[j] -> ActionStripKeyframeData[data_index] ->
//     channelbag_array[k] -> fcurve_array[m].
// In addition, fbtBlend's name-based DNA remap can drop the rna_path
// pointer on real Blender 4.0 files (observed: ellie). When that
// happens, we infer (bone, property) from group context: bActionGroup
// owns a contiguous range of FCurves whose order is location (3 curves
// at array_index 0..2), then rotation (4 curves quaternion or 3 curves
// euler), then scale (3 curves). The order is enforced by Blender's
// keyframing code (see `keyframing.cc::insert_keyframe_action`).
using BlendBucketMap = std::unordered_map<BoneChannelKey, BlendChannelBucket, BoneChannelKeyHash>;

void AddFCurveToBucket(const Blender::FCurve* fc,
                       const std::string& boneName,
                       BlendFCurveProp prop,
                       const std::unordered_map<std::string, short>* boneRotModeMap,
                       float fps,
                       BlendBucketMap& outBuckets)
{
    if (boneName.empty() || prop == BlendFCurveProp::Unsupported
        || prop == BlendFCurveProp::RotationAxisAngle)
        return;

    BoneChannelKey key;
    key.BoneNameHash = HashStringId(boneName);
    switch (prop)
    {
    case BlendFCurveProp::Location:           key.Path = GameEngine::Animation::AnimPath::Translation; break;
    case BlendFCurveProp::RotationEuler:
    case BlendFCurveProp::RotationQuaternion: key.Path = GameEngine::Animation::AnimPath::Rotation; break;
    case BlendFCurveProp::Scale:              key.Path = GameEngine::Animation::AnimPath::Scale; break;
    default: return;
    }

    BlendChannelBucket& bucket = outBuckets[key];
    if (bucket.BoneName.empty())
    {
        bucket.BoneName = boneName;
        bucket.Prop = prop;
        if (prop == BlendFCurveProp::RotationQuaternion)
            bucket.RotMode = kRotModeQuat;
        else if (prop == BlendFCurveProp::RotationEuler)
        {
            bucket.RotMode = 1;
            if (boneRotModeMap)
            {
                auto it = boneRotModeMap->find(boneName);
                if (it != boneRotModeMap->end() && it->second > 0)
                    bucket.RotMode = it->second;
            }
        }
    }
    else if (bucket.Prop == BlendFCurveProp::RotationEuler &&
             prop == BlendFCurveProp::RotationQuaternion)
    {
        bucket.Prop = BlendFCurveProp::RotationQuaternion;
        bucket.RotMode = kRotModeQuat;
    }

    const int idx = fc->array_index;
    if (idx < 0 || idx > 3) return;
    ExtractFCurveKnots(fc, fps, bucket.Curves[idx]);
    bucket.HasComponent[idx] = !bucket.Curves[idx].empty();
}

// Infer (bone, prop) from a group of FCurves when rna_path is null.
// Blender writes per-bone groups in this canonical order:
//   location.x/y/z, rotation_(euler|quaternion).{0..2|0..3}, scale.x/y/z.
// The boundaries are detectable from the array_index sequence:
//   - "location": 3 curves at indices 0,1,2
//   - "rotation_quaternion": 4 curves at indices 0,1,2,3 (W,X,Y,Z)
//   - "rotation_euler": 3 curves at indices 0,1,2
//   - "scale": 3 curves at indices 0,1,2
// We detect a fresh property when array_index resets back to 0 after
// a previous non-zero index, OR after consuming the documented count
// for the current property. Mixed-property authoring (rare) → falls
// back to the index-reset heuristic.
void ProcessGroupedFCurvesNoRnaPath(const Blender::bActionGroup* group,
                                    const std::unordered_map<std::string, short>* boneRotModeMap,
                                    float fps,
                                    BlendBucketMap& outBuckets)
{
    if (!group) return;
    const std::string boneName = group->name;
    const Blender::FCurve* first = static_cast<const Blender::FCurve*>(group->channels.first);
    const Blender::FCurve* last  = static_cast<const Blender::FCurve*>(group->channels.last);
    if (!first) return;

    // Snapshot the group's FCurves as a flat array first so we can walk
    // them deterministically (the linked-list walk respects the
    // [first, last] boundary).
    std::vector<const Blender::FCurve*> fcurves;
    fcurves.reserve(16);
    for (const Blender::FCurve* fc = first; fc != nullptr; fc = fc->next)
    {
        fcurves.push_back(fc);
        if (fc == last) break;
    }
    if (fcurves.empty()) return;

    // Walk the array using the index-reset heuristic.
    size_t i = 0;
    int phase = 0; // 0 = expect location, 1 = expect rotation, 2 = expect scale, 3+ = exhausted
    while (i < fcurves.size() && phase <= 2)
    {
        // Collect the consecutive run starting at i with monotonically
        // non-decreasing array_index. The run ends when array_index
        // resets to a value <= the previous one OR we've consumed
        // 3 (location/scale/euler) or 4 (quaternion) curves and the
        // index reset.
        const size_t runStart = i;
        int prevIdx = -1;
        while (i < fcurves.size())
        {
            const int idx = fcurves[i]->array_index;
            if (idx <= prevIdx) break;
            prevIdx = idx;
            ++i;
        }
        const size_t runLen = i - runStart;
        if (runLen == 0) { ++i; continue; }

        BlendFCurveProp prop = BlendFCurveProp::Unsupported;
        if (phase == 0 && runLen >= 3)
        {
            prop = BlendFCurveProp::Location;
            phase = 1;
        }
        else if (phase == 1)
        {
            // Rotation: 4 curves = quaternion (W,X,Y,Z), 3 curves = euler.
            if (runLen >= 4)
                prop = BlendFCurveProp::RotationQuaternion;
            else if (runLen >= 3)
                prop = BlendFCurveProp::RotationEuler;
            phase = 2;
        }
        else if (phase == 2 && runLen >= 3)
        {
            prop = BlendFCurveProp::Scale;
            phase = 3;
        }

        if (prop == BlendFCurveProp::Unsupported)
            continue; // Non-canonical run; ignore.

        // Emit the FCurves of this run into the bucket.
        for (size_t k = runStart; k < i; ++k)
            AddFCurveToBucket(fcurves[k], boneName, prop, boneRotModeMap, fps, outBuckets);
    }
}

void CollectActionFCurves(const Blender::bAction* action,
                          const std::unordered_map<std::string, short>* boneRotModeMap,
                          float fps,
                          BlendBucketMap& outBuckets)
{
    if (!action) return;

    // Whether ANY FCurve has a non-null rna_path in this Action. fbtBlend's
    // DNA remap on Blender 4.0 files drops rna_path uniformly across all
    // FCurves; if the first one's path is null, the whole Action falls
    // back to group-based inference.
    bool anyRnaPath = false;
    for (const Blender::FCurve* fc = static_cast<const Blender::FCurve*>(action->curves.first);
         fc != nullptr; fc = fc->next)
    {
        if (fc->rna_path) { anyRnaPath = true; break; }
    }

    auto processFCurveWithPath = [&](const Blender::FCurve* fc) {
        if (!fc || !fc->rna_path) return;
        const auto bind = ParseRnaPath(fc->rna_path);
        if (bind.Prop == BlendFCurveProp::Unsupported || bind.BoneName.empty()) return;
        AddFCurveToBucket(fc, bind.BoneName, bind.Prop, boneRotModeMap, fps, outBuckets);
    };

    if (anyRnaPath)
    {
        // Legacy path: walk action->curves directly.
        for (const Blender::FCurve* fc = static_cast<const Blender::FCurve*>(action->curves.first);
             fc != nullptr; fc = fc->next)
        {
            processFCurveWithPath(fc);
        }
    }
    else
    {
        // Fallback path: rna_path got dropped by DNA remap; group-based
        // inference. Each bActionGroup has `name` = bone name, and a
        // (first, last) FCurve range whose array_index sequence tells
        // us the property.
        for (const Blender::bActionGroup* g = static_cast<const Blender::bActionGroup*>(action->groups.first);
             g != nullptr;
             g = g->next)
        {
            ProcessGroupedFCurvesNoRnaPath(g, boneRotModeMap, fps, outBuckets);
        }
    }

    // New path (Blender 4.4+ "Slotted Actions"): FCurves live in
    //   bAction.layer_array[i] -> strip_array[j] -> data_index ->
    //   bAction.strip_keyframe_data_array[data_index] ->
    //   channelbag_array[k] -> fcurve_array[m].
    // Most recent rigs (e.g. ellie 4.0+) ship on this layout exclusively;
    // legacy `curves` ListBase is empty. Walk both paths to remain
    // version-agnostic.
    for (int li = 0; li < action->layer_array_num; ++li)
    {
        const Blender::ActionLayer* layer = action->layer_array ? action->layer_array[li] : nullptr;
        if (!layer || !layer->strip_array) continue;
        for (int si = 0; si < layer->strip_array_num; ++si)
        {
            const Blender::ActionStrip* strip = layer->strip_array[si];
            if (!strip) continue;
            const int dataIdx = strip->data_index;
            if (dataIdx < 0 || dataIdx >= action->strip_keyframe_data_array_num) continue;
            const Blender::ActionStripKeyframeData* skd =
                action->strip_keyframe_data_array
                    ? action->strip_keyframe_data_array[dataIdx]
                    : nullptr;
            if (!skd || !skd->channelbag_array) continue;
            for (int ci = 0; ci < skd->channelbag_array_num; ++ci)
            {
                const Blender::ActionChannelBag* bag = skd->channelbag_array[ci];
                if (!bag || !bag->fcurve_array) continue;
                for (int fi = 0; fi < bag->fcurve_array_num; ++fi)
                {
                    const Blender::FCurve* fc = bag->fcurve_array[fi];
                    if (fc) processFCurveWithPath(fc);
                }
            }
        }
    }
}

// Build the bone-name -> rotmode map by walking every Object's pose
// channels in the .blend. Multiple objects may share the same armature,
// but rotmode is per-pose-channel which is per-Object — we take the first
// pchan we see for each bone-name. This is good enough for v1 (rigs in
// the wild use a consistent rotmode across the whole armature).
void BuildBoneRotModeMap(const fbtList& objectList,
                         std::unordered_map<std::string, short>& outMap)
{
    for (const Blender::Object* obj = reinterpret_cast<const Blender::Object*>(objectList.first);
         obj != nullptr;
         obj = reinterpret_cast<const Blender::Object*>(obj->id.next))
    {
        if (!obj->pose) continue;
        for (const Blender::bPoseChannel* pc = static_cast<const Blender::bPoseChannel*>(obj->pose->chanbase.first);
             pc != nullptr;
             pc = pc->next)
        {
            if (!pc->name[0]) continue;
            outMap.emplace(pc->name, pc->rotmode);
        }
    }
}

// fbtBlend reads the 12-byte header Blender wrote before 5.0 ("BLENDER-v405":
// pointer size, endianness, version). Blender 5.0 and later write
// "BLENDER17-01v0500", with the header length in digits after "BLENDER", and a
// block layout fbtBlend does not parse.
bool IsBlender5Header(const fbtFixedString<12>& header)
{
    const char* text = header.c_str();
    return header.size() >= 9 && std::memcmp(text, "BLENDER", 7) == 0
        && text[7] >= '0' && text[7] <= '9' && text[8] >= '0' && text[8] <= '9';
}

// Why fbtFile::parse rejected a .blend, and what the user can do, for every
// status it returns: a FileStatus, or a positive byte count when the block scan
// reaches the end of the data without an ENDB block.
const char* DescribeBlendParseFailure(int status, bool isUncompressed, const fbtFixedString<12>& header)
{
    if (IsBlender5Header(header))
    {
        return "it was saved by Blender 5.0 or later, which the model loader does not read; "
               "export it from Blender as FBX or glTF instead";
    }
    switch (status)
    {
        case fbtFile::FS_FAILED:
            // Uncompressed data always opens, so there only the built-in SDNA
            // tables, which do not depend on the file, can fail; on compressed
            // data the cause that depends on the file is that neither decoder
            // produced bytes.
            return isUncompressed
                ? "the .blend parser's built-in type tables failed to load"
                : "it neither starts with a Blender header nor decompresses as gzip or zstd";
        case fbtFile::FS_INV_HEADER_STR:
            // Uncompressed data carries the "BLENDER" magic and passes the
            // header check, so only decompressed data gets here.
            return "it decompresses as gzip or zstd but does not hold Blender data";
        case fbtFile::FS_BAD_ALLOC:
        case fbtFile::FS_LINK_FAILED:
            // fbtFile::link fails only when an allocation fails.
            return "a block is too large to allocate, so it is corrupt or larger than the available memory";
        default:
            return "its Blender data is truncated or corrupt; save it again from Blender, "
                   "or export it as FBX or glTF";
    }
}

} // unnamed namespace

bool ModelAsset::LoadFromBlendData(const Vector<uint8>& data,
                                   const std::filesystem::path& modelPathForExternalTextures,
                                   const FbxLoaderOptions& loaderOptions)
{
    if (data.empty())
    {
        Logger::Log::Error("ModelAsset::LoadFromBlendData '{}': empty buffer", GetName());
        return false;
    }

    m_ParseOptionsHash = HashFbxLoaderOptions(loaderOptions);
    const ModelImport::AxisConversion axisConversion =
        ModelImport::MakeEngineConversion(BlendImport::SourceConversion(), loaderOptions.Axis);

    // Staging async clones construct with m_SkeletonId == 0. They must not
    // rewrite on-disk sidecars; first insert and in-place live loads may.
    const bool writeSidecars = m_SkeletonId != 0;

    fbtBlend blend;
    // Detect compression by sniffing the "BLENDER" magic prefix. When present
    // the buffer is uncompressed; otherwise it's gzip (Blender < 3.0) or zstd
    // (Blender >= 3.0) and fbtBlend dispatches to the matching codec.
    const bool isUncompressed =
        data.size() >= 7 && std::memcmp(data.data(), "BLENDER", 7) == 0;
    const int mode = isUncompressed ? fbtFile::PM_UNCOMPRESSED : fbtFile::PM_COMPRESSED;
    const int parseStatus = blend.parse(data.data(), data.size(), mode, /*suppressHeaderWarning=*/false);
    if (parseStatus != fbtFile::FS_OK)
    {
        Logger::Log::Error("ModelAsset::LoadFromBlendData '{}': not a readable .blend file: {} (fbtBlend status {})",
                           GetPath().string(),
                           DescribeBlendParseFailure(parseStatus, isUncompressed, blend.getHeader()),
                           parseStatus);
        return false;
    }

    // ---------------------------------------------------------------
    // Pass 1: pick the first Armature datablock (if any) and build skeleton.
    // .blend files can technically carry multiple armatures; v1 takes the
    // first non-empty one — same convention as the FBX path's "first skin
    // deformer wins" rule. Plan-blender §6 R3 captures multi-armature as a
    // B6+ follow-up.
    // ---------------------------------------------------------------
    const Blender::bArmature* armature = nullptr;
    for (const Blender::bArmature* arm = reinterpret_cast<const Blender::bArmature*>(blend.m_armature.first);
         arm != nullptr;
         arm = reinterpret_cast<const Blender::bArmature*>(arm->id.next))
    {
        if (arm->bonebase.first != nullptr)
        {
            armature = arm;
            break;
        }
    }

    std::vector<int32> parentIdx;
    std::vector<float> restT;
    std::vector<float> restR;
    std::vector<float> restS;
    std::vector<float> restLocalRM;
    std::vector<float> bindPose;
    std::vector<float> inverseBind;
    std::vector<std::string> boneNames;
    std::unordered_map<const Blender::Bone*, uint32> boneToIndex;
    // Map bone name -> index for vertex-group resolution.
    std::unordered_map<std::string, uint32> boneNameToIndex;

    if (armature != nullptr)
    {
        BoneAccumulator acc{
            parentIdx, restT, restR, restS, restLocalRM,
            inverseBind, bindPose, boneNames, boneToIndex,
            kBlenderUnitScale, &axisConversion
        };
        for (const Blender::Bone* root = static_cast<const Blender::Bone*>(armature->bonebase.first);
             root != nullptr;
             root = root->next)
        {
            AccumulateBoneSubtree(acc, root, -1);
        }
        for (const auto& [bone, idx] : boneToIndex)
        {
            if (bone && bone->name[0] != '\0')
                boneNameToIndex.emplace(bone->name, idx);
        }
    }

    const uint32 boneCount = static_cast<uint32>(parentIdx.size());

    if (boneCount > 0)
    {
        Animation::SkeletonData* skeleton = RetainOrCreateSkeleton(boneCount);
        if (!skeleton)
        {
            Logger::Log::Error("ModelAsset::LoadFromBlendData '{}': SkeletonStore::Get returned null", GetName());
            return false;
        }

        skeleton->Parent = std::move(parentIdx);
        skeleton->RestTranslation = std::move(restT);
        skeleton->RestRotation = std::move(restR);
        skeleton->RestScale = std::move(restS);
        skeleton->RestLocalMatrix = std::move(restLocalRM);
        skeleton->BindPose = std::move(bindPose);
        skeleton->InverseBind = std::move(inverseBind);
        skeleton->BoneNames = std::move(boneNames);
        skeleton->BoneCount = boneCount;
        skeleton->SourceModelPath = GetPath();

        skeleton->BuildBoneNameLookup();

        // For .blend, every bone is a skinning joint by construction (the
        // armature owns only deform-and-control bones; control rigs are
        // discoverable via the BONE_NO_DEFORM flag, but Phase B2 includes all
        // bones and lets retargeting filter at the chain level — Phase B4
        // closes the deform-only filter as part of HumanoidNameMatcher).
        skeleton->SkinJointCount = boneCount;
        skeleton->JointNodes.resize(boneCount);
        for (uint32 i = 0; i < boneCount; ++i)
            skeleton->JointNodes[i] = i;
        skeleton->ComputeTopologicalSort();

        skeleton->MeshRootNode = -1;
        StoreIdentityColumnMajor(skeleton->MeshRootWorld);
        StoreIdentityColumnMajor(skeleton->SkeletonRootWorld);
    }

    // ---------------------------------------------------------------
    // Pass 2: Materials. v1 = stub default + per-mesh material slot count
    // (the Blender Mesh's Material** mat array). Texture extraction +
    // Principled BSDF parsing is Phase B5.
    // ---------------------------------------------------------------
    m_EmbeddedImages.clear();
    m_Materials.clear();
    {
        ImportedMaterialData defaultMat{};
        defaultMat.Name = "DefaultMaterial";
        defaultMat.DiffuseColor[0] = defaultMat.DiffuseColor[1] = defaultMat.DiffuseColor[2] = 0.8f;
        defaultMat.DiffuseColor[3] = 1.0f;
        defaultMat.SpecularColor[0] = defaultMat.SpecularColor[1] = defaultMat.SpecularColor[2] = 0.2f;
        defaultMat.Shininess = 32.0f;
        defaultMat.Metallic = 0.0f;
        defaultMat.Roughness = 0.5f;
        m_Materials.push_back(std::move(defaultMat));
    }

    // ---------------------------------------------------------------
    // Pass 3: Meshes. Walk Mesh datablocks and emit one engine Mesh per
    // material part (mirrors FBX path; per-material splitting keeps each
    // draw call single-material per the batch-compactor contract).
    // ---------------------------------------------------------------
    size_t totalVerts = 0;
    size_t totalIndices = 0;
    size_t meshesSkippedAttributeStorage = 0;
    size_t meshesProcessed = 0;
    size_t meshesEnumerated = 0;
    size_t meshesEarlySkipZeroCounts = 0;

    for (const Blender::Mesh* meshDb = reinterpret_cast<const Blender::Mesh*>(blend.m_mesh.first);
         meshDb != nullptr;
         meshDb = reinterpret_cast<const Blender::Mesh*>(meshDb->id.next))
    {
        ++meshesEnumerated;
        if (!meshDb || meshDb->totvert <= 0 || meshDb->totloop <= 0 || meshDb->totpoly <= 0)
        {
            ++meshesEarlySkipZeroCounts;
            continue;
        }
        ++meshesProcessed;

        // Blender 3.x onwards moved vertex positions / corner indices into
        // CustomData attribute layers; the legacy `mvert/mloop/mpoly`
        // pointers are populated only on older files (or files re-saved with
        // the legacy compat option). Detect that case and use the attribute
        // path instead. The attribute layout the importer reads:
        //   - vdata layer named "position" with type CD_PROP_FLOAT3 (id 50).
        //     We fall back to scanning by type if name match fails.
        //   - corner_vert (loop->v) lives in ldata as CD_PROP_INT32 (id 11)
        //     under name ".corner_vert". Fallback layer type also accepted.
        //   - poly_offset_indices on the Mesh itself (replaces MPoly.loopstart
        //     / .totloop). totpoly is still the count.
        //   - material index per face: pdata layer "material_index" CD_PROP_INT32.
        const Blender::MVert* mvert = meshDb->mvert;
        const Blender::MLoop* mloop = meshDb->mloop;
        const Blender::MPoly* mpoly = meshDb->mpoly;

        // Attribute-storage fallback pointers.
        const float* attrPositions = nullptr;        // float3 per vertex (3 * totvert floats)
        const int*   attrCornerVert = nullptr;       // int per loop (totloop ints)
        const int*   polyOffsets = meshDb->poly_offset_indices;
        const int*   attrMaterialIndex = nullptr;    // int per poly (totpoly ints)

        // Blender DNA CustomData type IDs (probed from real 4.0.x files).
        // Type IDs have shifted over Blender versions, so we accept multiple
        // candidates. The actually-encountered values on our 4.0.x sample
        // bundles are FLOAT3=48, FLOAT2=49, INT32=11, MLOOPUV=16; we keep 47
        // for older 3.x files that may sneak in.
        constexpr int kCD_PROP_FLOAT3_A = 47;
        constexpr int kCD_PROP_FLOAT3_B = 48;
        constexpr int kCD_PROP_INT32    = 11;
        constexpr int kCD_MLOOPUV       = 16;
        constexpr int kCD_PROP_FLOAT2   = 49;

        auto findLayer = [](const Blender::CustomData& cd, int type, const char* name) -> const void*
        {
            if (!cd.layers || cd.totlayer <= 0) return nullptr;
            // Prefer a name match.
            if (name)
            {
                for (int i = 0; i < cd.totlayer; ++i)
                {
                    const Blender::CustomDataLayer& L = cd.layers[i];
                    if (L.type == type && L.data && L.name[0] != '\0'
                        && std::strcmp(L.name, name) == 0)
                        return L.data;
                }
            }
            // Fall back to first layer of the matching type.
            for (int i = 0; i < cd.totlayer; ++i)
            {
                const Blender::CustomDataLayer& L = cd.layers[i];
                if (L.type == type && L.data) return L.data;
            }
            return nullptr;
        };

        // Search attribute_storage by attribute name (4.5+ "all attributes
        // are equal" model). Returns the data pointer for the first matching
        // attribute name (stops on the first hit; storage_type is opaque to
        // us but the data array is a flat float/int buffer regardless).
        auto findAttributeByName = [](const Blender::AttributeStorage& storage,
                                       const char* name) -> const void*
        {
            if (!storage.dna_attributes || storage.dna_attributes_num <= 0)
                return nullptr;
            for (int i = 0; i < storage.dna_attributes_num; ++i)
            {
                const Blender::Attribute& a = storage.dna_attributes[i];
                if (a.name && a.data && std::strcmp(a.name, name) == 0)
                    return a.data;
            }
            return nullptr;
        };

        if (!mvert)
        {
            attrPositions = static_cast<const float*>(findLayer(meshDb->vdata, kCD_PROP_FLOAT3_A, "position"));
            if (!attrPositions)
                attrPositions = static_cast<const float*>(findLayer(meshDb->vdata, kCD_PROP_FLOAT3_B, "position"));
            if (!attrPositions)
                attrPositions = static_cast<const float*>(findAttributeByName(meshDb->attribute_storage, "position"));
        }
        if (!mloop)
        {
            attrCornerVert = static_cast<const int*>(findLayer(meshDb->ldata, kCD_PROP_INT32, ".corner_vert"));
            if (!attrCornerVert)
                attrCornerVert = static_cast<const int*>(findAttributeByName(meshDb->attribute_storage, ".corner_vert"));
        }
        attrMaterialIndex = static_cast<const int*>(findLayer(meshDb->pdata, kCD_PROP_INT32, "material_index"));
        if (!attrMaterialIndex)
            attrMaterialIndex = static_cast<const int*>(findAttributeByName(meshDb->attribute_storage, "material_index"));

        // We need either the legacy mvert+mloop+mpoly trio, or the new
        // attrPositions+attrCornerVert+polyOffsets trio. Modern Blender
        // 4.0+ files (saved with the SDNA rename `poly_offset_indices ->
        // face_offset_indices`) leak the offsets through fbtBlend's
        // name-based field remap as a null pointer, which kills attribute
        // extraction on those files. Fall back to a uniform-face-size
        // heuristic when totloop divides evenly into totpoly and we have
        // valid attrPositions+attrCornerVert; this catches quad-only and
        // tri-only meshes (the common cases) but can't handle mixed n-gons
        // — those produce wrong geometry, which the visual gate at B5 will
        // catch.
        const bool legacyOk = mvert && mloop && mpoly;
        const bool attrOk   = attrPositions && attrCornerVert && polyOffsets;
        bool uniformFaceFallback = false;
        int uniformCornersPerFace = 0;
        if (!legacyOk && !attrOk
            && attrPositions && attrCornerVert
            && meshDb->totpoly > 0
            && (meshDb->totloop % meshDb->totpoly) == 0)
        {
            uniformFaceFallback = true;
            uniformCornersPerFace = meshDb->totloop / meshDb->totpoly;
        }
        if (!legacyOk && !attrOk && !uniformFaceFallback)
        {
            ++meshesSkippedAttributeStorage;
            continue;
        }

        // Resolve the owning Object so we can find the Armature parent and
        // (later, B5) any modifiers driving deformation.
        const Blender::Object* meshOwner = FindMeshOwnerObject(blend.m_object, meshDb);
        const bool isSkinnedToOurArmature = (armature != nullptr)
            && (meshOwner != nullptr)
            && (meshOwner->parent != nullptr)
            && (meshOwner->parent->type == kOB_ARMATURE);

        // Per-material parts: mat_nr index on each polygon determines the slot.
        const int materialSlotCount = std::max<int>(1, static_cast<int>(meshDb->totcol));
        std::vector<std::vector<int32>> polysBySlot(static_cast<size_t>(materialSlotCount));
        for (int polyIdx = 0; polyIdx < meshDb->totpoly; ++polyIdx)
        {
            int slot = 0;
            if (mpoly)
                slot = mpoly[polyIdx].mat_nr;
            else if (attrMaterialIndex)
                slot = attrMaterialIndex[polyIdx];
            slot = std::clamp<int>(slot, 0, materialSlotCount - 1);
            polysBySlot[static_cast<size_t>(slot)].push_back(polyIdx);
        }

        // Resolve vertex-group index -> bone index map. MDeformVert::dw[].def_nr
        // indexes into Mesh.vertex_group_names (a ListBase of bDeformGroup).
        std::vector<int32> vgroupToBone;
        if (isSkinnedToOurArmature)
        {
            int vgroupCount = 0;
            for (const Blender::bDeformGroup* g = static_cast<const Blender::bDeformGroup*>(meshDb->vertex_group_names.first);
                 g != nullptr;
                 g = g->next)
            {
                ++vgroupCount;
            }
            vgroupToBone.assign(static_cast<size_t>(vgroupCount), -1);
            int gi = 0;
            for (const Blender::bDeformGroup* g = static_cast<const Blender::bDeformGroup*>(meshDb->vertex_group_names.first);
                 g != nullptr;
                 g = g->next, ++gi)
            {
                if (auto it = boneNameToIndex.find(g->name); it != boneNameToIndex.end())
                    vgroupToBone[static_cast<size_t>(gi)] = static_cast<int32>(it->second);
            }
        }

        const std::string baseName = StripIdPrefix(meshDb->id.name);

        for (int slot = 0; slot < materialSlotCount; ++slot)
        {
            if (polysBySlot[static_cast<size_t>(slot)].empty())
                continue;

            Mesh mesh;
            mesh.Name = materialSlotCount > 1
                ? (baseName + "_" + std::to_string(slot))
                : baseName;
            mesh.MaterialIndex = 0; // v1: stub default material; Phase B5 wires PBR mats.
            mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = std::numeric_limits<float>::max();
            mesh.MaxBounds[0] = mesh.MaxBounds[1] = mesh.MaxBounds[2] = std::numeric_limits<float>::lowest();

            // Vertex dedup: split positions by (positionIndex, uv, mat_nr) so
            // material seams produce distinct vertices. We don't have separate
            // normal/tangent indices in MLoop; normals are computed per loop
            // below from face geometry when not present in CustomData layers.
            // For Phase B2 we emit per-corner unique vertices keyed on
            // (vertIndex, uvX, uvY) so material splits and UV seams produce
            // separate vertices.
            std::unordered_map<uint64_t, uint32> vertCache;
            std::vector<uint32_t> vertSourceVertex; // parallel to mesh.Vertices

            // UV sources: legacy MLoopUV (CD_MLOOPUV) or attribute-storage
            // FLOAT2 layer (commonly named "UVMap"). UV is purely cosmetic at
            // the B2 mesh-level — null-safe in addLoopVertex.
            const Blender::MLoopUV* mloopuv = static_cast<const Blender::MLoopUV*>(
                findLayer(meshDb->ldata, kCD_MLOOPUV, nullptr));
            const float* attrUV = nullptr;
            if (!mloopuv)
            {
                attrUV = static_cast<const float*>(findLayer(meshDb->ldata, kCD_PROP_FLOAT2, nullptr));
                if (!attrUV)
                    attrUV = static_cast<const float*>(findAttributeByName(meshDb->attribute_storage, "UVMap"));
            }

            // Per-loop -> vertex index resolver.
            auto loopToVert = [&](int loopIdx) -> int
            {
                if (mloop) return mloop[loopIdx].v;
                if (attrCornerVert) return attrCornerVert[loopIdx];
                return -1;
            };
            // Per-vertex position resolver (writes into outPos[3]).
            auto readPosition = [&](int vertIdx, float outCo[3])
            {
                if (mvert)
                {
                    outCo[0] = mvert[vertIdx].co[0];
                    outCo[1] = mvert[vertIdx].co[1];
                    outCo[2] = mvert[vertIdx].co[2];
                }
                else if (attrPositions)
                {
                    outCo[0] = attrPositions[vertIdx * 3 + 0];
                    outCo[1] = attrPositions[vertIdx * 3 + 1];
                    outCo[2] = attrPositions[vertIdx * 3 + 2];
                }
                else
                {
                    outCo[0] = outCo[1] = outCo[2] = 0.0f;
                }
            };

            // Lambdas closed over mesh + caches.
            auto addLoopVertex = [&](int loopIdx) -> uint32
            {
                const int vertIdx = loopToVert(loopIdx);
                if (vertIdx < 0 || vertIdx >= meshDb->totvert)
                    return 0u;
                float co[3];
                readPosition(vertIdx, co);
                float uv[2] = { 0.0f, 0.0f };
                if (mloopuv)
                {
                    uv[0] = mloopuv[loopIdx].uv[0];
                    uv[1] = mloopuv[loopIdx].uv[1];
                }
                else if (attrUV)
                {
                    uv[0] = attrUV[loopIdx * 2 + 0];
                    uv[1] = attrUV[loopIdx * 2 + 1];
                }
                // Quantize UV for dedup key (8 decimal places). Avoids
                // tiny FP differences inflating cache size, while preserving
                // genuine seam splits.
                const uint32_t uvKeyX = static_cast<uint32_t>(static_cast<int32_t>(uv[0] * 1e6f));
                const uint32_t uvKeyY = static_cast<uint32_t>(static_cast<int32_t>(uv[1] * 1e6f));
                const uint64_t key =
                    (static_cast<uint64_t>(static_cast<uint32_t>(vertIdx)) << 32) ^
                    (static_cast<uint64_t>(uvKeyX) << 16) ^
                    static_cast<uint64_t>(uvKeyY);
                if (auto it = vertCache.find(key); it != vertCache.end())
                    return it->second;

                Vertex v{};
                ConvertBlendVec3Position(co, kBlenderUnitScale, axisConversion, v.Position);
                v.TexCoords[0] = uv[0];
                // Blender V origin is at the bottom; flip to match Vulkan
                // textures (row 0 at top).
                v.TexCoords[1] = 1.0f - uv[1];
                // Normals: MVert.no isn't reliable in modern Blender (often
                // computed lazily); we accumulate from face geometry below.
                v.Normal[0] = v.Normal[1] = v.Normal[2] = 0.0f;

                for (int c = 0; c < 3; ++c)
                {
                    mesh.MinBounds[c] = std::min(mesh.MinBounds[c], v.Position[c]);
                    mesh.MaxBounds[c] = std::max(mesh.MaxBounds[c], v.Position[c]);
                }

                const uint32 newIndex = static_cast<uint32>(mesh.Vertices.size());
                mesh.Vertices.push_back(v);
                vertSourceVertex.push_back(static_cast<uint32_t>(vertIdx));
                vertCache.emplace(key, newIndex);
                return newIndex;
            };

            // Triangulate each polygon via fan triangulation and emit indices.
            // Polygon range source:
            //   legacy: mpoly[polyIdx].loopstart, mpoly[polyIdx].totloop.
            //   attribute: poly_offset_indices is a prefix-sum of length
            //              totpoly+1; loopStart = polyOffsets[polyIdx],
            //              totLoop = polyOffsets[polyIdx+1] - polyOffsets[polyIdx].
            //   uniform fallback: totLoop = totloop/totpoly for every poly
            //              (correct only for tri-only / quad-only / fixed-N-gon
            //              meshes; mixed topology produces malformed geometry —
            //              flagged for B5).
            for (int polyIdx : polysBySlot[static_cast<size_t>(slot)])
            {
                int loopStart = 0;
                int totLoop = 0;
                if (mpoly)
                {
                    loopStart = mpoly[polyIdx].loopstart;
                    totLoop = mpoly[polyIdx].totloop;
                }
                else if (polyOffsets)
                {
                    loopStart = polyOffsets[polyIdx];
                    totLoop = polyOffsets[polyIdx + 1] - polyOffsets[polyIdx];
                }
                else if (uniformFaceFallback)
                {
                    loopStart = polyIdx * uniformCornersPerFace;
                    totLoop = uniformCornersPerFace;
                }
                if (totLoop < 3) continue;
                if (loopStart < 0 || loopStart + totLoop > meshDb->totloop) continue;

                const uint32 v0 = addLoopVertex(loopStart);
                for (int t = 1; t + 1 < totLoop; ++t)
                {
                    const uint32 v1 = addLoopVertex(loopStart + t);
                    const uint32 v2 = addLoopVertex(loopStart + t + 1);
                    // Blender face loops are already reversed vs engine.
                    // XOR with reverseWinding: default B_to_E is even det
                    // (false) so we keep the old v0,v2,v1 swap. FBX swaps
                    // when reverseWinding is true; here the extra swap is
                    // already applied, so we swap when it is false.
                    mesh.Indices.push_back(v0);
                    if (!axisConversion.reverseWinding)
                    {
                        mesh.Indices.push_back(v2);
                        mesh.Indices.push_back(v1);
                    }
                    else
                    {
                        mesh.Indices.push_back(v1);
                        mesh.Indices.push_back(v2);
                    }
                }
            }

            // Compute smooth per-vertex normals from face geometry; we apply
            // the engine-target conversion via face cross products (already
            // in target space because positions were converted).
            if (!mesh.Vertices.empty() && !mesh.Indices.empty())
            {
                for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
                {
                    const uint32 i0 = mesh.Indices[i + 0];
                    const uint32 i1 = mesh.Indices[i + 1];
                    const uint32 i2 = mesh.Indices[i + 2];
                    const float* p0 = mesh.Vertices[i0].Position;
                    const float* p1 = mesh.Vertices[i1].Position;
                    const float* p2 = mesh.Vertices[i2].Position;
                    const float e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
                    const float e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
                    const float n[3] = {
                        e1[1]*e2[2] - e1[2]*e2[1],
                        e1[2]*e2[0] - e1[0]*e2[2],
                        e1[0]*e2[1] - e1[1]*e2[0],
                    };
                    for (uint32 idx : { i0, i1, i2 })
                    {
                        mesh.Vertices[idx].Normal[0] += n[0];
                        mesh.Vertices[idx].Normal[1] += n[1];
                        mesh.Vertices[idx].Normal[2] += n[2];
                    }
                }
                for (auto& v : mesh.Vertices)
                {
                    const float lenSq = v.Normal[0]*v.Normal[0] + v.Normal[1]*v.Normal[1] + v.Normal[2]*v.Normal[2];
                    if (lenSq > 1e-12f)
                    {
                        const float inv = 1.0f / std::sqrt(lenSq);
                        v.Normal[0] *= inv;
                        v.Normal[1] *= inv;
                        v.Normal[2] *= inv;
                    }
                    else
                    {
                        v.Normal[0] = 0.0f;
                        v.Normal[1] = 1.0f;
                        v.Normal[2] = 0.0f;
                    }
                }
            }

            // Skinning weights: walk MDeformVert per source vertex and emit
            // per-engine-vertex (joint, weight) pairs. v1 caps at 4 influences
            // per the engine palette layout; AddVertexInfluence picks the
            // top-4 by magnitude.
            if (isSkinnedToOurArmature && meshDb->dvert)
            {
                const size_t vcount = mesh.Vertices.size();
                mesh.Joints0.assign(vcount * 4u, 0u);
                mesh.Weights0.assign(vcount * 4u, 0.0f);

                for (size_t vi = 0; vi < vcount; ++vi)
                {
                    const uint32_t srcVert = vertSourceVertex[vi];
                    if (static_cast<int>(srcVert) >= meshDb->totvert)
                        continue;
                    const Blender::MDeformVert& dv = meshDb->dvert[srcVert];
                    uint16_t* j4 = mesh.Joints0.data() + vi * 4u;
                    float*    w4 = mesh.Weights0.data() + vi * 4u;
                    for (int wi = 0; wi < dv.totweight; ++wi)
                    {
                        const Blender::MDeformWeight& dw = dv.dw[wi];
                        const int gi = dw.def_nr;
                        if (gi < 0 || gi >= static_cast<int>(vgroupToBone.size()))
                            continue;
                        const int32 boneIdx = vgroupToBone[static_cast<size_t>(gi)];
                        if (boneIdx < 0)
                            continue;
                        AddVertexInfluence(j4, w4, static_cast<uint32>(boneIdx), dw.weight);
                    }
                    NormalizeWeights(w4);
                }
                mesh.Skinned = !mesh.Vertices.empty()
                            && mesh.Joints0.size() == mesh.Vertices.size() * 4u
                            && mesh.Weights0.size() == mesh.Vertices.size() * 4u;
            }

            if (!mesh.Vertices.empty() && !mesh.Indices.empty())
            {
                totalVerts += mesh.Vertices.size();
                totalIndices += mesh.Indices.size();
                m_Meshes.push_back(std::move(mesh));
            }
        }
    }

    // ---------------------------------------------------------------
    // Animation extraction (Phase B3). Walk every bAction, parse its
    // FCurves' rna_paths, group by (bone, property), bake to engine-space
    // AnimChannels, register one AnimationClip per Action in ClipStore,
    // and emit a sidecar `<modelStem>__<actionName>.anim.json` next to
    // the .blend so re-imports see the registered clips. Mirrors the FBX
    // path's eager-load pattern.
    // ---------------------------------------------------------------

    // Scene framerate. Defaults to 24 fps (Blender's default) when the
    // scene block doesn't expose a custom rate. RenderData stores a
    // numerator (frs_sec, short) and denominator (frs_sec_base, float);
    // effective fps = frs_sec / frs_sec_base.
    float scenefps = 24.0f;
    if (const Blender::Scene* scene = reinterpret_cast<const Blender::Scene*>(blend.m_scene.first);
        scene != nullptr && scene->r.frs_sec > 0)
    {
        const float baseDen = scene->r.frs_sec_base > 0.0f ? scene->r.frs_sec_base : 1.0f;
        scenefps = static_cast<float>(scene->r.frs_sec) / baseDen;
        if (scenefps <= 0.0f) scenefps = 24.0f;
    }

    // Per-bone rotmode map for Euler FCurve evaluation order.
    std::unordered_map<std::string, short> boneRotModeMap;
    BuildBoneRotModeMap(blend.m_object, boneRotModeMap);

    // Walk Actions. Index in source-file order — used to derive stable
    // GUIDs the same way ModelAssetLoadFbx.cpp does for FBX anim stacks.
    using namespace GameEngine::Animation;

    const std::filesystem::path modelStem = GetPath().stem();
    const std::filesystem::path modelDir  = GetPath().parent_path();

    size_t actionIndex = 0;
    size_t actionCount = 0;
    size_t clipsRegistered = 0;
    size_t clipsSidecarWritten = 0;
    size_t totalChannels = 0;
    size_t axisAngleSkipped = 0;
    for (const Blender::bAction* action = reinterpret_cast<const Blender::bAction*>(blend.m_action.first);
         action != nullptr;
         action = reinterpret_cast<const Blender::bAction*>(action->id.next), ++actionIndex)
    {
        const std::string actionName = StripIdPrefix(action->id.name);
        m_AnimationNames.emplace_back(actionName);
        ++actionCount;

        // Bucket this Action's FCurves.
        BlendBucketMap buckets;
        CollectActionFCurves(action, &boneRotModeMap, scenefps, buckets);

        // Detect axis-angle FCurves before flattening (warn-once per Action).
        for (const Blender::FCurve* fc = static_cast<const Blender::FCurve*>(action->curves.first);
             fc != nullptr;
             fc = fc->next)
        {
            if (fc->rna_path && std::strstr(fc->rna_path, "rotation_axis_angle"))
            {
                ++axisAngleSkipped;
                break;
            }
        }

        // Flatten to AnimChannels. boneIndex is resolved from the
        // skeleton's name lookup when available; channels for unknown
        // bones still emit (with boneIndex==UINT32_MAX) so that the
        // retarget pipeline can resolve via targetNameId.
        std::vector<AnimChannel> channels;
        channels.reserve(buckets.size());
        for (auto& [key, bucket] : buckets)
        {
            uint32 boneIndex = std::numeric_limits<uint32>::max();
            if (auto it = boneNameToIndex.find(bucket.BoneName); it != boneNameToIndex.end())
                boneIndex = it->second;
            FlattenBucketToChannel(bucket, kBlenderUnitScale, boneIndex, axisConversion, channels);
        }

        if (channels.empty())
            continue;

        // Compute clip duration from the action's frame_range when set;
        // otherwise from the max key time across channels.
        float duration = 0.0f;
        for (const auto& ch : channels)
        {
            for (const auto& k : ch.keys)
                duration = std::max(duration, k.time);
        }
        if (action->frame_end > action->frame_start && scenefps > 0.0f)
        {
            const float durFromAction =
                (action->frame_end - action->frame_start) / scenefps;
            // Prefer whichever is bigger — frame_end may be set but the
            // FCurves might extend beyond it (rare but possible).
            duration = std::max(duration, durFromAction);
        }

        // Stable derived GUID — same convention as FBX.
        const GUID clipGuid = MintEmbeddedClipGuid(static_cast<uint32>(actionIndex));

        // Sidecar path. Replace any path-unsafe characters in the action
        // name with underscores so the filesystem accepts it.
        std::string safeAction = actionName;
        for (char& c : safeAction)
        {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
                || c == '"' || c == '<' || c == '>' || c == '|')
                c = '_';
        }
        std::filesystem::path sidecarPath =
            modelDir / (modelStem.string() + "__" + safeAction + ".anim.json");

        {
            auto clip = std::make_shared<AnimationClip>(clipGuid, sidecarPath);
            clip->SetSelectedAnimationIndex(static_cast<uint32>(actionIndex));
            clip->SetSourceInfo(GetPath(), static_cast<uint32>(actionIndex));
            clip->SetChannelsAndDurationForTest(channels, duration);

            // Persist as a sidecar so re-imports / external clip browsers
            // can resolve via path. Skip when this is a staging clone
            // replacing an already-published GUID (superseded decode must
            // not rewrite disk). First insert still writes; in-place live
            // reload (writeSidecars) updates the sidecar with new keys.
            auto& clipStore = Engine::Renderer::ClipStore::Instance();
            const bool firstInsert = clipStore.GetIndexIfPresent(clipGuid) == 0;
            if (firstInsert || writeSidecars)
            {
                std::error_code ec;
                std::filesystem::create_directories(modelDir, ec);
                if (clip->SaveToPath(sidecarPath))
                    ++clipsSidecarWritten;
            }

            StageOrPublishRuntimeClip(clipGuid, std::move(clip));
            ++clipsRegistered;
            totalChannels += channels.size();
        }
    }
    m_HasAnimations = actionCount > 0;

    Logger::Log::Info("ModelAsset::LoadFromBlendData '{}' -> meshes={}, verts={}, indices={}, bones={}, actions={}, clips={}, channels={}, sidecars={} (enumerated={}, earlySkip={}, attrSkip={}, fps={:.2f}, axisAngleSkipped={})",
                      GetName(),
                      m_Meshes.size(),
                      totalVerts,
                      totalIndices,
                      boneCount,
                      actionCount,
                      clipsRegistered,
                      totalChannels,
                      clipsSidecarWritten,
                      meshesEnumerated,
                      meshesEarlySkipZeroCounts,
                      meshesSkippedAttributeStorage,
                      scenefps,
                      axisAngleSkipped);
    if (meshesSkippedAttributeStorage > 0)
    {
        Logger::Log::Warning("ModelAsset::LoadFromBlendData '{}': {} mesh(es) skipped — Blender 4.0+ "
                             "renamed `poly_offset_indices` to `face_offset_indices` and fbtBlend's "
                             "name-based remap drops the field. Tracked as Phase B5 follow-up; "
                             "uniform-face heuristic covered quad/tri-only meshes.",
                             GetName(),
                             meshesSkippedAttributeStorage);
    }

    (void)modelPathForExternalTextures; // Phase B5 wires external-texture resolution.

    return !m_Meshes.empty() || m_HasAnimations || boneCount > 0;
}

#else  // !GE_HAVE_FBTBLEND

bool ModelAsset::LoadFromBlendData(const Vector<uint8>&, const std::filesystem::path&, const FbxLoaderOptions&)
{
    Logger::Log::Error("ModelAsset::LoadFromBlendData '{}': built without GE_HAVE_FBTBLEND, .blend support disabled",
                       GetName());
    return false;
}

#endif // GE_HAVE_FBTBLEND

} // namespace GameEngine
