/*
 * Copyright (c) 2019, Dylam De La Torre, (DyXel)
 * Copyright (c) 2019-2025, Edoardo Lolletti (edo9300) <edoardo762@gmail.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <algorithm>
#include <array>
#include <cstring> //std::memcpy
#include <new> //std::nothrow
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ocgapi.h"
#include "interpreter.h"
#include "duel.h"
#include "field.h"
#include "effect.h"
#include "group.h"

namespace {
std::mutex rondo_duel_mutex;
std::unordered_set<duel*> rondo_active_duels;
// Private v3-only state. Registry pins prevent a replaced Lua function from
// reusing its old registry slot or object address before the next comparison.
struct rondo_effect_baseline {
	std::array<uint64_t, 38> fields;
	std::vector<lua_Integer> label;
	std::array<int, 5> functions{};
	int effect_pin{};
	int label_object_pin{};
	LuaParam label_object_kind{ LuaParam::DELETED };
	lua_obj* label_object_target{};
	card_set label_group_cards;
};
struct rondo_duel_baseline {
	uint32_t marker;
	std::unordered_map<effect*, rondo_effect_baseline> effects;
};
std::unordered_map<duel*, rondo_duel_baseline> rondo_v3_baselines;

bool rondo_effect_registered(const duel* pduel, const effect* peffect) {
	return pduel->game_field->effects.indexer.count(const_cast<effect*>(peffect)) != 0 ||
		(peffect->handler && pduel->cards.count(peffect->handler) &&
		 peffect->handler->indexer.count(const_cast<effect*>(peffect)) != 0);
}

std::array<uint64_t, 38> rondo_effect_fields(const duel* pduel, const effect* e) {
	return { e->count_limit, e->count_limit_max, e->count_flag, e->count_hopt_index,
		// The resolver stamps triggering_player into effect_owner on activation;
		// compare the effective player, including PLAYER_NONE's owner fallback.
		(e->effect_owner == PLAYER_NONE && e->owner && pduel->cards.count(e->owner))
			? e->owner->current.controler : e->effect_owner,
		e->type, e->copy_id, e->range, e->s_range, e->o_range,
		// Activation source coordinates are stamped by the normal resolver;
		// native move provenance already carries the physical source location.
		e->reset_count, 0u, 0u,
		// AVAILABLE is a cached result of is_available(), not persistent state.
		static_cast<uint64_t>(e->status & ~EFFECT_STATUS_AVAILABLE),
		// id is re-stamped on normal availability/index refresh; initial_id is
		// the durable identity and registration is compared separately.
		e->code, e->flag[0], e->flag[1], 0u, e->initial_id,
		e->reset_flag, e->count_code, e->hint_timing[0], e->hint_timing[1],
		e->card_type, e->active_type, static_cast<uint32_t>(e->label_object),
		static_cast<uint32_t>(e->condition), static_cast<uint32_t>(e->cost),
		static_cast<uint32_t>(e->target), static_cast<uint32_t>(e->value),
		static_cast<uint32_t>(e->operation), e->category, e->description,
		reinterpret_cast<uintptr_t>(e->owner), reinterpret_cast<uintptr_t>(e->handler),
		reinterpret_cast<uintptr_t>(e->active_handler), rondo_effect_registered(pduel, e),
		e->rondo_owner_player_script_revision };
}

std::array<int, 5> rondo_function_handles(const effect* e) {
	return { e->condition, e->cost, e->target,
		(e->flag[0] & EFFECT_FLAG_FUNC_VALUE) ? e->value : 0, e->operation };
}

void rondo_release_baseline(duel* pduel) {
	auto found = rondo_v3_baselines.find(pduel);
	if(found == rondo_v3_baselines.end())
		return;
	auto* L = pduel->lua->lua_state;
	for(const auto& entry : found->second.effects)
	{
		for(auto ref : entry.second.functions)
			if(ref) luaL_unref(L, LUA_REGISTRYINDEX, ref);
		if(entry.second.effect_pin)
			luaL_unref(L, LUA_REGISTRYINDEX, entry.second.effect_pin);
		if(entry.second.label_object_pin)
			luaL_unref(L, LUA_REGISTRYINDEX, entry.second.label_object_pin);
	}
	rondo_v3_baselines.erase(found);
}

bool rondo_baseline_effect_changed(duel* pduel, const rondo_duel_baseline& baseline) {
	auto* L = pduel->lua->lua_state;
	for(const auto& entry : baseline.effects) {
		auto* e = entry.first;
		if(pduel->effects.count(e) == 0)
			continue; // A destroyed effect no longer contributes future authority.
		lua_rawgeti(L, LUA_REGISTRYINDEX, e->ref_handle);
		lua_rawgeti(L, LUA_REGISTRYINDEX, entry.second.effect_pin);
		const bool same_effect = lua_rawequal(L, -1, -2) != 0;
		lua_pop(L, 2);
		if(!same_effect)
			return true; // A newly allocated effect reused a baseline address.
		if(rondo_effect_fields(pduel, e) != entry.second.fields || e->label != entry.second.label)
			return true;
		if(e->label_object != 0) {
			if(entry.second.label_object_pin == 0)
				return true;
			lua_rawgeti(L, LUA_REGISTRYINDEX, e->label_object);
			lua_rawgeti(L, LUA_REGISTRYINDEX, entry.second.label_object_pin);
			const bool same = lua_rawequal(L, -1, -2) != 0;
			lua_pop(L, 1);
			if(!same) { lua_pop(L, 1); return true; }
			// Tables can change in place without invoking an Effect setter.
			if(lua_istable(L, -1)) { lua_pop(L, 1); return true; }
			if(!lua_isuserdata(L, -1)) { lua_pop(L, 1); return true; }
			auto* obj = *static_cast<lua_obj**>(lua_touserdata(L, -1));
			lua_pop(L, 1);
			if(obj != entry.second.label_object_target || !obj ||
				obj->lua_type != entry.second.label_object_kind)
				return true;
			if(obj->lua_type == LuaParam::GROUP) {
				auto* g = static_cast<group*>(obj);
				if(pduel->groups.count(g) == 0 || g->container != entry.second.label_group_cards)
					return true;
			} else if(obj->lua_type == LuaParam::CARD) {
				if(pduel->cards.count(static_cast<card*>(obj)) == 0)
					return true;
			} else if(obj->lua_type == LuaParam::EFFECT) {
				if(baseline.effects.count(static_cast<effect*>(obj)) == 0)
					return true;
			} else return true;
		} else if(entry.second.label_object_pin != 0) {
			return true;
		}
		const auto current = rondo_function_handles(e);
		for(size_t i = 0; i < current.size(); ++i) {
			if(current[i] == 0 && entry.second.functions[i] == 0)
				continue;
			if(current[i] == 0 || entry.second.functions[i] == 0)
				return true;
			lua_rawgeti(L, LUA_REGISTRYINDEX, current[i]);
			lua_rawgeti(L, LUA_REGISTRYINDEX, entry.second.functions[i]);
			const bool same = lua_rawequal(L, -1, -2) != 0;
			lua_pop(L, 2);
			if(!same) return true;
		}
	}
	return false;
}
}

void OCG_GetVersion(int* major, int* minor) {
	if(major)
		*major = OCG_VERSION_MAJOR;
	if(minor)
		*minor = OCG_VERSION_MINOR;
}

int OCG_CreateDuel(OCG_Duel* out_ocg_duel, const OCG_DuelOptions* options_ptr) {
	if(out_ocg_duel == nullptr)
		return OCG_DUEL_CREATION_NO_OUTPUT;
	auto options = *options_ptr;
	if(options.cardReader == nullptr) {
		*out_ocg_duel = nullptr;
		return OCG_DUEL_CREATION_NULL_DATA_READER;
	}
	if(options.scriptReader == nullptr) {
		*out_ocg_duel = nullptr;
		return OCG_DUEL_CREATION_NULL_SCRIPT_READER;
	}
	if(options.logHandler == nullptr) {
		options.logHandler = [](void* /*payload*/, const char* /*string*/, int /*type*/) {};
		options.payload3 = nullptr;
	}
	if(options.cardReaderDone == nullptr) {
		options.cardReaderDone = [](void* /*payload*/, OCG_CardData* /*data*/) {};
		options.payload4 = nullptr;
	}
	if(options.seed[0] == 0 && options.seed[1] == 0 && options.seed[2] == 0 && options.seed[3] == 0)
		return OCG_DUEL_CREATION_NULL_RNG_SEED;
	bool valid_lua_lib = true;
	auto* duelPtr = new (std::nothrow) duel(options, valid_lua_lib);
	if(duelPtr == nullptr)
		return OCG_DUEL_CREATION_NOT_CREATED;
	if(!valid_lua_lib) {
		delete duelPtr;
		return OCG_DUEL_CREATION_INCOMPATIBLE_LUA_API;
	}
	*out_ocg_duel = static_cast<OCG_Duel>(duelPtr);
	{
		std::lock_guard<std::mutex> lock(rondo_duel_mutex);
		rondo_active_duels.insert(duelPtr);
	}
	return OCG_DUEL_CREATION_SUCCESS;
}

