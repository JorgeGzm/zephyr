/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gd_gd32_rtc

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/gd32.h>
#include <zephyr/drivers/interrupt_controller/gd32_exti.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/sys/util.h>

#include <gd32_pmu.h>
#include <gd32_rcu.h>
#include <gd32_rtc.h>

#include "rtc_utils.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(rtc_gd32, CONFIG_RTC_LOG_LEVEL);

/*
 * The HAL defines the RTC register accessors on a fixed base address, so
 * re-derive the ones this driver uses from the devicetree reg property.
 */
#define GD32_RTC_TIME(reg) REG32((reg) + 0x00U)
#define GD32_RTC_DATE(reg) REG32((reg) + 0x04U)
#define GD32_RTC_CTL(reg)  REG32((reg) + 0x08U)
#define GD32_RTC_STAT(reg) REG32((reg) + 0x0CU)
#define GD32_RTC_PSC(reg)  REG32((reg) + 0x10U)
#define GD32_RTC_WPK(reg)  REG32((reg) + 0x24U)
#define GD32_RTC_SS(reg)   REG32((reg) + 0x28U)
/* Alarm 0 and alarm 1 registers, selected by the alarm id */
#define GD32_RTC_ALRMTD(reg, id) REG32((reg) + (((id) == 0U) ? 0x1CU : 0x20U))
#define GD32_RTC_ALRMSS(reg, id) REG32((reg) + (((id) == 0U) ? 0x44U : 0x48U))

/* BCD fields of the time and date registers */
#define GD32_RTC_TIME_SEC_POS  0U
#define GD32_RTC_TIME_SEC_MSK  0x7FU
#define GD32_RTC_TIME_MIN_POS  8U
#define GD32_RTC_TIME_MIN_MSK  0x7FU
#define GD32_RTC_TIME_HOUR_POS 16U
#define GD32_RTC_TIME_HOUR_MSK 0x3FU
#define GD32_RTC_DATE_DAY_POS  0U
#define GD32_RTC_DATE_DAY_MSK  0x3FU
#define GD32_RTC_DATE_MON_POS  8U
#define GD32_RTC_DATE_MON_MSK  0x1FU
#define GD32_RTC_DATE_DOW_POS  13U
#define GD32_RTC_DATE_DOW_MSK  0x7U
#define GD32_RTC_DATE_YEAR_POS 16U
#define GD32_RTC_DATE_YEAR_MSK 0xFFU

/* The hardware counts years from 2000; struct rtc_time from 1900. */
#define GD32_RTC_YEAR_BASE   2000
#define TM_YEAR_BASE         1900
#define GD32_RTC_TM_YEAR_MIN (GD32_RTC_YEAR_BASE - TM_YEAR_BASE)
#define GD32_RTC_TM_YEAR_MAX (GD32_RTC_TM_YEAR_MIN + 99)

/* Hardware weekday: 1 = Monday ... 7 = Sunday; struct rtc_time: 0 = Sunday */
#define GD32_RTC_DOW_SUNDAY 7U

#define GD32_RTC_PSC_ASYNC_MAX 0x7FU
#define GD32_RTC_PSC_SYNC_MAX  0x7FFFU

/* Default prescalers dividing each clock source down to 1 Hz */
#define GD32_RTC_IRC32K_ASYNC 31U
#define GD32_RTC_IRC32K_SYNC  999U
#define GD32_RTC_LXTAL_ASYNC  127U
#define GD32_RTC_LXTAL_SYNC   255U

#define GD32_RTC_SET_TIME_MASK                                                                     \
	(RTC_ALARM_TIME_MASK_SECOND | RTC_ALARM_TIME_MASK_MINUTE | RTC_ALARM_TIME_MASK_HOUR |      \
	 RTC_ALARM_TIME_MASK_MONTHDAY | RTC_ALARM_TIME_MASK_MONTH | RTC_ALARM_TIME_MASK_YEAR)

