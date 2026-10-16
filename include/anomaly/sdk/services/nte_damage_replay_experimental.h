#pragma once

#include "anomaly/sdk/base.h"

/*
 * EXPERIMENTAL Anomaly-host service contract for the NTE Attack Replay port.
 *
 * This is an Anomaly plugin ABI invented for the current integration; it is not a
 * native Unreal Engine API and has not been verified against the recovered legacy
 * DLL's binary or the supplied game dump. Keep it separate from the general NTE
 * service declarations until the request semantics and capture identity are proven.
 *
 * The sequence number refers to a Core-captured damage event. A successful return
 * means only that Core accepted/dispatched the replay request; a later independent
 * damage event is still required to confirm the gameplay effect.
 *
 * Do not add input/skill/animation replay to this contract as a fallback. Those are
 * separate paths and are not evidence of the legacy damage-replay implementation.
 */
#define ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_ID "anomaly.nte.damage-replay"
#define ANOMALY_NTE_DAMAGE_REPLAY_SERVICE_V1_VERSION 1u

typedef struct AnomalyNteDamageReplayRequestV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t damage_sequence;
} AnomalyNteDamageReplayRequestV1;

typedef struct AnomalyNteDamageReplayServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *replay_damage)(
        void* user, const AnomalyNteDamageReplayRequestV1* request);
} AnomalyNteDamageReplayServiceV1;
