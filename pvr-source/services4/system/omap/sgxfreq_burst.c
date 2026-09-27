/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * burst: a frame-aware SGX frequency governor.
 *
 * GPU busy time is the span from sgxfreq_notif_sgx_active() (a command
 * reaches an idle SGX) to sgxfreq_notif_sgx_idle() (the SGX stays idle for
 * SYS_SGX_ACTIVE_POWER_LATENCY_MS and powers down), accounted in
 * microseconds. sgxfreq_notif_sgx_frame_done() (PVRSRVCommandCompleteKM)
 * closes a frame.
 *
 * - After each frame the governor picks the lowest OPP whose predicted busy
 *   time, frame_busy * cur_freq / f, fits target_busy_us. It steps up at
 *   once and steps down only after down_frames consecutive frames agree.
 * - A boost window (boost_ms, written by the power HAL on input) raises the
 *   request to burst_freq on the next idle-to-active edge and holds that
 *   floor until the window ends.
 * - After park_ms without activity and outside a boost window, the request
 *   drops to the lowest OPP, which also lets the CORE voltage domain return
 *   to OPP50.
 *
 * - An up-step must pay off: within BURST_PROBE_FRAMES frames the smoothed
 *   frame busy time has to fall by at least sensitivity_pct percent. When it
 *   does not, the frame is bound by something other than the SGX clock
 *   (fence waits, power-up, CPU), so the governor returns to the lower OPP
 *   and ignores step-ups and boost windows for insensitive_hold_ms.
 *
 * Every request passes through sgxfreq_set_freq_request(), so
 * frequency_limit (the thermal or user cap) bounds all of them.
 */

#include <linux/ktime.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include "sgxfreq.h"

#define BURST_DEFAULT_TARGET_BUSY_US	4000
#define BURST_DEFAULT_PARK_MS		50
#define BURST_DEFAULT_DOWN_FRAMES	3
#define BURST_MIN_FRAME_US		2000
#define BURST_HIST_BUCKETS		7
#define BURST_PROBE_FRAMES		4
#define BURST_DEFAULT_SENSITIVITY_PCT	15
#define BURST_DEFAULT_INSENSITIVE_HOLD_MS	2000

static int burst_start(struct sgxfreq_sgx_data *data);
static void burst_stop(void);
static void burst_active(void);
static void burst_idle(void);
static void burst_frame_done(void);
static void burst_park(struct work_struct *work);

static struct sgxfreq_governor burst_gov = {
	.name = "burst",
	.gov_start = burst_start,
	.gov_stop = burst_stop,
	.sgx_active = burst_active,
	.sgx_idle = burst_idle,
	.sgx_frame_done = burst_frame_done,
};

static struct burst_data {
	unsigned int target_busy_us;
	unsigned int park_ms;
	unsigned int down_frames;
	unsigned long burst_freq;
	bool active;
	ktime_t active_start;
	ktime_t frame_start;
	ktime_t boost_until;
	u64 frame_busy_us;
	u64 last_frame_busy_us;
	unsigned int down_votes;
	unsigned int sensitivity_pct;
	unsigned int insensitive_hold_ms;
	u64 busy_ewma_us;
	unsigned long probe_from_freq;
	u64 probe_from_busy_us;
	unsigned int probe_frames;
	u64 probe_sum_us;
	ktime_t insensitive_until;
	/* Frame busy histogram, bucket upper bounds 1, 2, 4, 8, 16, 32 ms, inf. */
	unsigned long hist[BURST_HIST_BUCKETS];
	struct delayed_work park_work;
	struct mutex mutex;
} bd;

static bool burst_insensitive(ktime_t now)
{
	return ktime_to_us(ktime_sub(bd.insensitive_until, now)) > 0;
}

static bool burst_boosted(ktime_t now)
{
	return ktime_to_us(ktime_sub(bd.boost_until, now)) > 0 &&
		!burst_insensitive(now);
}

static unsigned long burst_pick(u64 busy_us, unsigned long cur)
{
	unsigned long *list;
	int cnt, i;

	cnt = sgxfreq_get_freq_list(&list);
	for (i = 0; i < cnt; i++) {
		/* Predicted busy at list[i] scales with cur / list[i]. */
		if (busy_us * cur <= (u64)bd.target_busy_us * list[i])
			return list[i];
	}
	return list[cnt - 1];
}

static void burst_hist_add(u64 busy_us)
{
	int b = 0;
	u64 bound = 1000;

	while (b < BURST_HIST_BUCKETS - 1 && busy_us > bound) {
		bound <<= 1;
		b++;
	}
	bd.hist[b]++;
}

/*********************** begin sysfs interface ***********************/

extern struct kobject *sgxfreq_kobj;

