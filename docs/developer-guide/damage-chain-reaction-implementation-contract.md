# Damage Chain Reaction — implementation contract (2026-10-18)

## Required behavior

- The first native hit is the ordinary game hit and is not replaced.
- When enabled, capture that first hit once and create a per-chain context bound to the original attacker, victim, damage-effect definition, damage tags, and any additional context proven necessary by the supplied 5.6.1 / 5.6.1-new dumps.
- Enqueue exactly N additional damage applications. N excludes the original hit.
- Rate controls scheduling only. It is not a chain lifetime or expiry. For N=100 and 100 applications/second, the target cadence is approximately 10 ms; if a frame/tick stalls, remaining items stay queued. Do not perform an unbounded burst to catch up.
- Each queued item must enter a validated native game damage/effect application path and produce an actual game damage result. Re-broadcasting a notification alone does not count.
- A queued item created by a chain must not create another chain. Different attacker/victim chains must have independent state.
- Cancel safely if world/session generation changes or the source/target handles become invalid. Do not dereference stale UObject pointers.
- Never hardcode an address/offset solely because it appears in a dump. Any RVA/offset candidate must be tied to an exact dump version, module identity, signature/validation, and a guarded failure path.

## Evidence currently available from the supplied 5.6.1 dump

- `FHTDamageEvent` carries `DamageGEDef` and `DamageTags`.
- `AHTAbilityCharacter::CharacterOnDamaged` is a multicast delegate with damage event, damaged character, instigator character, and damage causer.
- `AHTAbilityCharacter::OnDamaged` is a separate reflected function with `DamageAmount`, `HitInfo`, `DamageTags`, `DamageGameplayEffect`, `InstigatorCharacter`, and `DamageCauser`. Do not conflate the delegate with this reflected function.
- The dump declares several GameplayEffect application functions, including `UHTAbilitySystemComponent::K2_ApplyGameEffectWithClassByActor`, `K2_ApplyGameplayEffectToTargetWithClass`, and Character-level activation functions. Their presence does not prove which one creates the desired damage semantics.
- `ADamageTriggerActor` exposes periodic trigger configuration and GameplayEffect-related entry points. It remains a candidate, not an established implementation route.

## Required implementation stages

1. **Observation:** subscribe/read supported Anomaly combat events and log sequence, source, target, effect identity where available, and timestamps. This stage must be read-only.
2. **Correlation:** correlate the observed event with the dump-defined damage payload and determine which fields must be preserved to produce a genuine second damage application.
3. **Native apply adapter:** use an existing supported ABI if it genuinely exposes the needed semantics; otherwise add a narrowly scoped, versioned Anomaly Core ABI backed by dump-validated game calls. No invented service IDs or function signatures.
4. **Queue:** use a monotonic clock and per-chain deadline schedule; keep pending count independent from elapsed time. Rate changes affect future deadlines only.
5. **Re-entry guard:** mark chain-generated applications with internal provenance so they are observed but cannot recursively start another queue.
6. **Verification:** compare queued count with actual damage results and target HP deltas; log failed calls without pretending they succeeded.

## Current blocker / do not fake

The public combat service demonstrated by `examples/nte_combat_demo` is an observation interface. The skill invocation service activates a skill; it is not a generic GameplayEffect application API. Do not misuse either interface to synthesize damage. Until the native application adapter is proven from the supplied dump and the current Core ABI, the plugin must remain observation-only rather than shipping a fake “working” chain.

## Acceptance case

With the feature disabled: one native attack causes one normal damage result.

With the feature enabled, extra count = 100 and rate = 100/s: preserve the initial hit, enqueue 100 extra applications to the original victim, aim for 10 ms spacing, continue after one second if items remain, and finish only when all 100 have succeeded or a logged safe-cancel condition occurs. Confirm the 100 extra actual results individually; the original hit is not counted among the 100.
