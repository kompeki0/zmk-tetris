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

#define MAX_UPDATE_LINES 10

/* gravity */
static uint16_t idle_before_fall_ms = 2000;
static uint16_t fall_interval_ms    = 700;

/* clear effect */
static uint8_t  clear_frames   = 4;   // 点滅回数
static uint16_t clear_frame_ms = 110; // 点滅間隔

/* delays for editor ops (stability) */
static uint32_t delay_for_char(char c) { return (c == '\n') ? 25 : 6; }
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
 * Text typing
 * ============================== */
static bool char_to_keycode(char c, uint32_t *out) {
    switch (c) {
    case 'x': *out = X; return true;
    case '.': *out = DOT; return true;
    case ' ': *out = SPACE; return true;
    case '\n': *out = ENTER; return true;

    case '=': *out = EQUAL; return true;
    case '-': *out = MINUS; return true;

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

    /* lowercase letters */
    case 'a': *out = A; return true;
    case 'b': *out = B; return true;
    case 'c': *out = C; return true;
    case 'd': *out = D; return true;
    case 'e': *out = E; return true;
    case 'f': *out = F; return true;
    case 'g': *out = G; return true;
    case 'h': *out = H; return true;
    case 'i': *out = I; return true;
    case 'j': *out = J; return true;
    case 'k': *out = K; return true;
    case 'l': *out = L; return true;
    case 'm': *out = M; return true;
    case 'n': *out = N; return true;
    case 'o': *out = O; return true;
    case 'p': *out = P; return true;
    case 'q': *out = Q; return true;
    case 'r': *out = R; return true;
    case 's': *out = S; return true;
    case 't': *out = T; return true;
    case 'u': *out = U; return true;
    case 'v': *out = V; return true;
    case 'w': *out = W; return true;
    case 'y': *out = Y; return true;
    case 'z': *out = Z; return true;

    default: return false;
    }
}

static void clear_editor_async_start(void); /* forward */

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

/* queued inputs while rendering / clear animation */
static int pending_dx;
static int pending_rot_cw;
static int pending_rot_ccw;
static bool pending_hard_drop;
static int pending_soft_drop;
static uint32_t last_input_ms;

/* line clear state */
static bool clearing;
static uint16_t clear_mask;   // row bits
static uint8_t clear_step;

/* ==============================
 * 4x4 masks (bit 0 = (0,0), bit 15 = (3,3))
 * ============================== */
#define BIT_AT(r, c) ((uint16_t)(1u << (((r) * 4) + (c))))
static inline bool mask_has(uint16_t m, int r, int c) { return (m & BIT_AT(r, c)) != 0; }

static const uint16_t SHAPE[TET_COUNT][4] = {
    /* I */
    {
        BIT_AT(1,0) | BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(1,3),
        BIT_AT(0,2) | BIT_AT(1,2) | BIT_AT(2,2) | BIT_AT(3,2),
        BIT_AT(2,0) | BIT_AT(2,1) | BIT_AT(2,2) | BIT_AT(2,3),
        BIT_AT(0,1) | BIT_AT(1,1) | BIT_AT(2,1) | BIT_AT(3,1),
    },
    /* O */
    {
        BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(2,1) | BIT_AT(2,2),
        BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(2,1) | BIT_AT(2,2),
        BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(2,1) | BIT_AT(2,2),
        BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(2,1) | BIT_AT(2,2),
    },
    /* T */
    {
        BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(1,3) | BIT_AT(2,2),
        BIT_AT(0,2) | BIT_AT(1,2) | BIT_AT(2,2) | BIT_AT(1,3),
        BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(1,3) | BIT_AT(0,2),
        BIT_AT(0,2) | BIT_AT(1,2) | BIT_AT(2,2) | BIT_AT(1,1),
    },
    /* S */
    {
        BIT_AT(1,2) | BIT_AT(1,3) | BIT_AT(2,1) | BIT_AT(2,2),
        BIT_AT(0,2) | BIT_AT(1,2) | BIT_AT(1,3) | BIT_AT(2,3),
        BIT_AT(1,2) | BIT_AT(1,3) | BIT_AT(2,1) | BIT_AT(2,2),
        BIT_AT(0,2) | BIT_AT(1,2) | BIT_AT(1,3) | BIT_AT(2,3),
    },
    /* Z */
    {
        BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(2,2) | BIT_AT(2,3),
        BIT_AT(0,3) | BIT_AT(1,2) | BIT_AT(1,3) | BIT_AT(2,2),
        BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(2,2) | BIT_AT(2,3),
        BIT_AT(0,3) | BIT_AT(1,2) | BIT_AT(1,3) | BIT_AT(2,2),
    },
    /* J */
    {
        BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(1,3) | BIT_AT(2,1),
        BIT_AT(0,2) | BIT_AT(0,3) | BIT_AT(1,2) | BIT_AT(2,2),
        BIT_AT(0,3) | BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(1,3),
        BIT_AT(0,2) | BIT_AT(1,2) | BIT_AT(2,2) | BIT_AT(2,1),
    },
    /* L */
    {
        BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(1,3) | BIT_AT(2,3),
        BIT_AT(0,2) | BIT_AT(1,2) | BIT_AT(2,2) | BIT_AT(0,3),
        BIT_AT(0,1) | BIT_AT(1,1) | BIT_AT(1,2) | BIT_AT(1,3),
        BIT_AT(2,2) | BIT_AT(0,1) | BIT_AT(1,1) | BIT_AT(2,1),
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
 * Wall kick (SRS-like) CW + CCW
 * ============================== */
static const int8_t KICK_JLSTZ_CW[4][5][2] = {
    {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}}, // 0->1
    {{0,0},{ 1,0},{ 1,-1},{0,2},{ 1,2}},  // 1->2
    {{0,0},{ 1,0},{ 1,1},{0,-2},{ 1,-2}}, // 2->3
    {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}},  // 3->0
};
static const int8_t KICK_JLSTZ_CCW[4][5][2] = {
    /* 0->3 */ {{0,0},{ 1,0},{ 1,1},{0,-2},{ 1,-2}},
    /* 1->0 */ {{0,0},{ 1,0},{ 1,-1},{0,2},{ 1,2}},
    /* 2->1 */ {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}},
    /* 3->2 */ {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}},
};

