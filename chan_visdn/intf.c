/*
 * vISDN channel driver for Asterisk
 *
 * Copyright (C) 2006 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU General Public License.
 *
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <signal.h>
#include <ctype.h>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <linux/if_ether.h>
#include <net/if_arp.h>

#include <asterisk/version.h>
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10600)
#include <linux/if.h>
#else
#include <net/if.h>
#include <asterisk.h>
#endif
#include <asterisk/lock.h>
#include <asterisk/channel.h>
#include <asterisk/config.h>
#include <asterisk/logger.h>
#include <asterisk/module.h>
#include <asterisk/pbx.h>
#include <asterisk/options.h>
#include <asterisk/cli.h>

#include <linux/lapd.h>
#include <libq931/intf.h>
#include <libq931/timer.h>

#include "chan_visdn.h"
#include "util.h"
#include "huntgroup.h"
#include "ton.h"
#include "numbers_list.h"
#include "debug.h"

#define FAILED_RETRY_TIME (30 * SEC)
#define WAITING_INITIALIZATION_DELAY (2 * SEC)

struct visdn_intf *visdn_intf_alloc(void)
{
	struct visdn_intf *intf;

	intf = malloc(sizeof(*intf));
	if (!intf)
		return NULL;

	memset(intf, 0, sizeof(*intf));

	intf->refcnt = 1;

	intf->status = VISDN_INTF_STATUS_UNINITIALIZED;
	intf->timer_expiration = -1;

	intf->q931_intf = NULL;

	INIT_LIST_HEAD(&intf->suspended_calls);

	return intf;
}

struct visdn_intf *visdn_intf_get(struct visdn_intf *intf)
{
	assert(intf);
	assert(intf->refcnt > 0);

	ast_mutex_lock(&visdn.usecnt_lock);
	intf->refcnt++;
	ast_mutex_unlock(&visdn.usecnt_lock);

	return intf;
}

void visdn_intf_put(struct visdn_intf *intf)
{
	assert(intf);
	assert(intf->refcnt > 0);

	ast_mutex_lock(&visdn.usecnt_lock);
	intf->refcnt--;

	if (!intf->refcnt)
		free(intf);

	ast_mutex_unlock(&visdn.usecnt_lock);
}

static struct visdn_intf *_visdn_intf_get_by_name(const char *name)
{
	struct visdn_intf *intf;

	list_for_each_entry(intf, &visdn.intfs_list, node) {
		if (!strcasecmp(intf->name, name))
			return visdn_intf_get(intf);
	}

	return NULL;
}

struct visdn_intf *visdn_intf_get_by_name(const char *name)
{
	struct visdn_intf *intf;

	ast_rwlock_rdlock(&visdn.intfs_list_lock);
	intf = _visdn_intf_get_by_name(name);
	ast_rwlock_unlock(&visdn.intfs_list_lock);

	return intf;
}

struct visdn_ic *visdn_ic_alloc(void)
{
	struct visdn_ic *ic;

	ic = malloc(sizeof(*ic));
	if (!ic)
		return NULL;

	memset(ic, 0, sizeof(*ic));

	ic->refcnt = 1;

	INIT_LIST_HEAD(&ic->clip_numbers_list);
	INIT_LIST_HEAD(&ic->trans_numbers_list);

	return ic;
}

struct visdn_ic *visdn_ic_get(struct visdn_ic *ic)
{
	assert(ic);
	assert(ic->refcnt > 0);

	ast_mutex_lock(&visdn.usecnt_lock);
	ic->refcnt++;
	ast_mutex_unlock(&visdn.usecnt_lock);

	return ic;
}

void visdn_ic_put(struct visdn_ic *ic)
{
	assert(ic);
	assert(ic->refcnt > 0);

	ast_mutex_lock(&visdn.usecnt_lock);
	ic->refcnt--;

	if (!ic->refcnt) {
		if (ic->intf)
			visdn_intf_put(ic->intf);

		free(ic);
	}

	ast_mutex_unlock(&visdn.usecnt_lock);
}

static enum q931_interface_network_role
	visdn_string_to_network_role(const char *str)
{
	if (!strcasecmp(str, "user"))
		return Q931_INTF_NET_USER;
	else if (!strcasecmp(str, "private"))
		return Q931_INTF_NET_PRIVATE;
	else if (!strcasecmp(str, "local"))
		return Q931_INTF_NET_LOCAL;
	else if (!strcasecmp(str, "transit"))
		return Q931_INTF_NET_TRANSIT;
	else if (!strcasecmp(str, "international"))
		return Q931_INTF_NET_INTERNATIONAL;
	else {
		ast_log(LOG_ERROR,
			"Unknown network_role '%s'\n",
			str);

		return Q931_INTF_NET_PRIVATE;
	}
}

static enum visdn_clir_mode
	visdn_string_to_clir_mode(const char *str)
{
	if (!strcasecmp(str, "unrestricted"))
		return VISDN_CLIR_MODE_UNRESTRICTED;
	else if (!strcasecmp(str, "unrestricted_default"))
		return VISDN_CLIR_MODE_UNRESTRICTED_DEFAULT;
	else if (!strcasecmp(str, "restricted_default"))
		return VISDN_CLIR_MODE_RESTRICTED_DEFAULT;
	else if (!strcasecmp(str, "restricted"))
		return VISDN_CLIR_MODE_RESTRICTED;
	else {
		ast_log(LOG_ERROR,
			"Unknown clir_mode '%s'\n",
			str);

		return VISDN_CLIR_MODE_UNRESTRICTED_DEFAULT;
	}
}

static int visdn_ic_from_var(
	struct visdn_ic *ic,
	struct ast_variable *var)
{
	if (!strcasecmp(var->name, "status")) {
		if (!strcasecmp(var->value, "online"))
			ic->initial_status = VISDN_INTF_STATUS_ACTIVE;
		else
			ic->initial_status = VISDN_INTF_STATUS_OUT_OF_SERVICE;
	} else if (!strcasecmp(var->name, "tei")) {
		if (!strcasecmp(var->value, "dynamic"))
			ic->tei = LAPD_DYNAMIC_TEI;
		else
			ic->tei = atoi(var->value);
	} else if (!strcasecmp(var->name, "network_role")) {
		ic->network_role =
			visdn_string_to_network_role(var->value);
	} else if (!strcasecmp(var->name, "outbound_called_ton")) {
		ic->outbound_called_ton =
			visdn_ton_from_string(var->value);
	} else if (!strcasecmp(var->name, "force_outbound_cli")) {
		strncpy(ic->force_outbound_cli, var->value,
			sizeof(ic->force_outbound_cli));
	} else if (!strcasecmp(var->name, "force_outbound_cli_ton")) {
		if (!strlen(var->value) || !strcasecmp(var->value, "no"))
			ic->force_outbound_cli_ton =
				VISDN_TYPE_OF_NUMBER_UNSET;
		else
			ic->force_outbound_cli_ton =
				visdn_ton_from_string(var->value);
	} else if (!strcasecmp(var->name, "cli_rewriting")) {
		ic->cli_rewriting = ast_true(var->value);
	} else if (!strcasecmp(var->name, "national_prefix")) {
		strncpy(ic->national_prefix, var->value,
			sizeof(ic->national_prefix));
	} else if (!strcasecmp(var->name, "international_prefix")) {
		strncpy(ic->international_prefix, var->value,
			sizeof(ic->international_prefix));
	} else if (!strcasecmp(var->name, "network_specific_prefix")) {
		strncpy(ic->network_specific_prefix, var->value,
			sizeof(ic->network_specific_prefix));
	} else if (!strcasecmp(var->name, "subscriber_prefix")) {
		strncpy(ic->subscriber_prefix, var->value,
			sizeof(ic->subscriber_prefix));
	} else if (!strcasecmp(var->name, "abbreviated_prefix")) {
		strncpy(ic->abbreviated_prefix, var->value,
			sizeof(ic->abbreviated_prefix));
	} else if (!strcasecmp(var->name, "tones_option")) {
		ic->tones_option = ast_true(var->value);
	} else if (!strcasecmp(var->name, "context")) {
		strncpy(ic->context, var->value, sizeof(ic->context));
	} else if (!strcasecmp(var->name, "language")) {
		strncpy(ic->language, var->value, sizeof(ic->language));
	} else if (!strcasecmp(var->name, "trans_numbers")) {
		visdn_numbers_list_from_string(
			&ic->trans_numbers_list, var->value);
	} else if (!strcasecmp(var->name, "clip_enabled")) {
		ic->clip_enabled = ast_true(var->value);
	} else if (!strcasecmp(var->name, "clip_override")) {
		ic->clip_override = ast_true(var->value);
	} else if (!strcasecmp(var->name, "clip_default_name")) {
		strncpy(ic->clip_default_name, var->value,
			sizeof(ic->clip_default_name));
	} else if (!strcasecmp(var->name, "clip_default_number")) {
		strncpy(ic->clip_default_number, var->value,
			sizeof(ic->clip_default_number));
	} else if (!strcasecmp(var->name, "clip_numbers")) {
		visdn_numbers_list_from_string(
			&ic->clip_numbers_list, var->value);
	} else if (!strcasecmp(var->name, "clip_special_arrangement")) {
		ic->clip_special_arrangement = ast_true(var->value);
	} else if (!strcasecmp(var->name, "clir_mode")) {
		ic->clir_mode = visdn_string_to_clir_mode(var->value);
	} else if (!strcasecmp(var->name, "overlap_sending")) {
		ic->overlap_sending = ast_true(var->value);
	} else if (!strcasecmp(var->name, "overlap_receiving")) {
		ic->overlap_receiving = ast_true(var->value);
	} else if (!strcasecmp(var->name, "call_bumping")) {
		ic->call_bumping = ast_true(var->value);
	} else if (!strcasecmp(var->name, "autorelease_dlc")) {
		ic->dlc_autorelease_time = atoi(var->value);
	} else if (!strcasecmp(var->name, "echocancel")) {
		ic->echocancel = ast_true(var->value);
	} else if (!strcasecmp(var->name, "echocancel_taps")) {
		ic->echocancel_taps = atoi(var->value);
	} else if (!strcasecmp(var->name, "jitbuf_average")) {
		ic->jitbuf_average = atoi(var->value);
	} else if (!strcasecmp(var->name, "jitbuf_low")) {
		ic->jitbuf_low = atoi(var->value);
	} else if (!strcasecmp(var->name, "jitbuf_hardlow")) {
		ic->jitbuf_hardlow = atoi(var->value);
	} else if (!strcasecmp(var->name, "jitbuf_high")) {
		ic->jitbuf_high = atoi(var->value);
	} else if (!strcasecmp(var->name, "jitbuf_hardhigh")) {
		ic->jitbuf_hardhigh = atoi(var->value);
	} else if (!strcasecmp(var->name, "t301")) {
		ic->T301 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t302")) {
		ic->T302 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t303")) {
		ic->T303 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t304")) {
		ic->T304 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t305")) {
		ic->T305 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t306")) {
		ic->T306 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t307")) {
		ic->T307 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t308")) {
		ic->T308 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t309")) {
		ic->T309 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t310")) {
		ic->T310 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t312")) {
		ic->T312 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t313")) {
		ic->T313 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t314")) {
		ic->T314 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t316")) {
		ic->T316 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t317")) {
		ic->T317 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t318")) {
		ic->T318 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t319")) {
		ic->T319 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t320")) {
		ic->T320 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t321")) {
		ic->T321 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t322")) {
		ic->T322 = atoi(var->value);
	} else {
		return -1;
	}

	return 0;
}

static void visdn_ic_copy(
	struct visdn_ic *dst,
	const struct visdn_ic *src)
{
	dst->initial_status = src->initial_status;
	dst->tei = src->tei;
	dst->network_role = src->network_role;
	dst->outbound_called_ton = src->outbound_called_ton;
	strcpy(dst->force_outbound_cli, src->force_outbound_cli);
	dst->force_outbound_cli_ton = src->force_outbound_cli_ton;
	dst->tones_option = src->tones_option;
	strcpy(dst->context, src->context);
	strcpy(dst->language, src->language);
	dst->clip_enabled = src->clip_enabled;
	dst->clip_override = src->clip_override;
	strcpy(dst->clip_default_name, src->clip_default_name);
	strcpy(dst->clip_default_number, src->clip_default_number);

	visdn_numbers_list_copy(&dst->trans_numbers_list,
			&src->trans_numbers_list);
	visdn_numbers_list_copy(&dst->clip_numbers_list,
			&src->clip_numbers_list);

	dst->clip_special_arrangement = src->clip_special_arrangement;
	dst->clir_mode = src->clir_mode;
	dst->overlap_sending = src->overlap_sending;
	dst->overlap_receiving = src->overlap_receiving;
	dst->call_bumping = src->call_bumping;
	dst->cli_rewriting = src->cli_rewriting;
	strcpy(dst->national_prefix, src->national_prefix);
	strcpy(dst->international_prefix, src->international_prefix);
	strcpy(dst->network_specific_prefix, src->network_specific_prefix);
	strcpy(dst->subscriber_prefix, src->subscriber_prefix);
	strcpy(dst->abbreviated_prefix, src->abbreviated_prefix);
	dst->dlc_autorelease_time = src->dlc_autorelease_time;
	dst->echocancel = src->echocancel;
	dst->echocancel_taps = src->echocancel_taps;

	dst->jitbuf_average = src->jitbuf_average;
	dst->jitbuf_low = src->jitbuf_low;
	dst->jitbuf_hardlow = src->jitbuf_hardlow;
	dst->jitbuf_high = src->jitbuf_high;
	dst->jitbuf_hardhigh = src->jitbuf_hardhigh;

	dst->T301 = src->T301;
	dst->T302 = src->T302;
	dst->T303 = src->T303;
	dst->T304 = src->T304;
	dst->T305 = src->T305;
	dst->T306 = src->T306;
	dst->T307 = src->T307;
	dst->T308 = src->T308;
	dst->T309 = src->T309;
	dst->T310 = src->T310;
	dst->T312 = src->T312;
	dst->T313 = src->T313;
	dst->T314 = src->T314;
	dst->T316 = src->T316;
	dst->T317 = src->T317;
	dst->T318 = src->T318;
	dst->T319 = src->T319;
	dst->T320 = src->T320;
	dst->T321 = src->T321;
	dst->T322 = src->T322;
}

static const char *visdn_intf_status_to_text(enum visdn_intf_status status)
{
	switch(status) {
	case VISDN_INTF_STATUS_UNINITIALIZED:
		return "Uninitialized";
	case VISDN_INTF_STATUS_CONFIGURED:
		return "Configured";
	case VISDN_INTF_STATUS_OUT_OF_SERVICE:
		return "OUT OF SERVICE";
	case VISDN_INTF_STATUS_ACTIVE:
		return "Online";
	case VISDN_INTF_STATUS_FAILED:
		return "FAILED!";
	}

	return "*UNKNOWN*";
}


static void visdn_intf_set_status(
	struct visdn_intf *intf,
	enum visdn_intf_status status,
	longtime_t timeout,
	const char *fmt, ...)
	__attribute__ ((format (printf, 4, 5)));

static void visdn_intf_set_status(
	struct visdn_intf *intf,
	enum visdn_intf_status status,
	longtime_t timeout,
	const char *fmt, ...)
{
	visdn_intf_debug_state(intf,
		"changed state from %s to %s (timer %.2fs)\n",
		visdn_intf_status_to_text(intf->status),
		visdn_intf_status_to_text(status),
		timeout / 1000000.0);

	intf->status = status;
	intf->timer_expiration = longtime_now() + timeout;

	if (fmt) {
		va_list ap;

		if (intf->status_reason)
			free(intf->status_reason);

		va_start(ap, fmt);
		int res = vasprintf(&intf->status_reason, fmt, ap);
		va_end(ap);

		if (res < 0)
			intf->status_reason = NULL;
	}

	if (visdn.q931_thread != AST_PTHREADT_NULL)
		pthread_kill(visdn.q931_thread, SIGURG);
}

int visdn_intf_initialize(struct visdn_intf *intf)
{
	assert(intf->current_ic);
	assert(!intf->q931_intf);

	struct visdn_ic *ic = intf->current_ic;

	if (ic->initial_status == VISDN_INTF_STATUS_OUT_OF_SERVICE) {
		visdn_intf_set_status(intf,
			VISDN_INTF_STATUS_OUT_OF_SERVICE, -1, NULL);
		return 0;
	}

	intf->q931_intf = q931_intf_open(intf->name, 0, ic->tei);
	if (!intf->q931_intf) {
		visdn_intf_set_status(intf,
			VISDN_INTF_STATUS_FAILED,
			FAILED_RETRY_TIME,
			"Cannot open() interface");
		goto err_intf_open;
	}

	intf->q931_intf->pvt = intf;
	intf->q931_intf->network_role = ic->network_role;
	intf->q931_intf->dlc_autorelease_time = ic->dlc_autorelease_time;
	intf->q931_intf->enable_bumping = ic->call_bumping;

	intf->mgmt_fd = socket(PF_LAPD, SOCK_SEQPACKET, LAPD_SAPI_MGMT);
	if (intf->mgmt_fd < 0) {
		visdn_intf_set_status(intf,
			VISDN_INTF_STATUS_FAILED,
			FAILED_RETRY_TIME,
			"Cannot open() management socket: %s",
			strerror(errno));

		goto err_socket;
	}

	if (setsockopt(intf->mgmt_fd, SOL_LAPD, SO_BINDTODEVICE, intf->name,
					strlen(intf->name) + 1) < 0) {

		visdn_intf_set_status(intf,
			VISDN_INTF_STATUS_FAILED,
			FAILED_RETRY_TIME,
			"Cannot bind management socket to %s: %s",
			strerror(errno),
			intf->name);

		goto err_setsockopt;
	}

	int oldflags;
	oldflags = fcntl(intf->mgmt_fd, F_GETFL, 0);
	if (oldflags < 0) {
		visdn_intf_set_status(intf,
			VISDN_INTF_STATUS_FAILED,
			FAILED_RETRY_TIME,
			"fcntl(GETFL): %s",
			strerror(errno));
		goto err_fcntl_getfl;
	}

	if (fcntl(intf->mgmt_fd, F_SETFL, oldflags | O_NONBLOCK) < 0) {
		visdn_intf_set_status(intf,
			VISDN_INTF_STATUS_FAILED,
			FAILED_RETRY_TIME,
			"fcntl(SETFL): %s",
			strerror(errno));

		goto err_fcntl_setfl;
	}

	if (ic->T301) intf->q931_intf->T301 = ic->T301 * 1000000LL;
	if (ic->T302) intf->q931_intf->T302 = ic->T302 * 1000000LL;
	if (ic->T303) intf->q931_intf->T303 = ic->T303 * 1000000LL;
	if (ic->T304) intf->q931_intf->T304 = ic->T304 * 1000000LL;
	if (ic->T305) intf->q931_intf->T305 = ic->T305 * 1000000LL;
	if (ic->T306) intf->q931_intf->T306 = ic->T306 * 1000000LL;
	if (ic->T308) intf->q931_intf->T308 = ic->T308 * 1000000LL;
	if (ic->T309) intf->q931_intf->T309 = ic->T309 * 1000000LL;
	if (ic->T310) intf->q931_intf->T310 = ic->T310 * 1000000LL;
	if (ic->T312) intf->q931_intf->T312 = ic->T312 * 1000000LL;
	if (ic->T313) intf->q931_intf->T313 = ic->T313 * 1000000LL;
	if (ic->T314) intf->q931_intf->T314 = ic->T314 * 1000000LL;
	if (ic->T316) intf->q931_intf->T316 = ic->T316 * 1000000LL;
	if (ic->T317) intf->q931_intf->T317 = ic->T317 * 1000000LL;
	if (ic->T318) intf->q931_intf->T318 = ic->T318 * 1000000LL;
	if (ic->T319) intf->q931_intf->T319 = ic->T319 * 1000000LL;
	if (ic->T320) intf->q931_intf->T320 = ic->T320 * 1000000LL;
	if (ic->T321) intf->q931_intf->T321 = ic->T321 * 1000000LL;
	if (ic->T322) intf->q931_intf->T322 = ic->T322 * 1000000LL;

/*
	char goodpath[PATH_MAX];
	struct stat st;
	for(;;) {
		if (stat(path, &st) < 0) {
			if (errno == ENOENT)
				break;

			ast_log(LOG_ERROR,
				"cannot stat(%s): %s\n",
				path,
				strerror(errno));

			goto err_stat;
		}

		strcpy(goodpath, path);

		strncat(path, "connected/other_leg/", sizeof(path));
	}

	strncat(goodpath, "../..", sizeof(goodpath));*/

	char path[PATH_MAX];
	snprintf(path, sizeof(path),
		"/sys/class/net/%s/visdn_connected_port/",
		intf->name);

	char *res = realpath(path, intf->remote_port);
	if (!res) {
		visdn_intf_set_status(intf,
			VISDN_INTF_STATUS_FAILED,
			FAILED_RETRY_TIME,
			"cannot find realpath(%s): %s",
			path,
			strerror(errno));

		goto err_realpath;
	}

	if (intf->q931_intf->role == LAPD_INTF_ROLE_NT) {
		if (list_empty(&ic->clip_numbers_list)) {
			ast_log(LOG_NOTICE,
				"Interface '%s' is configured in network"
				" mode but clip_numbers is empty\n",
				intf->name);
		} else if (!strlen(ic->clip_default_number)) {
			ast_log(LOG_NOTICE,
				"Interface '%s' is configured in network"
				" mode but clip_default_number is empty\n",
				intf->name);
		} else if (!visdn_numbers_list_match(&ic->clip_numbers_list,
					ic->clip_default_number)) {

			ast_log(LOG_NOTICE,
				"Interface '%s' clip_numbers should contain "
				"clip_default_number (%s)\n",
				intf->name,
				ic->clip_default_number);
		}
	}

	visdn_intf_set_status(intf, VISDN_INTF_STATUS_ACTIVE, -1, NULL);

	refresh_polls_list();

	return 0;

