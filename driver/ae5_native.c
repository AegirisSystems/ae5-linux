// SPDX-License-Identifier: GPL-2.0-or-later
/* Managed AE-5 PCM integration for one verified kernel and stock driver.
 * Controller prepare adapted from sound/hda/common/controller.c:
 * Copyright(c) 2004 Intel Corporation
 * Copyright (c) 2004 Takashi Iwai <tiwai@suse.de>
 *                    PeiSen Hou <pshou@realtek.com.tw>
 *
 * Loading only pins already-active power. Explicit enable requires every PCM
 * closed. The managed service enables and restores the front
 * PCM's format/prepare callbacks. Only the fixed 96 kHz WUH capture may coexist.
 * A self reference prevents unload until verified restoration and disable.
 * DAC mute/attenuation surrounds Direct setup and teardown; gain registers are never written.
 */
#include <linux/module.h>
#include <linux/cred.h>
#include <linux/user_namespace.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/utsname.h>
#include <linux/capability.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>
#include <sound/hda_codec.h>
#include "ae5_chipio.h"
#include "ae5_direct_route_cycle.h"
#include "ae5_path_cycle.h"
#include "hda_controller.h"
#include "ae5_dac_volume.h"

static struct device *held_dev;
static struct hda_codec *held_codec;
static struct module *held_owner;
static struct ca0132_spec *held_spec;
static bool ready, managed_mode, enabled, needs_restore;
static bool route_pending, stream_ready;
/* Root chooses one existing ALSA-authorized playback owner at module load.
 * This grants no device-file access and leaves all control/recovery commands
 * privileged. The default retains root-only playback. */
static unsigned int playback_uid;
module_param(playback_uid,uint,0600);
MODULE_PARM_DESC(playback_uid,"Existing ALSA-authorized Linux UID allowed to open the Direct playback PCM");
static bool playback_owner_allowed(void)
{
 return capable(CAP_SYS_ADMIN) || uid_eq(current_euid(),make_kuid(&init_user_ns,playback_uid));
}
static struct ae5_volume_result volume;
static void __iomem *mmio;
static struct hdac_stream *reserved_stream;
static unsigned int reserved_tag, observed_caps, observed_captures;
static ae5_u32 initial[AE5_REG_COUNT];
static struct ae5_transport_cycle_result outcome;
static struct ae5_path_result path_result;
static DEFINE_MUTEX(state_mutex);
static unsigned int prepares, restores, starts, stops, recovery_attempts;
static unsigned int logical_rate, wire_format;
static int last_error, prepare_error, cleanup_error;
static struct snd_pcm *front_pcm;
static struct snd_pcm_substream *front_substream;
static struct hda_pcm_stream *front_info;
static const struct snd_pcm_ops *original_pcm_ops;
static struct snd_pcm_ops experimental_pcm_ops;
struct saved_hinfo { struct hda_pcm_stream *pointer; struct hda_pcm_stream value; };
static struct saved_hinfo saved[32];
static unsigned int saved_count;
static char codec_name[32]="hdaudioC4D1";
module_param_string(codec_name,codec_name,sizeof(codec_name),0600);
MODULE_PARM_DESC(codec_name,"Codec discovered by the service for the configured PCI device");
module_param(managed_mode,bool,0600);
MODULE_PARM_DESC(managed_mode,"Explicit activation by the installed AE-5 service");

static bool reserved_dma_stopped(void);
static int hardware_transfer(void *context, enum ae5_cycle_op op,
			     enum ae5_cycle_reg reg, ae5_u32 *value)
{
	struct hda_codec *codec = context;
	static const unsigned int addresses[] = {0x189000,0x189004,0x189024,0x189028};
	int err;
	if (reg < AE5_MODE || reg >= AE5_REG_COUNT) return -EINVAL;
	if (op == AE5_CYCLE_WRITE) {
		if (!reserved_dma_stopped()) return -EBUSY;
		/* Closed whitelist: amplifier/DAC/PLL data writes are impossible. */
		if (reg == AE5_PLL43) return -EPERM;
		if (reg == AE5_MODE) {
			if (*value != 0x80 && *value != 0x81) return -EINVAL;
			return snd_hda_codec_write(codec, 0x15, 0, 0x725, *value);
		}
		if (reg == AE5_ASI) {
			if (*value != 4 && *value != 7) return -EINVAL;
			err = ae5_chipio_send(codec, VENDOR_CHIPIO_STATUS, 0);
			if (err) return err;
			return snd_hda_codec_write(codec, 0x15, 0, 0x710, (*value << 5) | 0x17);
		}
		if ((reg == AE5_CLOCK0 || reg == AE5_CLOCK1) && *value != 0x1f101 && *value != 0x1f102 && *value != 0x1f103) return -EPERM;
		if (reg == AE5_DIVIDER && *value != 0x14004 && *value != 0x22003 && *value != 0x31002) return -EPERM;
		if (reg == AE5_CLOCK3 && *value != 0x2000f) return -EPERM;
		err = ae5_chipio_write_address(codec, addresses[reg - AE5_CLOCK0]);
		return err ? err : ae5_chipio_write_data(codec, *value);
	}
	if (op != AE5_CYCLE_READ) return -EINVAL;
	switch (reg) {
	case AE5_MODE:
		*value = snd_hda_codec_read(codec, 0x15, 0, 0xf25, 0);
		break;
	case AE5_ASI:
		*value = snd_hda_codec_read(codec, 0x15, 0, 0xf10, 0x17);
		break;
	case AE5_PLL43:
		err = snd_hda_codec_write(codec, 0x15, 0, 0x70d, 0x43);
		if (err) return err;
		*value = snd_hda_codec_read(codec, 0x15, 0, 0xf0c, 0);
		break;
	default:
		err = ae5_chipio_write_address(codec, addresses[reg - AE5_CLOCK0]);
		if (err) return err;
		err = ae5_chipio_read_data(codec, value);
		if (err) return err;
	}
	if (*value == ~0U) {
		held_spec->curr_chip_addx = ~0U;
		return -EIO;
	}
	return 0;
}

static int transport_route_read(void *context, bool exram, ae5_u32 address,
				ae5_u32 *value)
{
	struct hda_codec *codec = context;
	int err;
	if (exram) {
		err = snd_hda_codec_write(codec, 0x15, 0, 0x70d, address & 0xff);
		if (err) return err;
		err = snd_hda_codec_write(codec, 0x15, 0, 0x70e, address >> 8);
		if (err) return err;
		*value = snd_hda_codec_read(codec, 0x15, 0, 0xf07, 0);
		return *value > 0xff ? -EIO : 0;
	}
	err = ae5_chipio_write_address(codec, address);
	if (err) return err;
	err = ae5_chipio_read_data(codec, value);
	if (!err && *value == ~0U) {
		held_spec->curr_chip_addx = ~0U;
		return -EIO;
	}
	return err;
}