void OCG_DestroyDuel(OCG_Duel ocg_duel) {
	if(ocg_duel) {
		{
			std::lock_guard<std::mutex> lock(rondo_duel_mutex);
			if(rondo_active_duels.erase(static_cast<duel*>(ocg_duel)) == 0)
				return;
			rondo_release_baseline(static_cast<duel*>(ocg_duel));
		}
		delete static_cast<duel*>(ocg_duel);
	}
}

uint32_t OCG_RondoCardIdentityABIVersion(void) {
	return OCG_RONDO_CARD_IDENTITY_ABI_VERSION;
}

int OCG_RondoDuelQueryCardIDs(OCG_Duel ocg_duel, uint32_t abi_version,
	uint8_t controller, uint32_t location, uint32_t* out_ids,
	uint32_t capacity, uint32_t* out_count) {
	if(abi_version != OCG_RONDO_CARD_IDENTITY_ABI_VERSION || controller > 1 ||
		out_ids == nullptr || out_count == nullptr ||
		(location != LOCATION_DECK && location != LOCATION_HAND &&
		 location != LOCATION_MZONE && location != LOCATION_SZONE &&
		 location != LOCATION_GRAVE && location != LOCATION_REMOVED &&
		 location != LOCATION_EXTRA))
		return OCG_RONDO_CARD_IDENTITY_INVALID_ARGUMENT;
	*out_count = 0;
	std::lock_guard<std::mutex> lock(rondo_duel_mutex);
	auto* pduel = static_cast<duel*>(ocg_duel);
	if(rondo_active_duels.find(pduel) == rondo_active_duels.end())
		return OCG_RONDO_CARD_IDENTITY_INVALID_DUEL;
	const auto& player = pduel->game_field->player[controller];
	const card_vector* list = nullptr;
	switch(location) {
	case LOCATION_DECK: list = &player.list_main; break;
	case LOCATION_HAND: list = &player.list_hand; break;
	case LOCATION_MZONE: list = &player.list_mzone; break;
	case LOCATION_SZONE: list = &player.list_szone; break;
	case LOCATION_GRAVE: list = &player.list_grave; break;
	case LOCATION_REMOVED: list = &player.list_remove; break;
	case LOCATION_EXTRA: list = &player.list_extra; break;
	}
	if(capacity < list->size())
		return OCG_RONDO_CARD_IDENTITY_INSUFFICIENT_CAPACITY;
	for(size_t i = 0; i < list->size(); ++i)
		out_ids[i] = (*list)[i] ? (*list)[i]->cardid : 0;
	*out_count = static_cast<uint32_t>(list->size());
	return OCG_RONDO_CARD_IDENTITY_OK;
}

