/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "ae5_dac_volume.h"
struct fake {
 dac_u32 mmio[0x900/4],dac[32],original[32],selected;
 unsigned int calls,fail_at,after,positive,failed,writes,data_writes,drop_at,timeout;
 unsigned int live,mute_writes,permanent_failure;
 struct ae5_volume_result *policy;
};
static struct fake initial(void)
{
 struct fake f={0};
 f.mmio[0x20c/4]=0x800004; f.mmio[0x804/4]=0x48;
 f.dac[1]=0x80; f.dac[7]=0xa0; f.dac[15]=46; f.dac[16]=47;
 f.dac[17]=0xa1; f.dac[18]=0xb2; f.dac[19]=0xc3; f.dac[20]=0xd4;
 memcpy(f.original,f.dac,sizeof(f.dac)); return f;
}
static int fake_io(void *p,enum dac_operation op,dac_u32 offset,dac_u32 *v)
{
 struct fake *f=p; unsigned int i,found=0;
 int fail=(++f->calls==f->fail_at || (f->permanent_failure && f->calls>=f->fail_at)),error=f->positive?1:-EIO;
 if(fail) f->failed=1;
 if(fail && !f->after) return error;
 if(op==DAC_DELAY) assert(offset==100 || offset==20000);
 else if(op==DAC_MMIO_READ) { assert(offset<0x900 && !(offset&3)); *v=f->mmio[offset/4]; }
 else {
  assert(op==DAC_MMIO_WRITE); f->writes++;
  switch(offset) {
  case 0x210: assert(*v==0 || *v==0x7e || *v==0x5a); f->mmio[offset/4]=*v==0x5a?0xaa:*v; break;
  case 0x20c: assert((*v&~0x800000U)==3 || (*v&~0x800000U)==4 || (*v&~0x800000U)==5); f->mmio[offset/4]=*v; break;
  case 0x804: assert(*v==0x48); f->mmio[offset/4]=*v; break;
  case 0x204:
   assert(f->mmio[0x804/4]==0x48);
   if((f->mmio[0x20c/4]&7)==3) {
    for(i=0;i<DAC_READ_COUNT;i++) if(*v==ae5_dac_read_registers[i]) found=1;
    assert(found && *v<=255); f->selected=*v;
   } else {
    unsigned int reg=*v&255,value=*v>>8;
    assert((f->mmio[0x20c/4]&7)==5 && value<=255);
    if(f->policy) assert(ae5_volume_write_allowed(f->policy,*v,!f->live));
    assert(reg==7 || reg==15 || reg==16); /* gain writes fail immediately */
    if(reg==7) { f->mute_writes++; assert(value==f->original[7] || value==(f->original[7]|3)); }
    else if(!f->live) { assert(value>=f->original[reg]); assert((f->dac[7]&3)==3); }
    f->data_writes++;
    if(f->data_writes!=f->drop_at) f->dac[reg]=value;
   }
   f->mmio[offset/4]=*v;
   f->mmio[0x20c/4]=(f->mmio[0x20c/4]&7)|(f->timeout?0:0x800000); break;
  case 0x208:
   assert(*v==0xffff && (f->mmio[0x20c/4]&7)==3);
   f->mmio[offset/4]=0xffff0000U|f->dac[f->selected];
   f->mmio[0x20c/4]=3|(f->timeout?0:0x800000); break;
  default: assert(!"Unlisted register write");
  }
 }
 return fail?error:0;
}
static void same(const struct fake *f)
{
 assert(!memcmp(f->original,f->dac,sizeof(f->dac)));
 assert(f->mmio[0x210/4]==0 && (f->mmio[0x20c/4]&~0x800000U)==4 && f->mmio[0x804/4]==0x48);
}
int cycle_suite_main(void)
{
 struct fake f=initial(); struct dac_io io={fake_io,&f}; struct ae5_volume_result r={0};
 unsigned int count,idx,k,cases=0,data_writes;
 assert(!ae5_volume_cycle(&io,&r)); same(&f);
 assert(r.complete && r.applied_verified && r.restored && !r.needs_restore);
 assert(r.applied_regs[1]==0xa3 && r.applied_regs[2]==58 && r.applied_regs[3]==59);
 count=f.calls; data_writes=f.data_writes; cases++;
 for(k=0;k<4;k++) for(idx=1;idx<=count;idx++) {
  f=initial(); memset(&r,0,sizeof(r)); f.fail_at=idx; f.after=k&1; f.positive=k&2;
  assert(ae5_volume_cycle(&io,&r)<0 && f.failed);
  f.fail_at=0;
  if(r.needs_restore) assert(!ae5_volume_recover(&io,&r));
  assert(!r.needs_restore); same(&f); cases++;
 }
 for(idx=1;idx<=data_writes;idx++) {
  f=initial(); memset(&r,0,sizeof(r)); f.drop_at=idx;
  /* A redundant mute write may be dropped without changing the result. */
  (void)ae5_volume_cycle(&io,&r);
  f.drop_at=0; if(r.needs_restore) assert(!ae5_volume_recover(&io,&r));
  same(&f); cases++;
 }
 for(idx=0;idx<256;idx++) {
  f=initial(); memset(&r,0,sizeof(r)); f.dac[15]=idx; f.dac[16]=255-idx;
  f.dac[7]=0x80|(idx&3); memcpy(f.original,f.dac,sizeof(f.dac));
  assert(!ae5_volume_cycle(&io,&r)); same(&f); cases++;
 }
 f=initial(); memset(&r,0,sizeof(r)); f.timeout=1;
 assert(ae5_volume_cycle(&io,&r)==-ETIMEDOUT && !f.data_writes && f.calls<120);
 f.timeout=0; if(r.needs_restore) assert(!ae5_volume_recover(&io,&r)); same(&f); cases++;
 f=initial(); memset(&r,0,sizeof(r)); f.mmio[0x804/4]=0x49;
 assert(ae5_volume_cycle(&io,&r)==-EINVAL && !f.writes); cases++;
 f=initial(); memset(&r,0,sizeof(r)); r.needs_restore=1;
 assert(ae5_volume_cycle(&io,&r)==-EBUSY && !f.calls); cases++;
 printf("PASS %u DAC volume transaction, fault, restore and gain-preservation cases; %u operations in successful cycle\n",cases,count);
 return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
static int stream_sequence(struct dac_io *io,struct ae5_volume_result *r)
{
 int err=ae5_volume_hold(io,r,46);
 if(!err) err=ae5_volume_activate(io,r);
 if(!err) err=ae5_volume_quiet(io,r);
 if(!err) err=ae5_volume_recover(io,r);
 return err;
}
int main(void)
{
 struct fake f=initial(); struct dac_io io={fake_io,&f}; struct ae5_volume_result r={0};
 unsigned int calls,idx,k,cases=0;
 /* A stock DAC may have no attenuation, or already have a quieter level. */
 f.dac[15]=f.dac[16]=0; memcpy(f.original,f.dac,sizeof(f.dac));
 assert(!ae5_volume_hold(&io,&r,46)); assert(f.dac[7]==0xa3 && f.dac[15]==46 && f.dac[16]==46);
 assert(r.needs_restore && r.volume_pending && !r.bridge.needs_restore);
 assert(!ae5_volume_activate(&io,&r)); assert(f.dac[7]==0xa0 && f.dac[15]==46);
 assert(!ae5_volume_quiet(&io,&r)); assert(f.dac[7]==0xa3);
 assert(!ae5_volume_recover(&io,&r)); same(&f); calls=f.calls; cases++;
 for(k=0;k<4;k++) for(idx=1;idx<=calls;idx++) {
  f=initial(); f.dac[15]=f.dac[16]=0; memcpy(f.original,f.dac,sizeof(f.dac));
  memset(&r,0,sizeof(r)); f.fail_at=idx; f.after=k&1; f.positive=k&2;
  assert(stream_sequence(&io,&r)<0 && f.failed);
  f.fail_at=0; if(r.needs_restore) assert(!ae5_volume_recover(&io,&r));
  same(&f); cases++;
 }
 for(idx=0;idx<256;idx++) {
  f=initial(); f.dac[15]=idx; f.dac[16]=255-idx; memcpy(f.original,f.dac,sizeof(f.dac)); memset(&r,0,sizeof(r));
  assert(!stream_sequence(&io,&r)); same(&f); cases++;
 }
 f=initial(); memset(&r,0,sizeof(r)); assert(ae5_volume_hold(&io,&r,181)==-EINVAL && !f.calls); cases++;
 f=initial(); memset(&r,0,sizeof(r)); assert(!ae5_volume_hold(&io,&r,46));
 f.dac[15]=0; /* Simulated external interference must prevent activation. */
 assert(ae5_volume_activate(&io,&r)==-EIO && (f.dac[7]&3)==3);
 assert(!ae5_volume_recover(&io,&r)); same(&f); cases++;
 printf("PASS %u stream volume lifecycle/fault/attenuation/interference cases; %u operations in successful sequence\n",cases,calls);
 return 0;
}
