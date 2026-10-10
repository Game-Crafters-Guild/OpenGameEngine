#include <gtest/gtest.h>

#include "Rendering/Common/Math.h"
#include "Rendering/Common/MatrixUtils.h"

using namespace GameEngine::Rendering;
using namespace GameEngine::Mathematics;

namespace {

class MatrixConversionValidationTest : public ::testing::Test {
protected:
	Matrix4x4 identityMatrix;
	Matrix4x4 translationMatrix;
	Matrix4x4 rotationMatrix;
	Matrix4x4 scaleMatrix;
	Matrix4x4 viewMatrix;
	Matrix4x4 projMatrix;

	void SetUp() override
	{
		identityMatrix = Matrix4x4::Identity();
		translationMatrix = MakeTranslation(Vector3(1.0f, 2.0f, 3.0f));
		rotationMatrix = MakeRotationY(GameEngine::Rendering::Math::ToRadians(45.0f));
		scaleMatrix = MakeScale(Vector3(2.0f, 3.0f, 4.0f));

		viewMatrix = MakeLookAtLH(
			Vector3(0.0f, 0.0f, 10.0f),
			Vector3(0.0f, 0.0f, 0.0f),
			Vector3(0.0f, 1.0f, 0.0f));

		projMatrix = MakePerspectiveLH_ZO_ReverseZ(
			GameEngine::Rendering::Math::ToRadians(60.0f),
			16.0f / 9.0f,
			0.1f,
			100.0f);
	}

	static bool MatrixEquals(const Matrix4x4& a, const Matrix4x4& b, float epsilon)
	{
		const float* ad = a.Data();
		const float* bd = b.Data();
		for (int i = 0; i < 16; ++i)
		{
			if (std::abs(ad[i] - bd[i]) > epsilon)
			{
				return false;
			}
		}
		return true;
	}
};

} // namespace

TEST_F(MatrixConversionValidationTest, ConvertMatrixForShader_VulkanPreservesMatrix)
{
	Matrix4x4 result = ConvertMatrixForShader(translationMatrix, GraphicsAPI::Vulkan);
	EXPECT_TRUE(MatrixEquals(result, translationMatrix, 1e-5f));
}

TEST_F(MatrixConversionValidationTest, ConvertMatrixForShader_DirectX12TransposesMatrix)
{
	Matrix4x4 result = ConvertMatrixForShader(translationMatrix, GraphicsAPI::DirectX12);
	Matrix4x4 expected = Transpose(translationMatrix);
	EXPECT_TRUE(MatrixEquals(result, expected, 1e-5f));
}

TEST_F(MatrixConversionValidationTest, MatrixUtilsRequiresTranspositionMatchesAPI)
{
	EXPECT_FALSE(MatrixUtils::RequiresTransposition(GraphicsAPI::Vulkan));
	EXPECT_TRUE(MatrixUtils::RequiresTransposition(GraphicsAPI::DirectX12));
}

TEST_F(MatrixConversionValidationTest, MatrixUtilsToShaderDelegatesToConvertMatrixForShader)
{
	Matrix4x4 vulkanViaUtils = MatrixUtils::ToShader(translationMatrix, GraphicsAPI::Vulkan);
	Matrix4x4 directxViaUtils = MatrixUtils::ToShader(translationMatrix, GraphicsAPI::DirectX12);

	Matrix4x4 vulkanExpected = ConvertMatrixForShader(translationMatrix, GraphicsAPI::Vulkan);
	Matrix4x4 directxExpected = ConvertMatrixForShader(translationMatrix, GraphicsAPI::DirectX12);

	EXPECT_TRUE(MatrixEquals(vulkanViaUtils, vulkanExpected, 1e-5f));
	EXPECT_TRUE(MatrixEquals(directxViaUtils, directxExpected, 1e-5f));
}

