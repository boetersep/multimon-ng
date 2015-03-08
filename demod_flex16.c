/*
 *      demod_flex16.c -- 1600 baud FLEX demodulator
 *
 *      Copyright (C) 2015
 *          Erik Boeters (e.boeters@gmail.com)
 *
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* ---------------------------------------------------------------------- */

#include "multimon.h"
#include "filter.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------- */

#define FREQ_SAMP  22050
#define BAUD       1600
#define SUBSAMP    2
#define FILTLEN    1

/* ---------------------------------------------------------------------- */

#define SPHASEINC (0x10000u*BAUD*SUBSAMP/FREQ_SAMP)

/* ---------------------------------------------------------------------- */
	
static void flex16_init(struct demod_state *s)
{
	flex_init(s);
	memset(&s->l1.flex16, 0, sizeof(s->l1.flex16));
}

/* ---------------------------------------------------------------------- */

static void flex16_demod(struct demod_state *s, buffer_t buffer, int length)
{
	if (s->l1.flex16.subsamp) {
		int numfill = SUBSAMP - s->l1.flex16.subsamp;
		if (length < numfill) {
			s->l1.flex16.subsamp += length;
			return;
		}
		buffer.fbuffer += numfill;
		length -= numfill;
		s->l1.flex16.subsamp = 0;
	}
	for (; length >= SUBSAMP; length -= SUBSAMP, buffer.fbuffer += SUBSAMP) {
		s->l1.flex16.dcd_shreg <<= 1;
		s->l1.flex16.dcd_shreg |= ((*buffer.fbuffer) > 0);
		verbprintf(10, "%c", '0'+(s->l1.flex16.dcd_shreg & 1));
		/*
		 * check if transition
		 */
		if ((s->l1.flex16.dcd_shreg ^ (s->l1.flex16.dcd_shreg >> 1)) & 1) {
			if (s->l1.flex16.sphase < (0x8000u-(SPHASEINC/2)))
				s->l1.flex16.sphase += SPHASEINC/8;
			else
				s->l1.flex16.sphase -= SPHASEINC/8;
		}
		s->l1.flex16.sphase += SPHASEINC;
		if (s->l1.flex16.sphase >= 0x10000u) {
			s->l1.flex16.sphase &= 0xffffu;
			//pocsag_rxbit(s, s->l1.flex16.dcd_shreg & 1);
			flex_rxbit(s, (s->l1.flex16.dcd_shreg & 1) ? 3 : 0);
		}
	}
	s->l1.flex16.subsamp = length;
}

static void flex16_deinit(struct demod_state *s)
{
  flex_deinit(s);
}

/* ---------------------------------------------------------------------- */

const struct demod_param demod_flex16 = {
    "FLEX16", true, FREQ_SAMP, FILTLEN, flex16_init, flex16_demod, flex16_deinit
};

/* ---------------------------------------------------------------------- */
