#pragma once

// Threaded (stackless) BVH node format for the software ray-traversal lane.
//
// Ported from speedball-gi (MIT). Upstream repositories:
//   https://github.com/cl0nazepamm/speedball        (c) 2026 m3org
//   norio/speedball-gi                              (c) 2026 m3org, norio
//
// MIT License
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <bit>
#include <vector>

#include "Mathematics/Geometry.h"
#include "Types/Types.h"

namespace GameEngine::SceneBvh
{

// ── GPU node format ────────────────────────────────────────────────────────
//
// A ThreadedBvh is a bottom-level acceleration structure (one per source mesh,
// built in the mesh's LOCAL space) laid out so a compute shader can traverse it
// with a single cursor and no stack. The whole structure is four flat arrays,
// each of which maps 1:1 onto an SSBO:
//
//   Nodes             uint[]   kThreadedBvhNodeStrideU32 (8) words per node
//   TriangleIndices   uint[]   kTriangleIndexStride (3) words per triangle
//   TriangleMaterials uint[]   1 word per triangle
//   VertexData        float[]  kVertexDataStrideFloats (8) floats per vertex
//
// NODE (8 x u32, word offsets relative to nodeIndex * 8):
//
//   +0..+2  f32  AABB min x, y, z    <- bit pattern of a float stored in a uint
//   +3..+5  f32  AABB max x, y, z    <- ditto; shader reads uintBitsToFloat()
//   +6      u32  miss link
//   +7      u32  leaf word
//
// Bounds ride in the uint array as raw float bit patterns. A shader recovers
// them with uintBitsToFloat(); this is safe in the *uint* buffer. The TLAS
// (see TlasPacker.h) lives in a *float* buffer instead and therefore stores its
// links as plain floats — never bit-cast a uint into a float buffer: denormal
// links flush to zero on some drivers and 0xFFFFFFFF markers are NaNs whose
// payload bits may be canonicalized away.
//
// LEAF WORD (+7):
//
//   0xFFFFFFFF                                  -> interior node
//   (triangleCount << 24) | triangleOffset      -> leaf
//
// so the count occupies the top 8 bits (max 255 triangles per leaf) and the
// offset the low 24 bits (max 2^24 triangles addressable). Both ceilings are
// enforced at build time.
//
// TRAVERSAL CONTRACT — this is what makes the format stackless:
//
//   * Nodes are stored in PRE-ORDER. An interior node's LEFT child is always
//     the immediately following node, nodeIndex + 1. It is never stored.
//   * Every node's MISS LINK is the index of the first node AFTER its entire
//     subtree — the node to jump to when the ray misses this node's AABB, i.e.
//     the classic escape/skip pointer.
//   * Because the left child's subtree is immediately followed by the right
//     child, the LEFT CHILD'S MISS LINK *IS* THE RIGHT CHILD'S INDEX. The
//     refit relies on this to find the right child (ThreadedBvhRefit.h); a
//     traversal never needs it.
//
// A shader walk is therefore:
//
//   cursor = 0
//   while (cursor < nodeCount)
//       if (ray hits node[cursor].bounds closer than the current best hit)
//           if (interior) cursor += 1                 // descend into the left child
//           else          { test triangles; cursor = miss; }
//       else
//           cursor = miss                             // skip the whole subtree
//
// TRIANGLE INDIRECTION: a leaf covers TriangleIndices[3*t .. 3*t+2] for every
// t in [triangleOffset, triangleOffset + triangleCount). Triangles are stored
// in BVH order — the builder permutes them so every leaf's range is contiguous.
// TriangleMaterials[t] is that triangle's material index in the same order.
// The three words of a triangle are VERTEX indices into VertexData.
//
// VERTEX RECORD (8 x f32, offsets relative to vertexIndex * 8):
//
//   +0..+2  position xyz  (mesh-local space)
//   +3..+5  normal xyz    (mesh-local; all zero when the source had no normals)
//   +6..+7  texcoord uv
//
// Vertices are NOT permuted by the build, so a deform can rewrite positions in
// place and hand the tree straight to ThreadedBvhRefit.
//
// POOLING: node indices, miss links and leaf triangle offsets emitted here are
// BLAS-LOCAL (the root is node 0). Concatenating several meshes into one pair
// of shared SSBOs means adding the mesh's node base to every miss link and its
// triangle base to every leaf triangle offset, and its vertex base to every
// entry of TriangleIndices. Traversal then starts at the instance's node base
// and stops at its node end rather than at 0 / nodeCount.

inline constexpr uint32 kThreadedBvhNodeStrideU32 = 8u;
inline constexpr uint32 kVertexDataStrideFloats = 8u;
inline constexpr uint32 kTriangleIndexStride = 3u;

// Leaf word sentinel: this node has children rather than triangles.
inline constexpr uint32 kThreadedBvhInteriorLeafWord = 0xFFFFFFFFu;

inline constexpr uint32 kThreadedBvhTriangleCountShift = 24u;
inline constexpr uint32 kThreadedBvhTriangleOffsetMask = 0x00FFFFFFu;

// Encoding ceilings implied by the leaf word's bit split.
inline constexpr uint32 kThreadedBvhMaxLeafTriangles = 255u;
inline constexpr uint32 kThreadedBvhMaxTriangles = 1u << 24;
// Miss links and instance blas ranges are shipped to the GPU as f32 in the
// TLAS/instance records, which represent integers exactly only up to 2^24.
inline constexpr uint32 kThreadedBvhMaxNodes = 1u << 24;

// One mesh's bottom-level acceleration structure plus the geometry a hit needs.
// Public data: this is a GPU wire format, not an encapsulated object.
struct ThreadedBvh
{
    std::vector<uint32> Nodes;
    std::vector<uint32> TriangleIndices;
    std::vector<uint32> TriangleMaterials;
    std::vector<float32> VertexData;
    Mathematics::AABB LocalBounds{};

