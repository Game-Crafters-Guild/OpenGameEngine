struct type_7 {
    member: array<vec4<f32>>,
}

struct type_12 {
    member: array<u32>,
}

struct type_21 {
    member: u32,
    member_1: u32,
    member_2: u32,
}

struct type_25 {
    member: u32,
    member_1: u32,
    member_2: f32,
    member_3: u32,
    member_4: u32,
    member_5: array<u32, 3>,
}

struct type_27 {
    member: array<type_25>,
}

struct type_32 {
    member: u32,
    member_1: u32,
    member_2: u32,
    member_3: u32,
    member_4: u32,
    member_5: u32,
    member_6: u32,
    member_7: u32,
    member_8: u32,
    member_9: u32,
    member_10: array<u32, 2>,
    member_11: mat4x4<f32>,
    member_12: array<u32, 4>,
}

struct type_34 {
    member: array<type_32>,
}

struct type_38 {
    member: f32,
    member_1: u32,
    member_2: u32,
    member_3: u32,
    member_4: u32,
    member_5: u32,
    member_6: u32,
    member_7: array<u32, 9>,
}

struct type_40 {
    member: array<type_38>,
}

@group(0) @binding(5)
var<storage, read_write> global: type_7;
@group(0) @binding(1)
var<storage> global_1: type_12;
@group(0) @binding(3)
var<storage> global_2: type_12;
var<workgroup> global_3: array<vec4<f32>, 768>;
var<private> global_4: vec3<u32>;
@group(3) @binding(0)
var<uniform> global_5: type_21;
@group(0) @binding(4)
var<storage> global_6: type_27;
@group(0) @binding(6)
var<storage> global_7: type_12;
@group(0) @binding(0)
var<storage> global_8: type_34;
@group(0) @binding(2)
var<storage> global_9: type_40;
var<private> global_10: u32;