uint32_t OCG_RondoLatentUsageABIVersion(void) {
	return OCG_RONDO_LATENT_USAGE_ABI_VERSION;
}

int OCG_RondoDuelQueryUsage(OCG_Duel ocg_duel, uint32_t abi_version,
	OCG_RondoUsageCount* out_counts, uint32_t capacity,
	uint32_t* out_count, uint32_t* out_unsupported) {
	if(abi_version != OCG_RONDO_LATENT_USAGE_ABI_VERSION || !out_counts ||
		!out_count || !out_unsupported)
		return OCG_RONDO_USAGE_INVALID_ARGUMENT;
	*out_count = 0;
	*out_unsupported = 0;
	std::lock_guard<std::mutex> lock(rondo_duel_mutex);
	auto* pduel = static_cast<duel*>(ocg_duel);
	if(rondo_active_duels.find(pduel) == rondo_active_duels.end())
		return OCG_RONDO_USAGE_INVALID_DUEL;
	const auto& core = pduel->game_field->core;
	std::vector<OCG_RondoUsageCount> counts;
	auto collect = [&](const processor::effect_count_map& values, uint32_t scope) {
		for(const auto& entry : values) {
			if(entry.second == 0)
				continue;
			// The pinned field.cpp passes index/flag in reversed argument order
			// to generate_count_map_key; count_flag is stored at bits 16..23.
			const auto flag = static_cast<uint8_t>(entry.first >> 16);
			if(scope == 2 || (flag & (EFFECT_COUNT_CODE_SINGLE | EFFECT_COUNT_CODE_CHAIN))) {
				*out_unsupported |= 2u;
				continue;
			}
			counts.push_back({entry.first, entry.second, scope});
		}
	};
	collect(core.effect_count_code, 0);
	collect(core.effect_count_code_duel, 1);
	collect(core.effect_count_code_chain, 2);
	for(const auto* peffect : pduel->effects) {
		if(peffect->has_count_limit() && peffect->count_limit != peffect->count_limit_max)
			*out_unsupported |= 1u;
	}
	for(uint8_t player = 0; player < 2; ++player) {
		if(!core.spsummon_once_map[player].empty() ||
			!core.spsummon_once_map_rst[player].empty())
			*out_unsupported |= 4u;
		if(core.summon_count[player] != 0)
			*out_unsupported |= 8u;
	}
	std::sort(counts.begin(), counts.end(), [](const auto& left, const auto& right) {
		return left.scope < right.scope ||
			(left.scope == right.scope && left.key < right.key);
	});
	if(counts.size() > capacity)
		return OCG_RONDO_USAGE_INSUFFICIENT_CAPACITY;
	for(size_t i = 0; i < counts.size(); ++i)
		out_counts[i] = counts[i];
	*out_count = static_cast<uint32_t>(counts.size());
	return OCG_RONDO_USAGE_OK;
}

uint32_t OCG_RondoLatentStateABIVersion(void) {
	return OCG_RONDO_LATENT_STATE_ABI_VERSION;
}

int OCG_RondoDuelLatentBaseline(OCG_Duel ocg_duel, uint32_t abi_version,
	uint32_t* out_marker) {
	if(abi_version != OCG_RONDO_LATENT_STATE_ABI_VERSION || !out_marker)
		return OCG_RONDO_USAGE_INVALID_ARGUMENT;
	*out_marker = 0;
	std::lock_guard<std::mutex> lock(rondo_duel_mutex);
	auto* pduel = static_cast<duel*>(ocg_duel);
	if(rondo_active_duels.find(pduel) == rondo_active_duels.end())
		return OCG_RONDO_USAGE_INVALID_DUEL;
	*out_marker = pduel->game_field->infos.field_id;
	return OCG_RONDO_USAGE_OK;
}

