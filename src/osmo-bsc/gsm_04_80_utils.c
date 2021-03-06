/* OpenBSC utility functions for 3GPP TS 04.80 */

/* (C) 2016 by sysmocom s.m.f.c. GmbH <info@sysmocom.de>
 *
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

#include <osmocom/gsm/gsm0480.h>
#include <osmocom/bsc/bsc_subscr_conn_fsm.h>

int bsc_send_ussd_notify(struct gsm_subscriber_connection *conn, int level,
			 const char *text)
{
	struct msgb *msg = gsm0480_create_ussd_notify(level, text);
	if (!msg)
		return -1;
	gscon_submit_rsl_dtap(conn, msg, 0, 0);
	return 0;
}

int bsc_send_ussd_release_complete(struct gsm_subscriber_connection *conn)
{
	/* ugly: we obviously don't know if TID 0 is currently in user for the given subscriber... */
	struct msgb *msg = gsm0480_create_release_complete(0);
	if (!msg)
		return -1;
	gscon_submit_rsl_dtap(conn, msg, 0, 0);
	return 0;
}
