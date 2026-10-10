// Locks ModelImport axis conversion so clip keys and bind/IBM data cannot
// drift onto different target bases. Pins FBX (ufbx Make), glTF identity,
// Blend Y/Z swap, FormatFromPath, and LoadFBX/LoadGLTF bake on vertices.

#include <gtest/gtest.h>

#include "Animation/AnimationClip.h"
#include "Assets/FbxLoaderOptions.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "BlendAxisConversion.h"
#include "FbxAxisConversion.h"
#include "GltfAxisConversion.h"
#include "ModelAxisConversion.h"
#include "ModelAssetFbxTestAccess.h"
#include "StagedTestPaths.h"

#include <ufbx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using GameEngine::Animation::AnimChannel;
using GameEngine::Animation::AnimationClip;
using GameEngine::Animation::AnimKeyframe;
using GameEngine::Animation::AnimPath;
using GameEngine::FbxImport::Make;
using GameEngine::FbxLoaderOptions;
using GameEngine::ModelImport::ApplyEngineMirrors;
using GameEngine::ModelImport::AxisConversion;
using GameEngine::ModelImport::AxisOptions;
using GameEngine::ModelImport::ComposeEngineEulerDegrees;
using GameEngine::ModelImport::ConvertQuat;
using GameEngine::ModelImport::ConvertVec3;
using GameEngine::ModelImport::MakeEngineConversion;
using GameEngine::ModelImport::Quat;
using GameEngine::ModelImport::Vec3;
using GameEngine::GUID;
using GameEngine::ModelAsset;
using GameEngine::ModelAssetFbxTestAccess;
using GameEngine::ModelFormat;
using GameEngine::Vector;
using GameEngine::uint8;

TEST(FbxAxisConversion, DefaultInitIsIdentity)
{
    const AxisConversion c{};
    EXPECT_FLOAT_EQ(c.m[0][0], 1.0f);
    EXPECT_FLOAT_EQ(c.m[1][1], 1.0f);
    EXPECT_FLOAT_EQ(c.m[2][2], 1.0f);
    EXPECT_FALSE(c.reverseWinding);
}

TEST(FbxAxisConversion, YupPlusZFrontIsIdentityBeforeMirrors)
{
    ufbx_coordinate_axes axes{};
    axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;

    const AxisConversion c = Make(axes);
    EXPECT_FLOAT_EQ(c.m[0][0], 1.0f);
    EXPECT_FLOAT_EQ(c.m[0][1], 0.0f);
    EXPECT_FLOAT_EQ(c.m[0][2], 0.0f);
    EXPECT_FLOAT_EQ(c.m[1][0], 0.0f);
    EXPECT_FLOAT_EQ(c.m[1][1], 1.0f);
    EXPECT_FLOAT_EQ(c.m[1][2], 0.0f);
    EXPECT_FLOAT_EQ(c.m[2][0], 0.0f);
    EXPECT_FLOAT_EQ(c.m[2][1], 0.0f);
    EXPECT_FLOAT_EQ(c.m[2][2], 1.0f);
    EXPECT_FALSE(c.reverseWinding);
}

TEST(FbxAxisConversion, DefaultMirrorXMatchesLegacyUnityBake)
{
    ufbx_coordinate_axes axes{};
    axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;

    AxisConversion c = Make(axes);
    ApplyEngineMirrors(c, true, false, false);
    EXPECT_FLOAT_EQ(c.m[0][0], -1.0f);
    EXPECT_FLOAT_EQ(c.m[1][1], 1.0f);
    EXPECT_FLOAT_EQ(c.m[2][2], 1.0f);
    EXPECT_TRUE(c.reverseWinding);
}

TEST(FbxAxisConversion, DoesNotNegateFrontOnYupPlusZ)
{
    ufbx_coordinate_axes axes{};
    axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;

    const AxisConversion c = Make(axes);
    const ufbx_vec3 front = GameEngine::FbxImport::ConvertVec3({0.0, 0.0, 1.0}, c, 1.0f);
    EXPECT_NEAR(front.x, 0.0, 1e-6);
    EXPECT_NEAR(front.y, 0.0, 1e-6);
    EXPECT_NEAR(front.z, 1.0, 1e-6);
}

TEST(FbxAxisConversion, Bake180YTurnsFrontToMinusZ)
{
    ufbx_coordinate_axes axes{};
    axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;

    AxisConversion c = Make(axes);
    ComposeEngineEulerDegrees(c, 0.0f, 180.0f, 0.0f);
    const ufbx_vec3 front = GameEngine::FbxImport::ConvertVec3({0.0, 0.0, 1.0}, c, 1.0f);
    EXPECT_NEAR(front.x, 0.0, 1e-5);
    EXPECT_NEAR(front.y, 0.0, 1e-5);
    EXPECT_NEAR(front.z, -1.0, 1e-5);
}

TEST(FbxAxisConversion, BakeIdentityLeavesFront)
{
    ufbx_coordinate_axes axes{};
    axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;

    AxisConversion c = Make(axes);
    ComposeEngineEulerDegrees(c, 0.0f, 0.0f, 0.0f);
    const ufbx_vec3 front = GameEngine::FbxImport::ConvertVec3({0.0, 0.0, 1.0}, c, 1.0f);
    EXPECT_NEAR(front.x, 0.0, 1e-6);
    EXPECT_NEAR(front.y, 0.0, 1e-6);
    EXPECT_NEAR(front.z, 1.0, 1e-6);
}

TEST(FbxAxisConversion, ZupMayaMapsUpToEngineY)
{
    // Maya Z-up: +X right, +Z up, -Y front (or +Y; either must lift +Z to +Y).
    ufbx_coordinate_axes axes{};
    axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Z;
    axes.front = UFBX_COORDINATE_AXIS_NEGATIVE_Y;

    const AxisConversion c = Make(axes);
    const ufbx_vec3 up = GameEngine::FbxImport::ConvertVec3({0.0, 0.0, 1.0}, c, 1.0f);
    EXPECT_NEAR(up.x, 0.0, 1e-6);
    EXPECT_NEAR(up.y, 1.0, 1e-6);
    EXPECT_NEAR(up.z, 0.0, 1e-6);
}

