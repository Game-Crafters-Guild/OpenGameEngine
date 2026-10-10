struct type_8 {
    member: u32,
    member_1: u32,
}

struct type_21 {
    member: array<u32>,
}

struct type_27 {
    member: array<atomic<u32>>,
}

@group(0) @binding(66)
var global: sampler;
@group(0) @binding(2)
var global_1: texture_2d<f32>;
@group(3) @binding(0)
var<uniform> global_2: type_8;
var<private> global_3: u32;
var<workgroup> global_4: array<atomic<u32>, 512>;
var<private> global_5: vec3<u32>;
@group(0) @binding(64)
var global_6: sampler;
@group(0) @binding(0)
var global_7: texture_2d<f32>;
@group(0) @binding(1)
var<storage, read_write> global_8: type_27;

fn function_() {
    var phi_276_: u32;
    var phi_146_: bool;
    var phi_277_: u32;
    var phi_278_: u32;

    let _e35 = global_3;
    phi_276_ = _e35;
    loop {
        let _e37 = phi_276_;
        if (_e37 < 512u) {
            continue;
        } else {
            break;
        }
        continuing {
            atomicStore((&global_4[_e37]), 0u);
            phi_276_ = (_e37 + 64u);
        }
    }
    workgroupBarrier();
    let _e41 = global_5;
    let _e43 = bitcast<vec2<i32>>(_e41.xy);
    let _e47 = global_2.member;
    let _e48 = (bitcast<u32>(_e43.x) < _e47);
    phi_146_ = _e48;
    if _e48 {
        let _e52 = global_2.member_1;
        phi_146_ = (bitcast<u32>(_e43.y) < _e52);
    }
    let _e55 = phi_146_;
    if _e55 {
        let _e56 = textureLoad(global_7, _e43, 0i);
        let _e59 = dot(max(_e56.xyz, vec3<f32>(0f, 0f, 0f)), vec3<f32>(0.2126f, 0.7152f, 0.0722f));
        switch bitcast<i32>(0u) {
            default: {
                if (_e59 < 0.0000076f) {
                    phi_277_ = 0u;
                    break;
                }
                phi_277_ = u32(((clamp(((log2(_e59) - -16f) * 0.033333335f), 0f, 1f) * 254f) + 1f));
                break;
            }
        }
        let _e70 = phi_277_;
        let _e71 = textureDimensions(global_1, 0i);
        let _e72 = vec2<i32>(_e71);
        let _e77 = global_2.member_1;
        let _e86 = textureLoad(global_1, clamp(vec2<i32>((((vec2<f32>(_e43) + vec2<f32>(0.5f, 0.5f)) / vec2<f32>(f32(_e47), f32(_e77))) * vec2<f32>(_e72))), vec2<i32>(0i, 0i), (_e72 - vec2<i32>(1i, 1i))), 0i);
        let _e93 = atomicAdd((&global_4[((select(0u, 1u, (_e86.x <= 0f)) * 256u) + _e70)]), 1u);
    }
    workgroupBarrier();
    phi_278_ = _e35;
    loop {
        let _e95 = phi_278_;
        if (_e95 < 512u) {
            let _e98 = atomicLoad((&global_4[_e95]));
            if (_e98 > 0u) {
                let _e102 = atomicAdd((&global_8.member[_e95]), _e98);
            }
            continue;
        } else {
            break;
        }
        continuing {
            phi_278_ = (_e95 + 64u);
        }
    }
    return;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(local_invocation_index) param: u32, @builtin(global_invocation_id) param_1: vec3<u32>) {
    global_3 = param;
    global_5 = param_1;
    function_();
}
