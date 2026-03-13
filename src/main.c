/*
 * Blyn 1.0 — A modern modal CLI code editor
 * Inspired by Vim & Emacs. Designed for 2026.
 *
 * Modes:
 *   NORMAL  (default) — navigate with hjkl / arrows
 *   INSERT            — type text; ESC returns to NORMAL
 *   COMMAND           — entered via ':' or '/'
 *
 * Key bindings summary at bottom of this file.
 */

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Configuration ─────────────────────────────────────────────────────── */
#define MAX_LINES     65536
#define GUTTER_WIDTH  5        /* "1234 " */
#define VERSION       "1.0"
#define TABSIZE       4

/* ── Color pair IDs ────────────────────────────────────────────────────── */
#define CLR_NORMAL    1
#define CLR_GUTTER    2
#define CLR_CURLINE   3
#define CLR_STATUSN   4   /* NORMAL mode bar   — green  */
#define CLR_STATUSI   5   /* INSERT mode bar   — blue   */
#define CLR_STATUSC   6   /* COMMAND mode bar  — cyan   */
#define CLR_SEARCH    7   /* inline search match         */
#define CLR_WELCOME   8   /* welcome accent colour       */
#define CLR_TILDE     9   /* empty-line tildes           */

/* ── Editor modes ───────────────────────────────────────────────────────── */
typedef enum { MODE_NORMAL, MODE_INSERT, MODE_COMMAND } Mode;

/* ── Editor state ───────────────────────────────────────────────────────── */
typedef struct {
    char  **lines;          /* heap-allocated line array  */
    int     num_lines;
    int     row, col;       /* cursor position in file    */
    int     scroll_row;     /* first visible line         */
    Mode    mode;
    char    filename[256];
    int     dirty;          /* unsaved flag               */
    char    status_msg[256];
    char    cmd_buf[256];   /* command / search bar       */
    int     cmd_len;
    char    search[256];    /* last search pattern        */
    char   *undo_line;      /* single-level line undo     */
    int     undo_row;
    int     g_pressed;      /* partial 'gg' detection     */
    int     d_pressed;      /* partial 'dd' detection     */
} Editor;

static Editor E;

/* ── Utility ─────────────────────────────────────────────────────────────── */

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *d = malloc(n + 256);   /* extra headroom for in-place inserts */
    if (!d) { endwin(); fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(d, s, n + 1);
    return d;
}

static void ensure_line(int r) {
    while (E.num_lines <= r && E.num_lines < MAX_LINES) {
        E.lines[E.num_lines++] = xstrdup("");
    }
}

static void clamp_col(void) {
    ensure_line(E.row);
    int len = (int)strlen(E.lines[E.row]);
    int max = (E.mode == MODE_INSERT) ? len : (len > 0 ? len - 1 : 0);
    if (E.col > max) E.col = max;
    if (E.col < 0)   E.col = 0;
}

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
    va_end(ap);
}

/* ── Init / Free ─────────────────────────────────────────────────────────── */

static void editor_init(void) {
    E.lines = calloc(MAX_LINES, sizeof(char *));
    if (!E.lines) { fprintf(stderr, "OOM\n"); exit(1); }
    E.lines[0]   = xstrdup("");
    E.num_lines  = 1;
    E.row = E.col = E.scroll_row = 0;
    E.mode        = MODE_NORMAL;
    E.dirty       = 0;
    E.filename[0] = '\0';
    E.status_msg[0]= '\0';
    E.cmd_buf[0]  = '\0';
    E.cmd_len     = 0;
    E.search[0]   = '\0';
    E.undo_line   = NULL;
    E.undo_row    = 0;
    E.g_pressed   = 0;
    E.d_pressed   = 0;
}

static void editor_free(void) {
    for (int i = 0; i < E.num_lines; i++) free(E.lines[i]);
    free(E.lines);
    free(E.undo_line);
}

/* ── File I/O ─────────────────────────────────────────────────────────────── */