static int transport_peer_snapshot(struct hda_codec *codec,
				    struct ae5_transport_snapshot *out)
{
	static const unsigned int streams[] = {0x0c, 0x14, 0x18};
	unsigned int s, i, j;
	ae5_u32 port, last, next, seen[16];
	int err;
	for (s = 0; s < 3; s++) {
		err = transport_route_read(codec, true, 0x734 + 10 * streams[s],
					   &out->peers[s].references);
		if (err) return err;
		err = transport_route_read(codec, true, 0x1578 + streams[s], &port);
		if (err) return err;
		err = transport_route_read(codec, true, 0x159d + streams[s], &last);
		if (err) return err;
		out->peers[s].count = 0;
		if (port == 0xff) {
			if (out->peers[s].references) return -EINVAL;
			continue;
		}
		for (i = 0; i < 16; i++) {
			for (j = 0; j < i; j++) if (seen[j] == port) return -ELOOP;
			seen[i] = port;
			err = transport_route_read(codec, false, 0x190000 + 4 * port,
						   &out->peers[s].words[i]);
			if (err) return err;
			err = transport_route_read(codec, true, 0x1478 + port, &next);
			if (err) return err;
			out->peers[s].count++;
			if (next == 0xff) {
				if (port != last) return -EINVAL;
				break;
			}
			if (port == last) return -EINVAL;
			port = next;
		}
		if (i == 16) return -E2BIG;
	}
	return 0;
}

/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-License-Identifier: GPL-2.0-only */
static bool reserved_dma_stopped(void)
{
	return reserved_stream && reserved_stream->opened && !reserved_stream->running &&
		!(snd_hdac_stream_readb(reserved_stream, SD_CTL) & SD_CTL_DMA_START);
}
static int direct_param_get(struct hda_codec *codec, ae5_u32 id, ae5_u32 *value)
{
	*value = snd_hda_codec_read(codec, 0x15, 0, 0xf10, id);
	return *value > 255 ? -EIO : 0;
}
static int direct_param_set(struct hda_codec *codec, ae5_u32 id, ae5_u32 value)
{
	unsigned int i;
	ae5_u32 observed;
	int err;
	if (id < 0x18 || id > 0x1e || value > 255) return -EPERM;
	if (value < 8) err = snd_hda_codec_write(codec, 0x15, 0, 0x710, (value << 5) | id);
	else {
		err = ae5_chipio_send(codec, 0xf01, 0);
		if (!err) err = snd_hda_codec_write(codec, 0x15, 0, 0x717, id);
		if (!err) err = snd_hda_codec_write(codec, 0x15, 0, 0x718, value);
	}
	if (err) return err;
	for (i = 0; i < 21; i++) {
		err = direct_param_get(codec, id, &observed);
		if (err || observed == value) return err;
		if (i != 20) msleep(5);
	}
	return -EIO;
}
static int direct_param_write(void *context, ae5_u32 id, ae5_u32 value)
{
	struct hda_codec *codec = context;
	ae5_u32 selected;
	int err;
	if (!reserved_dma_stopped()) return -EBUSY;
	/* Selectors themselves do not alter a stream. Configuration writes below
	 * are constrained to the three measured streams and connection points.
	 */
	if (id == 0x18 || id == 0x1d) return direct_param_set(codec, id, value);
	if (id >= 0x19 && id <= 0x1c) {
		err = direct_param_get(codec, 0x18, &selected);
		if (err) return err;
		if (selected != 5 && selected != 0x14 && selected != 0x18) return -EPERM;
		if (id == 0x19 && !((selected == 5 && value == 0x43) ||
		    (selected == 0x14 && (value == 0x43 || value == 0x48)) ||
		    (selected == 0x18 && value == 9))) return -EPERM;
		if (id == 0x1a && !((selected != 0x18 && (value == 0 || value == 0xd0)) ||
		    (selected == 0x18 && value == 0xd0))) return -EPERM;
		if (id == 0x1b && !((selected == 5 && value <= 4) ||
		    (selected == 0x14 && value == 2) || (selected == 0x18 && value == 6))) return -EPERM;
		if (id == 0x1c && !(selected == 0x18 && value <= 1)) return -EPERM;
	} else if (id == 0x1e) {
		err = direct_param_get(codec, 0x1d, &selected);
		if (err) return err;
		if (!((selected == 0 && value == 0xb) || (selected == 0x43 && value <= 0xf) ||
		      (selected == 0xd0 && (value == 0xb || value == 0xe)))) return -EPERM;
	} else return -EPERM;
	return direct_param_set(codec, id, value);
}
static int direct_parameter_capture(struct hda_codec *codec, struct ae5_transport_snapshot *out)
{
	static const ae5_u32 streams[] = {5,0x14,0x18}, connections[] = {0,0x43,0xd0};
	unsigned int i, j;
	int err, restore, next;
	err = direct_param_get(codec, 0x18, &out->selectors[0]);
	if (!err) err = direct_param_get(codec, 0x1d, &out->selectors[1]);
	if (err) return err;
	for (i = 0; i < 3 && !err; i++) {
		err = direct_param_set(codec, 0x18, streams[i]);
		for (j = 0; j < 4 && !err; j++)
			err = direct_param_get(codec, 0x19 + j, &out->parameters[i][j]);
	}
	for (i = 0; i < 3 && !err; i++) {
		err = direct_param_set(codec, 0x1d, connections[i]);
		if (!err) err = direct_param_get(codec, 0x1e, &out->rates[i]);
	}
	restore = direct_param_set(codec, 0x18, out->selectors[0]);
	next = direct_param_set(codec, 0x1d, out->selectors[1]);
	if (!restore) restore = next;
	return restore ? restore : err;
}
static int transport_snapshot(void *context, ae5_u32 rate, bool attached,
	struct ae5_transport_snapshot *out)
{
	struct hda_codec *codec = context;
	struct ae5_cycle_io clock_io = {hardware_transfer, codec};
	struct ae5_route_reader reader = {transport_route_read, codec};
	unsigned int attempt;
	int err;
	if (attached && !reserved_dma_stopped()) return -EBUSY;
	*out = (struct ae5_transport_snapshot){0};
	out->converter = snd_hda_codec_read(codec, 2, 0, AC_VERB_GET_CONV, 0);
	out->format = snd_hda_codec_read(codec, 2, 0, AC_VERB_GET_STREAM_FORMAT, 0);
	if (out->converter == ~0U || out->format == ~0U) return -EIO;
	err = ae5_clock_baseline(&clock_io, out->clocks);
	if (err) return err;
	for (attempt = 0; attempt < 21; attempt++) {
		err = transport_route_read(codec, true, 0x766, &out->references);
		if (err) return err;
		err = ae5_route_capture(&reader, &out->raw);
		out->route_error = err;
		if (!err && attached) out->route_error = ae5_route_plan(&reader, rate, &out->route);
		out->peer_error = transport_peer_snapshot(codec, out);
		if (err && err != -ELOOP && err != -E2BIG) return err;
		if (!err && !out->route_error && !out->peer_error &&
		    out->references == (attached ? 1 : 0) &&
		    (out->peers[0].references == (attached ? 2 : 1) ||
		     out->peers[0].references == (attached ? 3 : 2)) && out->peers[1].references == 0 &&
		    out->peers[2].references == (attached ? 0 : 1)) break;
		if (attempt != 20) msleep(25);
	}
	err = direct_parameter_capture(codec, out);
	/* Strict semantic errors are retained for the transaction validator.
	 * I/O errors still propagate; neither diagnostic evidence nor an empty
	 * plan can authorize success or recovery.
	 */
	if (!err && out->raw.error) err = out->raw.error;
	if (!err && out->peer_error) err = out->peer_error;
	return err;
}
static int transport_format_write(void *context, ae5_u32 format)
{
	if (!reserved_dma_stopped()) return -EBUSY;
	if (format != 0 && format != 0x841 && format != 0x1841 && format != 0x1843) return -EPERM;
	return snd_hda_codec_write(context, 2, 0, AC_VERB_SET_STREAM_FORMAT, format);
}
static int transport_tag_write(void *context, bool attached)
{
	if (!reserved_dma_stopped()) return -EBUSY;
	if (!reserved_tag || reserved_tag > 15) return -EINVAL;
	return snd_hda_codec_write(context, 2, 0, AC_VERB_SET_CHANNEL_STREAMID, attached ? reserved_tag << 4 : 0);
}