    uint32 NodeCount() const
    {
        return static_cast<uint32>(Nodes.size() / kThreadedBvhNodeStrideU32);
    }

    uint32 TriangleCount() const
    {
        return static_cast<uint32>(TriangleIndices.size() / kTriangleIndexStride);
    }

    uint32 VertexCount() const
    {
        return static_cast<uint32>(VertexData.size() / kVertexDataStrideFloats);
    }

    bool IsEmpty() const { return Nodes.empty(); }
};

// ── Node field codecs ──────────────────────────────────────────────────────
// The one place the bit layout above is expressed in code. Builder, refit,
// tests and any future shader author all go through these.

inline bool IsInteriorLeafWord(uint32 leafWord)
{
    return leafWord == kThreadedBvhInteriorLeafWord;
}

inline uint32 EncodeLeafWord(uint32 triangleOffset, uint32 triangleCount)
{
    return (triangleCount << kThreadedBvhTriangleCountShift) |
           (triangleOffset & kThreadedBvhTriangleOffsetMask);
}

inline uint32 DecodeLeafTriangleOffset(uint32 leafWord)
{
    return leafWord & kThreadedBvhTriangleOffsetMask;
}

inline uint32 DecodeLeafTriangleCount(uint32 leafWord)
{
    return leafWord >> kThreadedBvhTriangleCountShift;
}

inline uint32 DecodeNodeMissLink(const uint32* nodes, uint32 nodeIndex)
{
    return nodes[nodeIndex * kThreadedBvhNodeStrideU32 + 6u];
}

inline uint32 DecodeNodeLeafWord(const uint32* nodes, uint32 nodeIndex)
{
    return nodes[nodeIndex * kThreadedBvhNodeStrideU32 + 7u];
}

inline Mathematics::AABB DecodeNodeBounds(const uint32* nodes, uint32 nodeIndex)
{
    const uint32* base = nodes + nodeIndex * kThreadedBvhNodeStrideU32;
    return {{std::bit_cast<float32>(base[0]),
             std::bit_cast<float32>(base[1]),
             std::bit_cast<float32>(base[2])},
            {std::bit_cast<float32>(base[3]),
             std::bit_cast<float32>(base[4]),
             std::bit_cast<float32>(base[5])}};
}

inline void EncodeNodeBounds(uint32* nodes, uint32 nodeIndex, const Mathematics::AABB& bounds)
{
    uint32* base = nodes + nodeIndex * kThreadedBvhNodeStrideU32;
    base[0] = std::bit_cast<uint32>(bounds.min.x);
    base[1] = std::bit_cast<uint32>(bounds.min.y);
    base[2] = std::bit_cast<uint32>(bounds.min.z);
    base[3] = std::bit_cast<uint32>(bounds.max.x);
    base[4] = std::bit_cast<uint32>(bounds.max.y);
    base[5] = std::bit_cast<uint32>(bounds.max.z);
}

} // namespace GameEngine::SceneBvh
