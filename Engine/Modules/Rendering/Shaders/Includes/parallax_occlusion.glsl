// Parallax occlusion mapping: the relief march for height-mapped surfaces.
//
// One technique, whose sample count follows the size of the relief on screen: a linear search in
// n steps, contact refinement inside the bracketing step, then the interpolated hit between the
// last sample above the ray and the first below it. n = min(budget, ceil(E)), where E is the
// on-screen length (pixels) of the shift the full relief depth produces; below half a pixel the
// view march is skipped and the surface is shaded where it lies. The self-shadow is gated on its own
// on-screen length, so it can still run where the view march is skipped (a wall seen head on).
//
// Height convention: the height map stores 1 at the polygon surface and 0 at the deepest point.
// Relief is carved BELOW the polygon, so geometry, collision and everything placed on the surface
// stay on the polygon, and nothing renders above it.
//
// Units, and where each conversion happens:
//   uv0 space       the mesh's first UV set (the adapter's vUV0), in texture repeats
//   height space    uv0 through the height slot's affine tiling transform, in repeats
//   surface metres  world metres in the tangent plane of the interpolated normal
//   pixels          render-target pixels, the unit of the screen-space derivatives
// reliefDepth, the material's one parameter, is a fraction of one height-space repeat; it becomes
// metres through the metres per height repeat the derivative frame measures.
//
// The includer defines, BEFORE including this file, the one texture access the march makes:
//   float GE_ParallaxSampleHeight(vec2 uvHeight, float lod)   // height in [0, 1], lod in mip levels
//
// The block between the GE_SHARED_PARALLAX_OCCLUSION markers is lifted verbatim into the host tests
// (ExtractShaderBlock.cmake -> Tests/ParallaxOcclusionTests.cpp) and compiled as C++ through
// GlslShim.h, so it is written component-wise, with no swizzles and with f-suffixed literals.
#ifndef GE_PARALLAX_OCCLUSION_GLSL
#define GE_PARALLAX_OCCLUSION_GLSL

// GE_SHARED_PARALLAX_OCCLUSION_BEGIN
// Engine constants, deliberately not material settings: the pixel rule scales the work with
// resolution, distance and angle. The compat profile (WebGPU-class devices) takes a fixed low arm:
// fewer linear steps, no refinement, no self-shadow.
#if defined(GE_COMPAT_PROFILE)
const int kParallaxLinearStepBudget = 12;
const int kParallaxRefinementBudget = 0;
const int kParallaxShadowStepBudget = 0;
#else
const int kParallaxLinearStepBudget = 32;
// Refinement divides the bracketing step into min(n, this) parts, so the hit lands within 1/n^2 of
// the full shift up to n = 8 and within 1/8 of a pixel beyond (one linear step spans one pixel).
const int kParallaxRefinementBudget = 8;
const int kParallaxShadowStepBudget = 8;
#endif

// Below this on-screen shift (pixels) the relief is invisible, and the view march is skipped.
const float kParallaxSkipBelowPixels = 0.5f;
// From the skip threshold up to this shift (pixels) the relief depth fades in with a smoothstep, so
// the skip boundary never shows as a line.
const float kParallaxFullDepthFromPixels = 1.5f;
// The self-shadow's own on-screen length decides whether it shows, not the view's: a wall seen head
// on shifts nothing yet still casts full-length shadows across its joints. Below this length
// (pixels) of the shadow the full relief depth casts, the self-shadow is skipped; it fades in up to
// the next, so the shadow of a relief too fine for the pixel grid neither sparkles nor pops.
const float kParallaxShadowSkipBelowPixels = 1.0f;
const float kParallaxShadowFullFromPixels = 3.0f;
// Floor on |cos(view, normal)|: bounds the lateral shift at grazing angles to 20 times the depth.
const float kParallaxMinViewCosine = 0.05f;
// The march LOD sits at most this many mip levels below the footprint's major axis.
const float kParallaxLodSpan = 3.0f;
// Relative floor on the uv0 footprint's determinant (dimensionless): below it the footprint is
// rank-deficient (a triangle with degenerate UVs, or a pixel on a silhouette, where dFdx(uv0) is
// unbounded) and the march is skipped rather than solving for a scale that is noise.
const float kParallaxMinFootprintConditioning = 1.0e-4f;
// Self-shadow penumbra width (relief depth per relief depth): an occluder standing this far above the
// light ray, per relief depth the ray has climbed from the shaded point to it, shadows fully, and a
// lower one in proportion, whatever the step count and however deep the point lies. Taken along the
// climb, the width narrows as the light lowers; a width across the plane would not, and would erase a
// low sun's shadow. It is an edge filter chosen for the look and for the shadow's stability in motion,
// not the light's size: the sun's disc gives a relief a few centimetres deep a penumbra of a few
// millimetres, under a pixel except where a low sun rakes a surface near the camera. The penumbra lies
// inside the shadow, so a wider one lightens the shadow of a thin joint (parallax design, D6).
const float kParallaxShadowPenumbraWidth = 0.25f;