/* Init mode entry and shadow register sync take a few RTC clock cycles */
#define GD32_RTC_FLAG_TIMEOUT_US 20000U
#define GD32_RTC_FLAG_POLL_US    10U

#ifdef CONFIG_RTC_ALARM
#define GD32_RTC_ALARM_COUNT 2U

/* The alarm output of the RTC drives EXTI line 17 */
#define GD32_RTC_EXTI_LINE_ALARM 17U

/*
 * The alarm compares seconds, minutes, hours and the day of the month or
 * of the week, each one optionally.
 */
#define GD32_RTC_SUPPORTED_ALARM_FIELDS                                                            \
	(RTC_ALARM_TIME_MASK_SECOND | RTC_ALARM_TIME_MASK_MINUTE | RTC_ALARM_TIME_MASK_HOUR |      \
	 RTC_ALARM_TIME_MASK_MONTHDAY | RTC_ALARM_TIME_MASK_WEEKDAY)

#define GD32_RTC_ALRMTD_SEC_POS  0U
#define GD32_RTC_ALRMTD_SEC_MSK  0x7FU
#define GD32_RTC_ALRMTD_MIN_POS  8U
#define GD32_RTC_ALRMTD_MIN_MSK  0x7FU
#define GD32_RTC_ALRMTD_HOUR_POS 16U
#define GD32_RTC_ALRMTD_HOUR_MSK 0x3FU
#define GD32_RTC_ALRMTD_DAY_POS  24U
#define GD32_RTC_ALRMTD_DAY_MSK  0x3FU

struct rtc_gd32_alarm {
	rtc_alarm_callback callback;
	void *user_data;
	bool pending;
};
#endif /* CONFIG_RTC_ALARM */

enum gd32_rtc_clock_source {
	GD32_RTC_CLOCK_IRC32K,
	GD32_RTC_CLOCK_LXTAL,
};

struct rtc_gd32_config {
	uint32_t reg;
	uint16_t clkid;
	enum gd32_rtc_clock_source clock_source;
	int32_t async_prescaler;
	int32_t sync_prescaler;
};

struct rtc_gd32_data {
	struct k_mutex lock;
	uint32_t sync_prescaler;
#ifdef CONFIG_RTC_ALARM
	struct k_spinlock alarm_lock;
	struct rtc_gd32_alarm alarms[GD32_RTC_ALARM_COUNT];
#endif /* CONFIG_RTC_ALARM */
};

static void rtc_gd32_unlock(const struct rtc_gd32_config *cfg)
{
	GD32_RTC_WPK(cfg->reg) = RTC_UNLOCK_KEY1;
	GD32_RTC_WPK(cfg->reg) = RTC_UNLOCK_KEY2;
}

static void rtc_gd32_lock(const struct rtc_gd32_config *cfg)
{
	GD32_RTC_WPK(cfg->reg) = RTC_LOCK_KEY;
}

static int rtc_gd32_wait_flag(const struct rtc_gd32_config *cfg, uint32_t flag)
{
	uint32_t waited = 0U;

	while ((GD32_RTC_STAT(cfg->reg) & flag) == 0U) {
		if (waited >= GD32_RTC_FLAG_TIMEOUT_US) {
			return -ETIMEDOUT;
		}
		k_busy_wait(GD32_RTC_FLAG_POLL_US);
		waited += GD32_RTC_FLAG_POLL_US;
	}

	return 0;
}

/* Must be called with the registers unlocked. */
static int rtc_gd32_enter_init_mode(const struct rtc_gd32_config *cfg)
{
	if ((GD32_RTC_STAT(cfg->reg) & RTC_STAT_INITF) != 0U) {
		return 0;
	}

	GD32_RTC_STAT(cfg->reg) |= RTC_STAT_INITM;

	return rtc_gd32_wait_flag(cfg, RTC_STAT_INITF);
}

static void rtc_gd32_exit_init_mode(const struct rtc_gd32_config *cfg)
{
	GD32_RTC_STAT(cfg->reg) &= ~RTC_STAT_INITM;
}

