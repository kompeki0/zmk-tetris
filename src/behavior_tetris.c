/*
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT zmk_behavior_tetris

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/events/keycode_state_changed.h>

#include <dt-bindings/zmk/keys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ============ key helpers (event-based) ============ */
static inline void press(uint32_t keycode) {
    raise_zmk_keycode_state_changed_from_encoded(keycode, true, (uint32_t)k_uptime_get());
}
static inline void release(uint32_t keycode) {
    raise_zmk_keycode_state_changed_from_encoded(keycode, false, (uint32_t)k_uptime_get());
}
static inline void tap(uint32_t keycode) {
    press(keycode);
    k_msleep(1);         // ← 安定化（短い押下）
    release(keycode);
}
static inline void tap_with_mod(uint32_t mod, uint32_t key) {
    press(mod);
    k_msleep(1);
    tap(key);
    k_msleep(1);
    release(mod);
}

/* ============ text typing ============ */
/* “まずは崩れにくい文字だけ” */
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

/* VS Code 安定：Ctrl+A → Backspace */
static void clear_editor(void) {
    tap_with_mod(LCTRL, A);
    tap(BACKSPACE);
}

/* ============ frames (full draw) ============ */
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

/* ============ diff demo (line replace) ============ */
/*
 * frame0 の行番号（0-based）：
 * 0: tetris (zmk)
 * 1: score 0000
 * 2: (blank)
 * 3: ".......... "
 * 4: "...xx..... "   ← ここを動かすデモにする
 * 5: "...xx..... "
 */
#define TARGET_LINE_INDEX 4

static const char *line_variants[] = {
    "...xx..... ",  // 左寄り
    "....xx.... ",  // 1右
    ".....xx... ",  // 2右
    "....xx.... ",  // 戻す
};
#define LINE_VARIANTS_LEN (sizeof(line_variants) / sizeof(line_variants[0]))

/* ============ worker-driven renderer ============ */
enum render_mode {
    RENDER_NONE = 0,
    RENDER_TYPE_TEXT_FULL,
    RENDER_REPLACE_LINE_SCRIPT,
};

enum script_phase {
    SPH_CTRL_HOME = 0,
    SPH_DOWN_REPEAT,
    SPH_HOME,
    SPH_SHIFT_END_PRESS,
    SPH_END_TAP,
    SPH_SHIFT_END_RELEASE,
    SPH_BACKSPACE,
    SPH_TYPE_LINE,
    SPH_DONE
};

struct render_state {
    bool inited;
    bool running;

    enum render_mode mode;

    /* full text typing */
    const char *text;
    size_t idx;

    /* line replace script */
    enum script_phase phase;
    int down_remaining;
    const char *line_text;
    size_t line_idx;

    /* common */
    struct k_work_delayable work;
};

static struct render_state rs;

/* 遅延（崩れたらここを増やす） */
static uint32_t delay_for_char(char c) {
    if (c == '\n') return 25;   // ENTER は長め
    return 6;                   // 通常文字
}
static uint32_t delay_nav(void) {
    return 12;                  // カーソル移動系
}
static uint32_t delay_action(void) {
    return 18;                  // ちょい重い操作（選択削除等）
}

static void stop_render(void) {
    rs.running = false;
    k_work_cancel_delayable(&rs.work);
}

static void start_full_text(const char *text) {
    rs.mode = RENDER_TYPE_TEXT_FULL;
    rs.text = text;
    rs.idx = 0;
    rs.running = true;
    k_work_reschedule(&rs.work, K_NO_WAIT);
}

static void start_replace_line(int line_index_zero_based, const char *line) {
    rs.mode = RENDER_REPLACE_LINE_SCRIPT;
    rs.phase = SPH_CTRL_HOME;
    rs.down_remaining = line_index_zero_based;
    rs.line_text = line;
    rs.line_idx = 0;
    rs.running = true;
    k_work_reschedule(&rs.work, K_NO_WAIT);
}

