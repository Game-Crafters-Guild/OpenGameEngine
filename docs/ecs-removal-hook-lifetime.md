# ECS component-hook ownership

World supports persistent registrations and scoped subscriptions for Add, Set
and Remove hooks. Each supports both `void(T&)` and `void(EntityHandle, T&)`
callbacks. There is one callback slot per component and hook kind; registering
again replaces that slot.

## Persistent and scoped ownership

`RegisterOnAdd`, `RegisterOnSet` and `RegisterOnRemove` return an opaque
`ComponentHookToken`. Ignoring the return value preserves the existing persistent
registration behavior. `UnregisterComponentHook(token)` removes only the exact
registration identified by its World, component, kind and monotonic registration
ID. Replacing a slot invalidates an old token even when the callback is identical.
Wrong-World, empty, replaced and already-removed tokens return false.

`SubscribeOnAdd`, `SubscribeOnSet` and `SubscribeOnRemove` return the existing
move-only Types `ScopedSubscription`. Destruction or `Reset()` detaches that
registration. Moving transfers ownership; resetting a replaced subscription
cannot erase the newer registration. A scope may outlive its World safely.

```cpp
// Persistent ownership; an ignored token leaves the hook installed.
auto token = world.RegisterOnSet<Position>(UpdatePosition);
world.UnregisterComponentHook(token);

// External resource owner with a shorter lifetime than the World.
auto removal = world.SubscribeOnRemove<OwnedResource>(ReleaseOwnedResource);
// ... component removal invokes ReleaseOwnedResource ...
DrainOwnedResources(world);
removal.Reset();
```

Null callbacks return empty handles without replacing an existing registration.
`Clear()` invokes removal hooks but preserves registrations and their IDs for
subsequent entities. Removing a Remove hook refreshes flags on existing
archetypes; other kinds and components remain registered. Failed allocation
cannot leave a hook without its signature or an unreachable scoped registration.

Registration, explicit unregistration and scope reset are quiescent external-owner
operations: do not run them concurrently with structural mutation or callback
invocation. Do not reset from inside a hook or from a World-owned object destructor
called during `Clear()`. Reset detaches a callable; it does not invoke cleanup.
Drain resources before detaching their cleanup hook. The supported concurrency
exception is resetting an external scope while its World is being destroyed.

## Entity destruction and removal notifications

Immediate, deferred and preserve-handle entity destruction, `Clear()` and World
destruction run every installed Remove hook while the component data and the
handle are still live, and treat each hook as a notification rather than a veto.
An exception, standard or not, is logged with its message, the remaining hooks
still run, and the row is retired exactly once: the subscribed `Removed`
lifecycle events are recorded, the row is removed, the handle is invalidated and
the counts are updated. A later request for the same handle is a no-op.
Preserve-handle destruction keeps its undo/revival behavior.

Before the first hook runs, the World reserves the free-index slot and the
subscribed `Removed` capacity the retirement will need. A failure there
propagates with the entity, its components and their ownership untouched. Once
the hooks start, retirement allocates nothing, so a throwing hook cannot leave a
half-destroyed row. A deferred destroy whose reservation fails is a playback
failure: the popped command is not retried.

### Destroying a set of entities

`DestroyEntitiesImmediate(std::span<const EntityHandle>)` gives a caller-selected
set one preparation boundary. A loop of `DestroyEntityImmediate` calls has one
per entity: entity N's reservation can fail after entities 0..N-1 have already
released their resources. The set call holds the world lock throughout, snapshots
the valid handles, reserves every free-index append and every subscribed
`Removed` append for the whole set, then retires the entities in input order
through the same body as the singular call. Invalid, stale and duplicate handles
are ignored; the first occurrence fixes notification order; pending lifecycle
events stay ahead of the new ones; version increments and free-index reuse order
match the equivalent singular calls. A reservation failure (`bad_alloc`,
`length_error`) propagates before any notification, invalidation, structural
version increment or event append, leaving every entity intact (internal
capacities may have grown). There is no rollback once notifications start, no
deferred or preserve-handle form, and no hierarchy traversal: callers pass the
child-first set themselves.

Each retirement re-reads the entity's current row because an earlier removal in
the set may have swap-moved it. Callbacks may replace the caller's input
container; the snapshot protects the remaining iteration. Reentry follows the
singular rules: from inside a hook, an empty set or a set holding only the
entity currently being removed is a no-op; any other set, including a sibling
the outer set already requested, throws `std::logic_error`.

Cost: O(N) transient handle storage, O(N*S) counting over the S subscribed
types, no persistent World data. Duplicates count toward reservation, so a
duplicate-heavy set may grow capacity it does not use.

A replacement operation sequences: prepare replacement values and external
resources; build the child-first obsolete list; call the set destroy; then
publish with prepared non-allocating operations. Reacquire surviving component
pointers afterwards because removed rows can move other entities.

Hooks run on the destroying thread under the exclusive world lock. From inside a
hook on that thread, destroying the current entity again through any entry point
(or any entity while `Clear()` or destruction is running) is a harmless no-op.
Every other same-world structural request (create, add/remove/set component,
destroy another entity, `Clear()`, `ProcessCommands()`, hook or lifecycle
registration, bulk mode) throws `std::logic_error` before it would take the lock;
the notification isolation then logs it and the owner's destruction still
completes. Same-world reads are not guarded and are outside the contract. Other
worlds may be mutated; a nested destruction scope restores the outer one when it
ends. Another thread sees none of this and waits for the world lock as usual, so
a hook must not wait on work it handed to another thread.

