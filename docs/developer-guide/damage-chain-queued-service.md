# Damage Chain queued native-effect API

## Behavioral contract

The original player damage event remains the first hit. `repeat_count` is the number of **additional** effect applications, so a request for 100 repeats is one observed root hit plus up to 100 queued native effect applications.

`rate_per_second` defines scheduling cadence only. It is not a duration, deadline, TTL, or cancellation timeout. The scheduler advances the next cadence slot by the requested interval, but never dispatches more than one application per chain per Game tick and never discards overdue repeats. It also waits for the prior application to correlate to one real native damage event before dispatching the next repeat on that chain. This prevents catch-up bursts and unbounded runs ahead of observed damage. If the Game thread or native damage path cannot sustain the requested rate, the queue continues across later ticks; 100/s is the requested cadence, not a guarantee the Game thread or game can meet.

A Core result of `OK` means the validated GameplayEffect call was dispatched; it does not prove health changed. The API reports `dispatched_repeat_count` separately from `observed_chain_damage_count`, which is incremented only when Core observes a corresponding native `CharacterOnDamaged` event.

## Implementation boundaries

- The new plugin uses `anomaly.nte.damage-chain`. It does not query or depend on the legacy public `anomaly.nte.damage-replay` service.
- Core currently reuses its existing, already dump-layout-validated GameplayEffect capture/replay implementation internally. The new public service owns the queue, count, cadence, lifecycle and diagnostics.
- The chain ID is scoped to the exact validated native GameplayEffect receiver/function during dispatch. Synchronous damage callbacks are accepted only when attacker, victim, source effect and world match the root chain. Delayed callbacks additionally require a one-to-one match to an unclaimed captured GameplayEffect/ASC/source/target invocation. A matching synchronous callback is still tagged as chain-generated to prevent recursion if invocation correlation is unavailable, but it does not increment the observed-hit counter without that one-to-one evidence.
- Every request must reference a retained original `CharacterOnDamaged` event from the current world/object generation, with valid attacker/victim handles and a resolved `DamageGEDef` source. Chain-generated events cannot be used as new roots. A retained root sequence can start at most one chain; stale or unidentifiable roots are rejected before queueing.
- Chains are limited to 32 concurrently tracked roots. The persistent dispatch-confirmation ledger has the same capacity because at most one application per chain may await confirmation. A chain is stopped early only by explicit cancellation, lifecycle/binding invalidation, or a native dispatch failure—not by elapsed time. World/object-generation invalidation releases obsolete confirmation tokens and marks unconfirmed work as invalidated rather than counting it as successful.
- The scheduler runs on the Game thread, limits each chain to one dispatch per tick, and waits for one correlated native damage callback before sending that chain's next repeat. It preserves the requested cadence slots across frame stalls without expiring the queue. It does not promise an exact 100/s when Game-thread cadence, native callback latency, or target state cannot sustain that cadence.
- The current combat service filters root events to the current controlled character; Core additionally validates that the captured source is the local player character, not a vehicle.

## Dump evidence and limitations

The implementation is based on the user-provided `5.6.1-0+UE5-HT` Dump. The verified native `FHTDamageEvent`, `CharacterOnDamaged` participants, and the parameter layouts for the four GameplayEffect entrypoint families are used by Core’s existing reflection/layout validator. The new queue introduces no fixed native RVA or guessed pointer offset.

The repository's `5.6.1-new` SDK files were returned by GitHub as Git LFS pointer files during this pass, not as the actual Dump contents. They were not treated as authoritative declarations. No new offset was inferred from those pointers.

## Validation status

The pull request's Windows/MSVC workflow builds Core, the generated ABI snapshot tool, and the new `anomaly_builtin_damage_chain` plugin; validates the generated ABI snapshot; checks the manifest identity and required service; and packages exactly `plugin.dll` plus `manifest.json`.

A successful CI build proves compilation and packaging, not in-game chain behavior. In-game validation must still verify that each dispatch produces a real damage event, that the correlated chain ID marks delayed events correctly, and that multiple target chains remain independent.
