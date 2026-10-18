# Damage Chain 1.6.0 — read-only observation probe

Date: 2026-10-18
Branch: feat/damage-chain-readonly-probe-20261018
Repository: Rabb1t79/Anomaly-main

## Purpose

This is an observation instrument, not a damage-chain implementation. It keeps arbitrary damage calls disabled and records public, read-only streams so in-game evidence can be compared with the native DamageTriggerActor candidate in the 5.6.1 Dump.

## SDK surface

- anomaly.nte.combat (nte-combat-read): records new AnomalyNteDamageEventV1 items using latest_damage_sequence/next_damage_event and resolves source_id through source_name_utf8.
- anomaly.nte.actors (nte-actor-snapshot): pages the documented actor snapshot frame and resolves class/entity names. The probe logs class names containing DamageTriggerActor or DamageTrigger.
- anomaly.ui (ui): state display, a display-only interval preference, and diagnostic-log export.

No ProcessEvent caller is exposed or used. No raw address is hooked or invoked; no UObject fields are written; no actor is spawned.

## Evidence interpretation

The log can show whether DamageTriggerActor-like actors are present or appear/disappear during the same observation window as captured character damage events. Each damage entry records sequence, tick, attacker/victim generation handles, source ID/name, final damage, and flags.

This does not prove the old DLL created those actors, that an actor periodically targets the first damaged enemy, or that an actor corresponds one-to-one with a damage event. Actor snapshots are sampled and paged. The probe requests no more than one Host-capped maximum-size page per Update callback. An incomplete sweep is discarded if the snapshot generation changes; a missing actor can also mean incomplete service coverage or a class-name filter miss.

## Suggested test matrix

1. Start observing out of combat and export a baseline.
2. Hit one enemy once, then stop attacking for several seconds. Compare damage entries with actor-appeared records.
3. Hit a second enemy separately. Compare attacker/victim handles, actor timing, and subsequent damage events for each target.
4. Repeat with other player-controlled characters. Do not use the chain-start button as an experiment; it deliberately refuses activation.

Positive timing correlation is a lead, not proof of old DLL behavior. Negative results are inconclusive until runtime service coverage and snapshot timing are checked. The chain remains disabled until an SDK-supported, type-specific invocation path is proven.
