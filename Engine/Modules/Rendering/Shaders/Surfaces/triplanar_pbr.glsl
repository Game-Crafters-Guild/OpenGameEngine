// Surface shader: Triplanar PBR front-end.
//
// A texture-PROJECTION front-end for the Synty PolygonShader family (rocks, cliffs,
// dirt, grass). It replaces UV sampling with world-space, normal-weighted three-axis
// projection and feeds the UNCHANGED OpenPBR BRDF via SurfaceOutput — it is NOT a
// lighting model. Everything below writes baseColor/normalWS/metallic/roughness/opacity
// exactly like standard_pbr.glsl; only the sampling differs.
//
// Why triplanar has no UV stretch: geometry is sampled by projecting worldPos onto the
// three cardinal planes and blending by |worldNormal|^sharpness, so a steep face reads
// the side projection at the same texel frequency as a flat one — no UV seam to smear.
//
// Texture layout (interleaved albedo/normal per axis, so the collapse fast path is 0-1):
//   slot 0 triplanarAlbedoTop   slot 1 triplanarNormalTop
//   slot 2 triplanarAlbedoSide  slot 3 triplanarNormalSide
//   slot 4 triplanarAlbedoBottom slot 5 triplanarNormalBottom
// Albedo and normal layering are INDEPENDENT flags: uParams23.w == 1 reads per-axis
// albedo from slots 0/2/4 (0 collapses every axis onto slot 0); uParams24.w == 1 reads
// per-axis normals from slots 1/3/5 (0 collapses onto slot 1). They must not share one
// flag: a per-axis-albedo material with a single (or no) normal set would otherwise read
// slot 3, whose unbound default is WHITE (the enum-slot kEmissive default) — and white
// decodes to a garbage (1,1,1) tangent normal on the side projection.
// The 3 normal slots are linear-tagged by name (IsLinearTextureSlot) so they upload UNORM.
//
// No vertex tangent is used: the world normal is reconstructed from the three tangent-space
// samples via the Whiteout blend, so these materials get correct relief even on Synty FBX
// meshes that carry no tangent stream (this sidesteps the HAS_TANGENT gate entirely).

// Axis-blend exponent range mapped from uParams22.w (0..1, from Unity _Triplanar_Fade).
// Higher -> sharper axis transitions (crisper top cap on near-flat ground); lower -> softer
// interpenetration (natural rocky blend). Artistic value stays in the material param; the
// physical exponent mapping lives here.
const float kTriplanarSharpnessMin = 1.0;
const float kTriplanarSharpnessMax = 8.0;