// The surface's texture-space frame at this pixel, solved from the screen-space derivatives.
struct GE_ParallaxFrame
{
    vec3 axisU;                   // surface metres per uv0 repeat along u, in the tangent plane
    vec3 axisV;                   // surface metres per uv0 repeat along v, in the tangent plane
    vec3 normal;                  // unit interpolated normal; the relief is carved along -normal
    vec2 uvPerPixelX;             // uv0 repeats per pixel step in x
    vec2 uvPerPixelY;             // uv0 repeats per pixel step in y
    float uvFootprintDeterminant; // det[uvPerPixelX uvPerPixelY], repeats^2 per pixel^2
    bool valid;                   // false where the footprint is rank-deficient; the march is skipped
};

// The view ray through the relief at this pixel.
struct GE_ParallaxRay
{
    vec2 heightUvEnd;   // height-space offset where the ray reaches height 0, repeats (faded)
    vec2 surfaceUvEnd;  // the same point as a uv0 offset, repeats (faded); displaces every other map
    float shiftPixels;  // E: on-screen length of the full-depth shift, pixels, before the fade
    float reliefMetres; // relief depth along the normal, metres (not faded: the self-shadow fades itself)
    float faceSign;     // +1 when the front face is seen, -1 for the back face of a two-sided surface
#ifdef GE_PARALLAX_RELIEF_DEPTH
    float depthMetres;  // metres along the view ray from the polygon to the ray's end at the full relief
                        // depth (faded); a hit lies rayDepth times this behind the polygon
#endif
    int linearSteps;    // n = min(budget, ceil(E)); 0 below kParallaxSkipBelowPixels
};

// Where the ray meets the relief.
struct GE_ParallaxHit
{
    vec2 heightUvOffset;  // height-space offset of the hit from the undisplaced UV, repeats
    vec2 surfaceUvOffset; // the hit as a uv0 offset, repeats
    float rayDepth;       // 0 at the polygon surface, 1 at the full relief depth
    int fetches;          // height samples the march issued (the steps view reads it)
};

// The linear part of the height slot's affine tiling transform applied to a uv0 offset.
vec2 GE_ParallaxHeightOffset(vec2 surfaceUvOffset, vec4 heightTransformRow0, vec4 heightTransformRow1)
{
    return vec2(heightTransformRow0.x * surfaceUvOffset.x + heightTransformRow0.y * surfaceUvOffset.y,
                heightTransformRow1.x * surfaceUvOffset.x + heightTransformRow1.y * surfaceUvOffset.y);
}

