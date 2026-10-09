# NTE Plugin UI Lifecycle Audit — 2026-10-09

## Framework contract checked

Source of truth:
- `docs/api-reference/ui-services.md`: the `anomaly.ui` facade is callback-scoped; except `developer_mode_enabled`, calls are valid only during the current `on_draw` callback.
- `docs/developer-guide/plugin-development.md`: `on_update` runs in Game domain; `on_draw` runs in Render domain and must draw an immutable snapshot rather than invoke game-semantic services.
- `include/anomaly/sdk/cpp.hpp`: `anomaly::sdk::UiWindow` wraps `begin_window`/`end_window`; the destructor balances `end_window` even when `begin_window` returns false for a collapsed window.
- `src/plugin/plugin_manager.cpp`: the host recovers unbalanced UI stacks after a plugin Draw callback and faults a plugin if the callback leaves the stack invalid. This is host recovery, not a substitute for correctly paired calls in a plugin.

Branch/source reference: `build/runtime-nte-ui-lifecycle-20261009` at commit `069563b3e5d228b1f2396c56f67d320b4e5b47e5`.

## Vehicle 0.8.0

- `Draw` uses the UI facade supplied for that callback; it does not cache the UI facade for future callbacks.
- The published view is copied while holding `ui_mutex`; the lock is released before UI calls.
- `anomaly::sdk::UiWindow` provides RAII pairing, including the collapsed-window early return.
- UI handlers only queue selection/page/summon commands. Catalog reads, player snapshots, selection, and summon service calls run in Game `Update`.
- The window-close state is persistent during a plugin generation; Start reopens it. Stop clears pending commands, and Unload clears published UI state and service pointers.

## Attack Replay 1.4.0

- `Draw` uses only the callback-scoped UI facade. It takes a short snapshot lock, releases the lock, then draws.
- `UiWindow` balances begin/end on all returns from the window body, including collapsed windows.
- UI controls enqueue intent only. Attack `press`, `release`, combat-event polling, and cursor advancement are called from Game `Update`, not from Draw or Stop.
- Start restores the window. Stop clears stale UI commands; it does not invoke the Game-thread-only input service. If Stop lands in the narrow interval after a press but before its next-tick release, the latch is retained and the next Game Update after re-enable releases it. This prevents an illegal cross-domain release, but cannot release input immediately while no Game Update callback is running.
- Unload clears service pointers and published UI state after host callback barriers have drained.

## Build/package checks

- Both source files passed `clang++ -std=c++20 -fsyntax-only` against the NTE v2 / attack-input v1 SDK headers.
- Both DLLs are PE32+ x86-64 and export `AnomalyPluginEntryV1`.
- Imported DLLs are only `msvcrt.dll` and `KERNEL32.dll`; no `libc++.dll` or `libunwind.dll` import.
- This local DLL build used host Clang 17 with the supplied LLVM-MinGW libc++ 23 headers/libraries and compatibility shims because the supplied Windows compiler executable cannot run in this Linux build environment. It is not an official Visual Studio plugin build, and neither plugin has been tested in-game here.

## Core/runtime status

The older successful archive `Anomaly-runtime-MSVC-20261009.zip` is **not compatible** with these plugin manifests: its Core was built from an older source revision, its vehicle service is v1, and its NTE profile lacks the `nte.attack-input` feature. Do not install this pair of plugins into that old Runtime. The current source branch has vehicle service v2, attack-input v1 (`press`/`release`), the updated NTE profile, and corrected dump-backed vehicle summon binding. A draft PR was opened for Core review/build: https://github.com/Rabb1t79/Anomaly-main/pull/14 . The GitHub Actions run/check result did not become available in this session, so no current Core DLL is represented as rebuilt or in-game validated. Consequently this artifact is the corrected plugin package/overlay, not a falsely labelled full Runtime.
