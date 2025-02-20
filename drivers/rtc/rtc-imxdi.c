// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2008-2009 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright 2010 Orex Computed Radiography
 */

/* based on rtc-mc13892.c */

/*
 * This driver uses the 47-bit 32 kHz counter in the Freescale DryIce block
 * to implement a Linux RTC. Times and alarms are truncated to seconds.
 * Since the RTC framework performs API locking via rtc->ops_lock the
 * only simultaneous accesses we need to deal with is updating DryIce
 * registers while servicing an alarm.
 *
 * Note that reading the DSR (DryIce Status Register) automatically clears
 * the WCF (Write Complete Flag). All DryIce writes are synchronized to the
 * LP (Low Power) domain and set the WCF upon completion. Writes to the
 * DIER (DryIce Interrupt Enable Register) are the only exception. These
 * occur at normal bus speeds and do not set WCF.  Periodic interrupts are
 * not supported by the hardware.
 */

#include <common.h>
#include <driver.h>
#include <init.h>
#include <rtc.h>
#include <io.h>
#include <linux/clk.h>
#include <linux/rtc.h>
#include <linux/nvmem-provider.h>

/* DryIce Register Definitions */

#define DTCMR     0x00           /* Time Counter MSB Reg */
#define DTCLR     0x04           /* Time Counter LSB Reg */

#define DCAMR     0x08           /* Clock Alarm MSB Reg */
#define DCALR     0x0c           /* Clock Alarm LSB Reg */
#define DCAMR_UNSET  0xFFFFFFFF  /* doomsday - 1 sec */

#define DCR       0x10           /* Control Reg */
#define DCR_TDCHL (1 << 30)      /* Tamper-detect configuration hard lock */
#define DCR_TDCSL (1 << 29)      /* Tamper-detect configuration soft lock */
#define DCR_KSSL  (1 << 27)      /* Key-select soft lock */
#define DCR_MCHL  (1 << 20)      /* Monotonic-counter hard lock */
#define DCR_MCSL  (1 << 19)      /* Monotonic-counter soft lock */
#define DCR_TCHL  (1 << 18)      /* Timer-counter hard lock */
#define DCR_TCSL  (1 << 17)      /* Timer-counter soft lock */
#define DCR_FSHL  (1 << 16)      /* Failure state hard lock */
#define DCR_TCE   (1 << 3)       /* Time Counter Enable */
#define DCR_MCE   (1 << 2)       /* Monotonic Counter Enable */

#define DSR       0x14           /* Status Reg */
#define DSR_WTD   (1 << 23)      /* Wire-mesh tamper detected */
#define DSR_ETBD  (1 << 22)      /* External tamper B detected */
#define DSR_ETAD  (1 << 21)      /* External tamper A detected */
#define DSR_EBD   (1 << 20)      /* External boot detected */
#define DSR_SAD   (1 << 19)      /* SCC alarm detected */
#define DSR_TTD   (1 << 18)      /* Temperature tamper detected */
#define DSR_CTD   (1 << 17)      /* Clock tamper detected */
#define DSR_VTD   (1 << 16)      /* Voltage tamper detected */
#define DSR_WBF   (1 << 10)      /* Write Busy Flag (synchronous) */
#define DSR_WNF   (1 << 9)       /* Write Next Flag (synchronous) */
#define DSR_WCF   (1 << 8)       /* Write Complete Flag (synchronous)*/
#define DSR_WEF   (1 << 7)       /* Write Error Flag */
#define DSR_CAF   (1 << 4)       /* Clock Alarm Flag */
#define DSR_MCO   (1 << 3)       /* monotonic counter overflow */
#define DSR_TCO   (1 << 2)       /* time counter overflow */
#define DSR_NVF   (1 << 1)       /* Non-Valid Flag */
#define DSR_SVF   (1 << 0)       /* Security Violation Flag */

#define DIER      0x18           /* Interrupt Enable Reg (synchronous) */
#define DIER_WNIE (1 << 9)       /* Write Next Interrupt Enable */
#define DIER_WCIE (1 << 8)       /* Write Complete Interrupt Enable */
#define DIER_WEIE (1 << 7)       /* Write Error Interrupt Enable */
#define DIER_CAIE (1 << 4)       /* Clock Alarm Interrupt Enable */
#define DIER_SVIE (1 << 0)       /* Security-violation Interrupt Enable */

#define DMCR      0x1c           /* DryIce Monotonic Counter Reg */

