/*
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT zmk_behavior_tetris

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

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

/* You said "10 is fine" */
#define MAX_UPDATE_LINES 10

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
 *  - ここは崩れにくい文字だけ推奨
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
    /* NOTE: 括弧は配列差で化けやすいので一旦封印
    case '(': *out = LPAR; return true;
    case ')': *out = RPAR; return true;
    */
    default: return false;
    }
}

/* ==============================
 * Game state (locked board + falling piece)
 * ============================== */
static uint8_t board_locked[BOARD_H][BOARD_W]; // 0/1

enum tetromino {
    TET_I = 0,
    TET_O = 1,
    TET_T = 2,
    TET_S = 3,
    TET_Z = 4,
    TET_J = 5,
    TET_L = 6,
    TET_COUNT
};

struct piece_state {
    int x;          // top-left of 4x4 mask on board
    int y;
    uint8_t type;   // enum tetromino
    uint8_t rot;    // 0..3
};

static struct piece_state falling;

/* queued inputs while rendering / falling animation */
static int pending_dx;
static int pending_rot_cw;
static int pending_soft_drop;
static uint32_t last_input_ms;

/* ==============================
 * 4x4 masks (bit 0 = (0,0), bit 15 = (3,3))
 * SRS-like orientations (CW)
 * ============================== */
static inline uint16_t bit_at(int r, int c) { return (uint16_t)(1u << (r * 4 + c)); }

/* Helper to read bit from mask */
static inline bool mask_has(uint16_t m, int r, int c) { return (m & bit_at(r, c)) != 0; }

/*
 * Masks are defined within 4x4.
 * This set is "SRS-ish" and works with the kick tables below.
 */
static const uint16_t SHAPE[TET_COUNT][4] = {
    /* I */
    {
        /* rot 0 */
        bit_at(1,0) | bit_at(1,1) | bit_at(1,2) | bit_at(1,3),
        /* rot 1 */
        bit_at(0,2) | bit_at(1,2) | bit_at(2,2) | bit_at(3,2),
        /* rot 2 */
        bit_at(2,0) | bit_at(2,1) | bit_at(2,2) | bit_at(2,3),
        /* rot 3 */
        bit_at(0,1) | bit_at(1,1) | bit_at(2,1) | bit_at(3,1),
    },
    /* O */
    {
        bit_at(1,1) | bit_at(1,2) | bit_at(2,1) | bit_at(2,2),
        bit_at(1,1) | bit_at(1,2) | bit_at(2,1) | bit_at(2,2),
        bit_at(1,1) | bit_at(1,2) | bit_at(2,1) | bit_at(2,2),
        bit_at(1,1) | bit_at(1,2) | bit_at(2,1) | bit_at(2,2),
    },
    /* T */
    {
        bit_at(1,1) | bit_at(1,2) | bit_at(1,3) | bit_at(2,2),
        bit_at(0,2) | bit_at(1,2) | bit_at(2,2) | bit_at(1,3),
        bit_at(1,1) | bit_at(1,2) | bit_at(1,3) | bit_at(0,2),
        bit_at(0,2) | bit_at(1,2) | bit_at(2,2) | bit_at(1,1),
    },
    /* S */
    {
        bit_at(1,2) | bit_at(1,3) | bit_at(2,1) | bit_at(2,2),
        bit_at(0,2) | bit_at(1,2) | bit_at(1,3) | bit_at(2,3),
        bit_at(1,2) | bit_at(1,3) | bit_at(2,1) | bit_at(2,2),
        bit_at(0,2) | bit_at(1,2) | bit_at(1,3) | bit_at(2,3),
    },
    /* Z */
    {
        bit_at(1,1) | bit_at(1,2) | bit_at(2,2) | bit_at(2,3),
        bit_at(0,3) | bit_at(1,2) | bit_at(1,3) | bit_at(2,2),
        bit_at(1,1) | bit_at(1,2) | bit_at(2,2) | bit_at(2,3),
        bit_at(0,3) | bit_at(1,2) | bit_at(1,3) | bit_at(2,2),
    },
    /* J */
    {
        bit_at(1,1) | bit_at(1,2) | bit_at(1,3) | bit_at(2,1),
        bit_at(0,2) | bit_at(0,3) | bit_at(1,2) | bit_at(2,2),
        bit_at(0,3) | bit_at(1,1) | bit_at(1,2) | bit_at(1,3),
        bit_at(0,2) | bit_at(1,2) | bit_at(2,2) | bit_at(2,1),
    },
    /* L */
    {
        bit_at(1,1) | bit_at(1,2) | bit_at(1,3) | bit_at(2,3),
        bit_at(0,2) | bit_at(1,2) | bit_at(2,2) | bit_at(0,3),
        bit_at(0,1) | bit_at(1,1) | bit_at(1,2) | bit_at(1,3),
        bit_at(2,2) | bit_at(0,1) | bit_at(1,1) | bit_at(2,1),
    },
};