/* Wait until the shadow registers reflect the calendar again. */
static int rtc_gd32_sync(const struct rtc_gd32_config *cfg)
{
	if ((GD32_RTC_CTL(cfg->reg) & RTC_CTL_BPSHAD) != 0U) {
		return 0;
	}

	GD32_RTC_STAT(cfg->reg) &= ~RTC_STAT_RSYNF;

	return rtc_gd32_wait_flag(cfg, RTC_STAT_RSYNF);
}

static int rtc_gd32_set_time(const struct device *dev, const struct rtc_time *timeptr)
{
	const struct rtc_gd32_config *cfg = dev->config;
	struct rtc_gd32_data *data = dev->data;
	uint32_t time_reg;
	uint32_t date_reg;
	uint32_t dow;
	int ret;

	if (timeptr == NULL || !rtc_utils_validate_rtc_time(timeptr, GD32_RTC_SET_TIME_MASK)) {
		return -EINVAL;
	}

	if (timeptr->tm_year < GD32_RTC_TM_YEAR_MIN || timeptr->tm_year > GD32_RTC_TM_YEAR_MAX) {
		LOG_ERR("Year out of the 2000-2099 range supported by the hardware");
		return -EINVAL;
	}

	if (timeptr->tm_wday >= 0) {
		dow = (timeptr->tm_wday == 0) ? GD32_RTC_DOW_SUNDAY : (uint32_t)timeptr->tm_wday;
	} else {
		/* Derive the weekday from the date: 1970-01-01 was a Thursday. */
		int64_t days = timeutil_timegm64((const struct tm *)timeptr) / 86400;

		dow = (uint32_t)((days + 3) % 7) + 1U;
	}

	time_reg = ((uint32_t)bin2bcd(timeptr->tm_sec) << GD32_RTC_TIME_SEC_POS) |
		   ((uint32_t)bin2bcd(timeptr->tm_min) << GD32_RTC_TIME_MIN_POS) |
		   ((uint32_t)bin2bcd(timeptr->tm_hour) << GD32_RTC_TIME_HOUR_POS);
	date_reg = ((uint32_t)bin2bcd(timeptr->tm_mday) << GD32_RTC_DATE_DAY_POS) |
		   ((uint32_t)bin2bcd(timeptr->tm_mon + 1) << GD32_RTC_DATE_MON_POS) |
		   (dow << GD32_RTC_DATE_DOW_POS) |
		   ((uint32_t)bin2bcd(timeptr->tm_year - GD32_RTC_TM_YEAR_MIN)
		    << GD32_RTC_DATE_YEAR_POS);

	k_mutex_lock(&data->lock, K_FOREVER);

	rtc_gd32_unlock(cfg);

	ret = rtc_gd32_enter_init_mode(cfg);
	if (ret == 0) {
		GD32_RTC_TIME(cfg->reg) = time_reg;
		GD32_RTC_DATE(cfg->reg) = date_reg;
	}
	rtc_gd32_exit_init_mode(cfg);
	if (ret == 0) {
		ret = rtc_gd32_sync(cfg);
	}

	rtc_gd32_lock(cfg);

	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		LOG_ERR("RTC did not acknowledge the new time (%d)", ret);
	}

	return ret;
}