err_realpath:
err_fcntl_setfl:
err_fcntl_getfl:
err_setsockopt:
	close(intf->mgmt_fd);
err_socket:
	q931_intf_close(intf->q931_intf);
err_intf_open:

	return -1;
}

void visdn_intf_close(struct visdn_intf *intf)
{
	close(intf->mgmt_fd);

	q931_intf_close(intf->q931_intf);
	intf->q931_intf = NULL;
}

static void visdn_intf_timer(struct visdn_intf *intf)
{
	switch(intf->status) {
	case VISDN_INTF_STATUS_UNINITIALIZED:
	break;

	case VISDN_INTF_STATUS_CONFIGURED:
	case VISDN_INTF_STATUS_FAILED:
		visdn_intf_initialize(intf);
	break;

	case VISDN_INTF_STATUS_OUT_OF_SERVICE:
	break;

	case VISDN_INTF_STATUS_ACTIVE:
	break;
	}
}

longtime_t visdn_intf_run_timers(longtime_t timeout)
{
	struct visdn_intf *intf;
	ast_rwlock_rdlock(&visdn.intfs_list_lock);
	list_for_each_entry(intf, &visdn.intfs_list, node) {

		if (intf->timer_expiration != -1) {
			if (intf->timer_expiration < longtime_now()) {
				intf->timer_expiration = -1;
				visdn_intf_timer(intf);
			} else {
				timeout = intf->timer_expiration -
							longtime_now();
			}
		}
	}
	ast_rwlock_unlock(&visdn.intfs_list_lock);

	return timeout;
}

