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
 * Ispired from http://ftp.sunet.se/pub2/gnu/vm/base64-encode.c :)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <asterisk/version.h>
#if ASTERISK_VERSION_NUM < 010600 || (ASTERISK_VERSION_NUM >=10200  && ASTERISK_VERSION_NUM < 10600)
#else
#include <asterisk.h>
#endif

#include "util.h"

int quoted_printable_decode(const __u8 *src, __u8 *dest, int dest_size)
{
	const __u8 *inp;
	__u8 *olddest = dest;
	__u8 *outp = dest;

	for (inp = src;
	     *inp && ((outp - olddest) < (dest_size - 2));
	     inp++) {

		if (*inp == '=') {
			int val;

			inp++;

			if (!*inp)
				return -EINVAL;

			if (*inp == '\n')
				continue;

			val = char_to_hexdigit(*inp);
			if (val < 0)
				return -EINVAL;

			*outp = val << 4;

			inp++;

			if (!*inp)
				return -EINVAL;

			val = char_to_hexdigit(*inp);
			if (val < 0)
				return -EINVAL;

			*outp += val;

		} else if (*inp == '_')
			*outp = ' ';
		else
			*outp = *inp;

		outp++;
	}

	*outp = '\0';

	return outp - olddest;
}
