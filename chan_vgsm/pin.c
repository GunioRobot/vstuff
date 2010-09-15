/*
 * vGSM channel driver for Asterisk
 *
 * Copyright (C) 2006-2008 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU General Public License.
 *
 */

#include <string.h>
#include <ctype.h>
#include <asterisk/version.h>
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
#else
#include <asterisk.h>
#endif

#include "pin.h"

BOOL vgsm_pin_valid(const char *pin)
{
	int i;

	for (i=0; i<strlen(pin); i++) {
		if (!isdigit(pin[i]))
			return FALSE;
	}

	return TRUE;
}