/* SPDX-License-Identifier: GPL-2.0-only */
static const struct ae5_route_plan *active_path_plan;
static int path_route_read(void *context, ae5_u32 port, ae5_u32 *word)
{
	if (port > 254) return -EINVAL;
	return transport_route_read(context, false, 0x190000 + 4 * port, word);
}
static int path_route_write(void *context, ae5_u32 port, ae5_u32 word)
{
	unsigned int i;
	int err;
	if (!reserved_dma_stopped() || !active_path_plan) return -EBUSY;
	for (i=0;i<active_path_plan->count;i++)
		if (active_path_plan->words[i].port==port &&
		    (active_path_plan->words[i].before==word || active_path_plan->words[i].after==word)) break;
	if (i==active_path_plan->count) return -EPERM;
	err=ae5_chipio_write_address(context,0x190000+4*port);
	return err ? err : ae5_chipio_write_data(context,word);
}
static int path_commit(void *context)
{
	int err;
	if (!reserved_dma_stopped() || !active_path_plan) return -EBUSY;
	err=ae5_chipio_write_address(context,0x19042c);
	return err ? err : ae5_chipio_write_data(context,1);
}
static int path_auxiliary(void *context, bool write, ae5_u32 *enabled, ae5_u32 *refs)
{
	unsigned int attempt;
	int err;
	err=direct_param_set(context,0x18,0x14);
	if (err) return err;
	if (write) {
		if (*enabled>1) return -EPERM;
		if (!reserved_dma_stopped()) return -EBUSY;
		err=direct_param_set(context,0x1c,*enabled);
		if (err) return err;
	}
	for (attempt=0;attempt<21;attempt++) {
		err=direct_param_get(context,0x1c,enabled);
		if (!err) err=transport_route_read(context,true,0x7fc,refs);
		if (err || *enabled==*refs) return err;
		if (attempt!=20) msleep(5);
	}
	return -EIO;
}


static struct ae5_transport_cycle_io route_io(void)
{
	return (struct ae5_transport_cycle_io){transport_snapshot,transport_format_write,
		transport_tag_write,direct_param_write,held_codec,reserved_tag,NULL};
}
static struct ae5_path_io path_io(void)
{
	return (struct ae5_path_io){{hardware_transfer,held_codec},path_route_read,
		path_route_write,path_commit,path_auxiliary,held_codec};
}
static int dac_hardware(void *context,enum dac_operation op,dac_u32 offset,dac_u32 *value)
{
 unsigned int i; bool allowed=false;
 if (op==DAC_DELAY) {
  if (offset==20000) { msleep(20); return 0; }
  if (offset!=100) return -EPERM;
  usleep_range(100,150); return 0;
 }
 if (op==DAC_MMIO_READ) {
  switch (offset) {
  case 0x210: case 0x20c: case 0x804: case 0x204: case 0x208:
  case 0x860: case 0x854: case 0x840: *value=readl(mmio+offset); return 0;
  default: return -EPERM;
  }
 }
 if (op!=DAC_MMIO_WRITE) return -EPERM;
 switch (offset) {
 case 0x210: allowed=*value==0 || *value==0x7e || *value==0x5a; break;
 case 0x20c: allowed=*value==0x800004 || *value==0x800005 ||
                       *value==0x800003 || *value==3 || *value==4 || *value==5; break;
 case 0x804: allowed=*value==0x48; break;
 case 0x204:
  if (readl(mmio+0x804)!=0x48) break;
  if ((readl(mmio+0x20c)&7)==3) {
   for (i=0;i<DAC_READ_COUNT;i++) if (*value==ae5_dac_read_registers[i]) allowed=true;
  } else if ((readl(mmio+0x20c)&7)==5) {
   allowed=ae5_volume_write_allowed(&volume,*value,reserved_dma_stopped());
  }
  break;
 case 0x208:
  allowed=*value==0xffff && (readl(mmio+0x20c)&7)==3 && readl(mmio+0x804)==0x48;
  break;
 }
 if (!allowed) return -EPERM;
 writel(*value,mmio+offset); return 0;
}

