/*
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT zmk_behavior_tetris

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/events/keycode_state_changed.h>

#include <dt-bindings/zmk/keys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static inline void press(uint32_t keycode, uint32_t ts) {
    raise_zmk_keycode_state_changed_from_encoded(keycode, true, ts);
}
static inline void release(uint32_t keycode, uint32_t ts) {
    raise_zmk_keycode_state_changed_from_encoded(keycode, false, ts);
}
static inline void tap(uint32_t keycode, uint32_t ts) {
    press(keycode, ts);
    release(keycode, ts);
}

static inline void tap_slow(uint32_t keycode) {
    uint32_t ts = (uint32_t)k_uptime_get();
    press(keycode, ts);
    k_msleep(1);
    release(keycode, (uint32_t)k_uptime_get());
}

static void tap_with_mod(uint32_t mod, uint32_t key, uint32_t ts) {
    press(mod, ts);
    tap(key, ts);
    release(mod, ts);
}

static bool char_to_keycode(char c, uint32_t *out) {
    switch (c) {
    case 'x': *out = X; return true;
    case '.': *out = DOT; return true;
    case ' ': *out = SPACE; return true;
    case '\n': *out = ENTER; return true;
    case '0': *out = N0; return true;
    case '1': *out = N1; return true;
    case '2': *out = N2; return true;
    case '3': *out = N3; return true;
    case '4': *out = N4; return true;
    case '5': *out = N5; return true;
    case '6': *out = N6; return true;
    case '7': *out = N7; return true;
    case '8': *out = N8; return true;
    case '9': *out = N9; return true;
    default: return false;
    }
}

/* VS Code: Ctrl+A → Backspace */
static void clear_editor(uint32_t ts) {
    tap_with_mod(LCTRL, A, ts);
    tap(BACKSPACE, ts);
}

/* 固定フレーム */
static const char *frame0 =
"tetris (zmk)\n"
"score 0000\n"
"\n"
".......... \n"
"...xx..... \n"
"...xx..... \n"
".......... \n"
".......... \n"
".......... \n"
".......... \n"
".......... \n"
".......... \n"
".......... \n";

/* ---- render work (send slowly) ---- */
struct tetris_render_state {
    bool inited;
    bool running;
    const char *text;
    size_t idx;
    uint32_t ts;
    struct k_work_delayable work;
};

static struct tetris_render_state rs;

static void render_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!rs.running || rs.text == NULL) return;

    char c = rs.text[rs.idx];
    if (c == '\0') { rs.running = false; return; }

    uint32_t kc;
    uint32_t delay_ms = 6; // 普通文字は遅め

    if (c == '\n') {
        kc = ENTER;
        delay_ms = 25; // ← ここが超効く
        tap_slow(kc);
        rs.idx++;
        k_work_reschedule(&rs.work, K_MSEC(delay_ms));
        return;
    }

    if (char_to_keycode(c, &kc)) {
        tap_slow(kc);
    }
    rs.idx++;

    k_work_reschedule(&rs.work, K_MSEC(delay_ms));
}

static void start_render(const char *text) {
    if (!rs.inited) {
        k_work_init_delayable(&rs.work, render_work_handler);
        rs.inited = true;
    }
    rs.text = text;
    rs.idx = 0;
    rs.running = true;
    k_work_reschedule(&rs.work, K_NO_WAIT);
}

static void stop_render(void) {
    rs.running = false;
    // 実際にはcancelしてもOK
    k_work_cancel_delayable(&rs.work);
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    uint32_t cmd = binding->param1;

    LOG_DBG("tetris cmd=%d", cmd);

    switch (cmd) {
    case 0: // START
        stop_render();
        clear_editor(event.timestamp);
        start_render(frame0);
        return ZMK_BEHAVIOR_OPAQUE;

    case 1: // CLEAR
        stop_render();
        clear_editor(event.timestamp);
        return ZMK_BEHAVIOR_OPAQUE;

    default:
        return ZMK_BEHAVIOR_TRANSPARENT;
    }
}

static const struct behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = NULL,
};

#define INST(n) \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, \
        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(INST)
