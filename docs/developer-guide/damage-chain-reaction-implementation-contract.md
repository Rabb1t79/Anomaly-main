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

`main` stores `5.6.1-new/` Dump files in Git LFS. This was not a permanent blocker: workflow run [39117662599](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39117662599) successfully checked out the selected files with LFS enabled and produced the `nte-native-damage-dump-evidence` artifact. Its focused declaration diff reported no textual difference for `FHTDamageEvent`, `FBufferData`, `UHTAbilitySystemComponent`, `AHTAbilityCharacter`, `UHTAbilitySpawnDamageTriggerActor`, `K2_ApplyGameEffectWithClassByActor`, `K2_ApplyGameplayEffectToTargetWithClass`, and `HTAbilityCharacter_OnDamaged`. `HTGame_parameters.hpp` has the same SHA-256 in both versions; `HTGame_classes.hpp`, `HTGame_structs.hpp`, and `GObjects-Dump.txt` differ. This is a focused block comparison, not proof the full Dumps are identical or that a specific runtime function is safe to invoke. The current adapter must fail closed if active-Profile reflection, parameter sizes/offsets, object identity, or call preconditions do not match.

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
- Core validation passed on the last checked head (`2e073e95d1a27d9d8aeec610b3975482a1746a0f`, run [39124127839](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39124127839)); the full Windows workflow for that same head was still running when last checked ([39124127821](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39124127821)). A previous full Windows run passed after SDK header synchronization, but that does not replace validation of later commits.
- The independent build workflow was pointing at the legacy `plugins/NteDamageChainReaction` source rather than this feature's `plugins/DamageChainReaction`. This revision corrects the target and adds deterministic `RateQueue` tests; its new workflow must pass before the scheduler change is called build-verified.
- No in-game test has yet established that one normal hit plus N queued events produces exactly N additional damage results or that the selected trigger event matches the old plugin's internal mechanism. Do not report it as a completed/working runtime plugin until this test is done.

## Acceptance case

With the feature disabled, one native attack produces one ordinary damage result. With it enabled, extra count = 100 and rate = 100/s, preserve the first hit, queue 100 additional applications against the same victim, aim for 10 ms spacing, and continue beyond one second if pending work remains. Every one of the 100 additional results must be confirmed individually; the original hit is excluded from that count. A clean build is necessary but not sufficient.

## 2026-10-18 progress update — LFS evidence and scheduler validation

- The successful LFS evidence workflow materialized the selected `5.6.1/` and `5.6.1-new/` files from `main`. Focused declaration blocks listed above are text-identical; the evidence artifact explicitly warns that this does not prove a candidate function is safe to call.
- The service implementation was moved away from the old captured-GameplayEffect buffer path. The current `apply_next` route invokes the validated `DamageTriggerActor` period function through Core's guarded reflected invoker. That establishes what this branch currently calls; it does **not** establish that the call actually creates one real hit in the game.
- This revision hardens the rate scheduler against non-finite rate values and rounds interval durations up so the scheduled cadence cannot exceed the selected rate due to duration truncation. It also adds deterministic queue tests and fixes the standalone workflow so it builds/packages `plugins/DamageChainReaction`, not the legacy `plugins/NteDamageChainReaction`.
- Pending proof: new workflow green; in-game result of 1 normal hit plus exactly N confirmed extra hits; no recursive chain roots; independent targets; and continued pending work after a long stall.

## UI contract correction — 2026-10-18

- The extra-count control now uses the plugin's declared bounds: default `1`, minimum `1`, maximum `1,000,000`. The previous hard-coded UI maximum `10` contradicted the Core and plugin count limits and blocked the required 100-extra-hit acceptance case.
- UI and manifest descriptions now name the current native `DamageTriggerActor` / `EffectContainer` route instead of describing a direct ASC GameplayEffect replay route. This is a description and input-range correction, not proof the native period event produces the intended damage in game.