TEST_F(MatrixConversionValidationTest, CullingRenderingConsistency)
{
	// Simulate camera parameters
	Vector3 cameraPos(0.0f, 30.0f, 80.0f);
	Vector3 cameraTarget(0.0f, 0.0f, 0.0f);
	float aspectRatio = 1920.0f / 1080.0f;
	float fov = GameEngine::Rendering::Math::ToRadians(60.0f);

	const float nearPlane = 1.0f;
	const float farPlane = 500.0f;

	// Culling system view/proj
	Matrix4x4 cullingViewMatrix = MakeLookAtLH(cameraPos, cameraTarget, Vector3(0.0f, 1.0f, 0.0f));
	Matrix4x4 cullingProjMatrix = MakePerspectiveLH_ZO_ReverseZ(fov, aspectRatio, nearPlane, farPlane);
	Matrix4x4 cullingViewProjMatrix = cullingProjMatrix * cullingViewMatrix;

	// Rendering system view/proj (should match culling)
	Matrix4x4 renderingViewMatrix = MakeLookAtLH(cameraPos, cameraTarget, Vector3(0.0f, 1.0f, 0.0f));
	Matrix4x4 renderingProjMatrix = MakePerspectiveLH_ZO_ReverseZ(fov, aspectRatio, nearPlane, farPlane);
	Matrix4x4 renderingViewProjMatrix = renderingProjMatrix * renderingViewMatrix;

	EXPECT_TRUE(MatrixEquals(cullingViewMatrix, renderingViewMatrix, 0.0001f));
	EXPECT_TRUE(MatrixEquals(cullingProjMatrix, renderingProjMatrix, 0.0001f));
	EXPECT_TRUE(MatrixEquals(cullingViewProjMatrix, renderingViewProjMatrix, 0.0001f));

	// Validate clip-space depth range [0,1] for near/far objects (Vulkan/GLM ZO)
		Vector3 nearObject = cameraPos + (cameraTarget - cameraPos).Normalize() * (nearPlane + 0.1f);
		Vector4 nearObjectClip = cullingViewProjMatrix.Transform(Vector4(nearObject.x, nearObject.y, nearObject.z, 1.0f));
	if (nearObjectClip.w != 0.0f)
	{
		nearObjectClip.x /= nearObjectClip.w;
		nearObjectClip.y /= nearObjectClip.w;
		nearObjectClip.z /= nearObjectClip.w;
		nearObjectClip.w = 1.0f;
	}

	EXPECT_GE(nearObjectClip.z, 0.0f);
	EXPECT_LE(nearObjectClip.z, 1.0f);

		Vector3 farObject = cameraPos + (cameraTarget - cameraPos).Normalize() * (farPlane - 0.1f);
		Vector4 farObjectClip = cullingViewProjMatrix.Transform(Vector4(farObject.x, farObject.y, farObject.z, 1.0f));
	if (farObjectClip.w != 0.0f)
	{
		farObjectClip.x /= farObjectClip.w;
		farObjectClip.y /= farObjectClip.w;
		farObjectClip.z /= farObjectClip.w;
		farObjectClip.w = 1.0f;
	}

	EXPECT_GE(farObjectClip.z, 0.0f);
	EXPECT_LE(farObjectClip.z, 1.0f);

	// Reverse-Z: near object's NDC depth must be greater than far object's. This
	// catches a forward-Z projection slipping past the [0,1] range check.
	EXPECT_GT(nearObjectClip.z, farObjectClip.z);
	EXPECT_GT(nearObjectClip.z, 0.5f);
	EXPECT_LT(farObjectClip.z, 0.5f);
}

// ---- LargestAxisScale: the factor a cull sphere has to be built on ---------
//
// A frustum cull sphere is the mesh's local bounding radius times "how much can
// this transform stretch a direction". For a perpendicular basis that is the
// longest column; for a SHEARED one it is not, because the diagonal between two
// columns grows past either of them. Getting it wrong publishes a sphere that
// does not contain its own mesh, and the object is culled with part of itself
// still on screen — a pop at the frustum edge that no still frame can show.
//
// Two producers of sheared instance transforms ship today, and both are covered
// below: the spline seam shear (Placement/SeamShear.h, which skews a tile in
// the ground plane to close a wedge at a bend) and SplineSpanGrade::Sheared
// (which slants a fence span onto a grade while its up axis stays with its stations).
//
// These tests assert the geometric PROPERTY — every corner of the transformed
// box lies inside the sphere — rather than a formula, so they cannot pass by
// agreeing with the implementation's own arithmetic.

