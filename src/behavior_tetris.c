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

/* Editor line layout (0-based):
 * 0: title
 * 1: score
 * 2: blank
 * 3..: board rows (H lines)
 */
#define BOARD_TOP_LINE_INDEX 3

/* Max lines updated per logical update (you requested 5) */
#define MAX_UPDATE_LINES 5

/* gravity:
 * - wait idle_before_fall_ms after last user input before falling
 * - once falling starts, fall every fall_interval_ms
 */
static uint16_t idle_before_fall_ms = 2000;
static uint16_t fall_interval_ms    = 700;

/* delays for editor ops (stability) */
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

/* ==============================
 * Game state
 * ============================== */
static uint8_t board_locked[BOARD_H][BOARD_W]; // 0/1 (future: colors)
static int piece_x; // O mino top-left
static int piece_y;

static int pending_dx;
static uint32_t last_input_ms;

/* clamp O mino to board (O is 2x2) */
static void clamp_piece(void) {
    if (piece_x < 0) piece_x = 0;
    if (piece_x > BOARD_W - 2) piece_x = BOARD_W - 2;
    if (piece_y < 0) piece_y = 0;
    if (piece_y > BOARD_H - 2) piece_y = BOARD_H - 2;
}

/* collision check for O at (x,y) against locked + bounds */
static bool can_place_o(int x, int y) {
    if (x < 0 || x + 1 >= BOARD_W) return false;
    if (y < 0 || y + 1 >= BOARD_H) return false;

    if (board_locked[y][x]) return false;
    if (board_locked[y][x + 1]) return false;
    if (board_locked[y + 1][x]) return false;
    if (board_locked[y + 1][x + 1]) return false;

    return true;
}

/* ==============================
 * Renderer: keep previous/next board lines
 * ============================== */
struct update_line {
    int line_index;                     // absolute editor line index
    char text[BOARD_W + 2];             // ".......... " + '\0'
};

static char render_prev[BOARD_H][BOARD_W + 2];
static char render_next[BOARD_H][BOARD_W + 2];

/* build one row string from locked board + current O overlay */
static void build_row_string(int row, char out[BOARD_W + 2]) {
    for (int c = 0; c < BOARD_W; c++) {
        out[c] = board_locked[row][c] ? 'x' : '.';
    }

    /* overlay current O (2x2) */
    if (row == piece_y || row == piece_y + 1) {
        int c0 = piece_x;
        int c1 = piece_x + 1;
        if (c0 >= 0 && c0 < BOARD_W) out[c0] = 'x';
        if (c1 >= 0 && c1 < BOARD_W) out[c1] = 'x';
    }

    out[BOARD_W] = ' ';
    out[BOARD_W + 1] = '\0';
}

static void rebuild_render_next(void) {
    for (int r = 0; r < BOARD_H; r++) {
        build_row_string(r, render_next[r]);
    }
}

static bool row_equals(const char *a, const char *b) {
    for (int i = 0; i < BOARD_W + 2; i++) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0') break;
    }
    return true;
}

/* make diff list from prev->next, up to 5 lines.
 * returns len, and also “commits” prev=row_next for rows included
 */
static uint8_t make_diff_lines(struct update_line out[MAX_UPDATE_LINES]) {
    uint8_t n = 0;
    for (int r = 0; r < BOARD_H; r++) {
        if (row_equals(render_prev[r], render_next[r])) continue;
        if (n >= MAX_UPDATE_LINES) break;

        out[n].line_index = BOARD_TOP_LINE_INDEX + r;

        for (int i = 0; i < BOARD_W + 2; i++) {
            out[n].text[i] = render_next[r][i];
            if (render_next[r][i] == '\0') break;
        }

        for (int i = 0; i < BOARD_W + 2; i++) {
            render_prev[r][i] = render_next[r][i];
            if (render_next[r][i] == '\0') break;
        }

        n++;
    }
    return n;
}