// Sample a bindless slot chosen at runtime (top/side/bottom collapse to the same index in
// the fast path). GE_SLOT_TEX/GE_SLOT_SAMPLER accept a dynamic slot — they index
// ge_MatData.TextureIndices[slot] and the shared sampler array. The explicit bias is the
// per-view TAAU sharpness compensation the alias overloads apply — raw-ordinal sampling
// bypasses those, so it rides here.
vec4 GE_SampleTriplanarSlot(uint slot, vec2 uv)
{
#if defined(GE_COMPAT_PROFILE)
    return ge_CompatSampleSlot(slot, uv, ge_mipBiasParams.x);
#else
    return texture(sampler2D(GE_SLOT_TEX(slot), GE_SLOT_SAMPLER(slot)), uv, ge_mipBiasParams.x);
#endif
}

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    // Geometric world normal drives both the projection weights and the Whiteout blend.
    vec3 N = normalize(sIn.normalWS);
    vec3 an = abs(N);

    float sharpness = mix(kTriplanarSharpnessMin, kTriplanarSharpnessMax,
                          clamp(Mat.uParams22.w, 0.0, 1.0));
    vec3 w = pow(an, vec3(sharpness));
    w /= max(w.x + w.y + w.z, 1e-5);

    // Slot selection. Albedo (uParams23.w) and normal (uParams24.w) layering collapse
    // independently: collapsed folds side/bottom onto the top slot of that set. See the
    // header note on why one shared flag would read slot 3's black default as a normal.
    bool albedoLayered = Mat.uParams23.w > 0.5;
    bool normalLayered = Mat.uParams24.w > 0.5;
    uint albedoTopSlot    = 0u;
    uint normalTopSlot    = 1u;
    uint albedoSideSlot   = albedoLayered ? 2u : 0u;
    uint normalSideSlot   = normalLayered ? 3u : 1u;
    uint albedoBottomSlot = albedoLayered ? 4u : 0u;
    uint normalBottomSlot = normalLayered ? 5u : 1u;

    // The Y (up/down) projection reads the top set on up-facing texels, the bottom set on
    // down-facing texels. X and Z projections always read the side set.
    bool topFacing = N.y >= 0.0;
    uint albedoYSlot = topFacing ? albedoTopSlot : albedoBottomSlot;
    uint normalYSlot = topFacing ? normalTopSlot : normalBottomSlot;

    float tilingSide = Mat.uParams22.y;
    float tilingY    = topFacing ? Mat.uParams22.x : Mat.uParams22.z;

    // World-space projected UVs. positionWS is FULL world (adapter_vertex.glsl sets
    // vPosWS = fullWorldPos), so the projection is stable as the camera moves.
    vec3 p = sIn.positionWS;
    vec2 uvX = p.zy * tilingSide; // X-facing plane -> side
    vec2 uvY = p.xz * tilingY;    // Y-facing plane -> top / bottom
    vec2 uvZ = p.xy * tilingSide; // Z-facing plane -> side

    // --- Albedo ---
    vec4 aX = GE_SampleTriplanarSlot(albedoSideSlot, uvX);
    vec4 aY = GE_SampleTriplanarSlot(albedoYSlot,    uvY);
    vec4 aZ = GE_SampleTriplanarSlot(albedoSideSlot, uvZ);
    vec4 albedo = aX * w.x + aY * w.y + aZ * w.z;
    o.baseColor = albedo.rgb * Mat.uBaseColor.rgb * sIn.vertexColor.rgb;
    o.opacity   = albedo.a * Mat.uBaseColor.a * sIn.vertexColor.a;

    // --- Metallic / roughness (per-axis scalar; census: zero map usage) ---
    // Side covers the X and Z projections; the Y projection uses top or bottom.
    float metalSide = Mat.uParams23.y;
    float metalY    = topFacing ? Mat.uParams23.x : Mat.uParams23.z;
    float roughSide = Mat.uParams24.y;
    float roughY    = topFacing ? Mat.uParams24.x : Mat.uParams24.z;
    o.metallic  = clamp(metalSide * (w.x + w.z) + metalY * w.y, 0.0, 1.0);
    o.roughness = clamp(roughSide * (w.x + w.z) + roughY * w.y, 0.04, 1.0);

    // --- Normal (Whiteout blend, reoriented tangent-space -> world) ---
    // Tangent-space normals per axis (no green-channel flip: engine expects OpenGL/+Y-up,
    // matching Unity's authoring — see standard_pbr.glsl:70). Z reconstructed —
    // see GE_DecodeTangentNormal.
    vec3 tnX = GE_DecodeTangentNormal(GE_SampleTriplanarSlot(normalSideSlot, uvX));
    vec3 tnY = GE_DecodeTangentNormal(GE_SampleTriplanarSlot(normalYSlot,    uvY));
    vec3 tnZ = GE_DecodeTangentNormal(GE_SampleTriplanarSlot(normalSideSlot, uvZ));
    // Whiteout blend: add the geometric normal into each tangent normal's XY and force its Z
    // to point along the surface, then swizzle each into world orientation and triblend.
    tnX = vec3(tnX.xy + N.zy, abs(tnX.z) * N.x);
    tnY = vec3(tnY.xy + N.xz, abs(tnY.z) * N.y);
    tnZ = vec3(tnZ.xy + N.xy, abs(tnZ.z) * N.z);
    o.normalWS = normalize(tnX.zyx * w.x + tnY.xzy * w.y + tnZ.xyz * w.z);
    o.coatNormalWS = o.normalWS;

    // Emission, snow cap, and height/parallax overlay are deferred (design §3.1): the
    // sampled corpus has _Enable_Triplanar_Emission / _Enable_Snow / overlay all off. The
    // OpenPBR neutral defaults (white specular, weight 1, IOR 1.5 -> F0 0.04) from
    // DefaultSurfaceOutput() carry the dielectric response for these stylized rocks.
    return o;
}
