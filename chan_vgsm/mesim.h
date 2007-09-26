/*
 * vGSM channel driver for Asterisk
 *
 * Copyright (C) 2007 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU General Public License.
 *
 */

#ifndef _VGSM_MESIM_H
#define _VGSM_MESIM_H

#include <linux/serial.h>

#include <asterisk/lock.h>

#include <list.h>

#include "timer.h"
#include "util.h"

#ifdef DEBUG_CODE
#define vgsm_mesim_debug(mesim, format, arg...)	\
	if ((mesim)->debug)				\
		ast_verbose("vgsm: mesim %s: "		\
			format,				\
			(mesim)->name,			\
			## arg)
#else
#define vgsm_mesim_debug(mesim, format, arg...)	\
	do {} while(0);
#endif

enum vgsm_mesim_state
{
	VGSM_MESIM_CLOSED,
	VGSM_MESIM_CLOSING,
	VGSM_MESIM_ME_POWERING_ON,
	VGSM_MESIM_UNCONFIGURED,
	VGSM_MESIM_SETTING_MODE,
	VGSM_MESIM_DIRECTLY_ROUTED,
	VGSM_MESIM_HOLDER_REMOVED,
	VGSM_MESIM_SIM_MISSING,
	VGSM_MESIM_READY,
	VGSM_MESIM_RESET,
};

enum vgsm_mesim_message_type
{
	VGSM_MESIM_MSG_CLOSE,
	VGSM_MESIM_MSG_REFRESH,
	VGSM_MESIM_MSG_RESET_ASSERTED,
	VGSM_MESIM_MSG_RESET_REMOVED,
	VGSM_MESIM_MSG_SET_MODE,
	VGSM_MESIM_MSG_ME_POWERING_ON,
	VGSM_MESIM_MSG_ME_POWERED_ON,
};

struct vgsm_mesim_message
{
	enum vgsm_mesim_message_type type;

	int len;
	__u8 data[];
};

enum vgsm_mesim_proto
{
        VGSM_MESIM_PROTO_LOCAL,
        VGSM_MESIM_PROTO_CLIENT,
        VGSM_MESIM_PROTO_IMPLEMENTA,
};

const char *vgsm_mesim_message_type_to_text(
	enum vgsm_mesim_message_type mt);

struct vgsm_mesim_set_mode
{
	enum vgsm_mesim_proto proto;

	union
	{
		struct {
		char device_filename[256];
		} local;

		struct {
		struct sockaddr bind_addr;
		} clnt;

		struct {
		struct sockaddr simclient_addr;
		} impl;
	};
};

enum vgsm_mesim_impl_state
{
	VGSM_MESIM_IMPL_STATE_NULL,
	VGSM_MESIM_IMPL_STATE_TRYING,
	VGSM_MESIM_IMPL_STATE_CONNECTED,
};

struct vgsm_module;

struct vgsm_mesim
{
	int fd;

	const char *name;

	enum vgsm_mesim_state state;
	enum vgsm_mesim_state prev_state;
	ast_mutex_t state_lock;
	ast_cond_t state_cond;

	struct vgsm_module *module;

	struct vgsm_timerset timerset;
	struct vgsm_timer timer;

	BOOL enabled;

	int cmd_pipe_read;
	int cmd_pipe_write;

	pthread_t comm_thread;
	pthread_t modem_thread;
	BOOL modem_thread_has_to_exit;

	BOOL vcc;
	BOOL rst;

	struct serial_icounter_struct icount;

	BOOL debug;

	/* External interface */
        enum vgsm_mesim_proto proto;

	/* protocol=local */
	char local_device_filename[PATH_MAX];
	int local_fd;
	int local_sim_id;

	/* protocol=client */
	int clnt_listen_fd;
	int clnt_sock_fd;
	struct sockaddr_in clnt_bind_addr;

	/* protocol=implementa */
	int impl_sock_fd;
	struct sockaddr_in impl_simclient_addr;
	enum vgsm_mesim_impl_state impl_state;
	struct vgsm_timer impl_timer;
};

const char *vgsm_mesim_state_to_text(
	enum vgsm_mesim_state state);

int vgsm_mesim_create(struct vgsm_mesim *mesim, struct vgsm_module *module);
void vgsm_mesim_destroy(struct vgsm_mesim *mesim);

struct vgsm_mesim *vgsm_mesim_get(
	struct vgsm_mesim *mesim);
void _vgsm_mesim_put(struct vgsm_mesim *mesim);
#define vgsm_mesim_put(sim) \
	do { _vgsm_mesim_put(sim); (sim) = NULL; } while(0)

int vgsm_mesim_open(struct vgsm_mesim *mesim, int fd);
void vgsm_mesim_close(struct vgsm_mesim *mesim);

void vgsm_mesim_get_ready_for_poweron(struct vgsm_mesim *mesim);

void vgsm_mesim_send_message(
	struct vgsm_mesim *mesim,
	enum vgsm_mesim_message_type mt,
	void *data, int len);


const char *vgsm_mesim_impl_state_to_text(
	enum vgsm_mesim_impl_state state);

#endif