/* state_mutex and ChipIO mutex are held; DMA must have stopped first. */
static int restore_path(void)
{
 struct ae5_transport_cycle_io rio=route_io();
 struct ae5_path_io pio=path_io();
 struct dac_io dio={dac_hardware,NULL}; int err;
 if(!needs_restore) return 0;
 WRITE_ONCE(stream_ready,false);
 if(!reserved_dma_stopped()) return -EBUSY;
 if(volume.bridge.needs_restore) {
  err=ae5_dac_bridge_restore(&dio,&volume.bridge); if(err) goto failed;
 }
 if(volume.volume_pending) {
  err=ae5_volume_quiet(&dio,&volume); if(err) goto failed;
 }
 if(active_path_plan) {
  err=ae5_path_restore(&pio,active_path_plan); if(err) goto failed;
  active_path_plan=NULL;
 }
 if(route_pending) {
  err=ae5_direct_route_restore(&rio,&outcome.before,&outcome.restored); if(err) goto failed;
  route_pending=false;
 }
 if(volume.needs_restore) {
  err=ae5_volume_recover(&dio,&volume); if(err) goto failed;
 }
 outcome.restore_error=0; outcome.restore_verified=true;
 needs_restore=false; cleanup_error=0; restores++; return 0;
failed:
 outcome.restore_verified=false; outcome.restore_error=err; return err;
}

static struct snd_pcm *wuh_pcm;
static struct snd_pcm_substream *wuh_substream;
static struct hda_pcm_stream *wuh_info;
static const struct snd_pcm_ops *wuh_original_ops;
static struct snd_pcm_ops wuh_experimental_ops;
static struct hdac_stream *wuh_stream;
static bool wuh_opened, wuh_preparing, wuh_prepared;
static unsigned int wuh_prepare_attempts;

static bool wuh_running(void)
{
    return wuh_opened && wuh_prepared && !wuh_preparing && wuh_stream &&
        wuh_stream->opened && wuh_stream->running &&
        (snd_hdac_stream_readb(wuh_stream, SD_CTL) & SD_CTL_DMA_START);
}
static int wuh_open(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
    struct snd_pcm_substream *substream)
{
    int err=0;
    mutex_lock(&state_mutex);
    if (!enabled || !managed_mode || !capable(CAP_SYS_ADMIN)) err=-EPERM;
    else if (substream!=wuh_substream || wuh_opened || reserved_stream || needs_restore) err=-EBUSY;
    else {
        wuh_opened=true; wuh_prepared=false; wuh_preparing=false;
        wuh_prepare_attempts=0;
        wuh_stream=azx_stream(get_azx_dev(substream));
    }
    mutex_unlock(&state_mutex);
    return err;
}
static int wuh_close(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
    struct snd_pcm_substream *substream)
{
    mutex_lock(&state_mutex);
    wuh_opened=false; wuh_prepared=false; wuh_preparing=false; wuh_stream=NULL;
    mutex_unlock(&state_mutex);
    return 0;
}
static int wuh_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *r=substream->runtime;
    int err;
    /* Reject before stock prepare can purify converter caches. A recorder
     * must prepare exactly once, before any Direct playback handle opens.
     * In particular, automatic XRUN recovery cannot touch the Direct path.
     */
    mutex_lock(&state_mutex);
    if (!enabled || !wuh_opened || substream!=wuh_substream ||
        !managed_mode || !capable(CAP_SYS_ADMIN)) err=-EPERM;
    else if (reserved_stream || needs_restore || wuh_preparing || wuh_prepare_attempts) err=-EBUSY;
    else if (r->rate!=96000 || r->channels!=2 || r->format!=SNDRV_PCM_FORMAT_S32_LE) err=-EINVAL;
    else {
        wuh_preparing=true; wuh_prepared=false; wuh_prepare_attempts++;
        err=0;
    }
    mutex_unlock(&state_mutex);
    if (err) return err;
    err=wuh_original_ops->prepare(substream);
    mutex_lock(&state_mutex);
    wuh_preparing=false; wuh_prepared=err==0;
    mutex_unlock(&state_mutex);
    return err;
}

