/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AE5_ROUTE_PLAN_H
#define AE5_ROUTE_PLAN_H
#include "ae5_direct_core.h"
struct ae5_route_word { ae5_u32 port, before, after; };
struct ae5_route_plan { unsigned int count; struct ae5_route_word words[16]; };
struct ae5_route_reader {
	int (*read)(void *context, bool exram, ae5_u32 address, ae5_u32 *value);
	void *context;
};
/* Read-only planner for AE-5 HDA stream 5. The caller must hold the stock
 * ChipIO lock and keep allocation stable. No plan is returned unless the
 * complete chain, last-port table, active bits and source order agree.
 * A later application must run under the same lock/allocation exclusion.
 */
int ae5_route_plan(const struct ae5_route_reader *io, ae5_u32 rate,
		   struct ae5_route_plan *out);
#endif