#define DTCR      0x28           /* DryIce Tamper Configuration Reg */
#define DTCR_MOE  (1 << 9)       /* monotonic overflow enabled */
#define DTCR_TOE  (1 << 8)       /* time overflow enabled */
#define DTCR_WTE  (1 << 7)       /* wire-mesh tamper enabled */
#define DTCR_ETBE (1 << 6)       /* external B tamper enabled */
#define DTCR_ETAE (1 << 5)       /* external A tamper enabled */
#define DTCR_EBE  (1 << 4)       /* external boot tamper enabled */
#define DTCR_SAIE (1 << 3)       /* SCC enabled */
#define DTCR_TTE  (1 << 2)       /* temperature tamper enabled */
#define DTCR_CTE  (1 << 1)       /* clock tamper enabled */
#define DTCR_VTE  (1 << 0)       /* voltage tamper enabled */

#define DGPR      0x3c           /* DryIce General Purpose Reg */

/**
 * struct imxdi_dev - private imxdi rtc data
 * @dev: pionter to dev
 * @rtc: pointer to rtc struct
 * @ioaddr: IO registers pointer
 * @clk: input reference clock
 * @dsr: copy of the DSR register
 */
struct imxdi_dev {
	struct device *dev;
	struct rtc_device rtc;
	void __iomem *ioaddr;
	struct clk *clk;
	u32 dsr;
	struct nvmem_device *nvmem;
};

/* Some background:
 *
 * The DryIce unit is a complex security/tamper monitor device. To be able do
 * its job in a useful manner it runs a bigger statemachine to bring it into
 * security/tamper failure state and once again to bring it out of this state.
 *
 * This unit can be in one of three states:
 *
 * - "NON-VALID STATE"
 *   always after the battery power was removed
 * - "FAILURE STATE"
 *   if one of the enabled security events has happened
 * - "VALID STATE"
 *   if the unit works as expected
 *
 * Everything stops when the unit enters the failure state including the RTC
 * counter (to be able to detect the time the security event happened).
 *
 * The following events (when enabled) let the DryIce unit enter the failure
 * state:
 *
 * - wire-mesh-tamper detect
 * - external tamper B detect
 * - external tamper A detect
 * - temperature tamper detect
 * - clock tamper detect
 * - voltage tamper detect
 * - RTC counter overflow
 * - monotonic counter overflow
 * - external boot
 *
 * If we find the DryIce unit in "FAILURE STATE" and the TDCHL cleared, we
 * can only detect this state. In this case the unit is completely locked and
 * must force a second "SYSTEM POR" to bring the DryIce into the
 * "NON-VALID STATE" + "FAILURE STATE" where a recovery is possible.
 * If the TDCHL is set in the "FAILURE STATE" we are out of luck. In this case
 * a battery power cycle is required.
 *
 * In the "NON-VALID STATE" + "FAILURE STATE" we can clear the "FAILURE STATE"
 * and recover the DryIce unit. By clearing the "NON-VALID STATE" as the last
 * task, we bring back this unit into life.
 */

/*
 * Do a write into the unit without interrupt support.
 * We do not need to check the WEF here, because the only reason this kind of
 * write error can happen is if we write to the unit twice within the 122 us
 * interval. This cannot happen, since we are using this function only while
 * setting up the unit.
 */
static void di_write_busy_wait(const struct imxdi_dev *imxdi, u32 val,
			       unsigned reg)
{
	/* do the register write */
	writel(val, imxdi->ioaddr + reg);

	/*
	 * now it takes four 32,768 kHz clock cycles to take
	 * the change into effect = 122 us
	 */
	udelay(130);
}

static void di_what_is_to_be_done(struct imxdi_dev *imxdi,
				  const char *power_supply)
{
	dev_emerg(imxdi->dev, "Please cycle the %s power supply in order to get the DryIce/RTC unit working again\n",
		  power_supply);
}

static int di_handle_failure_state(struct imxdi_dev *imxdi, u32 dsr)
{
	u32 dcr;

	dev_dbg(imxdi->dev, "DSR register reports: %08X\n", dsr);

	dcr = readl(imxdi->ioaddr + DCR);

	if (dcr & DCR_FSHL) {
		/* we are out of luck */
		di_what_is_to_be_done(imxdi, "battery");
		return -ENODEV;
	}
	/*
	 * with the next SYSTEM POR we will transit from the "FAILURE STATE"
	 * into the "NON-VALID STATE" + "FAILURE STATE"
	 */
	di_what_is_to_be_done(imxdi, "main");

	return -ENODEV;
}