/* ==============================
 * Collision / placement
 * ============================== */
static bool can_place(uint8_t type, uint8_t rot, int x, int y) {
    uint16_t m = SHAPE[type][rot & 3];

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!mask_has(m, r, c)) continue;

            int br = y + r;
            int bc = x + c;
            if (bc < 0 || bc >= BOARD_W || br < 0 || br >= BOARD_H) return false;
            if (board_locked[br][bc]) return false;
        }
    }
    return true;
}

/* ==============================
 * Wall kick (SRS-like)
 *  - We implement CW kicks only for now (enough for demo)
 *  - For O: no kick needed (it’s symmetric)
 * ============================== */

/* JLSTZ & T (same kick table) : CW from rot -> rot+1 */
static const int8_t KICK_JLSTZ_CW[4][5][2] = {
    /* 0->1 */ {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}},
    /* 1->2 */ {{0,0},{ 1,0},{ 1,-1},{0,2},{ 1,2}},
    /* 2->3 */ {{0,0},{ 1,0},{ 1,1},{0,-2},{ 1,-2}},
    /* 3->0 */ {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}},
};

/* I piece CW kicks */
static const int8_t KICK_I_CW[4][5][2] = {
    /* 0->1 */ {{0,0},{-2,0},{ 1,0},{-2,-1},{ 1,2}},
    /* 1->2 */ {{0,0},{-1,0},{ 2,0},{-1,2},{ 2,-1}},
    /* 2->3 */ {{0,0},{ 2,0},{-1,0},{ 2,1},{-1,-2}},
    /* 3->0 */ {{0,0},{ 1,0},{-2,0},{ 1,-2},{-2,1}},
};

static bool try_rotate_cw_with_kick(void) {
    uint8_t type = falling.type;
    if (type == TET_O) {
        /* O is symmetric: just rotate index and accept if placeable */
        uint8_t nr = (falling.rot + 1) & 3;
        if (can_place(type, nr, falling.x, falling.y)) {
            falling.rot = nr;
            return true;
        }
        return false;
    }

    uint8_t r0 = falling.rot & 3;
    uint8_t r1 = (r0 + 1) & 3;

    const int8_t (*kicks)[2] = NULL;
    if (type == TET_I) kicks = KICK_I_CW[r0];
    else kicks = KICK_JLSTZ_CW[r0];

    for (int i = 0; i < 5; i++) {
        int nx = falling.x + kicks[i][0];
        int ny = falling.y + kicks[i][1];
        if (can_place(type, r1, nx, ny)) {
            falling.x = nx;
            falling.y = ny;
            falling.rot = r1;
            return true;
        }
    }

    return false;
}

/* ==============================
 * Lock / clear / spawn
 * ============================== */
static void lock_falling(void) {
    uint16_t m = SHAPE[falling.type][falling.rot & 3];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!mask_has(m, r, c)) continue;
            int br = falling.y + r;
            int bc = falling.x + c;
            if (br >= 0 && br < BOARD_H && bc >= 0 && bc < BOARD_W) {
                board_locked[br][bc] = 1;
            }
        }
    }
}