namespace
{

bool ReadAllBytes(const std::filesystem::path& path, Vector<uint8>& out)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const auto size = in.tellg();
    if (size <= 0)
        return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

std::filesystem::path MayaLodAsciiFixture()
{
    return GameEngine::TestPaths::StagedRoot() / "Engine/Tests/Fixtures/maya_lod_group_7500_ascii.fbx";
}

// Struct default: Unity X-on, bake off. Not an identity mapping.
FbxLoaderOptions DefaultFbxOptions()
{
    return {};
}

bool LoadFixture(const Vector<uint8>& bytes, const FbxLoaderOptions& options, ModelAsset& out)
{
    return ModelAssetFbxTestAccess::Load(out, bytes, options);
}

// A store-licensed FBX from the optional, gitignored Assets/Models/FBXTest/ payload
// (staged by StageTestAssets); empty when this machine has no copy.
std::filesystem::path LicensedFbx(const char* fileName)
{
    const auto p = GameEngine::TestPaths::StagedRoot() / "Assets/Models/FBXTest" / fileName;
    std::error_code ec;
    if (std::filesystem::exists(p, ec) && !ec)
        return p;
    return {};
}

// Y-up +Z-front Null whose Lcl Translation is keyed (0,0,0) -> (10,0,100).
// In-tree so the clip pin does not depend on the licensed FBX payload or StageTestAssets.
Vector<uint8> IsolatedPlusZTranslateAscii()
{
    static const char kAscii[] = R"(; FBX 7.5.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7500
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
		P: "OriginalUnitScaleFactor", "double", "Number", "",1
		P: "TimeMode", "enum", "", "",6
		P: "TimeSpanStart", "KTime", "Time", "",0
		P: "TimeSpanStop", "KTime", "Time", "",46186158000
	}
}
Documents:  {
	Count: 1
	Document: 1000, "", "Scene" {
		Properties70:  {
			P: "SourceObject", "object", "", ""
			P: "ActiveAnimStackName", "KString", "", "", "Take"
		}
		RootNode: 0
	}
}
Definitions:  {
	Version: 100
	Count: 6
	ObjectType: "GlobalSettings" { Count: 1 }
	ObjectType: "Model" { Count: 1 }
	ObjectType: "NodeAttribute" { Count: 1 }
	ObjectType: "AnimationStack" { Count: 1 }
	ObjectType: "AnimationLayer" { Count: 1 }
	ObjectType: "AnimationCurveNode" { Count: 1 }
	ObjectType: "AnimationCurve" { Count: 2 }
}
Objects:  {
	NodeAttribute: 2001, "NodeAttribute::", "Null" {
	}
	Model: 2000, "Model::Mover", "Null" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,0,0
			P: "Lcl Rotation", "Lcl Rotation", "", "A",0,0,0
			P: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1
			P: "DefaultAttributeIndex", "int", "Integer", "",0
		}
		Shading: Y
		Culling: "CullingOff"
	}
	AnimationStack: 3000, "AnimStack::Take", "" {
		Properties70:  {
			P: "LocalStart", "KTime", "Time", "",0
			P: "LocalStop", "KTime", "Time", "",46186158000
			P: "ReferenceStart", "KTime", "Time", "",0
			P: "ReferenceStop", "KTime", "Time", "",46186158000
		}
	}
	AnimationLayer: 3001, "AnimLayer::BaseLayer", "" {
	}
	AnimationCurveNode: 4000, "AnimCurveNode::T", "T" {
		Properties70:  {
			P: "d|X", "Number", "", "A",0
			P: "d|Y", "Number", "", "A",0
			P: "d|Z", "Number", "", "A",0
		}
	}
	AnimationCurve: 5000, "AnimCurve::", "" {
		Default: 0
		KeyVer: 4000
		KeyTime: *2 { a: 0,46186158000 }
		KeyValueFloat: *2 { a: 0,100 }
		KeyAttrFlags: *1 { a: 24840 }
		KeyAttrDataFloat: *4 { a: 0,0,0,0 }
		KeyAttrRefCount: *1 { a: 2 }
	}
	AnimationCurve: 5001, "AnimCurve::", "" {
		Default: 0
		KeyVer: 4000
		KeyTime: *2 { a: 0,46186158000 }
		KeyValueFloat: *2 { a: 0,10 }
		KeyAttrFlags: *1 { a: 24840 }
		KeyAttrDataFloat: *4 { a: 0,0,0,0 }
		KeyAttrRefCount: *1 { a: 2 }
	}
}
Connections:  {
	C: "OO",2000,0
	C: "OO",2001,2000
	C: "OO",3001,3000
	C: "OO",4000,3001
	C: "OP",4000,2000, "Lcl Translation"
	C: "OP",5001,4000, "d|X"
	C: "OP",5000,4000, "d|Z"
}
Takes:  {
	Current: "Take"
	Take: "Take" {
		FileName: "Take.tak"
		LocalTime: 0,46186158000
		ReferenceTime: 0,46186158000
	}
}
)";
    Vector<uint8> bytes(sizeof(kAscii) - 1u);
    std::memcpy(bytes.data(), kAscii, bytes.size());
    return bytes;
}

// Bake opts must match AnimationClip.cpp (ufbx bake then ConvertVec3).
ufbx_baked_anim* BakeFirstStack(ufbx_scene* scene, ufbx_error* err)
{
    if (!scene || scene->anim_stacks.count == 0)
        return nullptr;
    const ufbx_anim* anim = scene->anim_stacks.data[0]->anim;
    const double fileFps = scene->settings.frames_per_second > 0.0
        ? scene->settings.frames_per_second
        : 30.0;
    const double sampleHz = std::max(fileFps, 30.0);
    ufbx_bake_opts bakeOpts{};
    bakeOpts.trim_start_time = true;
    bakeOpts.resample_rate = sampleHz;
    bakeOpts.minimum_sample_rate = sampleHz;
    bakeOpts.step_handling = UFBX_BAKE_STEP_HANDLING_DEFAULT;
    bakeOpts.key_reduction_enabled = true;
    bakeOpts.key_reduction_rotation = true;
    return ufbx_bake_anim(scene, anim, &bakeOpts, err);
}