static int reject_other_open(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
	struct snd_pcm_substream *substream)
{
	return -EBUSY;
}
static int direct_open(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
	struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime=substream->runtime;
	struct hdac_stream *s=azx_stream(get_azx_dev(substream));
	int err=0;
	/* The stock open callback holds chip->open_mutex. The enable reference
	 * protects every installed function pointer until all handles are closed.
	 */
	mutex_lock(&state_mutex);
	if (!enabled || !managed_mode || !playback_owner_allowed()) err=-EPERM;
	else if (needs_restore || reserved_stream || substream!=front_substream || (wuh_opened && !wuh_running())) err=-EBUSY;
	else {
		reserved_stream=s;
		runtime->hw.info &= ~(SNDRV_PCM_INFO_HAS_WALL_CLOCK |
			SNDRV_PCM_INFO_HAS_LINK_ATIME | SNDRV_PCM_INFO_HAS_LINK_SYNCHRONIZED_ATIME);
		/* No runtime resume, pause or linked-device experiment in this stage. */
		runtime->hw.info &= ~(SNDRV_PCM_INFO_RESUME | SNDRV_PCM_INFO_PAUSE);
	}
	mutex_unlock(&state_mutex);
	return err;
}
static int direct_close(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
	struct snd_pcm_substream *substream)
{
	mutex_lock(&state_mutex);
	/* Stock close has released the controller reservation. If cleanup failed,
	 * preserve the route plan and block all future opens until explicit repair.
	 */
	reserved_stream=NULL;
	mutex_unlock(&state_mutex);
	return 0;
}
static int direct_cleanup(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
 struct snd_pcm_substream *substream)
{
 int err;
 down_write(&held_codec->card->controls_rwsem);
 mutex_lock(&state_mutex); mutex_lock(&held_spec->chipio_mutex);
 mutex_lock(&ca0132_mmio_mutex);
 err=restore_path(); cleanup_error=err;
 mutex_unlock(&ca0132_mmio_mutex);
 mutex_unlock(&held_spec->chipio_mutex); mutex_unlock(&state_mutex);
 up_write(&held_codec->card->controls_rwsem); return err;
}
static int direct_prepare(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
 unsigned int tag, unsigned int format, struct snd_pcm_substream *substream)
{
 struct ae5_transport_cycle_io rio;
 struct ae5_path_io pio=path_io(); struct dac_io dio={dac_hardware,NULL};
 struct ae5_transport_snapshot *target;
 unsigned int rate=substream->runtime->rate; int err,repair;
 unsigned int steps[2];
 down_write(&held_codec->card->controls_rwsem);
 mutex_lock(&state_mutex); mutex_lock(&held_spec->chipio_mutex);
 mutex_lock(&ca0132_mmio_mutex);
 prepares++; WRITE_ONCE(stream_ready,false);
 err=restore_path(); if(err) goto out;
 if(!enabled || (wuh_opened && !wuh_running()) || !reserved_dma_stopped() || tag<1 || tag>15 ||
    (rate!=96000 && rate!=192000 && rate!=384000) ||
    format!=(rate==96000?0x841:rate==192000?0x1841:0x1843)) { err=-EINVAL; goto out; }
 reserved_tag=tag; logical_rate=rate; wire_format=format;
 rio=route_io(); path_result=(struct ae5_path_result){0}; outcome=(struct ae5_transport_cycle_result){0};
 /* First activation retains the deployment's conservative -23 dB policy.
  * Explicit ALSA selections survive stream reopen without touching gain. */
 steps[0]=255-held_spec->ae5_direct_volume[0];
 steps[1]=255-held_spec->ae5_direct_volume[1];
 err=ae5_volume_prepare(&dio,&volume,steps,held_spec->ae5_direct_volume_selected);
 needs_restore=volume.needs_restore;
 if(err) goto out;
 err=ae5_direct_route_activate(&rio,rate,&outcome);
 route_pending=outcome.attempted && !outcome.restore_verified;
 needs_restore=route_pending || volume.needs_restore;
 if(err) goto out;
 target=rate==96000?&outcome.at96:rate==192000?&outcome.at192:&outcome.at384;
 active_path_plan=&target->route;
 err=ae5_path_activate(&pio,rate,active_path_plan,&path_result);
 if(err) {
  if(!path_result.attempted || path_result.restore_verified) active_path_plan=NULL;
  goto out;
 }
 err=ae5_volume_activate(&dio,&volume);
 if(!err) {
  held_spec->ae5_direct_volume[0]=255-volume.desired[0];
  held_spec->ae5_direct_volume[1]=255-volume.desired[1];
  WRITE_ONCE(stream_ready,true);
 }
out:
 if(err && needs_restore) { repair=restore_path(); if(repair) err=repair; }
 prepare_error=err;
 mutex_unlock(&ca0132_mmio_mutex);
 mutex_unlock(&held_spec->chipio_mutex); mutex_unlock(&state_mutex);
 up_write(&held_codec->card->controls_rwsem); return err;
}
static int pcm_prepare(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm=snd_pcm_substream_chip(substream);
	struct azx *chip=apcm->chip;
	struct azx_dev *azx_dev=get_azx_dev(substream);
	struct hdac_stream *s=azx_stream(azx_dev);
	struct snd_pcm_runtime *r=substream->runtime;
	unsigned int bits, format, tag;
	int err;
#ifdef CONFIG_SND_HDA_DSP_LOADER
	guard(snd_hdac_dsp_lock)(s);
#endif
	if (snd_hdac_stream_is_locked(s)) return -EBUSY;
	s->prepared=0;
	if (r->channels!=2 || r->format!=SNDRV_PCM_FORMAT_S32_LE ||
	    (r->rate!=96000 && r->rate!=192000 && r->rate!=384000)) return -EINVAL;
	snd_hdac_stream_reset(s);
	bits=snd_hdac_stream_format_bits(r->format,SNDRV_PCM_SUBFORMAT_STD,32);
	format=snd_hdac_spdif_stream_format(r->rate==384000 ? 4 : 2,bits,
		r->rate==384000 ? 192000 : r->rate,0);
	if (format!=(r->rate==96000 ? 0x841 : r->rate==192000 ? 0x1841 : 0x1843)) return -EINVAL;
	err=snd_hdac_stream_set_params(s,format);
	if (err<0) return err;
	err=snd_hdac_stream_setup(s,false);
	if (err<0) return err;
	tag=s->stream_tag;
	if ((chip->driver_caps & AZX_DCAPS_CTX_WORKAROUND) && tag>chip->capture_streams)
		tag-=chip->capture_streams;
	/* The temporary path owns converter programming, with no stock converter
	 * cache changes. Do not run purify_inactive_streams on that untracked path.
	 * Keep the stock bus prepare lock for codec-level serialization.
	 */
	mutex_lock(&apcm->codec->bus->prepare_mutex);
	err=direct_prepare(front_info,apcm->codec,tag,s->format_val,substream);
	mutex_unlock(&apcm->codec->bus->prepare_mutex);
	if (err<0) return err;
	s->prepared=1;
	return 0;
}
static int pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	int err;
	if (cmd==SNDRV_PCM_TRIGGER_START && !READ_ONCE(stream_ready)) return -EIO;
	err=original_pcm_ops->trigger(substream,cmd);
	if (!err) {
		if (cmd==SNDRV_PCM_TRIGGER_START) starts++;
		if (cmd==SNDRV_PCM_TRIGGER_STOP) stops++;
	}
	return err;
}
/* Called only with device/user, every PCM-open mutex, chip-open and state
 * locks held. There are no active handles and no callback can enter.
 */