static void clear_lines(void) {
    for (int r = BOARD_H - 1; r >= 0; r--) {
        bool full = true;
        for (int c = 0; c < BOARD_W; c++) {
            if (!board_locked[r][c]) { full = false; break; }
        }
        if (!full) continue;

        /* pull down */
        for (int rr = r; rr > 0; rr--) {
            for (int c = 0; c < BOARD_W; c++) {
                board_locked[rr][c] = board_locked[rr - 1][c];
            }
        }
        for (int c = 0; c < BOARD_W; c++) board_locked[0][c] = 0;

        /* re-check same row after collapsing */
        r++;
    }
}

static uint8_t next_piece_counter;

static uint8_t rand_piece_type(void) {
    /* sys_rand32_get is available in Zephyr; fallback to counter if needed */
    uint32_t v = sys_rand32_get();
    uint8_t t = (uint8_t)(v % TET_COUNT);
    if (t >= TET_COUNT) t = (next_piece_counter++) % TET_COUNT;
    return t;
}

static void spawn_piece(void) {
    falling.type = rand_piece_type();
    falling.rot  = 0;

    /* spawn x centered-ish */
    falling.x = 3;
    falling.y = 0;

    /* If spawn collides, just reset board for demo (game over later) */
    if (!can_place(falling.type, falling.rot, falling.x, falling.y)) {
        for (int r = 0; r < BOARD_H; r++)
            for (int c = 0; c < BOARD_W; c++)
                board_locked[r][c] = 0;
        falling.x = 3; falling.y = 0; falling.rot = 0;
    }
}

/* one-step fall:
 * return true if moved, false if blocked -> lock+clear+spawn
 */
static bool do_fall_one_and_handle_lock(void) {
    int ny = falling.y + 1;
    if (can_place(falling.type, falling.rot, falling.x, ny)) {
        falling.y = ny;
        return true;
    }

    /* lock */
    lock_falling();
    clear_lines();
    spawn_piece();
    return false;
}

/* ==============================
 * Renderer: prev/next board lines (locked + falling overlay)
 * ============================== */
struct update_line {
    int line_index;                 // absolute editor line index
    char text[BOARD_W + 2];         // ".......... " + '\0'
};

static char render_prev[BOARD_H][BOARD_W + 2];
static char render_next[BOARD_H][BOARD_W + 2];

static void build_row_string(int row, char out[BOARD_W + 2]) {
    for (int c = 0; c < BOARD_W; c++) {
        out[c] = board_locked[row][c] ? 'x' : '.';
    }

    /* overlay falling */
    uint16_t m = SHAPE[falling.type][falling.rot & 3];
    for (int r = 0; r < 4; r++) {
        int br = falling.y + r;
        if (br != row) continue;
        for (int c = 0; c < 4; c++) {
            if (!mask_has(m, r, c)) continue;
            int bc = falling.x + c;
            if (bc >= 0 && bc < BOARD_W) out[bc] = 'x';
        }
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

        /* optimistic commit */
        for (int i = 0; i < BOARD_W + 2; i++) {
            render_prev[r][i] = render_next[r][i];
            if (render_next[r][i] == '\0') break;
        }

        n++;
    }
    return n;
}

/* ==============================
 * Render engine (async)
 * ============================== */
enum render_mode {
    RENDER_IDLE = 0,
    RENDER_CLEAR_EDITOR,
    RENDER_TYPE_FULL,
    RENDER_REPLACE_LINE_SCRIPT,
};

enum clear_phase { CLP_CTRL_A = 0, CLP_BS, CLP_DONE };
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

enum request_type { REQ_NONE = 0, REQ_CLEAR_ONLY, REQ_RESET_AND_DRAW };

struct render_state {
    bool inited;
    bool running;

    enum request_type req;
    enum render_mode mode;

    enum clear_phase clear_phase;

    const char *text;
    size_t text_idx;

    enum script_phase phase;
    int down_remaining;
    const char *line_text;
    size_t line_idx;