static int rtc_gd32_get_time(const struct device *dev, struct rtc_time *timeptr)
{
	const struct rtc_gd32_config *cfg = dev->config;
	struct rtc_gd32_data *data = dev->data;
	uint32_t subsecond;
	uint32_t time_reg;
	uint32_t date_reg;
	uint32_t dow;

	if (timeptr == NULL) {
		return -EINVAL;
	}

	if ((GD32_RTC_STAT(cfg->reg) & RTC_STAT_YCM) == 0U) {
		/* The calendar has never been set. */
		return -ENODATA;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	/*
	 * Reading the sub-second or time register freezes the date shadow
	 * register until the date register is read, so read them in this
	 * order to get a consistent timestamp.
	 */
	subsecond = GD32_RTC_SS(cfg->reg);
	time_reg = GD32_RTC_TIME(cfg->reg);
	date_reg = GD32_RTC_DATE(cfg->reg);

	k_mutex_unlock(&data->lock);

	timeptr->tm_sec = bcd2bin((time_reg >> GD32_RTC_TIME_SEC_POS) & GD32_RTC_TIME_SEC_MSK);
	timeptr->tm_min = bcd2bin((time_reg >> GD32_RTC_TIME_MIN_POS) & GD32_RTC_TIME_MIN_MSK);
	timeptr->tm_hour = bcd2bin((time_reg >> GD32_RTC_TIME_HOUR_POS) & GD32_RTC_TIME_HOUR_MSK);
	timeptr->tm_mday = bcd2bin((date_reg >> GD32_RTC_DATE_DAY_POS) & GD32_RTC_DATE_DAY_MSK);
	timeptr->tm_mon = bcd2bin((date_reg >> GD32_RTC_DATE_MON_POS) & GD32_RTC_DATE_MON_MSK) - 1;
	timeptr->tm_year = bcd2bin((date_reg >> GD32_RTC_DATE_YEAR_POS) & GD32_RTC_DATE_YEAR_MSK) +
			   GD32_RTC_TM_YEAR_MIN;

	dow = (date_reg >> GD32_RTC_DATE_DOW_POS) & GD32_RTC_DATE_DOW_MSK;
	timeptr->tm_wday = (dow == GD32_RTC_DOW_SUNDAY) ? 0 : (int)dow;

	timeptr->tm_yday = -1;
	timeptr->tm_isdst = -1;

	/* The sub-second register counts down from the synchronous prescaler. */
	if (subsecond > data->sync_prescaler) {
		subsecond = data->sync_prescaler;
	}
	timeptr->tm_nsec = (int)(((uint64_t)(data->sync_prescaler - subsecond) * NSEC_PER_SEC) /
				 (data->sync_prescaler + 1U));

	return 0;
}

#ifdef CONFIG_RTC_ALARM
static inline uint32_t rtc_gd32_alarm_en(uint16_t id)
{
	return (id == 0U) ? RTC_CTL_ALRM0EN : RTC_CTL_ALRM1EN;
}

static inline uint32_t rtc_gd32_alarm_ie(uint16_t id)
{
	return (id == 0U) ? RTC_CTL_ALRM0IE : RTC_CTL_ALRM1IE;
}

static inline uint32_t rtc_gd32_alarm_wf(uint16_t id)
{
	return (id == 0U) ? RTC_STAT_ALRM0WF : RTC_STAT_ALRM1WF;
}

static inline uint32_t rtc_gd32_alarm_flag(uint16_t id)
{
	return (id == 0U) ? RTC_STAT_ALRM0F : RTC_STAT_ALRM1F;
}

/* A set mask bit means the field takes part in the comparison; the register
 * mask bits mean the opposite.
 */
static uint32_t rtc_gd32_alarm_encode(uint16_t mask, const struct rtc_time *timeptr)
{
	uint32_t reg = 0U;

	if ((mask & RTC_ALARM_TIME_MASK_SECOND) != 0U) {
		reg |= (uint32_t)bin2bcd(timeptr->tm_sec) << GD32_RTC_ALRMTD_SEC_POS;
	} else {
		reg |= RTC_ALRMXTD_MSKS;
	}

	if ((mask & RTC_ALARM_TIME_MASK_MINUTE) != 0U) {
		reg |= (uint32_t)bin2bcd(timeptr->tm_min) << GD32_RTC_ALRMTD_MIN_POS;
	} else {
		reg |= RTC_ALRMXTD_MSKM;
	}

	if ((mask & RTC_ALARM_TIME_MASK_HOUR) != 0U) {
		reg |= (uint32_t)bin2bcd(timeptr->tm_hour) << GD32_RTC_ALRMTD_HOUR_POS;
	} else {
		reg |= RTC_ALRMXTD_MSKH;
	}

	if ((mask & RTC_ALARM_TIME_MASK_WEEKDAY) != 0U) {
		uint32_t dow =
			(timeptr->tm_wday == 0) ? GD32_RTC_DOW_SUNDAY : (uint32_t)timeptr->tm_wday;

		reg |= (dow << GD32_RTC_ALRMTD_DAY_POS) | RTC_ALRMXTD_DOWS;
	} else if ((mask & RTC_ALARM_TIME_MASK_MONTHDAY) != 0U) {
		reg |= (uint32_t)bin2bcd(timeptr->tm_mday) << GD32_RTC_ALRMTD_DAY_POS;
	} else {
		reg |= RTC_ALRMXTD_MSKD;
	}

	return reg;
}

static uint16_t rtc_gd32_alarm_decode(uint32_t reg, struct rtc_time *timeptr)
{
	uint16_t mask = 0U;

	if ((reg & RTC_ALRMXTD_MSKS) == 0U) {
		timeptr->tm_sec =
			bcd2bin((reg >> GD32_RTC_ALRMTD_SEC_POS) & GD32_RTC_ALRMTD_SEC_MSK);
		mask |= RTC_ALARM_TIME_MASK_SECOND;
	}

	if ((reg & RTC_ALRMXTD_MSKM) == 0U) {
		timeptr->tm_min =
			bcd2bin((reg >> GD32_RTC_ALRMTD_MIN_POS) & GD32_RTC_ALRMTD_MIN_MSK);
		mask |= RTC_ALARM_TIME_MASK_MINUTE;
	}

	if ((reg & RTC_ALRMXTD_MSKH) == 0U) {
		timeptr->tm_hour =
			bcd2bin((reg >> GD32_RTC_ALRMTD_HOUR_POS) & GD32_RTC_ALRMTD_HOUR_MSK);
		mask |= RTC_ALARM_TIME_MASK_HOUR;
	}

	if ((reg & RTC_ALRMXTD_MSKD) == 0U) {
		uint32_t day = (reg >> GD32_RTC_ALRMTD_DAY_POS) & GD32_RTC_ALRMTD_DAY_MSK;

		if ((reg & RTC_ALRMXTD_DOWS) != 0U) {
			timeptr->tm_wday = (day == GD32_RTC_DOW_SUNDAY) ? 0 : (int)day;
			mask |= RTC_ALARM_TIME_MASK_WEEKDAY;
		} else {
			timeptr->tm_mday = bcd2bin(day);
			mask |= RTC_ALARM_TIME_MASK_MONTHDAY;
		}
	}

	return mask;
}

static int rtc_gd32_alarm_get_supported_fields(const struct device *dev, uint16_t id,
					       uint16_t *mask)
{
	ARG_UNUSED(dev);

	if (id >= GD32_RTC_ALARM_COUNT || mask == NULL) {
		return -EINVAL;
	}

	*mask = GD32_RTC_SUPPORTED_ALARM_FIELDS;

	return 0;
}

static int rtc_gd32_alarm_set_time(const struct device *dev, uint16_t id, uint16_t mask,
				   const struct rtc_time *timeptr)
{
	const struct rtc_gd32_config *cfg = dev->config;
	struct rtc_gd32_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t reg = 0U;
	int ret = 0;

	if (id >= GD32_RTC_ALARM_COUNT) {
		return -EINVAL;
	}

	if (mask != 0U || timeptr != NULL) {
		if ((mask & ~GD32_RTC_SUPPORTED_ALARM_FIELDS) != 0U) {
			LOG_ERR("Unsupported alarm fields in mask 0x%04x", mask);
			return -EINVAL;
		}

		if ((mask & RTC_ALARM_TIME_MASK_WEEKDAY) != 0U &&
		    (mask & RTC_ALARM_TIME_MASK_MONTHDAY) != 0U) {
			LOG_ERR("The alarm matches the weekday or the day of the month, not both");
			return -EINVAL;
		}

		if (timeptr == NULL || !rtc_utils_validate_rtc_time(timeptr, mask)) {
			return -EINVAL;
		}

		reg = rtc_gd32_alarm_encode(mask, timeptr);
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	rtc_gd32_unlock(cfg);

	/* The alarm registers only take writes while the alarm is off */
	GD32_RTC_CTL(cfg->reg) &= ~(rtc_gd32_alarm_en(id) | rtc_gd32_alarm_ie(id));
	GD32_RTC_STAT(cfg->reg) &= ~rtc_gd32_alarm_flag(id);

	if (mask != 0U || timeptr != NULL) {
		ret = rtc_gd32_wait_flag(cfg, rtc_gd32_alarm_wf(id));
		if (ret == 0) {
			GD32_RTC_ALRMTD(cfg->reg, id) = reg;
			/* No sub-second comparison */
			GD32_RTC_ALRMSS(cfg->reg, id) = 0U;
			GD32_RTC_CTL(cfg->reg) |= rtc_gd32_alarm_en(id) | rtc_gd32_alarm_ie(id);
		}
	}

	rtc_gd32_lock(cfg);

	k_mutex_unlock(&data->lock);

	key = k_spin_lock(&data->alarm_lock);
	data->alarms[id].pending = false;
	k_spin_unlock(&data->alarm_lock, key);

	if (ret < 0) {
		LOG_ERR("Alarm %u did not accept the new setting (%d)", id, ret);
	}

	return ret;
}

static int rtc_gd32_alarm_get_time(const struct device *dev, uint16_t id, uint16_t *mask,
				   struct rtc_time *timeptr)
{
	const struct rtc_gd32_config *cfg = dev->config;

	if (id >= GD32_RTC_ALARM_COUNT || mask == NULL || timeptr == NULL) {
		return -EINVAL;
	}

	memset(timeptr, -1, sizeof(*timeptr));

	if ((GD32_RTC_CTL(cfg->reg) & rtc_gd32_alarm_en(id)) == 0U) {
		*mask = 0U;
		return 0;
	}

	*mask = rtc_gd32_alarm_decode(GD32_RTC_ALRMTD(cfg->reg, id), timeptr);

	return 0;
}

static int rtc_gd32_alarm_is_pending(const struct device *dev, uint16_t id)
{
	const struct rtc_gd32_config *cfg = dev->config;
	struct rtc_gd32_data *data = dev->data;
	k_spinlock_key_t key;
	int pending;

	if (id >= GD32_RTC_ALARM_COUNT) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->alarm_lock);

	pending = data->alarms[id].pending ? 1 : 0;
	data->alarms[id].pending = false;

	/* An alarm that fired with its interrupt off is only seen here */
	if ((GD32_RTC_STAT(cfg->reg) & rtc_gd32_alarm_flag(id)) != 0U) {
		rtc_gd32_unlock(cfg);
		GD32_RTC_STAT(cfg->reg) &= ~rtc_gd32_alarm_flag(id);
		rtc_gd32_lock(cfg);
		pending = 1;
	}

	k_spin_unlock(&data->alarm_lock, key);

	return pending;
}

static int rtc_gd32_alarm_set_callback(const struct device *dev, uint16_t id,
				       rtc_alarm_callback callback, void *user_data)
{
	struct rtc_gd32_data *data = dev->data;
	k_spinlock_key_t key;

	if (id >= GD32_RTC_ALARM_COUNT) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->alarm_lock);
	data->alarms[id].callback = callback;
	data->alarms[id].user_data = user_data;
	k_spin_unlock(&data->alarm_lock, key);

	return 0;
}

