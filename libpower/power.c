/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "TI OMAP PowerHAL"
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#define CPUFREQ_INTERACTIVE "/sys/devices/system/cpu/cpufreq/interactive/"
#define CPUFREQ_CPU0 "/sys/devices/system/cpu/cpu0/cpufreq/"
#define BOOSTPULSE_PATH (CPUFREQ_INTERACTIVE "boostpulse")
#define SCALINGMAXFREQ_PATH (CPUFREQ_CPU0 "scaling_max_freq")

#define SGXFREQ "/sys/devices/platform/omap/pvrsrvkm.0/sgxfreq/"
#define SGX_FREQ_LIST_PATH (SGXFREQ "frequency_list")
#define SGX_LIMIT_PATH (SGXFREQ "frequency_limit")
#define SGX_BOOST_PATH (SGXFREQ "burst/boost_ms")
#define CPU_DMA_LATENCY_PATH "/dev/cpu_dma_latency"

/*
 * OMAP4 C2-C4 exit in 1.1-1.5 ms (cpuidle44xx.c); a PM QoS bound below that
 * keeps both CPUs in C1 (WFI, 4 us exit) for the length of a boost, so a
 * vsync or input wakeup runs at once.
 */
#define BOOST_QOS_LATENCY_US 500
#define BOOST_DEFAULT_MS 120
#define BOOST_MAX_MS 2000
#define LAUNCH_BOOST_MS 2000

#define MAX_FREQ_NUMBER 10
#define NOM_FREQ_INDEX 3
#define FREQ_BUF_SIZE 10

static int freq_num;
static char *freq_list[MAX_FREQ_NUMBER];
static char nom_freq[FREQ_BUF_SIZE] = "\0";
static char max_freq[FREQ_BUF_SIZE] = "\0";


struct omap_power_module {
    struct power_module base;
    pthread_mutex_t lock;
    int boostpulse_fd;
    int boostpulse_warned;
    int inited;
    int screen_state;
    int sgx_boost_fd;
    int qos_fd;
    int boost_thread_started;
    pthread_cond_t boost_cond;
    struct timespec boost_until;
    char sgx_limit[FREQ_BUF_SIZE];
    char sgx_min[FREQ_BUF_SIZE];
};

static int str_to_tokens(char *str, char **token, int max_token_idx)
{
    char *pos, *start_pos = str;
    char *token_pos;
    int token_idx = 0;

    if (!str || !token || !max_token_idx)
        return 0;

    do {
        token_pos = strtok_r(start_pos, " \t\r\n", &pos);

        if (token_pos)
            token[token_idx++] = strdup(token_pos);
        start_pos = NULL;
    } while (token_pos && token_idx < max_token_idx);

    return token_idx;
}

static void sysfs_write(char *path, char *s)
{
    char buf[80];
    int len;
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);
    }

    close(fd);
}

static int sysfs_read(char *path, char *s, int s_size)
{
    char buf[80];
    int len;
    int fd;

    if (!path || !s || !s_size)
        return -1;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return fd;
    }

    len = read(fd, s, s_size-1);
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error reading from %s: %s\n", path, buf);
    } else {
        s[len] = '\0';
    }

    close(fd);
    return len;
}