static void editor_open(const char *fname) {
    strncpy(E.filename, fname, sizeof(E.filename) - 1);
    FILE *fp = fopen(fname, "r");
    if (!fp) {
        set_status("New file: %s", fname);
        return;
    }
    for (int i = 0; i < E.num_lines; i++) { free(E.lines[i]); E.lines[i] = NULL; }
    E.num_lines = 0;
    char tmp[8192];
    while (E.num_lines < MAX_LINES && fgets(tmp, sizeof(tmp), fp)) {
        tmp[strcspn(tmp, "\n")] = '\0';
        E.lines[E.num_lines++] = xstrdup(tmp);
    }
    fclose(fp);
    if (E.num_lines == 0) { E.lines[0] = xstrdup(""); E.num_lines = 1; }
    set_status("\"%s\" — %d lines", fname, E.num_lines);
}

static void editor_save(void) {
    if (E.filename[0] == '\0') {
        E.mode = MODE_COMMAND;
        snprintf(E.cmd_buf, sizeof(E.cmd_buf), "w ");
        E.cmd_len = 2;
        return;
    }
    FILE *fp = fopen(E.filename, "w");
    if (!fp) { set_status("ERROR: Cannot write \"%s\"", E.filename); return; }
    for (int i = 0; i < E.num_lines; i++) fprintf(fp, "%s\n", E.lines[i]);
    fclose(fp);
    E.dirty = 0;
    set_status("\"%s\" written — %d lines", E.filename, E.num_lines);
}

/* ── Undo ─────────────────────────────────────────────────────────────────── */

static void save_undo(void) {
    free(E.undo_line);
    ensure_line(E.row);
    E.undo_line = xstrdup(E.lines[E.row]);
    E.undo_row  = E.row;
}

static void editor_undo(void) {
    if (!E.undo_line) { set_status("Nothing to undo."); return; }
    free(E.lines[E.undo_row]);
    E.lines[E.undo_row] = xstrdup(E.undo_line);
    E.row = E.undo_row;
    E.col = 0;
    E.dirty = 1;
    set_status("Undo.");
}

/* ── Editing ─────────────────────────────────────────────────────────────── */

static void editor_insert_char(int ch) {
    ensure_line(E.row);
    save_undo();
    char *line = E.lines[E.row];
    int   len  = (int)strlen(line);
    /* Ensure capacity: realloc if within original headroom isn't enough */
    char *nl = realloc(line, (size_t)(len + 4));
    if (!nl) return;
    E.lines[E.row] = nl;
    memmove(&nl[E.col + 1], &nl[E.col], (size_t)(len - E.col + 1));
    nl[E.col] = (char)ch;
    E.col++;
    E.dirty = 1;
}

static void editor_insert_newline(void) {
    ensure_line(E.row);
    save_undo();
    char *cur  = E.lines[E.row];
    char *tail = xstrdup(cur + E.col);
    cur[E.col] = '\0';
    if (E.num_lines < MAX_LINES) {
        memmove(&E.lines[E.row + 2], &E.lines[E.row + 1],
                (size_t)(E.num_lines - E.row - 1) * sizeof(char *));
        E.lines[E.row + 1] = tail;
        E.num_lines++;
    } else {
        free(tail);
    }
    E.row++;
    E.col   = 0;
    E.dirty = 1;
}

static void editor_backspace(void) {
    ensure_line(E.row);
    save_undo();
    if (E.col > 0) {
        char *line = E.lines[E.row];
        int   len  = (int)strlen(line);
        memmove(&line[E.col - 1], &line[E.col], (size_t)(len - E.col + 1));
        E.col--;
        E.dirty = 1;
    } else if (E.row > 0) {
        /* Join with previous line */
        char *prev     = E.lines[E.row - 1];
        char *cur      = E.lines[E.row];
        int   prev_len = (int)strlen(prev);
        int   cur_len  = (int)strlen(cur);
        char *joined   = realloc(prev, (size_t)(prev_len + cur_len + 4));
        if (!joined) return;
        memcpy(joined + prev_len, cur, (size_t)(cur_len + 1));
        E.lines[E.row - 1] = joined;
        free(cur);
        memmove(&E.lines[E.row], &E.lines[E.row + 1],
                (size_t)(E.num_lines - E.row - 1) * sizeof(char *));
        E.num_lines--;
        E.row--;
        E.col   = prev_len;
        E.dirty = 1;
    }
}