static int enable_pcm(void)
{
	struct hda_pcm *p;
	unsigned int d;
	if (enabled || saved_count) return -EALREADY;
	front_pcm=NULL; front_substream=NULL; front_info=NULL;
	wuh_pcm=NULL; wuh_substream=NULL; wuh_info=NULL;
	list_for_each_entry(p,&held_codec->pcm_list_head,list) {
		if (!p->pcm) continue;
		for (d=0;d<2;d++) if (p->stream[d].substreams) {
			if (saved_count==ARRAY_SIZE(saved)) { saved_count=0; return -E2BIG; }
			saved[saved_count++]=(struct saved_hinfo){&p->stream[d],p->stream[d]};
		}

        if (p->stream[1].nid==0x0a && p->stream[1].substreams==1) {
            if (wuh_pcm || p->pcm->device!=2) { saved_count=0; return -EINVAL; }
            wuh_pcm=p->pcm; wuh_substream=p->pcm->streams[1].substream; wuh_info=&p->stream[1];
        }
		if (p->stream[0].nid==2 && p->stream[0].substreams==1) {
			if (front_pcm) { saved_count=0; return -EINVAL; }
			front_pcm=p->pcm; front_substream=p->pcm->streams[0].substream; front_info=&p->stream[0];
		}
	}
	if (!front_info || !front_substream || !front_substream->ops) { saved_count=0; return -ENODEV; }
	original_pcm_ops=front_substream->ops;
	if (!original_pcm_ops->open || !original_pcm_ops->close || !original_pcm_ops->hw_params ||
	    !original_pcm_ops->hw_free || !original_pcm_ops->trigger || !original_pcm_ops->pointer) {
		saved_count=0; return -EINVAL;
	}

    if (!wuh_info || !wuh_substream || !wuh_substream->ops ||
        !wuh_substream->ops->prepare || !wuh_info->ops.open ||
        wuh_info->ops.open!=wuh_info->ops.close ||
        wuh_info->ops.open!=front_info->ops.open ||
        wuh_info->ops.close!=front_info->ops.close) {
        saved_count=0; return -EINVAL;
    }
    wuh_original_ops=wuh_substream->ops;
	if (!try_module_get(THIS_MODULE)) { saved_count=0; return -ENODEV; }
	for (d=0;d<saved_count;d++) saved[d].pointer->ops.open=reject_other_open;

    wuh_info->channels_min=2; wuh_info->channels_max=2;
    wuh_info->rates=SNDRV_PCM_RATE_96000;
    wuh_info->formats=SNDRV_PCM_FMTBIT_S32_LE; wuh_info->maxbps=32;
    wuh_info->ops.open=wuh_open; wuh_info->ops.close=wuh_close;
    wuh_experimental_ops=*wuh_original_ops;
    wuh_experimental_ops.prepare=wuh_pcm_prepare;
    snd_pcm_set_ops(wuh_pcm,SNDRV_PCM_STREAM_CAPTURE,&wuh_experimental_ops);
	front_info->channels_min=2; front_info->channels_max=2;
	front_info->rates=SNDRV_PCM_RATE_96000|SNDRV_PCM_RATE_192000|SNDRV_PCM_RATE_384000;
	front_info->formats=SNDRV_PCM_FMTBIT_S32_LE; front_info->maxbps=32;
	front_info->ops=(struct hda_pcm_ops){.open=direct_open,.close=direct_close,
		.prepare=direct_prepare,.cleanup=direct_cleanup};
	experimental_pcm_ops=*original_pcm_ops;
	experimental_pcm_ops.prepare=pcm_prepare;
	experimental_pcm_ops.trigger=pcm_trigger;
	experimental_pcm_ops.get_time_info=NULL;
	snd_pcm_set_ops(front_pcm,SNDRV_PCM_STREAM_PLAYBACK,&experimental_pcm_ops);
	prepare_error=0; cleanup_error=0; recovery_attempts=0;
	enabled=true;
	return 0;
}
static int disable_pcm(void)
{
	unsigned int i;
	if (!enabled) return -EINVAL;
	if (needs_restore || reserved_stream || wuh_opened || wuh_preparing) return -EBUSY;
	snd_pcm_set_ops(front_pcm,SNDRV_PCM_STREAM_PLAYBACK,original_pcm_ops);
	snd_pcm_set_ops(wuh_pcm,SNDRV_PCM_STREAM_CAPTURE,wuh_original_ops);
	for (i=0;i<saved_count;i++) *saved[i].pointer=saved[i].value;
	saved_count=0; enabled=false;
	module_put(THIS_MODULE);
	return 0;
}
static int run_command(const char *value)
{
	struct snd_pcm *locked[16];
	struct hda_pcm *p;
	struct snd_pcm_substream *s;
	struct azx *chip;
	unsigned int n=0,d;
	int err=-EBUSY;
	bool repair=sysfs_streq(value,"recover");
	if (!ready || !held_dev) return -ENODEV;
	if (!sysfs_streq(value,"check") && !sysfs_streq(value,"enable") &&
	    !sysfs_streq(value,"disable") && !repair) return -EINVAL;
	if (!managed_mode) return -EPERM;
	if (!device_trylock(held_dev)) return -EBUSY;
	if (!held_dev->driver || held_dev->driver->owner!=held_owner || held_codec->spec!=held_spec) {
		err=-ENODEV; goto device_out;
	}
	if (!mutex_trylock(&held_codec->user_mutex)) goto device_out;
	list_for_each_entry(p,&held_codec->pcm_list_head,list) {
		if (!p->pcm) continue;
		if (n==ARRAY_SIZE(locked)) { err=-E2BIG; goto pcm_out; }
		if (!mutex_trylock(&p->pcm->open_mutex)) goto pcm_out;
		locked[n++]=p->pcm;
		for (d=0;d<2;d++) for (s=p->pcm->streams[d].substream;s;s=s->next)
			if (s->ref_count || s->runtime || s->hw_opened) goto pcm_out;
	}
	if (!n) { err=-ENODEV; goto pcm_out; }
	chip=bus_to_azx(&held_codec->bus->core);
	if (!mutex_trylock(&chip->open_mutex)) goto pcm_out;
	down_write(&held_codec->card->controls_rwsem);
	mutex_lock(&state_mutex);
	mutex_lock(&held_spec->chipio_mutex);
	mutex_lock(&ca0132_mmio_mutex);
	if (repair) {
		if (!enabled || !needs_restore || reserved_stream || recovery_attempts>=2) err=-EPERM;
		else {
			recovery_attempts++;
			reserved_stream=snd_hdac_stream_assign(&held_codec->bus->core,front_substream);
			err=reserved_stream ? restore_path() : -EBUSY;
			if (reserved_stream) snd_hdac_stream_release(reserved_stream);
			reserved_stream=NULL;
		}
	} else if (needs_restore) err=-EBUSY;
	else {
		struct ae5_cycle_io io={hardware_transfer,held_codec};
		err=ae5_clock_baseline(&io,initial);
		if (!err && (snd_hda_codec_read(held_codec,2,0,AC_VERB_GET_CONV,0) ||
		    snd_hda_codec_read(held_codec,2,0,AC_VERB_GET_STREAM_FORMAT,0))) err=-EINVAL;
		if (!err && sysfs_streq(value,"enable")) err=enable_pcm();
		if (!err && sysfs_streq(value,"disable")) err=disable_pcm();
	}
	mutex_unlock(&ca0132_mmio_mutex);
	mutex_unlock(&held_spec->chipio_mutex);
	mutex_unlock(&state_mutex);
	up_write(&held_codec->card->controls_rwsem);
	mutex_unlock(&chip->open_mutex);
pcm_out:
	while (n) mutex_unlock(&locked[--n]->open_mutex);
	mutex_unlock(&held_codec->user_mutex);
device_out:
	device_unlock(held_dev);
	return err;
}
static int ae5_attach(void);
static void ae5_detach(void);
static int command_set(const char *value,const struct kernel_param *kp)
{
    if (!managed_mode) return -EPERM;
    if (sysfs_streq(value,"attach")) {
        if (ready) return -EALREADY;
        return ae5_attach();
    }
    if (sysfs_streq(value,"detach")) {
        if (!ready) return 0;
        if (enabled || needs_restore || reserved_stream || wuh_opened) return -EBUSY;
        ae5_detach();
        return 0;
    }
	last_error=run_command(value);
	return last_error;
}
static int status_get(char *buffer,const struct kernel_param *kp)
{
	ae5_u32 clocks[AE5_REG_COUNT]={0};
	unsigned int i,fmt=0,lpib=0,running=0;
	int offset,err=0;
	mutex_lock(&state_mutex);
	if (reserved_stream) {
		fmt=snd_hdac_stream_readw(reserved_stream,SD_FORMAT);
		lpib=snd_hdac_stream_readl(reserved_stream,SD_LPIB);
		running=reserved_stream->running;
	}
	if (ready) {
		mutex_lock(&held_spec->chipio_mutex);
		for (i=0;i<AE5_REG_COUNT && !err;i++) err=hardware_transfer(held_codec,AE5_CYCLE_READ,i,&clocks[i]);
		mutex_unlock(&held_spec->chipio_mutex);
	}
	offset=sysfs_emit(buffer,"{\"ready\":%u,\"enabled\":%u,\"managed_mode\":%u,\"needs_restore\":%u,\"device\":%d,\"rate\":%u,\"wire_format\":%u,\"dma_format\":%u,\"dma_running\":%u,\"lpib\":%u,\"reserved_tag\":%u,\"prepares\":%u,\"restores\":%u,\"starts\":%u,\"stops\":%u,\"recovery_attempts\":%u,\"last_error\":%d,\"prepare_error\":%d,\"cleanup_error\":%d,\"route_verified\":%u,\"path_verified\":%u,\"restore_verified\":%u,\"restore_error\":%d,\"clock_error\":%d,\"wuh_opened\":%u,\"wuh_prepared\":%u,\"wuh_running\":%u,\"wuh_prepare_attempts\":%u,\"clocks\":[",
		ready,enabled,managed_mode,needs_restore,front_pcm ? front_pcm->device : -1,
		logical_rate,wire_format,fmt,running,lpib,reserved_tag,prepares,restores,starts,stops,recovery_attempts,
		last_error,prepare_error,cleanup_error,outcome.target_verified,path_result.target_verified,
		outcome.restore_verified,outcome.restore_error,err,wuh_opened,wuh_prepared,wuh_running(),wuh_prepare_attempts);
	for (i=0;i<AE5_REG_COUNT;i++) offset+=sysfs_emit_at(buffer,offset,"%s%u",i ? "," : "",clocks[i]);
	offset+=sysfs_emit_at(buffer,offset,"],\"volume\":{\"stream_ready\":%u,\"pending\":%u,\"applied_verified\":%u,\"restored\":%u,\"error\":%d,\"restore_error\":%d,\"before\":[",
 stream_ready,volume.needs_restore,volume.applied_verified,volume.restored,volume.error,volume.restore_error);
 for(i=0;i<DAC_READ_COUNT;i++) offset+=sysfs_emit_at(buffer,offset,"%s%u",i?",":"",volume.before_regs[i]);
 offset+=sysfs_emit_at(buffer,offset,"],\"applied\":[");
 for(i=0;i<DAC_READ_COUNT;i++) offset+=sysfs_emit_at(buffer,offset,"%s%u",i?",":"",volume.applied_regs[i]);
 offset+=sysfs_emit_at(buffer,offset,"],\"after\":[");
 for(i=0;i<DAC_READ_COUNT;i++) offset+=sysfs_emit_at(buffer,offset,"%s%u",i?",":"",volume.after_regs[i]);
 offset+=sysfs_emit_at(buffer,offset,"],\"cached\":true,\"live_updates\":%u,\"live_error\":%d,\"live_rollback_error\":%d}",
  volume.live_updates,volume.live_error,volume.live_rollback_error);
 offset+=sysfs_emit_at(buffer,offset,",\"baseline\":{\"format\":%u,\"converter\":%u,\"references\":%u,\"raw_count\":%u,\"raw_complete\":%u,\"route_count\":%u,\"route_error\":%d,\"peer_error\":%d,\"peers\":[",
        outcome.before.format,outcome.before.converter,outcome.before.references,
        outcome.before.raw.count,outcome.before.raw.complete,outcome.before.route.count,
        outcome.before.route_error,outcome.before.peer_error);
    for (i=0;i<3;i++) offset+=sysfs_emit_at(buffer,offset,"%s[%u,%u]",i ? "," : "",
        outcome.before.peers[i].references,outcome.before.peers[i].count);
    offset+=sysfs_emit_at(buffer,offset,"],\"parameters\":[");
    for (i=0;i<3;i++) offset+=sysfs_emit_at(buffer,offset,"%s[%u,%u,%u,%u]",i ? "," : "",
        outcome.before.parameters[i][0],outcome.before.parameters[i][1],
        outcome.before.parameters[i][2],outcome.before.parameters[i][3]);
    offset+=sysfs_emit_at(buffer,offset,"],\"rates\":[%u,%u,%u],\"selectors\":[%u,%u]}}\n",
        outcome.before.rates[0],outcome.before.rates[1],outcome.before.rates[2],
        outcome.before.selectors[0],outcome.before.selectors[1]);
	mutex_unlock(&state_mutex);
	return offset;
}
static const struct kernel_param_ops command_ops={.set=command_set};
static const struct kernel_param_ops status_ops={.get=status_get};
module_param_cb(command,&command_ops,NULL,0200);
module_param_cb(status,&status_ops,NULL,0400);

