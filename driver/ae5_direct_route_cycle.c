/* SPDX-License-Identifier: GPL-2.0-only */
#include "ae5_direct_route_cycle.h"
static int checked(int err) { return err > 0 ? -EIO : err; }
static int parameter(const struct ae5_transport_cycle_io *io, ae5_u32 id, ae5_u32 v)
{
	return checked(io->param_write(io->context, id, v));
}
static int stream_field(const struct ae5_transport_cycle_io *io, ae5_u32 s, ae5_u32 id, ae5_u32 v)
{
	int err = parameter(io, 0x18, s);
	return err ? err : parameter(io, id, v);
}
static int connection(const struct ae5_transport_cycle_io *io, ae5_u32 c, ae5_u32 rate)
{
	int err = parameter(io, 0x1d, c);
	return err ? err : parameter(io, 0x1e, rate);
}
static int clocks_match(const struct ae5_transport_snapshot *s)
{
	static const ae5_u32 expected[] = {0x81,7,0xc7,0x1f101,0x1f101,0x14004,0x2000f};
	unsigned int i;
	for (i = 0; i < AE5_REG_COUNT; i++) if (s->clocks[i] != expected[i]) return -EIO;
	return 0;
}
static int baseline(const struct ae5_transport_snapshot *s)
{
	if (clocks_match(s) || s->format || s->converter || s->references || s->raw.count || s->route.count ||
	    !s->raw.complete || s->route_error || s->peer_error) return -EINVAL;
	if ((s->peers[0].references != 1 && s->peers[0].references != 2) || s->peers[0].count != 12 ||
	    s->peers[1].references || s->peers[1].count ||
	    s->peers[2].references != 1 || s->peers[2].count != 12) return -EINVAL;
	if (s->parameters[0][0] != 0x43 || s->parameters[0][1] ||
	    s->parameters[0][2] > 4 || s->parameters[0][3] ||
	    s->parameters[1][0] != 0x48 || s->parameters[1][1] != 0xd0 ||
	    s->parameters[1][2] != 2 || s->parameters[1][3] ||
	    s->parameters[2][0] != 9 || s->parameters[2][1] != 0xd0 ||
	    s->parameters[2][2] != 6 || s->parameters[2][3] != 1 ||
	    s->rates[0] != 0xb || s->rates[1] > 0xf || s->rates[2] != 0xb)
		return -EINVAL;
	return 0;
}
static int restored_matches(const struct ae5_transport_snapshot *a, const struct ae5_transport_snapshot *b)
{
	unsigned int i, j;
	if (baseline(b)) return -EIO;
	if (b->peers[0].references > a->peers[0].references) return -EIO;
	for (i = 0; i < 3; i++) {
		if (a->rates[i] != b->rates[i] || a->peers[i].count != b->peers[i].count) return -EIO;
		for (j = 0; j < 4; j++) if (a->parameters[i][j] != b->parameters[i][j]) return -EIO;
		for (j = 0; j < a->peers[i].count; j++)
			if (a->peers[i].words[j] != b->peers[i].words[j]) return -EIO;
	}
	for (i = 0; i < 2; i++) if (a->selectors[i] != b->selectors[i]) return -EIO;
	return 0;
}
static int prepare(const struct ae5_transport_cycle_io *io, ae5_u32 rate)
{
	int err = stream_field(io, 0x18, 0x1c, 0);
	if (!err) err = stream_field(io, 5, 0x19, 0x43);
	if (!err) err = stream_field(io, 5, 0x1a, 0xd0);
	if (!err) err = stream_field(io, 5, 0x1b, 2);
	if (!err) err = connection(io, 0xd0, rate == 96000 ? 0xb : 0xe);
	if (!err) err = stream_field(io, 0x14, 0x19, 0x43);
	if (!err) err = stream_field(io, 0x14, 0x1a, 0);
	if (!err) err = connection(io, 0, 0xb);
	if (!err) err = stream_field(io, 0x14, 0x1b, 2);
	if (!err) err = checked(io->format_write(io->context, rate == 96000 ? 0x841 : rate == 192000 ? 0x1841 : 0x1843));
	if (!err) err = checked(io->tag_write(io->context, true));
	return err;
}
static void keep_error(int *first, int err) { if (!*first && err) *first = checked(err); }
static int restore(const struct ae5_transport_cycle_io *io,
	const struct ae5_transport_snapshot *before, struct ae5_transport_snapshot *after)
{
	static const ae5_u32 streams[] = {5,0x14,0x18}, connections[] = {0,0x43,0xd0};
	unsigned int i, j;
	int err = 0, read_error;
	keep_error(&err, io->tag_write(io->context, false));
	keep_error(&err, io->format_write(io->context, before->format));
	/* Continue bounded restoration after failures, but never call it verified
	 * if a recovery transfer failed, even if the final snapshot looks right.
	 */
	keep_error(&err, stream_field(io, 0x18, 0x1c, 0));
	for (i = 0; i < 3; i++) for (j = 0; j < 3; j++)
		keep_error(&err, stream_field(io, streams[i], 0x19 + j, before->parameters[i][j]));
	for (i = 0; i < 3; i++) keep_error(&err, connection(io, connections[i], before->rates[i]));
	keep_error(&err, stream_field(io, 0x18, 0x1c, before->parameters[2][3]));
	keep_error(&err, parameter(io, 0x18, before->selectors[0]));
	keep_error(&err, parameter(io, 0x1d, before->selectors[1]));
	read_error = checked(io->read(io->context, 96000, false, after));
	keep_error(&err, read_error);
	if (!read_error) keep_error(&err, restored_matches(before, after));
	return err;
}
static int target(const struct ae5_transport_cycle_io *io, ae5_u32 rate,
	const struct ae5_transport_snapshot *before, struct ae5_transport_snapshot *out)
{
	unsigned int i;
	int err = checked(io->read(io->context, rate, true, out));
	if (err) return err;
	if (clocks_match(out) || out->route_error || out->peer_error ||
	    out->converter != io->tag << 4 || out->references != 1 ||
	    out->format != (rate == 96000 ? 0x841 : rate == 192000 ? 0x1841 : 0x1843) ||
	    out->route.count != rate / 48000 * 2 || out->raw.count != out->route.count ||
	    !out->raw.complete || out->peers[0].references != before->peers[0].references + 1 || out->peers[0].count != 12 ||
	    out->peers[1].references || out->peers[1].count ||
	    out->peers[2].references || out->peers[2].count) return -EINVAL;
	for (i = 0; i < 12; i++)
		if (before->peers[0].words[i] != out->peers[0].words[i]) return -EIO;
	for (i = 0; i < 3; i++)
		if (before->parameters[2][i] != out->parameters[2][i]) return -EIO;
	if (out->parameters[0][0] != 0x43 || out->parameters[0][1] != 0xd0 ||
	    out->parameters[0][2] != (rate == 384000 ? 4 : 2) || out->parameters[0][3] != 1 ||
	    out->parameters[1][0] != 0x43 || out->parameters[1][1] ||
	    out->parameters[1][2] != 2 || out->parameters[1][3] ||
	    out->parameters[2][3] || out->rates[0] != 0xb ||
	    out->rates[1] != (rate == 96000 ? 0xb : 0xe) ||
	    out->rates[2] != (rate == 96000 ? 0xb : 0xe)) return -EINVAL;
	return 0;
}
int ae5_transport_cycle(const struct ae5_transport_cycle_io *io,
	struct ae5_transport_cycle_result *r, bool isolated)
{
	static const ae5_u32 rates[] = {96000,192000,384000};
	struct ae5_transport_snapshot *targets[3];
	unsigned int i;
	int err;
	if (!io || !r || !io->read || !io->format_write || !io->tag_write ||
	    !io->param_write || !io->exercise || !io->tag || io->tag > 15) return -EINVAL;
	*r = (struct ae5_transport_cycle_result){0};
	if (!isolated) return -EBUSY;
	err = checked(io->read(io->context, 96000, false, &r->before));
	if (!err) err = baseline(&r->before);
	if (err) return err;
	targets[0] = &r->at96; targets[1] = &r->at192; targets[2] = &r->at384;
	for (i = 0; i < 3; i++) {
		bool path_restored = true;
		r->attempted = true; r->restore_verified = false;
		err = prepare(io, rates[i]);
		if (!err) err = target(io, rates[i], &r->before, targets[i]);
		if (!err) err = checked(io->exercise(io->context, rates[i], &targets[i]->route, &path_restored));
		if (!path_restored) {
			r->transition_error = err; r->restore_error = -EIO;
			return err ? err : -EIO;
		}
		r->transition_error = err;
		r->restore_error = restore(io, &r->before, &r->restored);
		r->restore_verified = !r->restore_error;
		if (err || r->restore_error) return r->restore_error ? r->restore_error : err;
	}
	r->target_verified = true;
	return 0;
}

