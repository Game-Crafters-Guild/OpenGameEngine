@group(0) @binding(64) 
var global: sampler;
@group(0) @binding(0) 
var global_1: texture_2d<f32>;
var<private> global_2: vec2<f32>;
var<private> global_3: vec4<f32>;

fn function_() {
    var local: array<f32, 5>;
    var local_1: array<f32, 5>;
    var phi_107_: vec3<f32>;
    var phi_106_: i32;
    var local_2: vec3<f32>;
    var local_3: vec3<f32>;
    var local_4: vec3<f32>;

    let _e18 = textureDimensions(global_1, 0i);
    let _e24 = global_2;
    let _e25 = textureSample(global_1, global, _e24);
    phi_107_ = (_e25.xyz * 0.22702703f);
    phi_106_ = 1i;
    loop {
        let _e29 = phi_107_;
        let _e31 = phi_106_;
        local_2 = _e29;
        local_3 = _e29;
        local_4 = _e29;
        if (_e31 < 5i) {
            continue;
        } else {
            break;
        }
        continuing {
            let _e34 = (vec2<f32>((1f / f32(vec2<i32>(_e18).x)), 0f) * f32(_e31));
            let _e36 = textureSample(global_1, global, (_e24 + _e34));
            local = array<f32, 5>(0.22702703f, 0.19459459f, 0.12162162f, 0.054054055f, 0.016216217f);
            let _e39 = local[_e31];
            let _e43 = textureSample(global_1, global, (_e24 - _e34));
            local_1 = array<f32, 5>(0.22702703f, 0.19459459f, 0.12162162f, 0.054054055f, 0.016216217f);
            let _e46 = local_1[_e31];
            phi_107_ = ((_e29 + (_e36.xyz * _e39)) + (_e43.xyz * _e46));
            phi_106_ = (_e31 + 1i);
        }
    }
    let _e51 = local_2;
    let _e54 = local_3;
    let _e57 = local_4;
    global_3 = vec4<f32>(_e51.x, _e54.y, _e57.z, 1f);
    return;
}

@fragment 
fn main(@location(0) param: vec2<f32>) -> @location(0) vec4<f32> {
    global_2 = param;
    function_();
    let _e3 = global_3;
    return _e3;
}
