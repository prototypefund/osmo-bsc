/* Gb-proxy TLLI state handling */

/* (C) 2014 by On-Waves
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <osmocom/gsm/gsm48.h>

#include <openbsc/gb_proxy.h>

#include <openbsc/gprs_utils.h>
#include <openbsc/gprs_gb_parse.h>

#include <openbsc/debug.h>

#include <osmocom/gsm/gsm_utils.h>

#include <osmocom/core/rate_ctr.h>
#include <osmocom/core/talloc.h>

struct gbproxy_tlli_info *gbproxy_find_tlli(struct gbproxy_peer *peer,
					    uint32_t tlli)
{
	struct gbproxy_tlli_info *tlli_info;
	struct gbproxy_patch_state *state = &peer->patch_state;

	llist_for_each_entry(tlli_info, &state->enabled_tllis, list)
		if (tlli_info->tlli.current == tlli ||
		    tlli_info->tlli.assigned == tlli)
			return tlli_info;

	return NULL;
}

struct gbproxy_tlli_info *gbproxy_find_tlli_by_ptmsi(
	struct gbproxy_peer *peer,
	uint32_t ptmsi)
{
	struct gbproxy_tlli_info *tlli_info;
	struct gbproxy_patch_state *state = &peer->patch_state;

	llist_for_each_entry(tlli_info, &state->enabled_tllis, list)
		if (tlli_info->tlli.ptmsi == ptmsi)
			return tlli_info;

	return NULL;
}

struct gbproxy_tlli_info *gbproxy_find_tlli_by_sgsn_tlli(
	struct gbproxy_peer *peer,
	uint32_t tlli)
{
	struct gbproxy_tlli_info *tlli_info;
	struct gbproxy_patch_state *state = &peer->patch_state;

	llist_for_each_entry(tlli_info, &state->enabled_tllis, list)
		if (tlli_info->sgsn_tlli.current == tlli ||
		    tlli_info->sgsn_tlli.assigned == tlli)
			return tlli_info;

	return NULL;
}

struct gbproxy_tlli_info *gbproxy_find_tlli_by_imsi(
	struct gbproxy_peer *peer,
	const uint8_t *imsi,
	size_t imsi_len)
{
	struct gbproxy_tlli_info *tlli_info;
	struct gbproxy_patch_state *state = &peer->patch_state;

	if (!gprs_is_mi_imsi(imsi, imsi_len))
		return NULL;

	llist_for_each_entry(tlli_info, &state->enabled_tllis, list) {
		if (tlli_info->imsi_len != imsi_len)
			continue;
		if (memcmp(tlli_info->imsi, imsi, imsi_len) != 0)
			continue;

		return tlli_info;
	}

	return NULL;
}

void gbproxy_tlli_info_discard_messages(struct gbproxy_tlli_info *tlli_info)
{
	struct msgb *msg, *nxt;

	llist_for_each_entry_safe(msg, nxt, &tlli_info->stored_msgs, list) {
		llist_del(&msg->list);
		msgb_free(msg);
	}
}

void gbproxy_delete_tlli(struct gbproxy_peer *peer,
			 struct gbproxy_tlli_info *tlli_info)
{
	struct gbproxy_patch_state *state = &peer->patch_state;

	gbproxy_tlli_info_discard_messages(tlli_info);

	llist_del(&tlli_info->list);
	talloc_free(tlli_info);
	state->enabled_tllis_count -= 1;

	peer->ctrg->ctr[GBPROX_PEER_CTR_TLLI_CACHE_SIZE].current =
		state->enabled_tllis_count;
}

void gbproxy_delete_tllis(struct gbproxy_peer *peer)
{
	struct gbproxy_tlli_info *tlli_info, *nxt;
	struct gbproxy_patch_state *state = &peer->patch_state;

	llist_for_each_entry_safe(tlli_info, nxt, &state->enabled_tllis, list)
		gbproxy_delete_tlli(peer, tlli_info);

	OSMO_ASSERT(state->enabled_tllis_count == 0);
	OSMO_ASSERT(llist_empty(&state->enabled_tllis));
}

void gbproxy_attach_tlli_info(struct gbproxy_peer *peer, time_t now,
			      struct gbproxy_tlli_info *tlli_info)
{
	struct gbproxy_patch_state *state = &peer->patch_state;

	tlli_info->timestamp = now;
	llist_add(&tlli_info->list, &state->enabled_tllis);
	state->enabled_tllis_count += 1;

	peer->ctrg->ctr[GBPROX_PEER_CTR_TLLI_CACHE_SIZE].current =
		state->enabled_tllis_count;
}

int gbproxy_remove_stale_tllis(struct gbproxy_peer *peer, time_t now)
{
	struct gbproxy_patch_state *state = &peer->patch_state;
	int exceeded_max_len = 0;
	int deleted_count = 0;
	int check_for_age;

	if (peer->cfg->tlli_max_len > 0)
		exceeded_max_len =
			state->enabled_tllis_count - peer->cfg->tlli_max_len;

	check_for_age = peer->cfg->tlli_max_age > 0;

	for (; exceeded_max_len > 0; exceeded_max_len--) {
		struct gbproxy_tlli_info *tlli_info;
		OSMO_ASSERT(!llist_empty(&state->enabled_tllis));
		tlli_info = llist_entry(state->enabled_tllis.prev,
					struct gbproxy_tlli_info,
					list);
		LOGP(DGPRS, LOGL_INFO,
		     "Removing TLLI %08x from list "
		     "(stale, length %d, max_len exceeded)\n",
		     tlli_info->tlli.current, state->enabled_tllis_count);

		gbproxy_delete_tlli(peer, tlli_info);
		deleted_count += 1;
	}

	while (check_for_age && !llist_empty(&state->enabled_tllis)) {
		time_t age;
		struct gbproxy_tlli_info *tlli_info;
		tlli_info = llist_entry(state->enabled_tllis.prev,
					struct gbproxy_tlli_info,
					list);
		age = now - tlli_info->timestamp;
		/* age < 0 only happens after system time jumps, discard entry */
		if (age <= peer->cfg->tlli_max_age && age >= 0) {
			check_for_age = 0;
			continue;
		}

		LOGP(DGPRS, LOGL_INFO,
		     "Removing TLLI %08x from list "
		     "(stale, age %d, max_age exceeded)\n",
		     tlli_info->tlli.current, (int)age);

		gbproxy_delete_tlli(peer, tlli_info);
		deleted_count += 1;
	}

	return deleted_count;
}