static void editor_delete_char_at(void) {
    ensure_line(E.row);
    save_undo();
    char *line = E.lines[E.row];
    int   len  = (int)strlen(line);
    if (E.col < len) {
        memmove(&line[E.col], &line[E.col + 1], (size_t)(len - E.col));
        clamp_col();
        E.dirty = 1;
    }
}

static void editor_delete_line(void) {
    save_undo();
    free(E.lines[E.row]);
    if (E.num_lines > 1) {
        memmove(&E.lines[E.row], &E.lines[E.row + 1],
                (size_t)(E.num_lines - E.row - 1) * sizeof(char *));
        E.num_lines--;
        if (E.row >= E.num_lines) E.row = E.num_lines - 1;
    } else {
        E.lines[0] = xstrdup("");
    }
    clamp_col();
    E.dirty = 1;
    set_status("Line deleted.");
}

static void editor_open_line_below(void) {
    ensure_line(E.row);
    if (E.num_lines < MAX_LINES) {
        memmove(&E.lines[E.row + 2], &E.lines[E.row + 1],
                (size_t)(E.num_lines - E.row - 1) * sizeof(char *));
        E.lines[E.row + 1] = xstrdup("");
        E.num_lines++;
    }
    E.row++;
    E.col   = 0;
    E.mode  = MODE_INSERT;
    E.dirty = 1;
}

static void editor_open_line_above(void) {
    ensure_line(E.row);
    if (E.num_lines < MAX_LINES) {
        memmove(&E.lines[E.row + 1], &E.lines[E.row],
                (size_t)(E.num_lines - E.row) * sizeof(char *));
        E.lines[E.row] = xstrdup("");
        E.num_lines++;
    }
    E.col   = 0;
    E.mode  = MODE_INSERT;
    E.dirty = 1;
}

static void editor_kill_to_eol(void) {
    ensure_line(E.row);
    save_undo();
    E.lines[E.row][E.col] = '\0';
    E.dirty = 1;
}

static void editor_delete_word_back(void) {
    ensure_line(E.row);
    if (E.col == 0) { editor_backspace(); return; }
    save_undo();
    char *line = E.lines[E.row];
    int c = E.col;
    while (c > 0 && (line[c-1] == ' ' || line[c-1] == '\t')) c--;
    while (c > 0 && line[c-1] != ' ' && line[c-1] != '\t')  c--;
    memmove(&line[c], &line[E.col], strlen(line) - (size_t)E.col + 1);
    E.col   = c;
    E.dirty = 1;
}

static void editor_word_forward(void) {
    ensure_line(E.row);
    char *line = E.lines[E.row];
    int   len  = (int)strlen(line);
    while (E.col < len && !isspace((unsigned char)line[E.col])) E.col++;
    while (E.col < len &&  isspace((unsigned char)line[E.col])) E.col++;
}

static void editor_word_back(void) {
    ensure_line(E.row);
    char *line = E.lines[E.row];
    if (E.col > 0) E.col--;
    while (E.col > 0 &&  isspace((unsigned char)line[E.col])) E.col--;
    while (E.col > 0 && !isspace((unsigned char)line[E.col-1])) E.col--;
}

static void editor_search_next(void) {
    if (E.search[0] == '\0') { set_status("No search pattern."); return; }
    int r = E.row, c = E.col + 1;
    for (int pass = 0; pass < E.num_lines; pass++) {
        int ri = (r + pass) % E.num_lines;
        char *line  = E.lines[ri];
        int   start = (pass == 0) ? c : 0;
        char *found = strstr(line + start, E.search);
        if (found) {
            E.row = ri;
            E.col = (int)(found - line);
            set_status("/%s", E.search);
            return;
        }
    }
    set_status("Pattern not found: %s", E.search);
}

/* ── Colors ─────────────────────────────────────────────────────────────── */

