struct type_9 {
    member: vec3<f32>,
    member_1: f32,
    member_2: vec3<f32>,
    member_3: f32,
    member_4: vec3<f32>,
    member_5: f32,
    member_6: vec3<f32>,
    member_7: f32,
    member_8: vec3<f32>,
    member_9: f32,
    member_10: vec3<f32>,
    member_11: f32,
    member_12: f32,
    member_13: f32,
    member_14: f32,
    member_15: f32,
    member_16: vec3<f32>,
    member_17: f32,
    member_18: f32,
    member_19: f32,
    member_20: f32,
    member_21: f32,
    member_22: vec2<f32>,
    member_23: f32,
    member_24: f32,
    member_25: vec4<f32>,
    member_26: vec4<f32>,
    member_27: vec4<f32>,
    member_28: vec4<f32>,
    member_29: vec4<f32>,
    member_30: vec4<f32>,
    member_31: vec4<f32>,
    member_32: vec4<f32>,
    member_33: vec4<f32>,
    member_34: vec4<f32>,
}

struct type_12 {
    member: f32,
    member_1: f32,
    member_2: f32,
    member_3: f32,
    member_4: vec3<f32>,
    member_5: f32,
    member_6: vec3<f32>,
    member_7: f32,
    member_8: vec3<f32>,
    member_9: f32,
    member_10: vec3<f32>,
    member_11: vec3<f32>,
    member_12: vec3<f32>,
    member_13: vec3<f32>,
    member_14: f32,
    member_15: f32,
    member_16: f32,
    member_17: f32,
    member_18: f32,
    member_19: vec2<f32>,
    member_20: f32,
    member_21: f32,
    member_22: f32,
    member_23: vec3<f32>,
    member_24: u32,
    member_25: f32,
}

@group(0) @binding(0)
var<uniform> global: type_9;
@group(0) @binding(2)
var<uniform> global_1: type_12;
@group(0) @binding(65)
var global_2: sampler;
@group(0) @binding(1)
var global_3: texture_2d<f32>;
var<private> global_4: vec4<f32>;
@group(0) @binding(68)
var global_5: sampler;
@group(0) @binding(4)
var global_6: texture_2d<f32>;
var<private> global_7: vec2<f32>;
@group(0) @binding(67)
var global_8: sampler;
@group(0) @binding(3)
var global_9: texture_2d<f32>;