struct gbproxy_tlli_info *gbproxy_tlli_info_alloc( struct gbproxy_peer *peer)
{
	struct gbproxy_tlli_info *tlli_info;

	tlli_info = talloc_zero(peer, struct gbproxy_tlli_info);
	tlli_info->tlli.ptmsi = GSM_RESERVED_TMSI;
	tlli_info->sgsn_tlli.ptmsi = GSM_RESERVED_TMSI;

	INIT_LLIST_HEAD(&tlli_info->stored_msgs);

	return tlli_info;
}

void gbproxy_detach_tlli_info(
	struct gbproxy_peer *peer,
	struct gbproxy_tlli_info *tlli_info)
{
	struct gbproxy_patch_state *state = &peer->patch_state;

	llist_del(&tlli_info->list);
	OSMO_ASSERT(state->enabled_tllis_count > 0);
	state->enabled_tllis_count -= 1;

	peer->ctrg->ctr[GBPROX_PEER_CTR_TLLI_CACHE_SIZE].current =
		state->enabled_tllis_count;
}

void gbproxy_update_tlli_info(struct gbproxy_tlli_info *tlli_info,
			      const uint8_t *imsi, size_t imsi_len)
{
	if (!gprs_is_mi_imsi(imsi, imsi_len))
		return;

	tlli_info->imsi_len = imsi_len;
	tlli_info->imsi =
		talloc_realloc_size(tlli_info, tlli_info->imsi, imsi_len);
	OSMO_ASSERT(tlli_info->imsi != NULL);
	memcpy(tlli_info->imsi, imsi, imsi_len);
}

void gbproxy_reassign_tlli(struct gbproxy_tlli_state *tlli_state,
			   struct gbproxy_peer *peer, uint32_t new_tlli)
{
	if (new_tlli == tlli_state->current)
		return;

	LOGP(DGPRS, LOGL_INFO,
	     "The TLLI has been reassigned from %08x to %08x\n",
	     tlli_state->current, new_tlli);

