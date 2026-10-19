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

## CI update — 2026-10-18, source commit `9a53edc3b398e559ca469bdd55c507b74216d3e2`

- The corrected standalone workflow [39125202472](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202472) passed on this source commit: independent plugin build succeeded, deterministic RateQueue test passed (`1/1`, `100% tests passed`), and the package upload succeeded.
- Artifact: `DamageChainReaction-windows-x64`, SHA-256 `9e08a8a74ebc3489a6d9efd48c3ffc6767dacd8263fe461492196dfb9564d06a`. This is a **test package**, not a runtime-verified release.
- At the time of this update, full Windows workflow [39125202479](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202479) and Core/ABI workflow [39125202470](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202470) were still in progress. Their results must be checked separately.
- In-game damage behavior remains unverified: a green build/queue test proves compilation and scheduler behavior only, not that the DamageTriggerActor event creates exactly one additional real damage result each time.

## Dump evidence coverage correction — 2026-10-18

- Expanded evidence run [39125616938](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125616938) completed successfully after checking out the selected LFS objects from `main`.
- The report extracted all 15 focused blocks successfully with **zero missing markers**; all 15 focused blocks were text-identical across `5.6.1` and `5.6.1-new`. Newly covered blocks include `ADamageTriggerActor`, `AHTAbilitySpawnActor`, `FHTGameplayEffectContainer`, `FSpawnDamageTriggerActor`, `FSpawnDamageTriggerActorArray`, `FDamageTriggerActorStaticData`, and `HTAbilitySpawnActor_OverrideEffectContainer`.
- The relevant declaration details support the candidate adapter at declaration level: `ADamageTriggerActor` has `RuntimeTriggerPeriod` at `0x5E8`, `RuntimeTriggerDuration` at `0x5EC`, `RuntimeTriggerCountPerPeriod` at `0x5F0`, and `RuntimeMaxTriggerCount` at `0x5F4`; its base `AHTAbilitySpawnActor` has `EffectContainer` at `0x3A8` and `EventData` at `0x3D8`; `OverrideEffectContainer` takes one `FHTGameplayEffectContainer` parameter of size `0x28`.
- The complete `HTGame_classes.hpp`, `HTGame_structs.hpp`, and `GObjects-Dump.txt` files still have different hashes between versions, despite the focused blocks matching. These matches do not prove a reflected call's runtime semantics or that it causes damage. In-game confirmation remains required.

## CI result refresh — 2026-10-18

- The source commit `9a53edc3b398e559ca469bdd55c507b74216d3e2` has now passed all three relevant CI workflows: independent plugin + RateQueue test/package [39125202472](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202472), full Windows framework/runtime/package build [39125202479](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202479), and Core/ABI validation [39125202470](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125202470).
- The subsequent commit `eed4357560b63c8a6b5619465102861713080d2f` changed only evidence extraction/workflow/docs; its expanded two-Dump evidence run [39125616938](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39125616938) passed and all 15 extracted blocks matched textually.
- All green CI results still do not confirm in-game behavior. The decisive remaining test is that one native hit followed by N period dispatches creates exactly N new real damage events for the original target.

## Additional ABI-layout evidence check — 2026-10-18

- A follow-up audit of Core's native-trigger adapter found one more declaration dependency that must be covered explicitly: the adapter verifies `AHTAbilitySpawnActor.EventData` as a `0xB0` struct and resolves its child `Instigator` / `Target` object properties at offsets `0x08` / `0x10`. The old Dump declares `FGameplayEventData` at size `0xB0` with those fields at those offsets, but the previous cross-version evidence report did not include the GameplayAbilities package's actual struct block.
- The evidence extractor and LFS workflow now also load and compare `CppSDK/SDK/GameplayAbilities_structs.hpp` and extract `FGameplayEventData`. This makes the cross-version proof cover the nested structure whose field offsets are consumed by the adapter rather than relying only on the outer `EventData` property.
- No hard-coded offsets were changed in this patch. Wait for the new evidence job to finish and inspect the exact block diff before deciding whether the adapter's `FGameplayEventData` layout check is valid for both versions.
## Evidence result update — 2026-10-18

- The expanded evidence run [39126184168](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39126184168) completed successfully after materializing `GameplayAbilities_structs.hpp` from LFS in both Dump versions.
- The latest report contains **16 focused declaration blocks**: all 16 extracted successfully, zero missing markers, zero unavailable diffs, and all 16 report `No textual difference in extracted block.`
- `FGameplayEventData` matches in both versions: size `0xB0`, `Instigator` at `0x08` (8-byte object property), `Target` at `0x10` (8-byte object property). `GameplayAbilities_structs.hpp` is 150,574 bytes in both versions with SHA-256 `992b87956ae12d195bb8d1a3b492e4b9c5ef520b5bf5acedadcff6496fa6a556`.
- The refreshed evidence report at `/mnt/data/NTE-damage-Dump-evidence-5.6.1-vs-5.6.1-new.md` has SHA-256 `4c74d25bc53d3ccf2e3574aeb65011c3d2735f8eae3c5878654c61f802eb57e2`. The HTGame classes, HTGame structs, and GObjects full-file hashes still differ between versions. This closes the nested declaration-layout gap but still does not prove the period event results in real damage in game.

## Target-selection type evidence coverage — 2026-10-18

- Core configures the native container with `HTGATargetType_UseEventData` and validates it as a child of `HTGATargetType`. The prior 16-block report covered the container and nested `FGameplayEventData` layouts, but did not extract the target-type class blocks themselves.
- This revision adds both `UHTGATargetType` and `UHTGATargetType_UseEventData` to the paired LFS evidence extractor and adds the base type to the GObjects query. Wait for the new report to confirm these class blocks exist and match in both Dump versions; do not infer the target-type behavior from its name alone.
## Target-type declaration result — 2026-10-18