static void setup_colors(void) {
    start_color();
    use_default_colors();

    init_pair(CLR_NORMAL,  -1,            -1);
    init_pair(CLR_GUTTER,  COLOR_BLUE,    -1);
    init_pair(CLR_CURLINE, COLOR_WHITE,   COLOR_BLACK);
    init_pair(CLR_STATUSN, COLOR_BLACK,   COLOR_GREEN);
    init_pair(CLR_STATUSI, COLOR_BLACK,   COLOR_BLUE);
    init_pair(CLR_STATUSC, COLOR_BLACK,   COLOR_CYAN);
    init_pair(CLR_SEARCH,  COLOR_BLACK,   COLOR_YELLOW);
    init_pair(CLR_WELCOME, COLOR_CYAN,    -1);
    init_pair(CLR_TILDE,   COLOR_BLUE,    -1);

    if (COLORS >= 256) {
        /* Refined 256-color palette */
        init_pair(CLR_GUTTER,  242, -1);
        init_pair(CLR_CURLINE, 255, 235);
        init_pair(CLR_STATUSN, 232, 35);    /* deep-green background */
        init_pair(CLR_STATUSI, 255, 27);    /* bright blue */
        init_pair(CLR_STATUSC, 232, 51);    /* cyan */
        init_pair(CLR_TILDE,   240, -1);
    }
}

/* ── Rendering ────────────────────────────────────────────────────────────── */

static void render_status_bar(void) {
    int pair = (E.mode == MODE_NORMAL)  ? CLR_STATUSN :
               (E.mode == MODE_INSERT)  ? CLR_STATUSI : CLR_STATUSC;
    const char *modestr = (E.mode == MODE_NORMAL)  ? " NORMAL " :
                          (E.mode == MODE_INSERT)  ? " INSERT " :
                                                     "COMMAND ";

    char left[256], right[256];
    snprintf(left,  sizeof(left),  "%s│ %s%s",
             modestr,
             E.filename[0] ? E.filename : "[No Name]",
             E.dirty ? " [+]" : "");
    snprintf(right, sizeof(right), "Ln %d, Col %d │ %d lines ",
             E.row + 1, E.col + 1, E.num_lines);

    move(LINES - 2, 0);
    clrtoeol();
    attron(COLOR_PAIR(pair) | A_BOLD);
    printw("%s", left);
    int pad = COLS - (int)strlen(left) - (int)strlen(right);
    for (int i = 0; i < pad; i++) addch(' ');
    printw("%s", right);
    attroff(COLOR_PAIR(pair) | A_BOLD);
}

static void render_command_bar(void) {
    move(LINES - 1, 0);
    clrtoeol();
    if (E.mode == MODE_COMMAND) {
        attron(A_BOLD);
        printw("%c%s", E.cmd_buf[0] == '/' ? '/' : ':', E.cmd_buf[0] == '/' ? E.cmd_buf + 1 : E.cmd_buf);
        attroff(A_BOLD);
    } else if (E.status_msg[0]) {
        attron(COLOR_PAIR(CLR_GUTTER));
        printw("%s", E.status_msg);
        attroff(COLOR_PAIR(CLR_GUTTER));
    }
}

static void render(void) {
    int text_rows = LINES - 2;
    int text_cols = COLS  - GUTTER_WIDTH;

    /* Scroll viewport */
    if (E.row < E.scroll_row) E.scroll_row = E.row;
    if (E.row >= E.scroll_row + text_rows) E.scroll_row = E.row - text_rows + 1;

    for (int y = 0; y < text_rows; y++) {
        int file_row = E.scroll_row + y;
        move(y, 0);
        clrtoeol();

        /* Gutter */
        if (file_row < E.num_lines) {
            if (file_row == E.row) {
                attron(COLOR_PAIR(CLR_CURLINE) | A_BOLD);
                printw("%4d ", file_row + 1);
                attroff(COLOR_PAIR(CLR_CURLINE) | A_BOLD);
            } else {
                attron(COLOR_PAIR(CLR_GUTTER));
                printw("%4d ", file_row + 1);
                attroff(COLOR_PAIR(CLR_GUTTER));
            }
        } else {
            attron(COLOR_PAIR(CLR_TILDE));
            printw("   ~ ");
            attroff(COLOR_PAIR(CLR_TILDE));
            continue;
        }

        /* Line text */
        char *line = E.lines[file_row];
        int   len  = (int)strlen(line);
        int   is_cur = (file_row == E.row);

        /* Locate search hit for this line */
        int sh_start = -1, sh_end = -1;
        if (E.search[0]) {
            char *found = strstr(line, E.search);
            if (found) {
                sh_start = (int)(found - line);
                sh_end   = sh_start + (int)strlen(E.search);
            }
        }

        if (is_cur) attron(COLOR_PAIR(CLR_CURLINE));

        for (int x = 0; x < text_cols; x++) {
            if (x >= len) { addch(' '); break; }

            /* Search highlight toggle */
            if (x == sh_start) {
                if (is_cur) attroff(COLOR_PAIR(CLR_CURLINE));
                attron(COLOR_PAIR(CLR_SEARCH) | A_BOLD);
            } else if (x == sh_end) {
                attroff(COLOR_PAIR(CLR_SEARCH) | A_BOLD);
                if (is_cur) attron(COLOR_PAIR(CLR_CURLINE));
            }
            addch((unsigned char)line[x]);
        }

        if (sh_end > 0 && len < text_cols) {
            attroff(COLOR_PAIR(CLR_SEARCH) | A_BOLD);
            if (is_cur) attron(COLOR_PAIR(CLR_CURLINE));
        }
        if (is_cur) attroff(COLOR_PAIR(CLR_CURLINE));
    }

    render_status_bar();
    render_command_bar();

    /* Place terminal cursor */
    int cy = E.row - E.scroll_row;
    int cx = GUTTER_WIDTH + E.col;
    if (E.mode == MODE_COMMAND) {
        /* Cursor sits in the command bar */
        move(LINES - 1, (E.cmd_buf[0] == '/') ? 1 + E.cmd_len : 1 + E.cmd_len);
    } else {
        move(cy, cx);
    }
    refresh();
}

