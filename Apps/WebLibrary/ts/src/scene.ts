// The engine's world: entity handles by id, queries over its components, model loads (one at a
// time, polled until the engine reports them done), and the default camera, sun and sky.

import type { ComponentType, CreateOptions, LoadOptions, LoadProgress, Model, Query, Scene } from '../opengine.js';
import { AssetStatus, kInvalidAsset, kInvalidEntity, kOk, type EntityId } from './abi.js';
import type { Bridge } from './bridge.js';
import { EntityImpl, type EntityOwner } from './entity.js';
import { OpenEngineError } from './errors.js';
import { downloadModel } from './progress.js';
import { QueryImpl } from './query.js';
import type { SkyViewImpl } from './sky-view.js';
import type { SunViewImpl } from './sun-view.js';

/** What the scene needs from its engine. */
export interface SceneHost {
    readonly bridge: Bridge;
    /** Asks for pollLoads to be called soon: after the next tick, or on its own when the engine is not running. */
    requestLoadPoll(): void;
    onFrame(callback: (dt: number) => void): () => void;
    resolveUrl(url: string): string;
}

export interface SceneDefaults {
    camera: EntityImpl;
    sun: SunViewImpl;
    sky: SkyViewImpl;
}

export class SceneImpl implements Scene, EntityOwner {
    readonly bridge: Bridge;
    private readonly m_Host: SceneHost;
    private readonly m_Entities = new Map<EntityId, EntityImpl>();
    private m_Defaults: SceneDefaults | null = null;
    /** Settles when every load requested so far has settled. */
    private m_LoadQueue: Promise<unknown> = Promise.resolve();
    /** The address the engine loaded each URL under: the URL itself, or the blob: URL of its download with progress. */
    private readonly m_EngineUrls = new Map<string, string>();
    /** The load the engine is running, polled until it is Ready or Failed, and the emissive strength it instantiates with. */
    private m_ActiveLoad: {
        handle: number; url: string; emissiveStrength: number | undefined; resolve(model: Model): void; reject(error: unknown): void;
    } | null = null;

    constructor(host: SceneHost) {
        this.m_Host = host;
        this.bridge = host.bridge;
    }

    setDefaults(defaults: SceneDefaults): void {
        this.m_Defaults = defaults;
    }

    get camera(): EntityImpl { return this.defaults().camera; }
    get sun(): SunViewImpl { return this.defaults().sun; }
    get sky(): SkyViewImpl { return this.defaults().sky; }

    create(nameOrOptions?: string | CreateOptions): EntityImpl {
        const { name, mesh } = typeof nameOrOptions === 'string' ? { name: nameOrOptions, mesh: undefined } : (nameOrOptions ?? {});
        const id = this.bridge.abi.ge_entity_create();
        if (id === kInvalidEntity) throw this.bridge.failure('scene.create');
        if (mesh !== undefined && this.bridge.abi.ge_entity_set_mesh(id, this.bridge.writeString(mesh)) !== kOk) {
            // A refused mesh leaves no entity behind.
            const error = this.bridge.failure('scene.create');
            this.bridge.abi.ge_entity_destroy(id);
            throw error;
        }
        const entity = this.entityFor(id);
        if (name !== undefined && name !== '') {
            const info = this.bridge.reflection.component('Name');
            this.bridge.check(this.bridge.abi.ge_component_add(id, info.typeId), 'scene.create');
            entity.writeField(info, 'value', name);
        }
        return entity;
    }

    query(read: readonly ComponentType[], options?: { write?: readonly ComponentType[] }): Query {
        return new QueryImpl(this, read, options?.write ?? []);
    }

    load(url: string, options?: LoadOptions): Promise<Model> {
        if (this.bridge.contract.disposed) return Promise.reject(this.bridge.contract.disposedError('scene.load'));
        const strength = options?.emissiveStrength;
        if (strength !== undefined && !(Number.isFinite(strength) && strength >= 0)) {
            return Promise.reject(new OpenEngineError('InvalidArgument',
                `scene.load('${url}'): emissiveStrength takes a finite number of 0 or more (1 shows the file's emission); got ${String(strength)}.`));
        }
        const absolute = this.m_Host.resolveUrl(url);
        // A load starts in a promise continuation, never synchronously: one requested inside a
        // frame (an onFrame callback) starts after that frame's ge_tick has returned, and one
        // requested during another load starts when that load settles.
        const load = this.m_LoadQueue.then(() => this.startLoad(absolute, options?.onProgress, strength));
        this.m_LoadQueue = load.catch(() => undefined);
        return load;
    }