	/* Remember assigned TLLI */
	tlli_state->assigned = new_tlli;
	tlli_state->bss_validated = 0;
	tlli_state->net_validated = 0;
}

uint32_t gbproxy_map_tlli(uint32_t other_tlli,
			  struct gbproxy_tlli_info *tlli_info, int to_bss)
{
	uint32_t tlli = 0;
	struct gbproxy_tlli_state *src, *dst;
	if (to_bss) {
		src = &tlli_info->sgsn_tlli;
		dst = &tlli_info->tlli;
	} else {
		src = &tlli_info->tlli;
		dst = &tlli_info->sgsn_tlli;
	}
	if (src->current == other_tlli)
		tlli = dst->current;
	else if (src->assigned == other_tlli)
		tlli = dst->assigned;

	return tlli;
}

static void gbproxy_validate_tlli(struct gbproxy_tlli_state *tlli_state,
				  uint32_t tlli, int to_bss)
{
	LOGP(DGPRS, LOGL_DEBUG,
	     "%s({current = %08x, assigned = %08x, net_vld = %d, bss_vld = %d}, %08x)\n",
	     __func__, tlli_state->current, tlli_state->assigned,
	     tlli_state->net_validated, tlli_state->bss_validated, tlli);

	if (!tlli_state->assigned || tlli_state->assigned != tlli)
		return;

	/* TODO: Is this ok? Check spec */
	if (gprs_tlli_type(tlli) != TLLI_LOCAL)
		return;

	/* See GSM 04.08, 4.7.1.5 */
	if (to_bss)
		tlli_state->net_validated = 1;
	else
		tlli_state->bss_validated = 1;

	if (!tlli_state->bss_validated || !tlli_state->net_validated)
		return;

	LOGP(DGPRS, LOGL_INFO,
	     "The TLLI %08x has been validated (was %08x)\n",
	     tlli_state->assigned, tlli_state->current);

	tlli_state->current = tlli;
	tlli_state->assigned = 0;
}

void gbproxy_touch_tlli(struct gbproxy_peer *peer,
			struct gbproxy_tlli_info *tlli_info, time_t now)
{
	gbproxy_detach_tlli_info(peer, tlli_info);
	gbproxy_attach_tlli_info(peer, now, tlli_info);
}

static void gbproxy_unregister_tlli(struct gbproxy_peer *peer, uint32_t tlli)
{
	struct gbproxy_tlli_info *tlli_info;

	tlli_info = gbproxy_find_tlli(peer, tlli);
	if (tlli_info) {
		LOGP(DGPRS, LOGL_INFO,
		     "Removing TLLI %08x from list\n",
		     tlli);
		gbproxy_delete_tlli(peer, tlli_info);
	}
}

int gbproxy_check_tlli(struct gbproxy_peer *peer,
		       struct gbproxy_tlli_info *tlli_info)
{
	if (!peer->cfg->check_imsi)
		return 1;

	return tlli_info != NULL && tlli_info->enable_patching;
}

struct gbproxy_tlli_info *gbproxy_update_tlli_state_ul(
	struct gbproxy_peer *peer,
	time_t now,
	struct gprs_gb_parse_context *parse_ctx)
{
	struct gbproxy_tlli_info *tlli_info = NULL;

	if (parse_ctx->tlli_enc) {
		tlli_info = gbproxy_find_tlli(peer, parse_ctx->tlli);

		if (!tlli_info && parse_ctx->imsi)
			tlli_info = gbproxy_find_tlli_by_imsi(
				peer, parse_ctx->imsi, parse_ctx->imsi_len);
	}

