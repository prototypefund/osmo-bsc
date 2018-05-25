/*
 * Handle the connection to the MSC. This include ping/timeout/reconnect
 * (C) 2008-2009 by Harald Welte <laforge@gnumonks.org>
 * (C) 2009-2015 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2009-2015 by On-Waves
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

#include <osmocom/bsc/bsc_nat.h>
#include <osmocom/ctrl/control_cmd.h>
#include <osmocom/ctrl/control_if.h>
#include <osmocom/crypt/auth.h>
#include <osmocom/bsc/debug.h>
#include <osmocom/bsc/gsm_data.h>
#include <osmocom/bsc/ipaccess.h>
#include <osmocom/bsc/bsc_msc_data.h>
#include <osmocom/bsc/signal.h>

#include <osmocom/core/talloc.h>

#include <osmocom/gsm/gsm0808.h>

#include <osmocom/abis/ipa.h>

#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>

int osmo_bsc_msc_init(struct bsc_msc_data *data)
{
	data->msc_con = bsc_msc_create(data, &data->dests);
	if (!data->msc_con) {
		LOGP(DMSC, LOGL_ERROR, "Creating the MSC network connection failed.\n");
		return -1;
	}

	/* FIXME: This is a leftover from the old architecture that used
	 * sccp-lite with osmocom specific authentication. Since we now
	 * changed to AoIP the connected status and the authentication
	 * status is managed differently. However osmo_bsc_filter.c still
	 * needs the flags to be set to one. See also: OS#3112 */
	data->msc_con->is_connected = 1;
	data->msc_con->is_authenticated = 1;

	return 0;
}

struct bsc_msc_data *osmo_msc_data_find(struct gsm_network *net, int nr)
{
	struct bsc_msc_data *msc_data;

	llist_for_each_entry(msc_data, &net->bsc_data->mscs, entry)
		if (msc_data->nr == nr)
			return msc_data;
	return NULL;
}

struct bsc_msc_data *osmo_msc_data_alloc(struct gsm_network *net, int nr)
{
	struct bsc_msc_data *msc_data;

	/* check if there is already one */
	msc_data = osmo_msc_data_find(net, nr);
	if (msc_data)
		return msc_data;

	msc_data = talloc_zero(net, struct bsc_msc_data);
	if (!msc_data)
		return NULL;

	llist_add_tail(&msc_data->entry, &net->bsc_data->mscs);

	/* Init back pointer */
	msc_data->network = net;

	INIT_LLIST_HEAD(&msc_data->dests);
	msc_data->core_plmn = (struct osmo_plmn_id){
		.mcc = GSM_MCC_MNC_INVALID,
		.mnc = GSM_MCC_MNC_INVALID,
	};
	msc_data->core_ci = -1;
	msc_data->core_lac = -1;
	msc_data->rtp_base = 4000;

	msc_data->nr = nr;
	msc_data->allow_emerg = 1;
	msc_data->a.asp_proto = OSMO_SS7_ASP_PROT_M3UA;

	/* Defaults for the audio setup */
	msc_data->amr_conf.m5_90 = 1;

	return msc_data;
}

