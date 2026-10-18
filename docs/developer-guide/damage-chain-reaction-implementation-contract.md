# Damage Chain Reaction — implementation contract and status (2026-10-18)

## Required behavior

- The first native hit remains the ordinary game hit; it is not replaced and does not count toward the configured extra count.
- Enqueue exactly N additional applications for the original attacker/victim/effect. Each target's chain is independent; a new root never resets an existing chain.
- Rate controls cadence only, not chain lifetime. For N=100 at 100 applications/second, the target cadence is about 10 ms. After a stall, pending work remains queued; do not issue an unbounded catch-up burst.
- Count success only after a real matching `CharacterOnDamaged` result, not merely because a call was dispatched or a notification was broadcast.
- Chain-generated hits are marked by Core provenance and must not create another chain.
- Cancel safely when World/UObject generations change or relevant object handles become invalid. Never dereference stale UObject pointers.
- Hardcoded addresses are not used to dispatch the chain. Every selected reflected class, property, and function is validated against the active Profile before use.

## Dump evidence and current route

The supplied `5.6.1` Dump directly declares:

- `FHTDamageEvent` contains `DamageGEDef` and `DamageTags`.
- `AHTAbilityCharacter::CharacterOnDamaged` is a damage notification delegate. The separate `AHTAbilityCharacter::OnDamaged` reflected function has `DamageAmount`, `HitInfo`, `DamageTags`, `DamageGameplayEffect`, `InstigatorCharacter`, and `DamageCauser`.
- `ADamageTriggerActor` has a periodic GameplayEffect trigger event, `EventData`, `EffectContainer`, and runtime period/duration/count fields.
- `UHTAbilitySpawnActor.OverrideEffectContainer` and `HTGATargetType_UseEventData` provide the reflected route used to set an actor-owned effect container and its event-data target.

Declarations do not prove that this was the exact route used by the old plugin. The native-trigger route is a Dump-backed candidate with runtime reflection checks; matching it to the original plugin/video still requires further evidence.

The repository's `main` branch contains `5.6.1-new/` files as Git LFS pointers. Their source bytes were not available in the current branch review, so a complete `5.6.1` versus `5.6.1-new` declaration diff is **not yet established**. The current adapter must fail closed if the active Profile's reflected classes, properties, sizes, or offsets do not match the required checks.

## Current implementation in the independent branch

1. The plugin uses `RateQueue` to schedule finite extra work, with no queue expiry and no burst catch-up. It advances the count only when Core confirms a real hit.
2. Core validates the original non-chain-generated damage event, the player-controlled attacker, the hostile `HTAbilityCharacter` target, the `DamageGEDef` source, World generation, and object identity.
3. Core creates a `DamageTriggerActor`, sets its reflected event-data `Instigator` / `Target`, and invokes `OverrideEffectContainer` with the resolved effect class and `HTGATargetType_UseEventData`. Readback checks validate the actor class and configured fields.
4. Each queued item invokes the Dump-declared `DamageTriggerActor.K2_OnTriggerPeriodGamePlayEffect` event. The new service does **not** call `ApplyCapturedDamage` and does not replay the original ASC call's captured parameter buffer.
5. Core marks the synchronous nested damage with internal chain provenance and confirms only a matching positive damage event for the same World, attacker, target, and effect source. If a dispatch enters but no actual damage can be confirmed, it is not retried automatically; it remains pending rather than risk a duplicate.
6. Queue-managed actor configuration does not expire due to elapsed wall-clock time; the requested extra count is enforced by confirmed events. Stale World/UObject generations are cancelled without dereferencing old pointers.

The legacy `anomaly.nte.damage-replay` and legacy `anomaly.nte.damage-chain` services remain unpublished. The supported interface for this feature is the versioned `anomaly.nte.damage-chain-reaction` ABI declared in the public SDK and the packaged SDK copy.

## Validation status

- The packaged SDK's `services/nte.h` was out of sync with `include/anomaly/sdk/services/nte.h`. This was corrected on the independent branch; the full Windows workflow passed for that header-sync commit.
- The native-trigger integration is committed, but its Windows and Core-validation workflows must pass before treating the source as build-verified.
- No in-game test has yet established that one normal hit plus N queued events produces exactly N additional damage results or that the selected trigger event matches the old plugin's internal mechanism. Do not report it as a completed/working runtime plugin until this test is done.

## Acceptance case

With the feature disabled, one native attack produces one ordinary damage result. With it enabled, extra count = 100 and rate = 100/s, preserve the first hit, queue 100 additional applications against the same victim, aim for 10 ms spacing, and continue beyond one second if pending work remains. Every one of the 100 additional results must be confirmed individually; the original hit is excluded from that count. A clean build is necessary but not sufficient.
