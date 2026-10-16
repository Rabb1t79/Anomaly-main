# Attack Replay ABI quarantine

## Purpose

Keep the provisional service contract created during the Attack Replay port separate from the general NTE SDK service surface. This is an organization and audit boundary, not a claim that the service is already validated.

## Current separation

- Stable/general surface: `include/anomaly/sdk/services/nte.h` and the mirrored packaged SDK header no longer declare the Damage Replay service.
- Explicit experimental surface: `include/anomaly/sdk/services/nte_damage_replay_experimental.h`. The packaged SDK has the same header.
- Only the adapter, Attack Replay plugin and ABI snapshot tool include the experimental header directly.
- `plugins/NteAttackReplay/manifest.json` depends on `anomaly.nte.damage-replay` and the `nte-damage-replay` capability. It does not declare `anomaly.nte.attack-input`; the active replay path in this branch does not synthesize input, skill activation, or animation playback.
- Existing input-related APIs elsewhere in the repository are not deleted or silently repurposed; they remain separate and are not evidence for this damage-replay path.

## Evidence classification

### Supported by repository state

- The service identifier currently used by this branch is `anomaly.nte.damage-replay`, version 1.
- The request uses a Core-captured damage-event sequence number.
- Core accepts the request only through its own service table and capability policy.
- A successful service return is not the same as proof of a new game damage event; Attack Replay separately observes subsequent damage events.
- ABI snapshots record the layout produced by the current compiler and headers.

### Still provisional; must not be treated as recovered legacy behavior

- The service identifier and version were designed for this port; no original C++ source or original service contract has been recovered from the legacy DLL.
- The meaning of `damage_sequence` as the correct replay-capture identity still needs runtime proof.
- Dispatching the pinned GameplayEffect parameter buffer has not yet been proven to reproduce the old plugin's behavior or to create the requested branching chain.
- A matching struct layout in the ABI snapshot proves only the layout of this branch's declaration. It does not prove semantic correctness or compatibility with the old DLL.
- No key/secret recovery has been performed in this branch.

## Required verification before promoting the contract

1. Compare the contract and strings/symbols recoverable from the archived DLL against this header.
2. Trace capture, validation, pinning, dispatch and post-dispatch event matching in Core; record exactly which stage rejects a replay.
3. Verify that the captured native call is associated with the original hit's attacker, target and effect identity before dispatch.
4. Demonstrate a new independently observed damage event after dispatch, then test whether each generated event can seed the next generation.
5. Only after these checks, decide whether the interface belongs in the stable NTE SDK ABI or remains experimental.

## Build boundary

The stable ABI snapshot excludes the provisional Damage Replay request, service table and service entry. Their compiler-measured layout lives separately at `abi/experimental/nte-damage-replay-v1-windows-x64.json` with a mirrored copy under the packaged SDK. The ABI snapshot tool exposes `--write-experimental` and `--check-experimental` modes to measure and validate that separate file. The general `nte.h` header does not import the experimental declaration transitively. A future change that alters this provisional contract should update both the source and packaged experimental headers and snapshots, then pass the Attack plugin and Runtime/ABI workflows before being considered usable.