static int di_handle_valid_state(struct imxdi_dev *imxdi, u32 dsr)
{
	/* initialize alarm */
	di_write_busy_wait(imxdi, DCAMR_UNSET, DCAMR);
	di_write_busy_wait(imxdi, 0, DCALR);

	/* clear alarm flag */
	if (dsr & DSR_CAF)
		di_write_busy_wait(imxdi, DSR_CAF, DSR);

	return 0;
}

static int di_handle_invalid_state(struct imxdi_dev *imxdi, u32 dsr)
{
	u32 dcr, sec;

	/*
	 * lets disable all sources which can force the DryIce unit into
	 * the "FAILURE STATE" for now
	 */
	di_write_busy_wait(imxdi, 0x00000000, DTCR);
	/* and lets protect them at runtime from any change */
	di_write_busy_wait(imxdi, DCR_TDCSL, DCR);

	sec = readl(imxdi->ioaddr + DTCMR);
	if (sec != 0)
		dev_warn(imxdi->dev,
			 "The security violation has happened at %u seconds\n",
			 sec);
	/*
	 * the timer cannot be set/modified if
	 * - the TCHL or TCSL bit is set in DCR
	 */
	dcr = readl(imxdi->ioaddr + DCR);
	if (!(dcr & DCR_TCE)) {
		if (dcr & DCR_TCHL) {
			/* we are out of luck */
			di_what_is_to_be_done(imxdi, "battery");
			return -ENODEV;
		}
		if (dcr & DCR_TCSL) {
			di_what_is_to_be_done(imxdi, "main");
			return -ENODEV;
		}
	}
	/*
	 * - the timer counter stops/is stopped if
	 *   - its overflow flag is set (TCO in DSR)
	 *      -> clear overflow bit to make it count again
	 *   - NVF is set in DSR
	 *      -> clear non-valid bit to make it count again
	 *   - its TCE (DCR) is cleared
	 *      -> set TCE to make it count
	 *   - it was never set before
	 *      -> write a time into it (required again if the NVF was set)
	 */
	/* state handled */
	di_write_busy_wait(imxdi, DSR_NVF, DSR);
	/* clear overflow flag */
	di_write_busy_wait(imxdi, DSR_TCO, DSR);
	/* enable the counter */
	di_write_busy_wait(imxdi, dcr | DCR_TCE, DCR);
	/* set and trigger it to make it count */
	di_write_busy_wait(imxdi, sec, DTCMR);

	/* now prepare for the valid state */
	return di_handle_valid_state(imxdi, __raw_readl(imxdi->ioaddr + DSR));
}

static int di_handle_invalid_and_failure_state(struct imxdi_dev *imxdi, u32 dsr)
{
	u32 dcr;

	/*
	 * now we must first remove the tamper sources in order to get the
	 * device out of the "FAILURE STATE"
	 * To disable any of the following sources we need to modify the DTCR
	 */
	if (dsr & (DSR_WTD | DSR_ETBD | DSR_ETAD | DSR_EBD | DSR_SAD |
			DSR_TTD | DSR_CTD | DSR_VTD | DSR_MCO | DSR_TCO)) {
		dcr = __raw_readl(imxdi->ioaddr + DCR);
		if (dcr & DCR_TDCHL) {
			/*
			 * the tamper register is locked. We cannot disable the
			 * tamper detection. The TDCHL can only be reset by a
			 * DRYICE POR, but we cannot force a DRYICE POR in
			 * softwere because we are still in "FAILURE STATE".
			 * We need a DRYICE POR via battery power cycling....
			 */
			/*
			 * out of luck!
			 * we cannot disable them without a DRYICE POR
			 */
			di_what_is_to_be_done(imxdi, "battery");
			return -ENODEV;
		}
		if (dcr & DCR_TDCSL) {
			/* a soft lock can be removed by a SYSTEM POR */
			di_what_is_to_be_done(imxdi, "main");
			return -ENODEV;
		}
	}

	/* disable all sources */
	di_write_busy_wait(imxdi, 0x00000000, DTCR);

	/* clear the status bits now */
	di_write_busy_wait(imxdi, dsr & (DSR_WTD | DSR_ETBD | DSR_ETAD |
			DSR_EBD | DSR_SAD | DSR_TTD | DSR_CTD | DSR_VTD |
			DSR_MCO | DSR_TCO), DSR);

	dsr = readl(imxdi->ioaddr + DSR);
	if ((dsr & ~(DSR_NVF | DSR_SVF | DSR_WBF | DSR_WNF |
			DSR_WCF | DSR_WEF)) != 0)
		dev_warn(imxdi->dev,
			 "There are still some sources of pain in DSR: %08x!\n",
			 dsr & ~(DSR_NVF | DSR_SVF | DSR_WBF | DSR_WNF |
				 DSR_WCF | DSR_WEF));

	/*
	 * now we are trying to clear the "Security-violation flag" to
	 * get the DryIce out of this state
	 */
	di_write_busy_wait(imxdi, DSR_SVF, DSR);

	/* success? */
	dsr = readl(imxdi->ioaddr + DSR);
	if (dsr & DSR_SVF) {
		dev_crit(imxdi->dev,
			 "Cannot clear the security violation flag. We are ending up in an endless loop!\n");
		/* last resort */
		di_what_is_to_be_done(imxdi, "battery");
		return -ENODEV;
	}

	/*
	 * now we have left the "FAILURE STATE" and ending up in the
	 * "NON-VALID STATE" time to recover everything
	 */
	return di_handle_invalid_state(imxdi, dsr);
}

