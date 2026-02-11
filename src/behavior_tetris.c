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

/* ==============================
 * Tunables
 * ============================== */
#define BOARD_W 10
#define BOARD_H 10

/* frame0 text layout:
 * 0: tetris (zmk)
 * 1: score 0000
 * 2: blank
 * 3: board row0
 * ...
 */
#define BOARD_TOP_LINE_INDEX 3

/* Max lines updated per “one logical update” */
#define MAX_UPDATE_LINES 5

/* gravity behavior */
static uint16_t idle_before_fall_ms = 450;   // ユーザ操作後、落下を待つ時間
static uint16_t fall_interval_ms    = 550;   // 連続落下の間隔（idle後に動き出したらこの周期）

/* delays for typing/navigation (stability) */
static uint32_t delay_for_char(char c) {
    if (c == '\n') return 25;
    return 6;
}
static uint32_t delay_nav(void) { return 12; }
static uint32_t delay_action(void) { return 18; }

/* ==============================
 * Key helpers (event-based)
 * ============================== */
static inline void press(uint32_t keycode) {
    raise_zmk_keycode_state_changed_from_encoded(keycode, true, (uint32_t)k_uptime_get());
}
static inline void release(uint32_t keycode) {
    raise_zmk_keycode_state_changed_from_encoded(keycode, false, (uint32_t)k_uptime_get());
}
static inline void tap(uint32_t keycode) {
    press(keycode);
    k_msleep(1);
    release(keycode);
}
static inline void tap_with_mod(uint32_t mod, uint32_t key) {
    press(mod);
    k_msleep(1);
    tap(key);
    k_msleep(1);
    release(mod);
}

/* ==============================
 * Text typing (safe subset)
 * ============================== */
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
    case '(': *out = LPAR; return true;
    case ')': *out = RPAR; return true;
    default: return false;
    }
}

static void clear_editor(void) {
    tap_with_mod(LCTRL, A);
    tap(BACKSPACE);
}

/* ==============================
 * Game state (O piece only for now)
 * ============================== */
struct game_state {
    int x; // 0..BOARD_W-2
    int y; // 0..BOARD_H-2
    bool running; // reserved
};

static struct game_state gs;

/* input aggregation (queue) */
static int pending_dx;
static uint32_t last_input_ms;

/* ==============================
 * Renderer: batch update lines (max 5)
 * ============================== */
enum render_mode {
    RENDER_NONE = 0,
    RENDER_TYPE_TEXT_FULL,
    RENDER_REPLACE_LINE_SCRIPT,
    RENDER_BATCH_LINES,
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

struct update_line {
    int line_index;                 // absolute line in editor (0-based)
    char text[BOARD_W + 2];         // e.g. ".......... " (10+space) + '\0'
};

struct render_state {
    bool inited;
    bool running;

    enum render_mode mode;

    /* full text typing */
    const char *text;
    size_t idx;

    /* replace line script */
    enum script_phase phase;
    int down_remaining;
    const char *line_text;
    size_t line_idx;

    /* batch lines */
    struct update_line batch[MAX_UPDATE_LINES];
    uint8_t batch_len;
    uint8_t batch_pos;

    /* work */
    struct k_work_delayable work;
};

static struct render_state rs;

/* gravity */
static struct k_work_delayable gravity_work;

/* forward decl */
static void schedule_gravity_idle(void);
static void schedule_gravity_interval(void);
static void request_render_from_state(bool include_clear);

/* ----------------
 * low-level: start full text
 * ---------------- */
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

/* ----------------
 * low-level: line replace script (1 line)
 * ---------------- */
static void start_replace_line(int line_index_zero_based, const char *line) {
    rs.mode = RENDER_REPLACE_LINE_SCRIPT;
    rs.phase = SPH_CTRL_HOME;
    rs.down_remaining = line_index_zero_based;
    rs.line_text = line;
    rs.line_idx = 0;
    rs.running = true;
    k_work_reschedule(&rs.work, K_NO_WAIT);
}

/* ----------------
 * batch update API
 * ---------------- */
static void start_batch_lines(struct update_line *lines, uint8_t len) {
    if (len == 0) return;

    if (len > MAX_UPDATE_LINES) len = MAX_UPDATE_LINES;

    for (uint8_t i = 0; i < len; i++) {
        rs.batch[i] = lines[i];
    }
    rs.batch_len = len;
    rs.batch_pos = 0;

    rs.mode = RENDER_BATCH_LINES;
    rs.running = true;

    // kick first line
    start_replace_line(rs.batch[0].line_index, rs.batch[0].text);
}

/* ==============================
 * Board line builder (O piece)
 * ============================== */
static void build_board_line(int row, char out[BOARD_W + 2]) {
    // default empty
    for (int i = 0; i < BOARD_W; i++) out[i] = '.';
    out[BOARD_W] = ' ';
    out[BOARD_W + 1] = '\0';

    // O piece occupies (x,y),(x+1,y),(x,y+1),(x+1,y+1)
    int px = gs.x;
    int py = gs.y;

    if (row == py || row == py + 1) {
        int c0 = px;
        int c1 = px + 1;
        if (c0 >= 0 && c0 < BOARD_W) out[c0] = 'x';
        if (c1 >= 0 && c1 < BOARD_W) out[c1] = 'x';
    }
}

/* ==============================
 * Compute update list (max 5 lines)
 *
 * - Horizontal move: affects exactly 2 rows (y, y+1)
 * - Vertical move (1 step): affects up to 3 rows: y-1, y, y+1
 *   (this matches your “O落下は3行更新”)
 * ============================== */
static uint8_t make_update_lines_for_move(int old_x, int old_y, int new_x, int new_y,
                                         struct update_line out[MAX_UPDATE_LINES]) {
    // rows we might need to refresh
    bool rows[BOARD_H] = {0};