static void rtc_gd32_alarm_isr(uint8_t line, void *user)
{
	const struct device *dev = user;
	const struct rtc_gd32_config *cfg = dev->config;
	struct rtc_gd32_data *data = dev->data;

	ARG_UNUSED(line);

	for (uint16_t id = 0U; id < GD32_RTC_ALARM_COUNT; id++) {
		rtc_alarm_callback callback;
		void *user_data;
		k_spinlock_key_t key;

		if ((GD32_RTC_STAT(cfg->reg) & rtc_gd32_alarm_flag(id)) == 0U) {
			continue;
		}

		rtc_gd32_unlock(cfg);
		GD32_RTC_STAT(cfg->reg) &= ~rtc_gd32_alarm_flag(id);
		rtc_gd32_lock(cfg);

		key = k_spin_lock(&data->alarm_lock);
		data->alarms[id].pending = true;
		callback = data->alarms[id].callback;
		user_data = data->alarms[id].user_data;
		k_spin_unlock(&data->alarm_lock, key);

		if (callback != NULL) {
			callback(dev, id, user_data);
		}
	}
}

static int rtc_gd32_alarm_init(const struct device *dev)
{
	int ret;

	ret = gd32_exti_configure(GD32_RTC_EXTI_LINE_ALARM, rtc_gd32_alarm_isr, (void *)dev);
	if (ret < 0) {
		LOG_ERR("EXTI line %u is taken (%d)", GD32_RTC_EXTI_LINE_ALARM, ret);
		return ret;
	}

	gd32_exti_trigger(GD32_RTC_EXTI_LINE_ALARM, GD32_EXTI_TRIG_RISING);
	gd32_exti_enable(GD32_RTC_EXTI_LINE_ALARM);

	return 0;
}
#else  /* CONFIG_RTC_ALARM */
static inline int rtc_gd32_alarm_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}
#endif /* CONFIG_RTC_ALARM */

