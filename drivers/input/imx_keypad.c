// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the IMX keypad port.
 * Copyright (C) 2009 Alberto Panizzo <maramaopercheseimorto@gmail.com>
 * Copyright (C) 2012 Christian Kapeller <christian.kapeller@cmotion.eu>
 */

/*
	To use the imx keypad driver, you have to define the keys in your platform
	code.

	1. Configure the imx keypad row & column pads used by your board
	2. Define the keys you want to use:

	#define BTN_1         0x101
	#define BTN_2         0x102
	#define BTN_3         0x103

	static uint32_t keypad_codes[] = {
		// specify your keymap with KEY(row, col, keycode)
		KEY(0, 1, BTN_1),
		KEY(1, 0, BTN_2),
		KEY(1, 1, BTN_3),
	};

	static struct matrix_keymap_data keypad_data = {
		.keymap = keypad_codes,
		.keymap_size = ARRAY_SIZE(keypad_codes),
	};

	3. Add the keypad to your platform in your devices init callback:

	imx51_add_kpp(&keypad_data);

	4. Compile , flash, and enjoy
*/

#include <common.h>
#include <errno.h>
#include <init.h>
#include <io.h>
#include <poller.h>
#include <kfifo.h>
#include <malloc.h>
#include <input/matrix_keypad.h>
#include <linux/err.h>
#include <input/input.h>

/*
 * Keypad Controller registers (halfword)
 */
#define KPCR		0x00 /* Keypad Control Register */

#define KPSR		0x02 /* Keypad Status Register */
#define KBD_STAT_KPKD	(0x1 << 0) /* Key Press Interrupt Status bit (w1c) */
#define KBD_STAT_KPKR	(0x1 << 1) /* Key Release Interrupt Status bit (w1c) */
#define KBD_STAT_KDSC	(0x1 << 2) /* Key Depress Synch Chain Status bit (w1c)*/
#define KBD_STAT_KRSS	(0x1 << 3) /* Key Release Synch Status bit (w1c)*/
#define KBD_STAT_KDIE	(0x1 << 8) /* Key Depress Interrupt Enable Status bit */
#define KBD_STAT_KRIE	(0x1 << 9) /* Key Release Interrupt Enable */
#define KBD_STAT_KPPEN	(0x1 << 10) /* Keypad Clock Enable */

#define KDDR		0x04 /* Keypad Data Direction Register */
#define KPDR		0x06 /* Keypad Data Register */

#define MAX_MATRIX_KEY_ROWS	8
#define MAX_MATRIX_KEY_COLS	8
#define MATRIX_ROW_SHIFT	3

#define MAX_MATRIX_KEY_NUM	(MAX_MATRIX_KEY_ROWS * MAX_MATRIX_KEY_COLS)

struct imx_keypad {
	struct input_device input;
	struct clk *clk;
	struct device *dev;
	void __iomem *mmio_base;

	struct poller_struct poller;

	/*
	 * The matrix is stable only if no changes are detected after
	 * IMX_KEYPAD_SCANS_FOR_STABILITY scans
	 */
#define IMX_KEYPAD_SCANS_FOR_STABILITY 3
	int			stable_count;

	/* Masks for enabled rows/cols */
	unsigned short		rows_en_mask;
	unsigned short		cols_en_mask;

	unsigned short		keycodes[MAX_MATRIX_KEY_NUM];

	/*
	 * Matrix states:
	 * -stable: achieved after a complete debounce process.
	 * -unstable: used in the debouncing process.
	 */
	unsigned short		matrix_stable_state[MAX_MATRIX_KEY_COLS];
	unsigned short		matrix_unstable_state[MAX_MATRIX_KEY_COLS];
};

static inline struct imx_keypad *
poller_to_imx_kp_pdata(struct poller_struct *poller)
{
	return container_of(poller, struct imx_keypad, poller);
}