fn function_() {
    var phi_1462_: bool;
    var phi_1470_: bool;
    var phi_4592_: bool;
    var phi_4596_: u32;
    var phi_4597_: u32;
    var phi_1691_: bool;
    var phi_4626_: vec3<f32>;
    var phi_4623_: vec4<f32>;
    var phi_4620_: vec3<f32>;
    var phi_4614_: bool;
    var phi_4611_: u32;
    var phi_4639_: u32;
    var local: u32;
    var phi_4641_: u32;
    var phi_4642_: u32;
    var phi_4644_: f32;
    var phi_4651_: vec3<f32>;
    var phi_4650_: vec3<f32>;
    var phi_4646_: vec4<f32>;
    var phi_4645_: f32;
    var phi_4647_: vec4<f32>;
    var phi_4675_: vec4<f32>;
    var phi_4674_: vec4<f32>;
    var phi_4693_: vec3<f32>;
    var phi_4692_: vec3<f32>;
    var phi_4687_: vec3<f32>;
    var phi_4673_: vec4<f32>;
    var phi_4679_: vec3<f32>;
    var phi_4665_: vec4<f32>;
    var phi_4649_: vec3<f32>;
    var phi_4694_: bool;
    var phi_4678_: vec3<f32>;
    var phi_4664_: vec4<f32>;
    var phi_4648_: vec3<f32>;
    var phi_4624_: vec3<f32>;
    var phi_4621_: vec4<f32>;
    var phi_4618_: vec3<f32>;
    var phi_4612_: bool;
    var phi_4637_: mat4x4<f32>;
    var phi_4600_: u32;
    var phi_4601_: u32;
    var phi_4607_: u32;
    var phi_4605_: u32;
    var local_1: vec3<f32>;
    var local_2: vec4<f32>;
    var local_3: vec3<f32>;
    var local_4: bool;
    var local_5: u32;
    var local_6: vec3<f32>;
    var local_7: vec4<f32>;
    var local_8: vec3<f32>;
    var local_9: bool;

    let _e57 = global_4[0u];
    let _e59 = global_5.member;
    let _e60 = (_e57 < _e59);
    let _e63 = min(_e57, (max(_e59, 1u) - 1u));
    let _e66 = global_6.member[_e63];
    let _e73 = global_5.member_1;
    let _e74 = (_e73 != 0u);
    phi_1462_ = _e74;
    if _e74 {
        phi_1462_ = (_e66.member_4 != 0u);
    }
    let _e77 = phi_1462_;
    phi_1470_ = _e77;
    if _e77 {
        let _e79 = global_5.member_2;
        phi_1470_ = (_e66.member_4 < _e79);
    }
    let _e82 = phi_1470_;
    phi_4592_ = _e60;
    if _e82 {
        let _e85 = global_7.member[_e66.member_4];
        phi_4592_ = select(_e60, false, (_e85 == 0u));
    }
    let _e89 = phi_4592_;
    let _e92 = global_8.member[_e66.member];
    let _e105 = global_9.member[_e66.member_1];
    if _e89 {
        phi_4596_ = min(_e92.member, 256u);
    } else {
        phi_4596_ = 0u;
    }
    let _e114 = phi_4596_;
    let _e115 = global_10;
    phi_4597_ = _e115;
    loop {
        let _e117 = phi_4597_;
        if (_e117 < _e114) {
            let _e120 = (_e92.member_4 + (_e117 * 10u));
            let _e123 = global_1.member[_e120];
            let _e128 = global_1.member[(_e120 + 1u)];
            let _e133 = global_1.member[(_e120 + 2u)];
            let _e135 = vec3<f32>(bitcast<f32>(_e123), bitcast<f32>(_e128), bitcast<f32>(_e133));
            let _e139 = global_1.member[(_e120 + 3u)];
            let _e144 = global_1.member[(_e120 + 4u)];
            let _e149 = global_1.member[(_e120 + 5u)];
            let _e154 = global_1.member[(_e120 + 6u)];
            let _e156 = vec4<f32>(bitcast<f32>(_e139), bitcast<f32>(_e144), bitcast<f32>(_e149), bitcast<f32>(_e154));
            let _e160 = global_1.member[(_e120 + 7u)];
            let _e165 = global_1.member[(_e120 + 8u)];
            let _e170 = global_1.member[(_e120 + 9u)];
            let _e172 = vec3<f32>(bitcast<f32>(_e160), bitcast<f32>(_e165), bitcast<f32>(_e170));
            let _e176 = global_1.member[(_e92.member_9 + _e117)];
            let _e177 = (_e105.member_1 > 0u);
            phi_1691_ = _e177;
            if _e177 {
                phi_1691_ = (_e176 < _e105.member_6);
            }
            let _e180 = phi_1691_;
            phi_4624_ = _e172;
            phi_4621_ = _e156;
            phi_4618_ = _e135;
            phi_4612_ = false;
            if _e180 {
                let _e184 = global_2.member[(_e105.member_4 + _e176)];
                let _e188 = global_2.member[(_e105.member_5 + _e176)];
                phi_4626_ = _e172;
                phi_4623_ = _e156;
                phi_4620_ = _e135;
                phi_4614_ = false;
                phi_4611_ = 0u;
                loop {
                    let _e190 = phi_4626_;
                    let _e192 = phi_4623_;
                    let _e194 = phi_4620_;
                    let _e196 = phi_4614_;
                    let _e198 = phi_4611_;
                    local_6 = _e190;
                    local_7 = _e192;
                    local_8 = _e194;
                    local_9 = _e196;
                    if (_e198 < _e188) {
                        let _e202 = (_e105.member_2 + ((_e184 + _e198) * 4u));
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e207 = global_2.member[(_e202 + 1u)];
                                let _e211 = global_2.member[(_e202 + 2u)];
                                let _e215 = global_2.member[(_e202 + 3u)];
                                if (_e215 == 0u) {
                                    phi_4694_ = _e196;
                                    phi_4678_ = _e190;
                                    phi_4664_ = _e192;
                                    phi_4648_ = _e194;
                                    break;
                                }
                                let _e217 = (_e207 & 3u);
                                let _e220 = ((_e207 >> bitcast<u32>(2i)) & 3u);
                                let _e221 = (_e105.member_3 + _e211);
                                phi_4639_ = 0u;
                                loop {
                                    let _e223 = phi_4639_;
                                    local = _e223;
                                    local_5 = _e223;
                                    if (_e223 < _e215) {
                                        let _e229 = global_2.member[(_e221 + (_e223 * 28u))];
                                        if (bitcast<f32>(_e229) >= _e66.member_2) {
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_4639_ = (_e223 + bitcast<u32>(1i));
                                    }
                                }
                                let _e235 = local;
                                let _e1326 = local_5;
                                phi_4641_ = _e1326;
                                if (_e235 >= _e215) {
                                    phi_4641_ = (_e215 - 1u);
                                }
                                let _e239 = phi_4641_;
                                if (_e239 == 0u) {
                                    phi_4642_ = 0u;
                                } else {
                                    phi_4642_ = (_e239 - 1u);
                                }
                                let _e243 = phi_4642_;
                                let _e245 = (_e221 + (_e243 * 28u));
                                let _e247 = (_e221 + (_e239 * 28u));
                                let _e250 = global_2.member[_e245];
                                let _e251 = bitcast<f32>(_e250);
                                let _e254 = global_2.member[_e247];
                                let _e256 = (bitcast<f32>(_e254) - _e251);
                                if (_e256 > 0f) {
                                    phi_4644_ = clamp(((_e66.member_2 - _e251) / _e256), 0f, 1f);
                                } else {
                                    phi_4644_ = 0f;
                                }
                                let _e262 = phi_4644_;
                                let _e265 = ((_e220 == 0u) || (_e256 <= 0f));
                                if (_e217 == 0u) {
                                    let _e270 = global_2.member[(_e245 + 1u)];
                                    let _e271 = bitcast<f32>(_e270);
                                    let _e275 = global_2.member[(_e245 + 2u)];
                                    let _e276 = bitcast<f32>(_e275);
                                    let _e280 = global_2.member[(_e245 + 3u)];
                                    let _e281 = bitcast<f32>(_e280);
                                    let _e282 = vec3<f32>(_e271, _e276, _e281);
                                    if _e265 {
                                        phi_4650_ = _e282;
                                    } else {
                                        if (_e220 == 2u) {
                                            let _e287 = global_2.member[(_e247 + 1u)];
                                            let _e288 = bitcast<f32>(_e287);
                                            let _e292 = global_2.member[(_e247 + 2u)];
                                            let _e293 = bitcast<f32>(_e292);
                                            let _e297 = global_2.member[(_e247 + 3u)];
                                            let _e298 = bitcast<f32>(_e297);
                                            let _e302 = global_2.member[(_e245 + 15u)];
                                            let _e307 = global_2.member[(_e245 + 23u)];
                                            let _e312 = global_2.member[(_e247 + 11u)];
                                            let _e317 = global_2.member[(_e247 + 19u)];
                                            let _e327 = (_e262 * _e262);
                                            let _e328 = (_e327 * _e262);
                                            let _e329 = (1f - _e262);
                                            let _e330 = (_e329 * _e329);
                                            let _e331 = (_e330 * _e329);
                                            let _e334 = ((3f * _e330) * _e262);
                                            let _e338 = ((3f * _e329) * _e327);
                                            let _e346 = global_2.member[(_e245 + 16u)];
                                            let _e351 = global_2.member[(_e245 + 24u)];
                                            let _e356 = global_2.member[(_e247 + 12u)];
                                            let _e361 = global_2.member[(_e247 + 20u)];
                                            let _e381 = global_2.member[(_e245 + 17u)];
                                            let _e386 = global_2.member[(_e245 + 25u)];
                                            let _e391 = global_2.member[(_e247 + 13u)];
                                            let _e396 = global_2.member[(_e247 + 21u)];
                                            phi_4651_ = vec3<f32>(((((_e331 * _e271) + (_e334 * (_e271 + (bitcast<f32>(_e302) * (_e256 * clamp(bitcast<f32>(_e307), 0.05f, 0.49f)))))) + (_e338 * (_e288 - (bitcast<f32>(_e312) * (_e256 * clamp(bitcast<f32>(_e317), 0.05f, 0.49f)))))) + (_e328 * _e288)), ((((_e331 * _e276) + (_e334 * (_e276 + (bitcast<f32>(_e346) * (_e256 * clamp(bitcast<f32>(_e351), 0.05f, 0.49f)))))) + (_e338 * (_e293 - (bitcast<f32>(_e356) * (_e256 * clamp(bitcast<f32>(_e361), 0.05f, 0.49f)))))) + (_e328 * _e293)), ((((_e331 * _e281) + (_e334 * (_e281 + (bitcast<f32>(_e381) * (_e256 * clamp(bitcast<f32>(_e386), 0.05f, 0.49f)))))) + (_e338 * (_e298 - (bitcast<f32>(_e391) * (_e256 * clamp(bitcast<f32>(_e396), 0.05f, 0.49f)))))) + (_e328 * _e298)));
                                        } else {
                                            let _e417 = global_2.member[(_e247 + 1u)];
                                            let _e422 = global_2.member[(_e247 + 2u)];
                                            let _e427 = global_2.member[(_e247 + 3u)];
                                            phi_4651_ = mix(_e282, vec3<f32>(bitcast<f32>(_e417), bitcast<f32>(_e422), bitcast<f32>(_e427)), vec3(_e262));
                                        }
                                        let _e433 = phi_4651_;
                                        phi_4650_ = _e433;
                                    }
                                    let _e435 = phi_4650_;
                                    phi_4679_ = _e190;
                                    phi_4665_ = _e192;
                                    phi_4649_ = _e435;
                                } else {
                                    if (_e217 == 1u) {
                                        let _e440 = global_2.member[(_e245 + 4u)];
                                        let _e441 = bitcast<f32>(_e440);
                                        let _e445 = global_2.member[(_e245 + 5u)];
                                        let _e446 = bitcast<f32>(_e445);
                                        let _e450 = global_2.member[(_e245 + 6u)];
                                        let _e451 = bitcast<f32>(_e450);
                                        let _e455 = global_2.member[(_e245 + 7u)];
                                        let _e456 = bitcast<f32>(_e455);
                                        let _e457 = vec4<f32>(_e441, _e446, _e451, _e456);
                                        if _e265 {
                                            phi_4674_ = _e457;
                                        } else {
                                            if (_e220 == 2u) {
                                                let _e462 = global_2.member[(_e247 + 4u)];
                                                let _e463 = bitcast<f32>(_e462);
                                                let _e467 = global_2.member[(_e247 + 5u)];
                                                let _e468 = bitcast<f32>(_e467);
                                                let _e472 = global_2.member[(_e247 + 6u)];
                                                let _e473 = bitcast<f32>(_e472);
                                                let _e477 = global_2.member[(_e247 + 7u)];
                                                let _e478 = bitcast<f32>(_e477);
                                                let _e482 = global_2.member[(_e245 + 15u)];
                                                let _e487 = global_2.member[(_e245 + 23u)];
                                                let _e492 = global_2.member[(_e247 + 11u)];
                                                let _e497 = global_2.member[(_e247 + 19u)];
                                                let _e507 = (_e262 * _e262);
                                                let _e508 = (_e507 * _e262);
                                                let _e509 = (1f - _e262);
                                                let _e510 = (_e509 * _e509);
                                                let _e511 = (_e510 * _e509);
                                                let _e514 = ((3f * _e510) * _e262);
                                                let _e518 = ((3f * _e509) * _e507);
                                                let _e526 = global_2.member[(_e245 + 16u)];
                                                let _e531 = global_2.member[(_e245 + 24u)];
                                                let _e536 = global_2.member[(_e247 + 12u)];
                                                let _e541 = global_2.member[(_e247 + 20u)];
                                                let _e561 = global_2.member[(_e245 + 17u)];
                                                let _e566 = global_2.member[(_e245 + 25u)];
                                                let _e571 = global_2.member[(_e247 + 13u)];
                                                let _e576 = global_2.member[(_e247 + 21u)];
                                                let _e596 = global_2.member[(_e245 + 18u)];
                                                let _e601 = global_2.member[(_e245 + 26u)];
                                                let _e606 = global_2.member[(_e247 + 14u)];
                                                let _e611 = global_2.member[(_e247 + 22u)];
                                                phi_4675_ = normalize(vec4<f32>(((((_e511 * _e441) + (_e514 * (_e441 + (bitcast<f32>(_e482) * (_e256 * clamp(bitcast<f32>(_e487), 0.05f, 0.49f)))))) + (_e518 * (_e463 - (bitcast<f32>(_e492) * (_e256 * clamp(bitcast<f32>(_e497), 0.05f, 0.49f)))))) + (_e508 * _e463)), ((((_e511 * _e446) + (_e514 * (_e446 + (bitcast<f32>(_e526) * (_e256 * clamp(bitcast<f32>(_e531), 0.05f, 0.49f)))))) + (_e518 * (_e468 - (bitcast<f32>(_e536) * (_e256 * clamp(bitcast<f32>(_e541), 0.05f, 0.49f)))))) + (_e508 * _e468)), ((((_e511 * _e451) + (_e514 * (_e451 + (bitcast<f32>(_e561) * (_e256 * clamp(bitcast<f32>(_e566), 0.05f, 0.49f)))))) + (_e518 * (_e473 - (bitcast<f32>(_e571) * (_e256 * clamp(bitcast<f32>(_e576), 0.05f, 0.49f)))))) + (_e508 * _e473)), ((((_e511 * _e456) + (_e514 * (_e456 + (bitcast<f32>(_e596) * (_e256 * clamp(bitcast<f32>(_e601), 0.05f, 0.49f)))))) + (_e518 * (_e478 - (bitcast<f32>(_e606) * (_e256 * clamp(bitcast<f32>(_e611), 0.05f, 0.49f)))))) + (_e508 * _e478))));
                                            } else {
                                                let _e633 = global_2.member[(_e247 + 4u)];
                                                let _e638 = global_2.member[(_e247 + 5u)];
                                                let _e643 = global_2.member[(_e247 + 6u)];
                                                let _e648 = global_2.member[(_e247 + 7u)];
                                                let _e650 = vec4<f32>(bitcast<f32>(_e633), bitcast<f32>(_e638), bitcast<f32>(_e643), bitcast<f32>(_e648));
                                                switch bitcast<i32>(0u) {
                                                    default: {
                                                        let _e652 = dot(_e457, _e650);
                                                        phi_4646_ = _e650;
                                                        phi_4645_ = _e652;
                                                        if (_e652 < 0f) {
                                                            phi_4646_ = -(_e650);
                                                            phi_4645_ = -(_e652);
                                                        }
                                                        let _e657 = phi_4646_;
                                                        let _e659 = phi_4645_;
                                                        if (_e659 > 0.9995f) {
                                                            phi_4647_ = normalize(mix(_e457, _e657, vec4(_e262)));
                                                            break;
                                                        }
                                                        let _e665 = acos(clamp(_e659, -1f, 1f));
                                                        let _e666 = sin(_e665);
                                                        phi_4647_ = ((_e457 * (sin(((1f - _e262) * _e665)) / _e666)) + (_e657 * (sin((_e262 * _e665)) / _e666)));
                                                        break;
                                                    }
                                                }
                                                let _e678 = phi_4647_;
                                                phi_4675_ = _e678;
                                            }
                                            let _e680 = phi_4675_;
                                            phi_4674_ = _e680;
                                        }
                                        let _e682 = phi_4674_;
                                        phi_4687_ = _e190;
                                        phi_4673_ = _e682;
                                    } else {
                                        let _e686 = global_2.member[(_e245 + 8u)];
                                        let _e687 = bitcast<f32>(_e686);
                                        let _e691 = global_2.member[(_e245 + 9u)];
                                        let _e692 = bitcast<f32>(_e691);
                                        let _e696 = global_2.member[(_e245 + 10u)];
                                        let _e697 = bitcast<f32>(_e696);
                                        let _e698 = vec3<f32>(_e687, _e692, _e697);
                                        if _e265 {
                                            phi_4692_ = _e698;
                                        } else {
                                            if (_e220 == 2u) {
                                                let _e703 = global_2.member[(_e247 + 8u)];
                                                let _e704 = bitcast<f32>(_e703);
                                                let _e708 = global_2.member[(_e247 + 9u)];
                                                let _e709 = bitcast<f32>(_e708);
                                                let _e713 = global_2.member[(_e247 + 10u)];
                                                let _e714 = bitcast<f32>(_e713);
                                                let _e718 = global_2.member[(_e245 + 15u)];
                                                let _e723 = global_2.member[(_e245 + 23u)];
                                                let _e728 = global_2.member[(_e247 + 11u)];
                                                let _e733 = global_2.member[(_e247 + 19u)];
                                                let _e743 = (_e262 * _e262);
                                                let _e744 = (_e743 * _e262);
                                                let _e745 = (1f - _e262);
                                                let _e746 = (_e745 * _e745);
                                                let _e747 = (_e746 * _e745);
                                                let _e750 = ((3f * _e746) * _e262);
                                                let _e754 = ((3f * _e745) * _e743);
                                                let _e762 = global_2.member[(_e245 + 16u)];
                                                let _e767 = global_2.member[(_e245 + 24u)];
                                                let _e772 = global_2.member[(_e247 + 12u)];
                                                let _e777 = global_2.member[(_e247 + 20u)];
                                                let _e797 = global_2.member[(_e245 + 17u)];
                                                let _e802 = global_2.member[(_e245 + 25u)];
                                                let _e807 = global_2.member[(_e247 + 13u)];
                                                let _e812 = global_2.member[(_e247 + 21u)];
                                                phi_4693_ = vec3<f32>(((((_e747 * _e687) + (_e750 * (_e687 + (bitcast<f32>(_e718) * (_e256 * clamp(bitcast<f32>(_e723), 0.05f, 0.49f)))))) + (_e754 * (_e704 - (bitcast<f32>(_e728) * (_e256 * clamp(bitcast<f32>(_e733), 0.05f, 0.49f)))))) + (_e744 * _e704)), ((((_e747 * _e692) + (_e750 * (_e692 + (bitcast<f32>(_e762) * (_e256 * clamp(bitcast<f32>(_e767), 0.05f, 0.49f)))))) + (_e754 * (_e709 - (bitcast<f32>(_e772) * (_e256 * clamp(bitcast<f32>(_e777), 0.05f, 0.49f)))))) + (_e744 * _e709)), ((((_e747 * _e697) + (_e750 * (_e697 + (bitcast<f32>(_e797) * (_e256 * clamp(bitcast<f32>(_e802), 0.05f, 0.49f)))))) + (_e754 * (_e714 - (bitcast<f32>(_e807) * (_e256 * clamp(bitcast<f32>(_e812), 0.05f, 0.49f)))))) + (_e744 * _e714)));
                                            } else {
                                                let _e833 = global_2.member[(_e247 + 8u)];
                                                let _e838 = global_2.member[(_e247 + 9u)];
                                                let _e843 = global_2.member[(_e247 + 10u)];
                                                phi_4693_ = mix(_e698, vec3<f32>(bitcast<f32>(_e833), bitcast<f32>(_e838), bitcast<f32>(_e843)), vec3(_e262));
                                            }
                                            let _e849 = phi_4693_;
                                            phi_4692_ = _e849;
                                        }
                                        let _e851 = phi_4692_;
                                        phi_4687_ = _e851;
                                        phi_4673_ = _e192;
                                    }
                                    let _e853 = phi_4687_;
                                    let _e855 = phi_4673_;
                                    phi_4679_ = _e853;
                                    phi_4665_ = _e855;
                                    phi_4649_ = _e194;
                                }
                                let _e857 = phi_4679_;
                                let _e859 = phi_4665_;
                                let _e861 = phi_4649_;
                                phi_4694_ = true;
                                phi_4678_ = _e857;
                                phi_4664_ = _e859;
                                phi_4648_ = _e861;
                                break;
                            }
                        }
                        let _e863 = phi_4694_;
                        let _e865 = phi_4678_;
                        let _e867 = phi_4664_;
                        let _e869 = phi_4648_;
                        local_1 = _e865;
                        local_2 = _e867;
                        local_3 = _e869;
                        local_4 = _e863;
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1311 = local_1;
                        phi_4626_ = _e1311;
                        let _e1314 = local_2;
                        phi_4623_ = _e1314;
                        let _e1317 = local_3;
                        phi_4620_ = _e1317;
                        let _e1320 = local_4;
                        phi_4614_ = _e1320;
                        phi_4611_ = (_e198 + bitcast<u32>(1i));
                    }
                }
                let _e1349 = local_6;
                phi_4624_ = _e1349;
                let _e1352 = local_7;
                phi_4621_ = _e1352;
                let _e1355 = local_8;
                phi_4618_ = _e1355;
                let _e1358 = local_9;
                phi_4612_ = _e1358;
            }
            let _e873 = phi_4624_;
            let _e875 = phi_4621_;
            let _e877 = phi_4618_;
            let _e879 = phi_4612_;
            if _e879 {
                let _e884 = (_e875.x + _e875.x);
                let _e885 = (_e875.y + _e875.y);
                let _e886 = (_e875.z + _e875.z);
                let _e887 = (_e875.x * _e884);
                let _e888 = (_e875.x * _e885);
                let _e889 = (_e875.x * _e886);
                let _e890 = (_e875.y * _e885);
                let _e891 = (_e875.y * _e886);
                let _e892 = (_e875.z * _e886);
                let _e893 = (_e875.w * _e884);
                let _e894 = (_e875.w * _e885);
                let _e895 = (_e875.w * _e886);
                let _e912 = (vec3<f32>((1f - (_e890 + _e892)), (_e888 + _e895), (_e889 - _e894)) * _e873.x);
                let _e918 = (vec3<f32>((_e888 - _e895), (1f - (_e887 + _e892)), (_e891 + _e893)) * _e873.y);
                let _e924 = (vec3<f32>((_e889 + _e894), (_e891 - _e893), (1f - (_e887 + _e890))) * _e873.z);
                phi_4637_ = mat4x4<f32>(vec4<f32>(_e912.x, _e912.y, _e912.z, 0f), vec4<f32>(_e918.x, _e918.y, _e918.z, 0f), vec4<f32>(_e924.x, _e924.y, _e924.z, 0f), vec4<f32>(_e877.x, _e877.y, _e877.z, 1f));
            } else {
                let _e935 = (_e92.member_5 + (_e117 * 16u));
                let _e938 = global_1.member[_e935];
                let _e943 = global_1.member[(_e935 + 1u)];
                let _e948 = global_1.member[(_e935 + 2u)];
                let _e953 = global_1.member[(_e935 + 3u)];
                let _e958 = global_1.member[(_e935 + 4u)];
                let _e963 = global_1.member[(_e935 + 5u)];
                let _e968 = global_1.member[(_e935 + 6u)];
                let _e973 = global_1.member[(_e935 + 7u)];
                let _e978 = global_1.member[(_e935 + 8u)];
                let _e983 = global_1.member[(_e935 + 9u)];
                let _e988 = global_1.member[(_e935 + 10u)];
                let _e993 = global_1.member[(_e935 + 11u)];
                let _e998 = global_1.member[(_e935 + 12u)];
                let _e1003 = global_1.member[(_e935 + 13u)];
                let _e1008 = global_1.member[(_e935 + 14u)];
                let _e1013 = global_1.member[(_e935 + 15u)];
                phi_4637_ = mat4x4<f32>(vec4<f32>(bitcast<f32>(_e938), bitcast<f32>(_e943), bitcast<f32>(_e948), bitcast<f32>(_e953)), vec4<f32>(bitcast<f32>(_e958), bitcast<f32>(_e963), bitcast<f32>(_e968), bitcast<f32>(_e973)), vec4<f32>(bitcast<f32>(_e978), bitcast<f32>(_e983), bitcast<f32>(_e988), bitcast<f32>(_e993)), vec4<f32>(bitcast<f32>(_e998), bitcast<f32>(_e1003), bitcast<f32>(_e1008), bitcast<f32>(_e1013)));
            }
            let _e1021 = phi_4637_;
            let _e1022 = (3u * _e117);
            global_3[_e1022] = vec4<f32>(_e1021[0].x, _e1021[1].x, _e1021[2].x, _e1021[3].x);
            global_3[(_e1022 + 1u)] = vec4<f32>(_e1021[0].y, _e1021[1].y, _e1021[2].y, _e1021[3].y);
            global_3[(_e1022 + 2u)] = vec4<f32>(_e1021[0].z, _e1021[1].z, _e1021[2].z, _e1021[3].z);
            continue;
        } else {
            break;
        }
        continuing {
            phi_4597_ = (_e117 + 64u);
        }
    }
    workgroupBarrier();
    if _e89 {
        let _e1059 = global_6.member[_e63].member;
        let _e1063 = global_8.member[_e1059].member_2;
        phi_4600_ = _e1063;
    } else {
        phi_4600_ = 0u;
    }
    let _e1065 = phi_4600_;
    phi_4601_ = 1u;
    loop {
        let _e1067 = phi_4601_;
        if (_e1067 < 64u) {
            if (_e1067 < _e1065) {
                let _e1070 = (_e92.member_8 + _e1067);
                let _e1073 = global_1.member[_e1070];
                let _e1077 = global_1.member[(_e1070 + 1u)];
                phi_4607_ = (_e1073 + _e115);
                loop {
                    let _e1081 = phi_4607_;
                    if (_e1081 < min(_e1077, _e114)) {
                        let _e1086 = global_1.member[(_e92.member_3 + _e1081)];
                        if (_e1086 < _e1081) {
                            let _e1088 = (3u * _e1086);
                            let _e1089 = (3u * _e1081);
                            let _e1091 = global_3[_e1088];
                            let _e1094 = global_3[(_e1088 + 1u)];
                            let _e1097 = global_3[(_e1088 + 2u)];
                            let _e1099 = global_3[_e1089];
                            let _e1102 = global_3[(_e1089 + 1u)];
                            let _e1105 = global_3[(_e1089 + 2u)];
                            global_3[_e1089] = ((((_e1099 * _e1091.x) + (_e1102 * _e1091.y)) + (_e1105 * _e1091.z)) + vec4<f32>(0f, 0f, 0f, _e1091.w));
                            global_3[(_e1089 + 1u)] = ((((_e1099 * _e1094.x) + (_e1102 * _e1094.y)) + (_e1105 * _e1094.z)) + vec4<f32>(0f, 0f, 0f, _e1094.w));
                            global_3[(_e1089 + 2u)] = ((((_e1099 * _e1097.x) + (_e1102 * _e1097.y)) + (_e1105 * _e1097.z)) + vec4<f32>(0f, 0f, 0f, _e1097.w));
                        }
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        phi_4607_ = (_e1081 + 64u);
                    }
                }
            }
            workgroupBarrier();
            continue;
        } else {
            break;
        }
        continuing {
            phi_4601_ = (_e1067 + bitcast<u32>(1i));
        }
    }
    phi_4605_ = _e115;
    loop {
        let _e1144 = phi_4605_;
        if (_e1144 < select(0u, _e92.member_1, _e89)) {
            let _e1149 = global_1.member[(_e92.member_7 + _e1144)];
            if (_e1149 < _e114) {
                let _e1152 = (_e92.member_6 + (_e1144 * 16u));
                let _e1155 = global_1.member[_e1152];
                let _e1160 = global_1.member[(_e1152 + 1u)];
                let _e1165 = global_1.member[(_e1152 + 2u)];
                let _e1170 = global_1.member[(_e1152 + 3u)];
                let _e1175 = global_1.member[(_e1152 + 4u)];
                let _e1180 = global_1.member[(_e1152 + 5u)];
                let _e1185 = global_1.member[(_e1152 + 6u)];
                let _e1190 = global_1.member[(_e1152 + 7u)];
                let _e1195 = global_1.member[(_e1152 + 8u)];
                let _e1200 = global_1.member[(_e1152 + 9u)];
                let _e1205 = global_1.member[(_e1152 + 10u)];
                let _e1210 = global_1.member[(_e1152 + 11u)];
                let _e1215 = global_1.member[(_e1152 + 12u)];
                let _e1220 = global_1.member[(_e1152 + 13u)];
                let _e1225 = global_1.member[(_e1152 + 14u)];
                let _e1230 = global_1.member[(_e1152 + 15u)];
                let _e1237 = (3u * _e1149);
                let _e1239 = global_3[_e1237];
                let _e1242 = global_3[(_e1237 + 1u)];
                let _e1245 = global_3[(_e1237 + 2u)];
                let _e1264 = ((_e92.member_11 * mat4x4<f32>(vec4<f32>(_e1239.x, _e1242.x, _e1245.x, 0f), vec4<f32>(_e1239.y, _e1242.y, _e1245.y, 0f), vec4<f32>(_e1239.z, _e1242.z, _e1245.z, 0f), vec4<f32>(_e1239.w, _e1242.w, _e1245.w, 1f))) * mat4x4<f32>(vec4<f32>(bitcast<f32>(_e1155), bitcast<f32>(_e1160), bitcast<f32>(_e1165), bitcast<f32>(_e1170)), vec4<f32>(bitcast<f32>(_e1175), bitcast<f32>(_e1180), bitcast<f32>(_e1185), bitcast<f32>(_e1190)), vec4<f32>(bitcast<f32>(_e1195), bitcast<f32>(_e1200), bitcast<f32>(_e1205), bitcast<f32>(_e1210)), vec4<f32>(bitcast<f32>(_e1215), bitcast<f32>(_e1220), bitcast<f32>(_e1225), bitcast<f32>(_e1230))));
                let _e1266 = (3u * (_e66.member_3 + _e1144));
                global.member[_e1266] = vec4<f32>(_e1264[0].x, _e1264[1].x, _e1264[2].x, _e1264[3].x);
                global.member[(_e1266 + 1u)] = vec4<f32>(_e1264[0].y, _e1264[1].y, _e1264[2].y, _e1264[3].y);
                global.member[(_e1266 + 2u)] = vec4<f32>(_e1264[0].z, _e1264[1].z, _e1264[2].z, _e1264[3].z);
            }
            continue;
        } else {
            break;
        }
        continuing {
            phi_4605_ = (_e1144 + 64u);
        }
    }
    return;
}

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(workgroup_id) param: vec3<u32>, @builtin(local_invocation_index) param_1: u32) {
    global_4 = param;
    global_10 = param_1;
    function_();
}
