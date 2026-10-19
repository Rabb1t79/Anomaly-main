# Pink Paw Heist ESP — RobBank update checkpoint

Date: 2026-10-19

## Input / reported symptoms

The supplied handoff describes a 2026-10-18 game update that changed the Pink Paw RobBank layout. The reported live symptoms were 862 loot entries all showing `pickable: false`, and item values collapsing to `1262753616`. The handoff reports that a working local build found per-item Fons values such as `50000`, `666666` and `131400`; those game observations have not been independently reproduced from this branch.

## Source changes in this branch

- Changed `AHTPlayerState::ClientRobBankKeyDoorDataArray` from `0x9A90` to the handoff's updated `0x9A50` offset. This feeds only the RobBank key-door pickability context; generic teleport code is unchanged.
- Made the item data pointer path explicit: the row's `FInstancedStruct` begins at `0x1B8`, and its embedded data pointer is read at `0x1C0`. The previous code already computed the same effective address (`0x1B8 + 8 = 0x1C0`), so this clarifies and compile-time-checks the path; it is not evidence by itself that the reported abnormal value is fixed.
- Marked settings dirty when the Auto Teleport and Pickup checkbox or its delay changes. `SaveSettings` persists dirty settings during plugin stop, matching the existing configuration lifecycle. Manual teleport entry points were not changed.
- Bumped the plugin manifest patch version from `1.14.0` to `1.14.1`.

## Validation status

- Source-level checks confirm the old key-door offset was replaced, the effective item data pointer offset is compile-time asserted as `0x1C0`, and both auto-teleport-pickup controls mark the configuration dirty.
- A Windows x64 build is triggered by the source changes. Record the actual Actions result here when it completes.
- In-game verification is still required for: key-door gating no longer rejecting all 862 items, distinct values such as the reported sample values, and persistence after disabling/restarting the plugin. The reported local hot-update is useful evidence but is not a test of the newly built package.
- Manual teleport was intentionally left unchanged.
