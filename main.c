#include "game.h"
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

/*
 * Layout (fits a standard 80x24 terminal):
 *   row 0              title
 *   rows 1..22         board frame: 22 wide x 22 tall.  The 10x20
 *                      playfield is drawn with 2-char-wide blocks ("[]")
 *   row 23             message line (pause / game over), under the board
 *   panel at x = 25    NEXT + previews, SCORE/LINES/LEVEL, controls
 *
 * Redraw strategy: no clear() per frame - ncurses diffs every cell
 * against the physical screen and only transmits what changed.  Areas
 * whose content can shrink (previews, stat values, message line) are
 * explicitly blanked before being rewritten, so no stale cells remain.
 */
#define MIN_TERMINAL_WIDTH 62
#define MIN_TERMINAL_HEIGHT 24

#define BOARD_X 1
#define FRAME_TOP 1
#define FRAME_W (2 * BOARD_WIDTH + 2)        /* 22 */
#define FRAME_H (BOARD_HEIGHT + 2)           /* 22 */
#define FRAME_BOTTOM (FRAME_TOP + FRAME_H - 1) /* 22 */
#define MESSAGE_ROW (FRAME_BOTTOM + 1)       /* 23 */

/* Side panel: NEXT box (x 25..36, rows 1..16), score box (x 39..54,
 * rows 1..5), controls below the NEXT box. */
#define NEXT_X 25
#define NEXT_W 12
#define NEXT_BOTTOM (FRAME_TOP + 15)         /* row 16 */
#define SCORE_X 39
#define SCORE_W 16
#define SCORE_BOTTOM (FRAME_TOP + 4)         /* row 5 */

/* Color pair ids */
#define PAIR_TITLE 1
#define PAIR_BOARD 2
#define PAIR_PANEL 3
#define PAIR_MESSAGE 4
#define PAIR_GAMEOVER 5
#define PAIR_PIECE_FIRST 6 /* one pair per piece type, in enum order */

static game_t g_game;
static volatile sig_atomic_t g_signal_exit;

static void signal_handler(int sig)
{
    (void)sig;
    g_signal_exit = 1;
}

static void init_color_pairs(void)
{
    init_pair(PAIR_TITLE, COLOR_WHITE, COLOR_BLACK);
    init_pair(PAIR_BOARD, COLOR_WHITE, COLOR_BLACK);
    init_pair(PAIR_PANEL, COLOR_CYAN, COLOR_BLACK);
    init_pair(PAIR_MESSAGE, COLOR_YELLOW, COLOR_BLACK);
    init_pair(PAIR_GAMEOVER, COLOR_RED, COLOR_BLACK);
    for (int t = 0; t < TETROMINO_COUNT; t++) {
        init_pair(PAIR_PIECE_FIRST + t, get_color((tetromino_type)t),
                  COLOR_BLACK);
    }
}

static int piece_pair(tetromino_type type)
{
    return PAIR_PIECE_FIRST + type;
}

/* Draw one 2-char-wide block ("[]") of a piece. */
static void draw_block(int y, int x, tetromino_type type)
{
    attron(COLOR_PAIR(piece_pair(type)));
    mvaddch(y, x, '[');
    mvaddch(y, x + 1, ']');
    attroff(COLOR_PAIR(piece_pair(type)));
}

static void render_title(void)
{
    attron(COLOR_PAIR(PAIR_TITLE));
    mvaddstr(0, 1, " C U R S E D   T E T R I S ");
    attroff(COLOR_PAIR(PAIR_TITLE));
}

static void render_board(const game_t* game)
{
    /* Frame (constant every frame; ncurses skips unchanged cells) */
    attron(COLOR_PAIR(PAIR_BOARD));
    for (int x = 0; x < FRAME_W; x++) {
        char ch = (x == 0 || x == FRAME_W - 1) ? '+' : '-';
        mvaddch(FRAME_TOP, BOARD_X + x, ch);
        mvaddch(FRAME_BOTTOM, BOARD_X + x, ch);
    }
    for (int y = 1; y < FRAME_H - 1; y++) { /* walls: rows between the edges */
        mvaddch(FRAME_TOP + y, BOARD_X, '|');
        mvaddch(FRAME_TOP + y, BOARD_X + FRAME_W - 1, '|');
    }
    attroff(COLOR_PAIR(PAIR_BOARD));

    /* Locked cells (all 200 are written every frame so that
     * cells that become empty are explicitly blanked) */
    for (int y = 0; y < BOARD_HEIGHT; y++) {
        for (int x = 0; x < BOARD_WIDTH; x++) {
            int sy = FRAME_TOP + 1 + y;
            int sx = BOARD_X + 1 + 2 * x;
            int cell = game->board.grid[x][y];
            if (cell == 0) {
                mvaddch(sy, sx, ' ');
                mvaddch(sy, sx + 1, ' ');
            } else {
                draw_block(sy, sx, (tetromino_type)(cell - 1));
            }
        }
    }

    /* Active piece */
    if (game->state != STATE_GAMEOVER) {
        const piece_t* p = &game->current_piece;
        const int (*shape)[PIECE_BOX] = shape_of(p->type, p->rotation);
        for (int r = 0; r < PIECE_BOX; r++) {
            int y = p->y + r;
            if (y < 0 || y >= BOARD_HEIGHT)
                continue;
            for (int c = 0; c < PIECE_BOX; c++) {
                if (shape[r][c])
                    draw_block(FRAME_TOP + 1 + y,
                               BOARD_X + 1 + 2 * (p->x + c), p->type);
            }
        }
    }
}

