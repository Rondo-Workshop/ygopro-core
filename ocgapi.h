/*
 * Copyright (c) 2019, Dylam De La Torre, (DyXel)
 * Copyright (c) 2019-2025, Edoardo Lolletti (edo9300) <edoardo762@gmail.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef OCGAPI_H
#define OCGAPI_H
#include "ocgapi_types.h"

#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C
#endif

#if defined(OCGCORE_EXPORT_FUNCTIONS)
#if defined(_WIN32)
#define OCGAPI EXTERN_C __declspec(dllexport)
#else
#define OCGAPI EXTERN_C __attribute__ ((visibility ("default")))
#endif
#else
#define OCGAPI EXTERN_C
#endif

/*** CORE INFORMATION ***/
OCGAPI void OCG_GetVersion(int* major, int* minor);

/*** DUEL CREATION AND DESTRUCTION ***/
OCGAPI int OCG_CreateDuel(OCG_Duel* out_ocg_duel, const OCG_DuelOptions* options_ptr);
OCGAPI void OCG_DestroyDuel(OCG_Duel ocg_duel);
OCGAPI void OCG_DuelNewCard(OCG_Duel ocg_duel, const OCG_NewCardInfo* info_ptr);
OCGAPI void OCG_StartDuel(OCG_Duel ocg_duel);

/*** DUEL PROCESSING AND QUERYING ***/
OCGAPI int OCG_DuelProcess(OCG_Duel ocg_duel);
OCGAPI void* OCG_DuelGetMessage(OCG_Duel ocg_duel, uint32_t* length);
OCGAPI void OCG_DuelSetResponse(OCG_Duel ocg_duel, const void* buffer, uint32_t length);
OCGAPI int OCG_LoadScript(OCG_Duel ocg_duel, const char* buffer, uint32_t length, const char* name);

OCGAPI uint32_t OCG_DuelQueryCount(OCG_Duel ocg_duel, uint8_t team, uint32_t loc);
OCGAPI void* OCG_DuelQuery(OCG_Duel ocg_duel, uint32_t* length, const OCG_QueryInfo* info_ptr);
OCGAPI void* OCG_DuelQueryLocation(OCG_Duel ocg_duel, uint32_t* length, const OCG_QueryInfo* info_ptr);
OCGAPI void* OCG_DuelQueryField(OCG_Duel ocg_duel, uint32_t* length);

/* Private host reconciliation ABI. IDs are uint32, unique only within a duel,
 * and align with OCG_DuelQueryLocation's slot order (zero for empty slots).
 * No card/object pointer is returned. This does not change the core API version.
 */
#define OCG_RONDO_CARD_IDENTITY_ABI_VERSION 1u
typedef enum OCG_RondoCardIdentityStatus {
	OCG_RONDO_CARD_IDENTITY_OK = 0,
	OCG_RONDO_CARD_IDENTITY_INVALID_ARGUMENT = 1,
	OCG_RONDO_CARD_IDENTITY_INVALID_DUEL = 2,
	OCG_RONDO_CARD_IDENTITY_INSUFFICIENT_CAPACITY = 3
} OCG_RondoCardIdentityStatus;
OCGAPI uint32_t OCG_RondoCardIdentityABIVersion(void);
OCGAPI int OCG_RondoDuelQueryCardIDs(OCG_Duel ocg_duel, uint32_t abi_version,
	uint8_t controller, uint32_t location, uint32_t* out_ids,
	uint32_t capacity, uint32_t* out_count);

/* Independent private latent-usage ABI v1. Keys are the core's semantic
 * code/flag/index/player integers, never addresses or duel-local card IDs.
 * scope: 0 = turn, 1 = duel. Unsupported effect-local, SINGLE, chain, and
 * special-summon counters are signaled instead of being silently omitted.
 */
#define OCG_RONDO_LATENT_USAGE_ABI_VERSION 1u
typedef struct OCG_RondoUsageCount {
	uint64_t key;
	uint32_t count;
	uint32_t scope;
} OCG_RondoUsageCount;
typedef enum OCG_RondoUsageStatus {
	OCG_RONDO_USAGE_OK = 0,
	OCG_RONDO_USAGE_INVALID_ARGUMENT = 1,
	OCG_RONDO_USAGE_INVALID_DUEL = 2,
	OCG_RONDO_USAGE_INSUFFICIENT_CAPACITY = 3
} OCG_RondoUsageStatus;
OCGAPI uint32_t OCG_RondoLatentUsageABIVersion(void);
OCGAPI int OCG_RondoDuelQueryUsage(OCG_Duel ocg_duel, uint32_t abi_version,
	OCG_RondoUsageCount* out_counts, uint32_t capacity,
	uint32_t* out_count, uint32_t* out_unsupported);

/* Independent private latent-state ABI v2. ABI v1 above is unchanged.
 * The baseline marker is the duel-local monotonic field_id counter that also
 * assigns effect::initial_id; it is never an address and is only meaningful
 * for the same duel. Count records use the exact v1 key/count/scope shape.
 * Every OCG_RONDO_LATENT_* bit reports native state that GameState cannot
 * represent and must be treated as fail-closed by the caller.
 */
