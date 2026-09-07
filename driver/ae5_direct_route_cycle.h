/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AE5_DIRECT_ROUTE_CYCLE_H
#define AE5_DIRECT_ROUTE_CYCLE_H
#include "ae5_route_capture.h"
#include "ae5_clock_cycle.h"
struct ae5_transport_snapshot {
	ae5_u32 format, converter, references, clocks[AE5_REG_COUNT];
	struct ae5_route_plan route;
	struct ae5_route_capture raw;
	int route_error, peer_error;
	struct { ae5_u32 references, count, words[16]; } peers[3];
	/* Streams 05,14,18: source,destination,channels,enabled.
	 * Connections 00,43,d0: sample-rate enum. Selectors: stream,connection.
	 */
	ae5_u32 parameters[3][4], rates[3], selectors[2];
};
struct ae5_transport_cycle_io {
	int (*read)(void *, ae5_u32, bool, struct ae5_transport_snapshot *);
	int (*format_write)(void *, ae5_u32);
	int (*tag_write)(void *, bool);
	int (*param_write)(void *, ae5_u32, ae5_u32);
	void *context;
	ae5_u32 tag;
	int (*exercise)(void *, ae5_u32, const struct ae5_route_plan *, bool *);
};
struct ae5_transport_cycle_result {
	bool attempted, target_verified, restore_verified;
	int transition_error, restore_error;
	struct ae5_transport_snapshot before, at96, at192, at384, restored;
};
int ae5_transport_cycle(const struct ae5_transport_cycle_io *,
	struct ae5_transport_cycle_result *, bool);
int ae5_direct_route_restore(const struct ae5_transport_cycle_io *, const struct ae5_transport_snapshot *, struct ae5_transport_snapshot *);
int ae5_direct_route_activate(const struct ae5_transport_cycle_io *, ae5_u32, struct ae5_transport_cycle_result *);
#endif