/* Scan the matrix and return the new state in *matrix_volatile_state. */
static void imx_keypad_scan_matrix(struct imx_keypad *keypad,
				  unsigned short *matrix_volatile_state)
{
	int col;
	unsigned short reg_val;

	for (col = 0; col < MAX_MATRIX_KEY_COLS; col++) {
		if ((keypad->cols_en_mask & (1 << col)) == 0)
			continue;
		/*
		 * Discharge keypad capacitance:
		 * 2. write 1s on column data.
		 * 3. configure columns as totem-pole to discharge capacitance.
		 * 4. configure columns as open-drain.
		 */
		reg_val = readw(keypad->mmio_base + KPDR);
		reg_val |= 0xff00;
		writew(reg_val, keypad->mmio_base + KPDR);

		reg_val = readw(keypad->mmio_base + KPCR);
		reg_val &= ~((keypad->cols_en_mask & 0xff) << 8);
		writew(reg_val, keypad->mmio_base + KPCR);

		udelay(2);

		reg_val = readw(keypad->mmio_base + KPCR);
		reg_val |= (keypad->cols_en_mask & 0xff) << 8;
		writew(reg_val, keypad->mmio_base + KPCR);

		/*
		 * 5. Write a single column to 0, others to 1.
		 * 6. Sample row inputs and save data.
		 * 7. Repeat steps 2 - 6 for remaining columns.
		 */
		reg_val = readw(keypad->mmio_base + KPDR);
		reg_val &= ~(1 << (8 + col));
		writew(reg_val, keypad->mmio_base + KPDR);

		/*
		 * Delay added to avoid propagating the 0 from column to row
		 * when scanning.
		 */
		udelay(5);

		/*
		 * 1s in matrix_volatile_state[col] means key pressures
		 * throw data from non enabled rows.
		 */
		reg_val = readw(keypad->mmio_base + KPDR);
		matrix_volatile_state[col] = (~reg_val) & keypad->rows_en_mask;
	}

	/*
	 * Return in standby mode:
	 * 9. write 0s to columns
	 */
	reg_val = readw(keypad->mmio_base + KPDR);
	reg_val &= 0x00ff;
	writew(reg_val, keypad->mmio_base + KPDR);
}

/*
 * Compare the new matrix state (volatile) with the stable one stored in
 * keypad->matrix_stable_state and fire events if changes are detected.
 */
static void imx_keypad_fire_events(struct imx_keypad *keypad,
				   unsigned short *matrix_volatile_state)
{
	int row, col;

	for (col = 0; col < MAX_MATRIX_KEY_COLS; col++) {
		unsigned short bits_changed;
		int code;

		if ((keypad->cols_en_mask & (1 << col)) == 0)
			continue; /* Column is not enabled */

		bits_changed = keypad->matrix_stable_state[col] ^
						matrix_volatile_state[col];

		if (bits_changed == 0)
			continue; /* Column does not contain changes */

		for (row = 0; row < MAX_MATRIX_KEY_ROWS; row++) {
			if ((keypad->rows_en_mask & (1 << row)) == 0)
				continue; /* Row is not enabled */
			if ((bits_changed & (1 << row)) == 0)
				continue; /* Row does not contain changes */

			code = MATRIX_SCAN_CODE(row, col, MATRIX_ROW_SHIFT);

			input_report_key_event(&keypad->input, keypad->keycodes[code],
					       matrix_volatile_state[col] & (1 << row));

			dev_dbg(keypad->dev, "Event code: %d, val: %d",
				keypad->keycodes[code],
				matrix_volatile_state[col] & (1 << row));
		}
	}
}

/*
 * imx_keypad_check_for_events is the timer handler.
 */