// Builds the frame from the interpolated normal, the vertex tangent (xyz, zero when the mesh has
// none) and the screen-space footprint: world metres and uv0 repeats per pixel along x and y.
//
// The cotangent solve [dP/dx dP/dy] = [axisU axisV] [duv/dx duv/dy] gives the metres per repeat
// along each UV axis, exactly per triangle, handedness included, so non-uniform instance scale and
// stretched UVs step the ray the right distance along u and v. With a vertex tangent, the axes take
// its direction, which stays smooth across triangle edges on curved meshes; each axis keeps the
// solve's length and sign, so a mesh whose tangents follow another handedness convention still
// marches toward increasing u and v.
GE_ParallaxFrame GE_ParallaxBuildFrame(vec3 normal, vec3 tangent, vec3 positionPerPixelX, vec3 positionPerPixelY,
                                       vec2 uvPerPixelX, vec2 uvPerPixelY)
{
    GE_ParallaxFrame frame;
    frame.axisU = vec3(0.0f, 0.0f, 0.0f);
    frame.axisV = vec3(0.0f, 0.0f, 0.0f);
    frame.normal = normal;
    frame.uvPerPixelX = uvPerPixelX;
    frame.uvPerPixelY = uvPerPixelY;
    frame.uvFootprintDeterminant = uvPerPixelX.x * uvPerPixelY.y - uvPerPixelX.y * uvPerPixelY.x;
    float footprintScale = dot(uvPerPixelX, uvPerPixelX) + dot(uvPerPixelY, uvPerPixelY);
    frame.valid = abs(frame.uvFootprintDeterminant) > kParallaxMinFootprintConditioning * footprintScale;
    if (!frame.valid)
        return frame;

    float inverseDeterminant = 1.0f / frame.uvFootprintDeterminant;
    vec3 axisU = (positionPerPixelX * uvPerPixelY.y - positionPerPixelY * uvPerPixelX.y) * inverseDeterminant;
    vec3 axisV = (positionPerPixelY * uvPerPixelX.x - positionPerPixelX * uvPerPixelY.x) * inverseDeterminant;
    axisU = axisU - normal * dot(normal, axisU);
    axisV = axisV - normal * dot(normal, axisV);

    vec3 tangentInPlane = tangent - normal * dot(normal, tangent);
    float tangentLengthSquared = dot(tangentInPlane, tangentInPlane);
    if (tangentLengthSquared > 1.0e-12f)
    {
        vec3 tangentDirection = tangentInPlane * (1.0f / sqrt(tangentLengthSquared));
        vec3 bitangentDirection = cross(normal, tangentDirection);
        float lengthU = length(axisU);
        float lengthV = length(axisV);
        axisU = tangentDirection * (dot(tangentDirection, axisU) < 0.0f ? -lengthU : lengthU);
        axisV = bitangentDirection * (dot(bitangentDirection, axisV) < 0.0f ? -lengthV : lengthV);
    }
    frame.axisU = axisU;
    frame.axisV = axisV;
    return frame;
}

// uv0 offset (repeats) of a tangent-plane displacement (metres), through the dual basis of
// (axisU, axisV): exact for sheared parametrizations. Zero when the axes are degenerate.
vec2 GE_ParallaxSurfaceToUv0(GE_ParallaxFrame frame, vec3 displacementMetres)
{
    float uu = dot(frame.axisU, frame.axisU);
    float uv = dot(frame.axisU, frame.axisV);
    float vv = dot(frame.axisV, frame.axisV);
    float gram = uu * vv - uv * uv;
    if (!(gram > 0.0f))
        return vec2(0.0f, 0.0f);
    float alongU = dot(frame.axisU, displacementMetres);
    float alongV = dot(frame.axisV, displacementMetres);
    return vec2(vv * alongU - uv * alongV, uu * alongV - uv * alongU) / gram;
}

// On-screen length (pixels) of a uv0 offset, through the inverse of the uv0 footprint.
float GE_ParallaxPixelLength(GE_ParallaxFrame frame, vec2 surfaceUvOffset)
{
    vec2 pixels = vec2(frame.uvPerPixelY.y * surfaceUvOffset.x - frame.uvPerPixelY.x * surfaceUvOffset.y,
                       frame.uvPerPixelX.x * surfaceUvOffset.y - frame.uvPerPixelX.y * surfaceUvOffset.x)
                / frame.uvFootprintDeterminant;
    return length(pixels);
}