/* ==============================
 * Render engine
 * - MODE_CLEAR: Ctrl+A -> Backspace
 * - MODE_TYPE_FULL: type whole frame (header + board)
 * - MODE_REPLACE_LINE: replace one line, chained as batch (<=5)
 * ============================== */
enum render_mode {
    RENDER_IDLE = 0,
    RENDER_CLEAR_EDITOR,
    RENDER_TYPE_FULL,
    RENDER_REPLACE_LINE_SCRIPT,
};

enum clear_phase {
    CLP_CTRL_A = 0,
    CLP_BS,
    CLP_DONE,
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

enum request_type {
    REQ_NONE = 0,
    REQ_CLEAR_ONLY,
    REQ_RESET_AND_DRAW,
};

struct render_state {
    bool inited;
    bool running;

    enum request_type req;
    enum render_mode mode;

    /* clear editor */
    enum clear_phase clear_phase;

    /* full text typing */
    const char *text;
    size_t text_idx;

    /* replace-line script */
    enum script_phase phase;
    int down_remaining;
    const char *line_text;
    size_t line_idx;

    /* batch */
    struct update_line batch[MAX_UPDATE_LINES];
    uint8_t batch_len;
    uint8_t batch_pos;

    struct k_work_delayable work;
};

static struct render_state rs;

/* gravity */
static struct k_work_delayable gravity_work;

/* forward */
static void apply_pending_and_redraw_once(void);

/* stop current rendering */
static void stop_render(void) {
    rs.running = false;
    rs.req = REQ_NONE;
    rs.mode = RENDER_IDLE;
    rs.batch_len = 0;
    rs.batch_pos = 0;
    k_work_cancel_delayable(&rs.work);
}

/* start clear editor in worker */
static void start_clear_editor_async(enum request_type req_after) {
    rs.req = req_after;
    rs.mode = RENDER_CLEAR_EDITOR;
    rs.clear_phase = CLP_CTRL_A;
    rs.running = true;
    k_work_reschedule(&rs.work, K_NO_WAIT);
}

/* start full text typing in worker */
static void start_full_text_async(const char *text) {
    rs.mode = RENDER_TYPE_FULL;
    rs.text = text;
    rs.text_idx = 0;
    rs.running = true;
    k_work_reschedule(&rs.work, K_NO_WAIT);
}

/* start a single line replace script */
static void start_replace_line_script(int line_index_zero_based, const char *line) {
    rs.mode = RENDER_REPLACE_LINE_SCRIPT;
    rs.phase = SPH_CTRL_HOME;
    rs.down_remaining = line_index_zero_based;
    rs.line_text = line;
    rs.line_idx = 0;

    rs.running = true;
    k_work_reschedule(&rs.work, K_NO_WAIT);
}

/* start batch updates (len<=5) */
static void start_batch(struct update_line *lines, uint8_t len) {
    if (len == 0) return;
    if (len > MAX_UPDATE_LINES) len = MAX_UPDATE_LINES;

    for (uint8_t i = 0; i < len; i++) rs.batch[i] = lines[i];
    rs.batch_len = len;
    rs.batch_pos = 0;

    start_replace_line_script(rs.batch[0].line_index, rs.batch[0].text);
}

/* ==============================
 * Full frame buffer (typed in worker on reset)
 * ============================== */
static char full_frame_buf[256];

static void build_full_frame_text(void) {
    /* header */
    size_t w = 0;
    const char *hdr = "tetris (zmk)\nscore 0000\n\n";
    for (size_t i = 0; hdr[i] && w + 1 < sizeof(full_frame_buf); i++) {
        full_frame_buf[w++] = hdr[i];
    }

    /* board */
    rebuild_render_next();
    for (int r = 0; r < BOARD_H; r++) {
        for (int i = 0; render_next[r][i] && w + 1 < sizeof(full_frame_buf); i++) {
            full_frame_buf[w++] = render_next[r][i];
        }
        if (w + 1 < sizeof(full_frame_buf)) full_frame_buf[w++] = '\n';
    }

    full_frame_buf[w] = '\0';
}

/* ==============================
 * Rendering pipeline: state update -> diff -> start batch
 * ============================== */
static void request_diff_render(void) {
    rebuild_render_next();

    struct update_line lines[MAX_UPDATE_LINES];
    uint8_t len = make_diff_lines(lines);

    if (len == 0) return;

    /* only start if not already running */
    if (rs.running) return;

    start_batch(lines, len);
}

/* renderer worker */
static void render_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!rs.running) return;

    /* ---------- CLEAR EDITOR MODE ---------- */
    if (rs.mode == RENDER_CLEAR_EDITOR) {
        switch (rs.clear_phase) {
        case CLP_CTRL_A:
            tap_with_mod(LCTRL, A);
            rs.clear_phase = CLP_BS;
            k_work_reschedule(&rs.work, K_MSEC(delay_action()));
            return;

        case CLP_BS:
            tap(BACKSPACE);
            rs.clear_phase = CLP_DONE;
            k_work_reschedule(&rs.work, K_MSEC(delay_action()));
            return;

        case CLP_DONE:
        default: {
            enum request_type next = rs.req;
            rs.req = REQ_NONE;

            /* after clear, chain requested action */
            if (next == REQ_RESET_AND_DRAW) {
                start_full_text_async(full_frame_buf);
                return;
            }

            /* CLEAR only done */
            rs.running = false;
            rs.mode = RENDER_IDLE;
            rs.batch_len = 0;
            rs.batch_pos = 0;

            apply_pending_and_redraw_once();
            return;
        }
        }
    }

    /* ---------- FULL TEXT TYPE MODE ---------- */
    if (rs.mode == RENDER_TYPE_FULL) {
        char c = rs.text[rs.text_idx];
        if (c == '\0') {
            /* full draw finished: commit render_prev = render_next (current) */
            rebuild_render_next();
            for (int r = 0; r < BOARD_H; r++) {
                for (int i = 0; i < BOARD_W + 2; i++) {
                    render_prev[r][i] = render_next[r][i];
                    if (render_next[r][i] == '\0') break;
                }
            }

            rs.running = false;
            rs.mode = RENDER_IDLE;
            rs.batch_len = 0;
            rs.batch_pos = 0;

            apply_pending_and_redraw_once();
            return;
        }

        uint32_t kc;
        if (char_to_keycode(c, &kc)) {
            tap(kc);
        }
        rs.text_idx++;

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
            if (char_to_keycode(c, &kc)) tap(kc);
            rs.line_idx++;
            k_work_reschedule(&rs.work, K_MSEC(delay_for_char(c)));
            return;
        }

        case SPH_DONE:
        default:
            /* batch chaining */
            if (rs.batch_pos + 1 < rs.batch_len) {
                rs.batch_pos++;
                start_replace_line_script(rs.batch[rs.batch_pos].line_index,
                                          rs.batch[rs.batch_pos].text);
                return;
            }

            /* batch finished */
            rs.running = false;
            rs.mode = RENDER_IDLE;
            rs.batch_len = 0;
            rs.batch_pos = 0;

            apply_pending_and_redraw_once();
            return;
        }
    }

    /* fallback */
    rs.running = false;
    rs.mode = RENDER_IDLE;
    rs.batch_len = 0;
    rs.batch_pos = 0;
}

