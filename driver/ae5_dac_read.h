/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AE5_DAC_READ_H
#define AE5_DAC_READ_H
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>
typedef u32 dac_u32;
#else
#include <stdint.h>
#include <errno.h>
#include <string.h>
typedef uint32_t dac_u32;
#endif
enum dac_operation { DAC_MMIO_READ, DAC_MMIO_WRITE, DAC_DELAY };
struct dac_io {
 int (*transfer)(void *, enum dac_operation, dac_u32, dac_u32 *);
 void *context;
};
#define DAC_READ_COUNT 8
struct dac_result {
 dac_u32 before[3], after[3], values[DAC_READ_COUNT];
 unsigned int attempted, complete, needs_restore, restored, completed_reads;
 int error, restore_error;
};
extern const dac_u32 ae5_dac_read_registers[DAC_READ_COUNT];
int ae5_dac_inspect(const struct dac_io *, struct dac_result *);
int ae5_dac_bridge_restore(const struct dac_io *, struct dac_result *);
#endif