static int di_handle_state(struct imxdi_dev *imxdi)
{
	int rc;
	u32 dsr;

	dsr = readl(imxdi->ioaddr + DSR);

	switch (dsr & (DSR_NVF | DSR_SVF)) {
	case DSR_NVF:
		dev_warn(imxdi->dev, "Invalid stated unit detected\n");
		rc = di_handle_invalid_state(imxdi, dsr);
		break;
	case DSR_SVF:
		dev_warn(imxdi->dev, "Failure stated unit detected\n");
		rc = di_handle_failure_state(imxdi, dsr);
		break;
	case DSR_NVF | DSR_SVF:
		dev_warn(imxdi->dev,
			 "Failure+Invalid stated unit detected\n");
		rc = di_handle_invalid_and_failure_state(imxdi, dsr);
		break;
	default:
		dev_notice(imxdi->dev, "Unlocked unit detected\n");
		rc = di_handle_valid_state(imxdi, dsr);
	}

	return rc;
}

/*
 * This function attempts to clear the dryice write-error flag.
 *
 * A dryice write error is similar to a bus fault and should not occur in
 * normal operation.  Clearing the flag requires another write, so the root
 * cause of the problem may need to be fixed before the flag can be cleared.
 */
static void clear_write_error(struct imxdi_dev *imxdi)
{
	int cnt;

	dev_warn(imxdi->dev, "WARNING: Register write error!\n");

	/* clear the write error flag */
	writel(DSR_WEF, imxdi->ioaddr + DSR);

	/* wait for it to take effect */
	for (cnt = 0; cnt < 1000; cnt++) {
		if ((readl(imxdi->ioaddr + DSR) & DSR_WEF) == 0)
			return;
		udelay(10);
	}
	dev_err(imxdi->dev,
			"ERROR: Cannot clear write-error flag!\n");
}

/*
 * Write a dryice register and wait until it completes.
 *
 * This function uses interrupts to determine when the
 * write has completed.
 */
static int di_write_wait(struct imxdi_dev *imxdi, u32 val, int reg)
{
	int rc = 0;
	uint32_t dsr;
	uint64_t start;

	/* do the register write */
	writel(val, imxdi->ioaddr + reg);

	start = get_time_ns();

	/* wait for the write to finish */
	while (1) {
		dsr = readl(imxdi->ioaddr + DSR);

		if (dsr & (DSR_WCF | DSR_WEF))
			break;
		if (is_timeout(start, MSECOND))
			return -EIO;
	}

	/* check for write error */
	if (dsr & DSR_WEF) {
		clear_write_error(imxdi);
		rc = -EIO;
	}

	return rc;
}

static struct imxdi_dev *to_imxdi_dev(struct rtc_device *rtc)
{
	return container_of(rtc, struct imxdi_dev, rtc);
}

/*
 * read the seconds portion of the current time from the dryice time counter
 */
static int dryice_rtc_read_time(struct rtc_device *rtc, struct rtc_time *tm)
{
	struct imxdi_dev *imxdi = to_imxdi_dev(rtc);
	unsigned long now;

	now = readl(imxdi->ioaddr + DTCMR);
	rtc_time_to_tm(now, tm);

	return 0;
}