/* ==============================
 * Input handling & gravity scheduling
 * ============================== */
static void schedule_gravity_idle(void) {
    k_work_reschedule(&gravity_work, K_MSEC(idle_before_fall_ms));
}
static void schedule_gravity_interval(void) {
    k_work_reschedule(&gravity_work, K_MSEC(fall_interval_ms));
}

static void on_user_input_common(void) {
    last_input_ms = (uint32_t)k_uptime_get();
    schedule_gravity_idle(); // always postpone fall until idle
}

/* after render finishes, apply queued inputs once and redraw */
static void apply_pending_and_redraw_once(void) {
    if (pending_dx == 0) return;
    if (rs.running) return;

    int dx = pending_dx;
    pending_dx = 0;

    int new_x = piece_x + dx;

    /* apply with collision */
    if (can_place_o(new_x, piece_y)) {
        piece_x = new_x;
        clamp_piece();
    }

    request_diff_render();
}

static void on_user_dx(int dx) {
    on_user_input_common();

    if (rs.running) {
        pending_dx += dx;
        return;
    }

    int new_x = piece_x + dx;
    if (can_place_o(new_x, piece_y)) {
        piece_x = new_x;
        clamp_piece();
    }

    request_diff_render();
}

/* one-step fall (does not lock yet) */
static bool do_fall_one(void) {
    int new_y = piece_y + 1;
    if (!can_place_o(piece_x, new_y)) {
        return false;
    }
    piece_y = new_y;
    clamp_piece();
    return true;
}