    // old piece rows
    if (old_y >= 0 && old_y < BOARD_H) rows[old_y] = true;
    if (old_y + 1 >= 0 && old_y + 1 < BOARD_H) rows[old_y + 1] = true;

    // new piece rows
    if (new_y >= 0 && new_y < BOARD_H) rows[new_y] = true;
    if (new_y + 1 >= 0 && new_y + 1 < BOARD_H) rows[new_y + 1] = true;

    // For 1-step fall of O, union becomes at most 3 rows (old_y, old_y+1, new_y+1)
    // which is what we want.

    uint8_t n = 0;
    for (int r = 0; r < BOARD_H && n < MAX_UPDATE_LINES; r++) {
        if (!rows[r]) continue;
        out[n].line_index = BOARD_TOP_LINE_INDEX + r;
        build_board_line(r, out[n].text);
        n++;
    }

    return n;
}

/* ==============================
 * Apply queued input
 * ============================== */
static void clamp_piece(void) {
    if (gs.x < 0) gs.x = 0;
    if (gs.x > BOARD_W - 2) gs.x = BOARD_W - 2;
    if (gs.y < 0) gs.y = 0;
    if (gs.y > BOARD_H - 2) gs.y = BOARD_H - 2;
}

static void apply_pending_inputs(void) {
    if (pending_dx == 0) return;

    int old_x = gs.x;
    int old_y = gs.y;

    gs.x += pending_dx;
    pending_dx = 0;
    clamp_piece();

    // Create update list and render
    struct update_line lines[MAX_UPDATE_LINES];
    uint8_t len = make_update_lines_for_move(old_x, old_y, gs.x, gs.y, lines);

    if (!rs.running) {
        start_batch_lines(lines, len);
    } else {
        // Shouldn't happen because we only call this when render done,
        // but keep it safe: re-accumulate would be better than nested render.
        pending_dx += (gs.x - old_x);
        gs.x = old_x;
    }
}

/* ==============================
 * Gravity
 * ============================== */
static bool can_fall_one(void) {
    return (gs.y < BOARD_H - 2);
}

static void do_fall_one_step(void) {
    int old_x = gs.x;
    int old_y = gs.y;

    if (!can_fall_one()) return;

    gs.y += 1;
    clamp_piece();

    struct update_line lines[MAX_UPDATE_LINES];
    uint8_t len = make_update_lines_for_move(old_x, old_y, gs.x, gs.y, lines);

    if (!rs.running) {
        start_batch_lines(lines, len);
    } else {
        // If rendering, we defer fall by rescheduling soon.
        // Input will be queued; fall will retry after render finishes.
    }
}

static void gravity_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint32_t now = (uint32_t)k_uptime_get();
    uint32_t since_input = now - last_input_ms;

    // Enforce idle delay after last user action
    if (since_input < idle_before_fall_ms) {
        uint32_t remain = idle_before_fall_ms - since_input;
        if (remain < 20) remain = 20;
        k_work_reschedule(&gravity_work, K_MSEC(remain));
        return;
    }

    // If rendering, try a bit later (this also matches “fall animation: queue inputs”)
    if (rs.running) {
        k_work_reschedule(&gravity_work, K_MSEC(30));
        return;
    }

    // Apply any queued user moves first (so “after fall render, apply queued ops then draw” also works naturally)
    // But spec says: during fall animation, inputs queue and apply after fall draw.
    // Here, we are about to perform a fall; so DON'T apply pending inputs now.
    // We'll fall first, render, then after render finishes we'll apply queued moves.
    // -> so we do fall immediately.
    if (can_fall_one()) {
        do_fall_one_step();
        schedule_gravity_interval();
        return;
    }