static int timespec_after(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec > b->tv_sec ||
        (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

/*
 * Holds the cpuidle PM QoS request while a boost window is open and drops it
 * at boost_until; the kernel removes the request when the fd closes.
 */
static void *boost_thread(void *arg)
{
    struct omap_power_module *omap_device = arg;
    struct timespec now;

    pthread_mutex_lock(&omap_device->lock);
    for (;;) {
        while (omap_device->qos_fd < 0)
            pthread_cond_wait(&omap_device->boost_cond, &omap_device->lock);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!timespec_after(&omap_device->boost_until, &now)) {
            close(omap_device->qos_fd);
            omap_device->qos_fd = -1;
            continue;
        }
        pthread_cond_timedwait(&omap_device->boost_cond, &omap_device->lock,
                               &omap_device->boost_until);
    }
    return NULL;
}

/*
 * One boost raises three domains for ms: the MPU through the interactive
 * governor's boostpulse, the GPU and CORE through the sgxfreq burst
 * governor's boost window, and CPU wakeup latency through PM QoS.
 */
static void omap_power_boost(struct omap_power_module *omap_device, int ms)
{
    char buf[16];
    struct timespec until;
    int32_t qos = BOOST_QOS_LATENCY_US;
    int len;

    if (ms <= 0)
        ms = BOOST_DEFAULT_MS;
    if (ms > BOOST_MAX_MS)
        ms = BOOST_MAX_MS;

    pthread_mutex_lock(&omap_device->lock);

    if (omap_device->boostpulse_fd >= 0)
        len = write(omap_device->boostpulse_fd, "1", 1);

    if (omap_device->sgx_boost_fd >= 0) {
        len = snprintf(buf, sizeof(buf), "%d", ms);
        len = write(omap_device->sgx_boost_fd, buf, len);
    }
    (void)len;

    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    if (timespec_after(&until, &omap_device->boost_until))
        omap_device->boost_until = until;

    if (omap_device->qos_fd < 0) {
        omap_device->qos_fd = open(CPU_DMA_LATENCY_PATH, O_WRONLY | O_CLOEXEC);
        if (omap_device->qos_fd >= 0 &&
            write(omap_device->qos_fd, &qos, sizeof(qos)) != sizeof(qos)) {
            close(omap_device->qos_fd);
            omap_device->qos_fd = -1;
        }
    }
    if (omap_device->boost_thread_started)
        pthread_cond_signal(&omap_device->boost_cond);

    pthread_mutex_unlock(&omap_device->lock);
}

static void omap_power_boost_end(struct omap_power_module *omap_device)
{
    pthread_mutex_lock(&omap_device->lock);
    clock_gettime(CLOCK_MONOTONIC, &omap_device->boost_until);
    if (omap_device->boost_thread_started)
        pthread_cond_signal(&omap_device->boost_cond);
    pthread_mutex_unlock(&omap_device->lock);
}

static void omap_power_init_boost(struct omap_power_module *omap_device)
{
    char buf[MAX_FREQ_NUMBER * FREQ_BUF_SIZE];
    char *tok[MAX_FREQ_NUMBER];
    pthread_condattr_t attr;
    pthread_t thread;
    int n;

    /* init.omap4.rc selects the burst governor and hands its nodes to system. */
    if (sysfs_read(SGX_LIMIT_PATH, omap_device->sgx_limit,
                   sizeof(omap_device->sgx_limit)) <= 0)
        omap_device->sgx_limit[0] = '\0';
    if (sysfs_read(SGX_FREQ_LIST_PATH, buf, sizeof(buf)) > 0) {
        n = str_to_tokens(buf, tok, MAX_FREQ_NUMBER);
        if (n > 0)
            strlcpy(omap_device->sgx_min, tok[0], sizeof(omap_device->sgx_min));
        while (n-- > 0)
            free(tok[n]);
    }

    omap_device->boostpulse_fd = open(BOOSTPULSE_PATH, O_WRONLY | O_CLOEXEC);
    omap_device->sgx_boost_fd = open(SGX_BOOST_PATH, O_WRONLY | O_CLOEXEC);
    ALOGI("boost domains: cpu %s, gpu %s", omap_device->boostpulse_fd >= 0 ?
          "on" : "off", omap_device->sgx_boost_fd >= 0 ? "on" : "off");

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&omap_device->boost_cond, &attr);
    pthread_condattr_destroy(&attr);
    if (pthread_create(&thread, NULL, boost_thread, omap_device) == 0) {
        pthread_detach(thread);
        omap_device->boost_thread_started = 1;
    }
}

static void omap_power_init(struct power_module *module)
{
    struct omap_power_module *omap_device =
                                   (struct omap_power_module *) module;
    int tmp;
    char freq_buf[MAX_FREQ_NUMBER * FREQ_BUF_SIZE];
    char target_loads[64];

    tmp = sysfs_read(CPUFREQ_CPU0 "scaling_available_frequencies",
                                                   freq_buf, sizeof(freq_buf));
    if (tmp <= 0)
        return;

    freq_num = str_to_tokens(freq_buf, freq_list, MAX_FREQ_NUMBER);
    if (!freq_num)
        return;

    strcpy(max_freq, freq_list[freq_num - 1]);
    tmp = (NOM_FREQ_INDEX > freq_num) ? freq_num : NOM_FREQ_INDEX;
    strcpy(nom_freq, freq_list[tmp - 1]);

    snprintf(target_loads, sizeof(target_loads), "70 %s:80 %s:99",
        freq_list[freq_num - 2], freq_list[freq_num - 1]);

    sysfs_write(CPUFREQ_INTERACTIVE "timer_rate", "20000");
    sysfs_write(CPUFREQ_INTERACTIVE "min_sample_time","60000");
    sysfs_write(CPUFREQ_INTERACTIVE "hispeed_freq", nom_freq);
    sysfs_write(CPUFREQ_INTERACTIVE "go_hispeed_load", "99");
    sysfs_write(CPUFREQ_INTERACTIVE "above_hispeed_delay", "80000");
    sysfs_write(CPUFREQ_INTERACTIVE "target_loads", target_loads);

    omap_power_init_boost(omap_device);

    ALOGI("Initialized successfully");
    omap_device->inited = 1;
}