	if (parse_ctx->tlli_enc && parse_ctx->llc) {
		uint32_t sgsn_tlli;
		if (!tlli_info) {
			LOGP(DGPRS, LOGL_INFO, "Adding TLLI %08x to list\n",
			    parse_ctx->tlli);
			tlli_info = gbproxy_tlli_info_alloc(peer);
			gbproxy_attach_tlli_info(peer, now, tlli_info);

			/* Setup TLLIs */
			sgsn_tlli = gbproxy_make_sgsn_tlli(peer, tlli_info,
							   parse_ctx->tlli);
			tlli_info->sgsn_tlli.current = sgsn_tlli;
			tlli_info->tlli.current = parse_ctx->tlli;;
		} else {
			sgsn_tlli = gbproxy_map_tlli(parse_ctx->tlli, tlli_info, 0);
			if (!sgsn_tlli)
				sgsn_tlli = gbproxy_make_sgsn_tlli(peer, tlli_info,
								   parse_ctx->tlli);

			gbproxy_validate_tlli(&tlli_info->tlli,
					      parse_ctx->tlli, 0);
			gbproxy_validate_tlli(&tlli_info->sgsn_tlli,
					      sgsn_tlli, 0);
			gbproxy_touch_tlli(peer, tlli_info, now);
		}
	} else if (tlli_info) {
		gbproxy_touch_tlli(peer, tlli_info, now);
	}

	if (parse_ctx->imsi && tlli_info && tlli_info->imsi_len == 0) {
		int enable_patching;
		gbproxy_update_tlli_info(tlli_info,
					 parse_ctx->imsi, parse_ctx->imsi_len);

		/* Check, whether the IMSI matches */
		enable_patching = gbproxy_check_imsi(peer, parse_ctx->imsi,
						     parse_ctx->imsi_len);
		if (enable_patching >= 0)
			tlli_info->enable_patching = enable_patching;
	}

	return tlli_info;
}

struct gbproxy_tlli_info *gbproxy_update_tlli_state_dl(
	struct gbproxy_peer *peer,
	time_t now,
	struct gprs_gb_parse_context *parse_ctx)
{
	struct gbproxy_tlli_info *tlli_info = NULL;

	if (parse_ctx->tlli_enc)
		tlli_info = gbproxy_find_tlli_by_sgsn_tlli(peer, parse_ctx->tlli);

	if (parse_ctx->tlli_enc && parse_ctx->new_ptmsi_enc && tlli_info) {
		/* A new P-TMSI has been signalled in the message,
		 * register new TLLI */
		uint32_t new_sgsn_ptmsi;
		uint32_t new_bss_ptmsi;
		if (!gprs_parse_mi_tmsi(parse_ctx->new_ptmsi_enc, GSM48_TMSI_LEN,
					&new_sgsn_ptmsi)) {
			LOGP(DGPRS, LOGL_ERROR,
			     "Failed to parse new TLLI/PTMSI (current is %08x)\n",
			     parse_ctx->tlli);
			return tlli_info;
		}
		new_bss_ptmsi = gbproxy_make_bss_ptmsi(peer, new_sgsn_ptmsi);

		LOGP(DGPRS, LOGL_INFO,
		     "Got new PTMSI %08x from SGSN, using %08x for BSS\n",
		     new_sgsn_ptmsi, new_bss_ptmsi);
		/* Setup PTMSIs */
		tlli_info->sgsn_tlli.ptmsi = new_sgsn_ptmsi;
		tlli_info->tlli.ptmsi = new_bss_ptmsi;
	} else if (parse_ctx->tlli_enc && parse_ctx->new_ptmsi_enc && !tlli_info &&
		   !peer->cfg->patch_ptmsi) {
		/* A new P-TMSI has been signalled in the message with an unknown
		 * TLLI, create a new tlli_info */
		/* TODO: Add a test case for this branch */
		uint32_t new_ptmsi;
		if (!gprs_parse_mi_tmsi(parse_ctx->new_ptmsi_enc, GSM48_TMSI_LEN,
					&new_ptmsi)) {
			LOGP(DGPRS, LOGL_ERROR,
			     "Failed to parse new PTMSI (TLLI is %08x)\n",
			     parse_ctx->tlli);
			return tlli_info;
		}

		LOGP(DGPRS, LOGL_INFO,
		     "Adding TLLI %08x to list (SGSN, new P-TMSI is %08x)\n",
		     parse_ctx->tlli, new_ptmsi);

		tlli_info = gbproxy_tlli_info_alloc(peer);
		tlli_info->sgsn_tlli.current = parse_ctx->tlli;;
		tlli_info->tlli.current = parse_ctx->tlli;;
		tlli_info->sgsn_tlli.ptmsi = new_ptmsi;
		tlli_info->tlli.ptmsi = new_ptmsi;
		gbproxy_attach_tlli_info(peer, now, tlli_info);
	} else if (parse_ctx->tlli_enc && parse_ctx->llc && !tlli_info &&
		   !peer->cfg->patch_ptmsi) {
		/* Unknown SGSN TLLI, create a new tlli_info */
		uint32_t new_ptmsi;
		tlli_info = gbproxy_tlli_info_alloc(peer);
		LOGP(DGPRS, LOGL_INFO, "Adding TLLI %08x to list (SGSN)\n",
		     parse_ctx->tlli);

		gbproxy_attach_tlli_info(peer, now, tlli_info);

		/* Setup TLLIs */
		tlli_info->sgsn_tlli.current = parse_ctx->tlli;
		tlli_info->tlli.current = parse_ctx->tlli;

		if (!parse_ctx->new_ptmsi_enc)
			return tlli_info;
		/* A new P-TMSI has been signalled in the message */

		if (!gprs_parse_mi_tmsi(parse_ctx->new_ptmsi_enc,
					GSM48_TMSI_LEN, &new_ptmsi)) {
			LOGP(DGPRS, LOGL_ERROR,
			     "Failed to parse new PTMSI (TLLI is %08x)\n",
			     parse_ctx->tlli);
			return tlli_info;
		}
		LOGP(DGPRS, LOGL_INFO,
		     "Assigning new P-TMSI %08x\n", new_ptmsi);
		/* Setup P-TMSIs */
		tlli_info->sgsn_tlli.ptmsi = new_ptmsi;
		tlli_info->tlli.ptmsi = new_ptmsi;
	} else if (parse_ctx->tlli_enc && parse_ctx->llc && tlli_info) {
		uint32_t bss_tlli = gbproxy_map_tlli(parse_ctx->tlli,
						     tlli_info, 1);
		gbproxy_validate_tlli(&tlli_info->sgsn_tlli, parse_ctx->tlli, 1);
		gbproxy_validate_tlli(&tlli_info->tlli, bss_tlli, 1);
		gbproxy_touch_tlli(peer, tlli_info, now);
	} else if (tlli_info) {
		gbproxy_touch_tlli(peer, tlli_info, now);
	}