    // bottom reached: stop gravity for now
    // (future: lock piece, spawn next)
}

static void schedule_gravity_idle(void) {
    k_work_reschedule(&gravity_work, K_MSEC(idle_before_fall_ms));
}

static void schedule_gravity_interval(void) {
    k_work_reschedule(&gravity_work, K_MSEC(fall_interval_ms));
}

/* ==============================
 * Render worker
 * - also handles “after render finished, apply queued inputs then redraw once”
 * ============================== */
static void render_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!rs.running) return;

    /* ---------- FULL TEXT MODE ---------- */
    if (rs.mode == RENDER_TYPE_TEXT_FULL) {
        char c = rs.text[rs.idx];
        if (c == '\0') {
            rs.running = false;

            // After a full draw, if inputs queued, apply once
            if (pending_dx != 0) {
                apply_pending_inputs();
            }
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

            // If we are in batch mode, proceed next line
            if (rs.mode == RENDER_BATCH_LINES) {
                // (shouldn't hit here; batch uses replace_line + hooks below)
            }

            // After render finishes:
            // 1) If this was a batch runner, it will schedule next line via batch logic (below)
            // 2) Otherwise, apply pending inputs once and redraw (instant update after rendering)
            if (pending_dx != 0) {
                apply_pending_inputs();
            }
            return;
        }
    }

    /* ---------- BATCH MODE ----------
     * We implement batch by chaining replace_line scripts:
     * - when a replace_line finishes (SPH_DONE), we advance batch_pos and start next replace_line.
     * So we need to intercept “done” while in batch mode.
     */
    if (rs.mode == RENDER_BATCH_LINES) {
        // NOTE: In this design, batch mode reuses REPLACE_LINE_SCRIPT internally.
        // We keep rs.mode as BATCH_LINES, but phases are driven by the same switch above.
        // To do that, we simply never enter here.
    }

    rs.running = false;
}

/* Hook: after a line replace completes, if in batch mode, start next line */
static void maybe_continue_batch_after_line_done(void) {
    if (!rs.running && rs.mode == RENDER_BATCH_LINES) {
        // not used in this version (kept for future)
    }
}

/* ==============================
 * Frame0 Full draw text generator (optional)
 * Keep a static constant for now.
 * ============================== */
static const char *full_draw_text = NULL;

/* Build a full frame from current state (simple: header + all board lines) */
static void build_full_frame(char *buf, size_t buflen) {
    // very simple, no snprintf to avoid heavy deps; write manually
    // Format matches frame0 style.
    // NOTE: keep it small & safe; if overflow, just truncate.
    size_t p = 0;
    const char *h1 = "tetris (zmk)\n";
    const char *h2 = "score 0000\n\n";
    for (const char *s = h1; *s && p + 1 < buflen; s++) buf[p++] = *s;
    for (const char *s = h2; *s && p + 1 < buflen; s++) buf[p++] = *s;

    char line[BOARD_W + 2];
    for (int r = 0; r < BOARD_H; r++) {
        build_board_line(r, line);
        for (int i = 0; line[i] && p + 1 < buflen; i++) buf[p++] = line[i];
        if (p + 1 < buflen) buf[p++] = '\n';
    }
    buf[p] = '\0';
}

/* ==============================
 * Render request helpers
 * ============================== */
static void request_full_draw(void) {
    static char frame_buf[512];
    build_full_frame(frame_buf, sizeof(frame_buf));
    full_draw_text = frame_buf;

    stop_render();
    clear_editor();
    start_full_text(full_draw_text);
}

static void request_update_from_move(int old_x, int old_y, int new_x, int new_y) {
    struct update_line lines[MAX_UPDATE_LINES];
    uint8_t len = make_update_lines_for_move(old_x, old_y, new_x, new_y, lines);

    stop_render();
    // batch: implement by sequentially running replace_line for each line
    // Simple approach: start first line, then after completion apply next.
    // For now we do it via a tiny “manual batch runner”:
    // We'll just update lines one by one by calling start_replace_line and
    // storing remaining in rs.batch; then render_work will naturally run.
    rs.mode = RENDER_BATCH_LINES;
    rs.batch_len = len;
    rs.batch_pos = 0;
    for (uint8_t i = 0; i < len; i++) rs.batch[i] = lines[i];
    rs.running = true;

    // Start first line
    start_replace_line(rs.batch[0].line_index, rs.batch[0].text);
}

