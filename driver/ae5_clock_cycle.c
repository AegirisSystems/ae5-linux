/* SPDX-License-Identifier: GPL-2.0-only */
#include "ae5_clock_cycle.h"

static const ae5_u32 baseline[AE5_REG_COUNT] = {
	0x81, 7, 0xc7, 0x1f101, 0x1f101, 0x14004, 0x2000f
};
static const ae5_u32 target[AE5_REG_COUNT] = {
	0x80, 7, 0xc7, 0x1f103, 0x1f103, 0x31002, 0x2000f
};

static int cycle_transfer(const struct ae5_cycle_io *io, enum ae5_cycle_op op,
			  enum ae5_cycle_reg reg, ae5_u32 *value)
{
	int err = io->transfer(io->context, op, reg, value);
	return err > 0 ? -EIO : err;
}

static int cycle_write(const struct ae5_cycle_io *io, enum ae5_cycle_reg reg,
		       ae5_u32 value)
{
	return cycle_transfer(io, AE5_CYCLE_WRITE, reg, &value);
}

static int cycle_verify(const struct ae5_cycle_io *io,
			const ae5_u32 *expected, ae5_u32 *observed)
{
	unsigned int i;
	int err;
	for (i = 0; i < AE5_REG_COUNT; i++) {
		err = cycle_transfer(io, AE5_CYCLE_READ, i, &observed[i]);
		if (err)
			return err;
		if (observed[i] != expected[i])
			return -EIO;
	}
	return 0;
}

static int cycle_mode(const struct ae5_cycle_io *io, ae5_u32 desired)
{
	unsigned int i;
	ae5_u32 value;
	int err = cycle_write(io, AE5_MODE, desired);
	if (err)
		return err;
	for (i = 0; i < 11; i++) {
		value = ~0U;
		err = cycle_transfer(io, AE5_CYCLE_READ, AE5_MODE, &value);
		if (err)
			return err;
		if (value == desired)
			return 0;
	}
	return -ETIMEDOUT;
}

int ae5_clock_baseline(const struct ae5_cycle_io *io, ae5_u32 *observed)
{
	if (!io || !io->transfer || !observed) return -EINVAL;
	return cycle_verify(io, baseline, observed);
}

int ae5_clock_cycle(const struct ae5_cycle_io *io,
		    struct ae5_cycle_result *result, bool isolated)
{
	ae5_u32 restored[AE5_REG_COUNT];
	unsigned int i;
	int err;
	if (!io || !io->transfer || !result)
		return -EINVAL;
	*result = (struct ae5_cycle_result){0};
	if (!isolated)
		return -EBUSY;
	err = ae5_clock_baseline(io, result->before);
	if (err)
		return err; /* A different baseline is never overwritten. */
	result->attempted = true;
	err = cycle_write(io, AE5_ASI, 4);
	if (!err)
		err = cycle_mode(io, target[AE5_MODE]);
	for (i = AE5_CLOCK0; !err && i < AE5_REG_COUNT; i++)
		err = cycle_write(io, i, target[i]);
	if (!err)
		err = cycle_write(io, AE5_ASI, target[AE5_ASI]);
	if (!err)
		err = cycle_verify(io, target, result->target);
	result->transition_error = err;
	result->target_verified = !err;

	/* A callback may fail after a write reached hardware. Restore every field
	 * we could have touched, even after the first transfer fails. Continue
	 * restoration after errors; report them even if a later read matches.
	 * There are no DAC, gain, PLL-data or audio-routing writes in this test.
	 */
#define RESTORE(call) do { \
	err = (call); \
	if (err && !result->restore_error) result->restore_error = err; \
} while (0)
	RESTORE(cycle_write(io, AE5_ASI, 4));
	RESTORE(cycle_mode(io, result->before[AE5_MODE]));
	for (i = AE5_CLOCK0; i < AE5_REG_COUNT; i++)
		RESTORE(cycle_write(io, i, result->before[i]));
	RESTORE(cycle_write(io, AE5_ASI, result->before[AE5_ASI]));
	RESTORE(cycle_verify(io, result->before, restored));
#undef RESTORE
	result->restore_verified = !result->restore_error;
	return result->restore_error ? result->restore_error : result->transition_error;
}
