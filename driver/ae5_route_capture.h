/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AE5_ROUTE_CAPTURE_H
#define AE5_ROUTE_CAPTURE_H
#include "ae5_route_plan.h"
/* Diagnostic evidence only. This structure never authorizes router writes.
 * Preserve partial reads independently of the strict planner's empty-on-error
 * contract. count includes the current port; word_count/next_count say which
 * values were actually read. complete means an 0xff terminator was observed,
 * not that the chain has the expected length, source order or active bits.
 */
struct ae5_route_capture {
	ae5_u32 first, last;
	unsigned int count, word_count, next_count;
	bool first_valid, last_valid, complete;
	int error;
	struct { ae5_u32 port, word, next; } entries[16];
};
int ae5_route_capture(const struct ae5_route_reader *io,
		      struct ae5_route_capture *out);
#endif
