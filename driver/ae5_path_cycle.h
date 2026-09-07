/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AE5_PATH_CYCLE_H
#define AE5_PATH_CYCLE_H
#include "ae5_clock_cycle.h"
#include "ae5_route_plan.h"
struct ae5_path_io {
	struct ae5_cycle_io clock;
	int (*route_read)(void *, ae5_u32, ae5_u32 *);
	int (*route_write)(void *, ae5_u32, ae5_u32);
	int (*commit)(void *);
	int (*auxiliary)(void *, bool, ae5_u32 *, ae5_u32 *);
	void *context;
};
struct ae5_path_result {
	bool attempted, target_verified, restore_verified;
	int transition_error, restore_error;
	ae5_u32 rate, target_clocks[AE5_REG_COUNT], target_words[16], target_aux, target_refs;
};
int ae5_path_cycle(const struct ae5_path_io *, ae5_u32, const struct ae5_route_plan *, struct ae5_path_result *);
int ae5_path_restore(const struct ae5_path_io *, const struct ae5_route_plan *);
int ae5_path_activate(const struct ae5_path_io *, ae5_u32, const struct ae5_route_plan *, struct ae5_path_result *);
#endif