fn function_() {
    var phi_6598_: vec3<f32>;
    var phi_6547_: vec3<f32>;
    var phi_6548_: vec3<f32>;
    var phi_6549_: vec3<f32>;
    var phi_6553_: vec3<f32>;
    var phi_6551_: f32;
    var phi_6554_: vec3<f32>;
    var phi_6555_: vec3<f32>;
    var local: bool;
    var local_1: f32;
    var phi_6564_: vec3<f32>;
    var phi_6562_: f32;
    var phi_6565_: vec3<f32>;
    var phi_6566_: vec3<f32>;
    var local_2: bool;
    var local_3: f32;
    var phi_6570_: vec3<f32>;
    var phi_6568_: f32;
    var phi_6571_: vec3<f32>;
    var phi_6572_: vec3<f32>;
    var phi_6573_: vec3<f32>;
    var phi_3307_: bool;
    var phi_6574_: vec2<f32>;
    var phi_6579_: f32;
    var phi_6575_: f32;
    var phi_6587_: vec3<f32>;
    var phi_6588_: vec3<f32>;
    var phi_6580_: i32;
    var phi_6593_: vec2<f32>;
    var phi_6592_: vec2<f32>;
    var phi_6586_: vec3<f32>;
    var phi_6582_: vec2<f32>;
    var phi_6581_: vec2<f32>;
    var phi_6583_: f32;
    var phi_6591_: vec3<f32>;
    var phi_6590_: vec3<f32>;
    var phi_6480_: f32;
    var phi_6482_: f32;
    var phi_6538_: vec3<f32>;
    var phi_6537_: vec3<f32>;
    var phi_6525_: vec3<f32>;
    var phi_6484_: i32;
    var phi_6524_: vec3<f32>;
    var phi_6494_: vec3<f32>;
    var phi_6492_: f32;
    var phi_6495_: vec3<f32>;
    var phi_6497_: vec3<f32>;
    var phi_6496_: i32;
    var local_4: vec3<f32>;
    var phi_6500_: f32;
    var phi_6499_: bool;
    var phi_6516_: f32;
    var phi_6515_: vec3<f32>;
    var phi_6517_: vec3<f32>;
    var phi_6518_: vec3<f32>;
    var phi_6519_: vec3<f32>;
    var local_5: vec3<f32>;
    var local_6: vec3<f32>;
    var local_7: vec3<f32>;

    switch bitcast<i32>(0u) {
        default: {
            let _e185 = global_7;
            let _e188 = ((_e185.x * 2f) - 1f);
            let _e192 = (((1f - _e185.y) * 2f) - 1f);
            let _e194 = global.member_21;
            let _e196 = global.member_20;
            let _e197 = max(_e196, 0.001f);
            if (_e194 <= 0f) {
                switch bitcast<i32>(0u) {
                    default: {
                        let _e201 = global.member_23;
                        let _e202 = clamp(_e201, 0f, 1f);
                        let _e204 = global.member_7;
                        let _e206 = (_e192 - clamp(_e204, -1f, 1f));
                        let _e209 = clamp(((1f - _e206) * 0.5f), 0f, 1f);
                        let _e212 = global.member_32[3u];
                        if (_e212 >= 0.5f) {
                            if (_e209 < 0.5f) {
                                let _e216 = global.member_32;
                                let _e219 = global.member_33;
                                phi_6598_ = mix(_e216.xyz, _e219.xyz, vec3(smoothstep(0f, 0.5f, _e209)));
                            } else {
                                let _e225 = global.member_33;
                                let _e228 = global.member_34;
                                phi_6598_ = mix(_e225.xyz, _e228.xyz, vec3(smoothstep(0.5f, 1f, _e209)));
                            }
                            let _e234 = phi_6598_;
                            let _e236 = global.member_1;
                            let _e238 = (_e234 * exp2(_e236));
                            global_4 = vec4<f32>(_e238.x, _e238.y, _e238.z, 1f);
                            break;
                        }
                        let _e244 = global_1.member;
                        let _e248 = global.member;
                        let _e250 = normalize((_e248 - vec3<f32>(0f, -(_e244), 0f)));
                        let _e252 = global.member_6;
                        let _e255 = (_e252 - (_e250 * dot(_e252, _e250)));
                        phi_6547_ = _e255;
                        if (dot(_e255, _e255) < 0.000001f) {
                            let _e259 = global.member_4;
                            phi_6547_ = (_e259 - (_e250 * dot(_e259, _e250)));
                        }
                        let _e264 = phi_6547_;
                        phi_6548_ = _e264;
                        if (dot(_e264, _e264) < 0.000001f) {
                            let _e268 = global.member_2;
                            phi_6548_ = (_e268 - (_e250 * dot(_e268, _e250)));
                        }
                        let _e273 = phi_6548_;
                        phi_6549_ = _e273;
                        if (dot(_e273, _e273) < 0.000001f) {
                            let _e280 = select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 0f, 1f), vec3((abs(_e250.z) < 0.98f)));
                            phi_6549_ = (_e280 - (_e250 * dot(_e280, _e250)));
                        }
                        let _e285 = phi_6549_;
                        let _e286 = normalize(_e285);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e290 = global.member_29[3u];
                                let _e291 = (_e290 >= 0f);
                                local = _e291;
                                local_1 = _e290;
                                local_2 = _e291;
                                local_3 = _e290;
                                if _e291 {
                                    let _e294 = global.member_29[2u];
                                    let _e295 = sin(_e294);
                                    let _e296 = cos(_e294);
                                    let _e313 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e295 * _e250.x) + (_e296 * _e250.z)), ((_e296 * _e250.x) - (_e295 * _e250.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e250.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    phi_6555_ = (_e313.xyz * _e290);
                                    break;
                                }
                                let _e322 = normalize(cross(_e250, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e250.z) > 0.98f)))));
                                let _e326 = dot(_e250, _e250);
                                let _e336 = ((((asin(clamp(_e326, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e342 = (0.5f + ((0.5f * sign(_e336)) * sqrt(abs(_e336))));
                                let _e343 = vec2<f32>(((atan2(dot(_e250, normalize(cross(_e322, _e250))), dot(_e250, _e322)) * 0.15915494f) + 0.5f), _e342);
                                let _e348 = textureSampleLevel(global_3, global_2, vec2<f32>(_e343.x, clamp(_e342, 0.001953125f, 0.9980469f)), 0f);
                                let _e351 = global_1.member_24;
                                let _e352 = (_e351 == 2u);
                                let _e354 = global_1.member_8;
                                let _e356 = global_1.member_9;
                                let _e358 = global_1.member_10;
                                let _e360 = global_1.member_18;
                                let _e362 = global_1.member_23;
                                let _e364 = global_1.member_17;
                                let _e366 = global_1.member_13;
                                let _e370 = vec3(clamp(_e202, 0f, 1f));
                                let _e371 = mix((_e354 * max(_e356, 0f)), _e358, _e370);
                                phi_6553_ = _e348.xyz;
                                if (_e352 && (_e326 < 0f)) {
                                    phi_6553_ = mix(mix(_e371, _e362, vec3(clamp(_e360, 0f, 1f))), _e371, vec3(clamp((-(_e326) * max(_e364, 0.001f)), 0f, 1f)));
                                }
                                let _e384 = phi_6553_;
                                phi_6554_ = _e384;
                                if ((_e202 > 0f) && (_e326 > select(-1f, 0f, _e352))) {
                                    if _e352 {
                                        phi_6551_ = max(_e326, 0f);
                                    } else {
                                        phi_6551_ = abs(_e326);
                                    }
                                    let _e392 = phi_6551_;
                                    let _e404 = fract((((normalize(_e250) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e409 = (_e404 + vec3(dot(_e404, (_e404.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_6554_ = mix(_e384, (mix(mix(_e384, vec3<f32>(0.00018f, 0.00009f, 0.0006f), vec3(smoothstep(0f, 0.85f, _e392))), _e366, vec3((1f - smoothstep(0f, 0.55f, _e392)))) * (1f + ((fract(((_e409.x * _e409.y) * _e409.z)) - 0.5f) * 0.05f))), _e370);
                                }
                                let _e422 = phi_6554_;
                                phi_6555_ = _e422;
                                break;
                            }
                        }
                        let _e424 = phi_6555_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e427 = local;
                                if _e427 {
                                    let _e430 = global.member_29[2u];
                                    let _e431 = sin(_e430);
                                    let _e432 = cos(_e430);
                                    let _e449 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e431 * _e286.x) + (_e432 * _e286.z)), ((_e432 * _e286.x) - (_e431 * _e286.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e286.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    let _e452 = local_1;
                                    phi_6566_ = (_e449.xyz * _e452);
                                    break;
                                }
                                let _e460 = normalize(cross(_e250, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e250.z) > 0.98f)))));
                                let _e464 = dot(_e286, _e250);
                                let _e474 = ((((asin(clamp(_e464, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e480 = (0.5f + ((0.5f * sign(_e474)) * sqrt(abs(_e474))));
                                let _e481 = vec2<f32>(((atan2(dot(_e286, normalize(cross(_e460, _e250))), dot(_e286, _e460)) * 0.15915494f) + 0.5f), _e480);
                                let _e486 = textureSampleLevel(global_3, global_2, vec2<f32>(_e481.x, clamp(_e480, 0.001953125f, 0.9980469f)), 0f);
                                let _e489 = global_1.member_24;
                                let _e490 = (_e489 == 2u);
                                let _e492 = global_1.member_8;
                                let _e494 = global_1.member_9;
                                let _e496 = global_1.member_10;
                                let _e498 = global_1.member_18;
                                let _e500 = global_1.member_23;
                                let _e502 = global_1.member_17;
                                let _e504 = global_1.member_13;
                                let _e508 = vec3(clamp(_e202, 0f, 1f));
                                let _e509 = mix((_e492 * max(_e494, 0f)), _e496, _e508);
                                phi_6564_ = _e486.xyz;
                                if (_e490 && (_e464 < 0f)) {
                                    phi_6564_ = mix(mix(_e509, _e500, vec3(clamp(_e498, 0f, 1f))), _e509, vec3(clamp((-(_e464) * max(_e502, 0.001f)), 0f, 1f)));
                                }
                                let _e522 = phi_6564_;
                                phi_6565_ = _e522;
                                if ((_e202 > 0f) && (_e464 > select(-1f, 0f, _e490))) {
                                    if _e490 {
                                        phi_6562_ = max(_e464, 0f);
                                    } else {
                                        phi_6562_ = abs(_e464);
                                    }
                                    let _e530 = phi_6562_;
                                    let _e542 = fract((((normalize(_e286) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e547 = (_e542 + vec3(dot(_e542, (_e542.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_6565_ = mix(_e522, (mix(mix(_e522, vec3<f32>(0.00018f, 0.00009f, 0.0006f), vec3(smoothstep(0f, 0.85f, _e530))), _e504, vec3((1f - smoothstep(0f, 0.55f, _e530)))) * (1f + ((fract(((_e547.x * _e547.y) * _e547.z)) - 0.5f) * 0.05f))), _e508);
                                }
                                let _e560 = phi_6565_;
                                phi_6566_ = _e560;
                                break;
                            }
                        }
                        let _e562 = phi_6566_;
                        let _e563 = -(_e250);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e566 = local_2;
                                if _e566 {
                                    let _e569 = global.member_29[2u];
                                    let _e570 = sin(_e569);
                                    let _e571 = cos(_e569);
                                    let _e588 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e570 * _e563.x) + (_e571 * _e563.z)), ((_e571 * _e563.x) - (_e570 * _e563.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e563.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    let _e591 = local_3;
                                    phi_6572_ = (_e588.xyz * _e591);
                                    break;
                                }
                                let _e599 = normalize(cross(_e250, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e250.z) > 0.98f)))));
                                let _e603 = dot(_e563, _e250);
                                let _e613 = ((((asin(clamp(_e603, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e619 = (0.5f + ((0.5f * sign(_e613)) * sqrt(abs(_e613))));
                                let _e620 = vec2<f32>(((atan2(dot(_e563, normalize(cross(_e599, _e250))), dot(_e563, _e599)) * 0.15915494f) + 0.5f), _e619);
                                let _e625 = textureSampleLevel(global_3, global_2, vec2<f32>(_e620.x, clamp(_e619, 0.001953125f, 0.9980469f)), 0f);
                                let _e628 = global_1.member_24;
                                let _e629 = (_e628 == 2u);
                                let _e631 = global_1.member_8;
                                let _e633 = global_1.member_9;
                                let _e635 = global_1.member_10;
                                let _e637 = global_1.member_18;
                                let _e639 = global_1.member_23;
                                let _e641 = global_1.member_17;
                                let _e643 = global_1.member_13;
                                let _e647 = vec3(clamp(_e202, 0f, 1f));
                                let _e648 = mix((_e631 * max(_e633, 0f)), _e635, _e647);
                                phi_6570_ = _e625.xyz;
                                if (_e629 && (_e603 < 0f)) {
                                    phi_6570_ = mix(mix(_e648, _e639, vec3(clamp(_e637, 0f, 1f))), _e648, vec3(clamp((-(_e603) * max(_e641, 0.001f)), 0f, 1f)));
                                }
                                let _e661 = phi_6570_;
                                phi_6571_ = _e661;
                                if ((_e202 > 0f) && (_e603 > select(-1f, 0f, _e629))) {
                                    if _e629 {
                                        phi_6568_ = max(_e603, 0f);
                                    } else {
                                        phi_6568_ = abs(_e603);
                                    }
                                    let _e669 = phi_6568_;
                                    let _e681 = fract((((normalize(_e563) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e686 = (_e681 + vec3(dot(_e681, (_e681.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_6571_ = mix(_e661, (mix(mix(_e661, vec3<f32>(0.00018f, 0.00009f, 0.0006f), vec3(smoothstep(0f, 0.85f, _e669))), _e643, vec3((1f - smoothstep(0f, 0.55f, _e669)))) * (1f + ((fract(((_e686.x * _e686.y) * _e686.z)) - 0.5f) * 0.05f))), _e647);
                                }
                                let _e699 = phi_6571_;
                                phi_6572_ = _e699;
                                break;
                            }
                        }
                        let _e701 = phi_6572_;
                        if (_e209 < 0.5f) {
                            phi_6573_ = mix(_e424, _e562, vec3(smoothstep(0f, 0.5f, _e209)));
                        } else {
                            phi_6573_ = mix(_e562, _e701, vec3(smoothstep(0.5f, 1f, _e209)));
                        }
                        let _e710 = phi_6573_;
                        let _e712 = global_1.member_15;
                        let _e714 = global_1.member_16;
                        let _e723 = mix(_e710, _e562, vec3(clamp((1f - smoothstep(0f, max((mix(_e712, _e714, _e202) * 3f), 0.015f), abs(_e206))), 0f, 1f)));
                        let _e725 = global.member_8;
                        let _e726 = normalize(_e725);
                        let _e728 = global.member_9;
                        let _e729 = (_e728 > 0f);
                        phi_3307_ = _e729;
                        if _e729 {
                            phi_3307_ = (_e726.y > -0.18f);
                        }
                        let _e733 = phi_3307_;
                        phi_6587_ = _e723;
                        if (_e733 && (_e202 < 0.98f)) {
                            let _e737 = global.member_2;
                            let _e738 = normalize(_e737);
                            let _e740 = global.member_4;
                            let _e741 = normalize(_e740);
                            let _e744 = global.member_3;
                            let _e748 = max(((_e728 * max(_e744, 0.1f)) * 16f), 0.055f);
                            let _e750 = global.member_13;
                            if (_e750 > 0.5f) {
                                let _e754 = vec2<f32>(dot(_e726, _e738), dot(_e726, _e741));
                                let _e755 = length(_e754);
                                phi_6574_ = _e754;
                                if (_e755 > 0.82f) {
                                    phi_6574_ = (_e754 * (0.82f / _e755));
                                }
                                let _e760 = phi_6574_;
                                let _e767 = vec2<f32>(((_e188 - (_e760.x / _e197)) * _e197), (_e206 - _e760.y));
                                let _e769 = -(dot(_e767, _e767));
                                let _e770 = (_e748 * _e748);
                                phi_6579_ = exp((_e769 / max(_e770, 0.000001f)));
                                phi_6575_ = exp((_e769 / max((_e770 * 26f), 0.000001f)));
                            } else {
                                let _e784 = vec2<f32>(((_e188 - (dot(_e726, _e738) / _e197)) * _e197), (_e206 - dot(_e726, _e741)));
                                let _e786 = smoothstep(-0.05f, 0.05f, dot(_e726, normalize(_e252)));
                                let _e788 = -(dot(_e784, _e784));
                                let _e789 = (_e748 * _e748);
                                phi_6579_ = (exp((_e788 / max(_e789, 0.000001f))) * _e786);
                                phi_6575_ = (exp((_e788 / max((_e789 * 26f), 0.000001f))) * _e786);
                            }
                            let _e800 = phi_6579_;
                            let _e802 = phi_6575_;
                            let _e804 = smoothstep(-0.05f, 0.05f, _e726.y);
                            let _e805 = smoothstep(-0.006f, 0.006f, _e206);
                            let _e806 = (1f - _e202);
                            let _e810 = global.member_16;
                            let _e814 = global.member_17;
                            phi_6587_ = ((_e723 + (((mix(_e810, vec3<f32>(1f, 0.48f, 0.18f), vec3((1f - smoothstep(0.04f, 0.55f, _e726.y)))) * ((clamp((_e814 * 0.018f), 0f, 1.2f) * _e804) * _e806)) * _e802) * _e805)) + (((_e810 * (((_e814 * 0.4f) * _e804) * _e806)) * _e800) * _e805));
                        }
                        let _e831 = phi_6587_;
                        let _e834 = global.member_30[0u];
                        phi_6586_ = _e831;
                        if ((_e834 > 0.5f) && (_e202 > 0.01f)) {
                            let _e839 = global.member_2;
                            let _e842 = global.member_4;
                            let _e847 = global.member_30[1u];
                            let _e851 = global.member_30[2u];
                            let _e855 = global.member_30[3u];
                            let _e856 = clamp(_e855, 0.2f, 50f);
                            let _e859 = global.member_31[0u];
                            let _e860 = clamp(_e859, 0.2f, 4f);
                            let _e863 = global.member_31[1u];
                            let _e864 = clamp(_e863, 0.1f, 4f);
                            let _e867 = global.member_31[3u];
                            let _e868 = clamp(_e867, 0.1f, 4f);
                            phi_6588_ = _e831;
                            phi_6580_ = 0i;
                            loop {
                                let _e870 = phi_6588_;
                                let _e872 = phi_6580_;
                                local_6 = _e870;
                                if (_e872 < 8i) {
                                    let _e874 = f32(_e872);
                                    let _e880 = global.member_24;
                                    let _e887 = (((_e874 * 31.73f) + (floor(((_e880 * 0.017f) + (_e874 * 7.13f))) * 19.19f)) + 3.11f);
                                    let _e891 = fract(((vec3<f32>(_e887, 37f, 9f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e896 = (_e891 + vec3(dot(_e891, (_e891.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e912 = fract(((vec3<f32>(_e887, 2f, 5f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e917 = (_e912 + vec3(dot(_e912, (_e912.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e925 = fract(((_e880 / max((mix(8.5f, 1f, (clamp(_e851, 0f, 4f) * 0.25f)) * mix(0.58f, 1.72f, fract(((_e896.x * _e896.y) * _e896.z)))), 0.1f)) + fract(((_e917.x * _e917.y) * _e917.z))));
                                    let _e929 = fract(((vec3<f32>(_e887, 43f, 17f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e934 = (_e929 + vec3(dot(_e929, (_e929.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e941 = mix(0.55f, 1.65f, fract(((_e934.x * _e934.y) * _e934.z)));
                                    let _e945 = mix(0.42f, 0.13f, clamp(((_e856 * _e941) * 0.25f), 0f, 1f));
                                    let _e954 = fract(((vec3<f32>(_e887, 11f, 7f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e959 = (_e954 + vec3(dot(_e954, (_e954.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e969 = fract(((vec3<f32>(_e887, 41f, 29f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e974 = (_e969 + vec3(dot(_e969, (_e969.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e983 = ((fract(((_e959.x * _e959.y) * _e959.z)) + (0.23f * fract(((_e974.x * _e974.y) * _e974.z)))) * 6.2831855f);
                                    let _e987 = fract(((vec3<f32>(_e887, 13f, 3f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e992 = (_e987 + vec3(dot(_e987, (_e987.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e1000 = mix(0.12f, 0.96f, pow(fract(((_e992.x * _e992.y) * _e992.z)), 0.72f));
                                    let _e1004 = sqrt(max(0f, (1f - (_e1000 * _e1000))));
                                    let _e1010 = normalize(vec3<f32>((cos(_e983) * _e1004), _e1000, (sin(_e983) * _e1004)));
                                    let _e1012 = vec2<f32>((_e188 * _e197), _e206);
                                    let _e1016 = vec2<f32>((dot(_e1010, normalize(_e839)) / _e197), dot(_e1010, normalize(_e842)));
                                    let _e1018 = global.member_13;
                                    phi_6592_ = _e1016;
                                    if (_e1018 > 0.5f) {
                                        let _e1020 = length(_e1016);
                                        phi_6593_ = _e1016;
                                        if (_e1020 > 1.18f) {
                                            phi_6593_ = (_e1016 * (1.18f / _e1020));
                                        }
                                        let _e1025 = phi_6593_;
                                        phi_6592_ = _e1025;
                                    }
                                    let _e1027 = phi_6592_;
                                    let _e1031 = fract(((vec3<f32>(_e887, 71f, 5f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e1036 = (_e1031 + vec3(dot(_e1031, (_e1031.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e1043 = mix(-2.55f, -0.58f, fract(((_e1036.x * _e1036.y) * _e1036.z)));
                                    let _e1048 = normalize(vec2<f32>((cos(_e1043) / _e197), sin(_e1043)));
                                    let _e1057 = (vec2<f32>((_e1027.x * _e197), _e1027.y) + (_e1048 * (((_e925 * 0.58f) * _e856) * _e941)));
                                    let _e1061 = fract(((vec3<f32>(_e887, 53f, 11f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e1066 = (_e1061 + vec3(dot(_e1061, (_e1061.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e1073 = mix(0.55f, 1.75f, fract(((_e1066.x * _e1066.y) * _e1066.z)));
                                    let _e1077 = fract(((vec3<f32>(_e887, 61f, 13f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e1082 = (_e1077 + vec3(dot(_e1077, (_e1077.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e1095 = max((((0.3f * _e860) * _e1073) * smoothstep(0f, (_e945 * 0.55f), _e925)), 0.0001f);
                                    let _e1096 = (_e1012 - _e1057);
                                    let _e1098 = dot(_e1096, -(_e1048));
                                    let _e1099 = clamp(_e1098, 0f, _e1095);
                                    let _e1103 = length((_e1012 - (_e1057 - (_e1048 * _e1099))));
                                    let _e1108 = (105000f / max((_e864 * _e864), 0.01f));
                                    local_5 = (_e870 + ((((((((vec3<f32>(0.62f, 0.78f, 1f) * ((exp(((-(_e1103) * _e1103) * _e1108)) * exp((-(_e1099) * (20f / max((_e860 * _e1073), 0.2f))))) * (smoothstep(0f, 0.006f, _e1098) * (1f - smoothstep((_e1095 * 0.88f), _e1095, _e1098))))) * 0.85f) + ((vec3<f32>(1f, 0.96f, 0.82f) * exp((((-(dot(_e1096, _e1096)) * _e1108) * 0.42f) / max((_e868 * _e868), 0.01f)))) * 2.4f)) * (smoothstep(0f, 0.035f, _e925) * (1f - smoothstep((_e945 * 0.58f), _e945, _e925)))) * step((_e874 + 0.5f), (clamp(_e847, 0f, 1f) * 8f))) * smoothstep(-0.03f, 0.03f, dot(_e1010, normalize(_e252)))) * _e202) * mix(0.55f, 1.85f, fract(((_e1082.x * _e1082.y) * _e1082.z)))));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2132 = local_5;
                                    phi_6588_ = _e2132;
                                    phi_6580_ = (_e872 + 1i);
                                }
                            }
                            let _e2138 = local_6;
                            phi_6586_ = _e2138;
                        }
                        let _e1147 = phi_6586_;
                        let _e1149 = global.member_10;
                        let _e1150 = normalize(_e1149);
                        let _e1152 = smoothstep(-0.18f, 0.02f, _e1150.y);
                        let _e1154 = global.member_11;
                        phi_6590_ = _e1147;
                        if ((_e1154 > 0f) && (_e1152 > 0f)) {
                            let _e1159 = global.member_2;
                            let _e1162 = global.member_4;
                            let _e1166 = vec2<f32>(dot(_e1150, normalize(_e1159)), dot(_e1150, normalize(_e1162)));
                            let _e1168 = global.member_13;
                            phi_6581_ = _e1166;
                            if (_e1168 > 0.5f) {
                                let _e1170 = length(_e1166);
                                phi_6582_ = _e1166;
                                if (_e1170 > 0.82f) {
                                    phi_6582_ = (_e1166 * (0.82f / _e1170));
                                }
                                let _e1175 = phi_6582_;
                                phi_6581_ = _e1175;
                            }
                            let _e1177 = phi_6581_;
                            let _e1182 = global.member_12;
                            let _e1185 = global.member_5;
                            let _e1188 = max(((_e1182 * 6f) * max(_e1185, 0.1f)), 0.05f);
                            let _e1192 = vec2<f32>(((_e188 - (_e1177.x / _e197)) * _e197), (_e206 - _e1177.y));
                            let _e1197 = (smoothstep(1f, 0.92f, (length(_e1192) / _e1188)) * smoothstep(-0.006f, 0.006f, _e206));
                            phi_6591_ = _e1147;
                            if (_e1197 > 0f) {
                                let _e1202 = (((_e1192 / vec2(_e1188)) * 0.5f) + vec2<f32>(0.5f, 0.5f));
                                let _e1207 = vec2<f32>(_e1202.x, (1f - _e1202.y));
                                let _e1208 = textureSampleLevel(global_6, global_5, _e1207, 0f);
                                let _e1210 = global.member_14;
                                switch bitcast<i32>(0u) {
                                    default: {
                                        let _e1213 = ((_e1207 * 2f) - vec2<f32>(1f, 1f));
                                        let _e1214 = dot(_e1213, _e1213);
                                        if (_e1214 > 1f) {
                                            phi_6583_ = 0f;
                                            break;
                                        }
                                        let _e1221 = ((fract(_e1210) - 0.5f) * 6.2831855f);
                                        phi_6583_ = smoothstep(-0.04f, 0.04f, dot(vec3<f32>(_e1213.x, _e1213.y, sqrt(max(0f, (1f - _e1214)))), normalize(vec3<f32>(-(sin(_e1221)), 0f, cos(_e1221)))));
                                        break;
                                    }
                                }
                                let _e1233 = phi_6583_;
                                let _e1240 = global.member_15;
                                phi_6591_ = (_e1147 + ((((((_e1208.xyz * vec3<f32>(0.85f, 0.9f, 1.05f)) * ((_e1154 * exp2(_e1240)) * mix(0.2f, 1f, _e202))) * _e1197) * _e1208.w) * mix(0.08f, 1f, _e1233)) * _e1152));
                            }
                            let _e1251 = phi_6591_;
                            phi_6590_ = _e1251;
                        }
                        let _e1253 = phi_6590_;
                        let _e1255 = global.member_1;
                        let _e1257 = (_e1253 * exp2(_e1255));
                        global_4 = vec4<f32>(_e1257.x, _e1257.y, _e1257.z, 1f);
                        break;
                    }
                }
                break;
            }
            let _e1263 = global.member_2;
            let _e1266 = global.member_4;
            let _e1269 = global.member_6;
            let _e1275 = normalize(vec3<f32>(((_e188 * _e197) * _e194), (_e192 * _e194), -1f));
            let _e1285 = normalize((((normalize(_e1263) * _e1275.x) + (normalize(_e1266) * _e1275.y)) + (normalize(_e1269) * -(_e1275.z))));
            let _e1288 = global.member_32[3u];
            if (_e1288 >= 0.5f) {
                let _e1291 = max(_e1285.y, 0f);
                let _e1293 = max(-(_e1285.y), 0f);
                let _e1297 = global.member_32;
                let _e1301 = global.member_33;
                let _e1306 = global.member_34;
                let _e1311 = global.member_1;
                let _e1313 = ((((_e1297.xyz * _e1291) + (_e1301.xyz * ((1f - _e1291) - _e1293))) + (_e1306.xyz * _e1293)) * exp2(_e1311));
                global_4 = vec4<f32>(_e1313.x, _e1313.y, _e1313.z, 1f);
                break;
            }
            let _e1320 = global.member_29[3u];
            if (_e1320 >= 0f) {
                let _e1324 = global.member_29[2u];
                let _e1325 = sin(_e1324);
                let _e1326 = cos(_e1324);
                let _e1343 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e1325 * _e1285.x) + (_e1326 * _e1285.z)), ((_e1326 * _e1285.x) - (_e1325 * _e1285.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e1285.y, -1f, 1f)) * 0.31830987f)), 0f);
                let _e1347 = global.member_1;
                let _e1349 = ((_e1343.xyz * _e1320) * exp2(_e1347));
                global_4 = vec4<f32>(_e1349.x, _e1349.y, _e1349.z, 1f);
                break;
            }
            let _e1355 = global_1.member;
            let _e1357 = vec3<f32>(0f, -(_e1355), 0f);
            let _e1359 = global.member;
            let _e1360 = (_e1359 - _e1357);
            let _e1361 = length(_e1360);
            let _e1364 = (_e1360 / vec3(max(_e1361, 0.00001f)));
            let _e1365 = dot(_e1285, _e1364);
            let _e1367 = global.member_8;
            let _e1368 = normalize(_e1367);
            let _e1370 = global.member_23;
            let _e1371 = clamp(_e1370, 0f, 1f);
            let _e1378 = normalize(cross(_e1364, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e1364.z) > 0.98f)))));
            let _e1391 = ((((asin(clamp(_e1365, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
            let _e1397 = (0.5f + ((0.5f * sign(_e1391)) * sqrt(abs(_e1391))));
            let _e1398 = vec2<f32>(((atan2(dot(_e1285, normalize(cross(_e1378, _e1364))), dot(_e1285, _e1378)) * 0.15915494f) + 0.5f), _e1397);
            let _e1403 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1398.x, clamp(_e1397, 0.001953125f, 0.9980469f)), 0f);
            let _e1405 = dot(_e1285, _e1368);
            let _e1407 = global.member_9;
            let _e1411 = (_e1365 > 0f);
            phi_6480_ = 0f;
            if (((_e1407 > 0f) && (_e1405 > 0f)) && _e1411) {
                let _e1415 = max(_e1407, 0.000001f);
                let _e1416 = (_e1415 * 0.25f);
                phi_6480_ = ((clamp((1f - smoothstep((_e1415 - _e1416), (_e1415 + _e1416), acos(clamp(_e1405, -1f, 1f)))), 0f, 1f) * smoothstep(0f, 0.02f, _e1365)) * (1f - _e1371));
            }
            let _e1427 = phi_6480_;
            let _e1429 = (_e1360 / vec3(_e1361));
            let _e1432 = (_e1361 - _e1355);
            let _e1434 = global_1.member_1;
            let _e1442 = textureSampleLevel(global_9, global_8, vec2<f32>((0.5f * (clamp(dot(_e1285, _e1429), -1f, 1f) + 1f)), sqrt(clamp((_e1432 / (_e1434 - _e1355)), 0f, 1f))), 0f);
            let _e1445 = global.member_1;
            let _e1446 = exp2(_e1445);
            let _e1448 = global.member_17;
            let _e1449 = max(_e1407, 0.000001f);
            let _e1461 = global.member_16;
            let _e1466 = global.member_11;
            phi_6537_ = vec3<f32>(0f, 0f, 0f);
            if (_e1466 > 0f) {
                let _e1469 = global.member_10;
                let _e1470 = normalize(_e1469);
                let _e1471 = dot(_e1285, _e1470);
                let _e1473 = smoothstep(0f, 0.02f, dot(_e1470, _e1364));
                let _e1475 = global.member_12;
                let _e1478 = max(0.000001f, select(0.026f, _e1475, (_e1475 > 0f)));
                phi_6538_ = vec3<f32>(0f, 0f, 0f);
                if (((_e1471 > cos((_e1478 * 1.02f))) && _e1411) && (_e1473 > 0f)) {
                    let _e1497 = normalize(cross(select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 1f, 0f), vec3((abs(_e1470.y) < 0.99f))), _e1470));
                    let _e1500 = (_e1285 - (_e1470 * _e1471));
                    let _e1509 = vec2<f32>((((dot(_e1500, _e1497) / _e1478) * 0.5f) + 0.5f), (0.5f - ((dot(_e1500, cross(_e1470, _e1497)) / _e1478) * 0.5f)));
                    let _e1510 = textureSampleLevel(global_6, global_5, _e1509, 0f);
                    let _e1512 = global.member_14;
                    switch bitcast<i32>(0u) {
                        default: {
                            let _e1515 = ((_e1509 * 2f) - vec2<f32>(1f, 1f));
                            let _e1516 = dot(_e1515, _e1515);
                            if (_e1516 > 1f) {
                                phi_6482_ = 0f;
                                break;
                            }
                            let _e1523 = ((fract(_e1512) - 0.5f) * 6.2831855f);
                            phi_6482_ = smoothstep(-0.04f, 0.04f, dot(vec3<f32>(_e1515.x, _e1515.y, sqrt(max(0f, (1f - _e1516)))), normalize(vec3<f32>(-(sin(_e1523)), 0f, cos(_e1523)))));
                            break;
                        }
                    }
                    let _e1535 = phi_6482_;
                    let _e1547 = global.member_15;
                    phi_6538_ = (((_e1510.xyz * vec3<f32>(0.85f, 0.9f, 1.05f)) * ((_e1466 * exp2(_e1547)) * mix(0.15f, 1f, _e1371))) * (smoothstep(1f, 0.92f, (sqrt(max(0f, (2f * (1f - _e1471)))) / _e1478)) * (((smoothstep(0f, 0.02f, _e1365) * _e1473) * _e1510.w) * mix(0.08f, 1f, _e1535))));
                }
                let _e1554 = phi_6538_;
                phi_6537_ = _e1554;
            }
            let _e1556 = phi_6537_;
            let _e1559 = global.member_30[0u];
            phi_6524_ = vec3<f32>(0f, 0f, 0f);
            if ((_e1559 > 0.5f) && (_e1371 > 0.01f)) {
                let _e1565 = global.member_30[1u];
                let _e1569 = global.member_30[2u];
                let _e1573 = global.member_30[3u];
                let _e1574 = clamp(_e1573, 0.2f, 50f);
                let _e1577 = global.member_31[0u];
                let _e1578 = clamp(_e1577, 0.2f, 4f);
                let _e1581 = global.member_31[1u];
                let _e1582 = clamp(_e1581, 0.1f, 4f);
                let _e1585 = global.member_31[2u];
                let _e1586 = clamp(_e1585, 0.1f, 4f);
                phi_6525_ = vec3<f32>(0f, 0f, 0f);
                phi_6484_ = 0i;
                loop {
                    let _e1588 = phi_6525_;
                    let _e1590 = phi_6484_;
                    local_7 = _e1588;
                    if (_e1590 < 8i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1592 = f32(_e1590);
                        let _e1598 = global.member_24;
                        let _e1605 = (((_e1592 * 31.73f) + (floor(((_e1598 * 0.017f) + (_e1592 * 7.13f))) * 19.19f)) + 3.11f);
                        let _e1609 = fract(((vec3<f32>(_e1605, 37f, 9f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1614 = (_e1609 + vec3(dot(_e1609, (_e1609.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1630 = fract(((vec3<f32>(_e1605, 2f, 5f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1635 = (_e1630 + vec3(dot(_e1630, (_e1630.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1643 = fract(((_e1598 / max((mix(8.5f, 1f, (clamp(_e1569, 0f, 4f) * 0.25f)) * mix(0.58f, 1.72f, fract(((_e1614.x * _e1614.y) * _e1614.z)))), 0.1f)) + fract(((_e1635.x * _e1635.y) * _e1635.z))));
                        let _e1647 = fract(((vec3<f32>(_e1605, 43f, 17f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1652 = (_e1647 + vec3(dot(_e1647, (_e1647.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1659 = mix(0.55f, 1.65f, fract(((_e1652.x * _e1652.y) * _e1652.z)));
                        let _e1663 = mix(0.42f, 0.13f, clamp(((_e1574 * _e1659) * 0.25f), 0f, 1f));
                        let _e1672 = fract(((vec3<f32>(_e1605, 11f, 7f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1677 = (_e1672 + vec3(dot(_e1672, (_e1672.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1687 = fract(((vec3<f32>(_e1605, 41f, 29f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1692 = (_e1687 + vec3(dot(_e1687, (_e1687.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1701 = ((fract(((_e1677.x * _e1677.y) * _e1677.z)) + (0.23f * fract(((_e1692.x * _e1692.y) * _e1692.z)))) * 6.2831855f);
                        let _e1705 = fract(((vec3<f32>(_e1605, 13f, 3f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1710 = (_e1705 + vec3(dot(_e1705, (_e1705.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1718 = mix(0.12f, 0.96f, pow(fract(((_e1710.x * _e1710.y) * _e1710.z)), 0.72f));
                        let _e1722 = sqrt(max(0f, (1f - (_e1718 * _e1718))));
                        let _e1728 = normalize(vec3<f32>((cos(_e1701) * _e1722), _e1718, (sin(_e1701) * _e1722)));
                        let _e1732 = fract(((vec3<f32>(_e1605, 17f, 2f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1737 = (_e1732 + vec3(dot(_e1732, (_e1732.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1748 = fract(((vec3<f32>(_e1605, 23f, 31f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1753 = (_e1748 + vec3(dot(_e1748, (_e1748.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1764 = fract(((vec3<f32>(_e1605, 19f, 5f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1769 = (_e1764 + vec3(dot(_e1764, (_e1764.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1778 = normalize(vec3<f32>(mix(-0.9f, 0.9f, fract(((_e1737.x * _e1737.y) * _e1737.z))), mix(-1.35f, -0.45f, fract(((_e1753.x * _e1753.y) * _e1753.z))), mix(-0.9f, 0.9f, fract(((_e1769.x * _e1769.y) * _e1769.z)))));
                        let _e1782 = normalize((_e1778 - (_e1728 * dot(_e1778, _e1728))));
                        let _e1788 = normalize((_e1728 + (_e1782 * (((_e1643 * 0.8f) * _e1574) * _e1659))));
                        let _e1791 = (_e1285 - _e1788);
                        let _e1795 = fract(((vec3<f32>(_e1605, 53f, 11f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1800 = (_e1795 + vec3(dot(_e1795, (_e1795.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1807 = mix(0.55f, 1.75f, fract(((_e1800.x * _e1800.y) * _e1800.z)));
                        let _e1811 = fract(((vec3<f32>(_e1605, 61f, 13f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1816 = (_e1811 + vec3(dot(_e1811, (_e1811.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1829 = max((((0.1f * _e1578) * _e1807) * smoothstep(0f, (_e1663 * 0.55f), _e1643)), 0.0001f);
                        let _e1831 = dot(_e1791, -(_e1782));
                        let _e1832 = clamp(_e1831, 0f, _e1829);
                        let _e1836 = abs(dot((_e1791 + (_e1782 * _e1832)), normalize(cross(_e1788, _e1782))));
                        let _e1837 = dot(_e1285, _e1788);
                        let _e1838 = smoothstep(0.7648422f, 0.9393727f, _e1837);
                        phi_6525_ = (_e1588 + (((((((vec3<f32>(0.62f, 0.78f, 1f) * (((exp(((-(_e1836) * _e1836) * (340000f / max((_e1582 * _e1582), 0.01f)))) * exp((-(_e1832) * (44f / max((_e1578 * _e1807), 0.2f))))) * _e1838) * (smoothstep(0f, 0.002f, _e1831) * (1f - smoothstep((_e1829 * 0.88f), _e1829, _e1831))))) * 0.85f) + ((vec3<f32>(1f, 0.96f, 0.82f) * (exp(((max(0f, (1f - _e1837)) * -9000f) / max((_e1586 * _e1586), 0.01f))) * _e1838)) * 2.4f)) * (smoothstep(0f, 0.035f, _e1643) * (1f - smoothstep((_e1663 * 0.58f), _e1663, _e1643)))) * step((_e1592 + 0.5f), (clamp(_e1565, 0f, 1f) * 8f))) * _e1371) * mix(0.55f, 1.85f, fract(((_e1816.x * _e1816.y) * _e1816.z)))));
                        phi_6484_ = (_e1590 + 1i);
                    }
                }
                let _e2152 = local_7;
                phi_6524_ = _e2152;
            }
            let _e1880 = phi_6524_;
            let _e1882 = global_1.member_24;
            let _e1883 = (_e1882 == 2u);
            let _e1885 = global_1.member_8;
            let _e1887 = global_1.member_9;
            let _e1889 = global_1.member_10;
            let _e1891 = global_1.member_18;
            let _e1893 = global_1.member_23;
            let _e1895 = global_1.member_17;
            let _e1897 = global_1.member_13;
            let _e1898 = max(_e1887, 0f);
            let _e1901 = vec3(clamp(_e1371, 0f, 1f));
            let _e1902 = mix((_e1885 * _e1898), _e1889, _e1901);
            phi_6494_ = _e1403.xyz;
            if (_e1883 && (_e1365 < 0f)) {
                phi_6494_ = mix(mix(_e1902, _e1893, vec3(clamp(_e1891, 0f, 1f))), _e1902, vec3(clamp((-(_e1365) * max(_e1895, 0.001f)), 0f, 1f)));
            }
            let _e1915 = phi_6494_;
            phi_6495_ = _e1915;
            if ((_e1371 > 0f) && (_e1365 > select(-1f, 0f, _e1883))) {
                if _e1883 {
                    phi_6492_ = max(_e1365, 0f);
                } else {
                    phi_6492_ = abs(_e1365);
                }
                let _e1923 = phi_6492_;
                let _e1935 = fract((((normalize(_e1285) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                let _e1940 = (_e1935 + vec3(dot(_e1935, (_e1935.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                phi_6495_ = mix(_e1915, (mix(mix(_e1915, vec3<f32>(0.00018f, 0.00009f, 0.0006f), vec3(smoothstep(0f, 0.85f, _e1923))), _e1897, vec3((1f - smoothstep(0f, 0.55f, _e1923)))) * (1f + ((fract(((_e1940.x * _e1940.y) * _e1940.z)) - 0.5f) * 0.05f))), _e1901);
            }
            let _e1953 = phi_6495_;
            phi_6518_ = _e1953;
            if ((_e1882 == 1u) && (_e1365 < 0.005f)) {
                phi_6497_ = vec3<f32>(0f, 0f, 0f);
                phi_6496_ = 0i;
                loop {
                    let _e1958 = phi_6497_;
                    let _e1960 = phi_6496_;
                    local_4 = _e1958;
                    if (_e1960 < 16i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1962 = f32(_e1960);
                        let _e1964 = ((_e1962 + 0.5f) * 0.0625f);
                        let _e1965 = sqrt(_e1964);
                        let _e1969 = (_e1962 * 2.3999631f);
                        let _e1982 = ((((asin(clamp(sqrt(max((1f - _e1964), 0f)), -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                        let _e1988 = (0.5f + ((0.5f * sign(_e1982)) * sqrt(abs(_e1982))));
                        let _e1989 = vec2<f32>(((atan2((_e1965 * sin(_e1969)), (_e1965 * cos(_e1969))) * 0.15915494f) + 0.5f), _e1988);
                        let _e1994 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1989.x, clamp(_e1988, 0.001953125f, 0.9980469f)), 0f);
                        phi_6497_ = (_e1958 + _e1994.xyz);
                        phi_6496_ = (_e1960 + 1i);
                    }
                }
                let _e1999 = local_4;
                let _e2002 = global_1.member_4;
                let _e2004 = global_1.member_2;
                let _e2006 = global_1.member_25;
                switch bitcast<i32>(0u) {
                    default: {
                        if (_e1432 <= (_e1355 * 0.0000008f)) {
                            phi_6516_ = 0f;
                            phi_6515_ = select(vec3<f32>(0f, 1f, 0f), _e1429, vec3((_e1361 > 0f)));
                        } else {
                            switch bitcast<i32>(0u) {
                                default: {
                                    let _e2014 = (_e1357 - _e1359);
                                    let _e2015 = dot(_e2014, _e1285);
                                    let _e2017 = (_e2014 - (_e1285 * _e2015));
                                    let _e2018 = dot(_e2017, _e2017);
                                    let _e2019 = (_e1355 * _e1355);
                                    if (_e2018 > _e2019) {
                                        phi_6500_ = f32();
                                        phi_6499_ = false;
                                        break;
                                    }
                                    phi_6500_ = (_e2015 - sqrt(max((_e2019 - _e2018), 0f)));
                                    phi_6499_ = true;
                                    break;
                                }
                            }
                            let _e2026 = phi_6500_;
                            let _e2028 = phi_6499_;
                            if (!(_e2028) || (_e2026 <= 0f)) {
                                phi_6517_ = _e1953;
                                break;
                            }
                            phi_6516_ = _e2026;
                            phi_6515_ = normalize(((_e1359 + (_e1285 * _e2026)) - _e1357));
                        }
                        let _e2037 = phi_6516_;
                        let _e2039 = phi_6515_;
                        let _e2040 = dot(_e2039, _e1368);
                        let _e2044 = textureSampleLevel(global_9, global_8, vec2<f32>((0.5f + (0.5f * _e2040)), 0f), 0f);
                        phi_6517_ = mix(_e1953, mix(_e1953, mix((((((((_e1885 * 0.31830987f) * max(_e2040, 0f)) * _e2044.xyz) * _e1461) * _e1448) + (_e1885 * (_e1999 * vec3<f32>(0.0625f, 0.0625f, 0.0625f)))) * _e1898), _e1889, _e1901), vec3(exp((-(_e2037) * ((dot(_e2002, vec3<f32>(0.299f, 0.587f, 0.114f)) * exp((-(_e1432) / _e2004))) * max(_e2006, 0f)))))), vec3(smoothstep(-0.004f, 0f, -(_e1365))));
                        break;
                    }
                }
                let _e2073 = phi_6517_;
                phi_6518_ = _e2073;
            }
            let _e2075 = phi_6518_;
            phi_6519_ = _e2075;
            if _e1883 {
                let _e2077 = global_1.member_15;
                let _e2079 = global_1.member_16;
                let _e2086 = global_1.member_11;
                let _e2088 = global_1.member_12;
                phi_6519_ = mix(_e2075, mix(_e2086, _e2088, vec3(_e1371)), vec3(clamp((1f - smoothstep(0f, max(mix(_e2077, _e2079, _e1371), 0.00001f), abs(_e1365))), 0f, 1f)));
            }
            let _e2095 = phi_6519_;
            let _e2099 = ((((_e2095 + _e1880) + (((_e1461 * min((_e1448 / (3.1415927f * ((_e1449 * _e1449) + ((_e1449 * 0.05f) * (_e1449 * 0.25f))))), (32752f / max(_e1446, 0.000001f)))) * _e1427) * _e1442.xyz)) + _e1556) * _e1446);
            global_4 = vec4<f32>(_e2099.x, _e2099.y, _e2099.z, 1f);
            break;
        }
    }
    return;
}

@fragment
fn main(@location(0) param: vec2<f32>) -> @location(0) vec4<f32> {
    global_7 = param;
    function_();
    let _e3 = global_4;
    return _e3;
}
