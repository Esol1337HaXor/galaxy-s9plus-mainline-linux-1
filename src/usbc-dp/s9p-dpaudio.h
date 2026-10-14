/* SPDX-License-Identifier: GPL-2.0 */
#ifndef S9P_DPAUDIO_H
#define S9P_DPAUDIO_H

#include <linux/types.h>

/* physical address of the SST1 audio FIFO (DP controller + 0x5818) */
#define S9P_DP_AUDIO_FIFO	0x11095818

/* exported by s9p-dp for s9p-dpaudio */
int s9p_dp_audio_start(unsigned int rate, unsigned int channels,
		       unsigned int bits, unsigned int phys_bits);
void s9p_dp_audio_stop(void);
void s9p_dp_audio_run(bool run);	/* DMA requests on/off; atomic context */
bool s9p_dp_audio_ready(void);		/* link trained, video on */

#endif