void ExpectClipKeysUseEngineConversion(const Vector<uint8>& bytes, const std::filesystem::path& path)
{
    ufbx_load_opts opts{};
    opts.ignore_geometry = true;
    opts.ignore_embedded = true;
    opts.load_external_files = false;
    opts.ignore_missing_external_files = true;
    ufbx_error err{};
    ufbx_scene* scene = ufbx_load_memory(bytes.data(), bytes.size(), &opts, &err);
    ASSERT_NE(scene, nullptr) << (err.description.length ? std::string(err.description.data, err.description.length) : path.string());
    if (scene->anim_stacks.count == 0)
    {
        ufbx_free_scene(scene);
        FAIL() << "no anim stacks: " << path.string();
        return;
    }

    const float unitScale = scene->settings.unit_meters > 0.0
        ? static_cast<float>(scene->settings.unit_meters)
        : 1.0f;
    const AxisConversion engineC = GameEngine::FbxImport::MakeEngineConversion(scene->settings.axes, FbxLoaderOptions{});
    AxisConversion legacyC = Make(scene->settings.axes);
    ApplyEngineMirrors(legacyC, true, false, true);

    ufbx_error bakeErr{};
    ufbx_baked_anim* baked = BakeFirstStack(scene, &bakeErr);
    if (!baked)
    {
        ufbx_free_scene(scene);
        FAIL() << "ufbx bake failed: " << path.string();
        return;
    }

    AnimationClip clip(GUID::Generate(), path);
    ASSERT_TRUE(clip.LoadFromData(bytes)) << path.string();
    ASSERT_FALSE(clip.GetChannels().empty()) << path.string();

    int compared = 0;
    int disagreedLegacy = 0;
    for (const ufbx_baked_node& bakedNode : baked->nodes)
    {
        for (const ufbx_baked_vec3& tk : bakedNode.translation_keys)
        {
            if (std::fabs(tk.value.z) < 1.0e-3)
                continue;
            const ufbx_vec3 engineT = GameEngine::FbxImport::ConvertVec3(tk.value, engineC, unitScale);
            const ufbx_vec3 legacyT = GameEngine::FbxImport::ConvertVec3(tk.value, legacyC, unitScale);
            if (std::fabs(engineT.z - legacyT.z) < 1.0e-3)
                continue;

            const AnimChannel* chan = nullptr;
            for (const AnimChannel& ch : clip.GetChannels())
            {
                if (ch.path == AnimPath::Translation && ch.boneIndex == bakedNode.typed_id)
                {
                    chan = &ch;
                    break;
                }
            }
            if (!chan)
                continue;
            for (const auto& key : chan->keys)
            {
                if (std::fabs(key.time - static_cast<float>(tk.time)) > 1.0e-3f)
                    continue;
                EXPECT_NEAR(key.translation[0], engineT.x, 1.0e-3) << path.string();
                EXPECT_NEAR(key.translation[1], engineT.y, 1.0e-3) << path.string();
                EXPECT_NEAR(key.translation[2], engineT.z, 1.0e-3) << path.string();
                if (std::fabs(key.translation[2] - static_cast<float>(legacyT.z)) > 1.0e-3f)
                    ++disagreedLegacy;
                ++compared;
                break;
            }
        }
    }

    ufbx_free_baked_anim(baked);
    ufbx_free_scene(scene);

    ASSERT_GT(compared, 0) << "no Z-discriminating translation keys in " << path.string();
    EXPECT_GT(disagreedLegacy, 0) << "clip keys still match legacy -Z front in " << path.string();
}

} // namespace

// FBX materials have no vertex-colour input: every material the loader imports
// ignores the mesh's colour layer, which glTF materials multiply in by spec.
TEST(FbxMaterialImport, MaterialsIgnoreVertexColor)
{
    const std::filesystem::path fixture = MayaLodAsciiFixture();
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture missing: " << fixture.string();

    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(fixture, bytes));
    ModelAsset model(GUID::Generate(), fixture);
    ASSERT_TRUE(LoadFixture(bytes, DefaultFbxOptions(), model));
    ASSERT_FALSE(model.GetMaterials().empty());
    for (const auto& material : model.GetMaterials())
        EXPECT_TRUE(material.IgnoresVertexColor) << material.Name;
}

TEST(FbxAxisConversion, LoadFbxBake180YMirrorsXAndZ)
{
    // Engine-space 180 Y after the X-reflect conversion maps (x,y,z) -> (-x,y,-z).
    // This is the loader path the inspector bake toggles actually run, not the
    // 3x3 helper alone.
    const std::filesystem::path fixture = MayaLodAsciiFixture();
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture missing: " << fixture.string();

    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(fixture, bytes));

    ModelAsset plain(GUID::Generate(), fixture);
    ModelAsset baked(GUID::Generate(), fixture);
    ASSERT_TRUE(LoadFixture(bytes, DefaultFbxOptions(), plain));

    FbxLoaderOptions bakeY = DefaultFbxOptions();
    bakeY.Axis.BakeRotationDeg[1] = 180.0f;
    bakeY.Axis.BakeRotationAxisEnabled[1] = true;
    ASSERT_TRUE(LoadFixture(bytes, bakeY, baked));

    ASSERT_EQ(plain.GetMeshCount(), baked.GetMeshCount());
    ASSERT_GT(plain.GetMeshCount(), 0u);

    float maxXZ = 0.0f;
    size_t compared = 0;
    for (GameEngine::uint32 i = 0; i < plain.GetMeshCount(); ++i)
    {
        const auto& aMesh = plain.GetMesh(i);
        const auto& bMesh = baked.GetMesh(i);
        ASSERT_EQ(aMesh.Vertices.size(), bMesh.Vertices.size()) << aMesh.Name;
        for (size_t v = 0; v < aMesh.Vertices.size(); ++v)
        {
            const float* a = aMesh.Vertices[v].Position;
            const float* b = bMesh.Vertices[v].Position;
            EXPECT_NEAR(b[0], -a[0], 1e-4f) << aMesh.Name << " v" << v;
            EXPECT_NEAR(b[1], a[1], 1e-4f) << aMesh.Name << " v" << v;
            EXPECT_NEAR(b[2], -a[2], 1e-4f) << aMesh.Name << " v" << v;
            maxXZ = std::max(maxXZ, std::max(std::fabs(a[0]), std::fabs(a[2])));
            ++compared;
        }
    }
    EXPECT_GT(compared, 0u);
    EXPECT_GT(maxXZ, 1e-3f) << "fixture vertices have no XZ extent; bake 180Y would be a tautology";
}