static DEVICE_API(rtc, rtc_gd32_driver_api) = {
	.set_time = rtc_gd32_set_time,
	.get_time = rtc_gd32_get_time,
#ifdef CONFIG_RTC_ALARM
	.alarm_get_supported_fields = rtc_gd32_alarm_get_supported_fields,
	.alarm_set_time = rtc_gd32_alarm_set_time,
	.alarm_get_time = rtc_gd32_alarm_get_time,
	.alarm_is_pending = rtc_gd32_alarm_is_pending,
	.alarm_set_callback = rtc_gd32_alarm_set_callback,
#endif /* CONFIG_RTC_ALARM */
};

static int rtc_gd32_start_oscillator(rcu_osci_type_enum osci)
{
	rcu_osci_on(osci);
	if (rcu_osci_stab_wait(osci) != SUCCESS) {
		LOG_ERR("RTC clock source did not start");
		return -EIO;
	}

	return 0;
}

/*
 * The backup domain latches the RTC clock selection and enable until it
 * is reset, so a running RTC is left alone: it may be keeping time from
 * before this boot. The oscillator itself is not part of the backup
 * domain, so it has to be (re)started on every boot.
 */
static int rtc_gd32_enable_clock(const struct rtc_gd32_config *cfg, uint32_t *source)
{
	uint32_t selected = RCU_BDCTL & RCU_BDCTL_RTCSRC;
	rcu_osci_type_enum osci;
	int ret;

	if ((RCU_BDCTL & RCU_BDCTL_RTCEN) != 0U && selected != RCU_RTCSRC_NONE) {
		if (selected != RCU_RTCSRC_LXTAL && selected != RCU_RTCSRC_IRC32K) {
			LOG_ERR("Unsupported RTC clock source already selected (%#x)", selected);
			return -ENOTSUP;
		}

		osci = (selected == RCU_RTCSRC_LXTAL) ? RCU_LXTAL : RCU_IRC32K;
		ret = rtc_gd32_start_oscillator(osci);
		if (ret < 0) {
			return ret;
		}

		*source = selected;
		return 0;
	}

	if (cfg->clock_source == GD32_RTC_CLOCK_LXTAL) {
		osci = RCU_LXTAL;
		selected = RCU_RTCSRC_LXTAL;
	} else {
		osci = RCU_IRC32K;
		selected = RCU_RTCSRC_IRC32K;
	}

	ret = rtc_gd32_start_oscillator(osci);
	if (ret < 0) {
		return ret;
	}

	rcu_rtc_clock_config(selected);
	rcu_periph_clock_enable(RCU_RTC);

	*source = selected;
	return 0;
}

