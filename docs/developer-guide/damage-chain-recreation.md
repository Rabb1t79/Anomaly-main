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
