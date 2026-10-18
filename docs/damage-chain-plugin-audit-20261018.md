# Damage Chain Plugin Audit — 2026-10-18

## Scope

- Repository: `Rabb1t79/Anomaly-main`
- Base branch: `feature/damage-echo-plugin-20261018`
- Working branch: `fix/damage-chain-plugin-ui-guards-20261018`
- Plugin: `plugins/DamageEcho/plugin.cpp`
- This is an experimental candidate. It is not a verified recreation of the recovered legacy DLL.

## Directly verified from repository declarations

1. `include/anomaly/sdk/services/interop.h` defines `AnomalyHookServiceV1` with `create`, `release`, `begin_callback`, and `end_callback`.
2. `include/anomaly/sdk/services/ue5.h` defines `AnomalyUe5FrameworkServiceV1`, including the `is_game_thread` query. The plugin can therefore decline to call the candidate damage function outside the framework-confirmed Game thread.
3. `include/anomaly/sdk/services/ui.h` declares `checkbox`, `slider_float`, `button`, `separator`, and `same_line`; the revised UI uses those existing fields rather than inventing new UI ABI entries.
4. The repository Dump's `5.6.1/CppSDK/SDK/HTGame_classes.hpp` declares `ADamageTriggerActor`, including periodic trigger state and GameplayEffect trigger methods. `HTGame_structs.hpp` includes damage-trigger configuration and GameplayEffect-container structures. This establishes a native-system research lead, not proof that the legacy DLL used it.
5. The inspected public NTE actor/combat services are snapshot/read-oriented. No reviewed public service table exposes a general-purpose DamageTriggerActor spawn/configure operation. The Dump declarations alone do not authorize direct calls into the game.

## Not established

- `0x42D5E50` has not been verified as the intended damage function for the relevant HTGame build.
- The candidate function's native signature, parameter layout, and return type have not been proven by a matching Dump declaration or a validated signature record.
- The legacy DLL's internal mechanism has not been recovered, so periodic calls to this candidate function must not be described as equivalent to the original plugin's chain-reaction mechanism.
- An executable-image page check only rules out some invalid addresses; it does not identify a function or establish its ABI.
- There has been no in-game validation in this audit.

## Source changes on this branch

- Added a capability-declared `anomaly.ue5.framework` dependency and Game-thread gate; repeated damage is skipped when the framework cannot confirm the Game thread.
- Kept independent per-target task slots. A subsequent hit on an already registered target refreshes the damage amount without resetting that target's due time.
- Added a bounded interval control (50–2000 ms, default 200 ms), active-target/counter display, manual task clearing, a bounded diagnostic ring, and log export to the Windows temporary directory.
- Added checks for an executable committed page within the `HTGame.exe` image before registering the candidate hook. The UI and log explicitly state that function identity/signature remain unverified.
- Aligned the plugin descriptor version with the manifest (1.4.0), included the header needed for `std::size`, and added static assertions for the used SDK callback signatures.

## Verification status

- Repository source and manifest were re-read after edits; manifest JSON parses as valid JSON.
- Compile status must be taken from the GitHub Actions result for this branch. A successful compile alone does not prove that the candidate function or chain-reaction behavior is correct.
- Do not ship this candidate as a validated legacy-mechanism reproduction until offset/signature and runtime behavior are independently verified against the supplied repository/Dump and permitted Anomaly interfaces.