static void imx_keypad_check_for_events(struct poller_struct *poller)
{
	struct imx_keypad *keypad = (struct imx_keypad *) poller_to_imx_kp_pdata(poller);
	unsigned short matrix_volatile_state[MAX_MATRIX_KEY_COLS];
	unsigned short reg_val;
	bool state_changed, is_zero_matrix;
	int i;

	memset(matrix_volatile_state, 0, sizeof(matrix_volatile_state));

	imx_keypad_scan_matrix(keypad, matrix_volatile_state);

	state_changed = false;
	for (i = 0; i < MAX_MATRIX_KEY_COLS; i++) {
		if ((keypad->cols_en_mask & (1 << i)) == 0)
			continue;

		if (keypad->matrix_unstable_state[i] ^ matrix_volatile_state[i]) {
			state_changed = true;
			break;
		}
	}

	/*
	 * If the matrix state is changed from the previous scan
	 *   (Re)Begin the debouncing process, saving the new state in
	 *    keypad->matrix_unstable_state.
	 * else
	 *   Increase the count of number of scans with a stable state.
	 */
	if (state_changed) {
		memcpy(keypad->matrix_unstable_state, matrix_volatile_state,
			sizeof(matrix_volatile_state));
		keypad->stable_count = 0;
	} else
		keypad->stable_count++;

	/*
	 * If the matrix is not as stable as we want reschedule scan
	 * in the near future.
	 */
	if (keypad->stable_count < IMX_KEYPAD_SCANS_FOR_STABILITY) {
		return;
	}

	/*
	 * If the matrix state is stable, fire the events and save the new
	 * stable state. Note, if the matrix is kept stable for longer
	 * (keypad->stable_count > IMX_KEYPAD_SCANS_FOR_STABILITY) all
	 * events have already been generated.
	 */
	if (keypad->stable_count == IMX_KEYPAD_SCANS_FOR_STABILITY) {
		imx_keypad_fire_events(keypad, matrix_volatile_state);

		memcpy(keypad->matrix_stable_state, matrix_volatile_state,
			sizeof(matrix_volatile_state));
	}

	is_zero_matrix = true;
	for (i = 0; i < MAX_MATRIX_KEY_COLS; i++) {
		if (matrix_volatile_state[i] != 0) {
			is_zero_matrix = false;
			break;
		}
	}


	if (is_zero_matrix) {
		/*
		 * All keys have been released. Enable only the KDI
		 * interrupt for future key presses (clear the KDI
		 * status bit and its sync chain before that).
		 */
		reg_val = readw(keypad->mmio_base + KPSR);
		reg_val |= KBD_STAT_KPKD | KBD_STAT_KDSC;
		writew(reg_val, keypad->mmio_base + KPSR);

		reg_val = readw(keypad->mmio_base + KPSR);
		reg_val |= KBD_STAT_KDIE;
		reg_val &= ~KBD_STAT_KRIE;
		writew(reg_val, keypad->mmio_base + KPSR);
	} else {
		/*
		 * Some keys are still pressed. Schedule a rescan in
		 * attempt to detect multiple key presses and enable
		 * the KRI interrupt to react quickly to key release
		 * event.
		 */
		reg_val = readw(keypad->mmio_base + KPSR);
		reg_val |= KBD_STAT_KPKR | KBD_STAT_KRSS;
		writew(reg_val, keypad->mmio_base + KPSR);

		reg_val = readw(keypad->mmio_base + KPSR);
		reg_val |= KBD_STAT_KRIE;
		reg_val &= ~KBD_STAT_KDIE;
		writew(reg_val, keypad->mmio_base + KPSR);
	}
}