int OCG_RondoDuelQueryLatentState(OCG_Duel ocg_duel, uint32_t abi_version,
	uint32_t baseline_marker, OCG_RondoUsageCount* out_counts, uint32_t capacity,
	uint32_t* out_count, uint32_t* out_unsupported) {
	if(abi_version != OCG_RONDO_LATENT_STATE_ABI_VERSION || baseline_marker == 0 ||
		!out_counts || !out_count || !out_unsupported)
		return OCG_RONDO_USAGE_INVALID_ARGUMENT;
	*out_count = 0;
	*out_unsupported = 0;
	// Exact v1 count-map authority (validates the duel under its own lock).
	uint32_t unsupported = 0;
	const auto status = OCG_RondoDuelQueryUsage(ocg_duel, OCG_RONDO_LATENT_USAGE_ABI_VERSION,
		out_counts, capacity, out_count, &unsupported);
	if(status != OCG_RONDO_USAGE_OK) {
		*out_count = 0;
		return status;
	}
	std::lock_guard<std::mutex> lock(rondo_duel_mutex);
	auto* pduel = static_cast<duel*>(ocg_duel);
	if(rondo_active_duels.find(pduel) == rondo_active_duels.end()) {
		*out_count = 0;
		return OCG_RONDO_USAGE_INVALID_DUEL;
	}
	const auto& game_field = *pduel->game_field;
	const auto& core = game_field.core;
	if(baseline_marker > game_field.infos.field_id) {
		*out_count = 0;
		return OCG_RONDO_USAGE_INVALID_ARGUMENT;
	}
	// Effects registered at or after the baseline that are still indexed by
	// the field or by their handler card survive into future decisions.
	for(auto* peffect : pduel->effects) {
		if(peffect->initial_id < baseline_marker)
			continue;
		const bool field_registered = game_field.effects.indexer.count(peffect) != 0;
		const bool card_registered = peffect->handler != nullptr &&
			peffect->handler->indexer.count(peffect) != 0;
		if(!field_registered && !card_registered)
			continue;
		if((peffect->code & 0xf0000000u) == 0x10000000u)
			unsupported |= OCG_RONDO_LATENT_FLAG_EFFECT;
		else if(peffect->flag[0] & EFFECT_FLAG_FIELD_ONLY)
			unsupported |= OCG_RONDO_LATENT_PLAYER_EFFECT;
		else
			unsupported |= OCG_RONDO_LATENT_CARD_EFFECT;
	}
	for(uint8_t player = 0; player < 2; ++player) {
		if(core.summon_state_count[player] || core.normalsummon_state_count[player] ||
			core.flipsummon_state_count[player] || core.spsummon_state_count[player] ||
			core.spsummon_state_count_rst[player] || core.attack_state_count[player] ||
			core.battle_phase_count[player] || core.battled_count[player])
			unsupported |= OCG_RONDO_LATENT_ACTIVITY_COUNTER;
	}
	for(const auto* counters : { &core.summon_counter, &core.normalsummon_counter,
		&core.spsummon_counter, &core.flipsummon_counter, &core.attack_counter,
		&core.chain_counter }) {
		for(const auto& entry : *counters) {
			if(entry.second.player_amount[0] || entry.second.player_amount[1])
				unsupported |= OCG_RONDO_LATENT_CUSTOM_ACTIVITY_COUNTER;
		}
	}
	if(core.phase_action)
		unsupported |= OCG_RONDO_LATENT_PHASE_ACTIVITY;
	// Relocation re-stamps fieldid from the same monotonic counter together
	// with turnid and previous location/reason (e.g. aux.exccon authority).
	for(const auto* pcard : pduel->cards) {
		if(pcard->fieldid >= baseline_marker)
			unsupported |= OCG_RONDO_LATENT_CARD_MOVE_PROVENANCE;
	}
	*out_unsupported = unsupported;
	return OCG_RONDO_USAGE_OK;
}

uint32_t OCG_RondoLatentStateV3ABIVersion(void) {
	return OCG_RONDO_LATENT_STATE_V3_ABI_VERSION;
}

int OCG_RondoDuelLatentBaselineV3(OCG_Duel ocg_duel, uint32_t abi_version,
	uint32_t* out_marker) {
	if(abi_version != OCG_RONDO_LATENT_STATE_V3_ABI_VERSION || !out_marker)
		return OCG_RONDO_USAGE_INVALID_ARGUMENT;
	*out_marker = 0;
	std::lock_guard<std::mutex> lock(rondo_duel_mutex);
	auto* pduel = static_cast<duel*>(ocg_duel);
	if(rondo_active_duels.count(pduel) == 0)
		return OCG_RONDO_USAGE_INVALID_DUEL;
	const auto existing = rondo_v3_baselines.find(pduel);
	if(existing != rondo_v3_baselines.end()) {
		*out_marker = existing->second.marker;
		return OCG_RONDO_USAGE_OK;
	}
	rondo_duel_baseline baseline{};
	baseline.marker = pduel->game_field->infos.field_id;
	auto* L = pduel->lua->lua_state;
	for(auto* e : pduel->effects) {
		rondo_effect_baseline saved{ rondo_effect_fields(pduel, e), e->label };
		lua_rawgeti(L, LUA_REGISTRYINDEX, e->ref_handle);
		saved.effect_pin = luaL_ref(L, LUA_REGISTRYINDEX);
		const auto handles = rondo_function_handles(e);
		for(size_t i = 0; i < handles.size(); ++i) {
			if(handles[i] == 0) continue;
			lua_rawgeti(L, LUA_REGISTRYINDEX, handles[i]);
			saved.functions[i] = luaL_ref(L, LUA_REGISTRYINDEX);
		}
		if(e->label_object) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, e->label_object);
			if(lua_isuserdata(L, -1)) {
				saved.label_object_target = *static_cast<lua_obj**>(lua_touserdata(L, -1));
				if(saved.label_object_target) {
					saved.label_object_kind = saved.label_object_target->lua_type;
					if(saved.label_object_kind == LuaParam::GROUP) {
						auto* g = static_cast<group*>(saved.label_object_target);
						if(pduel->groups.count(g)) saved.label_group_cards = g->container;
					}
				}
			}
			saved.label_object_pin = luaL_ref(L, LUA_REGISTRYINDEX);
		}
		baseline.effects.emplace(e, std::move(saved));
	}
	*out_marker = baseline.marker;
	rondo_v3_baselines.emplace(pduel, std::move(baseline));
	return OCG_RONDO_USAGE_OK;
}

