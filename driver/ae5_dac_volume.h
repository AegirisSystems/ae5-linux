/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AE5_DAC_VOLUME_H
#define AE5_DAC_VOLUME_H
#include "ae5_dac_read.h"
struct ae5_volume_result {
 struct dac_result bridge;
 dac_u32 before_regs[DAC_READ_COUNT],applied_regs[DAC_READ_COUNT],after_regs[DAC_READ_COUNT],desired[2];
 dac_u32 teardown_regs[DAC_READ_COUNT];
 unsigned int teardown_valid,external_controls_seen;
 unsigned int baseline_valid,applied_verified,volume_pending,needs_restore,restored,complete;
 int error,restore_error;
 /* Live transactions own only attenuation; mute is an error fallback. */
 dac_u32 live_before[DAC_READ_COUNT],live_next[2];
 unsigned int live_active,live_snapshot_valid,live_verified,live_updates;
 int live_error,live_rollback_error;
};
int ae5_volume_cycle(const struct dac_io *,struct ae5_volume_result *);
int ae5_volume_recover(const struct dac_io *,struct ae5_volume_result *);
int ae5_volume_hold(const struct dac_io *,struct ae5_volume_result *,unsigned int);
int ae5_volume_prepare(const struct dac_io *,struct ae5_volume_result *,const unsigned int [2],int);
int ae5_volume_set_live(const struct dac_io *,struct ae5_volume_result *,const unsigned int [2]);
int ae5_volume_write_allowed(const struct ae5_volume_result *,dac_u32,int);
int ae5_volume_activate(const struct dac_io *,struct ae5_volume_result *);
int ae5_volume_quiet(const struct dac_io *,struct ae5_volume_result *);
#endif