/* ── Welcome Screen ──────────────────────────────────────────────────────── */

static void show_welcome(void) {
    clear();
    int cy = LINES / 2 - 4;
    if (cy < 0) cy = 0;
    int cx = (COLS - 44) / 2;
    if (cx < 0) cx = 0;

    attron(COLOR_PAIR(CLR_WELCOME) | A_BOLD);
    mvprintw(cy + 0, cx, " __   __         _ _   _____    _ _ _   ");
    mvprintw(cy + 1, cx, " \\ \\ / /__ _   _| | |_| ____|__| (_) |_ ");
    mvprintw(cy + 2, cx, "  \\ V / _` | | | | __|  _| / _` | | __|");
    mvprintw(cy + 3, cx, "   | | (_| | |_| | |_| |__| (_| | | |_ ");
    mvprintw(cy + 4, cx, "   |_|\\__,_|\\__,_|\\__|_____\\__,_|_|\\__|");
    attroff(COLOR_PAIR(CLR_WELCOME) | A_BOLD);

    attron(A_BOLD);
    int vcx = (COLS - 30) / 2;
    mvprintw(cy + 6, vcx, "   Blyn %s — Where Code Lives", VERSION);
    attroff(A_BOLD);

    attron(COLOR_PAIR(CLR_GUTTER));
    int hcx = (COLS - 52) / 2;
    mvprintw(cy + 8,  hcx, "  hjkl / arrows  navigate         i  insert mode ");
    mvprintw(cy + 9,  hcx, "  ESC            normal mode       :w  save       ");
    mvprintw(cy + 10, hcx, "  :q  quit       :wq  save & quit  /   search     ");
    mvprintw(cy + 11, hcx, "  dd  delete line   o  open below   u  undo       ");
    mvprintw(cy + 13, (COLS - 28) / 2, "Press any key to begin...");
    attroff(COLOR_PAIR(CLR_GUTTER));

    refresh();
}

/* ── Command execution ────────────────────────────────────────────────────── */