	if (parse_ctx->imsi && tlli_info && tlli_info->imsi_len == 0) {
		int enable_patching;
		gbproxy_update_tlli_info(tlli_info,
					 parse_ctx->imsi, parse_ctx->imsi_len);

		/* Check, whether the IMSI matches */
		enable_patching = gbproxy_check_imsi(peer, parse_ctx->imsi,
						     parse_ctx->imsi_len);
		if (enable_patching >= 0)
			tlli_info->enable_patching = enable_patching;
	}

	return tlli_info;
}

void gbproxy_update_tlli_state_after(
	struct gbproxy_peer *peer,
	struct gbproxy_tlli_info *tlli_info,
	time_t now,
	struct gprs_gb_parse_context *parse_ctx)
{
	if (parse_ctx->invalidate_tlli) {
		gbproxy_unregister_tlli(peer, parse_ctx->tlli);
	} else if (parse_ctx->to_bss && parse_ctx->tlli_enc &&
		   parse_ctx->new_ptmsi_enc && tlli_info) {
		/* A new PTMSI has been signaled in the message,
		 * register new TLLI */
		uint32_t new_sgsn_ptmsi = tlli_info->sgsn_tlli.ptmsi;
		uint32_t new_bss_ptmsi = tlli_info->tlli.ptmsi;
		uint32_t new_sgsn_tlli;
		uint32_t new_bss_tlli = 0;

		new_sgsn_tlli = gprs_tmsi2tlli(new_sgsn_ptmsi, TLLI_LOCAL);
		if (new_bss_ptmsi != GSM_RESERVED_TMSI)
			new_bss_tlli = gprs_tmsi2tlli(new_bss_ptmsi, TLLI_LOCAL);
		LOGP(DGPRS, LOGL_INFO,
		     "Assigning new TLLI %08x to SGSN, %08x to BSS\n",
		     new_sgsn_tlli, new_bss_tlli);

		gbproxy_reassign_tlli(&tlli_info->sgsn_tlli,
				      peer, new_sgsn_tlli);
		gbproxy_reassign_tlli(&tlli_info->tlli,
				      peer, new_bss_tlli);
	}

	gbproxy_remove_stale_tllis(peer, now);
}