void visdn_ic_setdefault(struct visdn_ic *ic)
{
	ic->tei = LAPD_DYNAMIC_TEI;
	ic->network_role = Q931_INTF_NET_PRIVATE;
	ic->outbound_called_ton = VISDN_TYPE_OF_NUMBER_UNKNOWN;

	strcpy(ic->force_outbound_cli, "");

	ic->force_outbound_cli_ton = VISDN_TYPE_OF_NUMBER_UNSET;
	ic->tones_option = TRUE;

	strcpy(ic->context, "visdn");
	strcpy(ic->language, "");

	ic->clip_enabled = TRUE;
	ic->clip_override = FALSE;

	strcpy(ic->clip_default_name, "");
	strcpy(ic->clip_default_number, "");

	ic->clip_special_arrangement = FALSE;
	ic->clir_mode = VISDN_CLIR_MODE_UNRESTRICTED_DEFAULT;
	ic->overlap_sending = TRUE;
	ic->overlap_receiving = FALSE;
	ic->call_bumping = FALSE;
	ic->cli_rewriting = FALSE;

	strcpy(ic->national_prefix, "");
	strcpy(ic->international_prefix, "");
	strcpy(ic->network_specific_prefix, "");
	strcpy(ic->subscriber_prefix, "");
	strcpy(ic->abbreviated_prefix, "");

	ic->dlc_autorelease_time = 10;

	ic->echocancel = FALSE;
	ic->echocancel_taps = 256;

	ic->jitbuf_average = 5;
	ic->jitbuf_low = 10;
	ic->jitbuf_hardlow = 0;
	ic->jitbuf_high = 50;
	ic->jitbuf_hardhigh = 1024;

	ic->T307 = 180;
}

