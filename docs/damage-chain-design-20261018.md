# NTE Damage Chain — design and validation record

Date: 2026-10-18

## Intended behavior

An original player-character damage event is the root of a chain. With a repeat count of 100 and a rate of 100 per second, the queue requests 100 additional native GameplayEffect applications after the first hit. The count is an additional count, not an overall count. The rate sets the desired cadence only; there is no fixed one-second lifetime. If Game-thread execution is delayed, due work is retained and continues later. Native dispatch success is not counted as damage success until a matching real `CharacterOnDamaged` event is observed.

The plug-in ignores events positively identified as chain-generated when deciding whether to create a new chain. Each chain retains its own root event, attacker, target, DamageGEDef identity, world/object generations, and confirmation tokens. A stale generation, invalid target, missing function layout, unsafe tag-array capture, or failed native dispatch stops that chain rather than guessing.

## Evidence used

The code targets functions declared in the user-provided `5.6.1-0+UE5-HT` dump:

- `HTAbilitySystemComponent.K2_ApplyGameEffectWithClassByActor`
- `HTAbilitySystemComponent.K2_ApplyGameplayEffectToTargetWithClass`
- `HTAbilityCharacter.K2_ActivateGameEffectsWithClassByActor`
- `HTAbilityCharacter.ServerActivateGameEffectsWithClassByActor`
- `AHTAbilityCharacter.CharacterOnDamaged` and `FHTDamageEvent` for native damage observation.

The dump documents different parameter layouts for the ASC, Character wrapper, and server function. In particular, `ServerActivateGameEffectsWithClassByActor` takes a GameplayEffect object pointer rather than the same class argument used by the other wrappers; code paths are not interchangeable.

The Core resolves exact reflected function paths and checks their live `UFunction` owner, argument count, parameter-buffer size, return offset and required property names/types before capture or dispatch. There are no hardcoded `HTGame.exe + RVA` attack/effect calls. Captured parameter bytes are retained only with a matching world/object generation. The two embedded GameplayTagContainer arrays are copied into owned bounded storage and rebuilt for each dispatch; stale TArray pointers are not reused.

The new public service is `anomaly.nte.damage-chain`. The older public `anomaly.nte.damage-replay` service is not exported by this branch. Internally, repeat dispatch is private to the chain service.

## Dispatch and confirmation

1. The Core captures a real CharacterOnDamaged event and associated source/target generation handles.
2. The plug-in may submit that root sequence with an additional repeat count and desired rate.
3. Before accepting the queue, Core drains pending captures and requires a validated matching GameplayEffect invocation, safely copied tag arrays, a player-controlled character attacker at hit time and a non-friendly target check.
4. Each native call has a one-use confirmation token. The next repeat is gated on observing the corresponding real damage event; the event must match root source, attacker, target, world/object generation and token provenance.
5. UI separates accepted requests, native applications dispatched, and real damage callbacks observed. A dispatch returning successfully is not labeled as damage success.
6. Queue lifetime is count/state based, not a wall-clock timeout. Game/object/world changes invalidate stale contexts. User disable/stop cancels future work; already dispatched damage is not rolled back.

## Rate semantics and bounds

- Default plug-in configuration: 100 additional applications, desired rate 100 per second.
- Maximum public limits are bounded in the ABI: 100,000 additional applications, 1,000 per second and 32 simultaneous chains.
- Work runs on the Game thread. The scheduler uses bounded dispatch work and retains overdue count rather than expiring it. Actual rate depends on the game's event/tick behavior; the requested rate is not a guarantee that every environment can sustain 100 damage events per second.
- Independent targets have independent chain records. The original hit is never replayed as player input, keypress, skill activation or animation.

## Dump-version limitation

The current available, identified dump is `5.6.1-0+UE5-HT.zip`. At this pass, a separate `5.6.1-new` archive/path was not available in the repository or file library, so no claim is made that a second dump version was compared. Instead, each live function layout is revalidated and the feature remains unavailable on mismatch. Do not add speculative offsets from an unavailable dump.

## Validation required before declaring gameplay support

Automated build/ABI checks validate compilation and layout reporting only. A live game test is still required to validate this causal chain:

`root CharacterOnDamaged` → `captured GameplayEffect invocation` → `one native additional application` → `one matching real CharacterOnDamaged event`.

Test separately with one target, two targets, chain counts 1 and 100, several rates, a target defeated mid-chain, world/character changes, and plug-in disable while a repeat is in flight. Confirm that dispatched count and observed real-damage count agree before marking a chain completed. No successful in-game chain is claimed by this document.