TEST(FbxAxisConversion, LoadFbxMirrorXOffNegatesXRelativeToDefault)
{
    const std::filesystem::path fixture = MayaLodAsciiFixture();
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture missing: " << fixture.string();

    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(fixture, bytes));

    ModelAsset mirrored(GUID::Generate(), fixture);
    ModelAsset raw(GUID::Generate(), fixture);
    ASSERT_TRUE(LoadFixture(bytes, DefaultFbxOptions(), mirrored));

    FbxLoaderOptions noX = DefaultFbxOptions();
    noX.Axis.MirrorAxis[0] = false;
    ASSERT_TRUE(LoadFixture(bytes, noX, raw));

    ASSERT_EQ(mirrored.GetMeshCount(), raw.GetMeshCount());
    ASSERT_GT(mirrored.GetMeshCount(), 0u);
    const auto& a = mirrored.GetMesh(0).Vertices;
    const auto& b = raw.GetMesh(0).Vertices;
    ASSERT_EQ(a.size(), b.size());
    ASSERT_FALSE(a.empty());
    EXPECT_NEAR(b[0].Position[0], -a[0].Position[0], 1e-4f);
    EXPECT_NEAR(b[0].Position[1], a[0].Position[1], 1e-4f);
    EXPECT_NEAR(b[0].Position[2], a[0].Position[2], 1e-4f);
}

TEST(FbxAxisConversion, LoadFbxDisabledBakeAxisIsIdentity)
{
    const std::filesystem::path fixture = MayaLodAsciiFixture();
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture missing: " << fixture.string();

    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(fixture, bytes));

    ModelAsset plain(GUID::Generate(), fixture);
    ModelAsset stored(GUID::Generate(), fixture);
    ASSERT_TRUE(LoadFixture(bytes, DefaultFbxOptions(), plain));

    FbxLoaderOptions disabledY = DefaultFbxOptions();
    disabledY.Axis.BakeRotationDeg[1] = 180.0f;
    disabledY.Axis.BakeRotationAxisEnabled[1] = false;
    ASSERT_TRUE(LoadFixture(bytes, disabledY, stored));

    ASSERT_EQ(plain.GetMeshCount(), stored.GetMeshCount());
    ASSERT_GT(plain.GetMeshCount(), 0u);
    const auto& a = plain.GetMesh(0).Vertices;
    const auto& b = stored.GetMesh(0).Vertices;
    ASSERT_EQ(a.size(), b.size());
    ASSERT_FALSE(a.empty());
    EXPECT_NEAR(b[0].Position[0], a[0].Position[0], 1e-5f);
    EXPECT_NEAR(b[0].Position[1], a[0].Position[1], 1e-5f);
    EXPECT_NEAR(b[0].Position[2], a[0].Position[2], 1e-5f);
}

TEST(FbxAxisConversion, EngineConversionKeepsPlusZFront)
{
    ufbx_coordinate_axes axes{};
    axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;

    const AxisConversion c = GameEngine::FbxImport::MakeEngineConversion(axes, DefaultFbxOptions());
    const ufbx_vec3 front = GameEngine::FbxImport::ConvertVec3({0.0, 0.0, 1.0}, c, 1.0f);
    EXPECT_NEAR(front.x, 0.0, 1e-6);
    EXPECT_NEAR(front.y, 0.0, 1e-6);
    EXPECT_NEAR(front.z, 1.0, 1e-6);
}

TEST(FbxAxisConversion, LegacyNegZFrontFlipsZ)
{
    ufbx_coordinate_axes axes{};
    axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;

    AxisConversion legacy = Make(axes);
    ApplyEngineMirrors(legacy, true, false, true);
    const ufbx_vec3 front = GameEngine::FbxImport::ConvertVec3({0.0, 0.0, 1.0}, legacy, 1.0f);
    EXPECT_NEAR(front.z, -1.0, 1e-6);
}

TEST(FbxAxisConversion, IsolatedAsciiClipKeysKeepPlusZFront)
{
    const Vector<uint8> bytes = IsolatedPlusZTranslateAscii();
    const std::filesystem::path path{"plus_z_translate.fbx"};
    ASSERT_NO_FATAL_FAILURE(ExpectClipKeysUseEngineConversion(bytes, path));

    AnimationClip clip(GUID::Generate(), path);
    ASSERT_TRUE(clip.LoadFromData(bytes));

    const AnimChannel* trans = nullptr;
    for (const AnimChannel& ch : clip.GetChannels())
    {
        if (ch.path == AnimPath::Translation && !ch.keys.empty())
        {
            trans = &ch;
            break;
        }
    }
    ASSERT_NE(trans, nullptr);
    const AnimKeyframe& last = trans->keys.back();
    // Source end key is (+10, 0, +100). Default X-mirror keeps +Z and negates X.
    EXPECT_LT(last.translation[0], 0.0f);
    EXPECT_GT(last.translation[2], 0.0f);
}

