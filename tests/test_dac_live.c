/* SPDX-License-Identifier: GPL-2.0-or-later */
#define main lifecycle_suite_main
#include "test_dac_lifecycle.c"
#undef main

static void playing(struct fake *f,struct ae5_volume_result *r)
{
 struct dac_io io={fake_io,f};
 *f=initial(); memset(r,0,sizeof(*r));
 f->dac[15]=f->dac[16]=0; memcpy(f->original,f->dac,sizeof(f->dac));
 assert(!ae5_volume_hold(&io,r,46)); assert(!ae5_volume_activate(&io,r));
 f->policy=r;
 f->live=1; f->calls=f->data_writes=f->writes=f->mute_writes=0;
}
static void unchanged_gain(const struct fake *f)
{
 unsigned int i;
 assert(f->dac[1]==f->original[1]);
 for(i=17;i<=20;i++) assert(f->dac[i]==f->original[i]);
}
static void closed(struct fake *f,struct ae5_volume_result *r)
{
 struct dac_io io={fake_io,f};
 f->fail_at=f->permanent_failure=f->drop_at=f->timeout=0;
 f->live=0;
 if(r->bridge.needs_restore) assert(!ae5_dac_bridge_restore(&io,&r->bridge));
 assert(!ae5_volume_quiet(&io,r)); assert(!ae5_volume_recover(&io,r)); same(f);
}
int main(void)
{
 struct fake f; struct ae5_volume_result r;
 struct dac_io io={fake_io,&f};
 unsigned int target[2]={42,40},count,writes,i,k,cases=0;
 setbuf(stdout,NULL);
 assert(!cycle_suite_main()); assert(!lifecycle_suite_main());
 playing(&f,&r);
 assert(!ae5_volume_set_live(&io,&r,target));
 assert(f.dac[15]==42 && f.dac[16]==40 && f.mute_writes==0);
 assert(r.desired[0]==42 && r.desired[1]==40 && r.live_verified && !r.live_active);
 count=f.calls; writes=f.data_writes; unchanged_gain(&f); closed(&f,&r); cases++;
 for(k=0;k<4;k++) for(i=1;i<=count;i++) {
  playing(&f,&r); f.fail_at=i; f.after=k&1; f.positive=k&2;
  assert(ae5_volume_set_live(&io,&r,target)<0 && f.failed);
  assert(!r.live_verified && !r.live_active);
  assert(r.desired[0]==46 && r.desired[1]==46);
  assert(f.dac[15]==46 && f.dac[16]==46);
  unchanged_gain(&f); closed(&f,&r); cases++;
 }
 for(i=1;i<=writes;i++) {
  playing(&f,&r); f.drop_at=i;
  assert(ae5_volume_set_live(&io,&r,target)<0);
  assert(f.dac[15]==46 && f.dac[16]==46);
  unchanged_gain(&f); closed(&f,&r); cases++;
 }
 /* Endpoints, asymmetric requests, both directions, and unchanged values. */
 playing(&f,&r);
 for(i=0;i<256;i++) {
  unsigned int pair[2]={i,255-i};
  assert(!ae5_volume_set_live(&io,&r,pair));
  assert(f.dac[15]==i && f.dac[16]==255-i && !f.mute_writes);
  assert(!ae5_volume_set_live(&io,&r,pair));
  unchanged_gain(&f); cases++;
 }
 closed(&f,&r);
 playing(&f,&r); target[0]=256;
 assert(ae5_volume_set_live(&io,&r,target)==-EINVAL && !f.calls); target[0]=42; cases++;
 f.dac[15]=45;
 assert(ae5_volume_set_live(&io,&r,target)==-EIO && !f.data_writes);
 assert(!r.applied_verified && r.error); closed(&f,&r); cases++;
 playing(&f,&r); f.mmio[0x804/4]=0x30;
 assert(ae5_volume_set_live(&io,&r,target)==-EBUSY && !f.writes);
 f.mmio[0x804/4]=0x48; closed(&f,&r); cases++;
 /* The same policy used by native MMIO rejects unowned gain/register data,
  * an unarmed transaction, and any ordinary write during active DMA. */
 playing(&f,&r);
 for(i=0;i<32;i++) for(k=0;k<256;k++)
  assert(!ae5_volume_write_allowed(&r,i|(k<<8),0));
 r.live_active=r.live_snapshot_valid=1;
 r.live_before[1]=0xa0;r.live_before[2]=r.live_before[3]=46;
 r.live_next[0]=r.live_next[1]=44;
 for(i=0;i<32;i++) for(k=0;k<256;k++) {
  int allowed=((i==15 || i==16) && (k==46 || k==44)) || (i==7 && k==0xa3);
  assert(ae5_volume_write_allowed(&r,i|(k<<8),0)==allowed);cases++;
 }
 r.live_active=0;closed(&f,&r);
 playing(&f,&r); f.fail_at=200; f.permanent_failure=1;
 assert(ae5_volume_set_live(&io,&r,target)<0);
 assert(!r.applied_verified && r.live_rollback_error && r.error);
 assert(ae5_volume_set_live(&io,&r,target)==-EBUSY);
 unchanged_gain(&f); closed(&f,&r); cases++;
 /* Explicit selection is retained on the next preparation, even when
  * the stock baseline is more attenuated. The original hold stays conservative. */
 f=initial(); memset(&r,0,sizeof(r)); f.live=1;
 target[0]=12; target[1]=18;
 assert(!ae5_volume_prepare(&io,&r,target,1));
 assert(f.dac[15]==12 && f.dac[16]==18);
 assert(!ae5_volume_activate(&io,&r)); closed(&f,&r); cases++;
 printf("PASS %u live DAC success/fault/rollback/range/persistence cases; no gain writes\n",cases);
 return 0;
}