int ae5_direct_route_restore(const struct ae5_transport_cycle_io *io,
    const struct ae5_transport_snapshot *before, struct ae5_transport_snapshot *after)
{
    if (!io || !before || !after || !io->read || !io->format_write || !io->tag_write || !io->param_write || baseline(before)) return -EINVAL;
    return restore(io,before,after);
}

int ae5_direct_route_activate(const struct ae5_transport_cycle_io *io, ae5_u32 rate,
    struct ae5_transport_cycle_result *r)
{
    int err;
    struct ae5_transport_snapshot *out;
    if (!io || !r || !io->read || !io->format_write || !io->tag_write ||
        !io->param_write || !io->tag || io->tag>15 ||
        (rate!=96000 && rate!=192000 && rate!=384000)) return -EINVAL;
    *r=(struct ae5_transport_cycle_result){0};
    err=checked(io->read(io->context,96000,false,&r->before));
    if (!err) err=baseline(&r->before);
    /* baseline() accepts exactly the measured standalone or active-WUH topology. */
    if (err) return err;
    r->attempted=true;
    out=rate==96000 ? &r->at96 : rate==192000 ? &r->at192 : &r->at384;
    err=prepare(io,rate);
    if (!err) err=target(io,rate,&r->before,out);
    r->transition_error=err;
    r->target_verified=!err;
    if (!err) return 0;
    r->restore_error=restore(io,&r->before,&r->restored);
    r->restore_verified=!r->restore_error;
    return r->restore_error ? r->restore_error : err;
}