// Sets up the view ray. viewDirection: unit, surface to eye, world space. reliefDepth: the
// material's relief depth as a fraction of one height repeat (0 disables). The height transform
// rows are the height slot's affine tiling rows (GE_TransformUV's st and st2).
//
// Two-sided surfaces march with |cos|, which carves inward from whichever face is seen: a mirrored
// relief on the back face, the standard choice for thin two-sided sheets.
GE_ParallaxRay GE_ParallaxSetupRay(GE_ParallaxFrame frame, vec3 viewDirection, float reliefDepth,
                                   vec4 heightTransformRow0, vec4 heightTransformRow1)
{
    GE_ParallaxRay ray;
    ray.heightUvEnd = vec2(0.0f, 0.0f);
    ray.surfaceUvEnd = vec2(0.0f, 0.0f);
    ray.shiftPixels = 0.0f;
    ray.reliefMetres = 0.0f;
    ray.faceSign = 1.0f;
#ifdef GE_PARALLAX_RELIEF_DEPTH
    ray.depthMetres = 0.0f;
#endif
    ray.linearSteps = 0;
    if (!frame.valid || !(reliefDepth > 0.0f))
        return ray;

    // Metres per height repeat: the square root of the surface area one height repeat covers
    // (the geometric mean of the metres per repeat along u and v when the axes are orthogonal).
    float heightDeterminant = heightTransformRow0.x * heightTransformRow1.y - heightTransformRow0.y * heightTransformRow1.x;
    float surfaceAreaPerRepeat = length(cross(frame.axisU, frame.axisV));
    if (!(abs(heightDeterminant) > 0.0f) || !(surfaceAreaPerRepeat > 0.0f))
        return ray;
    float reliefMetres = reliefDepth * sqrt(surfaceAreaPerRepeat / abs(heightDeterminant));
    ray.reliefMetres = reliefMetres;

    float viewCosineSigned = dot(viewDirection, frame.normal);
    ray.faceSign = viewCosineSigned < 0.0f ? -1.0f : 1.0f;
    float viewCosine = max(abs(viewCosineSigned), kParallaxMinViewCosine);
    vec3 viewInPlane = viewDirection - frame.normal * viewCosineSigned;
    // The ray runs from the eye into the surface (-viewDirection): descending reliefMetres below the
    // polygon moves it this far across the plane, metres.
    vec3 lateralMetres = viewInPlane * (-reliefMetres / viewCosine);
    vec2 surfaceUvEnd = GE_ParallaxSurfaceToUv0(frame, lateralMetres);
    ray.shiftPixels = GE_ParallaxPixelLength(frame, surfaceUvEnd);
    if (!(ray.shiftPixels >= kParallaxSkipBelowPixels))
        return ray;

    float fade = smoothstep(kParallaxSkipBelowPixels, kParallaxFullDepthFromPixels, ray.shiftPixels);
    ray.linearSteps = int(min(ceil(ray.shiftPixels), float(kParallaxLinearStepBudget)));
    ray.surfaceUvEnd = surfaceUvEnd * fade;
#ifdef GE_PARALLAX_RELIEF_DEPTH
    ray.depthMetres = reliefMetres * fade / viewCosine;
#endif
    ray.heightUvEnd = GE_ParallaxHeightOffset(ray.surfaceUvEnd, heightTransformRow0, heightTransformRow1);
    return ray;
}

