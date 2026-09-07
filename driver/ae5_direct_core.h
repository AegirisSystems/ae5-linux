/* SPDX-License-Identifier: GPL-2.0-only */
/* AE-5 experimental Direct PCM transport/clock component. No hardware backend.
 * Derived from local interoperability observations documented in the workspace.
 * This component alone is not a playback driver. The caller must keep playback
 * stopped and output muted throughout clock changes and any failed transition.
 */
#ifndef AE5_DIRECT_CORE_H
#define AE5_DIRECT_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
typedef u32 ae5_u32;
#else
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
typedef uint32_t ae5_u32;
#endif

struct ae5_transport {
	ae5_u32 rate;
	ae5_u32 channels;
	ae5_u32 container_bits;
	ae5_u32 valid_bits;
	ae5_u32 bytes_per_second;
};

enum ae5_clock_op {
	AE5_DAC_COMMAND,
	AE5_CONTROL_PARAMETER,
	AE5_PLL_WRITE,
	AE5_HDA_VERB,
	AE5_CHIPIO_WRITE,
};

/* The callback returns 0 on success or a negative errno. An HDA read writes its
 * response to result. A backend must not report success before a write finishes.
 */
struct ae5_clock_io {
	int (*transfer)(void *context, enum ae5_clock_op op,
		       ae5_u32 a, ae5_u32 b, ae5_u32 c, ae5_u32 *result);
	void *context;
};

/* confirmed means this component's supported clock operations completed with
 * matching mode readback. It does NOT certify analog output or DAC lock.
 * Reset this state after reset, resume, or any external hardware reconfiguration.
 */
struct ae5_clock_state {
	bool confirmed;
	ae5_u32 rate;
};

int ae5_direct_transport(ae5_u32 rate, ae5_u32 channels, ae5_u32 valid_bits,
			struct ae5_transport *out);
int ae5_direct_clock_apply(const struct ae5_clock_io *io,
			   struct ae5_clock_state *state, ae5_u32 rate,
			   bool playback_stopped, bool output_muted);

#endif