static void run_command(void) {
    /* Copy cmd_buf before clearing, to avoid alias invalidation */
    char cmd[256];
    strncpy(cmd, E.cmd_buf, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    E.mode = MODE_NORMAL;
    E.cmd_buf[0] = '\0'; E.cmd_len = 0;

    /* Detect search commands (start with '/') */
    if (cmd[0] == '/') {
        strncpy(E.search, cmd + 1, sizeof(E.search) - 1);
        editor_search_next();
        return;
    }

    if (strcmp(cmd, "w") == 0) {
        editor_save();
    } else if (strcmp(cmd, "q") == 0) {
        if (E.dirty) set_status("Unsaved changes! Use  :q!  to force quit.");
        else         { editor_free(); endwin(); exit(0); }
    } else if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0) {
        editor_save(); editor_free(); endwin(); exit(0);
    } else if (strcmp(cmd, "q!") == 0) {
        editor_free(); endwin(); exit(0);
    } else if (cmd[0] >= '0' && cmd[0] <= '9') {
        int target = atoi(cmd) - 1;
        if (target < 0) target = 0;
        if (target >= E.num_lines) target = E.num_lines - 1;
        E.row = target; clamp_col();
        set_status("Line %d", target + 1);
    } else if (strncmp(cmd, "w ", 2) == 0) {
        strncpy(E.filename, cmd + 2, sizeof(E.filename) - 1);
        editor_save();
    } else if (cmd[0] != '\0') {
        set_status("Unknown command: :%s", cmd);
    }
}

/* ── Input handlers ─────────────────────────────────────────────────────── */

static void handle_normal(int ch) {
    int len;

    /* Reset double-key buffers on unrelated key */
    if (ch != 'g') E.g_pressed = 0;
    if (ch != 'd') E.d_pressed = 0;

    switch (ch) {
        /* ── Movement ── */
        case 'h': case KEY_LEFT:
            if (E.col > 0) E.col--;
            break;
        case 'l': case KEY_RIGHT:
            len = (int)strlen(E.lines[E.row]);
            if (E.col < (len > 0 ? len - 1 : 0)) E.col++;
            break;
        case 'k': case KEY_UP:
            if (E.row > 0) { E.row--; clamp_col(); }
            break;
        case 'j': case KEY_DOWN:
            if (E.row < E.num_lines - 1) { E.row++; clamp_col(); }
            break;
        case '0':
            E.col = 0;
            break;
        case '$':
            len = (int)strlen(E.lines[E.row]);
            E.col = len > 0 ? len - 1 : 0;
            break;
        case 'G':
            E.row = E.num_lines - 1; clamp_col();
            break;
        case 'g':
            if (E.g_pressed) { E.row = 0; E.col = 0; E.g_pressed = 0; }
            else              { E.g_pressed = 1; }
            return; /* skip resetting g_pressed below */
        case 'w': editor_word_forward(); break;
        case 'b': editor_word_back();    break;
        case 4:   /* Ctrl+D — half-page down */
            E.row += (LINES - 2) / 2;
            if (E.row >= E.num_lines) E.row = E.num_lines - 1;
            clamp_col();
            break;
        case 21:  /* Ctrl+U — half-page up */
            E.row -= (LINES - 2) / 2;
            if (E.row < 0) E.row = 0;
            clamp_col();
            break;

        /* ── Mode switches ── */
        case 'i':
            E.mode = MODE_INSERT; curs_set(2);
            set_status("");
            break;
        case 'I':
            E.col = 0; E.mode = MODE_INSERT; curs_set(2);
            break;
        case 'a':
            len = (int)strlen(E.lines[E.row]);
            if (E.col < len) E.col++;
            E.mode = MODE_INSERT; curs_set(2);
            break;
        case 'A':
            E.col = (int)strlen(E.lines[E.row]);
            E.mode = MODE_INSERT; curs_set(2);
            break;
        case 'o': editor_open_line_below(); curs_set(2); break;
        case 'O': editor_open_line_above(); curs_set(2); break;
        case ':':
            E.mode = MODE_COMMAND;
            E.cmd_buf[0] = '\0'; E.cmd_len = 0;
            break;
        case '/':
            E.mode = MODE_COMMAND;
            E.cmd_buf[0] = '/'; E.cmd_buf[1] = '\0'; E.cmd_len = 1;
            break;

        /* ── Editing in normal mode ── */
        case 'x': editor_delete_char_at(); break;
        case 'd':
            if (E.d_pressed) { editor_delete_line(); E.d_pressed = 0; }
            else              { E.d_pressed = 1; }
            return;
        case 'u': editor_undo(); break;
        case 'n': editor_search_next(); break;

        /* ── Save shortcut ── */
        case 19: /* Ctrl+S */
            editor_save();
            break;
    }
    E.g_pressed = 0;
    E.d_pressed = 0;
}

