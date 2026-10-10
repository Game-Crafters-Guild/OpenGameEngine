// entity.animation: a loaded model's clips, played one at a time through the engine module's
// animation calls (the Animator component is not reflected, so entity.get/set cannot reach it).
// The view holds the entity's id, never a playback state: the engine owns that.

import type { AnimationView } from '../opengine.js';
import type { EntityId } from './abi.js';
import type { Bridge } from './bridge.js';

export class AnimationViewImpl implements AnimationView {
    private readonly m_Bridge: Bridge;
    private readonly m_Entity: EntityId;
    private readonly m_AssertAlive: () => void;

    constructor(bridge: Bridge, entity: EntityId, assertAlive: () => void) {
        this.m_Bridge = bridge;
        this.m_Entity = entity;
        this.m_AssertAlive = assertAlive;
    }

    get clips(): string[] {
        this.m_AssertAlive();
        const text = this.m_Bridge.abi.ge_animation_clips(this.m_Entity);
        if (text === 0) throw this.m_Bridge.failure('entity.animation.clips');
        return JSON.parse(this.m_Bridge.readString(text)) as string[];
    }

    play(clipName: string, options: { speed?: number } = {}): void {
        this.m_AssertAlive();
        const bridge = this.m_Bridge;
        const code = bridge.abi.ge_animation_play(this.m_Entity, bridge.writeString(clipName), options.speed ?? 1);
        bridge.check(code, `entity.animation.play('${clipName}')`);
    }

    pause(): void {
        this.m_AssertAlive();
        this.m_Bridge.check(this.m_Bridge.abi.ge_animation_pause(this.m_Entity), 'entity.animation.pause()');
    }
}