#define BURST_UINT_ATTR(field, minval)					\
static ssize_t show_burst_##field(struct device *dev,			\
	struct device_attribute *attr, char *buf)			\
{									\
	return sprintf(buf, "%u\n", bd.field);				\
}									\
static ssize_t store_burst_##field(struct device *dev,			\
	struct device_attribute *attr, const char *buf, size_t count)	\
{									\
	unsigned int v;							\
									\
	if (kstrtouint(buf, 0, &v) || v < (minval))			\
		return -EINVAL;						\
	mutex_lock(&bd.mutex);						\
	bd.field = v;							\
	mutex_unlock(&bd.mutex);					\
	return count;							\
}									\
static DEVICE_ATTR(field, 0644, show_burst_##field, store_burst_##field)

BURST_UINT_ATTR(target_busy_us, 500);
BURST_UINT_ATTR(park_ms, 1);
BURST_UINT_ATTR(down_frames, 1);
BURST_UINT_ATTR(sensitivity_pct, 0);
BURST_UINT_ATTR(insensitive_hold_ms, 0);

static ssize_t show_burst_freq(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", bd.burst_freq);
}

static ssize_t store_burst_freq(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long v;

	if (kstrtoul(buf, 0, &v))
		return -EINVAL;
	mutex_lock(&bd.mutex);
	bd.burst_freq = sgxfreq_get_freq_ceil(v);
	mutex_unlock(&bd.mutex);
	return count;
}

static DEVICE_ATTR(burst_freq, 0644, show_burst_freq, store_burst_freq);

static ssize_t show_burst_boost_ms(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	s64 left = ktime_to_ms(ktime_sub(bd.boost_until, ktime_get()));

	return sprintf(buf, "%lld\n", left > 0 ? left : 0);
}

/*
 * A boost window raises the floor to burst_freq. The request takes effect
 * at once when the SGX is already active, otherwise at the next
 * idle-to-active edge, so an idle GPU stays at its parked OPP.
 */
static ssize_t store_burst_boost_ms(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int ms;
	ktime_t until;

	if (kstrtouint(buf, 0, &ms) || ms > 10000)
		return -EINVAL;
	until = ktime_add_ns(ktime_get(), (u64)ms * NSEC_PER_MSEC);
	mutex_lock(&bd.mutex);
	if (ktime_to_us(ktime_sub(until, bd.boost_until)) > 0)
		bd.boost_until = until;
	if (bd.active && sgxfreq_get_freq_request() < bd.burst_freq)
		sgxfreq_set_freq_request(bd.burst_freq);
	mutex_unlock(&bd.mutex);
	return count;
}

static DEVICE_ATTR(boost_ms, 0644, show_burst_boost_ms, store_burst_boost_ms);

static ssize_t show_burst_last_frame_busy_us(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%llu\n", bd.last_frame_busy_us);
}

static DEVICE_ATTR(last_frame_busy_us, 0444,
	show_burst_last_frame_busy_us, NULL);

static ssize_t show_burst_frame_busy_hist(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf,
		"<=1ms %lu\n<=2ms %lu\n<=4ms %lu\n<=8ms %lu\n<=16ms %lu\n<=32ms %lu\n>32ms %lu\n",
		bd.hist[0], bd.hist[1], bd.hist[2], bd.hist[3], bd.hist[4],
		bd.hist[5], bd.hist[6]);
}

static DEVICE_ATTR(frame_busy_hist, 0444, show_burst_frame_busy_hist, NULL);

static struct attribute *burst_attributes[] = {
	&dev_attr_target_busy_us.attr,
	&dev_attr_park_ms.attr,
	&dev_attr_down_frames.attr,
	&dev_attr_sensitivity_pct.attr,
	&dev_attr_insensitive_hold_ms.attr,
	&dev_attr_burst_freq.attr,
	&dev_attr_boost_ms.attr,
	&dev_attr_last_frame_busy_us.attr,
	&dev_attr_frame_busy_hist.attr,
	NULL
};

static struct attribute_group burst_attr_group = {
	.attrs = burst_attributes,
	.name = "burst",
};

/************************ end sysfs interface ************************/

int burst_init(void)
{
	mutex_init(&bd.mutex);
	INIT_DELAYED_WORK(&bd.park_work, burst_park);
	return sgxfreq_register_governor(&burst_gov);
}

int burst_deinit(void)
{
	return 0;
}