    /** True while the engine is running a load. */
    get loading(): boolean {
        return this.m_ActiveLoad !== null;
    }

    /** Checks the running load once; resolves or rejects it when the engine is done with it. */
    pollLoads(): void {
        const active = this.m_ActiveLoad;
        if (!active) return;
        const bridge = this.bridge;
        try {
            const status = bridge.abi.ge_asset_status(active.handle);
            if (status === AssetStatus.Loading) {
                this.m_Host.requestLoadPoll();
                return;
            }
            this.m_ActiveLoad = null;
            if (status !== AssetStatus.Ready) throw bridge.failure(`scene.load('${active.url}')`);
            const id = bridge.abi.ge_instantiate_model(active.handle, kInvalidEntity);
            if (id === kInvalidEntity) throw bridge.failure(`scene.load('${active.url}')`);
            // The instance reset the load's materials to the file's emission. A strength the engine
            // refuses takes the instance away with it: the load leaves no model behind when it rejects.
            if (active.emissiveStrength !== undefined && bridge.abi.ge_model_emissive_strength(active.handle, active.emissiveStrength) !== kOk) {
                const error = bridge.failure(`scene.load('${active.url}')`);
                bridge.abi.ge_entity_destroy(id);
                throw error;
            }
            active.resolve(this.entityFor(id) as EntityImpl & Model);
        } catch (error) {
            this.m_ActiveLoad = null;
            active.reject(error);
        }
    }

    /** Rejects the running load: the engine is going away. */
    abandonLoads(): void {
        const active = this.m_ActiveLoad;
        this.m_ActiveLoad = null;
        active?.reject(this.bridge.contract.disposedError(`scene.load('${active.url}')`));
    }

    entityFor(id: EntityId): EntityImpl {
        let entity = this.m_Entities.get(id);
        if (!entity) {
            entity = new EntityImpl(this, id);
            this.m_Entities.set(id, entity);
        }
        return entity;
    }

    forget(id: EntityId): void {
        this.m_Entities.delete(id);
    }

    onFrame(callback: (dt: number) => void): () => void {
        return this.m_Host.onFrame(callback);
    }

    /**
     * Hands the engine `url`, or the file downloaded with progress the first time it is loaded
     * that way: the engine fetches a URL once and reuses it, so a URL keeps the address the
     * engine first had it under, until a load of it fails.
     */
    private async startLoad(url: string, onProgress: ((progress: LoadProgress) => void) | undefined,
        emissiveStrength: number | undefined): Promise<Model> {
        const bridge = this.bridge;
        bridge.contract.checkAlive(`scene.load('${url}')`);
        let engineUrl = this.m_EngineUrls.get(url);
        const downloaded = !engineUrl && onProgress ? await downloadModel(url, onProgress) : null;
        engineUrl ??= downloaded ?? url;
        // The download's object URL is its address without the file name after '#'.
        const release = () => { if (downloaded) URL.revokeObjectURL(downloaded.slice(0, downloaded.indexOf('#'))); };
        let handle: number;
        try {
            bridge.contract.checkAlive(`scene.load('${url}')`);
            handle = bridge.abi.ge_load_asset(bridge.writeString(engineUrl));
            if (handle === kInvalidAsset) throw bridge.failure(`scene.load('${url}')`);
        } catch (error) {
            release();
            throw error;
        }
        this.m_EngineUrls.set(url, engineUrl);
        const loaded = new Promise<Model>((resolve, reject) => {
            this.m_ActiveLoad = { handle, url, emissiveStrength, resolve, reject };
            this.m_Host.requestLoadPoll();
        });
        // Once the engine is done with the file it holds the bytes, or a failed load that a
        // retry fetches again.
        const settled = (failed: boolean) => {
            release();
            if (failed) this.m_EngineUrls.delete(url);
        };
        loaded.then(() => settled(false), () => settled(true));
        return loaded;
    }

    private defaults(): SceneDefaults {
        if (!this.m_Defaults) throw new Error('the default entities are created by Engine.create');
        return this.m_Defaults;
    }
}
