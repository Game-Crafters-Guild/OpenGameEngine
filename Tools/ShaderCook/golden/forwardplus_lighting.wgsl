struct type_5 {
    member: vec4<u32>,
    member_1: array<vec2<u32>>,
}

struct type_9 {
    member: vec4<u32>,
}

struct type_19 {
    member: mat4x4<f32>,
    member_1: mat4x4<f32>,
    member_2: vec4<f32>,
    member_3: vec4<f32>,
}

struct type_27 {
    member: array<u32>,
}

struct type_30 {
    member: vec4<u32>,
    member_1: vec4<f32>,
    member_2: vec4<f32>,
    member_3: vec4<f32>,
    member_4: vec4<f32>,
    member_5: vec4<f32>,
    member_6: vec4<f32>,
    member_7: vec4<f32>,
    member_8: vec4<i32>,
}

struct type_32 {
    member: vec4<u32>,
    member_1: array<type_30>,
}

@group(0) @binding(1) 
var<storage> global: type_5;
@group(0) @binding(4) 
var<uniform> global_1: type_9;
var<private> global_2: vec4<f32>;
@group(0) @binding(64) 
var global_3: sampler;
@group(0) @binding(0) 
var global_4: texture_2d<f32>;
@group(0) @binding(5) 
var<uniform> global_5: type_19;
var<private> global_6: vec4<f32>;
@group(0) @binding(2) 
var<storage> global_7: type_27;
@group(0) @binding(3) 
var<storage> global_8: type_32;