static const int8_t KICK_I_CW[4][5][2] = {
    {{0,0},{-2,0},{ 1,0},{-2,-1},{ 1,2}}, // 0->1
    {{0,0},{-1,0},{ 2,0},{-1,2},{ 2,-1}}, // 1->2
    {{0,0},{ 2,0},{-1,0},{ 2,1},{-1,-2}}, // 2->3
    {{0,0},{ 1,0},{-2,0},{ 1,-2},{-2,1}}, // 3->0
};
static const int8_t KICK_I_CCW[4][5][2] = {
    /* 0->3 */ {{0,0},{-1,0},{ 2,0},{-1,2},{ 2,-1}},
    /* 1->0 */ {{0,0},{ 2,0},{-1,0},{ 2,1},{-1,-2}},
    /* 2->1 */ {{0,0},{ 1,0},{-2,0},{ 1,-2},{-2,1}},
    /* 3->2 */ {{0,0},{-2,0},{ 1,0},{-2,-1},{ 1,2}},
};

static bool try_rotate(int dir /* +1 CW, -1 CCW */) {
    uint8_t type = falling.type;
    uint8_t r0 = falling.rot & 3;
    uint8_t r1 = (dir > 0) ? ((r0 + 1) & 3) : ((r0 + 3) & 3);

    if (type == TET_O) {
        if (can_place(type, r1, falling.x, falling.y)) { falling.rot = r1; return true; }
        return false;
    }

    const int8_t (*kicks)[2] = NULL;
    if (type == TET_I) kicks = (dir > 0) ? KICK_I_CW[r0] : KICK_I_CCW[r0];
    else kicks = (dir > 0) ? KICK_JLSTZ_CW[r0] : KICK_JLSTZ_CCW[r0];

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
            if (br >= 0 && br < BOARD_H && bc >= 0 && bc < BOARD_W) board_locked[br][bc] = 1;
        }
    }
}

static uint16_t detect_full_lines(void) {
    uint16_t mask = 0;
    for (int r = 0; r < BOARD_H; r++) {
        bool full = true;
        for (int c = 0; c < BOARD_W; c++) {
            if (!board_locked[r][c]) { full = false; break; }
        }
        if (full) mask |= (1u << r);
    }
    return mask;
}

static void apply_line_clear(uint16_t mask) {
    if (!mask) return;

    int dst = BOARD_H - 1;
    for (int src = BOARD_H - 1; src >= 0; src--) {
        if (mask & (1u << src)) continue;
        if (dst != src) {
            for (int c = 0; c < BOARD_W; c++) board_locked[dst][c] = board_locked[src][c];
        }
        dst--;
    }
    for (int r = dst; r >= 0; r--) for (int c = 0; c < BOARD_W; c++) board_locked[r][c] = 0;
}

static uint8_t next_piece_counter;
static uint8_t rand_piece_type(void) {
    uint32_t v = sys_rand32_get();
    uint8_t t = (uint8_t)(v % TET_COUNT);
    if (t >= TET_COUNT) t = (next_piece_counter++) % TET_COUNT;
    return t;
}