static int rtc_gd32_init(const struct device *dev)
{
	const struct rtc_gd32_config *cfg = dev->config;
	struct rtc_gd32_data *data = dev->data;
	uint32_t async_prescaler;
	uint32_t sync_prescaler;
	uint32_t source;
	int ret;

	k_mutex_init(&data->lock);

	/* The PMU gates write access to the backup domain, where the RTC lives. */
	ret = clock_control_on(GD32_CLOCK_CONTROLLER, (clock_control_subsys_t)&cfg->clkid);
	if (ret < 0) {
		return ret;
	}

	pmu_backup_write_enable();

	ret = rtc_gd32_enable_clock(cfg, &source);
	if (ret < 0) {
		return ret;
	}

	/* Pick the prescalers for the source that is actually selected. */
	if (source == RCU_RTCSRC_LXTAL) {
		async_prescaler = GD32_RTC_LXTAL_ASYNC;
		sync_prescaler = GD32_RTC_LXTAL_SYNC;
	} else {
		async_prescaler = GD32_RTC_IRC32K_ASYNC;
		sync_prescaler = GD32_RTC_IRC32K_SYNC;
	}

	if ((cfg->clock_source == GD32_RTC_CLOCK_LXTAL) != (source == RCU_RTCSRC_LXTAL)) {
		LOG_WRN("RTC clock source was already selected, keeping it");
	}

	if (cfg->async_prescaler >= 0) {
		async_prescaler = (uint32_t)cfg->async_prescaler;
	}
	if (cfg->sync_prescaler >= 0) {
		sync_prescaler = (uint32_t)cfg->sync_prescaler;
	}

	rtc_gd32_unlock(cfg);

	ret = rtc_gd32_sync(cfg);
	if (ret == 0 && (GD32_RTC_STAT(cfg->reg) & RTC_STAT_YCM) == 0U) {
		/* First use since the backup domain reset: program the divider. */
		ret = rtc_gd32_enter_init_mode(cfg);
		if (ret == 0) {
			GD32_RTC_PSC(cfg->reg) = (async_prescaler << 16) | sync_prescaler;
			/* 24-hour format */
			GD32_RTC_CTL(cfg->reg) &= ~RTC_CTL_CS;
		}
		rtc_gd32_exit_init_mode(cfg);
	}

	rtc_gd32_lock(cfg);

	if (ret < 0) {
		LOG_ERR("RTC initialization failed (%d)", ret);
		return ret;
	}

	data->sync_prescaler = (GD32_RTC_PSC(cfg->reg) & RTC_PSC_FACTOR_S);

	if (IS_ENABLED(CONFIG_RTC_ALARM)) {
		return rtc_gd32_alarm_init(dev);
	}

	return 0;
}