namespace {

// Column-major upper 3x3 into a 4x4, the layout MatrixUtils reads.
void MakeBasisMatrix(float m[16], const Vector3& c0, const Vector3& c1, const Vector3& c2)
{
	m[0] = c0.x;  m[1] = c0.y;  m[2] = c0.z;  m[3] = 0.0f;
	m[4] = c1.x;  m[5] = c1.y;  m[6] = c1.z;  m[7] = 0.0f;
	m[8] = c2.x;  m[9] = c2.y;  m[10] = c2.z; m[11] = 0.0f;
	m[12] = 0.0f; m[13] = 0.0f; m[14] = 0.0f; m[15] = 1.0f;
}

float MaxColumnNorm(const float m[16])
{
	const float a = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
	const float b = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
	const float c = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
	return std::max({a, b, c});
}

// The largest singular value by POWER ITERATION on M^T M — deliberately a
// different algorithm from the closed form under test, so agreement between
// them is evidence rather than a restatement.
float LargestSingularValueByIteration(const float m[16])
{
	const float s[3][3] = {
		{m[0] * m[0] + m[1] * m[1] + m[2] * m[2],
		 m[0] * m[4] + m[1] * m[5] + m[2] * m[6],
		 m[0] * m[8] + m[1] * m[9] + m[2] * m[10]},
		{m[0] * m[4] + m[1] * m[5] + m[2] * m[6],
		 m[4] * m[4] + m[5] * m[5] + m[6] * m[6],
		 m[4] * m[8] + m[5] * m[9] + m[6] * m[10]},
		{m[0] * m[8] + m[1] * m[9] + m[2] * m[10],
		 m[4] * m[8] + m[5] * m[9] + m[6] * m[10],
		 m[8] * m[8] + m[9] * m[9] + m[10] * m[10]}};
	double v[3] = {0.5773502692, 0.5773502692, 0.5773502692};
	double eigenvalue = 0.0;
	for (int iter = 0; iter < 2000; ++iter)
	{
		double w[3] = {0.0, 0.0, 0.0};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				w[i] += static_cast<double>(s[i][j]) * v[j];
		const double len = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
		if (len <= 0.0)
			return 0.0;
		for (int i = 0; i < 3; ++i)
			v[i] = w[i] / len;
		eigenvalue = len;
	}
	return static_cast<float>(std::sqrt(eigenvalue));
}

// Does the published sphere actually contain the transformed box? This is the
// question the radius exists to answer, and it needs no formula: walk the eight
// corners of the local box through the matrix and measure.
float WorstCornerOvershoot(const float m[16], const Vector3& halfExtents, float radiusScale)
{
	const float radius = halfExtents.Length() * radiusScale;
	float worst = 0.0f;
	for (int corner = 0; corner < 8; ++corner)
	{
		const float lx = (corner & 1) ? halfExtents.x : -halfExtents.x;
		const float ly = (corner & 2) ? halfExtents.y : -halfExtents.y;
		const float lz = (corner & 4) ? halfExtents.z : -halfExtents.z;
		const Vector3 world(m[0] * lx + m[4] * ly + m[8] * lz,
		                    m[1] * lx + m[5] * ly + m[9] * lz,
		                    m[2] * lx + m[6] * ly + m[10] * lz);
		worst = std::max(worst, world.Length() - radius);
	}
	return worst;
}

// A fence span sheared onto a grade: its up stays world up, its across-run axis
// stays horizontal, and only its along-run axis leaves that plane, by the
// grade. This is exactly what FenceLayout emits for SplineSpanGrade::Sheared.
void MakeShearedSpanBasis(float m[16], float gradeDegrees, float lengthScale)
{
	const float k = std::tan(gradeDegrees * 3.14159265358979f / 180.0f);
	MakeBasisMatrix(m,
	                Vector3(lengthScale, lengthScale * k, 0.0f), // along the run
	                Vector3(0.0f, 1.0f, 0.0f),                   // up, and it stays up
	                Vector3(0.0f, 0.0f, 1.0f));                  // across travel
}

// The measured FantasyKingdom fence panel: 2.4707 x 1.6387 x 0.1567 m.
const Vector3 kFencePanelHalfExtents(1.2353f, 0.8194f, 0.0784f);

} // namespace