namespace {
bool rondo_live_card(const duel* pduel, card* pcard) {
	return pcard != nullptr && pduel->cards.find(pcard) != pduel->cards.end();
}
bool rondo_live_effect(const duel* pduel, effect* peffect) {
	return peffect != nullptr && pduel->effects.find(peffect) != pduel->effects.end();
}
void rondo_located_cards(const duel* pduel, std::vector<card*>& out) {
	for(const auto& player : pduel->game_field->player) {
		for(const auto* list : { &player.list_main, &player.list_hand, &player.list_mzone,
			&player.list_szone, &player.list_grave, &player.list_remove, &player.list_extra }) {
			for(auto* pcard : *list) {
				if(pcard != nullptr)
					out.push_back(pcard);
			}
		}
	}
	std::sort(out.begin(), out.end(), [](const card* left, const card* right) {
		return left->cardid < right->cardid;
	});
}
}

int OCG_RondoDuelQueryEffectOwnersV3(OCG_Duel ocg_duel, uint32_t abi_version,
	uint32_t baseline_marker, OCG_RondoEffectOwnerV3* out_owners, uint32_t capacity,
	uint32_t* out_count) {
	if(abi_version != OCG_RONDO_LATENT_STATE_V3_ABI_VERSION || baseline_marker == 0 ||
		!out_owners || !out_count)
		return OCG_RONDO_USAGE_INVALID_ARGUMENT;
	*out_count = 0;
	std::lock_guard<std::mutex> lock(rondo_duel_mutex);
	auto* pduel = static_cast<duel*>(ocg_duel);
	if(rondo_active_duels.find(pduel) == rondo_active_duels.end())
		return OCG_RONDO_USAGE_INVALID_DUEL;
	if(baseline_marker > pduel->game_field->infos.field_id)
		return OCG_RONDO_USAGE_INVALID_ARGUMENT;
	std::vector<OCG_RondoEffectOwnerV3> owners;
	for(auto* peffect : pduel->effects) {
		if(peffect->initial_id == 0 || peffect->initial_id >= baseline_marker)
			continue;
		if(!rondo_live_card(pduel, peffect->owner) || peffect->owner->cardid == 0)
			continue;
		owners.push_back({ peffect->initial_id, peffect->owner->cardid });
	}
	std::sort(owners.begin(), owners.end(), [](const auto& left, const auto& right) {
		return left.initial_id < right.initial_id;
	});
	if(owners.size() > capacity)
		return OCG_RONDO_USAGE_INSUFFICIENT_CAPACITY;
	for(size_t i = 0; i < owners.size(); ++i)
		out_owners[i] = owners[i];
	*out_count = static_cast<uint32_t>(owners.size());
	return OCG_RONDO_USAGE_OK;
}

