// FBX mesh instancing as the importer reads it: one geometry under several models imports as one
// Mesh placed by the first model, with the others as its ExtraPlacements.

#include <gtest/gtest.h>

#include "Assets/FbxLoaderOptions.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "ModelAssetFbxTestAccess.h"

#include <cstring>
#include <string_view>

namespace
{

// One triangle geometry under two models in meters, Y up, Z front: "First" moved 1 along +X,
// "Second" 2 along +Y, turned 180 degrees about Y and drawing the geometry 0.5 along its own +X
// (a geometric translation, which the first model does not carry).
constexpr std::string_view kTwoModelsShareOneGeometry = R"(; FBX 7.5.0 project file
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
		P: "UnitScaleFactor", "double", "Number", "",100
		P: "OriginalUnitScaleFactor", "double", "Number", "",100
	}
}
Objects:  {
	Geometry: 3000, "Geometry::Triangle", "Mesh" {
		Vertices: *9 {
			a: 1,0,0,0,1,0,0,0,1
		}
		PolygonVertexIndex: *3 {
			a: 0,1,-3
		}
		GeometryVersion: 124
	}
	Model: 2000, "Model::First", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",1,0,0
		}
	}
	Model: 2001, "Model::Second", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,2,0
			P: "Lcl Rotation", "Lcl Rotation", "", "A",0,180,0
			P: "GeometricTranslation", "Vector3D", "Vector", "",0.5,0,0
		}
	}
}
Connections:  {
	C: "OO",2000,0
	C: "OO",2001,0
	C: "OO",3000,2000
	C: "OO",3000,2001
}
)";

} // namespace

TEST(FbxMeshInstance, EveryModelThatDrawsAGeometryPlacesIt)
{
    GameEngine::Vector<GameEngine::uint8> bytes(kTwoModelsShareOneGeometry.size());
    std::memcpy(bytes.data(), kTwoModelsShareOneGeometry.data(), bytes.size());
    GameEngine::ModelAsset asset(GameEngine::GUID::Generate(), "shared.fbx");
    ASSERT_TRUE(GameEngine::ModelAssetFbxTestAccess::Load(asset, bytes, GameEngine::FbxLoaderOptions{}));
    ASSERT_EQ(asset.GetMeshCount(), 1u);
    const GameEngine::Mesh& mesh = asset.GetMesh(0);

    // The default import mirrors X: "First" sits at -1 on X, the turn about Y stays a turn about Y.
    EXPECT_NEAR(mesh.SourceNodeTransform[12], -1.0f, 1e-5f);
    ASSERT_EQ(mesh.ExtraPlacements.size(), 1u);
    const GameEngine::MeshPlacement& second = mesh.ExtraPlacements[0];
    EXPECT_NE(second.SourceNodeIndex, mesh.SourceNodeIndex);
    EXPECT_NEAR(second.SourceNodeTransform[0], -1.0f, 1e-5f);
    EXPECT_NEAR(second.SourceNodeTransform[10], -1.0f, 1e-5f);
    // The vertices carry the first model's geometry offset (none), so the second draws them
    // through its own: 0.5 along its +X, which its turn points along -X, mirrored to +X.
    EXPECT_NEAR(second.SourceNodeTransform[12], 0.5f, 1e-5f);
    EXPECT_NEAR(second.SourceNodeTransform[13], 2.0f, 1e-5f);
    EXPECT_NEAR(second.SourceNodeTransform[14], 0.0f, 1e-5f);
    // Below its own model the placement is that geometry offset alone, mirrored.
    EXPECT_NEAR(second.SourceNodeLocalTransform[0], 1.0f, 1e-5f);
    EXPECT_NEAR(second.SourceNodeLocalTransform[12], -0.5f, 1e-5f);
    EXPECT_NEAR(second.SourceNodeLocalTransform[13], 0.0f, 1e-5f);
    EXPECT_NEAR(second.SourceNodeLocalTransform[14], 0.0f, 1e-5f);
}
