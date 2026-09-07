// SPDX-License-Identifier: GPL-2.0-or-later
/* Bounded Direct transactions against the actual ca0132_spec, not a copied structure prefix. */
static int ae5_chipio_send(struct hda_codec *codec,
		       unsigned int reg,
		       unsigned int data)
{
	unsigned int res;
	unsigned long timeout = jiffies + msecs_to_jiffies(100);

	/* send bits of data specified by reg */
	do {
		res = snd_hda_codec_read(codec, WIDGET_CHIP_CTRL, 0,
					 reg, data);
		if (res == VENDOR_STATUS_CHIPIO_OK)
			return 0;
		msleep(20);
	} while (time_before(jiffies, timeout));

	return -EIO;
}
static int ae5_chipio_write_address(struct hda_codec *codec,
				unsigned int chip_addx)
{
	struct ca0132_spec *spec = codec->spec;
	int res;

	if (spec->curr_chip_addx == chip_addx)
			return 0;

	/* send low 16 bits of the address */
	res = ae5_chipio_send(codec, VENDOR_CHIPIO_ADDRESS_LOW,
			  chip_addx & 0xffff);

	if (res != -EIO) {
		/* send high 16 bits of the address */
		res = ae5_chipio_send(codec, VENDOR_CHIPIO_ADDRESS_HIGH,
				  chip_addx >> 16);
	}

	spec->curr_chip_addx = (res < 0) ? ~0U : chip_addx;

	return res;
}
static int ae5_chipio_read_data(struct hda_codec *codec, unsigned int *data)
{
	struct ca0132_spec *spec = codec->spec;
	int res;

	/* post read */
	res = ae5_chipio_send(codec, VENDOR_CHIPIO_HIC_POST_READ, 0);

	if (res != -EIO) {
		/* read status */
		res = ae5_chipio_send(codec, VENDOR_CHIPIO_STATUS, 0);
	}

	if (res != -EIO) {
		/* read data */
		*data = snd_hda_codec_read(codec, WIDGET_CHIP_CTRL, 0,
					   VENDOR_CHIPIO_HIC_READ_DATA,
					   0);
	}

	/*If no error encountered, automatically increment the address
	as per chip behaviour*/
	spec->curr_chip_addx = (res != -EIO) ?
					(spec->curr_chip_addx + 4) : ~0U;
	return res;
}

/* This experiment shortens the stock busy-poll bound to 100 ms. */
static int ae5_chipio_write_data(struct hda_codec *codec, unsigned int data)
{
	struct ca0132_spec *spec = codec->spec;
	int res;

	/* send low 16 bits of the data */
	res = ae5_chipio_send(codec, VENDOR_CHIPIO_DATA_LOW, data & 0xffff);

	if (res != -EIO) {
		/* send high 16 bits of the data */
		res = ae5_chipio_send(codec, VENDOR_CHIPIO_DATA_HIGH,
				  data >> 16);
	}

	/*If no error encountered, automatically increment the address
	as per chip behaviour*/
	spec->curr_chip_addx = (res != -EIO) ?
					(spec->curr_chip_addx + 4) : ~0U;
	return res;
}
