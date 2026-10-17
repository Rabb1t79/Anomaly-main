# Damage-chain recreation work log

## Scope and isolation

This branch, `recreation/damage-chain-native-20261017`, was created from `main` on 2026-10-17. It is separate from the existing Attack Replay branches. Do not merge this work into an Attack branch without explicit authorization.

The target is the user's old-plugin damage-chain behavior. It is not defined as a key/input replay, skill activation replay, or animation replay. The old DLL remains the first source for internal behavior only if reliable code or behavior logic can be recovered; current analysis has not recovered such code.

## Evidence established from this repository

- The current `main` SDK declares `anomaly.nte.combat v1`, including a damage-event stream whose comments identify `AHTAbilityCharacter::CharacterOnDamaged` as the event source.
- The current `main` SDK header does not declare `anomaly.nte.damage-replay`; that service exists in a separate, divergent Attack Replay work branch. Its existence there does not establish that its current replay route is correct for this recreation.
- The old DLL has nonstandard PE section layout and high-entropy data, but no validated decryptor, key, or recovered original function body has been found. Therefore its internal trigger, target tracking, and stop conditions remain unknown.
- The repository's existing `src/game/ue5/ue5_nte_adapter.cpp` and `src/game/ue5/ue5_damage_function_hook.cpp` are the main in-repository locations to inspect for the supported damage-event and reflection-hook path.

## Implementation gates

1. Keep this branch isolated from Attack Replay. Existing work may be consulted as a reference where needed, but its Replay assumptions must not be inherited automatically.
2. Establish from the supplied game Dump and current Anomaly source which native GameplayEffect entry point is valid, its full reflected signature/layout, its object and thread constraints, and whether a supported service can safely invoke it.
3. Add or adapt a stable Core/SDK ABI only after the native path is validated. The ABI must validate request fields, object generations, World/thread/lifecycle state, and fail closed on ambiguity.
4. Build the chain around a real damage event and independent per-target chain state. Do not equate a successful function return with an actual new hit; only a subsequent matching native damage event is evidence of effect.
5. Keep claims separate: static analysis, Dump verification, Core/ABI build, plugin build, load, observed runtime behavior, and controlled in-game validation.

## Current status

- Old DLL recovery: **blocked / unverified** (no reliable decryption or original code recovered).
- Clean recreation branch: **created**.
- Native damage-event surface in current SDK: **directly confirmed**.
- Native GameplayEffect re-application route on current `main`: **not yet implemented or validated**.
- In-game behavior: **not tested**.

## Initial source inspection (2026-10-17)

- `include/anomaly/sdk/services/nte.h` on `main` defines the public damage event record and combat service. Its comments identify `AHTAbilityCharacter::CharacterOnDamaged` as the capture source. This confirms the root-event observation surface, not a safe effect-reapplication API.
- `src/game/ue5/ue5_damage_function_hook.cpp` implements the `anomaly.nte.damage-native / character-on-damaged` hook. It observes and forwards the original call; it does not itself establish a supported GameplayEffect re-application route.
- `profiles/nte/nte-current.json` declares `nte.HTAbilityCharacter.BroadcastCharacterOnDamaged` and layout values including `damageEvent.size = 368` and `damageEvent.damageGEDef = 312`. These are current repository Profile values; they must still be checked against the user's authoritative 5.6.1 Dump before being treated as sufficient for a new invocation path.
- The current Profile does not, in the inspected function entries, provide a verified GameplayEffect apply entry point for the chain. The next implementation gate is to validate such an entry point and full reflected signature from the provided Dump and the current Anomaly reflection/hook code. Until then, implementing a call based only on the old Replay branch would repeat an unverified assumption.

## First code change: diagnostic-only wrapper observation

Commit `a1b5aa49e516275b37c267edb864c7f9a4591174` adds optional exact-path discovery and a call counter for `/Script/HTGame.HTAbilityCharacter.K2_ActivateGameEffectsWithClass`.

The discovery gate checks the reflected function against the Dump-derived expected signature: parameter buffer size `0x68`, `WorldContextObject` (`ObjectProperty`, 8 bytes), `GameplayEffect` (`ClassProperty`, 8 bytes), and `ModifyData` (`StructProperty`, 0x58 bytes). The new counter records calls only when the exact reflected function binding passes the existing metadata validator.

This is **observation only**. The implementation does not read or retain `ModifyData`, does not call the function, does not add it to any replay whitelist, and does not claim the wrapper carries attacker/target identity. The diagnostic binding-mask bit is `1 << 6`; the call counter is exposed through the internal `NteCombatDiagnosticsSnapshot`.

### Validation still required

- Windows Core build and tests have not yet completed for this commit.
- In-game occurrence of this wrapper has not been observed yet.
- A positive count would show only that the function executes; correlation with a specific `FHTDamageEvent` and its `DamageGEDef` remains required.
- Zero calls would not prove the path is unused, because the call may be absent for the tested attack or may be dispatched through a different native route.


## Additional diagnostic observations

The isolated branch now observes five exact Dump-defined GameplayEffect-related paths. In addition to `HTAbilityCharacter.K2_ActivateGameEffectsWithClass`, it counts calls to `HTAbilityCharacter.K2_ActivateGameEffectsWithClassByActor`, `HTAbilitySystemComponent.K2_ApplyGameEffectWithClassByActor`, `HTAbilitySystemComponent.ApplyEffectContainerSpec`, and `HTAbilitySystemBlueprintLibrary.ApplyExternalEffectContainerSpec`. These are read-only counters after exact-path and reflected-layout validation; no raw parameter buffer is retained, and none of these candidates is invoked by this change.

The first CI attempt stopped during CMake configuration because `plugins/TimeAccel/plugin.cpp` is absent while the root build still declared it as a target. The branch now conditionally declares and stages that target only if its source exists. It does not restore unrelated historical source code. This is a build-check workaround for the missing file, not a claim that a full release package has been validated.


The diagnostic set also includes `HTAbilitySystemComponent.K2_ApplyGameplayEffectToTargetWithClass` (`ParmsSize=0x78`), with the exact Dump-defined parameter order: GameplayEffect class, target ASC, FBufferData, EBuffEventType, prediction bool, and FActiveGameplayEffectHandle return. Its reflected enum/property type and all field sizes must pass the live metadata validator before its counter can be enabled. This counter remains observation-only.

These counters establish whether candidate entry points execute during a test. They do not, by themselves, identify the triggering attack or prove that invoking any candidate again would reproduce a hit. The next step after a successful build is to correlate observed call counters with real `CharacterOnDamaged` events and the matching `DamageGEDef`; no replay invocation has been added.


The read-only counters are now included in the existing combat diagnostics JSON under `gameplayEffectRoutes`. Each candidate has a `Bound` boolean and a `Calls` count. A bound candidate means the exact path and reflected metadata passed discovery; a non-zero count means that reflected function was observed through the existing Game-thread ProcessEvent tap. Neither condition alone proves causal linkage to a specific hit. No native effect call or replay behavior is enabled by these diagnostics.


The reflected binding validator now also checks the exact UStruct object name for the candidate's pointer-backed struct parameters: `BufferData`, `HTGameplayEffectContainerSpec`, and `ActiveGameplayEffectHandle`. This avoids treating an arbitrary same-sized struct as equivalent. The enum parameter remains fail-closed on its expected reflected property type; if the live metadata uses a different property representation, that candidate will stay unbound rather than bypass validation.
