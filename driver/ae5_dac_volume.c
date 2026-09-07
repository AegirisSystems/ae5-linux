/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AE-5-only bridge transaction derived from offline CtxHDb.sys 0x4468,
 * 0x4378, 0x4928 and 0x49b0, reached by CtxHda.sys DAC read wrapper 0x2adac.
 * Read transactions send one address byte; the volume extension also sends two-byte writes.
 * 0x208=0xffff requests a read. The volume extension restricts data to registers 7, 15 and 16.
 * This is a bounded experiment, not a general I2C API or production driver.
 */
#include "ae5_dac_read.h"
const dac_u32 ae5_dac_read_registers[DAC_READ_COUNT]={1,7,15,16,17,18,19,20};
static const dac_u32 bridge_offsets[3]={0x210,0x20c,0x804};
static int transfer(const struct dac_io *io, enum dac_operation op,
                    dac_u32 reg, dac_u32 *value)
{
 int err=io->transfer(io->context,op,reg,value);
 if (err) return err<0 ? err : -EIO;
 return op==DAC_MMIO_READ && *value==0xffffffffU ? -EIO : 0;
}
static int read_reg(const struct dac_io *io,dac_u32 reg,dac_u32 *value)
{ return transfer(io,DAC_MMIO_READ,reg,value); }
static int write_reg(const struct dac_io *io,dac_u32 reg,dac_u32 value)
{
 dac_u32 ignored;
 int err=transfer(io,DAC_MMIO_WRITE,reg,&value);
 /* Posted-write flush, as in the vendor routine. */
 return err ? err : read_reg(io,reg,&ignored);
}
static int read_bridge(const struct dac_io *io,dac_u32 values[3])
{
 unsigned int i; int err;
 for (i=0;i<3;i++) { err=read_reg(io,bridge_offsets[i],&values[i]); if (err) return err; }
 return 0;
}
static int wait_ack(const struct dac_io *io)
{
 dac_u32 status=0,value=0; unsigned int i; int err,first=0;
 for (i=0;i<10;i++) {
  err=transfer(io,DAC_DELAY,100,&value);
  if (err) { first=err; break; }
  err=read_reg(io,0x20c,&status);
  if (err) { first=err; break; }
  if (status & 0x800000) break;
 }
 if (!first && !(status & 0x800000)) first=-ETIMEDOUT;
 /* Drain the three vendor response registers even on a completion timeout. */
 err=read_reg(io,0x860,&value); if (!first) first=err;
 err=read_reg(io,0x854,&value); if (!first) first=err;
 err=read_reg(io,0x840,&value); if (!first) first=err;
 return first;
}
static int byte_width(const struct dac_io *io)
{
 dac_u32 value; int err=read_reg(io,0x20c,&value);
 if (err) return err;
 /* Require the already-selected bridge protocol; never use legacy 0x200. */
 if (!(value&1)) return -EIO;
 err=write_reg(io,0x20c,(value&~6U)|2U);
 if (err) return err;
 err=read_reg(io,0x20c,&value);
 if (err) return err;
 return (value&7)==3 ? 0 : -EIO;
}
int ae5_dac_bridge_restore(const struct dac_io *io,struct dac_result *r)
{
 int err,first=0;
 if (!r->needs_restore) return 0;
 /* Restore selectors/control even if an earlier restore write failed. */
 err=write_reg(io,0x804,r->before[2]); if (!first) first=err;
 err=write_reg(io,0x20c,r->before[1]); if (!first) first=err;
 err=write_reg(io,0x210,r->before[0]); if (!first) first=err;
 err=read_reg(io,0x210,&r->after[0]); if (!first) first=err;
 err=read_bridge(io,r->after); if (!first) first=err;
 r->restore_error=first;
 /* Bit 23 is the completion STATUS, not writable bridge configuration.
  * Observed hardware can return 0x4 briefly after restoring 0x800004.
  * Preserve and compare every configuration bit; report raw values too.
  */
 r->restored=!first && r->before[0]==r->after[0] && r->before[2]==r->after[2] &&
             ((r->before[1]^r->after[1])&~0x800000U)==0;
 if (!r->restored && !r->restore_error) r->restore_error=-EIO;
 if (r->restored) r->needs_restore=0;
 return r->restore_error;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ae5_dac_volume.h"
int ae5_volume_write_allowed(const struct ae5_volume_result *r,dac_u32 encoded,int stopped)
{
 unsigned int reg=encoded&255,data=encoded>>8,i;
 if(!r || encoded>0xffff || !r->baseline_valid || !r->volume_pending) return 0;
 if(r->live_active) {
  if(!r->live_snapshot_valid) return 0;
  if(reg==15 || reg==16) {
   i=reg-15;
   return data==r->live_before[2+i] || data==r->live_next[i];
  }
  return reg==7 && data==(r->live_before[1]|3);
 }
 if(!stopped) return 0;
 if(reg==15 || reg==16) {
  i=reg-15;
  return data==r->before_regs[2+i] || data==r->desired[i];
 }
 if(reg==7) {
  if(data==r->before_regs[1] || data==(r->before_regs[1]|3)) return 1;
  return r->teardown_valid && (data==(r->teardown_regs[1]|3) ||
         data==((r->teardown_regs[1]&~3U)|(r->before_regs[1]&3U)));
 }
 return 0;
}
/* This experiment only adds 6dB attenuation under soft mute, then restores.
 * It cannot change gain/master trim, select outputs or start playback. */
static int volume_begin(const struct dac_io *io,struct ae5_volume_result *r)
{
 dac_u32 value; int err;
 r->bridge.needs_restore=1;
 err=write_reg(io,0x210,0x7e); if(err) return err;
 err=write_reg(io,0x210,0x5a); if(err) return err;
 err=read_reg(io,0x210,&value); if(err) return err;
 if((value&255)!=0xaa) return -EIO;
 err=write_reg(io,0x20c,r->bridge.before[1]|1); if(err) return err;
 return write_reg(io,0x804,0x48);
}
static int volume_read(const struct dac_io *io,dac_u32 reg,dac_u32 *value)
{
 int err=byte_width(io); if(err) return err;
 err=write_reg(io,0x204,reg); if(err) return err;
 err=wait_ack(io); if(err) return err;
 err=byte_width(io); if(err) return err;
 err=write_reg(io,0x208,0xffff); if(err) return err;
 err=wait_ack(io); if(err) return err;
 err=read_reg(io,0x208,value); if(!err) *value&=255;
 return err;
}
static int volume_snapshot(const struct dac_io *io,dac_u32 *values)
{
 unsigned int i; int err;
 for(i=0;i<DAC_READ_COUNT;i++) {
  err=volume_read(io,ae5_dac_read_registers[i],&values[i]); if(err) return err;
 }
 return 0;
}
static int volume_write(const struct dac_io *io,dac_u32 reg,dac_u32 value)
{
 dac_u32 control,seen,ignored=0; int err;
 if((reg!=7 && reg!=15 && reg!=16) || value>255) return -EPERM;
 err=read_reg(io,0x20c,&control); if(err) return err;
 if(!(control&1)) return -EIO;
 err=write_reg(io,0x20c,(control&~6U)|4); if(err) return err;
 err=read_reg(io,0x20c,&control); if(err) return err;
 if((control&7)!=5) return -EIO;
 /* Two bytes, address then value: Linux ca0113_mmio_command_set and
  * CtxHDb.sys RVA 0x45a0 both use this transport representation. */
 err=write_reg(io,0x204,reg|(value<<8)); if(err) return err;
 err=transfer(io,DAC_DELAY,20000,&ignored); if(err) return err;
 err=wait_ack(io); if(err) return err;
 err=volume_read(io,reg,&seen); if(err) return err;
 return seen==value ? 0 : -EIO;
}
static int volume_restore_values(const struct dac_io *io,struct ae5_volume_result *r)
{
 dac_u32 expected[DAC_READ_COUNT]; int err;
 if(!r->volume_pending) return 0;
 /* A stock output/filter control may have changed unowned DAC bits while
  * the stream was active. Preserve the current values during teardown.
  * Gain registers remain read-only; only our mute bits and attenuation
  * are restored. Activation still requires the entire original snapshot. */
 r->teardown_valid=0;
 err=volume_snapshot(io,r->teardown_regs); if(err) return err;
 r->teardown_valid=1;
 if(r->teardown_regs[0]!=r->before_regs[0] ||
    ((r->teardown_regs[1]^r->before_regs[1])&~3U) ||
    memcmp(r->teardown_regs+4,r->before_regs+4,4*sizeof(dac_u32)))
  r->external_controls_seen=1;
 memcpy(expected,r->teardown_regs,sizeof(expected));
 expected[1]=(expected[1]&~3U)|(r->before_regs[1]&3U);
 expected[2]=r->before_regs[2]; expected[3]=r->before_regs[3];
 /* Verify mute first. If either attenuation restoration fails, do not
  * clear the mute; retain the pending state for explicit recovery. */
 err=volume_write(io,7,r->teardown_regs[1]|3); if(err) return err;
 err=volume_write(io,15,r->before_regs[2]); if(err) return err;
 err=volume_write(io,16,r->before_regs[3]); if(err) return err;
 err=volume_write(io,7,expected[1]); if(err) return err;
 err=volume_snapshot(io,r->after_regs); if(err) return err;
 if(memcmp(expected,r->after_regs,sizeof(expected))) return -EIO;
 r->volume_pending=0;
 return 0;
}
int ae5_volume_recover(const struct dac_io *io,struct ae5_volume_result *r)
{
 int first=0,err;
 if(!io || !io->transfer || !r) return -EINVAL;
 if(!r->needs_restore) return 0;
 if(r->volume_pending) {
  first=volume_begin(io,r);
  if(!first) first=volume_restore_values(io,r);
 }
 err=ae5_dac_bridge_restore(io,&r->bridge); if(!first) first=err;
 r->restore_error=first;
 r->restored=!first && !r->volume_pending && !r->bridge.needs_restore;
 r->needs_restore=!r->restored;
 return first;
}
int ae5_volume_cycle(const struct dac_io *io,struct ae5_volume_result *r)
{
 dac_u32 expected[DAC_READ_COUNT]; unsigned int i; int err;
 if(!io || !io->transfer || !r) return -EINVAL;
 if(r->needs_restore) return -EBUSY;
 memset(r,0,sizeof(*r));
 err=read_bridge(io,r->bridge.before); if(err) goto out;
 if(r->bridge.before[0] || (r->bridge.before[1]&~0x800000U)!=4 || r->bridge.before[2]!=0x48) { err=-EINVAL; goto out; }
 r->needs_restore=1;
 err=volume_begin(io,r); if(err) goto cleanup;
 err=volume_snapshot(io,r->before_regs); if(err) goto cleanup;
 r->baseline_valid=1;
 memcpy(expected,r->before_regs,sizeof(expected)); expected[1]|=3;
 for(i=0;i<2;i++) {
  r->desired[i]=r->before_regs[2+i]>243 ? 255 : r->before_regs[2+i]+12;
  expected[2+i]=r->desired[i];
 }
 r->volume_pending=1;
 err=volume_write(io,7,expected[1]); if(err) goto cleanup;
 err=volume_write(io,15,r->desired[0]); if(err) goto cleanup;
 err=volume_write(io,16,r->desired[1]); if(err) goto cleanup;
 err=volume_snapshot(io,r->applied_regs); if(err) goto cleanup;
 if(memcmp(expected,r->applied_regs,sizeof(expected))) { err=-EIO; goto cleanup; }
 r->applied_verified=1;
cleanup:
 ae5_volume_recover(io,r);
out:
 r->error=err;
 r->complete=!err && r->applied_verified && r->restored;
 return err ? err : r->restore_error;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stream lifecycle extension. Caller stops DMA and serializes control access.
 * Hold applies attenuation while muted. Activate restores only the saved mute
 * bits, after the caller verifies route/clock setup. Quiet precedes route
 * teardown. Recover restores the saved DAC values after that teardown. */
int ae5_volume_prepare(const struct dac_io *io,struct ae5_volume_result *r,
                      const unsigned int steps[2],int explicit_level)
{
 unsigned int i; dac_u32 expected[DAC_READ_COUNT]; int err,restore;
 if(!io || !io->transfer || !r || !steps || steps[0]>255 || steps[1]>255) return -EINVAL;
 if(r->needs_restore) return -EBUSY;
 memset(r,0,sizeof(*r));
 err=read_bridge(io,r->bridge.before); if(err) goto out;
 if(r->bridge.before[0] || (r->bridge.before[1]&~0x800000U)!=4 || r->bridge.before[2]!=0x48) { err=-EINVAL; goto out; }
 r->needs_restore=1;
 err=volume_begin(io,r); if(err) goto failed;
 err=volume_snapshot(io,r->before_regs); if(err) goto failed;
 r->baseline_valid=1;
 memcpy(expected,r->before_regs,sizeof(expected)); expected[1]|=3;
 for(i=0;i<2;i++) {
  /* Preserve the original startup policy until the owner selects a level. */
  r->desired[i]=!explicit_level && r->before_regs[2+i]>steps[i] ? r->before_regs[2+i] : steps[i];
  expected[2+i]=r->desired[i];
 }
 r->volume_pending=1;
 err=volume_write(io,7,expected[1]); if(err) goto failed;
 err=volume_write(io,15,r->desired[0]); if(err) goto failed;
 err=volume_write(io,16,r->desired[1]); if(err) goto failed;
 err=volume_snapshot(io,r->applied_regs); if(err) goto failed;
 if(memcmp(expected,r->applied_regs,sizeof(expected))) { err=-EIO; goto failed; }
 err=ae5_dac_bridge_restore(io,&r->bridge); if(err) goto failed;
 r->applied_verified=1; r->complete=1;
 goto out;
failed:
 restore=ae5_volume_recover(io,r); if(restore) err=restore;
out:
 r->error=err; return err;
}
int ae5_volume_hold(const struct dac_io *io,struct ae5_volume_result *r,unsigned int steps)
{
 const unsigned int pair[2]={steps,steps};
 if(steps>180) return -EINVAL;
 return ae5_volume_prepare(io,r,pair,0);
}

/* Caller serializes against setup/cleanup and every stock bridge command.
 * No clock, route, DMA, amplifier gain, or mute changes on the success path.
 * Move at most 1 dB per channel per iteration. A failed write rolls both
 * channels back; if that cannot be verified, attempt soft mute and leave
 * the error latched for explicit recovery. Never claim rollback succeeded
 * merely because a write returned success.
 */
int ae5_volume_set_live(const struct dac_io *io,struct ae5_volume_result *r,
                        const unsigned int steps[2])
{
 dac_u32 expected[DAC_READ_COUNT],observed[DAC_READ_COUNT];
 unsigned int i; int err,restore,rollback=0,writes=0;
 if(!io || !io->transfer || !r || !steps || steps[0]>255 || steps[1]>255) return -EINVAL;
 if(!r->baseline_valid || !r->volume_pending || !r->applied_verified || r->error ||
    r->bridge.needs_restore || r->live_active) return -EBUSY;
 r->live_active=1; r->live_snapshot_valid=0; r->live_verified=0;
 r->live_error=0; r->live_rollback_error=0;
 /* Refuse an unexpected bridge owner/configuration before any write. */
 {
  dac_u32 bridge[3];
  err=read_bridge(io,bridge);
  if(err) goto done;
  if(bridge[0]!=r->bridge.before[0] || bridge[2]!=r->bridge.before[2] ||
     ((bridge[1]^r->bridge.before[1])&~0x800000U)) { err=-EBUSY; goto done; }
 }
 err=volume_begin(io,r); if(err) goto finish;
 err=volume_snapshot(io,r->live_before); if(err) goto finish;
 if(r->live_before[2]!=r->desired[0] || r->live_before[3]!=r->desired[1]) {
  err=-EIO; r->applied_verified=0; r->error=err; goto finish;
 }
 r->live_snapshot_valid=1;
 memcpy(expected,r->live_before,sizeof(expected));
 r->live_next[0]=expected[2]; r->live_next[1]=expected[3];
 while(expected[2]!=steps[0] || expected[3]!=steps[1]) {
  for(i=0;i<2;i++) {
   unsigned int old=expected[2+i],next=steps[i];
   if(next>old+2) next=old+2;
   if(old>next+2) next=old-2;
   r->live_next[i]=next;
   if(next==old) continue;
   writes=1;
   err=volume_write(io,15+i,next); if(err) goto undo;
   expected[2+i]=next;
  }
 }
 err=volume_snapshot(io,observed); if(err) goto undo;
 if(memcmp(expected,observed,sizeof(expected))) { err=-EIO; goto undo; }
 restore=ae5_dac_bridge_restore(io,&r->bridge);
 if(restore) { err=restore; goto undo; }
 memcpy(r->applied_regs,observed,sizeof(observed));
 r->desired[0]=steps[0]; r->desired[1]=steps[1];
 r->live_verified=1; r->live_updates++; r->applied_verified=1;
 goto done;
undo:
 if(writes) {
  rollback=volume_begin(io,r);
  if(!rollback) rollback=volume_write(io,15,r->live_before[2]);
  if(!rollback) rollback=volume_write(io,16,r->live_before[3]);
  if(!rollback) rollback=volume_snapshot(io,observed);
  if(!rollback && memcmp(observed,r->live_before,sizeof(observed))) rollback=-EIO;
  if(rollback) {
   /* This is a failure containment attempt, not a proven mute. */
   (void)volume_write(io,7,r->live_before[1]|3);
   r->applied_verified=0; r->error=rollback;
  } else {
   memcpy(r->applied_regs,observed,sizeof(observed));
  }
 }
finish:
 restore=ae5_dac_bridge_restore(io,&r->bridge);
 if(restore) { r->applied_verified=0; r->error=restore; if(!rollback) rollback=restore; }
 if(!err) err=restore;
done:
 r->live_error=err; r->live_rollback_error=rollback; r->live_active=0;
 return err;
}
static int volume_stream_mute(const struct dac_io *io,struct ae5_volume_result *r,int quiet)
{
 dac_u32 expected[DAC_READ_COUNT],observed[DAC_READ_COUNT]; int err,restore;
 if(!io || !io->transfer || !r || !r->baseline_valid || !r->volume_pending) return -EINVAL;
 if(r->bridge.needs_restore) return -EBUSY;
 /* A failed operation may only be followed by quiet/recovery, never active. */
 if(!quiet && (!r->applied_verified || r->error)) return -EIO;
 memcpy(expected,r->before_regs,sizeof(expected));
 expected[1]=r->before_regs[1]|(quiet?3:0);
 expected[2]=r->desired[0]; expected[3]=r->desired[1];
 r->applied_verified=0;
 err=volume_begin(io,r); if(err) goto out;
 if(quiet) {
  r->teardown_valid=0;
  err=volume_snapshot(io,r->teardown_regs); if(err) goto out;
  r->teardown_valid=1;
  if(r->teardown_regs[0]!=r->before_regs[0] ||
     ((r->teardown_regs[1]^r->before_regs[1])&~3U) ||
     memcmp(r->teardown_regs+4,r->before_regs+4,4*sizeof(dac_u32)))
   r->external_controls_seen=1;
  memcpy(expected,r->teardown_regs,sizeof(expected));
  expected[1]|=3;
 }
 /* Before removing mute, confirm the intended attenuation still holds. */
 if(!quiet) {
  err=volume_snapshot(io,observed); if(err) goto out;
  if(observed[1]!=(r->before_regs[1]|3) || observed[2]!=r->desired[0] || observed[3]!=r->desired[1] ||
     observed[0]!=r->before_regs[0] || memcmp(observed+4,r->before_regs+4,4*sizeof(dac_u32))) { err=-EIO; goto out; }
 }
 err=volume_write(io,7,expected[1]); if(err) goto out;
 err=volume_snapshot(io,r->applied_regs); if(err) goto out;
 if(memcmp(expected,r->applied_regs,sizeof(expected))) { err=-EIO; goto out; }
out:
 restore=ae5_dac_bridge_restore(io,&r->bridge); if(!err) err=restore;
 r->applied_verified=!err; r->error=err;
 return err;
}
int ae5_volume_activate(const struct dac_io *io,struct ae5_volume_result *r)
{ return volume_stream_mute(io,r,0); }
int ae5_volume_quiet(const struct dac_io *io,struct ae5_volume_result *r)
{ return volume_stream_mute(io,r,1); }