The scope lives in thread-local storage of the ECS copy inside Engine.dll, which
every product binary reaches through the import library; `World` gains no
member for it.

Component-only removal (`RemoveComponent*`) keeps its existing behavior: a
throwing hook propagates and the component stays.

This guarantees ECS finalization, not the cleanup a hook owns: a hook that
throws has abandoned whatever it had not yet released, and the log line says so.
Resource owners (GPU scene rows, skeleton and animation slots, physics bodies,
navigation maps, terrain resources) must make their release paths complete
without throwing. The renderer does this by reserving the storage a release
path needs at acquisition time, where failing is still harmless.

## World and native-code lifetime

The scope's closure and weak-state control block are created in engine code.
The closure captures a token, never a raw World pointer. It resolves the World
identity under the live-world registry mutex and holds that mutex throughout
unregistration. World destruction removes its registry entry under the same lock
before touching members or invoking resource cleanup. Thus reset either finishes
before teardown begins, or finds no World and does nothing. The weak marker alone
does not keep a World alive. It may remain lockable during destruction; the
registry protocol supplies the lifetime guarantee. Construction publishes the
World only after potentially failing initialization has completed.

`World::CountComponentHooksOwnedByImage(base, size)` counts Add, Set and Remove
slots whose callback or invocation thunk lies in the image's half-open address
range. Each slot counts once. It scans this linked ECS copy's live-world registry,
including empty worlds. Run it at the main-thread module unload boundary, without
concurrent registration, mutation or World destruction.

The native-module image observer includes this count in its superseded-module
unload ledger. A remaining hook retains the old DLL and appears as
`ECS component hook(s)` in blocker diagnostics. Unload does not revoke hooks:
surviving component data may still require their cleanup callback. A missed
teardown therefore retains callable code. Singleton destructors and other
registries have separate lifetime contracts.

The failed-load abort path still purges registrations and unmaps directly; this
change does not extend pin retention to that path. Do not register World hooks
from DLL static initialization or a load handshake. Play `OnStart` runs only
after a successful load, outside that abort path.

## Play-session integration

The Editor defers native reload before starting user systems. Exiting play stops
those systems before restoring the scene snapshot and flushing deferred reloads.
A play-only owner therefore subscribes in `OnStart`, drains resources in
`OnDestroy`, then resets its subscription. The Editor retains its World between
play sessions, so World destruction alone cannot establish this boundary.

The current user-system adapters are non-owning: clearing their registry does
not delete them. An explicit reset in `OnDestroy` is still required even when the
subscription is a C++ member. If resource teardown fails before reset, the hook
and its DLL pin remain. Embedders replacing modules outside the Editor lifecycle
must arrange their own teardown; `LoadModule` does not start or stop systems.

## Verification

`RemovalHookLifetimeTests.cpp` covers token unregistration, archetype flags,
component cleanup, Clear, destruction, callback replacement and image ranges.
`ComponentHookSubscriptionTests.cpp` covers all six registration/subscription
overloads, same-callback replacement, move/reset, World/kind isolation, Clear,
null callbacks, address reuse, constructor failure and DLL pin release. Its
blocked-destructor test resets a scope after the World leaves the registry while
the destructor is still invoking cleanup.

`EntityDestroyNotificationTests` (its own executable, because it replaces the
global allocation functions) covers notification isolation on every destroy
path, capacity preparation under allocation failure, the reentry rules, the
independent-world and competing-thread cases, destruction of the World, and set
destruction (order, duplicates, stale handles, snapshot, reservation failure).

The focused `RemovalHookLifetime.*:HookTest.*:ComponentHookSubscription.*`
selection contains 35 tests. An SDK consumer must additionally verify that the
engine registry sees callbacks compiled in the consumer, resource cleanup occurs
before reset, and a scope can be destroyed after the engine World has gone.

## Reflected-only component handlers during native reload

The lightweight generated registration path must revisit `RegisterBlobComponent`
on each load. Skipping an existing handler leaves reflected-only components
(such as a UI objective view with no typed ECS queries) stamped with the old
generation. The unload ledger then retains the superseded module indefinitely.

Only an engine-owned `BlobComponentHandler` can change generation without handler
replacement. A typed handler may have an old-DLL vtable; a blob registration for
that type must not clear its pin. Its typed registrar replaces the dispatch and
then advances ownership. Same, older and foreign module generations do not take
ownership of an existing blob entry.

Lightweight registration passes an existing component's registered size when
refreshing ownership. New reflected fields and default bytes still go through the
normal reload migrator; it repacks live columns before changing the handler size.
A same-name size change is not permission to alter live ECS strides during static
registration. This change does not alter failed-load purge or host unload policy.

`LiteComponentRegistrationTests` covers blob ownership reuse, typed-dispatch pin
preservation, generation isolation and size-changing field-preserving migration.
The first two regressions failed against the preceding Engine DLL. Passing these
registry tests proves the ownership mechanism; a full Editor reload/unmap cycle
is a separate integration check.

## Changelog

- 2026-09-18: entity destruction and removal-notification contract folded in from PR #1619.
- 2026-09-18: set destruction (`DestroyEntitiesImmediate`) folded in from PR #1621.