- Evidence run [39126720146](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39126720146) completed successfully. The report now contains **18 focused blocks**, with zero missing markers, zero unavailable diffs, and all 18 blocks text-identical across `5.6.1` and `5.6.1-new`.
- The new blocks confirm `UHTGATargetType` has `GetTargets(OwningActor, TargetingActor, FGameplayEventData&, OutTargetData, OutHitResults, OutActors) const`; `UHTGATargetType_UseEventData` has no extra fields and inherits that function. This provides a declared target-resolution interface that receives `FGameplayEventData` and returns actor/target-data outputs.
- This still does not prove that the concrete `GetTargets` implementation returns `EventData.Target` in all relevant game states. Its runtime behavior needs a read-only in-game probe or a trace of the native event path before the adapter can be considered verified.
- Refreshed report path `/mnt/data/NTE-damage-Dump-evidence-5.6.1-vs-5.6.1-new.md`; SHA-256 `2b7b9b5657b00571dced930f5dfe0a7f2f5e759cc4259421514700efa0ccdc8f`.
## Nested callback / actor lifecycle review — 2026-10-18

- Static call-order review found no obvious same-thread lock re-entry deadlock in the reviewed path: `ApplyNextDamageChainReaction` marks the chain as awaiting confirmation while holding the adapter state mutex, releases that mutex before invoking the reflected period event, then reacquires it and drains captured damage events. The native damage-hook dispatcher releases its short hook-management lock before entering the original handler and callback.
- Provenance is set around the direct `K2_OnTriggerPeriodGamePlayEffect` call and restored on scope exit. This is structurally compatible with a synchronous nested damage callback, but the generated event’s chain ID must be captured at callback time; the code/CI review does not prove this at runtime.
- Important unresolved lifecycle risk: `SpawnNativeDamageTriggerActorLocked` completes the deferred actor spawn before `ConfigureNativeDamageTriggerActorLocked` writes `EventData`, `EffectContainer`, and the runtime period/count fields. Dump declarations do not establish whether completion auto-starts any actor timer or trigger logic. The feature code explicitly calls `K2_OnTriggerPeriodGamePlayEffect` for each queue item and does not explicitly call `StartTriggerGamePlayEffect` / `TriggerPeriodGamePlayEffectEvent`, but it has not proved the actor cannot independently schedule damage after spawn.
- Do not patch the lifecycle order or period fields by intuition. Next action is to validate the trigger actor’s actual lifecycle and event semantics from project evidence/runtime diagnostics, then make a narrowly scoped fix if the behavior is established. The latest evidence and queue tests remain valid declaration/scheduler checks only.
## Runtime probe workflow correction — 2026-10-18

- Audit found `.github/workflows/nte-damage-chain-runtime-probe.yml` was mislabeled: it packaged `plugins/NteAttackReplay` v1.9.7 and had no `push` trigger, so it did not produce a Runtime probe for this feature on independent-branch updates.
- The workflow now stages/verifies `plugins/DamageChainReaction`, checks manifest ID `anomaly.builtin.nte-damage-chain-reaction` and version `0.1.0`, emits `DamageChainReactionRuntimeProbe-windows-x64.zip`, and runs for relevant pushes as well as pull requests/manual dispatch.
- This corrects packaging/automation only. A successful Runtime build or artifact does not prove the native period event creates exactly one hit per queue item; the actor lifecycle and in-game hit confirmation remain separate validation items.
## RateQueue acceptance coverage expanded — 2026-10-18

- Added deterministic tests for a five-second pause: the chain remains eligible, only one confirmed item is consumed when execution resumes, and the next item is scheduled one configured interval later rather than issuing a catch-up burst.
- Added a full 100-item progression test: the queue remains active until all 100 confirmations are committed and reaches completion only after the 100th success. This validates scheduler/count semantics; it does not simulate native damage or prove each Core dispatch hits the game target.
- Verified: independent plugin workflow [39127512880](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39127512880) passed and its RateQueue test passed 1/1 on commit `8dd451f2abb7079a0ec2b275b025305186007888`.
## Latest verified build state — 2026-10-18, commit `8dd451f2abb7079a0ec2b275b025305186007888`

- Independent `DamageChainReaction` plugin build, RateQueue tests, and plugin package passed: [run 39127512880](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39127512880). Test report: `1/1` passed, `100% tests passed`; artifact `DamageChainReaction-windows-x64`, SHA-256 `4b609129500c3db882ccf0bb25a33d7a010486ee2d534a9438ad8b42d4fc8811`.
- Core and ABI snapshot validation passed: [run 39127512883](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39127512883). Artifact `nte-damage-chain-core-validation`, SHA-256 `73568e4be73ac5fe38acf61eec272b1d9f3030d37bc28de01ab8daaff26df564`.
- Full Windows build and ABI/package checks passed: [run 39127513011](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39127513011).
- Runtime probe successfully built and staged the current `plugins/DamageChainReaction` Runtime package: [run 39127512930](https://github.com/Rabb1t79/Anomaly-main/actions/runs/39127512930). Artifact `DamageChainReactionRuntimeProbe-windows-x64`, SHA-256 `59c9a40a63dc44b56ee7196f02a332aeaa769ddcd8f391c6884f50c8107be080`.
- These results prove build/ABI/package checks for the specified commit. They are not an in-game execution result: the native period event's actual damage outcome, exact N-hit count, target stability, and absence of autonomous extra triggers remain unverified. In particular, the post-spawn field-configuration order remains a documented lifecycle risk pending runtime evidence.