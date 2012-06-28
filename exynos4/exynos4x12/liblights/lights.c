/*
 * Copyright (C) 2012 The Android Open Source Project
 *                    The CyanogenMod Project
 *                    Daniel Hillenbrand <codeworkx@cyanogenmod.com>
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


#define LOG_TAG "lights"
#define LOG_NDEBUG 0

#include <cutils/log.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <hardware/lights.h>

/******************************************************************************/

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

char const*const PANEL_FILE = "/sys/class/backlight/panel/brightness";
char const*const BUTTON_FILE = "/sys/class/sec/sec_touchkey/brightness";

char const*const LED_RED = "/sys/class/sec/led/led_r";
char const*const LED_GREEN = "/sys/class/sec/led/led_g";
char const*const LED_BLUE = "/sys/class/sec/led/led_b";
char const*const LED_BLINK = "/sys/class/sec/led/led_blink";
char const*const LED_BRIGHTNESS = "/sys/class/sec/led/led_br_lev";

#define MAX_WRITE_CMD 25

struct led_config {
    int red;
    int green;
    int blue;
    char blink[MAX_WRITE_CMD];
};

void init_g_lock(void)
{
    pthread_mutex_init(&g_lock, NULL);
}

static int write_int(char const *path, int value)
{
    int fd;
    static int already_warned;

    already_warned = 0;

    LOGV("write_int: path %s, value %d", path, value);
    fd = open(path, O_RDWR);

    if (fd >= 0) {
        char buffer[20];
        int bytes = sprintf(buffer, "%d\n", value);
        int amt = write(fd, buffer, bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            LOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

static int write_str(char const *path, const char* value)
{
    int fd;
    static int already_warned;

    already_warned = 0;

    LOGV("write_str: path %s, value %s", path, value);
    fd = open(path, O_RDWR);

    if (fd >= 0) {
        char buffer[MAX_WRITE_CMD];
        int bytes = sprintf(buffer, "%s\n", value);
        int amt = write(fd, buffer, bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            LOGE("write_str failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

static int rgb_to_brightness(struct light_state_t const *state)
{
    int color = state->color & 0x00ffffff;

    return ((77*((color>>16) & 0x00ff))
        + (150*((color>>8) & 0x00ff)) + (29*(color & 0x00ff))) >> 8;
}

static int set_light_backlight(struct light_device_t *dev,
            struct light_state_t const *state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);

    pthread_mutex_lock(&g_lock);
    err = write_int(PANEL_FILE, brightness);
    err = write_int(BUTTON_FILE, brightness > 0 ? 1 : 2);
    pthread_mutex_unlock(&g_lock);

    return err;
}

static int close_lights(struct light_device_t *dev)
{
    LOGV("close_light is called");
    if (dev)
        free(dev);

    return 0;
}

/* LEDs */
static int write_leds(struct led_config led)
{
    int err = 0;

    pthread_mutex_lock(&g_lock);
    err = write_int(LED_RED, led.red);
    err = write_int(LED_GREEN, led.green);
    err = write_int(LED_BLUE, led.blue);
    if (*(led.blink))
        err = write_str(LED_BLINK, led.blink);
    pthread_mutex_unlock(&g_lock);

    return err;
}

static unsigned int adjust_brightness(unsigned int colorRGB, unsigned int percentage)
{
    return (((((colorRGB >> 16) & 0xFF) * percentage / 100) & 0xFF) << 16) |
        (((((colorRGB >> 8) & 0xFF) * percentage / 100) & 0xFF) << 8) |
        ((((colorRGB) & 0xFF) * percentage / 100) & 0xFF);
}

static int set_light_leds(struct light_state_t const *state, int type)
{
    struct led_config led;
    unsigned int colorRGB;

    if (0 == type)
        colorRGB = adjust_brightness(state->color, 75);
    else
        colorRGB = state->color;

    switch (state->flashMode) {
    case LIGHT_FLASH_NONE:
            led.red = 0;
            led.green = 0;
            led.blue = 0;
            snprintf(led.blink, MAX_WRITE_CMD, "0x000000 0 0");
        break;
    case LIGHT_FLASH_TIMED:
    case LIGHT_FLASH_HARDWARE:
            led.red = (colorRGB >> 16) & 0xFF;
            led.green = (colorRGB >> 8) & 0xFF;
            led.blue = colorRGB & 0xFF;
            snprintf(led.blink, MAX_WRITE_CMD, "0x%x %d %d", colorRGB, state->flashOnMS, state->flashOffMS);
        break;
    default:
        return -EINVAL;
    }

    return write_leds(led);
}

static int set_light_leds_notifications(struct light_device_t *dev,
            struct light_state_t const *state)
{
    return set_light_leds(state, 0);
}

static int set_light_battery(struct light_device_t *dev,
            struct light_state_t const *state)
{
    int err = 0;
    struct led_config led;
    int brightness = rgb_to_brightness(state);
    unsigned int colorRGB;

    colorRGB = state->color;

    *led.blink = '\0';

    if (brightness == 0) {
        led.red = 0;
        led.green = 0;
        led.blue = 0;
    } else {
        // tone down the brightness of the battery life with /12 (255->21)
        led.red = ((colorRGB >> 16) & 0xFF) / 12;
        led.green = ((colorRGB >> 8) & 0xFF) / 12;
        led.blue = (colorRGB & 0xFF) / 12;
    }

    return write_leds(led);
}

static int set_light_leds_attention(struct light_device_t *dev,
            struct light_state_t const *state)
{
    return set_light_leds(state, 1);
}

static int open_lights(const struct hw_module_t *module, char const *name,
                        struct hw_device_t **device)
{
    int (*set_light)(struct light_device_t *dev,
        struct light_state_t const *state);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
        set_light = set_light_backlight;
    else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
        set_light = set_light_leds_notifications;
    else if (0 == strcmp(LIGHT_ID_ATTENTION, name))
        set_light = set_light_leds_attention;
    else if (0 == strcmp(LIGHT_ID_BATTERY, name))
        set_light = set_light_battery;
    else
        return -EINVAL;

    pthread_once(&g_init, init_g_lock);

    struct light_device_t *dev = malloc(sizeof(struct light_device_t));
    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = (int (*)(struct hw_device_t *))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t *)dev;

    return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open =  open_lights,
};

const struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "Exynos4x12 lights Module",
    .author = "The CyanogenMod Project",
    .methods = &lights_module_methods,
};