// The footprint of a pixel in height texels, as mip levels: x the geometric mean of the footprint
// ellipse's axes, y its major axis. heightTexels: the height map's size in texels.
//
// The footprint is the 2x2 map from pixels to height texels. Its determinant and its Frobenius norm
// do not change when the screen axes rotate, so neither do the levels: sqrt|det| is the geometric
// mean of the ellipse's axes, and the larger singular value, computed from both, its major axis.
vec2 GE_ParallaxFootprintLevels(GE_ParallaxFrame frame, vec4 heightTransformRow0, vec4 heightTransformRow1,
                                vec2 heightTexels)
{
    vec2 footprintX = GE_ParallaxHeightOffset(frame.uvPerPixelX, heightTransformRow0, heightTransformRow1);
    vec2 footprintY = GE_ParallaxHeightOffset(frame.uvPerPixelY, heightTransformRow0, heightTransformRow1);
    vec2 texelsPerPixelX = vec2(footprintX.x * heightTexels.x, footprintX.y * heightTexels.y);
    vec2 texelsPerPixelY = vec2(footprintY.x * heightTexels.x, footprintY.y * heightTexels.y);
    float determinant = abs(texelsPerPixelX.x * texelsPerPixelY.y - texelsPerPixelX.y * texelsPerPixelY.x);
    float frobeniusSquared = dot(texelsPerPixelX, texelsPerPixelX) + dot(texelsPerPixelY, texelsPerPixelY);
    float discriminant = max(frobeniusSquared * frobeniusSquared - 4.0f * determinant * determinant, 0.0f);
    float majorSquared = 0.5f * (frobeniusSquared + sqrt(discriminant));
    return vec2(0.5f * log2(max(determinant, 1.0e-16f)), 0.5f * log2(max(majorSquared, 1.0e-16f)));
}

// The height-map LOD the whole view march samples at, mip levels: the geometric mean of the
// footprint, held within kParallaxLodSpan levels of its major axis so a grazing footprint does not
// fetch a mip far blurrier than the pixel's minor axis and flatten the relief exactly where it
// should show. mipBias: the view's TAAU bias, the same one every other material sample carries.
float GE_ParallaxMarchLod(GE_ParallaxFrame frame, vec4 heightTransformRow0, vec4 heightTransformRow1,
                          vec2 heightTexels, float mipBias)
{
    vec2 levels = GE_ParallaxFootprintLevels(frame, heightTransformRow0, heightTransformRow1, heightTexels);
    return clamp(levels.x, levels.y - kParallaxLodSpan, levels.y) + mipBias;
}

// The LOD the self-shadow samples at, mip levels: halfway between the footprint's extent along the
// light path's own screen direction (texelsPerPixelAlongPath, height texels per pixel; held within
// kParallaxLodSpan levels of the major axis as the march's LOD is) and its major axis, that is the
// geometric mean of the two extents. The shadow march resolves the relief along the path, so a gap
// about a pixel wide across it keeps most of its shadow where the footprint's long axis runs
// elsewhere (ground seen at eye height, whose long axis runs into the screen); the half toward the
// major axis filters most of what the pixel's long side cannot hold, so a groove narrower than it
// rarely blackens a whole distant pixel. Where the path runs along the long axis this is the major
// axis. mipBias as for GE_ParallaxMarchLod.
float GE_ParallaxShadowLod(GE_ParallaxFrame frame, vec4 heightTransformRow0, vec4 heightTransformRow1,
                           vec2 heightTexels, float texelsPerPixelAlongPath, float mipBias)
{
    float major = GE_ParallaxFootprintLevels(frame, heightTransformRow0, heightTransformRow1, heightTexels).y;
    float alongPath = clamp(log2(max(texelsPerPixelAlongPath, 1.0e-8f)), major - kParallaxLodSpan, major);
    return 0.5f * (alongPath + major) + mipBias;
}

// Depth of linear sample `layer` of `steps`, 0 at the polygon and 1 at the full relief depth. The
// last sample is exactly 1 rather than steps * (1 / steps), which a GPU may round one ulp short:
// the ray height there is 0, so every field value meets it, and that is what guarantees the search
// a crossing even on a floor of height exactly 0.
float GE_ParallaxLayerDepth(int layer, int steps, float stepDepth)
{
    return layer >= steps ? 1.0f : float(layer) * stepDepth;
}