static void gravity_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint32_t now = (uint32_t)k_uptime_get();
    uint32_t since_input = now - last_input_ms;

    /* enforce idle delay */
    if (since_input < idle_before_fall_ms) {
        uint32_t remain = idle_before_fall_ms - since_input;
        if (remain < 50) remain = 50;
        k_work_reschedule(&gravity_work, K_MSEC(remain));
        return;
    }

    /* if rendering, retry shortly */
    if (rs.running) {
        k_work_reschedule(&gravity_work, K_MSEC(30));
        return;
    }

    /* fall first, then render */
    if (do_fall_one()) {
        request_diff_render();
        schedule_gravity_interval();
        return;
    }

    /* reached bottom (future: lock & spawn) */
}

/* ==============================
 * Init/reset
 * ============================== */
static void reset_game(void) {
    for (int r = 0; r < BOARD_H; r++) {
        for (int c = 0; c < BOARD_W; c++) {
            board_locked[r][c] = 0;
        }
        for (int i = 0; i < BOARD_W + 2; i++) render_prev[r][i] = '\0';
    }

    piece_x = 3;
    piece_y = 0;
    clamp_piece();

    pending_dx = 0;
    last_input_ms = (uint32_t)k_uptime_get();

    rebuild_render_next();
    for (int r = 0; r < BOARD_H; r++) {
        for (int i = 0; i < BOARD_W + 2; i++) {
            render_prev[r][i] = '\0';
        }
    }
}

/* ==============================
 * Behavior entry
 *
 * Commands:
 * 0: reset + async clear + async draw + start gravity (idle)
 * 1: async clear editor
 * 10: left
 * 11: right
 * ============================== */
static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    uint32_t cmd = binding->param1;

    if (!rs.inited) {
        k_work_init_delayable(&rs.work, render_work_handler);
        k_work_init_delayable(&gravity_work, gravity_work_handler);
        rs.inited = true;
    }

    LOG_DBG("tetris cmd=%d", cmd);

    switch (cmd) {
    case 0: /* RESET + ASYNC DRAW */
        stop_render();
        k_work_cancel_delayable(&gravity_work);

        reset_game();
        build_full_frame_text();              /* build full_frame_buf from current state */
        start_clear_editor_async(REQ_RESET_AND_DRAW); /* clear -> full draw in worker */

        schedule_gravity_idle();
        return ZMK_BEHAVIOR_OPAQUE;

    case 1: /* CLEAR (ASYNC) */
        stop_render();
        k_work_cancel_delayable(&gravity_work);

        start_clear_editor_async(REQ_CLEAR_ONLY);
        return ZMK_BEHAVIOR_OPAQUE;

    case 10: /* LEFT */
        on_user_dx(-1);
        return ZMK_BEHAVIOR_OPAQUE;

    case 11: /* RIGHT */
        on_user_dx(+1);
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