// The fast path must be BYTE-identical to the max-column-norm it replaces, or
// this fix is a silent re-bounding of every object in every scene.
TEST(LargestAxisScale, PerpendicularBasesAreUnchangedToTheBit)
{
	float m[16];
	// Identity.
	MakeBasisMatrix(m, Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1));
	EXPECT_EQ(MatrixUtils::LargestAxisScale(m), MaxColumnNorm(m));

	// Non-uniform scale — the case a column-norm max exists to handle.
	MakeBasisMatrix(m, Vector3(2, 0, 0), Vector3(0, 3, 0), Vector3(0, 0, 4));
	EXPECT_EQ(MatrixUtils::LargestAxisScale(m), MaxColumnNorm(m));
	EXPECT_FLOAT_EQ(MatrixUtils::LargestAxisScale(m), 4.0f);

	// Rotated and scaled: still perpendicular, so still the longest column.
	const float c = std::cos(0.7f), s = std::sin(0.7f);
	MakeBasisMatrix(m, Vector3(c * 2.0f, s * 2.0f, 0), Vector3(-s * 5.0f, c * 5.0f, 0),
	                Vector3(0, 0, 0.25f));
	EXPECT_EQ(MatrixUtils::LargestAxisScale(m), MaxColumnNorm(m));

	// A mirror is perpendicular too, and negative scale must not confuse it.
	MakeBasisMatrix(m, Vector3(-3, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1));
	EXPECT_EQ(MatrixUtils::LargestAxisScale(m), MaxColumnNorm(m));

	// A tiny, physically meaningless shear stays on the cheap path by design.
	MakeBasisMatrix(m, Vector3(1.0f, 1.0e-5f, 0.0f), Vector3(0, 1, 0), Vector3(0, 0, 1));
	EXPECT_EQ(MatrixUtils::LargestAxisScale(m), MaxColumnNorm(m));
}

// The defect itself, on a basis chosen so the two answers cannot be confused:
// unit columns 45 degrees apart stretch their own diagonal by 1.307, while
// every column still measures exactly 1.
TEST(LargestAxisScale, ShearedBasisExceedsItsLongestColumn)
{
	float m[16];
	const float r = std::sqrt(0.5f);
	MakeBasisMatrix(m, Vector3(1, 0, 0), Vector3(r, r, 0), Vector3(0, 0, 1));

	EXPECT_FLOAT_EQ(MaxColumnNorm(m), 1.0f) << "every column of this basis is a unit vector";
	const float sigma = MatrixUtils::LargestAxisScale(m);
	EXPECT_NEAR(sigma, LargestSingularValueByIteration(m), 1.0e-4f);
	EXPECT_GT(sigma, MaxColumnNorm(m) * 1.25f)
	    << "the old formula reports 1.0 here and the true stretch is 1.307";
}