static void render_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!rs.running) return;

    /* ---------- FULL TEXT MODE ---------- */
    if (rs.mode == RENDER_TYPE_TEXT_FULL) {
        char c = rs.text[rs.idx];
        if (c == '\0') {
            rs.running = false;
            return;
        }

        uint32_t kc;
        if (char_to_keycode(c, &kc)) {
            tap(kc);
        }
        rs.idx++;

        k_work_reschedule(&rs.work, K_MSEC(delay_for_char(c)));
        return;
    }

    /* ---------- REPLACE LINE SCRIPT MODE ---------- */
    if (rs.mode == RENDER_REPLACE_LINE_SCRIPT) {
        switch (rs.phase) {
        case SPH_CTRL_HOME:
            tap_with_mod(LCTRL, HOME);
            rs.phase = SPH_DOWN_REPEAT;
            k_work_reschedule(&rs.work, K_MSEC(delay_nav()));
            return;

        case SPH_DOWN_REPEAT:
            if (rs.down_remaining > 0) {
                tap(DOWN);
                rs.down_remaining--;
                k_work_reschedule(&rs.work, K_MSEC(delay_nav()));
                return;
            }
            rs.phase = SPH_HOME;
            k_work_reschedule(&rs.work, K_MSEC(delay_nav()));
            return;

        case SPH_HOME:
            tap(HOME);
            rs.phase = SPH_SHIFT_END_PRESS;
            k_work_reschedule(&rs.work, K_MSEC(delay_action()));
            return;

        case SPH_SHIFT_END_PRESS:
            press(LSHIFT);
            rs.phase = SPH_END_TAP;
            k_work_reschedule(&rs.work, K_MSEC(4));
            return;

        case SPH_END_TAP:
            tap(END);
            rs.phase = SPH_SHIFT_END_RELEASE;
            k_work_reschedule(&rs.work, K_MSEC(4));
            return;

        case SPH_SHIFT_END_RELEASE:
            release(LSHIFT);
            rs.phase = SPH_BACKSPACE;
            k_work_reschedule(&rs.work, K_MSEC(delay_action()));
            return;

        case SPH_BACKSPACE:
            tap(BACKSPACE);
            rs.phase = SPH_TYPE_LINE;
            rs.line_idx = 0;
            k_work_reschedule(&rs.work, K_MSEC(delay_action()));
            return;

        case SPH_TYPE_LINE: {
            char c = rs.line_text[rs.line_idx];
            if (c == '\0') {
                rs.phase = SPH_DONE;
                k_work_reschedule(&rs.work, K_MSEC(delay_action()));
                return;
            }
            uint32_t kc;
            if (char_to_keycode(c, &kc)) {
                tap(kc);
            }
            rs.line_idx++;
            k_work_reschedule(&rs.work, K_MSEC(delay_for_char(c)));
            return;
        }

        case SPH_DONE:
        default:
            rs.running = false;
            return;
        }
    }

    rs.running = false;
}

/* ============ behavior entry ============ */
static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    uint32_t cmd = binding->param1;

    if (!rs.inited) {
        k_work_init_delayable(&rs.work, render_work_handler);
        rs.inited = true;
    }

    LOG_DBG("tetris cmd=%d", cmd);

    switch (cmd) {
    case 0: /* FULL DRAW */
        stop_render();
        clear_editor();
        start_full_text(frame0);
        return ZMK_BEHAVIOR_OPAQUE;

    case 1: /* CLEAR */
        stop_render();
        clear_editor();
        return ZMK_BEHAVIOR_OPAQUE;

    case 2: /* DIFF: variant 0 */
        stop_render();
        start_replace_line(TARGET_LINE_INDEX, line_variants[0]);
        return ZMK_BEHAVIOR_OPAQUE;

    case 3: /* DIFF: variant 1 */
        stop_render();
        start_replace_line(TARGET_LINE_INDEX, line_variants[1]);
        return ZMK_BEHAVIOR_OPAQUE;

    case 4: /* DIFF: variant 2 */
        stop_render();
        start_replace_line(TARGET_LINE_INDEX, line_variants[2]);
        return ZMK_BEHAVIOR_OPAQUE;

    case 5: /* DIFF: variant 3 */
        stop_render();
        start_replace_line(TARGET_LINE_INDEX, line_variants[3]);
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