int OCG_RondoDuelQueryLatentStateV3(OCG_Duel ocg_duel, uint32_t abi_version,
	uint32_t baseline_marker, OCG_RondoUsageCount* out_counts, uint32_t count_capacity,
	uint32_t* out_count, OCG_RondoCardHistoryV3* out_cards, uint32_t card_capacity,
	uint32_t* out_card_count, OCG_RondoLatentSummaryV3* out_summary) {
	if(abi_version != OCG_RONDO_LATENT_STATE_V3_ABI_VERSION || baseline_marker == 0 ||
		!out_counts || !out_count || !out_cards || !out_card_count || !out_summary)
		return OCG_RONDO_USAGE_INVALID_ARGUMENT;
	*out_count = 0;
	*out_card_count = 0;
	*out_summary = {};
	// Exact v2 authority (validates the duel and marker under its own lock).
	uint32_t unsupported = 0;
	auto status = OCG_RondoDuelQueryLatentState(ocg_duel, OCG_RONDO_LATENT_STATE_ABI_VERSION,
		baseline_marker, out_counts, count_capacity, out_count, &unsupported);
	if(status != OCG_RONDO_USAGE_OK) {
		*out_count = 0;
		return status;
	}
	std::lock_guard<std::mutex> lock(rondo_duel_mutex);
	auto* pduel = static_cast<duel*>(ocg_duel);
	if(rondo_active_duels.find(pduel) == rondo_active_duels.end()) {
		*out_count = 0;
		return OCG_RONDO_USAGE_INVALID_DUEL;
	}
	const auto found_baseline = rondo_v3_baselines.find(pduel);
	if(found_baseline == rondo_v3_baselines.end() || found_baseline->second.marker != baseline_marker) {
		*out_count = 0;
		return OCG_RONDO_USAGE_INVALID_ARGUMENT;
	}
	if(rondo_baseline_effect_changed(pduel, found_baseline->second))
		unsupported |= OCG_RONDO_LATENT_BASELINE_EFFECT_MUTATION;
	std::vector<card*> cards;
	rondo_located_cards(pduel, cards);
	if(cards.size() > card_capacity) {
		*out_count = 0;
		return OCG_RONDO_USAGE_INSUFFICIENT_CAPACITY;
	}
	for(size_t i = 0; i < cards.size(); ++i) {
		const auto* pcard = cards[i];
		OCG_RondoCardHistoryV3 record{};
		record.card_id = pcard->cardid;
		record.moved = pcard->fieldid >= baseline_marker ? 1u : 0u;
		record.turn_id = pcard->turnid;
		record.status = pcard->status;
		record.previous_controller = pcard->previous.controler;
		record.previous_location = pcard->previous.location;
		record.previous_sequence = pcard->previous.sequence;
		record.previous_position = pcard->previous.position;
		record.previous_code = pcard->previous.code;
		record.previous_code2 = pcard->previous.code2;
		record.previous_type = pcard->previous.type;
		record.previous_level = pcard->previous.level;
		record.previous_rank = pcard->previous.rank;
		record.previous_attribute = pcard->previous.attribute;
		record.previous_race = pcard->previous.race;
		record.previous_attack = pcard->previous.attack;
		record.previous_defense = pcard->previous.defense;
		record.reason = pcard->current.reason;
		record.reason_player = pcard->current.reason_player;
		auto* reason_effect = pcard->current.reason_effect;
		if(reason_effect != nullptr) {
			if(!rondo_live_effect(pduel, reason_effect)) {
				record.reason_effect_state = 2;
			} else {
				record.reason_effect_state = 1;
				record.reason_effect_initial_id = reason_effect->initial_id;
				if(rondo_live_card(pduel, reason_effect->owner))
					record.reason_effect_owner_card_id = reason_effect->owner->cardid;
				record.reason_effect_handler_is_owner =
					reason_effect->handler == reason_effect->owner ? 1u : 0u;
			}
		}
		auto* reason_card = pcard->current.reason_card;
		if(reason_card != nullptr) {
			if(rondo_live_card(pduel, reason_card)) {
				record.reason_card_state = 1;
				record.reason_card_id = reason_card->cardid;
			} else {
				record.reason_card_state = 2;
			}
		}
		record.summon_type = pcard->summon.type;
		record.summon_location = pcard->summon.location;
		record.summon_player = pcard->summon.player;
		record.summon_sequence = pcard->summon.sequence;
		record.turn_counter = pcard->turn_counter;
		uint32_t aux = 0;
		if(!pcard->counters.empty())
			aux |= OCG_RONDO_CARD_AUX_COUNTERS;
		if(!pcard->relations.empty() || !pcard->relate_effect.empty())
			aux |= OCG_RONDO_CARD_AUX_RELATIONS;
		if(!pcard->assume.empty())
			aux |= OCG_RONDO_CARD_AUX_ASSUME;
		if(!pcard->indestructable_effects.empty())
			aux |= OCG_RONDO_CARD_AUX_INDESTRUCTABLE;
		if(!pcard->announced_cards.empty() || !pcard->attacked_cards.empty() ||
			!pcard->battled_cards.empty())
			aux |= OCG_RONDO_CARD_AUX_BATTLE_MAPS;
		if(pcard->equiping_target || pcard->pre_equip_target || pcard->overlay_target ||
			pcard->pre_overlay_target || !pcard->equiping_cards.empty() ||
			!pcard->effect_target_owner.empty() || !pcard->effect_target_cards.empty())
			aux |= OCG_RONDO_CARD_AUX_EQUIP_TARGETS;
		if(!pcard->material_cards.empty() || !pcard->xyz_materials.empty())
			aux |= OCG_RONDO_CARD_AUX_MATERIALS;
		if(pcard->previous.pzone || pcard->current.pzone || pcard->summon.pzone)
			aux |= OCG_RONDO_CARD_AUX_PZONE;
		if(pcard->spsummon_counter[0] || pcard->spsummon_counter[1] ||
			pcard->spsummon_counter_rst[0] || pcard->spsummon_counter_rst[1])
			aux |= OCG_RONDO_CARD_AUX_SPSUMMON_COUNTER;
		// attack_all_target is deliberately excluded: every relocation resets it
		// and it is only read for a field attacker after RESET_TOFIELD resets it.
		if(pcard->attack_announce_count || pcard->announce_count || pcard->attacked_count ||
			pcard->direct_attackable)
			aux |= OCG_RONDO_CARD_AUX_ATTACK_COUNTS;
		record.auxiliary_state = aux;
		record.unique_code = pcard->unique_code;
		out_cards[i] = record;
	}
	*out_card_count = static_cast<uint32_t>(cards.size());
	out_summary->unsupported = unsupported &
		~(OCG_RONDO_LATENT_PHASE_ACTIVITY | OCG_RONDO_LATENT_CARD_MOVE_PROVENANCE);
	out_summary->phase_action = pduel->game_field->core.phase_action ? 1u : 0u;
	return OCG_RONDO_USAGE_OK;
}