#define RTC_GD32_INIT(inst)                                                                        \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, async_prescaler, 0) <= GD32_RTC_PSC_ASYNC_MAX,          \
		     "async-prescaler out of range");                                              \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, sync_prescaler, 0) <= GD32_RTC_PSC_SYNC_MAX,            \
		     "sync-prescaler out of range");                                               \
                                                                                                   \
	static const struct rtc_gd32_config rtc_gd32_cfg_##inst = {                                \
		.reg = DT_INST_REG_ADDR(inst),                                                     \
		.clkid = DT_INST_CLOCKS_CELL(inst, id),                                            \
		.clock_source = DT_INST_ENUM_IDX(inst, clock_source),                              \
		.async_prescaler = DT_INST_PROP_OR(inst, async_prescaler, -1),                     \
		.sync_prescaler = DT_INST_PROP_OR(inst, sync_prescaler, -1),                       \
	};                                                                                         \
                                                                                                   \
	static struct rtc_gd32_data rtc_gd32_data_##inst;                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, rtc_gd32_init, NULL, &rtc_gd32_data_##inst,                    \
			      &rtc_gd32_cfg_##inst, POST_KERNEL, CONFIG_RTC_INIT_PRIORITY,         \
			      &rtc_gd32_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RTC_GD32_INIT)