static void imx_keypad_config(struct imx_keypad *keypad)
{
	unsigned short reg_val;

	/*
	 * Include enabled rows in interrupt generation (KPCR[7:0])
	 * Configure keypad columns as open-drain (KPCR[15:8])
	 */
	reg_val = readw(keypad->mmio_base + KPCR);
	reg_val |= keypad->rows_en_mask & 0xff;		/* rows */
	reg_val |= (keypad->cols_en_mask & 0xff) << 8;	/* cols */
	writew(reg_val, keypad->mmio_base + KPCR);

	/* Write 0's to KPDR[15:8] (Colums) */
	reg_val = readw(keypad->mmio_base + KPDR);
	reg_val &= 0x00ff;
	writew(reg_val, keypad->mmio_base + KPDR);

	/* Configure columns as output, rows as input (KDDR[15:0]) */
	writew(0xff00, keypad->mmio_base + KDDR);

	/*
	 * Clear Key Depress and Key Release status bit.
	 * Clear both synchronizer chain.
	 */
	reg_val = readw(keypad->mmio_base + KPSR);
	reg_val |= KBD_STAT_KPKR | KBD_STAT_KPKD |
		   KBD_STAT_KDSC | KBD_STAT_KRSS;
	writew(reg_val, keypad->mmio_base + KPSR);

	/* Enable KDI and disable KRI (avoid false release events). */
	reg_val |= KBD_STAT_KDIE;
	reg_val &= ~KBD_STAT_KRIE;
	writew(reg_val, keypad->mmio_base + KPSR);
}

static void imx_keypad_inhibit(struct imx_keypad *keypad)
{
	unsigned short reg_val;

	/* Inhibit KDI and KRI interrupts. */
	reg_val = readw(keypad->mmio_base + KPSR);
	reg_val &= ~(KBD_STAT_KRIE | KBD_STAT_KDIE);
	writew(reg_val, keypad->mmio_base + KPSR);

	/* Colums as open drain and disable all rows */
	writew(0xff00, keypad->mmio_base + KPCR);
}

static int __init imx_keypad_probe(struct device *dev)
{
	struct resource *iores;
	struct imx_keypad *keypad;
	const struct matrix_keymap_data *keymap_data = dev->platform_data;
	int i, ret, row, col;

	keypad = xzalloc(sizeof(struct imx_keypad));

	keypad->dev = dev;
	iores = dev_request_mem_resource(dev, 0);
	if (IS_ERR(iores))
		return PTR_ERR(iores);
	keypad->mmio_base = IOMEM(iores->start);

	ret = matrix_keypad_build_keymap(dev, keymap_data, MATRIX_ROW_SHIFT,
				keypad->keycodes);
	if (ret)
		return ret;

	/* Search for rows and cols enabled */
	for (row = 0; row < MAX_MATRIX_KEY_ROWS; row++) {
		for (col = 0; col < MAX_MATRIX_KEY_COLS; col++) {
			i = MATRIX_SCAN_CODE(row, col, MATRIX_ROW_SHIFT);
			if (keypad->keycodes[i] != KEY_RESERVED) {
				keypad->rows_en_mask |= 1 << row;
				keypad->cols_en_mask |= 1 << col;
			}
		}
	}

	if (keypad->rows_en_mask > ((1 << MAX_MATRIX_KEY_ROWS) - 1) ||
	   keypad->cols_en_mask > ((1 << MAX_MATRIX_KEY_COLS) - 1)) {
		dev_err(dev, "invalid key data (too many rows or colums)\n");
		free(keypad);
		return -EINVAL;
	}

	dev_dbg(dev, "enabled rows mask: %x\n", keypad->rows_en_mask);
	dev_dbg(dev, "enabled cols mask: %x\n", keypad->cols_en_mask);

	imx_keypad_config(keypad);

	/* Ensure that the keypad will stay dormant until opened */
	imx_keypad_inhibit(keypad);

	keypad->poller.func = imx_keypad_check_for_events;

	ret = poller_register(&keypad->poller, dev_name(dev));
	if (ret)
		return ret;

	ret = input_device_register(&keypad->input);
	if (ret)
		return ret;

	return 0;
}

static __maybe_unused struct of_device_id imx_keypad_dt_ids[] = {
        { .compatible = "fsl,imx21-kpp", },
        { }
};

static struct driver imx_keypad_driver = {
	.name   = "imx-kpp",
	.probe  = imx_keypad_probe,
	.of_compatible = DRV_OF_COMPAT(imx_keypad_dt_ids),
};
device_platform_driver(imx_keypad_driver);