void OCG_DuelNewCard(OCG_Duel ocg_duel, const OCG_NewCardInfo* info_ptr) {
	auto* pduel = static_cast<duel*>(ocg_duel);
	auto& game_field = *(pduel->game_field);
	const auto& info = *info_ptr;
	if(bit::popcnt(info.loc) > 1)
		return;
	auto duelist = info.duelist;
	if(duelist == 0) {
		if(game_field.is_location_useable(info.con, info.loc, info.seq)) {
			card* pcard = pduel->new_card(info.code);
			pcard->owner = info.team;
			game_field.add_card(info.con, pcard, (uint8_t)info.loc, (uint8_t)info.seq);
			pcard->current.position = info.pos;
			if(!(info.loc & LOCATION_ONFIELD) || (info.pos & POS_FACEUP)) {
				pcard->enable_field_effect(true);
				game_field.adjust_instant();
			}
			if(info.loc & LOCATION_ONFIELD) {
				if(info.loc == LOCATION_MZONE)
					pcard->set_status(STATUS_PROC_COMPLETE, TRUE);
			}
		}
	} else {
		if(info.team > 1 || !(info.loc & (LOCATION_DECK | LOCATION_EXTRA)))
			return;
		card* pcard = pduel->new_card(info.code);
		auto& player = game_field.player[info.team];
		if(duelist > player.extra_lists_main.size()) {
			player.extra_lists_main.resize(duelist);
			player.extra_lists_extra.resize(duelist);
			player.extra_lists_hand.resize(duelist);
			player.extra_extra_p_count.resize(duelist);
		}
		--duelist;
		pcard->current.location = static_cast<uint8_t>(info.loc);
		pcard->owner = info.team;
		pcard->current.controler = info.team;
		pcard->current.position = POS_FACEDOWN_DEFENSE;
		auto& list = (info.loc == LOCATION_DECK) ? player.extra_lists_main[duelist] : player.extra_lists_extra[duelist];
		list.push_back(pcard);
		pcard->current.sequence = static_cast<uint32_t>(list.size() - 1);
	}
}

void OCG_StartDuel(OCG_Duel ocg_duel) {
	auto* pduel = static_cast<duel*>(ocg_duel);
	pduel->game_field->emplace_process<Processors::Startup>();
}

int OCG_DuelProcess(OCG_Duel ocg_duel) {
	auto* pduel = static_cast<duel*>(ocg_duel);
	pduel->buff.clear();
	auto flag = OCG_DUEL_STATUS_END;
	do {
		flag = pduel->game_field->process();
		pduel->generate_buffer();
	} while(pduel->buff.size() == 0 && flag == OCG_DUEL_STATUS_CONTINUE);
	return flag;
}

void* OCG_DuelGetMessage(OCG_Duel ocg_duel, uint32_t* length) {
	auto* pduel = static_cast<duel*>(ocg_duel);
	pduel->generate_buffer();
	if(length)
		*length = static_cast<uint32_t>(pduel->buff.size());
	return pduel->buff.data();
}

void OCG_DuelSetResponse(OCG_Duel ocg_duel, const void* buffer, uint32_t length) {
	auto* pduel = static_cast<duel*>(ocg_duel);
	pduel->set_response(buffer, length);
}

int OCG_LoadScript(OCG_Duel ocg_duel, const char* buffer, uint32_t length, const char* name) {
	auto* pduel = static_cast<duel*>(ocg_duel);
	return pduel->lua->load_script(buffer, length, name);
}

uint32_t OCG_DuelQueryCount(OCG_Duel ocg_duel, uint8_t team, uint32_t loc) {
	auto* pduel = static_cast<duel*>(ocg_duel);
	if(team > 1)
		return 0;
	if(bit::popcnt(loc) != 1)
		return 0;
	auto& player = pduel->game_field->player[team];
	if(loc == LOCATION_HAND)
		return static_cast<uint32_t>(player.list_hand.size());
	if(loc == LOCATION_GRAVE)
		return static_cast<uint32_t>(player.list_grave.size());
	if (loc == LOCATION_REMOVED)
		return static_cast<uint32_t>(player.list_remove.size());
	if (loc == LOCATION_EXTRA)
		return static_cast<uint32_t>(player.list_extra.size());
	if (loc == LOCATION_DECK)
		return static_cast<uint32_t>(player.list_main.size());
	uint32_t count = 0;
	if(loc == LOCATION_MZONE) {
		for(auto& pcard : player.list_mzone)
			if(pcard) ++count;
	}
	if(loc == LOCATION_SZONE) {
		for(auto& pcard : player.list_szone)
			if(pcard) ++count;
	}
	return count;
}
template<typename T>
void insert_value_int(std::vector<uint8_t>& vec, T val) {
	const auto vec_size = vec.size();
	const auto val_size = sizeof(T);
	vec.resize(vec_size + val_size);
	std::memcpy(&vec[vec_size], &val, val_size);
}
template<typename T, typename T2>
ForceInline void insert_value(std::vector<uint8_t>& vec, T2 val) {
	insert_value_int<T>(vec, static_cast<T>(val));
}

