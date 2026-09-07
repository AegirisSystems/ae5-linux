/* SPDX-License-Identifier: GPL-2.0-only */
#include "ae5_path_cycle.h"
static int normalized(int err) { return err > 0 ? -EIO : err; }
static void remember(int *first, int err) { if (!*first && err) *first = normalized(err); }
static int clock_set(const struct ae5_path_io *io, enum ae5_cycle_reg reg, ae5_u32 v)
{
	return normalized(io->clock.transfer(io->clock.context, AE5_CYCLE_WRITE, reg, &v));
}
static int clock_read(const struct ae5_path_io *io, const ae5_u32 *expected, ae5_u32 *out)
{
	unsigned int i;
	int err;
	for (i=0;i<AE5_REG_COUNT;i++) {
		err=normalized(io->clock.transfer(io->clock.context,AE5_CYCLE_READ,i,&out[i]));
		if (err) return err;
		if (out[i]!=expected[i]) return -EIO;
	}
	return 0;
}
static int mode_set(const struct ae5_path_io *io, ae5_u32 mode)
{
	ae5_u32 value;
	unsigned int i;
	int err=clock_set(io,AE5_MODE,mode);
	if (err) return err;
	for (i=0;i<11;i++) {
		err=normalized(io->clock.transfer(io->clock.context,AE5_CYCLE_READ,AE5_MODE,&value));
		if (err || value==mode) return err;
	}
	return -ETIMEDOUT;
}
static int words_check(const struct ae5_path_io *io, const struct ae5_route_plan *p, bool remapped, ae5_u32 *out)
{
	unsigned int i;
	int err;
	for (i=0;i<p->count;i++) {
		err=normalized(io->route_read(io->context,p->words[i].port,&out[i]));
		if (err) return err;
		if (out[i]!=(remapped ? p->words[i].after : p->words[i].before)) return -EIO;
	}
	return 0;
}
int ae5_path_restore(const struct ae5_path_io *io, const struct ae5_route_plan *plan)
{
	static const ae5_u32 normal[]={0x81,7,0xc7,0x1f101,0x1f101,0x14004,0x2000f};
	ae5_u32 scratch[16],aux=0,refs=0;
	unsigned int i,j;
	int recovery=0;
	if (!io || !plan || !io->clock.transfer || !io->route_read || !io->route_write || !io->commit || !io->auxiliary ||
	    !plan->count || plan->count>16) return -EINVAL;
	for (i=0;i<plan->count;i++) {
		if (plan->words[i].port>254 || !(plan->words[i].before&0x10000) || (plan->words[i].before&255)!=i) return -EINVAL;
		for (j=0;j<i;j++) if (plan->words[i].port==plan->words[j].port) return -EINVAL;
	}
	remember(&recovery,clock_set(io,AE5_ASI,4));
	remember(&recovery,io->auxiliary(io->context,true,&aux,&refs));
	if (aux || refs) remember(&recovery,-EIO);
	for (i=0;i<plan->count;i++) remember(&recovery,io->route_write(io->context,plan->words[i].port,plan->words[i].before));
	remember(&recovery,io->commit(io->context));
	remember(&recovery,mode_set(io,normal[AE5_MODE]));
	for (i=AE5_CLOCK0;i<AE5_REG_COUNT;i++) remember(&recovery,clock_set(io,i,normal[i]));
	remember(&recovery,clock_set(io,AE5_ASI,7));
	remember(&recovery,clock_read(io,normal,scratch));
	remember(&recovery,words_check(io,plan,false,scratch));
	remember(&recovery,io->auxiliary(io->context,false,&aux,&refs));
	if (aux || refs) remember(&recovery,-EIO);
	return recovery;
}
int ae5_path_cycle(const struct ae5_path_io *io, ae5_u32 rate,
	const struct ae5_route_plan *plan, struct ae5_path_result *r)
{
	static const ae5_u32 normal[]={0x81,7,0xc7,0x1f101,0x1f101,0x14004,0x2000f};
	ae5_u32 target[AE5_REG_COUNT]={0x81,7,0xc7,0x1f101,0x1f101,0x14004,0x2000f};
	ae5_u32 scratch[16], aux=0, refs=0;
	unsigned int i,j;
	int err, recovery=0;
	if (!io || !r || !plan || !io->clock.transfer || !io->route_read || !io->route_write || !io->commit || !io->auxiliary ||
	    (rate!=96000 && rate!=192000 && rate!=384000) || plan->count!=rate/48000*2) return -EINVAL;
	*r=(struct ae5_path_result){.rate=rate};
	for (i=0;i<plan->count;i++) {
		if (plan->words[i].port>254 || !(plan->words[i].before&0x10000) || (plan->words[i].before&255)!=i ||
		    plan->words[i].after!=((plan->words[i].before&0xfffd00ff)|(0x4000+i*0x100))) return -EINVAL;
		for (j=0;j<i;j++) if (plan->words[i].port==plan->words[j].port) return -EINVAL;
	}
	if (rate!=96000) {
		target[AE5_MODE]=rate==384000 ? 0x80 : 0x81;
		target[AE5_CLOCK0]=target[AE5_CLOCK1]=rate==384000 ? 0x1f103 : 0x1f102;
		target[AE5_DIVIDER]=rate==384000 ? 0x31002 : 0x22003;
	}
	err=clock_read(io,normal,scratch);
	if (!err) err=words_check(io,plan,false,scratch);
	if (!err) err=normalized(io->auxiliary(io->context,false,&aux,&refs));
	if (!err && (aux || refs)) err=-EBUSY;
	if (err) return err;
	r->attempted=true;
	err=clock_set(io,AE5_ASI,4);
	if (!err) err=mode_set(io,target[AE5_MODE]);
	for (i=AE5_CLOCK0;i<AE5_REG_COUNT && !err;i++) err=clock_set(io,i,target[i]);
	for (i=0;i<plan->count && !err;i++) err=normalized(io->route_write(io->context,plan->words[i].port,plan->words[i].after));
	if (!err) err=normalized(io->commit(io->context));
	aux=1;
	if (!err) err=normalized(io->auxiliary(io->context,true,&aux,&refs));
	if (!err && (aux!=1 || refs!=1)) err=-EIO;
	if (!err) err=clock_set(io,AE5_ASI,7);
	if (!err) err=clock_read(io,target,r->target_clocks);
	if (!err) err=words_check(io,plan,true,r->target_words);
	if (!err) err=normalized(io->auxiliary(io->context,false,&r->target_aux,&r->target_refs));
	if (!err && (r->target_aux!=1 || r->target_refs!=1)) err=-EIO;
	r->target_verified=!err; r->transition_error=err;
	/* Stop the auxiliary stream before restoring the original words/clocks.
	 * A failed transfer remains a failed recovery even if later reads match.
	 */
	recovery=ae5_path_restore(io,plan);
	r->restore_error=recovery; r->restore_verified=!recovery;
	return recovery ? recovery : err;
}