static void visdn_intf_reconfigure(
	struct ast_config *cfg,
	const char *name)
{
	/* Allocate a new interface */

	struct visdn_ic *ic;
	ic = visdn_ic_alloc();
	if (!ic)
		return;

	visdn_ic_copy(ic, visdn.default_ic);

	/* Now read the configuration from file */
	struct ast_variable *var;
	var = ast_variable_browse(cfg, (char *)name);
	while (var) {
		if (visdn_ic_from_var(ic, var) < 0) {
			ast_log(LOG_WARNING,
				"Unknown configuration "
				"variable %s\n",
				var->name);
		}

		var = var->next;
	}

	ast_rwlock_wrlock(&visdn.intfs_list_lock);

	struct visdn_intf *intf = _visdn_intf_get_by_name(name);
	if (!intf) {
		intf = visdn_intf_alloc();
		if (!intf) {
			ast_rwlock_unlock(&visdn.intfs_list_lock);
			return;
		}

		strncpy(intf->name, name, sizeof(intf->name));

		list_add_tail(&intf->node, &visdn.intfs_list);
	}

	if (intf->current_ic)
		visdn_ic_put(intf->current_ic);

	ic->intf = intf;

	intf->current_ic = visdn_ic_get(ic);

	if (intf->status == VISDN_INTF_STATUS_UNINITIALIZED)
		visdn_intf_set_status(intf,
			VISDN_INTF_STATUS_CONFIGURED,
			WAITING_INITIALIZATION_DELAY, NULL);

	ast_rwlock_unlock(&visdn.intfs_list_lock);
}