TEST(FbxAxisConversion, ClipKeysMatchEngineConversionNotLegacyNegZ_Sheep)
{
    const auto path = LicensedFbx("Sheep.fbx");
    if (path.empty())
        GTEST_SKIP() << "Sheep.fbx not in Assets/Models/FBXTest";
    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(path, bytes)) << path.string();
    ASSERT_NO_FATAL_FAILURE(ExpectClipKeysUseEngineConversion(bytes, path));
}

TEST(FbxAxisConversion, ClipKeysMatchEngineConversionNotLegacyNegZ_SyntyWalk)
{
    const auto path = LicensedFbx("A_Walk_F_Masc.fbx");
    if (path.empty())
        GTEST_SKIP() << "A_Walk_F_Masc.fbx not in Assets/Models/FBXTest";
    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(path, bytes)) << path.string();
    ASSERT_NO_FATAL_FAILURE(ExpectClipKeysUseEngineConversion(bytes, path));
}

TEST(FbxAxisConversion, FormatFromPathClassifiesExtensions)
{
    EXPECT_EQ(ModelAsset::FormatFromPath("mesh.fbx"), ModelFormat::FBX);
    EXPECT_EQ(ModelAsset::FormatFromPath("mesh.FBX"), ModelFormat::FBX);
    EXPECT_EQ(ModelAsset::FormatFromPath("mesh.gltf"), ModelFormat::GLTF);
    EXPECT_EQ(ModelAsset::FormatFromPath("mesh.GLB"), ModelFormat::GLB);
    EXPECT_EQ(ModelAsset::FormatFromPath("mesh.blend"), ModelFormat::BLEND);
    EXPECT_EQ(ModelAsset::FormatFromPath("mesh.obj"), ModelFormat::OBJ);
    EXPECT_EQ(ModelAsset::FormatFromPath("mesh.png"), ModelFormat::Unknown);
}

TEST(FbxAxisConversion, UsesAxisImportOptionsForFbxGltfBlendNotObj)
{
    EXPECT_TRUE(ModelAsset::UsesAxisImportOptions(ModelFormat::FBX));
    EXPECT_TRUE(ModelAsset::UsesAxisImportOptions(ModelFormat::GLTF));
    EXPECT_TRUE(ModelAsset::UsesAxisImportOptions(ModelFormat::GLB));
    EXPECT_TRUE(ModelAsset::UsesAxisImportOptions(ModelFormat::BLEND));
    EXPECT_FALSE(ModelAsset::UsesAxisImportOptions(ModelFormat::OBJ));
    EXPECT_FALSE(ModelAsset::UsesAxisImportOptions(ModelFormat::Unknown));
}

TEST(FbxAxisConversion, GltfSourceConversionIsIdentityBeforeMirrors)
{
    const AxisConversion c = GameEngine::GltfImport::SourceConversion();
    EXPECT_FLOAT_EQ(c.m[0][0], 1.0f);
    EXPECT_FLOAT_EQ(c.m[1][1], 1.0f);
    EXPECT_FLOAT_EQ(c.m[2][2], 1.0f);
    EXPECT_FALSE(c.reverseWinding);
}

TEST(FbxAxisConversion, BlendSourceConversionSwapsYZBeforeMirrors)
{
    const AxisConversion c = GameEngine::BlendImport::SourceConversion();
    const Vec3 x = ConvertVec3({1.0f, 0.0f, 0.0f}, c, 1.0f);
    const Vec3 y = ConvertVec3({0.0f, 1.0f, 0.0f}, c, 1.0f);
    const Vec3 z = ConvertVec3({0.0f, 0.0f, 1.0f}, c, 1.0f);
    EXPECT_NEAR(x.x, 1.0, 1e-6);
    EXPECT_NEAR(x.y, 0.0, 1e-6);
    EXPECT_NEAR(x.z, 0.0, 1e-6);
    EXPECT_NEAR(y.x, 0.0, 1e-6);
    EXPECT_NEAR(y.y, 0.0, 1e-6);
    EXPECT_NEAR(y.z, 1.0, 1e-6);
    EXPECT_NEAR(z.x, 0.0, 1e-6);
    EXPECT_NEAR(z.y, 1.0, 1e-6);
    EXPECT_NEAR(z.z, 0.0, 1e-6);
    EXPECT_TRUE(c.reverseWinding);
}

TEST(FbxAxisConversion, BlendDefaultMirrorsMatchLegacyBtoE)
{
    const AxisConversion c = MakeEngineConversion(
        GameEngine::BlendImport::SourceConversion(), AxisOptions{});
    const Vec3 p = ConvertVec3({1.0f, 2.0f, 3.0f}, c, 1.0f);
    EXPECT_NEAR(p.x, -1.0, 1e-6);
    EXPECT_NEAR(p.y, 3.0, 1e-6);
    EXPECT_NEAR(p.z, 2.0, 1e-6);
    EXPECT_FALSE(c.reverseWinding);
    // Engine-order quat (X,Y,Z,W). B_to_E on the vector part is (-qx, qz, qy).
    const Quat q = ConvertQuat({0.0f, 1.0f, 0.0f, 0.0f}, c);
    EXPECT_NEAR(q.x, 0.0, 1e-5);
    EXPECT_NEAR(q.y, 0.0, 1e-5);
    EXPECT_NEAR(std::fabs(q.z), 1.0, 1e-5);
    EXPECT_NEAR(q.w, 0.0, 1e-5);
}

std::filesystem::path BlendSamplePendulum()
{
    return GameEngine::TestPaths::StagedRoot()
        / "Tests/BlendSamples/anim-fund-rigs"
        / "animation_fundamental_rigs_release_01/pendulum.blend";
}

