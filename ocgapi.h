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

#undef OCGAPI
#undef EXTERN_C

#endif /* OCGAPI_H */
