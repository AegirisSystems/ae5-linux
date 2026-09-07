# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the actual ALSA put callback with mutexes and simulated hardware."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

class NativeCallbacks(unittest.TestCase):
    def test_concurrent_writes_and_failure_state(self):
        source=(ROOT/'driver/ae5_native.c').read_text()
        callback=source[source.index('static int ae5_dac_volume_put('):source.index('static int ae5_direct_active_get(')]
        pre=r'''
#define _DEFAULT_SOURCE
#include <pthread.h>
#include <assert.h>
#include <stdbool.h>
#include <unistd.h>
#include "ae5_dac_volume.h"
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define WRITE_ONCE(a,b) ((a)=(b))
struct ca0132_spec { unsigned int ae5_direct_volume[2]; bool ae5_direct_volume_selected; pthread_mutex_t chipio_mutex; };
struct hda_codec { struct ca0132_spec *spec; };
struct snd_kcontrol { struct hda_codec *chip; };
struct snd_ctl_elem_value { struct { struct { long value[2]; } integer; } value; };
#define snd_kcontrol_chip(c) ((c)->chip)
static pthread_mutex_t state_mutex=PTHREAD_MUTEX_INITIALIZER,ca0132_mmio_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct ca0132_spec spec={{209,209},false,PTHREAD_MUTEX_INITIALIZER};
static struct hda_codec codec={&spec},*held_codec=&codec;
static struct snd_kcontrol control={&codec};
static struct ae5_volume_result volume;
static bool ready,enabled,stream_ready,needs_restore,reserved_stream;
static int calls,error,active;
static int dac_hardware(void *p,enum dac_operation op,dac_u32 reg,dac_u32 *value)
{ (void)p;(void)op;(void)reg;(void)value;assert(0);return 0; }
int ae5_volume_set_live(const struct dac_io *io,struct ae5_volume_result *r,const unsigned int pair[2])
{
 (void)io;calls++;assert(active++==0);usleep(25);
 if(!error){r->desired[0]=pair[0];r->desired[1]=pair[1];}
 assert(--active==0);return error;
}
'''
        post=r'''
static int set(long left,long right)
{
 struct snd_ctl_elem_value value={.value.integer.value={left,right}};
 return ae5_dac_volume_put(&control,&value);
}
static void *writer(void *arg)
{
 long base=(long)arg;int i;
 for(i=0;i<100;i++) assert(set(base+i%10,base+i%10)>=0);
 return NULL;
}
int main(void)
{
 pthread_t a,b;
 assert(set(-1,0)==-EINVAL && set(256,0)==-EINVAL && calls==0);
 assert(set(209,209)==1 && spec.ae5_direct_volume_selected && calls==0);
 assert(set(209,209)==0 && calls==0);
 assert(set(215,217)==1 && calls==0); /* inactive: preference only */
 ready=enabled=stream_ready=true;volume.volume_pending=volume.applied_verified=1;
 assert(set(219,221)==1 && calls==1 && volume.desired[0]==36 && volume.desired[1]==34);
 error=-EIO;
 assert(set(225,225)==-EIO && spec.ae5_direct_volume[0]==219 && stream_ready);
 volume.error=-EIO;
 assert(set(225,225)==-EIO && spec.ae5_direct_volume[0]==219);
 volume.error=error=0;
 stream_ready=false;needs_restore=true;
 assert(set(220,220)==-EBUSY && spec.ae5_direct_volume[0]==219);
 needs_restore=false;reserved_stream=true;
 assert(set(220,220)==-EBUSY);
 reserved_stream=false;stream_ready=true;
 assert(!pthread_create(&a,NULL,writer,(void *)190));
 assert(!pthread_create(&b,NULL,writer,(void *)210));
 pthread_join(a,NULL);pthread_join(b,NULL);
 assert(!active && volume.desired[0]==255-spec.ae5_direct_volume[0]);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            path=Path(d);(path/'test.c').write_text(pre+callback+post)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'driver'),str(path/'test.c'),'-o',str(path/'test')],check=True,capture_output=True,text=True)
            subprocess.run([str(path/'test')],check=True,capture_output=True,text=True,timeout=10)

if __name__=='__main__':unittest.main()
