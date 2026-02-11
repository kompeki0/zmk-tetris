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

/* ---- low level key press/release via event ---- */
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

/* Ctrl+A, Ctrl+Home など “修飾付き” を確実に打つ */
static void tap_with_mod(uint32_t mod, uint32_t key, uint32_t ts) {
    press(mod, ts);
    tap(key, ts);
    release(mod, ts);
}

/* char -> keycode（まずはshift不要のみ） */
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

static void type_string(const char *s, uint32_t ts) {
    for (const char *p = s; *p; p++) {
        uint32_t kc;
        if (char_to_keycode(*p, &kc)) {
            tap(kc, ts);
        }
    }
}

/* VS Code: Ctrl+A → Backspace が安定 */
static void clear_editor(uint32_t ts) {
    tap_with_mod(LCTRL, A, ts);
    tap(BACKSPACE, ts);
}

/* 盤面（固定） */
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

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    uint32_t cmd = binding->param1; // 0=start, 1=clear

    LOG_DBG("tetris cmd=%d", cmd);

    switch (cmd) {
    case 0: // START
        clear_editor(event.timestamp);
        type_string(frame0, event.timestamp);
        return ZMK_BEHAVIOR_OPAQUE;

    case 1: // CLEAR
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