static void spawn_piece(void) {
    falling.type = rand_piece_type();
    falling.rot  = 0;
    falling.x = 3;
    falling.y = 0;

    if (!can_place(falling.type, falling.rot, falling.x, falling.y)) {
        for (int r = 0; r < BOARD_H; r++) for (int c = 0; c < BOARD_W; c++) board_locked[r][c] = 0;
        falling.type = TET_O; falling.rot = 0; falling.x = 3; falling.y = 0;
    }
}

/* ==============================
 * Renderer: prev/next board lines (locked + falling overlay)
 * ============================== */
struct update_line {
    int line_index;
    char text[BOARD_W + 2];   // ".......... " + '\0'
};

static char render_prev[BOARD_H][BOARD_W + 2];
static char render_next[BOARD_H][BOARD_W + 2];

static void build_row_string(int row, char out[BOARD_W + 2]) {
    /* clear effect overrides (no next piece during clearing) */
    if (clearing && (clear_mask & (1u << row))) {
        bool on = ((clear_step % 2) == 0);
        for (int c = 0; c < BOARD_W; c++) out[c] = on ? '=' : '.';
        out[BOARD_W] = ' ';
        out[BOARD_W + 1] = '\0';
        return;
    }

    for (int c = 0; c < BOARD_W; c++) out[c] = board_locked[row][c] ? 'x' : '.';

    if (!clearing) {
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
    }

    out[BOARD_W] = ' ';
    out[BOARD_W + 1] = '\0';
}