static int burst_start(struct sgxfreq_sgx_data *data)
{
	ktime_t now = ktime_get();

	bd.target_busy_us = BURST_DEFAULT_TARGET_BUSY_US;
	bd.park_ms = BURST_DEFAULT_PARK_MS;
	bd.down_frames = BURST_DEFAULT_DOWN_FRAMES;
	bd.burst_freq = sgxfreq_get_freq_max();
	bd.active = data->active;
	bd.active_start = now;
	bd.frame_start = now;
	bd.boost_until = now;
	bd.frame_busy_us = 0;
	bd.last_frame_busy_us = 0;
	bd.down_votes = 0;
	bd.sensitivity_pct = BURST_DEFAULT_SENSITIVITY_PCT;
	bd.insensitive_hold_ms = BURST_DEFAULT_INSENSITIVE_HOLD_MS;
	bd.busy_ewma_us = 0;
	bd.probe_from_freq = 0;
	bd.probe_frames = 0;
	bd.insensitive_until = now;
	memset(bd.hist, 0, sizeof(bd.hist));

	sgxfreq_set_freq_request(sgxfreq_get_freq_min());
	return sysfs_create_group(sgxfreq_kobj, &burst_attr_group);
}

static void burst_stop(void)
{
	cancel_delayed_work_sync(&bd.park_work);
	sysfs_remove_group(sgxfreq_kobj, &burst_attr_group);
}

static void burst_active(void)
{
	ktime_t now = ktime_get();

	cancel_delayed_work(&bd.park_work);
	mutex_lock(&bd.mutex);
	bd.active = true;
	bd.active_start = now;
	if (burst_boosted(now) && sgxfreq_get_freq_request() < bd.burst_freq)
		sgxfreq_set_freq_request(bd.burst_freq);
	mutex_unlock(&bd.mutex);
}

static void burst_idle(void)
{
	ktime_t now = ktime_get();

	mutex_lock(&bd.mutex);
	if (bd.active)
		bd.frame_busy_us += ktime_to_us(ktime_sub(now, bd.active_start));
	bd.active = false;
	mutex_unlock(&bd.mutex);
	schedule_delayed_work(&bd.park_work, msecs_to_jiffies(bd.park_ms));
}

static void burst_frame_done(void)
{
	ktime_t now = ktime_get();
	unsigned long cur, want;
	u64 busy;

	mutex_lock(&bd.mutex);
	/* Completions closer together than BURST_MIN_FRAME_US belong to one frame. */
	if (ktime_to_us(ktime_sub(now, bd.frame_start)) < BURST_MIN_FRAME_US)
		goto out;
	if (bd.active) {
		bd.frame_busy_us += ktime_to_us(ktime_sub(now, bd.active_start));
		bd.active_start = now;
	}
	busy = bd.frame_busy_us;
	bd.frame_busy_us = 0;
	bd.frame_start = now;
	bd.last_frame_busy_us = busy;
	burst_hist_add(busy);
	bd.busy_ewma_us = bd.busy_ewma_us ? (3 * bd.busy_ewma_us + busy) / 4 : busy;

	if (bd.probe_from_freq) {
		bd.probe_sum_us += busy;
		if (++bd.probe_frames >= BURST_PROBE_FRAMES) {
			/* Keep the higher OPP only when busy fell by sensitivity_pct. */
			if (bd.probe_sum_us * 100 > (u64)BURST_PROBE_FRAMES *
			    bd.probe_from_busy_us * (100 - bd.sensitivity_pct)) {
				bd.insensitive_until = ktime_add_ns(now,
					(u64)bd.insensitive_hold_ms * NSEC_PER_MSEC);
				sgxfreq_set_freq_request(bd.probe_from_freq);
			}
			bd.probe_from_freq = 0;
		}
		goto out;
	}

	cur = sgxfreq_get_freq();
	want = burst_pick(busy, cur ? cur : sgxfreq_get_freq_min());
	if (burst_boosted(now) && want < bd.burst_freq)
		want = bd.burst_freq;

	if (want > sgxfreq_get_freq_request()) {
		bd.down_votes = 0;
		if (burst_insensitive(now))
			goto out;
		bd.probe_from_freq = sgxfreq_get_freq_request();
		bd.probe_from_busy_us = bd.busy_ewma_us;
		bd.probe_frames = 0;
		bd.probe_sum_us = 0;
		sgxfreq_set_freq_request(want);
	} else if (want < sgxfreq_get_freq_request()) {
		if (++bd.down_votes >= bd.down_frames) {
			bd.down_votes = 0;
			sgxfreq_set_freq_request(want);
		}
	} else {
		bd.down_votes = 0;
	}
out:
	mutex_unlock(&bd.mutex);
}

static void burst_park(struct work_struct *work)
{
	mutex_lock(&bd.mutex);
	if (!bd.active && !burst_boosted(ktime_get())) {
		bd.down_votes = 0;
		sgxfreq_set_freq_request(sgxfreq_get_freq_min());
	}
	mutex_unlock(&bd.mutex);
}
