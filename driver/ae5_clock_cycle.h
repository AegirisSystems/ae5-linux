/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AE5_CLOCK_CYCLE_H
#define AE5_CLOCK_CYCLE_H
#include "ae5_direct_core.h"

/* This experiment tests only clock-register programming and restoration.
 * It never selects Direct playback, configures DMA, or touches the DAC/amp.
 * The caller must physically disconnect headphones, close every codec PCM,
 * hold all PCM open mutexes and the stock ChipIO mutex, and pin active power.
 */
enum ae5_cycle_op { AE5_CYCLE_READ, AE5_CYCLE_WRITE };
enum ae5_cycle_reg { AE5_MODE, AE5_ASI, AE5_PLL43,
	AE5_CLOCK0, AE5_CLOCK1, AE5_DIVIDER, AE5_CLOCK3, AE5_REG_COUNT };
struct ae5_cycle_io {
	int (*transfer)(void *context, enum ae5_cycle_op op,
		       enum ae5_cycle_reg reg, ae5_u32 *value);
	void *context;
};
struct ae5_cycle_result {
	bool attempted, target_verified, restore_verified;
	int transition_error, restore_error;
	ae5_u32 before[AE5_REG_COUNT], target[AE5_REG_COUNT];
};
int ae5_clock_cycle(const struct ae5_cycle_io *io,
		    struct ae5_cycle_result *result, bool isolated);
int ae5_clock_baseline(const struct ae5_cycle_io *io, ae5_u32 *observed);
#endif
