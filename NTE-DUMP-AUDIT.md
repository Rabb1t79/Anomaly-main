# NTE runtime dump audit — 2026-10-09

Build branch: build/runtime-dump-verified-20261009
Base Core commit: 9c561a4571e5d6060530ae888c8feb5dfbd6307e
Target native dump: 5.6.1-0+UE5-HT.zip

Direct CppSDK evidence used in this build:
- HTPlayerController.DT_AbilityInput: offset 0x1978.
- FHTAbilityInputRow: InputID 0x08, InputAction 0x10, Param 0x2C; total size 0x70.
- HTPlayerController.ActivateAbilityFromID and ReleaseAbilityFromID: 8-byte parameters, InputID at 0 and Param at 4.
- HTPlayerController.BP_GetCurrentDriveVehicle returns an object.
- HTCheatManager.CheatSpawnVehicle(FName VehicleID) and TestSummonVehicle(FName VehicleID) have 8-byte parameter blocks.
- APlayerController.CheatManager is at +0x430.
- UHTGameData.GetVehicleDataAsset -> UHTVehicleDataAsset.DT_VehicleData; table row struct VehicleData contains VehicleID (FName).
- Engine.Actor.K2_SetActorLocation: block size 0x130; NewLocation at 0x00, bSweep at 0x18, SweepHitResult at 0x20, bTeleport at 0x128, ReturnValue at 0x129.
- HTVehicleMovementComponent.GetForwardSpeedKmH and SetEnableWheelFriction execute on the movement component.
- Summon offset is X-2000/Y+2000/Z+2000; Actor.Owner is set to the player pawn.

Limitations:
- CI compilation and package integrity do not replace in-game testing. Confirm runtime logs for catalog rows, selected VehicleID, spawn, final coordinates, Actor.Owner, attack-input dispatch, and the correlated new DamageEvent before treating gameplay behavior as proven.