static void rebuild_render_next(void) {
    for (int r = 0; r < BOARD_H; r++) build_row_string(r, render_next[r]);
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
enum render_mode { RENDER_IDLE = 0, RENDER_CLEAR_EDITOR, RENDER_TYPE_FULL, RENDER_REPLACE_LINE_SCRIPT };
enum clear_phase { CLP_CTRL_A = 0, CLP_BS, CLP_DONE };
enum script_phase {
    SPH_CTRL_HOME = 0, SPH_DOWN_REPEAT, SPH_HOME, SPH_SHIFT_END_PRESS, SPH_END_TAP,
    SPH_SHIFT_END_RELEASE, SPH_BACKSPACE, SPH_TYPE_LINE, SPH_DONE
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
static struct k_work_delayable clear_work;

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

/* full frame buffer */
static char full_frame_buf[256];

static void build_full_frame_text(void) {
    size_t w = 0;
    const char *hdr = "tetris zmk\nscore 0000\n\n";
    for (size_t i = 0; hdr[i] && w + 1 < sizeof(full_frame_buf); i++) full_frame_buf[w++] = hdr[i];

    rebuild_render_next();
    for (int r = 0; r < BOARD_H; r++) {
        for (int i = 0; render_next[r][i] && w + 1 < sizeof(full_frame_buf); i++) full_frame_buf[w++] = render_next[r][i];
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
                start_replace_line_script(rs.batch[rs.batch_pos].line_index, rs.batch[rs.batch_pos].text);
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
 * Gravity / Clear scheduling
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

static void begin_clear_animation(uint16_t mask) {
    clearing = true;
    clear_mask = mask;
    clear_step = 0;

    /* draw first effect immediately if possible */
    request_diff_render();
    k_work_reschedule(&clear_work, K_MSEC(clear_frame_ms));
}

static void on_piece_landed(void) {
    lock_falling();
    uint16_t mask = detect_full_lines();
    if (mask) {
        begin_clear_animation(mask);
        return;
    }
    spawn_piece();
    request_diff_render();
    schedule_gravity_idle();
}

static bool do_fall_one(void) {
    int ny = falling.y + 1;
    if (can_place(falling.type, falling.rot, falling.x, ny)) {
        falling.y = ny;
        return true;
    }
    return false;
}

static void hard_drop_and_land(void) {
    while (do_fall_one()) {
        /* keep falling */
    }
    on_piece_landed();
}

static void clear_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!clearing) return;

    if (rs.running) {
        k_work_reschedule(&clear_work, K_MSEC(30));
        return;
    }

    clear_step++;

    if (clear_step < clear_frames) {
        request_diff_render();
        k_work_reschedule(&clear_work, K_MSEC(clear_frame_ms));
        return;
    }

    /* finish: actually clear + spawn next, then draw */
    uint16_t mask = clear_mask;

    clearing = false;
    clear_mask = 0;
    clear_step = 0;

    apply_line_clear(mask);
    spawn_piece();

    request_diff_render();
    schedule_gravity_idle();
}

static void gravity_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (clearing) {
        k_work_reschedule(&gravity_work, K_MSEC(50));
        return;
    }

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

    if (do_fall_one()) {
        request_diff_render();
        schedule_gravity_interval();
        return;
    }

    /* landed */
    on_piece_landed();
}

/* ==============================
 * Input handling (queue while rendering / clearing)
 * ============================== */
static void apply_pending_and_redraw_once(void) {
    if (rs.running || clearing) return;

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

    /* rotate: apply CCW first if queued, then CW */
    while (pending_rot_ccw > 0) {
        pending_rot_ccw--;
        if (try_rotate(-1)) changed = true;
    }
    while (pending_rot_cw > 0) {
        pending_rot_cw--;
        if (try_rotate(+1)) changed = true;
    }

    /* hard drop overrides */
    if (pending_hard_drop) {
        pending_hard_drop = false;
        hard_drop_and_land();
        return; // landing path already schedules draw
    }

    /* soft drop */
    if (pending_soft_drop > 0) {
        int n = pending_soft_drop;
        pending_soft_drop = 0;
        for (int i = 0; i < n; i++) {
            if (do_fall_one()) {
                changed = true;
            } else {
                on_piece_landed();
                return;
            }
        }
        changed = true;
    }

    if (changed) request_diff_render();
}

static void on_user_dx(int dx) {
    on_user_input_common();
    if (rs.running || clearing) { pending_dx += dx; return; }

    int nx = falling.x + dx;
    if (can_place(falling.type, falling.rot, nx, falling.y)) {
        falling.x = nx;
        request_diff_render();
    }
}

static void on_user_rotate(int dir) {
    on_user_input_common();
    if (rs.running || clearing) {
        if (dir > 0) pending_rot_cw++;
        else pending_rot_ccw++;
        return;
    }
    if (try_rotate(dir)) request_diff_render();
}

static void on_user_soft_drop(void) {
    on_user_input_common();
    if (rs.running || clearing) { pending_soft_drop++; return; }

    if (do_fall_one()) {
        request_diff_render();
    } else {
        on_piece_landed();
    }
}

static void on_user_hard_drop(void) {
    on_user_input_common();
    if (rs.running || clearing) { pending_hard_drop = true; return; }
    hard_drop_and_land();
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
    pending_rot_ccw = 0;
    pending_soft_drop = 0;
    pending_hard_drop = false;

    clearing = false;
    clear_mask = 0;
    clear_step = 0;

    last_input_ms = (uint32_t)k_uptime_get();

    spawn_piece();

    rebuild_render_next();
    for (int r = 0; r < BOARD_H; r++) for (int i = 0; i < BOARD_W + 2; i++) render_prev[r][i] = '\0';
}

/* ==============================
 * Behavior entry
 *
 * Commands:
 * 0: reset + async clear + async draw + start gravity (idle)
 * 1: async clear editor
 * 10: left
 * 11: right
 * 12: rotate CW (wall-kick)
 * 13: soft drop (1 step)
 * 14: rotate CCW (wall-kick)
 * 15: hard drop
 * ============================== */
static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    uint32_t cmd = binding->param1;

    if (!rs.inited) {
        k_work_init_delayable(&rs.work, render_work_handler);
        k_work_init_delayable(&gravity_work, gravity_work_handler);
        k_work_init_delayable(&clear_work, clear_work_handler);
        rs.inited = true;
    }

    LOG_DBG("tetris cmd=%d", cmd);

    switch (cmd) {
    case 0:
        stop_render();
        k_work_cancel_delayable(&gravity_work);
        k_work_cancel_delayable(&clear_work);

        reset_game();
        build_full_frame_text();
        start_clear_editor_async(REQ_RESET_AND_DRAW);

        schedule_gravity_idle();
        return ZMK_BEHAVIOR_OPAQUE;

    case 1:
        stop_render();
        k_work_cancel_delayable(&gravity_work);
        k_work_cancel_delayable(&clear_work);

        start_clear_editor_async(REQ_CLEAR_ONLY);
        return ZMK_BEHAVIOR_OPAQUE;

    case 10:
        on_user_dx(-1);
        return ZMK_BEHAVIOR_OPAQUE;

    case 11:
        on_user_dx(+1);
        return ZMK_BEHAVIOR_OPAQUE;

    case 12:
        on_user_rotate(+1);
        return ZMK_BEHAVIOR_OPAQUE;

    case 13:
        on_user_soft_drop();
        return ZMK_BEHAVIOR_OPAQUE;

    case 14:
        on_user_rotate(-1);
        return ZMK_BEHAVIOR_OPAQUE;

    case 15:
        on_user_hard_drop();
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