static int ae5_attach(void)
{
	struct ae5_cycle_io io = {hardware_transfer, NULL};
	struct device *dev;
	int err = -ENODEV;
	if (strcmp(utsname()->release,"7.0.0-31-generic")) return err;
	dev = bus_find_device_by_name(&snd_hda_bus_type,NULL,codec_name);
	if (!dev) return err;
	device_lock(dev);
	if (!dev->driver || !dev->driver->owner) goto unlock;
	held_owner = dev->driver->owner;
	if (held_owner != THIS_MODULE)
		goto unlock;
	held_codec = dev_to_hda_codec(dev);
	observed_caps = bus_to_azx(&held_codec->bus->core)->driver_caps;
	observed_captures = bus_to_azx(&held_codec->bus->core)->capture_streams;
	if (held_codec->core.vendor_id != 0x11020011 ||
	    held_codec->core.subsystem_id != 0x11020051 || !held_codec->spec)
		goto unlock;
	if (!try_module_get(held_owner)) goto unlock;
	err = pm_runtime_get_if_active(dev);
	if (err <= 0) { err = err < 0 ? err : -EAGAIN; goto put_owner; }
	held_spec = held_codec->spec;
	err = -EBUSY;
	if (!mutex_trylock(&held_codec->user_mutex)) goto put_pm;
	if (!mutex_trylock(&held_spec->chipio_mutex)) goto unlock_user;
	io.context = held_codec;
	err = ae5_clock_baseline(&io, initial);
	mutex_unlock(&held_spec->chipio_mutex);
unlock_user:
	mutex_unlock(&held_codec->user_mutex);
	if (!err) {
		if (!held_codec->bus->pci || pci_resource_len(held_codec->bus->pci,2)<0x900) { err=-ENODEV; goto put_pm; }
		mmio=pci_iomap(held_codec->bus->pci,2,0x900);
		if (!mmio) { err=-ENOMEM; goto put_pm; }
		held_dev = dev;
		ready = true;
		device_unlock(dev);
		pr_info("ae5_pcm_experiment: active power pinned, 96k baseline read; no format writes\n");
		return 0;
	}
put_pm:
	pm_runtime_put(dev);
put_owner:
	module_put(held_owner);
unlock:
	device_unlock(dev);
	put_device(dev);
	return err;
}