static int boostpulse_open(struct omap_power_module *omap_device)
{
    char buf[80];

    pthread_mutex_lock(&omap_device->lock);

    if (omap_device->boostpulse_fd < 0) {
        omap_device->boostpulse_fd = open(BOOSTPULSE_PATH, O_WRONLY);

        if (omap_device->boostpulse_fd < 0) {
            if (!omap_device->boostpulse_warned) {
                strerror_r(errno, buf, sizeof(buf));
                ALOGE("Error opening %s: %s\n", BOOSTPULSE_PATH, buf);
                omap_device->boostpulse_warned = 1;
            }
        }
    }

    pthread_mutex_unlock(&omap_device->lock);
    return omap_device->boostpulse_fd;
}

static void omap_power_set_interactive(struct power_module *module,
                                               int on)
{
    struct omap_power_module *omap_device =
                                   (struct omap_power_module *) module;
    char buf[FREQ_BUF_SIZE];
    int len;

    if (!omap_device->inited || omap_device->screen_state == on)
        return;

    /*
     * Lower maximum frequency when screen is off.  CPU 0 and 1 share a
     * cpufreq policy.
     */

    /* Save the current setting to avoid overwriting custom user settings.
     * Note this must be avoided on boot, so we check previous on state. */
    if (omap_device->screen_state != -1) {
        len = sysfs_read(SCALINGMAXFREQ_PATH, buf, sizeof(buf));
        if (len > 0)
            strcpy(omap_device->screen_state ? max_freq : nom_freq, buf);
    }

    sysfs_write(SCALINGMAXFREQ_PATH, on ? max_freq : nom_freq);

    /* With the screen off the GPU serves no frame; cap it at its lowest OPP. */
    if (omap_device->sgx_limit[0] && omap_device->sgx_min[0])
        sysfs_write(SGX_LIMIT_PATH, on ? omap_device->sgx_limit : omap_device->sgx_min);

    /* Update state on exit to allow referencing the previous state above. */
    omap_device->screen_state = on;
}

static void omap_power_hint(struct power_module *module,
                                    power_hint_t hint, void *data)
{
    struct omap_power_module *omap_device =
            (struct omap_power_module *) module;

    if (!omap_device->inited)
        return;

    switch (hint) {
    case POWER_HINT_INTERACTION:
        if (boostpulse_open(omap_device) < 0 && omap_device->sgx_boost_fd < 0)
            break;
        omap_power_boost(omap_device, data ? *(int32_t *)data : 0);
        break;

    case POWER_HINT_LAUNCH:
        if (data && *(int32_t *)data)
            omap_power_boost(omap_device, LAUNCH_BOOST_MS);
        else
            omap_power_boost_end(omap_device);
        break;

    case POWER_HINT_VSYNC:
        break;

    default:
        break;
    }
}

#ifdef ANDROID_API_LP_MR1_OR_LATER
static void omap_set_feature(struct power_module *module,
                             feature_t feature, int state)
{
    struct omap_power_module *omap_device =
               (struct omap_power_module *) module;

    if (!omap_device->inited)
        return;

    switch (feature) {
    case POWER_FEATURE_DOUBLE_TAP_TO_WAKE:
#ifdef DOUBLE_TAP_TO_WAKE_PATH
        sysfs_write(DOUBLE_TAP_TO_WAKE_PATH, (state ? "1" : "0"));
#else
        /* Silly code to avoid issue of "state" being otherwised unused */
        if (state)
            return;
#endif
        break;
    default:
        break;
    }
}
#endif

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct omap_power_module HAL_MODULE_INFO_SYM = {
    .base = {
        .common = {
            .tag = HARDWARE_MODULE_TAG,
#ifdef ANDROID_API_LP_MR1_OR_LATER
            .module_api_version = POWER_MODULE_API_VERSION_0_3,
#else
            .module_api_version = POWER_MODULE_API_VERSION_0_2,
#endif
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = POWER_HARDWARE_MODULE_ID,
            .name = "OMAP Power HAL",
            .author = "The Android Open Source Project",
            .methods = &power_module_methods,
        },

       .init = omap_power_init,
       .setInteractive = omap_power_set_interactive,
       .powerHint = omap_power_hint,
#ifdef ANDROID_API_LP_MR1_OR_LATER
       .setFeature = omap_set_feature,
#endif
    },

    .lock = PTHREAD_MUTEX_INITIALIZER,
    .boostpulse_fd = -1,
    .boostpulse_warned = 0,
    .sgx_boost_fd = -1,
    .qos_fd = -1,
    .inited = 0,
    .screen_state = -1,
};