// Marches the view ray through the height field from heightUv (the undisplaced height-space UV).
//
// Linear search in batches of four: four fetches issued back to back, then four compares, so the
// texture latency overlaps without leaning on occupancy (at most three fetches land past the hit).
// Sample depths past the last step clamp to it. The gap is field height minus ray height: the ray
// is inside the relief once the gap reaches zero, which it does at the latest at the last sample
// (depth exactly 1, ray height 0, and the field is never below 0). Refinement then walks the
// bracketing step in min(n, budget) parts, which catches
// thin features the linear pass stepped between, and the hit is interpolated between the last
// sample above the ray and the first at or below it.
GE_ParallaxHit GE_ParallaxMarch(vec2 heightUv, GE_ParallaxRay ray, float lod)
{
    GE_ParallaxHit hit;
    hit.heightUvOffset = vec2(0.0f, 0.0f);
    hit.surfaceUvOffset = vec2(0.0f, 0.0f);
    hit.rayDepth = 0.0f;
    hit.fetches = 0;
    int steps = ray.linearSteps;
    if (steps <= 0)
        return hit;

    float stepDepth = 1.0f / float(steps);
    float aboveDepth = 0.0f;
    float aboveGap = -1.0f;
    float belowDepth = 0.0f;
    float belowGap = 0.0f;
    bool crossed = false;
    for (int first = 0; first <= steps && !crossed; first += 4)
    {
        vec4 depths = vec4(GE_ParallaxLayerDepth(first, steps, stepDepth),
                           GE_ParallaxLayerDepth(first + 1, steps, stepDepth),
                           GE_ParallaxLayerDepth(first + 2, steps, stepDepth),
                           GE_ParallaxLayerDepth(first + 3, steps, stepDepth));
        vec4 heights = vec4(GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * depths.x, lod),
                            GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * depths.y, lod),
                            GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * depths.z, lod),
                            GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * depths.w, lod));
        hit.fetches += 4;
        vec4 gaps = vec4(heights.x - (1.0f - depths.x), heights.y - (1.0f - depths.y),
                         heights.z - (1.0f - depths.z), heights.w - (1.0f - depths.w));
        int crossing = gaps.x >= 0.0f ? 0 : (gaps.y >= 0.0f ? 1 : (gaps.z >= 0.0f ? 2 : (gaps.w >= 0.0f ? 3 : 4)));
        if (crossing < 4)
        {
            crossed = true;
            belowDepth = depths[crossing];
            belowGap = gaps[crossing];
            if (crossing > 0)
            {
                aboveDepth = depths[crossing - 1];
                aboveGap = gaps[crossing - 1];
            }
        }
        else
        {
            aboveDepth = depths.w;
            aboveGap = gaps.w;
        }
    }

    // A crossing at depth 0 means the undisplaced point already lies on the relief's top surface.
    if (!(belowDepth > 0.0f))
        return hit;

    int parts = min(steps, kParallaxRefinementBudget);
    float bracketStart = aboveDepth;
    float partDepth = (belowDepth - aboveDepth) / float(max(parts, 1));
    bool refined = false;
    for (int first = 1; first < parts && !refined; first += 4)
    {
        vec4 depths = vec4(bracketStart + partDepth * float(min(first, parts - 1)),
                           bracketStart + partDepth * float(min(first + 1, parts - 1)),
                           bracketStart + partDepth * float(min(first + 2, parts - 1)),
                           bracketStart + partDepth * float(min(first + 3, parts - 1)));
        vec4 heights = vec4(GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * depths.x, lod),
                            GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * depths.y, lod),
                            GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * depths.z, lod),
                            GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * depths.w, lod));
        hit.fetches += 4;
        vec4 gaps = vec4(heights.x - (1.0f - depths.x), heights.y - (1.0f - depths.y),
                         heights.z - (1.0f - depths.z), heights.w - (1.0f - depths.w));
        int crossing = gaps.x >= 0.0f ? 0 : (gaps.y >= 0.0f ? 1 : (gaps.z >= 0.0f ? 2 : (gaps.w >= 0.0f ? 3 : 4)));
        if (crossing < 4)
        {
            refined = true;
            belowDepth = depths[crossing];
            belowGap = gaps[crossing];
            if (crossing > 0)
            {
                aboveDepth = depths[crossing - 1];
                aboveGap = gaps[crossing - 1];
            }
        }
        else
        {
            aboveDepth = depths.w;
            aboveGap = gaps.w;
        }
    }

    // aboveGap < 0 <= belowGap, so the denominator is positive.
    hit.rayDepth = aboveDepth + (belowDepth - aboveDepth) * (-aboveGap / (belowGap - aboveGap));
    hit.heightUvOffset = ray.heightUvEnd * hit.rayDepth;
    hit.surfaceUvOffset = ray.surfaceUvEnd * hit.rayDepth;
    return hit;
}

