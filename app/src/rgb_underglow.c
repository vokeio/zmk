/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <device.h>
#include <init.h>
#include <kernel.h>
#include <settings/settings.h>

#include <math.h>
#include <stdlib.h>

#include <logging/log.h>

#include <drivers/led_strip.h>
#include <drivers/ext_power.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define STRIP_LABEL DT_LABEL(DT_CHOSEN(zmk_underglow))
#define STRIP_NUM_PIXELS DT_PROP(DT_CHOSEN(zmk_underglow), chain_length)

enum rgb_underglow_effect {
    UNDERGLOW_EFFECT_SOLID,
    UNDERGLOW_EFFECT_BREATHE,
    UNDERGLOW_EFFECT_SPECTRUM,
    UNDERGLOW_EFFECT_SWIRL,
    UNDERGLOW_EFFECT_NUMBER // Used to track number of underglow effects
};

struct led_hsb {
    uint16_t h;
    uint8_t s;
    uint8_t b;
};

struct rgb_underglow_state {
    uint16_t hue;
    uint8_t saturation;
    uint8_t brightness;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
};

static const struct device *led_strip;

static struct led_rgb pixels[STRIP_NUM_PIXELS];

static struct rgb_underglow_state state;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
static const struct device *ext_power;
#endif

static struct led_rgb hsb_to_rgb(struct led_hsb hsb) {
    double r, g, b;

    uint8_t i = hsb.h / 60;
    double v = hsb.b / 100.0;
    double s = hsb.s / 100.0;
    double f = hsb.h / 360.0 * 6 - i;
    double p = v * (1 - s);
    double q = v * (1 - f * s);
    double t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    }

    struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};

    return rgb;
}

static void zmk_rgb_underglow_off() {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}

static void zmk_rgb_underglow_effect_solid() {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int hue = state.hue;
        int sat = state.saturation;
        int brt = state.brightness;

        struct led_hsb hsb = {hue, sat, brt};

        pixels[i] = hsb_to_rgb(hsb);
    }
}

static void zmk_rgb_underglow_effect_breathe() {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int hue = state.hue;
        int sat = state.saturation;
        int brt = abs(state.animation_step - 1200) / 12;

        struct led_hsb hsb = {hue, sat, brt};

        pixels[i] = hsb_to_rgb(hsb);
    }

    state.animation_step += state.animation_speed * 10;

    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void zmk_rgb_underglow_effect_spectrum() {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int hue = state.animation_step;
        int sat = state.saturation;
        int brt = state.brightness;

        struct led_hsb hsb = {hue, sat, brt};

        pixels[i] = hsb_to_rgb(hsb);
    }

    state.animation_step += state.animation_speed;
    state.animation_step = state.animation_step % 360;
}

static void zmk_rgb_underglow_effect_swirl() {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int hue = (360 / STRIP_NUM_PIXELS * i + state.animation_step) % 360;
        int sat = state.saturation;
        int brt = state.brightness;

        struct led_hsb hsb = {hue, sat, brt};

        pixels[i] = hsb_to_rgb(hsb);
    }

    state.animation_step += state.animation_speed * 2;
    state.animation_step = state.animation_step % 360;
}

static void zmk_rgb_underglow_tick(struct k_work *work) {
    switch (state.current_effect) {
    case UNDERGLOW_EFFECT_SOLID:
        zmk_rgb_underglow_effect_solid();
        break;
    case UNDERGLOW_EFFECT_BREATHE:
        zmk_rgb_underglow_effect_breathe();
        break;
    case UNDERGLOW_EFFECT_SPECTRUM:
        zmk_rgb_underglow_effect_spectrum();
        break;
    case UNDERGLOW_EFFECT_SWIRL:
        zmk_rgb_underglow_effect_swirl();
        break;
    }

    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}

K_WORK_DEFINE(underglow_work, zmk_rgb_underglow_tick);

static void zmk_rgb_underglow_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        zmk_rgb_underglow_off();

        k_timer_stop(timer);

        return;
    }

    k_work_submit(&underglow_work);
}