void visdn_intf_reload(struct ast_config *cfg)
{
	/* Read default interface configuration */
	struct ast_variable *var;
	var = ast_variable_browse(cfg, "global");
	while (var) {
		if (visdn_ic_from_var(visdn.default_ic, var) < 0) {
			ast_log(LOG_WARNING,
				"Unknown configuration variable %s\n",
				var->name);
		}

		var = var->next;
	}

	const char *cat;
	for (cat = ast_category_browse(cfg, NULL); cat;
	     cat = ast_category_browse(cfg, (char *)cat)) {

		if (!strcasecmp(cat, "general") ||
		    !strcasecmp(cat, "global") ||
		    !strncasecmp(cat, VISDN_HUNTGROUP_PREFIX,
				strlen(VISDN_HUNTGROUP_PREFIX)))
			continue;

		visdn_intf_reconfigure(cfg, cat);
	}
}

/*---------------------------------------------------------------------------*/

static const char *visdn_intf_mode_to_string(
	enum lapd_intf_mode value)
{
	switch(value) {
	case LAPD_INTF_MODE_POINT_TO_POINT:
		return "Point-to-point";
	case LAPD_INTF_MODE_MULTIPOINT:
		return "Point-to-Multipoint";
	}

	return "*UNKNOWN*";
}

static const char *visdn_intf_mode_to_string_short(
	enum lapd_intf_mode value)
{
	switch(value) {
	case LAPD_INTF_MODE_POINT_TO_POINT:
		return "P2P";
	case LAPD_INTF_MODE_MULTIPOINT:
		return "P2MP";
	}

	return "*UNKNOWN*";
}

static const char *visdn_ic_network_role_to_string(
	enum q931_interface_network_role value)
{
	switch(value) {
	case Q931_INTF_NET_USER:
		return "User";
	case Q931_INTF_NET_PRIVATE:
		return "Private Network";
	case Q931_INTF_NET_LOCAL:
		return "Local Network";
	case Q931_INTF_NET_TRANSIT:
		return "Transit Network";
	case Q931_INTF_NET_INTERNATIONAL:
		return "International Network";
	}

	return "*UNKNOWN*";
}

static const char *visdn_clir_mode_to_text(
	enum visdn_clir_mode mode)
{
	switch(mode) {
	case VISDN_CLIR_MODE_UNRESTRICTED:
		return "Unrestricted";
	case VISDN_CLIR_MODE_UNRESTRICTED_DEFAULT:
		return "Unrestricted by default";
	case VISDN_CLIR_MODE_RESTRICTED_DEFAULT:
		return "Restricted by default";
	case VISDN_CLIR_MODE_RESTRICTED:
		return "Restricted";
	}

	return "*UNKNOWN*";
}