int ae5_path_activate(const struct ae5_path_io *io, ae5_u32 rate,
	const struct ae5_route_plan *plan, struct ae5_path_result *r)
{
	static const ae5_u32 normal[]={0x81,7,0xc7,0x1f101,0x1f101,0x14004,0x2000f};
	ae5_u32 target[AE5_REG_COUNT]={0x81,7,0xc7,0x1f101,0x1f101,0x14004,0x2000f};
	ae5_u32 scratch[16], aux=0, refs=0;
	unsigned int i,j;
	int err, recovery=0;
	if (!io || !r || !plan || !io->clock.transfer || !io->route_read || !io->route_write || !io->commit || !io->auxiliary ||
	    (rate!=96000 && rate!=192000 && rate!=384000) || plan->count!=rate/48000*2) return -EINVAL;
	*r=(struct ae5_path_result){.rate=rate};
	for (i=0;i<plan->count;i++) {
		if (plan->words[i].port>254 || !(plan->words[i].before&0x10000) || (plan->words[i].before&255)!=i ||
		    plan->words[i].after!=((plan->words[i].before&0xfffd00ff)|(0x4000+i*0x100))) return -EINVAL;
		for (j=0;j<i;j++) if (plan->words[i].port==plan->words[j].port) return -EINVAL;
	}
	if (rate!=96000) {
		target[AE5_MODE]=rate==384000 ? 0x80 : 0x81;
		target[AE5_CLOCK0]=target[AE5_CLOCK1]=rate==384000 ? 0x1f103 : 0x1f102;
		target[AE5_DIVIDER]=rate==384000 ? 0x31002 : 0x22003;
	}
	err=clock_read(io,normal,scratch);
	if (!err) err=words_check(io,plan,false,scratch);
	if (!err) err=normalized(io->auxiliary(io->context,false,&aux,&refs));
	if (!err && (aux || refs)) err=-EBUSY;
	if (err) return err;
	r->attempted=true;
	err=clock_set(io,AE5_ASI,4);
	if (!err) err=mode_set(io,target[AE5_MODE]);
	for (i=AE5_CLOCK0;i<AE5_REG_COUNT && !err;i++) err=clock_set(io,i,target[i]);
	for (i=0;i<plan->count && !err;i++) err=normalized(io->route_write(io->context,plan->words[i].port,plan->words[i].after));
	if (!err) err=normalized(io->commit(io->context));
	aux=1;
	if (!err) err=normalized(io->auxiliary(io->context,true,&aux,&refs));
	if (!err && (aux!=1 || refs!=1)) err=-EIO;
	if (!err) err=clock_set(io,AE5_ASI,7);
	if (!err) err=clock_read(io,target,r->target_clocks);
	if (!err) err=words_check(io,plan,true,r->target_words);
	if (!err) err=normalized(io->auxiliary(io->context,false,&r->target_aux,&r->target_refs));
	if (!err && (r->target_aux!=1 || r->target_refs!=1)) err=-EIO;
	r->target_verified=!err; r->transition_error=err;
	if (!err) return 0;
	/* Stop the auxiliary stream before restoring the original words/clocks.
	 * A failed transfer remains a failed recovery even if later reads match.
	 */
	recovery=ae5_path_restore(io,plan);
	r->restore_error=recovery; r->restore_verified=!recovery;
	return recovery ? recovery : err;
}
