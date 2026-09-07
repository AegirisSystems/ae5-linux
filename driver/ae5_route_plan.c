/* SPDX-License-Identifier: GPL-2.0-only */
#include "ae5_route_plan.h"
static int route_read(const struct ae5_route_reader *io, bool exram,
		      ae5_u32 addr, ae5_u32 *value)
{
	int err = io->read(io->context, exram, addr, value);
	if (err) return err < 0 ? err : -EIO;
	return exram && *value > 0xff ? -EIO : 0;
}
int ae5_route_plan(const struct ae5_route_reader *io, ae5_u32 rate,
		   struct ae5_route_plan *out)
{
	struct ae5_route_plan plan = {0};
	ae5_u32 port, last, next, word;
	unsigned int i, j;
	int err;
	if (!out || !io || !io->read ||
	    (rate != 48000 && rate != 96000 && rate != 192000 && rate != 384000))
		return -EINVAL;
	/* A failure cannot leave a stale usable plan from an earlier request. */
	*out = (struct ae5_route_plan){0};
	plan.count = rate / 48000 * 2;
	err = route_read(io, true, 0x157d, &port);
	if (err) return err;
	err = route_read(io, true, 0x15a2, &last);
	if (err) return err;
	if (port == 0xff || last == 0xff) return -ENOSPC;
	for (i = 0; i < plan.count; i++) {
		if (port == 0xff) return -ENOSPC;
		for (j = 0; j < i; j++)
			if (plan.words[j].port == port) return -ELOOP;
		err = route_read(io, false, 0x190000 + 4 * port, &word);
		if (err) return err;
		if (!(word & 0x10000) || (word & 0xff) != i) return -EINVAL;
		plan.words[i] = (struct ae5_route_word){port, word,
			(word & 0xfffd00ff) | (0x4000 + i * 0x100)};
		err = route_read(io, true, 0x1478 + port, &next);
		if (err) return err;
		if (i + 1 == plan.count) {
			if (port != last || next != 0xff) return -EINVAL;
		} else if (port == last) {
			return -ENOSPC;
		}
		port = next;
	}
	*out = plan;
	return 0;
}
