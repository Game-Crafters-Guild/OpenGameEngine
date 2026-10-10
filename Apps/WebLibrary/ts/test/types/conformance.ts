// The implementation's exports have the types opengine.d.ts declares.
import type * as Api from '../../opengine.js';
import * as Impl from '../../src/index.js';

export const conformance: {
    Engine: typeof Api.Engine;
    OrbitControls: typeof Api.OrbitControls;
    OpenEngineError: typeof Api.OpenEngineError;
    Vector3: typeof Api.Vector3;
    Transform: typeof Api.Transform;
    Light: typeof Api.Light;
    LocalBounds: typeof Api.LocalBounds;
    MeshRenderer: typeof Api.MeshRenderer;
    Camera: typeof Api.Camera;
    SkyEnvironment: typeof Api.SkyEnvironment;
} = Impl;
