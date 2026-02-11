/*
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT zmk_behavior_tetris

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/events/keycode_state_changed.h>

#include <dt-bindings/zmk/keys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ---- helpers: send key taps via behavior_queue ---- */

static const char *kp_dev = DEVICE_DT_NAME(DT_NODELABEL(key_press));

static inline struct zmk_behavior_binding kp(uint32_t keycode) {
    return (struct zmk_behavior_binding){
        .behavior_dev = kp_dev,
        .param1 = keycode,
        .param2 = 0,
    };
}

static void tap(struct zmk_behavior_binding_event *ev, uint32_t keycode) {
    struct zmk_behavior_binding b = kp(keycode);
    zmk_behavior_queue_add(ev, b, true, 0);
    zmk_behavior_queue_add(ev, b, false, 0);
}

/* Ctrl+A */
static void ctrl_a(struct zmk_behavior_binding_event *ev) {
    // 「修飾キー込みのエンコード」をCで作るのは流派が複数あるので、
    // まずは確実策：Aを押す前に LCTRL を押して、後で離す（低レベル方式）
    tap(ev, LCTRL); // ← これは “タップ” なのでダメに見えるけど、ZMKのkpは押下/解放として扱える
    // ↑ここが気になる場合は、下の「より確実なCtrl修飾方式」を使う（後述）
}

/* char -> keycode（shift不要文字だけ） */
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

static void type_string(struct zmk_behavior_binding_event *ev, const char *s) {
    for (const char *p = s; *p; p++) {
        uint32_t kc;
        if (char_to_keycode(*p, &kc)) {
            tap(ev, kc);
        }
    }
}

/* 盤面（まずは固定） */
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

static void clear_editor(struct zmk_behavior_binding_event *ev) {
    // VS Codeで安定：Ctrl+A → Backspace
    // ここは「Ctrl修飾の押しっぱなし」が必要なので、イベントを直接上げる方式に寄せます。
    raise_zmk_keycode_state_changed_from_encoded(LCTRL, true, ev->timestamp);
    raise_zmk_keycode_state_changed_from_encoded(A, true, ev->timestamp);
    raise_zmk_keycode_state_changed_from_encoded(A, false, ev->timestamp);
    raise_zmk_keycode_state_changed_from_encoded(LCTRL, false, ev->timestamp);

    tap(ev, BACKSPACE);
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    uint32_t cmd = binding->param1; // 0=start, 1=clear, 2=redraw etc.

    LOG_DBG("tetris cmd=%d", cmd);

    switch (cmd) {
    case 0: // START = clear + draw
        clear_editor(&event);
        type_string(&event, frame0);
        return ZMK_BEHAVIOR_OPAQUE;

    case 1: // CLEAR
        clear_editor(&event);
        return ZMK_BEHAVIOR_OPAQUE;

    default:
        return ZMK_BEHAVIOR_TRANSPARENT;
    }
}

static const struct behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = NULL,
};

#define INST(n)                                                                 \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL,                          \
        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(INST)