static void ae5_detach(void)
{
	/* Enable owns a self reference, so normal unload cannot reach here while
	 * callbacks are installed or restoration is pending. Never force unload.
	 */
	ready=false;
	pci_iounmap(held_codec->bus->pci,mmio);
	pm_runtime_put(held_dev);
	module_put(held_owner);
    put_device(held_dev);
    held_dev=NULL; held_codec=NULL; held_spec=NULL; held_owner=NULL; mmio=NULL;
    front_pcm=NULL; front_substream=NULL; front_info=NULL;
    wuh_pcm=NULL; wuh_substream=NULL; wuh_info=NULL;
}

/* ALSA holds the card's control read lock around these callbacks. Never
 * acquire controls_rwsem for writing here. State -> ChipIO -> MMIO is the
 * same order used by the PCM preparation and recovery paths.
 */
static int ae5_dac_volume_info(struct snd_kcontrol *control,
                              struct snd_ctl_elem_info *info)
{
 info->type=SNDRV_CTL_ELEM_TYPE_INTEGER;
 info->count=2;
 info->value.integer.min=0;
 info->value.integer.max=255;
 info->value.integer.step=1;
 return 0;
}
static int ae5_dac_volume_get(struct snd_kcontrol *control,
                             struct snd_ctl_elem_value *value)
{
 struct hda_codec *codec=snd_kcontrol_chip(control);
 struct ca0132_spec *spec=codec->spec;
 mutex_lock(&state_mutex);
 value->value.integer.value[0]=spec->ae5_direct_volume[0];
 value->value.integer.value[1]=spec->ae5_direct_volume[1];
 mutex_unlock(&state_mutex);
 return 0;
}
static int ae5_dac_volume_put(struct snd_kcontrol *control,
                             struct snd_ctl_elem_value *value)
{
 struct hda_codec *codec=snd_kcontrol_chip(control);
 struct ca0132_spec *spec=codec->spec;
 struct dac_io io={dac_hardware,NULL};
 unsigned int steps[2];
 long left=value->value.integer.value[0],right=value->value.integer.value[1];
 int err=0,changed;
 if(left<0 || left>255 || right<0 || right>255) return -EINVAL;
 mutex_lock(&state_mutex);
 changed=spec->ae5_direct_volume[0]!=left || spec->ae5_direct_volume[1]!=right ||
         !spec->ae5_direct_volume_selected;
 if(ready && held_codec==codec) {
  if(enabled && stream_ready && volume.volume_pending) {
   if(volume.error || !volume.applied_verified) { err=-EIO; goto out; }
   if(changed) {
    steps[0]=255-left; steps[1]=255-right;
    mutex_lock(&spec->chipio_mutex);
    mutex_lock(&ca0132_mmio_mutex);
    err=ae5_volume_set_live(&io,&volume,steps);
    mutex_unlock(&ca0132_mmio_mutex);
    mutex_unlock(&spec->chipio_mutex);
    if(err) {
     if(volume.error) WRITE_ONCE(stream_ready,false);
     goto out;
    }
   }
  } else if(needs_restore || reserved_stream) { err=-EBUSY; goto out; }
 }
 /* When Direct is inactive, save only. Never alter the ordinary DSP path. */
 spec->ae5_direct_volume[0]=left; spec->ae5_direct_volume[1]=right;
 spec->ae5_direct_volume_selected=true;
out:
 mutex_unlock(&state_mutex);
 return err ? err : changed;
}
static int ae5_direct_active_get(struct snd_kcontrol *control,
                                struct snd_ctl_elem_value *value)
{
 mutex_lock(&state_mutex);
 value->value.integer.value[0]=ready && enabled && held_codec==snd_kcontrol_chip(control);
 mutex_unlock(&state_mutex);
 return 0;
}
static int ae5_dac_status_info(struct snd_kcontrol *control,struct snd_ctl_elem_info *info)
{
 static const char * const names[]={"Queued","Applied","Unknown after error"};
 return snd_ctl_enum_info(info,1,ARRAY_SIZE(names),names);
}
static int ae5_dac_status_get(struct snd_kcontrol *control,struct snd_ctl_elem_value *value)
{
 unsigned int status=0;
 mutex_lock(&state_mutex);
 if(ready && held_codec==snd_kcontrol_chip(control) && enabled) {
  if(volume.error || (volume.volume_pending && !volume.applied_verified)) status=2;
  else if(stream_ready && volume.applied_verified) status=1;
 }
 value->value.enumerated.item[0]=status;
 mutex_unlock(&state_mutex);
 return 0;
}
static const DECLARE_TLV_DB_SCALE(ae5_dac_volume_db,-12750,50,0);
static int ae5_add_volume_controls(struct hda_codec *codec)
{
 struct ca0132_spec *spec=codec->spec;
 static const struct snd_kcontrol_new controls[]={
  {
   .iface=SNDRV_CTL_ELEM_IFACE_MIXER,
   .name="AE-5: Direct DAC Playback Volume",
   .access=SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ |
           SNDRV_CTL_ELEM_ACCESS_VOLATILE,
   .info=ae5_dac_volume_info,.get=ae5_dac_volume_get,.put=ae5_dac_volume_put,
   .tlv.p=ae5_dac_volume_db,
  },
  {
   .iface=SNDRV_CTL_ELEM_IFACE_MIXER,
   .name="AE-5: Direct Active",
   .access=SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
   .info=snd_ctl_boolean_mono_info,.get=ae5_direct_active_get,
  },
  {
   .iface=SNDRV_CTL_ELEM_IFACE_MIXER,
   .name="AE-5: Direct DAC Status",
   .access=SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
   .info=ae5_dac_status_info,.get=ae5_dac_status_get,
  },
 };
 unsigned int i; int err;
 if(codec->core.vendor_id!=0x11020011 || codec->core.subsystem_id!=0x11020051) return 0;
 spec->ae5_direct_volume[0]=spec->ae5_direct_volume[1]=209; /* -23 dB */
 spec->ae5_direct_volume_selected=false;
 for(i=0;i<ARRAY_SIZE(controls);i++) {
  err=snd_ctl_add(codec->card,snd_ctl_new1(&controls[i],codec));
  if(err<0) return err;
 }
 return 0;
}