void* OCG_DuelQuery(OCG_Duel ocg_duel, uint32_t* length, const OCG_QueryInfo* info_ptr) {
	const auto& info = *info_ptr;
	auto* pduel = static_cast<duel*>(ocg_duel);
	if(bit::popcnt(info.loc & ~LOCATION_OVERLAY) != 1)
		return nullptr;
	pduel->query_buffer.clear();
	card* pcard = nullptr;
	if(info.loc & LOCATION_OVERLAY) {
		auto olcard = pduel->game_field->get_field_card(info.con, (info.loc & LOCATION_OVERLAY), info.seq);
		if(olcard && olcard->xyz_materials.size() > info.overlay_seq) {
			pcard = olcard->xyz_materials[info.overlay_seq];
		}
	} else {
		pcard = pduel->game_field->get_field_card(info.con, info.loc, info.seq);
	}
	if(pcard == nullptr) {
		if(length)
			*length = 0;
		return nullptr;
	} else {
		pcard->get_infos(info.flags);
	}
	if(length)
		*length = static_cast<uint32_t>(pduel->query_buffer.size());
	return pduel->query_buffer.data();
}

void* OCG_DuelQueryLocation(OCG_Duel ocg_duel, uint32_t* length, const OCG_QueryInfo* info_ptr) {
	const auto& info = *info_ptr;
	auto* pduel = static_cast<duel*>(ocg_duel);
	auto& buffer = pduel->query_buffer;
	auto populate = [&flags = info.flags, &buffer](const card_vector& list) {
		for(auto& pcard : list) {
			if(pcard == nullptr) {
				insert_value<int16_t>(buffer, 0);
			} else {
				pcard->get_infos(flags);
			}
		}
	};
	buffer.clear();
	if(info.con <= 1u && bit::popcnt(info.loc & ~LOCATION_OVERLAY) == 1) {
		if(info.loc & LOCATION_OVERLAY) {
			insert_value<int16_t>(buffer, 0);
		} else {
			auto& player = pduel->game_field->player[info.con];
			if(info.loc == LOCATION_MZONE)
				populate(player.list_mzone);
			else if(info.loc == LOCATION_SZONE)
				populate(player.list_szone);
			else if(info.loc == LOCATION_HAND)
				populate(player.list_hand);
			else if(info.loc == LOCATION_GRAVE)
				populate(player.list_grave);
			else if(info.loc == LOCATION_REMOVED)
				populate(player.list_remove);
			else if(info.loc == LOCATION_EXTRA)
				populate(player.list_extra);
			else if(info.loc == LOCATION_DECK)
				populate(player.list_main);
		}
		std::vector<uint8_t> tmp_vector;
		insert_value<uint32_t>(tmp_vector, buffer.size());
		buffer.insert(buffer.begin(), tmp_vector.begin(), tmp_vector.end());
	}
	if(length)
		*length = static_cast<uint32_t>(buffer.size());
	return buffer.data();
}

void* OCG_DuelQueryField(OCG_Duel ocg_duel, uint32_t* length) {
	auto* pduel = static_cast<duel*>(ocg_duel);
	auto& query = pduel->query_buffer;
	query.clear();
	//insert_value<int8_t>(query, MSG_RELOAD_FIELD);
	insert_value<uint32_t>(query, pduel->game_field->core.duel_options);
	for(int playerid = 0; playerid < 2; ++playerid) {
		auto& player = pduel->game_field->player[playerid];
		insert_value<uint32_t>(query, player.lp);
		for(auto& pcard : player.list_mzone) {
			if(pcard) {
				insert_value<uint8_t>(query, 1);
				insert_value<uint8_t>(query, pcard->current.position);
				insert_value<uint32_t>(query, pcard->xyz_materials.size());
			} else {
				insert_value<uint8_t>(query, 0);
			}
		}
		for(auto& pcard : player.list_szone) {
			if(pcard) {
				insert_value<uint8_t>(query, 1);
				insert_value<uint8_t>(query, pcard->current.position);
				insert_value<uint32_t>(query, pcard->xyz_materials.size());
			} else {
				insert_value<uint8_t>(query, 0);
			}
		}
		insert_value<uint32_t>(query, player.list_main.size());
		insert_value<uint32_t>(query, player.list_hand.size());
		insert_value<uint32_t>(query, player.list_grave.size());
		insert_value<uint32_t>(query, player.list_remove.size());
		insert_value<uint32_t>(query, player.list_extra.size());
		insert_value<uint32_t>(query, player.extra_p_count);
	}
	insert_value<uint32_t>(query, pduel->game_field->core.current_chain.size());
	for(const auto& ch : pduel->game_field->core.current_chain) {
		effect* peffect = ch.triggering_effect;
		insert_value<uint32_t>(query, peffect->get_handler()->data.code);
		loc_info info = peffect->get_handler()->get_info_location();
		insert_value<uint8_t>(query, info.controler);
		insert_value<uint8_t>(query, info.location);
		insert_value<uint32_t>(query, info.sequence);
		insert_value<uint32_t>(query, info.position);
		insert_value<uint8_t>(query, ch.triggering_controler);
		insert_value<uint8_t>(query, static_cast<uint8_t>(ch.triggering_location));
		insert_value<uint32_t>(query, ch.triggering_sequence);
		insert_value<uint64_t>(query, peffect->description);
	}
	if(length)
		*length = static_cast<uint32_t>(query.size());
	return query.data();
}