fn function_() {
    var phi_196_: bool;
    var phi_574_: f32;
    var phi_580_: vec3<f32>;
    var phi_578_: vec3<f32>;
    var phi_577_: u32;
    var phi_581_: f32;
    var phi_587_: vec3<f32>;
    var phi_586_: vec3<f32>;
    var local: vec3<f32>;
    var local_1: vec3<f32>;
    var local_2: vec3<f32>;

    switch bitcast<i32>(0u) {
        default: {
            let _e33 = global.member[0u];
            let _e34 = max(_e33, 1u);
            let _e37 = global.member[1u];
            let _e38 = max(_e37, 1u);
            let _e41 = global.member[2u];
            let _e45 = global.member[3u];
            let _e48 = global_1.member;
            let _e50 = max(_e48.xy, vec2<u32>(1u, 1u));
            let _e57 = vec2<f32>(f32((_e34 * _e50.x)), f32((_e38 * _e50.y)));
            let _e58 = global_2;
            let _e59 = _e58.xy;
            let _e62 = vec2<u32>(clamp(_e59, vec2<f32>(0f, 0f), (_e57 - vec2<f32>(1f, 1f))));
            let _e64 = textureLoad(global_4, bitcast<vec2<i32>>(_e62), 0i);
            let _e68 = (((_e59 / _e57) * 2f) - vec2<f32>(1f, 1f));
            let _e70 = global_5.member;
            let _e74 = (_e70 * vec4<f32>(_e68.x, _e68.y, _e64.x, 1f));
            let _e79 = (_e74.xyz / vec3(max(_e74.w, 0.000001f)));
            let _e85 = vec4<f32>(_e79.x, vec4<f32>().y, vec4<f32>().z, vec4<f32>().w);
            let _e91 = vec4<f32>(_e85.x, _e79.y, _e85.z, _e85.w);
            let _e98 = vec4<f32>(_e91.x, _e91.y, _e79.z, _e91.w).xyz;
            let _e99 = dpdx(_e98);
            let _e100 = dpdy(_e98);
            let _e102 = normalize(cross(_e99, _e100));
            if (_e64.x <= 0.000001f) {
                global_6 = vec4<f32>(0f, 0f, 0f, 0f);
                break;
            }
            let _e104 = (_e62 / _e50);
            let _e106 = (_e104.x >= _e34);
            phi_196_ = _e106;
            if !(_e106) {
                phi_196_ = (_e104.y >= _e38);
            }
            let _e111 = phi_196_;
            if _e111 {
                global_6 = vec4<f32>(0f, 0f, 0f, 0f);
                break;
            }
            let _e114 = global_5.member_2[0u];
            let _e115 = max(_e114, 0.0001f);
            let _e118 = global_5.member_2[1u];
            phi_574_ = _e118;
            if (_e118 <= (_e115 + 0.0001f)) {
                phi_574_ = (_e115 + 1000f);
            }
            let _e123 = phi_574_;
            let _e132 = global_5.member_2[2u];
            let _e149 = global.member_1[((_e104.x + (_e104.y * _e34)) + (u32(floor((clamp((log((((_e115 * _e123) / max(((_e64.x * (_e123 - _e115)) + _e115), 0.000001f)) / _e115)) * _e132), 0f, 0.999999f) * f32(max(_e41, 1u))))) * (_e34 * _e38)))];
            phi_580_ = _e102;
            if (dot(_e102, -(normalize(_e98))) < 0f) {
                phi_580_ = -(_e102);
            }
            let _e159 = phi_580_;
            phi_578_ = vec3<f32>(0f, 0f, 0f);
            phi_577_ = 0u;
            loop {
                let _e161 = phi_578_;
                let _e163 = phi_577_;
                local = _e161;
                local_1 = _e161;
                local_2 = _e161;
                if (_e163 < min(_e149.y, max(_e45, 1u))) {
                    let _e168 = global_7.member[(_e149.x + _e163)];
                    let _e171 = global_8.member[0u];
                    if (_e168 >= _e171) {
                        phi_586_ = _e161;
                        continue;
                    }
                    let _e175 = global_8.member_1[_e168];
                    if (_e175.member.w == 0u) {
                        phi_586_ = _e161;
                        continue;
                    }
                    let _e184 = _e175.member_3.xyz;
                    if (_e175.member.x == 0u) {
                        let _e189 = normalize(_e175.member_2.xyz);
                        let _e191 = global_5.member_1;
                        phi_587_ = (_e161 + (_e184 * (_e175.member_2.w * max(dot(_e159, normalize(-(normalize((_e191 * vec4<f32>(_e189.x, _e189.y, _e189.z, 0f)).xyz)))), 0f))));
                    } else {
                        let _e209 = global_5.member_1;
                        let _e216 = ((_e209 * vec4<f32>(_e175.member_1.x, _e175.member_1.y, _e175.member_1.z, 1f)).xyz - _e98);
                        let _e217 = length(_e216);
                        let _e220 = (_e216 / vec3(max(_e217, 0.000001f)));
                        let _e225 = clamp((1f - (_e217 / max(_e175.member_1.w, 0.001f))), 0f, 1f);
                        phi_581_ = 1f;
                        if (_e175.member.x == 2u) {
                            let _e228 = normalize(_e175.member_2.xyz);
                            phi_581_ = smoothstep(_e175.member_5.x, _e175.member_4.w, dot(normalize(-(_e220)), normalize(normalize((_e209 * vec4<f32>(_e228.x, _e228.y, _e228.z, 0f)).xyz))));
                        }
                        let _e244 = phi_581_;
                        phi_587_ = (_e161 + (_e184 * ((((_e175.member_2.w * max(dot(_e159, _e220), 0f)) * _e225) * _e225) * _e244)));
                    }
                    let _e252 = phi_587_;
                    phi_586_ = _e252;
                    continue;
                } else {
                    break;
                }
                continuing {
                    let _e254 = phi_586_;
                    phi_578_ = _e254;
                    phi_577_ = (_e163 + bitcast<u32>(1i));
                }
            }
            let _e258 = local;
            let _e261 = local_1;
            let _e264 = local_2;
            global_6 = vec4<f32>(_e258.x, _e261.y, _e264.z, 1f);
            break;
        }
    }
    return;
}

@fragment 
fn main(@builtin(position) param: vec4<f32>) -> @location(0) vec4<f32> {
    global_2 = param;
    function_();
    let _e3 = global_6;
    return _e3;
}