/* ==============================
 * Input API (called from behavior)
 * ============================== */
static void on_user_input_dx(int dx) {
    uint32_t now = (uint32_t)k_uptime_get();
    last_input_ms = now;

    // Always postpone gravity until idle
    schedule_gravity_idle();

    if (rs.running) {
        pending_dx += dx;
        return;
    }

    int old_x = gs.x, old_y = gs.y;
    gs.x += dx;
    clamp_piece();

    // immediate render
    request_update_from_move(old_x, old_y, gs.x, gs.y);
}

/* ==============================
 * Batch chaining: continue updating next line
 *
 * We need to detect when replace_line finishes. Easiest:
 * - In render_work_handler, when SPH_DONE occurs for REPLACE_LINE_SCRIPT,
 *   if rs.mode == RENDER_BATCH_LINES, start next line and keep running.
 *
 * To do that cleanly, we slightly tweak the SPH_DONE branch by checking mode.
 * We'll implement that by redefining a small helper:
 * ============================== */

/* Re-implement a safer “done handling” for replace_line */
static void on_replace_line_done(void) {
    // if batching, go next line
    if (rs.mode == RENDER_BATCH_LINES && rs.batch_pos + 1 < rs.batch_len) {
        rs.batch_pos++;
        // kick next line
        start_replace_line(rs.batch[rs.batch_pos].line_index, rs.batch[rs.batch_pos].text);
        return;
    }

    // batch finished
    rs.running = false;

    // After any render completes, apply queued inputs once and redraw once
    if (pending_dx != 0) {
        apply_pending_inputs();
        return;
    }
}

/* ==============================
 * We need to patch render_work_handler’s SPH_DONE path to call on_replace_line_done().
 * The simplest: replace the SPH_DONE handling above with this function call.
 *
 * For clarity and to keep “全文コピペ” workable:
 * We'll wrap render_work_handler2 that has the correct SPH_DONE behavior
 * and use it instead.
 * ============================== */

static void render_work_handler2(struct k_work *work) {
    ARG_UNUSED(work);

    if (!rs.running) return;

    /* FULL TEXT */
    if (rs.mode == RENDER_TYPE_TEXT_FULL) {
        char c = rs.text[rs.idx];
        if (c == '\0') {
            rs.running = false;
            if (pending_dx != 0) {
                apply_pending_inputs();
            }
            return;
        }
        uint32_t kc;
        if (char_to_keycode(c, &kc)) tap(kc);
        rs.idx++;
        k_work_reschedule(&rs.work, K_MSEC(delay_for_char(c)));
        return;
    }

    /* LINE SCRIPT (also used by BATCH) */
    // (rs.mode can be REPLACE_LINE_SCRIPT or BATCH_LINES; phases are shared)
    if (rs.mode == RENDER_REPLACE_LINE_SCRIPT || rs.mode == RENDER_BATCH_LINES) {
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
            if (char_to_keycode(c, &kc)) tap(kc);
            rs.line_idx++;
            k_work_reschedule(&rs.work, K_MSEC(delay_for_char(c)));
            return;
        }

        case SPH_DONE:
        default:
            // IMPORTANT: handle batch chaining here
            on_replace_line_done();
            return;
        }
    }

    rs.running = false;
}

/* ==============================
 * Behavior entry
 *
 * Commands:
 * 0: FULL DRAW (reset)
 * 1: CLEAR
 * 10: LEFT
 * 11: RIGHT
 * (future: 12 soft drop, 13 rotate, etc.)
 * ============================== */
static void reset_game_state(void) {
    gs.x = 3;
    gs.y = 1;
    gs.running = true;
    pending_dx = 0;
    last_input_ms = (uint32_t)k_uptime_get();
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    uint32_t cmd = binding->param1;

    if (!rs.inited) {
        k_work_init_delayable(&rs.work, render_work_handler2);
        k_work_init_delayable(&gravity_work, gravity_work_handler);
        rs.inited = true;
    }

    LOG_DBG("tetris cmd=%d", cmd);

    switch (cmd) {
    case 0: /* FULL DRAW + RESET */
        stop_render();
        k_work_cancel_delayable(&gravity_work);
        reset_game_state();
        request_full_draw();
        schedule_gravity_idle();
        return ZMK_BEHAVIOR_OPAQUE;

    case 1: /* CLEAR */
        stop_render();
        k_work_cancel_delayable(&gravity_work);
        clear_editor();
        return ZMK_BEHAVIOR_OPAQUE;

    case 10: /* LEFT */
        on_user_input_dx(-1);
        return ZMK_BEHAVIOR_OPAQUE;

    case 11: /* RIGHT */
        on_user_input_dx(+1);
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
