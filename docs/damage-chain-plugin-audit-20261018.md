# Damage Chain Plugin Audit — 2026-10-18

## Scope

- Repository: `Rabb1t79/Anomaly-main`
- Dump source branch: `fix/restore-attack-work-and-repair-20261016`
- Plugin repair branch: `fix/damage-chain-plugin-ui-guards-20261018`
- Plugin path: `plugins/DamageEcho/plugin.cpp`
- This branch is an entry-guarded diagnostic build, not a verified recreation of the recovered legacy DLL.

## Dump cross-check for the hard-coded address

The user-supplied code tried to hook `HTGame.exe + 0x42D5E50` as if it were a direct C++ function with the five-argument `ApplyDamage` signature.

### What the repository's 5.6.1 Dump does confirm

1. `5.6.1/CppSDK/SDK/Engine_classes.hpp`, Blob SHA `d8e3399fc3e723159a9a4c86672c51150d467164`, declares:
   `UGameplayStatics::ApplyDamage(AActor* DamagedActor, float BaseDamage, AController* EventInstigator, AActor* DamageCauser, TSubclassOf<UDamageType> DamageTypeClass)` returning `float`.
2. `5.6.1/CppSDK/SDK/Engine_parameters.hpp`, Blob SHA `44869f6561b6a8af1a7d3ccb6f2887f4c0fd104c`, defines the reflected parameter layout:
   - `DamagedActor`: offset `0x00`
   - `BaseDamage`: offset `0x08`
   - padding: offset `0x0C`
   - `EventInstigator` (`AController*`): offset `0x10`
   - `DamageCauser` (`AActor*`): offset `0x18`
   - `DamageTypeClass`: offset `0x20`
   - `ReturnValue`: offset `0x28`
3. `5.6.1/CppSDK/SDK/Engine_functions.cpp`, Blob SHA `65714c47053096aeeb1da5fb51641834bea75584`, shows the generated wrapper resolving the reflected function with `StaticClass()->GetFunction("GameplayStatics", "ApplyDamage")`, filling the parameter struct, invoking `GetDefaultObj()->ProcessEvent(Func, &Parms)`, restoring function flags, and returning `Parms.ReturnValue`.
4. `5.6.1/Dumpspace/OffsetsInfo.json`, Blob SHA `32f04a7dc01a06eef454e11c0e53bea286c70446`, lists global offsets for GObjects, AppendString, GNames, GWorld and ProcessEvent plus the ProcessEvent index. It does not list a machine-code RVA for `UGameplayStatics::ApplyDamage`.

### What remains unverified

- No inspected 5.6.1 Dump entry maps `0x42D5E50` to `UGameplayStatics::ApplyDamage`.
- The SDK's generated wrapper signature is not evidence that `HTGame.exe + 0x42D5E50` has that callable ABI. Checking whether the address lies on an executable page cannot establish function identity or calling convention.
- The generated SDK wrapper is a reflected `ProcessEvent` call; the inspected public `AnomalyUe5ProcessEventServiceV1` only exposes subscribe/unsubscribe callbacks, not a general invocation function. The reviewed public NTE actor/combat services are observation/snapshot-oriented and do not expose a DamageTriggerActor spawn/configure entry point.
- `ADamageTriggerActor` and its periodic GameplayEffect-related fields/method declarations exist in the 5.6.1 Dump, but the old DLL's relationship to that system, its configuration values, and its exact first-target/multi-target semantics remain unproven.

## Code correction

The previous implementation was unsafe to treat as a working feature. The code directly called the unverified address using a guessed function-pointer type, retained raw UObject pointers in a repeat task queue, and could keep rescheduling damage without confirming that the target function and ABI matched the game.

The repaired source now:
- Does not install a hook on `0x42D5E50` and does not make direct damage calls.
- Performs only a memory-page sanity check on that candidate, clearly labelled as such; page executability is not treated as proof of function identity.
- Keeps a Chinese UI for entry status, a preset interval, a blocked-start diagnostic, and bounded log export. The interval control is informational until a supported/verified invocation route exists.
- Exports logs to the Windows temporary directory and records why the hook was not installed.
- Aligns source and manifest version at `1.5.0`, with only the UI capability declared because this guarded build does not use the Hook service.

## Verification boundary

- A successful DLL build validates compilation and packaging only.
- This guarded build intentionally does not perform chain damage. It must not be presented as a working legacy-mechanism recreation.
- The next implementation step needs either a 5.6.1/profile-validated function entry with a matching ABI, or a supported Anomaly Core service that safely resolves and invokes the appropriate reflected/game-native behavior on the Game thread. Do not invent a signature pattern or re-enable the raw offset based only on an executable-page check.