static void visdn_print_intf_details(int fd, struct visdn_intf *intf)
{
	struct visdn_ic *ic = intf->current_ic;

	ast_cli(fd, "\n-- %s --\n", intf->name);
		ast_cli(fd, "Status                    : %s\n",
			visdn_intf_status_to_text(intf->status));

	if (intf->q931_intf) {
		ast_cli(fd, "Role                      : %s\n",
			intf->q931_intf->role == LAPD_INTF_ROLE_NT ?
				"NT" : "TE");

		ast_cli(fd,
			"Mode                      : %s\n",
			visdn_intf_mode_to_string(
				intf->q931_intf->mode));

		if (intf->q931_intf->tei != LAPD_DYNAMIC_TEI)
			ast_cli(fd, "TEI                       : %d\n",
				intf->q931_intf->tei);
		else
			ast_cli(fd, "TEI                       : "
					"Dynamic\n");
	}

	ast_cli(fd,
		"Network role              : %s\n"
		"Context                   : %s\n"
		"Language                  : %s\n",
		visdn_ic_network_role_to_string(
			ic->network_role),
		ic->context,
		ic->language);

	ast_cli(fd, "Transparent Numbers       : ");
	{
	struct visdn_number *num;
	list_for_each_entry(num, &ic->clip_numbers_list, node) {
		ast_cli(fd, "%s ", num->number);
	}
	}
	ast_cli(fd, "\n");

	ast_cli(fd,
		"Echo canceller            : %s\n"
		"Echo canceller taps       : %d (%d ms)\n"
		"Jitter buffer average     : %d\n"
		"Jitter buffer low-mark    : %d\n"
		"Jitter buffer hard low-mark: %d\n"
		"Jitter buffer high-mark   : %d\n"
		"Jitter buffer hard high-mark: %d\n"
		"Tones option              : %s\n"
		"Overlap Sending           : %s\n"
		"Overlap Receiving         : %s\n"
		"Call bumping              : %s\n",
		ic->echocancel ? "Yes" : "No",
		ic->echocancel_taps, ic->echocancel_taps / 8,
		ic->jitbuf_average,
		ic->jitbuf_low,
		ic->jitbuf_hardlow,
		ic->jitbuf_high,
		ic->jitbuf_hardhigh,
		ic->tones_option ? "Yes" : "No",
		ic->overlap_sending ? "Yes" : "No",
		ic->overlap_receiving ? "Yes" : "No",
		ic->call_bumping ? "Yes" : "No");

	ast_cli(fd,
		"National prefix           : %s\n"
		"International prefix      : %s\n"
		"Newtork specific prefix   : %s\n"
		"Subscriber prefix         : %s\n"
		"Abbreviated prefix        : %s\n"
		"Autorelease time          : %d\n\n",
		ic->national_prefix,
		ic->international_prefix,
		ic->network_specific_prefix,
		ic->subscriber_prefix,
		ic->abbreviated_prefix,
		ic->dlc_autorelease_time);

	ast_cli(fd,
		"Outbound type of number   : %s\n\n"
		"Forced CLI                : %s\n"
		"Forced CLI type of number : %s\n"
		"CLI rewriting             : %s\n"
		"CLIR mode                 : %s\n"
		"CLIP enabled              : %s\n"
		"CLIP override             : %s\n"
		"CLIP default              : %s <%s>\n"
		"CLIP special arrangement  : %s\n",
		visdn_ton_to_string(
			ic->outbound_called_ton),
		ic->force_outbound_cli,
		visdn_ton_to_string(
			ic->force_outbound_cli_ton),
		ic->cli_rewriting ? "Yes" : "No",
		visdn_clir_mode_to_text(ic->clir_mode),
		ic->clip_enabled ? "Yes" : "No",
		ic->clip_override ? "Yes" : "No",
		ic->clip_default_name,
		ic->clip_default_number,
		ic->clip_special_arrangement ? "Yes" : "No");

	ast_cli(fd, "CLIP Numbers              : ");
	{
	struct visdn_number *num;
	list_for_each_entry(num, &ic->clip_numbers_list, node) {
		ast_cli(fd, "%s ", num->number);
	}
	}
	ast_cli(fd, "\n\n");

	if (intf->q931_intf) {
		if (intf->q931_intf->role == LAPD_INTF_ROLE_NT) {
			ast_cli(fd, "DLCs                      : ");

			struct q931_dlc *dlc;
			list_for_each_entry(dlc, &intf->q931_intf->dlcs,
					intf_node) {
				ast_cli(fd, "%d ", dlc->tei);

			}

			ast_cli(fd, "\n\n");

#define TIMER_CONFIG(timer)					\
	ast_cli(fd, #timer ": %3lld%-5s",			\
		intf->q931_intf->timer / 1000000LL,		\
		ic->timer ? " (*)" : "");

#define TIMER_CONFIG_LN(timer)					\
	ast_cli(fd, #timer ": %3lld%-5s\n",			\
		intf->q931_intf->timer / 1000000LL,		\
		ic->timer ? " (*)" : "");

			TIMER_CONFIG(T301);
			TIMER_CONFIG(T301);
			TIMER_CONFIG(T302);
			TIMER_CONFIG_LN(T303);
			TIMER_CONFIG(T304);
			TIMER_CONFIG(T305);
			TIMER_CONFIG(T306);
			ast_cli(fd, "T307: %d\n", ic->T307);
			TIMER_CONFIG(T308);
			TIMER_CONFIG(T309);
			TIMER_CONFIG(T310);
			TIMER_CONFIG_LN(T312);
			TIMER_CONFIG(T314);
			TIMER_CONFIG(T316);
			TIMER_CONFIG(T317);
			TIMER_CONFIG_LN(T320);
			TIMER_CONFIG(T321);
			TIMER_CONFIG_LN(T322);
		} else {
			TIMER_CONFIG(T301);
			TIMER_CONFIG(T302);
			TIMER_CONFIG(T303);
			TIMER_CONFIG_LN(T304);
			TIMER_CONFIG(T305);
			TIMER_CONFIG(T306);
			ast_cli(fd, "T307: %d\n", ic->T307);
			TIMER_CONFIG_LN(T308);
			TIMER_CONFIG(T309);
			TIMER_CONFIG(T310);
			TIMER_CONFIG(T312);
			TIMER_CONFIG_LN(T313);
			TIMER_CONFIG(T314);
			TIMER_CONFIG(T316);
			TIMER_CONFIG(T317);
			TIMER_CONFIG(T318);
			TIMER_CONFIG_LN(T319);
			TIMER_CONFIG(T320);
			TIMER_CONFIG(T321);
			TIMER_CONFIG_LN(T322);
		}

		ast_cli(fd, "\nChannels:\n");
		int i;
		for (i=0; i<intf->q931_intf->n_channels; i++) {
			ast_cli(fd, "  B%d: %s",
				intf->q931_intf->channels[i].id + 1,
				q931_channel_state_to_text(
					intf->q931_intf->channels[i].state));

			if (intf->q931_intf->channels[i].call) {
				struct q931_call *call =
					intf->q931_intf->channels[i].call;

				ast_cli(fd, "  Call: %5d.%c %s",
					call->call_reference,
					(call->direction ==
						Q931_CALL_DIRECTION_INBOUND)
							? 'I' : 'O',
					q931_call_state_to_text(call->state));
			}

			ast_cli(fd, "\n");
		}
	}

	ast_cli(fd, "\nParked calls:\n");
	struct visdn_suspended_call *suspended_call;
	list_for_each_entry(suspended_call, &intf->suspended_calls,
		       					node) {

		char sane_str[10];
		char hex_str[20];
		int i;
		for(i=0;
		    i<sizeof(sane_str) &&
			i<suspended_call->call_identity_len;
		    i++) {
			sane_str[i] =
			isprint(suspended_call->call_identity[i]) ?
				suspended_call->call_identity[i] : '.';

			snprintf(hex_str + (i*2),
				sizeof(hex_str)-(i*2),
				"%02x ",
				suspended_call->call_identity[i]);
		}
		sane_str[i] = '\0';
		hex_str[i*2] = '\0';

		ast_cli(fd, "    %s (%s)\n",
			sane_str,
			hex_str);
	}

}

char *visdn_intf_completion(
#if ASTERISK_VERSION_NUM < 010400 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10400)
	char *line, char *word,
#else
	const char *line, const char *word,
#endif
	int state)
{
	int which = 0;

	struct visdn_intf *intf;
	ast_rwlock_rdlock(&visdn.intfs_list_lock);
	list_for_each_entry(intf, &visdn.intfs_list, node) {
		if (!strncasecmp(word, intf->name, strlen(word))) {
			if (++which > state) {
				ast_rwlock_unlock(&visdn.intfs_list_lock);
				return strdup(intf->name);
			}
		}
	}
	ast_rwlock_unlock(&visdn.intfs_list_lock);

	return NULL;
}

static char *complete_visdn_interface_show(
#if ASTERISK_VERSION_NUM < 010400 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10400)
	char *line, char *word,
#else
	const char *line, const char *word,
#endif
	int pos, int state)
{
	if (pos != 3)
		return NULL;

	return visdn_intf_completion(line, word, state);
}

