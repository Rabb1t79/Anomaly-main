# NTE Vehicle: Tokky Baseline and UI Requirements

This document records project-local reverse-engineering evidence and UI rules for future NTE Vehicle changes. Evidence is restricted to the user-provided `Tokky-NTE.dll`, `launcher.exe`, `tokky.zip`, the `5.6.1-0+UE5-HT` native dump, and this repository. Do not supplement Tokky claims with external network sources or invent reflected functions that are absent from the supplied dump.

## Permanent Vehicle UI invariant

**The summon/catalog list must live in its own bordered child frame with its own vertical scrollbar.** The child has a fixed height and scrolls independently; other controls, selected-item/status text, speed controls, torque controls and chassis settings must not move inside the catalog's scroll area. When changing the UI, retain the separate frame and independent scrollbar. Render the entire filtered catalog inside the child rather than silently truncating it. Prefer the existing append-only `AnomalyUiServiceV1.begin_child/end_child` ABI when the Host advertises those fields; only use a compatibility fallback when that ABI is genuinely absent, and always balance begin/end calls.

The existing Host UI implementation maps the child flags directly to `ImGuiChildFlags`; flag `1` is the bordered-child flag used by this UI. No new UI ABI is needed for this layout.

## Tokky evidence found in the supplied binary

The readable `.text` references and nearby strings in `Tokky-NTE.dll` establish these call-path facts:

- `HTVehicleDataAsset` is referenced from the native code.
- Current vehicle lookup strings are `BP_GetCurrentDriveVehicle` and `BP_GetCurrentDirvingVehicle`; the latter spelling matches the supplied native dump.
- The summon-related path references `GameplayStatics.BeginDeferredActorSpawnFromClass` and `GameplayStatics.FinishSpawningActor`.
- The wheeled-vehicle branch identifies `HTWheeledVehicleDrivable` and looks up `SafeSetVehicleTransform` in the reflected function list, then dispatches it via UObject virtual dispatch.
- The supplied `launcher.exe` contains UI/settings strings for “Summon Vehicle”, “Vehicle Speed”, `bind.summon_vehicle.key`, `bind.summon_vehicle.mode`, `bind.vehicle_speed.mode`, `vehicle_speed.enabled`, and `vehicle_speed.multiplier`. These confirm launcher controls, not the full protected speed implementation.

Static disassembly is not a complete source reconstruction: the DLL has custom `.tkn0` / `.tkn1` sections whose virtual ranges overlap. Do not claim a protected method's exact algorithm unless it is established by readable instructions and the supplied dump or a controlled live trace.

## Confirmed by the supplied 5.6.1-0+UE5-HT dump

- `HTGame.VehicleData.VehicleID` is at row offset `0x08`.
- `HTGame.VehicleData.InVehicleTemple` is a `TSoftClassPtr<UClass>` at row offset `0xE0`, size `0x28`.
- `UHTVehicleDataAsset.DT_VehicleData` is at offset `0x58`.
- `HTPlayerController.BP_GetCurrentDriveVehicle` and `HTPlayerCharacter.BP_GetCurrentDirvingVehicle` are present.
- `GameplayStatics.BeginDeferredActorSpawnFromClass`, `GameplayStatics.FinishSpawningActor`, and `HTWheeledVehicleDrivable.SafeSetVehicleTransform(FTransform,bool)` are present.
- `HTWheeledVehicleBase.SetTopSpeedRatio(float)`, `HTVehicleMovementComponent.SetExternalTorqueRatio(float)`, and `SetEnableWheelFriction` are present.
- `HTCheatManager.CheatSpawnVehicle(FName)` exists as a fallback. Prefer direct deferred spawning when the row's soft class can be resolved; do not require a CheatManager for the direct route.
- `HTWheeledVehicleDrivable.OwnerPlayerID` and `SetOwnerPlayerID(int64)` are present in the dump, but Tokky's readable references do not prove it calls this method, and no safe source for its player-ID argument is established here. Do not write it speculatively. Keep the validated Actor.Owner-to-player-pawn invariant unless a dump-backed and runtime-validated ID source is implemented.

## Existing Anomaly ABI decision

The Host-owned `anomaly.nte.vehicle` V2 service already exposes the runtime catalog, VehicleID selection, summon position/owner request, current driving vehicle snapshot, top-speed multiplier, engine-torque multiplier, and wheel-friction control. The `AnomalyUiServiceV1` already exposes `begin_child/end_child`. Prefer using these verified existing interfaces; add a new ABI only when a missing capability is demonstrated and its data layout / call contract is backed by the supplied dump and verified by tests. Do not add duplicate services for features already covered by V2.

For summon, resolve `InVehicleTemple` through the dump-validated reflected soft-class loader, validate every reflected function signature before invoking it, use the dump's `0x60` UE5.6 `FTransform` layout, and use `SafeSetVehicleTransform` for wheeled actors. Do not use `K2_GetActorTransform` unless that exact function is independently present and signature-validated in the target dump.

The speed control has two required effects: adjust the native top-speed limit through the dump-backed `SetTopSpeedRatio(float)` method **and** scale the current movement `Velocity` vector by new/previous ratio after reflected type/size/readability checks. A failure must report the specific failed stage and must not claim success.

## Attack Replay native call safety

The supplied dump declares `HTAbilitySystemComponent.K2_ApplyGameEffectWithClassByActor` and `K2_ApplyGameplayEffectToTargetWithClass` with a `0x78` parameter block and inline `FBufferData` at `0x10). `FBufferData.DynamicGrantedTags` is at `0x08); its `FGameplayTagContainer` contains two `TArray<FGameplayTag>` fields at relative offsets `0x00` and `0x10`. Thus the two tag arrays live at parameter offsets `0x18` and `0x28`. Since a delayed replay cannot safely reuse the original temporary TArray pointers, capture the bounded tag values while the original ProcessEvent parameter memory is live and rebuild the arrays with call-scoped storage before replay. Validate the reflected signature and every pointer/array size; refuse replay if this evidence is unavailable. Clear one-shot return fields and do not reuse a stale prediction key. Do not synthesize attack input or write target HP as a substitute for replaying the observed effect call.

## Acceptance checks for future edits

1. The catalog child remains bordered, fixed-height, independently scrollable, and includes all filtered IDs.
2. Runtime reflected calls must match this dump's function path, parameter size, field types and offsets before invocation.
3. Native/plugin ABI additions must be made in the main framework with an explicit service/version contract and corresponding manifest/capability integration; do not hardcode unverified game APIs in plugins.
4. Build Core and the affected plugin(s), validate manifest JSON, native exports and ZIP contents, then report separately what is build-verified and what has been confirmed in a live HTGame process.