#ifdef GE_PARALLAX_RELIEF_DEPTH
// The hit the march found, rebuilt from where it lies along the view ray: depthOffsetMetres behind the
// polygon (GE_ParallaxOffsetToDepth of the depth the prepass wrote), clamped to the relief.
GE_ParallaxHit GE_ParallaxHitAtDepth(GE_ParallaxRay ray, float depthOffsetMetres)
{
    GE_ParallaxHit hit;
    hit.heightUvOffset = vec2(0.0f, 0.0f);
    hit.surfaceUvOffset = vec2(0.0f, 0.0f);
    hit.rayDepth = 0.0f;
    hit.fetches = 0;
    if (ray.linearSteps <= 0 || !(ray.depthMetres > 0.0f))
        return hit;
    hit.rayDepth = clamp(depthOffsetMetres / ray.depthMetres, 0.0f, 1.0f);
    hit.heightUvOffset = ray.heightUvEnd * hit.rayDepth;
    hit.surfaceUvOffset = ray.surfaceUvEnd * hit.rayDepth;
    return hit;
}

// Whether a point on the view ray, rayDepth into the relief, is the relief's own hit or lies in the air
// above it. Every point the ray passes before its first crossing is in the air (the field below the
// ray), so a nearer surface inside the relief (a prop sunk into it) reads as air. The march samples the
// ray on one grid, the linear steps split into refinement parts (width 1 / (n min(n, budget))), and
// interpolates its hit inside the part whose far end it found inside the field: the hit counts when that
// grid point, the first at or past it, is inside the field again, or the hit itself is. rayDepthError
// bounds how far a hit rebuilt from a depth lies from the one the march found (in relief depths), so the
// march's grid point is the first at or past one of the two ends of that interval: both are tried, and
// a rebuilt hit that rounding carried across a grid point still finds the one the march stopped at.
struct GE_ParallaxReliefCheck
{
    bool onRelief;
    int fetches;
};

GE_ParallaxReliefCheck GE_ParallaxCheckRelief(vec2 heightUv, GE_ParallaxRay ray, float rayDepth, float rayDepthError,
                                             float lod)
{
    GE_ParallaxReliefCheck check;
    check.onRelief = true;
    check.fetches = 0;
    int steps = ray.linearSteps;
    if (steps <= 0)
        return check;
    float parts = float(steps) * float(max(min(steps, kParallaxRefinementBudget), 1));
    float gapAt = GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * rayDepth, lod) - (1.0f - rayDepth);
    check.fetches = 1;
    if (gapAt >= -1.0f / parts)
        return check;
    // The grid point at or past the earliest and the latest the hit can be, with a thousandth of a part of
    // slack for a hit that sits on a grid point.
    float earliest = min(ceil((rayDepth - rayDepthError) * parts - 1.0e-3f) / parts, 1.0f);
    float latest = min(ceil((rayDepth + rayDepthError) * parts - 1.0e-3f) / parts, 1.0f);
    float gapEarliest = GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * earliest, lod) - (1.0f - earliest);
    check.fetches = 2;
    check.onRelief = gapEarliest >= 0.0f;
    if (check.onRelief || !(latest > earliest))
        return check;
    float gapLatest = GE_ParallaxSampleHeight(heightUv + ray.heightUvEnd * latest, lod) - (1.0f - latest);
    check.fetches = 3;
    check.onRelief = gapLatest >= 0.0f;
    return check;
}
#endif

// What the self-shadow hands back.
struct GE_ParallaxShadow
{
    float light; // how much of the primary directional light reaches the point: 1 unoccluded, 0 shadowed
    int fetches; // height samples it took (the steps view adds them to the view march's)
};