static void visdn_show_interface(int fd, struct visdn_intf *intf)
{
	ast_mutex_lock(&intf->lock);

	if (intf->q931_intf) {
		char tei[6] = "";
		int ncalls = 0;

		if (intf->q931_intf->tei != LAPD_DYNAMIC_TEI)
			snprintf(tei, sizeof(tei), "%d", intf->q931_intf->tei);
		else
			strcpy(tei, "DYN");

		struct q931_call *call;
		list_for_each_entry(call, &intf->q931_intf->calls, calls_node)
			ncalls++;

		ast_cli(fd, "%-10s %-14s %-4s %-4s %-3s %d\n",
			intf->name,
			visdn_intf_status_to_text(intf->status),
			(intf->q931_intf->role == LAPD_INTF_ROLE_NT) ?
				"NT" : "TE",
			visdn_intf_mode_to_string_short(intf->q931_intf->mode),
			tei,
			ncalls);
	} else {
		ast_cli(fd, "%-10s %-14s\n",
			intf->name,
			visdn_intf_status_to_text(intf->status));
	}

	ast_mutex_unlock(&intf->lock);
}

#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
static int do_visdn_interface_show(int fd, int argc, char *argv[])
#else
static char *do_visdn_interface_show(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#endif
{
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)

#else
	switch (cmd) {
	case CLI_INIT:
		e->command = "visdn interface show";
		e->usage =   "Usage: visdn interface show [<interface>]\n"
			     "\n"
			     "	Displays informations on vISDN's interfaces. If no interface name is\n"
			     "	specified, shows a summary of all the interfaces.\n";
		return NULL;
	case CLI_GENERATE:
		return complete_visdn_interface_show(a->line, a->word, a->pos, a->n);
	}
#endif
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
	if (argc == 3) {
		ast_cli(fd, "Interface  Status         Role Mode TEI Calls\n");

		struct visdn_intf *intf;
		ast_rwlock_rdlock(&visdn.intfs_list_lock);
		list_for_each_entry(intf, &visdn.intfs_list, node)
			visdn_show_interface(fd, intf);
		ast_rwlock_unlock(&visdn.intfs_list_lock);

	} else if (argc == 4) {
		struct visdn_intf *intf;
		ast_rwlock_rdlock(&visdn.intfs_list_lock);
		list_for_each_entry(intf, &visdn.intfs_list, node) {
			if (!strcasecmp(argv[3], intf->name)) {
				visdn_print_intf_details(fd, intf);
				break;
			}
		}
		ast_rwlock_unlock(&visdn.intfs_list_lock);
	} else
		return RESULT_SHOWUSAGE;

	return RESULT_SUCCESS;

#else
	if (a->argc == 3) {
		ast_cli(a->fd, "Interface  Status         Role Mode TEI Calls\n");

		struct visdn_intf *intf;
		ast_rwlock_rdlock(&visdn.intfs_list_lock);
		list_for_each_entry(intf, &visdn.intfs_list, node)
			visdn_show_interface(a->fd, intf);
		ast_rwlock_unlock(&visdn.intfs_list_lock);

	} else if (a->argc == 4) {
		struct visdn_intf *intf;
		ast_rwlock_rdlock(&visdn.intfs_list_lock);
		list_for_each_entry(intf, &visdn.intfs_list, node) {
			if (!strcasecmp(a->argv[3], intf->name)) {
				visdn_print_intf_details(a->fd, intf);
				break;
			}
		}
		ast_rwlock_unlock(&visdn.intfs_list_lock);
	} else
		return CLI_SHOWUSAGE;

	return CLI_SUCCESS;
#endif
}
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
static char visdn_interface_show_help[] =
"Usage: visdn interface show [<interface>]\n"
"\n"
"	Displays informations on vISDN's interfaces. If no interface name is\n"
"	specified, shows a summary of all the interfaces.\n";

static struct ast_cli_entry visdn_interface_show =
{
	{ "visdn", "interface", "show", NULL },
	do_visdn_interface_show,
	"Displays vISDN's interface information",
	visdn_interface_show_help,
	complete_visdn_interface_show,
};
#endif
/*---------------------------------------------------------------------------*/

#ifdef DEBUG_CODE
static int visdn_intf_cli_debug_state(
	int fd, struct visdn_intf *intf, BOOL enable)
{
	if (intf) {
		intf->debug_state = enable;
		intf->debug_generic = enable;
	} else {
		ast_rwlock_rdlock(&visdn.intfs_list_lock);
		struct visdn_intf *intf;
		list_for_each_entry(intf, &visdn.intfs_list, node) {
			intf->debug_state = enable;
			intf->debug_generic = enable;
		}
		ast_rwlock_unlock(&visdn.intfs_list_lock);
	}

	return RESULT_SUCCESS;
}

static int visdn_intf_cli_debug_jitbuf(
	int fd, struct visdn_intf *intf, BOOL enable)
{
	if (intf) {
		intf->debug_jitbuf = enable;
	} else {
		ast_rwlock_rdlock(&visdn.intfs_list_lock);
		struct visdn_intf *intf;
		list_for_each_entry(intf, &visdn.intfs_list, node)
			intf->debug_jitbuf = enable;
		ast_rwlock_unlock(&visdn.intfs_list_lock);
	}

	return RESULT_SUCCESS;
}

static int visdn_intf_cli_debug_frames(
	int fd, struct visdn_intf *intf, BOOL enable)
{
	if (intf) {
		intf->debug_frames = enable;
	} else {
		ast_rwlock_rdlock(&visdn.intfs_list_lock);
		struct visdn_intf *intf;
		list_for_each_entry(intf, &visdn.intfs_list, node)
			intf->debug_frames = enable;
		ast_rwlock_unlock(&visdn.intfs_list_lock);
	}

	return RESULT_SUCCESS;
}

static int visdn_intf_cli_debug_all(int fd, BOOL enable)
{
	ast_rwlock_rdlock(&visdn.intfs_list_lock);
	struct visdn_intf *intf;
	list_for_each_entry(intf, &visdn.intfs_list, node) {
		intf->debug_generic = enable;
		intf->debug_state = enable;
		intf->debug_jitbuf = enable;
	}
	ast_rwlock_unlock(&visdn.intfs_list_lock);

	return RESULT_SUCCESS;
}

#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
static int visdn_intf_cli_debug(int fd, int argc, char *argv[],
				int args, BOOL enable)
#else
static char *visdn_intf_cli_debug(int fd, int argc, char *argv[],
				int args, BOOL enable)