TEST(FbxAxisConversion, LoadBlendDefaultNegatesXRelativeToMirrorXOff)
{
    const auto path = BlendSamplePendulum();
    if (!std::filesystem::exists(path))
        GTEST_SKIP() << "blend-samples pendulum.blend not present";
    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(path, bytes)) << path.string();

    ModelAsset mirrored(GUID::Generate(), path);
    ModelAsset raw(GUID::Generate(), path);
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadBlend(mirrored, bytes, DefaultFbxOptions()));
    FbxLoaderOptions noX = DefaultFbxOptions();
    noX.Axis.MirrorAxis[0] = false;
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadBlend(raw, bytes, noX));
    ASSERT_GT(mirrored.GetMeshCount(), 0u);
    ASSERT_EQ(mirrored.GetMeshCount(), raw.GetMeshCount());
    ASSERT_GT(mirrored.GetMesh(0).Vertices.size(), 0u);
    const float* a = mirrored.GetMesh(0).Vertices[0].Position;
    const float* b = raw.GetMesh(0).Vertices[0].Position;
    EXPECT_NEAR(a[0], -b[0], 1e-4f);
    EXPECT_NEAR(a[1], b[1], 1e-4f);
    EXPECT_NEAR(a[2], b[2], 1e-4f);
    EXPECT_GT(std::fabs(b[0]) + std::fabs(b[1]) + std::fabs(b[2]), 1e-4f)
        << "pendulum v0 has no extent; X-mirror pin would be a tautology";
}

// Triangle: (1,0,0) +X, (0,1,0) up, (0,0,1) front. data URI is little-endian float32.
Vector<uint8> IsolatedYUpFrontGltf()
{
    static const char kJson[] = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{
    "bufferView": 0,
    "componentType": 5126,
    "count": 3,
    "type": "VEC3",
    "max": [1, 1, 1],
    "min": [0, 0, 0]
  }],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]
})";
    Vector<uint8> bytes(sizeof(kJson) - 1u);
    std::memcpy(bytes.data(), kJson, bytes.size());
    return bytes;
}

TEST(FbxAxisConversion, IsolatedGltfDefaultKeepsUpAndFront)
{
    const Vector<uint8> bytes = IsolatedYUpFrontGltf();
    ModelAsset asset(GUID::Generate(), std::filesystem::path{"up_front.gltf"});
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, bytes, DefaultFbxOptions()));
    ASSERT_GT(asset.GetMeshCount(), 0u);
    const auto& mesh = asset.GetMesh(0);
    ASSERT_EQ(mesh.Vertices.size(), 3u);
    EXPECT_NEAR(mesh.Vertices[0].Position[0], -1.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[0].Position[1], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[0].Position[2], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[1].Position[0], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[1].Position[1], 1.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[1].Position[2], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[2].Position[0], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[2].Position[1], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[2].Position[2], 1.0f, 1e-5f);
}

TEST(FbxAxisConversion, IsolatedGltfBakeX90TurnsUpToForward)
{
    const Vector<uint8> bytes = IsolatedYUpFrontGltf();
    FbxLoaderOptions bakeX = DefaultFbxOptions();
    bakeX.Axis.BakeRotationDeg[0] = 90.0f;
    bakeX.Axis.BakeRotationAxisEnabled[0] = true;
    ModelAsset asset(GUID::Generate(), std::filesystem::path{"up_front.gltf"});
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, bytes, bakeX));
    ASSERT_GT(asset.GetMeshCount(), 0u);
    const auto& mesh = asset.GetMesh(0);
    ASSERT_EQ(mesh.Vertices.size(), 3u);
    // Default X-mirror then Rx(90): (1,0,0)->(-1,0,0); (0,1,0)->(0,0,1); (0,0,1)->(0,-1,0).
    EXPECT_NEAR(mesh.Vertices[0].Position[0], -1.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[0].Position[1], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[0].Position[2], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[1].Position[0], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[1].Position[1], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[1].Position[2], 1.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[2].Position[0], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[2].Position[1], -1.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[2].Position[2], 0.0f, 1e-5f);
}

TEST(FbxAxisConversion, IsolatedGltfMirrorXOffMatchesSource)
{
    const Vector<uint8> bytes = IsolatedYUpFrontGltf();
    FbxLoaderOptions noMirror = DefaultFbxOptions();
    noMirror.Axis.MirrorAxis[0] = false;
    ModelAsset asset(GUID::Generate(), std::filesystem::path{"up_front.gltf"});
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset, bytes, noMirror));
    ASSERT_GT(asset.GetMeshCount(), 0u);
    const auto& mesh = asset.GetMesh(0);
    ASSERT_EQ(mesh.Vertices.size(), 3u);
    EXPECT_NEAR(mesh.Vertices[0].Position[0], 1.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[1].Position[1], 1.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[2].Position[2], 1.0f, 1e-5f);
}

// Node translation keys (0,0,0) -> (1,0,1). Default Mirror X must negate X
// and keep +Z, same as LoadGLTF vertices.
Vector<uint8> IsolatedGltfPlusXTranslate()
{
    static const char kJson[] = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "Mover"}],
  "animations": [{
    "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}],
    "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}]
  }],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "max": [1], "min": [0]},
    {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 8},
    {"buffer": 0, "byteOffset": 8, "byteLength": 24}
  ],
  "buffers": [{"byteLength": 32, "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAgD8="}]
})";
    Vector<uint8> bytes(sizeof(kJson) - 1u);
    std::memcpy(bytes.data(), kJson, bytes.size());
    return bytes;
}

TEST(FbxAxisConversion, IsolatedGltfClipKeysMirrorX)
{
    const Vector<uint8> bytes = IsolatedGltfPlusXTranslate();
    AnimationClip clip(GUID::Generate(), std::filesystem::path{"plus_x.gltf"});
    ASSERT_TRUE(clip.LoadFromData(bytes));
    const AnimChannel* trans = nullptr;
    for (const AnimChannel& ch : clip.GetChannels())
    {
        if (ch.path == AnimPath::Translation && !ch.keys.empty())
        {
            trans = &ch;
            break;
        }
    }
    ASSERT_NE(trans, nullptr);
    const AnimKeyframe& last = trans->keys.back();
    EXPECT_LT(last.translation[0], 0.0f);
    EXPECT_NEAR(last.translation[1], 0.0f, 1e-5f);
    EXPECT_GT(last.translation[2], 0.0f);
}

