
#ifndef _Q931_IE_CALL_STATE_H
#define _Q931_IE_CALL_STATE_H

#include "q931_ie.h"

enum q931_ie_call_state_coding_standard
{
	Q931_IE_CS_CS_CCITT	= 0x0,
	Q931_IE_CS_CS_RESERVED	= 0x1,
	Q931_IE_CS_CS_NATIONAL	= 0x2,
	Q931_IE_CS_CS_SPECIFIC	= 0x3,
};

enum q931_ie_call_state_call_state_net
{
	Q931_IE_CS_N0_NULL_STATE		= 0x00,
	Q931_IE_CS_N1_CALL_INITIATED		= 0x01,
	Q931_IE_CS_N2_OVERLAP_SENDING		= 0x02,
	Q931_IE_CS_N3_OUTGOING_CALL_PROCEEDING	= 0x03,
	Q931_IE_CS_N4_CALL_DELIVERED		= 0x04,
	Q931_IE_CS_N6_CALL_PRESENT		= 0x06,
	Q931_IE_CS_N7_CALL_RECEIVED		= 0x07,
	Q931_IE_CS_N8_CONNECT_REQUEST		= 0x08,
	Q931_IE_CS_N9_INCOMING_CALL_PROCEEDING	= 0x09,
	Q931_IE_CS_N10_ACTIVE			= 0x0A,
	Q931_IE_CS_N11_DISCONNECT_REQUEST	= 0x0B,
	Q931_IE_CS_N12_DISCONNECT_INDICATION	= 0x0C,
	Q931_IE_CS_N15_SUSPEND_REQUEST		= 0x0F,
	Q931_IE_CS_N17_RESUME_REQUEST		= 0x11,
	Q931_IE_CS_N19_RELEASE_REQUEST		= 0x13,
	Q931_IE_CS_N22_CALL_ABORT		= 0x16,
	Q931_IE_CS_N25_OVERLAP_RECEIVING	= 0x19,
};

enum q931_ie_call_state_call_state_user
{
	Q931_IE_CS_U0_NULL_STATE		= 0x00,
	Q931_IE_CS_U1_CALL_INITIATED		= 0x01,
	Q931_IE_CS_U2_OVERLAP_SENDING		= 0x02,
	Q931_IE_CS_U3_OUTGOING_CALL_PROCEEDING	= 0x03,
	Q931_IE_CS_U4_CALL_DELIVERED		= 0x04,
	Q931_IE_CS_U6_CALL_PRESENT		= 0x06,
	Q931_IE_CS_U7_CALL_RECEIVED		= 0x07,
	Q931_IE_CS_U8_CONNECT_REQUEST		= 0x08,
	Q931_IE_CS_U9_INCOMING_CALL_PROCEEDING	= 0x09,
	Q931_IE_CS_U10_ACTIVE			= 0x0A,
	Q931_IE_CS_U11_DISCONNECT_REQUEST	= 0x0B,
	Q931_IE_CS_U12_DISCONNECT_INDICATION	= 0x0C,
	Q931_IE_CS_U15_SUSPEND_REQUEST		= 0x0F,
	Q931_IE_CS_U17_RESUME_REQUEST		= 0x11,
	Q931_IE_CS_U19_RELEASE_REQUEST		= 0x13,
	Q931_IE_CS_U25_OVERLAP_RECEIVING	= 0x19,
};

enum q931_ie_call_state_call_state_glob
{
	Q931_IE_GIS_REST_0			= 0x00,
	Q931_IE_GIS_REST_1			= 0x3D,
	Q931_IE_GIS_REST_2			= 0x3E,
};

struct q931_ie_call_state_onwire_3
{
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 coding_standard:2;
	__u8 value:6;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	__u8 value:6;
	__u8 coding_standard:2;
#endif
} __attribute__ ((__packed__));

static inline int q931_append_ie_call_state(void *buf,
        int value)
{
 struct q931_ie_onwire *ie = (struct q931_ie_onwire *)buf;

 ie->id = Q931_IE_CALL_STATE;
 ie->size = 0;

 struct q931_ie_call_state_onwire_3 *oct_3 =
   (struct q931_ie_call_state_onwire_3 *)(&ie->data[ie->size]);
 oct_3->coding_standard = Q931_IE_CS_CS_CCITT;
 oct_3->value = value;
 ie->size += 1;

 return ie->size + sizeof(struct q931_ie_onwire);
}

#endif