static void render_next_box(const game_t* game)
{
    int top = FRAME_TOP;
    attron(COLOR_PAIR(PAIR_PANEL));
    mvaddnstr(top, NEXT_X, "+-- NEXT --+", -1);
    mvaddnstr(NEXT_BOTTOM, NEXT_X, "+----------+", -1);
    for (int y = top + 1; y < NEXT_BOTTOM; y++) {
        mvaddch(y, NEXT_X, '|');
        mvaddch(y, NEXT_X + NEXT_W - 1, '|');
    }
    /* blank the interior (old preview shapes may be wider elsewhere) */
    for (int y = top + 1; y < NEXT_BOTTOM; y++)
        mvaddnstr(y, NEXT_X + 1, "          ", 10);
    attroff(COLOR_PAIR(PAIR_PANEL));

    for (int i = 0; i < NEXT_PREVIEW_COUNT; i++) {
        const piece_t* p = &game->next_pieces.pieces[i];
        const int (*shape)[PIECE_BOX] = shape_of(p->type, p->rotation);
        int base_y = top + 1 + i * (PIECE_BOX + 1); /* 2, 7, 12 */
        for (int r = 0; r < PIECE_BOX; r++) {
            for (int c = 0; c < PIECE_BOX; c++) {
                if (shape[r][c])
                    draw_block(base_y + r, NEXT_X + 2 + 2 * c, p->type);
            }
        }
    }
}

static void render_score_box(const game_t* game)
{
    int top = FRAME_TOP;
    attron(COLOR_PAIR(PAIR_PANEL));
    mvaddnstr(top, SCORE_X, "+--------------+", -1);
    mvaddnstr(SCORE_BOTTOM, SCORE_X, "+--------------+", -1);
    for (int y = top + 1; y < SCORE_BOTTOM; y++) {
        mvaddch(y, SCORE_X, '|');
        mvaddch(y, SCORE_X + SCORE_W - 1, '|');
        mvaddnstr(y, SCORE_X + 1, "              ", 14); /* blank interior */
    }
    attroff(COLOR_PAIR(PAIR_PANEL));

    mvaddnstr(top + 1, SCORE_X + 1, "SCORE", 5);
    mvprintw(top + 1, SCORE_X + 7, "%7d", game_get_score(game));
    mvaddnstr(top + 2, SCORE_X + 1, "LINES", 5);
    mvprintw(top + 2, SCORE_X + 7, "%7d", game_get_lines(game));
    mvaddnstr(top + 3, SCORE_X + 1, "LEVEL", 5);
    mvprintw(top + 3, SCORE_X + 7, "%7d", game_get_level(game));
}

static void render_controls(void)
{
    mvaddnstr(FRAME_TOP + 17, NEXT_X, "h/l: move  j/k: rotate  m: drop", -1);
    mvaddnstr(FRAME_TOP + 18, NEXT_X, "space: hard drop  p: pause  q: quit", -1);
}

static void render_message(const game_t* game)
{
    /* Blank the whole message line every frame, then show the message
     * when the game is paused or over. */
    for (int x = 0; x < FRAME_W; x++)
        mvaddch(MESSAGE_ROW, BOARD_X + x, ' ');

    if (game->state == STATE_PLAYING || game->message[0] == '\0')
        return;

    int pair = (game->state == STATE_GAMEOVER) ? PAIR_GAMEOVER : PAIR_MESSAGE;
    int len = (int)strlen(game->message);
    if (len > FRAME_W - 2)
        len = FRAME_W - 2;
    int x = BOARD_X + (FRAME_W - len) / 2;

    attron(COLOR_PAIR(pair));
    mvaddnstr(MESSAGE_ROW, x, game->message, len);
    attroff(COLOR_PAIR(pair));
}

static void render(const game_t* game)
{
    /* No clear(): ncurses only transmits cells that changed. */
    render_title();
    render_board(game);
    render_next_box(game);
    render_score_box(game);
    render_controls();
    render_message(game);
    refresh();
}

int main(void)
{
    game_init(&g_game, "Cursed Tetris", "Player");

    if (initscr() == NULL) {
        fprintf(stderr, "Error: could not initialize ncurses "
                        "(is TERM set? is it a terminal?)\n");
        return 1;
    }
    if (has_colors()) {
        start_color();
        init_color_pairs();
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    /* SIGWINCH is a non-POSIX extension (hidden on macOS in strict
     * POSIX mode); its default action is ignore anyway, and the
     * terminal size is only checked once at startup. */
#ifdef SIGWINCH
    signal(SIGWINCH, SIG_IGN);
#endif

    int height, width;
    getmaxyx(stdscr, height, width);
    if (height < MIN_TERMINAL_HEIGHT || width < MIN_TERMINAL_WIDTH) {
        endwin();
        fprintf(stderr, "Terminal too small: need %dx%d, have %dx%d\n",
                MIN_TERMINAL_WIDTH, MIN_TERMINAL_HEIGHT, width, height);
        return 1;
    }

    game_start(&g_game);

    while (g_game.running && !g_signal_exit) {
        render(&g_game);
        napms(16); /* ~60 FPS; gravity is timestamp-based */
        game_handle_input(&g_game, input_wait());
        game_update(&g_game);
    }

    /* Show the final frame (game over screen) briefly before exiting. */
    render(&g_game);
    napms(500);

    endwin();
    return 0;
}
