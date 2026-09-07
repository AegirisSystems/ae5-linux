/* SPDX-License-Identifier: GPL-2.0-only */
#include "ae5_route_capture.h"
static int capture_read(const struct ae5_route_reader *io, bool exram,
	ae5_u32 address, ae5_u32 *value)
{
	int err = io->read(io->context, exram, address, value);
	if (err) return err < 0 ? err : -EIO;
	return (exram && *value > 0xff) || *value == ~0U ? -EIO : 0;
}
int ae5_route_capture(const struct ae5_route_reader *io,
		      struct ae5_route_capture *out)
{
	ae5_u32 port;
	unsigned int i, j;
	int err;
	if (!out) return -EINVAL;
	*out = (struct ae5_route_capture){0};
	if (!io || !io->read) { out->error = -EINVAL; return out->error; }
	err = capture_read(io, true, 0x157d, &out->first);
	if (err) goto done;
	out->first_valid = true;
	err = capture_read(io, true, 0x15a2, &out->last);
	if (err) goto done;
	out->last_valid = true;
	port = out->first;
	for (i = 0; i < 16; i++) {
		if (port == 0xff) { out->complete = true; goto done; }
		for (j = 0; j < i; j++)
			if (out->entries[j].port == port) { err = -ELOOP; goto done; }
		out->entries[i].port = port;
		out->count++;
		err = capture_read(io, false, 0x190000 + 4 * port, &out->entries[i].word);
		if (err) goto done;
		out->word_count++;
		err = capture_read(io, true, 0x1478 + port, &out->entries[i].next);
		if (err) goto done;
		out->next_count++;
		port = out->entries[i].next;
	}
	if (port == 0xff) out->complete = true;
	else err = -E2BIG;
done:
	out->error = err;
	return err;
}