    struct update_line batch[MAX_UPDATE_LINES];
    uint8_t batch_len;
    uint8_t batch_pos;

    struct k_work_delayable work;
};

static struct render_state rs;
static struct k_work_delayable gravity_work;

/* forward */
static void apply_pending_and_redraw_once(void);

static void stop_render(void) {
    rs.running = false;
    rs.req = REQ_NONE;
    rs.mode = RENDER_IDLE;
    rs.batch_len = 0;
    rs.batch_pos = 0;
    k_work_cancel_delayable(&rs.work);
}

static void start_clear_editor_async(enum request_type req_after) {
    rs.req = req_after;
    rs.mode = RENDER_CLEAR_EDITOR;
    rs.clear_phase = CLP_CTRL_A;
    rs.running = true;
    k_work_reschedule(&rs.work, K_NO_WAIT);
}

static void start_full_text_async(const char *text) {
    rs.mode = RENDER_TYPE_FULL;
    rs.text = text;
    rs.text_idx = 0;
    rs.running = true;
    k_work_reschedule(&rs.work, K_NO_WAIT);
}

static void start_replace_line_script(int line_index_zero_based, const char *line) {
    rs.mode = RENDER_REPLACE_LINE_SCRIPT;
    rs.phase = SPH_CTRL_HOME;
    rs.down_remaining = line_index_zero_based;
    rs.line_text = line;
    rs.line_idx = 0;

    rs.running = true;
    k_work_reschedule(&rs.work, K_NO_WAIT);
}

static void start_batch(struct update_line *lines, uint8_t len) {
    if (len == 0) return;
    if (len > MAX_UPDATE_LINES) len = MAX_UPDATE_LINES;

    for (uint8_t i = 0; i < len; i++) rs.batch[i] = lines[i];
    rs.batch_len = len;
    rs.batch_pos = 0;

    start_replace_line_script(rs.batch[0].line_index, rs.batch[0].text);
}

/* full frame buffer (typed on reset) */
static char full_frame_buf[256];

static void build_full_frame_text(void) {
    size_t w = 0;
    /* 括弧をやめて文字化け回避 */
    const char *hdr = "tetris zmk\nscore 0000\n\n";
    for (size_t i = 0; hdr[i] && w + 1 < sizeof(full_frame_buf); i++) {
        full_frame_buf[w++] = hdr[i];
    }

    rebuild_render_next();
    for (int r = 0; r < BOARD_H; r++) {
        for (int i = 0; render_next[r][i] && w + 1 < sizeof(full_frame_buf); i++) {
            full_frame_buf[w++] = render_next[r][i];
        }
        if (w + 1 < sizeof(full_frame_buf)) full_frame_buf[w++] = '\n';
    }

    full_frame_buf[w] = '\0';
}

static void request_diff_render(void) {
    rebuild_render_next();

    struct update_line lines[MAX_UPDATE_LINES];
    uint8_t len = make_diff_lines(lines);
    if (len == 0) return;
    if (rs.running) return;

    start_batch(lines, len);
}

static void render_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!rs.running) return;

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

            if (next == REQ_RESET_AND_DRAW) {
                start_full_text_async(full_frame_buf);
                return;
            }

            rs.running = false;
            rs.mode = RENDER_IDLE;
            rs.batch_len = 0;
            rs.batch_pos = 0;

            apply_pending_and_redraw_once();
            return;
        }
        }
    }

    if (rs.mode == RENDER_TYPE_FULL) {
        char c = rs.text[rs.text_idx];
        if (c == '\0') {
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
        if (char_to_keycode(c, &kc)) tap(kc);
        rs.text_idx++;
        k_work_reschedule(&rs.work, K_MSEC(delay_for_char(c)));
        return;
    }

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
            if (rs.batch_pos + 1 < rs.batch_len) {
                rs.batch_pos++;
                start_replace_line_script(rs.batch[rs.batch_pos].line_index,
                                          rs.batch[rs.batch_pos].text);
                return;
            }

            rs.running = false;
            rs.mode = RENDER_IDLE;
            rs.batch_len = 0;
            rs.batch_pos = 0;

            apply_pending_and_redraw_once();
            return;
        }
    }

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
    schedule_gravity_idle();
}