/*
 * set the seconds portion of dryice time counter and clear the
 * fractional part.
 */
static int dryice_rtc_set_time(struct rtc_device *rtc, struct rtc_time *tm)
{
	struct imxdi_dev *imxdi = to_imxdi_dev(rtc);
	u32 dcr, dsr;
	int ret;
	unsigned long secs;

	ret = rtc_tm_to_time(tm, &secs);
	if (ret)
		return ret;

	dcr = readl(imxdi->ioaddr + DCR);
	dsr = readl(imxdi->ioaddr + DSR);

	if (!(dcr & DCR_TCE) || (dsr & DSR_SVF)) {
		if (dcr & DCR_TCHL) {
			/* we are even more out of luck */
			di_what_is_to_be_done(imxdi, "battery");
			return -EPERM;
		}
		if ((dcr & DCR_TCSL) || (dsr & DSR_SVF)) {
			/* we are out of luck for now */
			di_what_is_to_be_done(imxdi, "main");
			return -EPERM;
		}
	}

	/* zero the fractional part first */
	ret = di_write_wait(imxdi, 0, DTCLR);
	if (ret)
		return ret;

	ret = di_write_wait(imxdi, secs, DTCMR);
	if (ret)
		return ret;

	return di_write_wait(imxdi, readl(imxdi->ioaddr + DCR) | DCR_TCE, DCR);
}

static const struct rtc_class_ops dryice_rtc_ops = {
	.read_time = dryice_rtc_read_time,
	.set_time = dryice_rtc_set_time,
};

static int nvstore_write(void *ctx, unsigned reg, const void *val, size_t bytes)
{
	struct imxdi_dev *imxdi = ctx;
	const u32 *val32 = val;

	if (bytes != 4)
		return 0;

	writel(*val32, imxdi->ioaddr + DGPR);

	return 0;
}

static int nvstore_read(void *ctx, unsigned reg, void *val, size_t bytes)
{
	struct imxdi_dev *imxdi = ctx;
	u32 *val32 = val;

	if (bytes != 4)
		return 0;

	*val32 = readl(imxdi->ioaddr + DGPR);

	return 0;
}

static struct nvmem_bus nvstore_nvmem_bus = {
	.write = nvstore_write,
	.read  = nvstore_read,
};

static struct nvmem_config nvstore_nvmem_config = {
	.name = "nvstore",
	.stride = 4,
	.word_size = 4,
	.size = 4,
	.bus = &nvstore_nvmem_bus,
};

static int __init dryice_rtc_probe(struct device *dev)
{
	struct resource *res;
	struct imxdi_dev *imxdi;
	int ret;

	imxdi = xzalloc(sizeof(*imxdi));

	imxdi->dev = dev;
	imxdi->rtc.ops = &dryice_rtc_ops;

	res = dev_request_mem_resource(dev, 0);
	if (IS_ERR(res))
		return PTR_ERR(res);

	imxdi->ioaddr = IOMEM(res->start);

	imxdi->clk = clk_get(dev, NULL);
	if (IS_ERR(imxdi->clk))
		return PTR_ERR(imxdi->clk);

	ret = clk_enable(imxdi->clk);
	if (ret)
		return ret;

	/*
	 * Initialize dryice hardware
	 */

	/* mask all interrupts */
	writel(0, imxdi->ioaddr + DIER);

	ret = di_handle_state(imxdi);
	if (ret)
		goto err;

	nvstore_nvmem_config.dev = dev;
	nvstore_nvmem_config.priv = imxdi;

	imxdi->nvmem = nvmem_register(&nvstore_nvmem_config);
	if (IS_ENABLED(CONFIG_NVMEM) && IS_ERR(imxdi->nvmem)) {
		ret = PTR_ERR(imxdi->nvmem);
		goto err;
	}

	ret = rtc_register(&imxdi->rtc);
	if (ret)
		goto err;

	return 0;

err:
	clk_disable(imxdi->clk);

	return ret;
}

static __maybe_unused const struct of_device_id dryice_dt_ids[] = {
	{ .compatible = "fsl,imx25-rtc" },
	{ /* sentinel */ }
};

static struct driver dryice_rtc_driver = {
        .name   = "imx-di-rtc",
        .probe  = dryice_rtc_probe,
        .of_compatible = DRV_OF_COMPAT(dryice_dt_ids),
};
device_platform_driver(dryice_rtc_driver);