// The self-shadow of the shaded point toward the primary directional light. lightDirection: unit,
// surface to light, world space; zero when no directional light shines. heightTexels and mipBias as
// for GE_ParallaxMarchLod. The point is the view march's hit (the undisplaced surface where the view
// march was skipped); its height and the light path are read at GE_ParallaxShadowLod, one sample for
// the point and min(budget, ceil(E_L)) along the path, E_L its on-screen length in pixels. The
// shadow fades in with the length the full relief depth casts (kParallaxShadowSkipBelowPixels to
// kParallaxShadowFullFromPixels). A light below the seen face (the lighting's own cosine darkens
// it), a point on the top surface, a path shorter than half a pixel and the compat arm all leave the
// light at 1.
GE_ParallaxShadow GE_ParallaxSelfShadow(GE_ParallaxFrame frame, GE_ParallaxRay ray, GE_ParallaxHit hit,
                                        vec2 heightUv, vec3 lightDirection, vec4 heightTransformRow0,
                                        vec4 heightTransformRow1, vec2 heightTexels, float mipBias)
{
    GE_ParallaxShadow shadow;
    shadow.light = 1.0f;
    shadow.fetches = 0;
    if (kParallaxShadowStepBudget <= 0 || !(ray.reliefMetres > 0.0f))
        return shadow;
    float lightCosineSigned = dot(lightDirection, frame.normal);
    float lightCosine = lightCosineSigned * ray.faceSign;
    if (!(lightCosine > 0.0f))
        return shadow;

    // The path the light takes from the full relief depth up to the polygon, and its length on screen.
    vec3 lightInPlane = lightDirection - frame.normal * lightCosineSigned;
    vec3 fullPathMetres = lightInPlane * (ray.reliefMetres / max(lightCosine, kParallaxMinViewCosine));
    vec2 surfaceUvFullPath = GE_ParallaxSurfaceToUv0(frame, fullPathMetres);
    float fullPathPixels = GE_ParallaxPixelLength(frame, surfaceUvFullPath);
    if (!(fullPathPixels >= kParallaxShadowSkipBelowPixels))
        return shadow;

    // The same path in height texels: its length over its length on screen is the footprint's
    // extent along it.
    vec2 heightUvFullPath = GE_ParallaxHeightOffset(surfaceUvFullPath, heightTransformRow0, heightTransformRow1);
    float fullPathTexels = length(vec2(heightUvFullPath.x * heightTexels.x, heightUvFullPath.y * heightTexels.y));
    float shadowLod = GE_ParallaxShadowLod(frame, heightTransformRow0, heightTransformRow1, heightTexels,
                                           fullPathTexels / fullPathPixels, mipBias);
    vec2 start = heightUv + hit.heightUvOffset;
    float pointHeight = GE_ParallaxSampleHeight(start, shadowLod);
    shadow.fetches = 1;
    float rise = 1.0f - pointHeight;
    float pathPixels = fullPathPixels * rise;
    if (!(pathPixels >= kParallaxSkipBelowPixels))
        return shadow;

    int steps = int(min(ceil(pathPixels), float(kParallaxShadowStepBudget)));
    vec2 heightUvPath = heightUvFullPath * rise;
    // The steepest occluder: its height above the ray (relief depths) per fraction of the path. Over
    // that fraction the ray climbs rise * along relief depths, so steepest / rise is the occluder's
    // height above the ray per relief depth of climb.
    float steepest = 0.0f;
    for (int i = 1; i <= steps; ++i)
    {
        float along = float(i) / float(steps);
        float field = GE_ParallaxSampleHeight(start + heightUvPath * along, shadowLod);
        float rayHeight = pointHeight + rise * along;
        steepest = max(steepest, (field - rayHeight) / along);
    }
    shadow.fetches += steps;
    float occlusion = steepest / (rise * kParallaxShadowPenumbraWidth);
    float fade = smoothstep(kParallaxShadowSkipBelowPixels, kParallaxShadowFullFromPixels, fullPathPixels);
    shadow.light = 1.0f - clamp(occlusion, 0.0f, 1.0f) * fade;
    return shadow;
}
// GE_SHARED_PARALLAX_OCCLUSION_END

#endif // GE_PARALLAX_OCCLUSION_GLSL