static void apply_pending_and_redraw_once(void) {
    if (rs.running) return;

    bool changed = false;

    /* dx */
    if (pending_dx != 0) {
        int dx = pending_dx;
        pending_dx = 0;

        int nx = falling.x + dx;
        if (can_place(falling.type, falling.rot, nx, falling.y)) {
            falling.x = nx;
            changed = true;
        }
    }

    /* rotate cw */
    if (pending_rot_cw > 0) {
        pending_rot_cw = 0;
        if (try_rotate_cw_with_kick()) changed = true;
    }

    /* soft drop */
    if (pending_soft_drop > 0) {
        int n = pending_soft_drop;
        pending_soft_drop = 0;
        for (int i = 0; i < n; i++) {
            do_fall_one_and_handle_lock();
            changed = true;
        }
    }

    if (changed) request_diff_render();
}

static void on_user_dx(int dx) {
    on_user_input_common();
    if (rs.running) { pending_dx += dx; return; }

    int nx = falling.x + dx;
    if (can_place(falling.type, falling.rot, nx, falling.y)) {
        falling.x = nx;
        request_diff_render();
    }
}

static void on_user_rotate_cw(void) {
    on_user_input_common();
    if (rs.running) { pending_rot_cw++; return; }

    if (try_rotate_cw_with_kick()) request_diff_render();
}

static void on_user_soft_drop(void) {
    on_user_input_common();
    if (rs.running) { pending_soft_drop++; return; }

    do_fall_one_and_handle_lock();
    request_diff_render();
}

static void gravity_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint32_t now = (uint32_t)k_uptime_get();
    uint32_t since_input = now - last_input_ms;

    if (since_input < idle_before_fall_ms) {
        uint32_t remain = idle_before_fall_ms - since_input;
        if (remain < 50) remain = 50;
        k_work_reschedule(&gravity_work, K_MSEC(remain));
        return;
    }

    if (rs.running) {
        k_work_reschedule(&gravity_work, K_MSEC(30));
        return;
    }

    /* fall */
    do_fall_one_and_handle_lock();
    request_diff_render();
    schedule_gravity_interval();
}

/* ==============================
 * Init/reset
 * ============================== */
static void reset_game(void) {
    for (int r = 0; r < BOARD_H; r++) {
        for (int c = 0; c < BOARD_W; c++) board_locked[r][c] = 0;
        for (int i = 0; i < BOARD_W + 2; i++) render_prev[r][i] = '\0';
    }

    pending_dx = 0;
    pending_rot_cw = 0;
    pending_soft_drop = 0;

    last_input_ms = (uint32_t)k_uptime_get();

    spawn_piece();

    rebuild_render_next();
    for (int r = 0; r < BOARD_H; r++) {
        for (int i = 0; i < BOARD_W + 2; i++) render_prev[r][i] = '\0';
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
 * 12: rotate CW (with wall-kick)
 * 13: soft drop (1 step)
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
    case 0:
        stop_render();
        k_work_cancel_delayable(&gravity_work);

        reset_game();
        build_full_frame_text();
        start_clear_editor_async(REQ_RESET_AND_DRAW);

        schedule_gravity_idle();
        return ZMK_BEHAVIOR_OPAQUE;

    case 1:
        stop_render();
        k_work_cancel_delayable(&gravity_work);

        start_clear_editor_async(REQ_CLEAR_ONLY);
        return ZMK_BEHAVIOR_OPAQUE;

    case 10:
        on_user_dx(-1);
        return ZMK_BEHAVIOR_OPAQUE;

    case 11:
        on_user_dx(+1);
        return ZMK_BEHAVIOR_OPAQUE;

    case 12:
        on_user_rotate_cw();
        return ZMK_BEHAVIOR_OPAQUE;

    case 13:
        on_user_soft_drop();
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