static void handle_insert(int ch) {
    switch (ch) {
        case 27: /* ESC → NORMAL */
            E.mode = MODE_NORMAL; curs_set(1);
            if (E.col > 0) E.col--;
            clamp_col();
            break;
        case '\n': case KEY_ENTER:
            editor_insert_newline();
            break;
        case KEY_BACKSPACE: case 127:
            editor_backspace();
            break;
        case KEY_DC:
            editor_delete_char_at();
            break;
        case KEY_LEFT:  if (E.col > 0) E.col--; break;
        case KEY_RIGHT: { int l=(int)strlen(E.lines[E.row]); if(E.col<l) E.col++; } break;
        case KEY_UP:    if (E.row > 0) { E.row--; clamp_col(); } break;
        case KEY_DOWN:  if (E.row < E.num_lines-1) { E.row++; clamp_col(); } break;
        case 1:  /* Ctrl+A */ E.col = 0; break;
        case 5:  /* Ctrl+E */ E.col = (int)strlen(E.lines[E.row]); break;
        case 11: /* Ctrl+K */ editor_kill_to_eol(); break;
        case 23: /* Ctrl+W */ editor_delete_word_back(); break;
        case 19: /* Ctrl+S */ editor_save(); break;
        default:
            if (ch >= 32 && ch < 127) editor_insert_char(ch);
            break;
    }
}

static void handle_command(int ch) {
    switch (ch) {
        case 27: /* ESC — cancel */
            E.mode = MODE_NORMAL;
            E.cmd_buf[0] = '\0'; E.cmd_len = 0;
            break;
        case '\n': case KEY_ENTER:
            run_command();
            break;
        case KEY_BACKSPACE: case 127:
            if (E.cmd_len > 0) {
                E.cmd_buf[--E.cmd_len] = '\0';
                /* If buf is now empty (or just the '/' prefix gone), cancel */
                if (E.cmd_len == 0 || (E.cmd_buf[0]=='/' && E.cmd_len==1-1))
                    E.mode = MODE_NORMAL;
            } else {
                E.mode = MODE_NORMAL;
            }
            break;
        default:
            if (ch >= 32 && ch < 127 && E.cmd_len < (int)sizeof(E.cmd_buf) - 2) {
                E.cmd_buf[E.cmd_len++] = (char)ch;
                E.cmd_buf[E.cmd_len]   = '\0';
            }
            break;
    }
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    editor_init();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);   /* snappy ESC response */
    curs_set(1);

    if (has_colors()) setup_colors();
    bkgd(COLOR_PAIR(CLR_NORMAL));

    if (argc > 1) {
        editor_open(argv[1]);
        clear(); refresh();
    } else {
        show_welcome();
        getch();
        clear(); refresh();
    }

    /* Main event loop */
    while (1) {
        render();
        int ch = getch();

        if (ch == KEY_RESIZE) {
            clear(); refresh();
            continue;
        }

        switch (E.mode) {
            case MODE_NORMAL:  handle_normal(ch);  break;
            case MODE_INSERT:  handle_insert(ch);  break;
            case MODE_COMMAND: handle_command(ch); break;
        }
    }

    editor_free();
    endwin();
    return 0;
}

/*
 * ── Blyn 1.0 Cheat Sheet ────────────────────────────────────────────────
 *
 * NORMAL mode (default on start)
 *   h j k l     ←↓↑→   (also arrow keys)
 *   0 $          line start / end
 *   w b          word forward / back
 *   gg G         top / bottom of file
 *   Ctrl+D/U     half-page down / up
 *   i I          insert at cursor / line start
 *   a A          append after cursor / line end
 *   o O          open line below / above (and insert)
 *   x            delete char under cursor
 *   dd           delete current line
 *   u            undo (last line change)
 *   /pattern     search forward    n  next match
 *   :w           save              :q   quit
 *   :wq  :x      save & quit       :q!  force quit
 *   :N           go to line N      Ctrl+S  save
 *
 * INSERT mode  (i / a / o to enter)
 *   ESC          back to NORMAL
 *   Ctrl+A       start of line
 *   Ctrl+E       end of line
 *   Ctrl+K       kill to end of line
 *   Ctrl+W       delete previous word
 *   Ctrl+S       save
 * ────────────────────────────────────────────────────────────────────────
 */