#endif
{
	int err = 0;

	if (argc < args)
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
		return RESULT_SHOWUSAGE;

#else
		return CLI_SHOWUSAGE;
#endif
	struct visdn_intf *intf = NULL;

	if (argc < args + 1) {
		err = visdn_intf_cli_debug_all(fd, enable);
	} else {
		if (argc > args + 1) {
			intf = visdn_intf_get_by_name(argv[args + 1]);
			if (!intf) {
				ast_cli(fd, "Cannot find interface '%s'\n",
					argv[args]);
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
				return RESULT_FAILURE;
#else
				return CLI_SHOWUSAGE;
#endif
			}
		}

		if (!strcasecmp(argv[args], "state"))
			err = visdn_intf_cli_debug_state(fd, intf, enable);
		else if (!strcasecmp(argv[args], "jitbuf"))
			err = visdn_intf_cli_debug_jitbuf(fd, intf, enable);
		else if (!strcasecmp(argv[args], "frames"))
			err = visdn_intf_cli_debug_frames(fd, intf, enable);
		else {
			ast_cli(fd, "Unrecognized category '%s'\n",
					argv[args]);
			err = RESULT_SHOWUSAGE;
		}

		if (intf)
			visdn_intf_put(intf);
	}

	if (err)
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
		return err;

#else
		return CLI_SHOWUSAGE;
#endif
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
	return RESULT_SUCCESS;
#else
	return CLI_SUCCESS;
#endif
}


static char *visdn_intf_debug_category_complete(
#if ASTERISK_VERSION_NUM < 010400 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10400)
	char *line, char *word,
#else
	const char *line, const char *word,
#endif
	int state)
{
	char *commands[] = { "state", "jitbuf", "frames" };
	int i;

	for(i=state; i<ARRAY_SIZE(commands); i++) {
		if (!strncasecmp(word, commands[i], strlen(word)))
			return strdup(commands[i]);
	}

	return NULL;
}

static char *visdn_intf_debug_complete(
#if ASTERISK_VERSION_NUM < 010400 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10400)
	char *line, char *word,
#else
	const char *line, const char *word,
#endif
	int pos, int state)
{

	switch(pos) {
	case 3:
		return visdn_intf_debug_category_complete(line, word, state);
	case 4:
		return visdn_intf_completion(line, word, state);
	}

	return NULL;
}

static char *visdn_intf_no_debug_complete(
#if ASTERISK_VERSION_NUM < 010400 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10400)
	char *line, char *word,
#else
	const char *line, const char *word,
#endif
	int pos, int state)
{

	switch(pos) {
	case 4:
		return visdn_intf_debug_category_complete(line, word, state);
	case 5:
		return visdn_intf_completion(line, word, state);
	}

	return NULL;
}

#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
static int visdn_intf_debug_func(int fd, int argc, char *argv[])
#else
static char *visdn_intf_debug_func(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#endif
{
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)

#else
	switch (cmd) {
	case CLI_INIT:
		e->command = "visdn interface debug";
		e->usage =   "Usage: visdn interface debug [<state | jitbuf | frames> [interface]]\n"
			     "\n"
			     "	Debug vISDN's interface related events\n"
			     "\n"
			     "	state		Interface state transitions\n"
			     "	jitbuf		Audio jitter buffer\n"
			     "	frames		Audio frames\n";
		return NULL;
	case CLI_GENERATE:
		return visdn_intf_debug_complete(a->line, a->word, a->pos, a->n);
	}
#endif
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
	return visdn_intf_cli_debug(fd, argc, argv, 3, TRUE);
#else
	return visdn_intf_cli_debug(a->fd, a->argc,a->argv, 3, TRUE);
#endif
}

#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
static int visdn_intf_no_debug_func(int fd, int argc, char *argv[])
#else
static char *visdn_intf_no_debug_func(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#endif
{
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)

#else
	switch (cmd) {
	case CLI_INIT:
		e->command = "visdn interface no debug";
		e->usage =   "Usage: Disable interface debugging\n";
		return NULL;
	case CLI_GENERATE:
		return visdn_intf_no_debug_complete(a->line, a->word, a->pos, a->n);
	}
#endif

#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
	return visdn_intf_cli_debug(fd, argc, argv, 4, FALSE);
#else
	return visdn_intf_cli_debug(a->fd, a->argc, a->argv, 4, FALSE);
#endif
}
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10600)
static char visdn_intf_debug_help[] =
"Usage: visdn interface [<state | jitbuf | frames> [interface]]\n"
"\n"
"	Debug vISDN's interface related events\n"
"\n"
"	state		Interface state transitions\n"
"	jitbuf		Audio jitter buffer\n"
"	frames		Audio frames\n";

static struct ast_cli_entry visdn_intf_debug =
{
	{ "visdn", "interface", "debug", NULL },
	visdn_intf_debug_func,
	"Enable interface debugging",
	visdn_intf_debug_help,
	visdn_intf_debug_complete
};

static struct ast_cli_entry visdn_intf_no_debug =
{
	{ "visdn", "interface", "no", "debug", NULL },
	visdn_intf_no_debug_func,
	"Disable interface debugging",
	NULL,
	visdn_intf_no_debug_complete
};
#else
static struct ast_cli_entry cli_ksdeb[] = {
	AST_CLI_DEFINE(visdn_intf_debug_func, "Enable interface debugging"),
	AST_CLI_DEFINE(visdn_intf_no_debug_func, "Disable interface debugging")
	};
static struct ast_cli_entry cli_ksnodeb[] = {
	AST_CLI_DEFINE(do_visdn_interface_show, "visdn interface show")
	};
#endif

#endif
/*---------------------------------------------------------------------------*/
void visdn_intf_cli_register(void)
{
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10600)
	ast_cli_register(&visdn_interface_show);
#else
	ast_cli_register_multiple(cli_ksnodeb, ARRAY_LEN(cli_ksnodeb));
#endif

#ifdef DEBUG_CODE
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10600)
	ast_cli_register(&visdn_intf_debug);
	ast_cli_register(&visdn_intf_no_debug);
#else
	ast_cli_register_multiple(cli_ksdeb, ARRAY_LEN(cli_ksdeb));
#endif
#endif
}

void visdn_intf_cli_unregister(void)
{
#ifdef DEBUG_CODE
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10600)
	ast_cli_unregister(&visdn_intf_no_debug);
	ast_cli_unregister(&visdn_intf_debug);
#else
	ast_cli_unregister_multiple(cli_ksdeb, ARRAY_LEN(cli_ksdeb));
#endif

#endif
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >= 10200 && ASTERISK_VERSION_NUM < 10600)
	ast_cli_unregister(&visdn_interface_show);
#else
	ast_cli_unregister_multiple(cli_ksnodeb, ARRAY_LEN(cli_ksnodeb));
#endif
}