TEST(FbxAxisConversion, SheepBakeXChangesAabb)
{
    const auto path = LicensedFbx("Sheep.fbx");
    if (path.empty())
        GTEST_SKIP() << "Sheep.fbx not in Assets/Models/FBXTest";
    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(path, bytes)) << path.string();

    ufbx_load_opts opts{};
    opts.ignore_embedded = true;
    ufbx_error err{};
    ufbx_scene* scene = ufbx_load_memory(bytes.data(), bytes.size(), &opts, &err);
    ASSERT_NE(scene, nullptr);
    std::printf("Sheep source axes right=%d up=%d front=%d unit_m=%g\n",
                static_cast<int>(scene->settings.axes.right),
                static_cast<int>(scene->settings.axes.up),
                static_cast<int>(scene->settings.axes.front),
                scene->settings.unit_meters);
    ufbx_free_scene(scene);

    auto loadBakeX = [&](ModelAsset& m, float deg, bool enabled) {
        FbxLoaderOptions o = DefaultFbxOptions();
        o.Axis.BakeRotationDeg[0] = deg;
        o.Axis.BakeRotationAxisEnabled[0] = enabled;
        ASSERT_TRUE(LoadFixture(bytes, o, m));
    };

    ModelAsset def(GUID::Generate(), path);
    ModelAsset x90(GUID::Generate(), path);
    ModelAsset xm90(GUID::Generate(), path);
    ModelAsset x180(GUID::Generate(), path);
    loadBakeX(def, 0.0f, false);
    loadBakeX(x90, 90.0f, true);
    loadBakeX(xm90, -90.0f, true);
    loadBakeX(x180, 180.0f, true);
    ASSERT_GT(def.GetMeshCount(), 0u);

    auto dump = [](const char* label, const ModelAsset& m) {
        float mn[3], mx[3];
        m.GetBoundingBox(mn, mx);
        std::printf("%s min=(%.4f,%.4f,%.4f) max=(%.4f,%.4f,%.4f) ext=(%.4f,%.4f,%.4f)\n",
                    label, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2],
                    mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
    };
    dump("sheep default", def);
    dump("sheep X90", x90);
    dump("sheep X-90", xm90);
    dump("sheep X180", x180);

    float defMin[3], defMax[3], x90Min[3], x90Max[3];
    def.GetBoundingBox(defMin, defMax);
    x90.GetBoundingBox(x90Min, x90Max);
    const float defY = defMax[1] - defMin[1];
    const float defZ = defMax[2] - defMin[2];
    const float x90Y = x90Max[1] - x90Min[1];
    const float x90Z = x90Max[2] - x90Min[2];
    EXPECT_GT(std::fabs(x90Y - defY) + std::fabs(x90Z - defZ), 1e-3f)
        << "bake X 90 did not change Sheep Y/Z extents";
    // Default import is already Y-up with feet at the origin — not on its head.
    // X 90 swaps Y/Z extents (lays the mesh over); X 180 is the invert.
    EXPECT_NEAR(defMin[1], 0.0f, 0.05f);
    EXPECT_GT(defMax[1], 1.0f);
}

TEST(FbxAxisConversion, ReloadBakeReusesSkeletonAndRebindsClips)
{
    const auto path = LicensedFbx("Sheep.fbx");
    if (path.empty())
        GTEST_SKIP() << "Sheep.fbx not in Assets/Models/FBXTest";
    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(path, bytes)) << path.string();

    ModelAsset asset(GUID::Generate(), path);
    ASSERT_TRUE(LoadFixture(bytes, DefaultFbxOptions(), asset));
    const GameEngine::uint32 skeletonId = asset.GetSkeletonId();
    ASSERT_NE(skeletonId, 0u);
    auto& skelStore = GameEngine::Engine::Renderer::SkeletonStore::Instance();
    const auto* rest0 = skelStore.Get(skeletonId);
    ASSERT_NE(rest0, nullptr);
    ASSERT_GE(rest0->RestTranslation.size(), 3u);
    size_t boneBase = 0;
    bool foundBone = false;
    for (size_t i = 0; i + 2 < rest0->RestTranslation.size(); i += 3)
    {
        if (std::fabs(rest0->RestTranslation[i + 1]) > 0.05f
            || std::fabs(rest0->RestTranslation[i + 2]) > 0.05f)
        {
            boneBase = i;
            foundBone = true;
            break;
        }
    }
    ASSERT_TRUE(foundBone) << "no rest translation with Y/Z extent";
    const float identityY = rest0->RestTranslation[boneBase + 1];
    const float identityZ = rest0->RestTranslation[boneBase + 2];

    const GameEngine::GUID clipGuid = ModelAsset::DeriveEmbeddedClipGuid(asset.GetGUID(), 0);
    auto& clipStore = GameEngine::Engine::Renderer::ClipStore::Instance();
    const GameEngine::uint32 clipIndex = clipStore.GetIndexIfPresent(clipGuid);

    FbxLoaderOptions bakeX = DefaultFbxOptions();
    bakeX.Axis.BakeRotationDeg[0] = 90.0f;
    bakeX.Axis.BakeRotationAxisEnabled[0] = true;
    ASSERT_TRUE(LoadFixture(bytes, bakeX, asset));
    EXPECT_EQ(asset.GetSkeletonId(), skeletonId);
    const auto* rest90 = skelStore.Get(skeletonId);
    ASSERT_NE(rest90, nullptr);
    ASSERT_GT(rest90->RestTranslation.size(), boneBase + 2);
    EXPECT_NEAR(rest90->RestTranslation[boneBase + 1], -identityZ, 1e-3f);
    EXPECT_NEAR(rest90->RestTranslation[boneBase + 2], identityY, 1e-3f);

    ASSERT_TRUE(LoadFixture(bytes, DefaultFbxOptions(), asset));
    EXPECT_EQ(asset.GetSkeletonId(), skeletonId);
    const auto* restBack = skelStore.Get(skeletonId);
    ASSERT_NE(restBack, nullptr);
    ASSERT_GT(restBack->RestTranslation.size(), boneBase + 2);
    EXPECT_NEAR(restBack->RestTranslation[boneBase + 1], identityY, 1e-3f);
    EXPECT_NEAR(restBack->RestTranslation[boneBase + 2], identityZ, 1e-3f);

    if (clipIndex != 0)
    {
        EXPECT_EQ(clipStore.GetIndexIfPresent(clipGuid), clipIndex);
        ASSERT_NE(clipStore.Get(clipIndex), nullptr);
    }
}