// Every shipped fence rail angle, against the independent solver and against
// the property that actually matters.
TEST(LargestAxisScale, ShearedFenceSpansStayInsideTheirCullSphere)
{
	// 5.575 and 18.179 degrees are measured off the village's built spans;
	// 29.74 is its steepest along-run grade. The rest bracket them.
	for (const float grade : {5.575f, 12.426f, 18.179f, 29.74f, 45.0f})
	{
		float m[16];
		MakeShearedSpanBasis(m, grade, 1.0f);

		const float sigma = MatrixUtils::LargestAxisScale(m);
		EXPECT_NEAR(sigma, LargestSingularValueByIteration(m), 1.0e-4f) << grade << " deg";

		// The fixture must reach the case: the old formula has to be WRONG here,
		// or a green result below would mean nothing.
		EXPECT_GT(sigma, MaxColumnNorm(m) * 1.0001f)
		    << grade << " deg: max column norm must fall short, or this proves nothing";

		// The property. Old formula: some corner sticks out. New: none does.
		const float oldOvershoot =
		    WorstCornerOvershoot(m, kFencePanelHalfExtents, MaxColumnNorm(m));
		EXPECT_GT(oldOvershoot, 0.0f)
		    << grade << " deg: the panel escapes a max-column-norm sphere by " << oldOvershoot
		    << " m";
		EXPECT_LE(WorstCornerOvershoot(m, kFencePanelHalfExtents, sigma), 1.0e-5f)
		    << grade << " deg: the panel must not escape the sphere published for it";
	}
}

// The escaped FIRST instance of this defect: spline placement's seam shear has
// been emitting non-perpendicular tile bases since before fence spans existed,
// and the same undersized sphere applies to every tile at a bend.
TEST(LargestAxisScale, SeamShearedTilesStayInsideTheirCullSphere)
{
	// SeamShear.h: the factor s maps tile-local (x, z) to (x, z + s * x) and is
	// applied in the pose basis as Right' = Right + s * Forward. s = tan(yaw/2)
	// clamped to tan(cap), and the recipe default cap is 8 degrees.
	for (const float capDegrees : {2.0f, 4.0f, 8.0f})
	{
		const float s = std::tan(capDegrees * 3.14159265358979f / 180.0f);
		float m[16];
		// Right sheared toward Forward; up untouched. A path tile: 2 x 0.1 x 2 m.
		MakeBasisMatrix(m, Vector3(1.0f, 0.0f, s), Vector3(0, 1, 0), Vector3(0, 0, 1));
		const Vector3 tileHalfExtents(1.0f, 0.05f, 1.0f);

		const float sigma = MatrixUtils::LargestAxisScale(m);
		EXPECT_NEAR(sigma, LargestSingularValueByIteration(m), 1.0e-4f) << capDegrees << " deg cap";
		EXPECT_GT(sigma, MaxColumnNorm(m) * 1.0001f)
		    << capDegrees << " deg cap: fixture must reach the case";
		EXPECT_GT(WorstCornerOvershoot(m, tileHalfExtents, MaxColumnNorm(m)), 0.0f)
		    << capDegrees << " deg cap: a seam-sheared tile escapes its old sphere";
		EXPECT_LE(WorstCornerOvershoot(m, tileHalfExtents, sigma), 1.0e-5f)
		    << capDegrees << " deg cap";
	}
}

// A sphere may only ever grow. A build that shrank one would cull geometry that
// the previous build drew, which is the same pop with the sign flipped.
TEST(LargestAxisScale, NeverReportsLessThanTheLongestColumn)
{
	float m[16];
	for (const float grade : {0.0f, 1.0f, 5.0f, 20.0f, 40.0f, 60.0f, 80.0f})
	{
		for (const float scale : {0.05f, 1.0f, 37.0f})
		{
			MakeShearedSpanBasis(m, grade, scale);
			EXPECT_GE(MatrixUtils::LargestAxisScale(m), MaxColumnNorm(m))
			    << grade << " deg at " << scale << "x";
		}
	}
	// Degenerate inputs must not return a NaN into a cull radius.
	MakeBasisMatrix(m, Vector3(0, 0, 0), Vector3(0, 0, 0), Vector3(0, 0, 0));
	EXPECT_TRUE(std::isfinite(MatrixUtils::LargestAxisScale(m)));
	EXPECT_FLOAT_EQ(MatrixUtils::LargestAxisScale(m), 0.0f);
}
