#ifndef LAPD_H
#define LAPD_H

#include <net/sock.h>
#include <asm/atomic.h>

#include "tei_mgmt_nt.h"

#ifdef __KERNEL__

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#define LAPD_SAPI_Q931		0x00
#define LAPD_SAPI_X25		0x0f

#define LAPD_BROADCAST_TEI	127

extern struct hlist_head lapd_hash;
extern rwlock_t lapd_hash_lock;

struct lapd_sap
{
	atomic_t refcnt;

	// SAP parameters

	int k;

	int N200;
	int N201;

	int T200;
	int T203;
};

struct lapd_dev
{
	struct lapd_net_tei_mgmt_entity *net_tme;
	struct lapd_sap q931;
	struct lapd_sap x25;
};

/*struct lapd_connect_request
{
	struct list_head hash_list;
	u8 tei;
};*/

static inline struct lapd_sap *lapd_sap_alloc()
{
	return kmalloc(sizeof(struct lapd_sap), GFP_ATOMIC);
}

static inline void lapd_sap_free(
	struct lapd_sap *entity)
{
	kfree(entity);
}

static inline void lapd_sap_hold(
	struct lapd_sap *entity)
{
	atomic_inc(&entity->refcnt);
}

static inline void lapd_sap_put(
	struct lapd_sap *entity)
{
	if (atomic_dec_and_test(&entity->refcnt))
		lapd_sap_free(entity);
}

struct lapd_opt
{
	struct net_device *dev;

	int nt_mode;

	// Datalink status
	int N200_cnt;

	struct timer_list T200_timer;
	struct timer_list T203_timer;

	u8 v_s;
	u8 v_r;
	u8 v_a;

	enum lapd_datalink_status status;

	int peer_busy;
	int me_busy;
	int peer_waiting_for_ack;
	int rejection_exception;
	int in_timer_recovery;
	// ------------------

	struct lapd_sap *sap;
	struct lapd_usr_tei_mgmt_entity *usr_tme;
	struct lapd_net_tei_mgmt_entity *net_tme;
};

/* WARNING: don't change the layout of the members in lapd_sock! */
struct lapd_sock {
	struct sock sk;
	struct lapd_opt lapd;
};

struct lapd_device {
	struct net_device *dev;
	
};

static inline struct lapd_opt *lapd_sk(const struct sock *__sk)
{
	return &((struct lapd_sock *)__sk)->lapd;
}

extern void setup_lapd(struct net_device *netdev);

// INOUT

void lapd_unhash(struct sock *sk);
void lapd_start_multiframe_establishment(struct sock *sk);
void lapd_start_multiframe_release(struct lapd_opt *lo);
void lapd_T200_timer(unsigned long data);
inline int lapd_handle_socket_frame(struct sock *sk, struct sk_buff *skb);
void lapd_T203_timer(unsigned long data);
void lapd_T203_timer(unsigned long data);

int lapd_send_iframe(struct sock *sk, u8 sapi, u8 tei,
	void *data, int datalen);
int lapd_prepare_iframe(struct sk_buff *skb, u8 sapi, u8 tei);
int lapd_send_completed_iframe(struct sk_buff *skb);

int lapd_prepare_uframe(struct sk_buff *skb, u8 sapi, u8 tei,
	enum lapd_uframe_function function);
int lapd_send_completed_uframe(struct sk_buff *skb);

int lapd_send_uframe(struct sock *sk, u8 sapi, u8 tei,
	enum lapd_uframe_function function, void *data, int datalen);

inline int lapd_send_frame(struct sk_buff *skb);

static inline int lapd_is_valid_nr(struct lapd_opt *lo, int n_r)
{
	// V(A) <= N(R) <= V(S)

	return (n_r - lo->v_a) % 128 <=
		(lo->v_s - lo->v_a) % 128;
}


#endif
#endif