TEST(FbxAxisConversion, AdoptReloadBakeKeepsSkeletonIdentity)
{
    const auto path = LicensedFbx("Sheep.fbx");
    if (path.empty())
        GTEST_SKIP() << "Sheep.fbx not in Assets/Models/FBXTest";
    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(path, bytes)) << path.string();

    const GUID guid = GUID::Generate();
    ModelAsset live(guid, path);
    ASSERT_TRUE(LoadFixture(bytes, DefaultFbxOptions(), live));
    const GameEngine::uint32 skeletonId = live.GetSkeletonId();
    ASSERT_NE(skeletonId, 0u);
    auto& skelStore = GameEngine::Engine::Renderer::SkeletonStore::Instance();
    const auto* rest0 = skelStore.Get(skeletonId);
    ASSERT_NE(rest0, nullptr);
    ASSERT_GE(rest0->RestTranslation.size(), 3u);
    size_t boneBase = 0;
    bool foundBone = false;
    for (size_t i = 0; i + 2 < rest0->RestTranslation.size(); i += 3)
    {
        if (std::fabs(rest0->RestTranslation[i + 1]) > 0.05f
            || std::fabs(rest0->RestTranslation[i + 2]) > 0.05f)
        {
            boneBase = i;
            foundBone = true;
            break;
        }
    }
    ASSERT_TRUE(foundBone) << "no rest translation with Y/Z extent";
    const float identityY = rest0->RestTranslation[boneBase + 1];
    const float identityZ = rest0->RestTranslation[boneBase + 2];

    FbxLoaderOptions bakeX = DefaultFbxOptions();
    bakeX.Axis.BakeRotationDeg[0] = 90.0f;
    bakeX.Axis.BakeRotationAxisEnabled[0] = true;
    ModelAsset staged(guid, path);
    ASSERT_TRUE(LoadFixture(bytes, bakeX, staged));
    ASSERT_NE(staged.GetSkeletonId(), 0u);
    EXPECT_NE(staged.GetSkeletonId(), skeletonId);

    ASSERT_TRUE(live.AdoptReload(staged));
    EXPECT_EQ(live.GetSkeletonId(), skeletonId);
    const auto* rest90 = skelStore.Get(skeletonId);
    ASSERT_NE(rest90, nullptr);
    ASSERT_GT(rest90->RestTranslation.size(), boneBase + 2);
    EXPECT_NEAR(rest90->RestTranslation[boneBase + 1], -identityZ, 1e-3f);
    EXPECT_NEAR(rest90->RestTranslation[boneBase + 2], identityY, 1e-3f);
}

TEST(FbxAxisConversion, StagingDecodeDoesNotReplaceLiveClipsUntilAdopt)
{
    const auto path = LicensedFbx("Sheep.fbx");
    if (path.empty())
        GTEST_SKIP() << "Sheep.fbx not in Assets/Models/FBXTest";
    Vector<uint8> bytes;
    ASSERT_TRUE(ReadAllBytes(path, bytes)) << path.string();

    const GUID guid = GUID::Generate();
    ModelAsset live(guid, path);
    ASSERT_TRUE(LoadFixture(bytes, DefaultFbxOptions(), live));

    const GUID clipGuid = ModelAsset::DeriveEmbeddedClipGuid(guid, 0);
    auto& clipStore = GameEngine::Engine::Renderer::ClipStore::Instance();
    const GameEngine::uint32 clipIndex = clipStore.GetIndexIfPresent(clipGuid);
    ASSERT_NE(clipIndex, 0u) << "Sheep.fbx must register an embedded clip";
    const auto liveClip = clipStore.Get(clipIndex);
    ASSERT_NE(liveClip, nullptr);
    const AnimationClip* const livePtr = liveClip.get();

    FbxLoaderOptions bakeX = DefaultFbxOptions();
    bakeX.Axis.BakeRotationDeg[0] = 90.0f;
    bakeX.Axis.BakeRotationAxisEnabled[0] = true;

    {
        ModelAsset discarded(guid, path);
        ASSERT_TRUE(LoadFixture(bytes, bakeX, discarded));
        ASSERT_NE(discarded.GetSkeletonId(), 0u);
        EXPECT_NE(discarded.GetSkeletonId(), live.GetSkeletonId());
        EXPECT_EQ(clipStore.Get(clipIndex).get(), livePtr)
            << "superseded worker decode must not replace the live ClipStore slot";
    }
    EXPECT_EQ(clipStore.Get(clipIndex).get(), livePtr)
        << "dropping a staging payload without Adopt must leave live clips in place";

    ModelAsset staged(guid, path);
    ASSERT_TRUE(LoadFixture(bytes, bakeX, staged));
    EXPECT_EQ(clipStore.Get(clipIndex).get(), livePtr)
        << "in-flight staging decode must not publish clip replacements";

    ASSERT_TRUE(live.AdoptReload(staged));
    EXPECT_EQ(clipStore.GetIndexIfPresent(clipGuid), clipIndex);
    const auto adopted = clipStore.Get(clipIndex);
    ASSERT_NE(adopted, nullptr);
    EXPECT_NE(adopted.get(), livePtr)
        << "AdoptReloadedPayload publishes the staged clip replacements";
}
