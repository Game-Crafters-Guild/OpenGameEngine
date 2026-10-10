#include "Inspectors/SubmeshLodProvenance.h"

#include "Assets/MeshLODGeometry.h"
#include "Assets/ModelAsset.h"

namespace GameEngine::Editor
{

SubmeshLodProvenance ClassifySubmeshLod(const Mesh& mesh)
{
    if (mesh.LODCount() <= 1u)
        return SubmeshLodProvenance::Lod0Only;
    if (!mesh.HasAuthoredLODs())
        return SubmeshLodProvenance::Generated;
    if (ResolveMeshLODGeometry(mesh).LevelCount <= 1u)
        return SubmeshLodProvenance::AuthoredDropped;
    return SubmeshLodProvenance::Authored;
}

} // namespace GameEngine::Editor