#define OCG_RONDO_LATENT_STATE_ABI_VERSION 2u
#define OCG_RONDO_LATENT_EFFECT_LOCAL_COUNT      0x0001u
#define OCG_RONDO_LATENT_SINGLE_OR_CHAIN_COUNT   0x0002u
#define OCG_RONDO_LATENT_SPSUMMON_ONCE           0x0004u
#define OCG_RONDO_LATENT_NORMAL_SUMMON_COUNT     0x0008u
#define OCG_RONDO_LATENT_PLAYER_EFFECT           0x0010u
#define OCG_RONDO_LATENT_CARD_EFFECT             0x0020u
#define OCG_RONDO_LATENT_FLAG_EFFECT             0x0040u
#define OCG_RONDO_LATENT_ACTIVITY_COUNTER        0x0080u
#define OCG_RONDO_LATENT_CUSTOM_ACTIVITY_COUNTER 0x0100u
#define OCG_RONDO_LATENT_PHASE_ACTIVITY          0x0200u
#define OCG_RONDO_LATENT_CARD_MOVE_PROVENANCE    0x0400u
OCGAPI uint32_t OCG_RondoLatentStateABIVersion(void);
OCGAPI int OCG_RondoDuelLatentBaseline(OCG_Duel ocg_duel, uint32_t abi_version,
	uint32_t* out_marker);
OCGAPI int OCG_RondoDuelQueryLatentState(OCG_Duel ocg_duel, uint32_t abi_version,
	uint32_t baseline_marker, OCG_RondoUsageCount* out_counts, uint32_t capacity,
	uint32_t* out_count, uint32_t* out_unsupported);

/* Independent private latent-state ABI v3. ABI v1 and v2 above are unchanged.
 * v3 exports exact phase activity and per-card move history instead of the v2
 * PHASE_ACTIVITY / CARD_MOVE_PROVENANCE fail-closed bits. card_id,
 * effect initial_id and owner/reason card IDs are duel-local reconciliation
 * evidence only; they are never addresses and never semantic identity.
 */
#define OCG_RONDO_LATENT_STATE_V3_ABI_VERSION 3u
#define OCG_RONDO_LATENT_BASELINE_EFFECT_MUTATION 0x0800u
/* auxiliary_state bits: non-empty per-card containers GameState omits. */
#define OCG_RONDO_CARD_AUX_COUNTERS      0x0001u
#define OCG_RONDO_CARD_AUX_RELATIONS     0x0002u
#define OCG_RONDO_CARD_AUX_ASSUME        0x0004u
#define OCG_RONDO_CARD_AUX_INDESTRUCTABLE 0x0008u
#define OCG_RONDO_CARD_AUX_BATTLE_MAPS   0x0010u
#define OCG_RONDO_CARD_AUX_EQUIP_TARGETS 0x0020u
#define OCG_RONDO_CARD_AUX_MATERIALS     0x0040u
#define OCG_RONDO_CARD_AUX_PZONE         0x0080u
#define OCG_RONDO_CARD_AUX_SPSUMMON_COUNTER 0x0100u
#define OCG_RONDO_CARD_AUX_ATTACK_COUNTS 0x0200u
typedef struct OCG_RondoEffectOwnerV3 {
	uint32_t initial_id;
	uint32_t owner_card_id;
} OCG_RondoEffectOwnerV3;
typedef struct OCG_RondoCardHistoryV3 {
	uint32_t card_id;
	uint32_t moved;
	uint32_t turn_id;
	uint32_t status;
	uint32_t previous_controller;
	uint32_t previous_location;
	uint32_t previous_sequence;
	uint32_t previous_position;
	uint32_t previous_code;
	uint32_t previous_code2;
	uint32_t previous_type;
	uint32_t previous_level;
	uint32_t previous_rank;
	uint32_t previous_attribute;
	uint64_t previous_race;
	int32_t previous_attack;
	int32_t previous_defense;
	uint32_t reason;
	uint32_t reason_player;
	uint32_t reason_effect_state; /* 0 none, 1 live effect, 2 not a live effect */
	uint32_t reason_effect_initial_id;
	uint32_t reason_effect_owner_card_id; /* 0 unless owner is a live duel card */
	uint32_t reason_effect_handler_is_owner;
	uint32_t reason_card_state; /* 0 none, 1 live card, 2 not a live card */
	uint32_t reason_card_id;
	uint32_t summon_type;
	uint32_t summon_location;
	uint32_t summon_player;
	uint32_t summon_sequence;
	uint32_t turn_counter;
	uint32_t auxiliary_state;
	uint32_t unique_code;
} OCG_RondoCardHistoryV3;
typedef struct OCG_RondoLatentSummaryV3 {
	uint32_t unsupported; /* v2 bits except PHASE_ACTIVITY/CARD_MOVE_PROVENANCE; v3 baseline mutation */
	uint32_t phase_action;
} OCG_RondoLatentSummaryV3;
OCGAPI uint32_t OCG_RondoLatentStateV3ABIVersion(void);
OCGAPI int OCG_RondoDuelLatentBaselineV3(OCG_Duel ocg_duel, uint32_t abi_version,
	uint32_t* out_marker);
OCGAPI int OCG_RondoDuelQueryEffectOwnersV3(OCG_Duel ocg_duel, uint32_t abi_version,
	uint32_t baseline_marker, OCG_RondoEffectOwnerV3* out_owners, uint32_t capacity,
	uint32_t* out_count);
OCGAPI int OCG_RondoDuelQueryLatentStateV3(OCG_Duel ocg_duel, uint32_t abi_version,
	uint32_t baseline_marker, OCG_RondoUsageCount* out_counts, uint32_t count_capacity,
	uint32_t* out_count, OCG_RondoCardHistoryV3* out_cards, uint32_t card_capacity,
	uint32_t* out_card_count, OCG_RondoLatentSummaryV3* out_summary);

#undef OCGAPI
#undef EXTERN_C

#endif /* OCGAPI_H */