K_TIMER_DEFINE(underglow_tick, zmk_rgb_underglow_tick_handler, NULL);

#if IS_ENABLED(CONFIG_SETTINGS)
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
            return 0;
        }

        return rc;
    }

    return -ENOENT;
}

struct settings_handler rgb_conf = {.name = "rgb/underglow", .h_set = rgb_settings_set};

static void zmk_rgb_underglow_save_state_work() {
    settings_save_one("rgb/underglow/state", &state, sizeof(state));
}

static struct k_delayed_work underglow_save_work;
#endif

static int zmk_rgb_underglow_init(const struct device *_arg) {
    led_strip = device_get_binding(STRIP_LABEL);
    if (led_strip) {
        LOG_INF("Found LED strip device %s", STRIP_LABEL);
    } else {
        LOG_ERR("LED strip device %s not found", STRIP_LABEL);
        return -EINVAL;
    }

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    ext_power = device_get_binding("EXT_POWER");
    if (ext_power == NULL) {
        LOG_ERR("Unable to retrieve ext_power device: EXT_POWER");
    }
#endif

    state = (struct rgb_underglow_state){
        hue : CONFIG_ZMK_RGB_UNDERGLOW_HUE_START,
        saturation : CONFIG_ZMK_RGB_UNDERGLOW_SAT_START,
        brightness : CONFIG_ZMK_RGB_UNDERGLOW_BRT_START,
        animation_speed : CONFIG_ZMK_RGB_UNDERGLOW_SPD_START,
        current_effect : CONFIG_ZMK_RGB_UNDERGLOW_EFF_START,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_ON_START)
    };

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_register(&rgb_conf);
    k_delayed_work_init(&underglow_save_work, zmk_rgb_underglow_save_state_work);

    settings_load_subtree("rgb/underglow");
#endif

    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));

    return 0;
}

int zmk_rgb_underglow_save_state() {
#if IS_ENABLED(CONFIG_SETTINGS)
    k_delayed_work_cancel(&underglow_save_work);
    return k_delayed_work_submit(&underglow_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#else
    return 0;
#endif
}

int zmk_rgb_underglow_cycle_effect(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.current_effect == 0 && direction < 0) {
        state.current_effect = UNDERGLOW_EFFECT_NUMBER - 1;
        return 0;
    }

    state.current_effect += direction;

    if (state.current_effect >= UNDERGLOW_EFFECT_NUMBER) {
        state.current_effect = 0;
    }

    state.animation_step = 0;

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_toggle() {
    if (!led_strip)
        return -ENODEV;

    state.on = !state.on;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc;

        if (state.on) {
            rc = ext_power_enable(ext_power);
        } else {
            rc = ext_power_disable(ext_power);
        }

        if (rc != 0) {
            LOG_ERR("Unable to toggle EXT_POWER: %d", rc);
        }
    }
#endif

    if (state.on) {
        state.animation_step = 0;
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
    } else {
        zmk_rgb_underglow_off();

        k_timer_stop(&underglow_tick);
    }

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.hue == 0 && direction < 0) {
        state.hue = 360 - CONFIG_ZMK_RGB_UNDERGLOW_HUE_STEP;
        return 0;
    }

    state.hue += direction * CONFIG_ZMK_RGB_UNDERGLOW_HUE_STEP;

    state.hue = state.hue % 360;

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.saturation == 0 && direction < 0) {
        return 0;
    }

    state.saturation += direction * CONFIG_ZMK_RGB_UNDERGLOW_SAT_STEP;

    if (state.saturation > 100) {
        state.saturation = 100;
    }

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.brightness == 0 && direction < 0) {
        return 0;
    }

    state.brightness += direction * CONFIG_ZMK_RGB_UNDERGLOW_BRT_STEP;

    if (state.brightness > 100) {
        state.brightness = 100;
    }

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.animation_speed == 1 && direction < 0) {
        return 0;
    }

    state.animation_speed += direction;

    if (state.animation_speed > 5) {
        state.animation_speed = 5;
    }

    return zmk_rgb_underglow_save_state();
}

SYS_INIT(zmk_rgb_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
