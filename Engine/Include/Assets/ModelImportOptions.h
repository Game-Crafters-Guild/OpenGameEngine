#pragma once

namespace GameEngine::ModelImport
{

// Bake Euler + engine-axis mirrors applied after a format's source conversion.
// Shared by FBX, glTF/GLB, and Blend. kv keys stay assets.fbx.* and are
// resolved through FbxLoaderOptions::Axis.
struct AxisOptions
{
    float BakeRotationDeg[3] = {0.0f, 0.0f, 0.0f};
    bool BakeRotationAxisEnabled[3] = {false, false, false};
    // Default X on: FBX is right-handed, this engine is left-handed, so the bake mirrors X.
    bool MirrorAxis[3] = {true, false, false};

    void EffectiveBakeRotationDeg(float out[3]) const
    {
        for (int i = 0; i < 3; ++i)
            out[i] = BakeRotationAxisEnabled[i] ? BakeRotationDeg[i] : 0.0f;
    }

    bool AnyBakeRotationEnabled() const
    {
        return BakeRotationAxisEnabled[0] || BakeRotationAxisEnabled[1] || BakeRotationAxisEnabled[2];
    }
};

} // namespace GameEngine::ModelImport
