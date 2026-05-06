// go_viewer.c - A minimal Go (Baduk) game viewer using SDL2 for graphical display
// Displays games as an animated playback with per-move delays
// Dependencies: SDL2 (stones are rendered as pixel-perfect circles)
// Compile (Linux/Mac): gcc go_viewer.c -o go_viewer -lSDL2 -lm
// Windows: Use a setup like MinGW or Visual Studio with SDL2 libs

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <strings.h>
#endif
#include <SDL2/SDL.h>

#define BOARD_SIZE 19
#define SCREEN_SIZE 800
#define DEFAULT_GAMES_DIR "games/"
#define MAX_MOVES 1000
#define MOVE_TEXT_LEN 8
#define MOVE_DELAY_MS 2000
#define MOVE_DELAY_MIN_MS 200
#define MOVE_DELAY_MAX_MS 10000
#define MOVE_DELAY_STEP_MS 200
#define MOVE_ANIM_MS 300
#define NAME_LEN 128
#define RESULT_LEN 16
#define GAME_OVER_PAUSE_MS 20000

#define SPEED_MESSAGE_MS 1500
#define CURSOR_IDLE_MS 2500
#define GAME_NAV_PREV -1
#define GAME_NAV_NONE 0
#define GAME_NAV_NEXT 1
#define GAME_NAV_RESTART 2
#define GAME_NAV_SELECT 3

#ifdef _WIN32
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#else
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

typedef struct {
    char *name;
    int type;
} CatalogEntry;

char board[BOARD_SIZE][BOARD_SIZE];
SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
SDL_Texture *stone_textures[2] = {NULL}; // 0 = black, 1 = white (unused now)
SDL_Cursor *analysis_cursor = NULL;
char current_black_name[NAME_LEN] = "Black";
char current_white_name[NAME_LEN] = "White";
const char *games_dir_root = DEFAULT_GAMES_DIR;
int move_delay_ms = MOVE_DELAY_MS;
Uint32 speed_message_until = 0;
int analysis_mode = 0;
int show_help = 0;
int guess_mode = 0;
int guess_score = 0;
int chain_mode = 0;
int turn_is_black = 1;
int analysis_turn_is_black = 1;
int game_nav_request = GAME_NAV_NONE;
int game_finished = 0;
Uint32 game_finished_timer = 0;
int paused = 0;
int catalog_active = 0;
int catalog_selection_made = 0;
CatalogEntry *catalog_entries = NULL;
int catalog_entry_count = 0;
int catalog_index = 0;
int catalog_scroll = 0;
char *forced_sgf_path = NULL;
char catalog_base_dir[1024] = "";
char catalog_dir[1024] = "";
char sequential_dir[1024] = "";
int sequential_index = 0;
int suppress_present = 0;
int cursor_visible = 1;
Uint32 last_mouse_activity = 0;
char result_message[64] = "";

typedef struct {
    int square;
    int offset_x;
    int offset_y;
    int screen_w;
    int screen_h;
    int board_px;
} BoardView;

typedef struct {
    int active;
    int is_black;
    float x;
    float y;
    int skip_r1, skip_f1;
} Overlay;

typedef struct {
    char c;
    unsigned char rows[7];
} Glyph;

typedef struct {
    int r, f;
    int is_black;
} Stone;

typedef struct {
    char board[BOARD_SIZE][BOARD_SIZE];
    Stone stones[MAX_MOVES];
    int stone_count;
    int black_prisoners;
    int white_prisoners;
} BoardState;

Stone stones[MAX_MOVES];
int stone_count = 0;
Stone analysis_stones[MAX_MOVES];
int analysis_stone_count = 0;

BoardState board_states[MAX_MOVES];
int board_state_count = 0;

int black_prisoners = 0;
int white_prisoners = 0;
int analysis_saved_black_prisoners = 0;
int analysis_saved_white_prisoners = 0;

int liberty_r[BOARD_SIZE * BOARD_SIZE];
int liberty_f[BOARD_SIZE * BOARD_SIZE];
int liberty_count = 0;
int liberty_display_r = -1;
int liberty_display_f = -1;
int selected_group_stones[BOARD_SIZE * BOARD_SIZE][2]; // r,f pairs
int selected_group_count = 0;

// Forward declarations
void calculate_chain_liberties(int r, int f);
void store_selected_group(int r, int f);
int is_stone_in_selected_group(int r, int f);

int adjust_move_delay(int delta_ms, Uint32 now);
void board_to_screen(const BoardView *view, int board_r, int board_f, int *out_x, int *out_y);
int parse_sgf_move(const char *move_str, int *out_r, int *out_f);
int load_sgf_game(const char *path, char moves[][MOVE_TEXT_LEN], int max_moves,
                  char *black_name, size_t black_name_size,
                  char *white_name, size_t white_name_size,
                  char *result, size_t result_size);
void render_board(const BoardView *view, const Overlay *overlay);
void render_chain_connections(const BoardView *view);
void render_liberties(const BoardView *view) {
    if (selected_group_count == 0) return;

    // Recalculate liberties for the selected group
    liberty_count = 0;
    if (selected_group_count > 0) {
        // Use the first stone in the group to calculate liberties
        int r = selected_group_stones[0][0];
        int f = selected_group_stones[0][1];
        calculate_chain_liberties(r, f);
    }

    if (liberty_count == 0) return;

    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red color for liberty dots

    for (int i = 0; i < liberty_count; i++) {
        int r = liberty_r[i];
        int f = liberty_f[i];
        int x = view->offset_x + f * view->square + view->square / 2;
        int y = view->offset_y + r * view->square + view->square / 2;
        int radius = view->square / 8; // Small dot radius
        if (radius < 2) radius = 2;

        // Draw filled circle for liberty dot
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                if (dx*dx + dy*dy <= radius*radius) {
                    SDL_RenderDrawPoint(renderer, x + dx, y + dy);
                }
            }
        }
    }
}
void draw_board();
int animate_move(int r, int f, int is_black);
int play_game(char moves[][MOVE_TEXT_LEN], int move_count, const char *result);
void render_speed_label(const BoardView *view);
void render_help_overlay(const BoardView *view);
void render_guess_score(const BoardView *view);
void render_result_message(const BoardView *view);
void render_mode_status(const BoardView *view);

// Forward declarations for text functions
int text_width_px(const char *text, int scale);
void draw_text(int x, int y, int scale, const char *text, SDL_Color color);

// Convert SGF result format to human-readable text
const char* format_result_message(const char *sgf_result) {
    if (!sgf_result || sgf_result[0] == '\0') return "";

    static char formatted[64];

    if (strcmp(sgf_result, "B+R") == 0) {
        strcpy(formatted, "Black Resigns");
    } else if (strcmp(sgf_result, "W+R") == 0) {
        strcpy(formatted, "White Resigns");
    } else if (strcmp(sgf_result, "B+Resign") == 0) {
        strcpy(formatted, "Black Resigns");
    } else if (strcmp(sgf_result, "W+Resign") == 0) {
        strcpy(formatted, "White Resigns");
    } else if (strcmp(sgf_result, "B+T") == 0) {
        strcpy(formatted, "Black Wins by Time");
    } else if (strcmp(sgf_result, "W+T") == 0) {
        strcpy(formatted, "White Wins by Time");
    } else if (strcmp(sgf_result, "1/2-1/2") == 0) {
        strcpy(formatted, "Draw");
    } else if (strcmp(sgf_result, "Jigo") == 0) {
        strcpy(formatted, "Draw");
    } else if (sgf_result[0] == 'B' && sgf_result[1] == '+') {
        // Black wins by points, e.g., "B+5", "B+12.5"
        snprintf(formatted, sizeof(formatted), "Black Wins by %s", sgf_result + 2);
    } else if (sgf_result[0] == 'W' && sgf_result[1] == '+') {
        // White wins by points, e.g., "W+5", "W+12.5"
        snprintf(formatted, sizeof(formatted), "White Wins by %s", sgf_result + 2);
    } else if (strcmp(sgf_result, "Void") == 0) {
        strcpy(formatted, "Game Void");
    } else if (strcmp(sgf_result, "Unfinished") == 0) {
        strcpy(formatted, "Unfinished");
    } else {
        // Fallback to original format if unrecognized
        strncpy(formatted, sgf_result, sizeof(formatted) - 1);
        formatted[sizeof(formatted) - 1] = '\0';
    }

    return formatted;
}

void render_mode_status(const BoardView *view) {
    const char *mode_text = NULL;

    if (analysis_mode) {
        mode_text = "ANALYSIS MODE";
    } else if (guess_mode) {
        mode_text = "GUESS MODE";
    } else if (paused) {
        mode_text = "PAUSED";
    }

    if (!mode_text) return;

    int scale = (view->square >= 30) ? 3 : 2;
    int margin = (view->square >= 30) ? 16 : 8;
    int text_w = text_width_px(mode_text, scale);
    int text_h = 7 * scale;

    // Position to the left of the board, outside the board area
    int x = view->offset_x - margin - text_w;
    int y = view->offset_y + margin; // Top margin

    // Text color - yellow like result message
    SDL_Color text_color = {255, 255, 180, 255};
    draw_text(x, y, scale, mode_text, text_color);
}

void render_result_message(const BoardView *view) {
    if (!game_finished || result_message[0] == '\0') return;

    const char *display_text = format_result_message(result_message);

    int scale = (view->square >= 30) ? 3 : 2;
    int margin = (view->square >= 30) ? 16 : 8;
    int text_w = text_width_px(display_text, scale);
    int text_h = 7 * scale;
    int pad = (scale >= 3) ? 4 : 3;

    // Position to the right of the board, below mode status if present
    int x = view->offset_x + view->board_px + margin;
    int y = view->offset_y + (view->board_px - text_h) / 2; // Vertically centered

    // If mode status is showing, position result message below it
    if (analysis_mode || guess_mode || paused) {
        int mode_scale = (view->square >= 30) ? 3 : 2;
        int mode_text_h = 7 * mode_scale;
        y = view->offset_y + margin + mode_text_h + margin; // Below mode status
    }

    // Text
    SDL_Color text_color = {255, 255, 180, 255};
    draw_text(x, y, scale, display_text, text_color);
}
void render_catalog_overlay(const BoardView *view);
void catalog_free(void);
void catalog_open(const char *games_dir);
void catalog_select(const char *games_dir);
int handle_catalog_event(const SDL_Event *e, const char *games_dir);
int catalog_total_entries(void);
char *copy_string(const char *s);
int has_sgf_extension(const char *name);
void free_string_list(char **items, int count);
char *join_path(const char *dir, const char *name);
int list_sgf_files(const char *dir, char ***out_files);
static int list_sgf_files_recursive(const char *dir, const char *base,
                                    char ***out_files, int *count, int *cap);
SDL_Cursor *create_analysis_cursor(void);
void set_cursor_visible(int visible);
void note_mouse_activity(Uint32 now);
void update_cursor_auto_hide(Uint32 now);
void note_mouse_activity_event(const SDL_Event *e);
void draw_thick_line(int x1, int y1, int x2, int y2, int thickness, SDL_Color color);
void reset_board(void);
void add_stone(int r, int f, int is_black);
void remove_last_stone(void);
int is_surrounded(int r, int f, int color);
int would_be_suicide(int r, int f, int color);
void remove_captured_stones(int skip_color, int skip_r, int skip_f, int placed_stone_color);
void get_group(int r, int f, int color, int visited[BOARD_SIZE][BOARD_SIZE], int *group_count, int group_r[], int group_f[]);
int has_liberties(int r, int f, int color, int visited[BOARD_SIZE][BOARD_SIZE]);
void get_liberties(int r, int f, int color, int visited[BOARD_SIZE][BOARD_SIZE]);
void calculate_chain_liberties(int r, int f) {
    // Reset liberty count
    liberty_count = 0;

    // Get the color of the clicked stone
    int color = (board[r][f] == 1) ? 1 : 0;

    // Find liberties for the chain
    int visited[BOARD_SIZE][BOARD_SIZE] = {0};
    get_liberties(r, f, color, visited);
}

void store_selected_group(int r, int f) {
    // Get the color of the clicked stone
    int color = (board[r][f] == 1) ? 1 : 0;

    // Find the group
    int visited[BOARD_SIZE][BOARD_SIZE] = {0};
    int group_r[BOARD_SIZE * BOARD_SIZE];
    int group_f[BOARD_SIZE * BOARD_SIZE];
    int group_count = 0;
    get_group(r, f, color, visited, &group_count, group_r, group_f);

    // Store the group stones
    selected_group_count = group_count;
    for (int i = 0; i < group_count; i++) {
        selected_group_stones[i][0] = group_r[i];
        selected_group_stones[i][1] = group_f[i];
    }
}

int is_stone_in_selected_group(int r, int f) {
    for (int i = 0; i < selected_group_count; i++) {
        if (selected_group_stones[i][0] == r && selected_group_stones[i][1] == f) {
            return 1;
        }
    }
    return 0;
}

// Go rules implementation for analysis mode
int is_surrounded(int r, int f, int color) {
    int visited[BOARD_SIZE][BOARD_SIZE] = {0};
    return !has_liberties(r, f, color, visited);
}

int would_be_suicide(int r, int f, int color) {
    // Save the current board and stones state
    char saved_board[BOARD_SIZE][BOARD_SIZE];
    memcpy(saved_board, board, sizeof(board));
    Stone saved_stones[MAX_MOVES];
    memcpy(saved_stones, stones, sizeof(stones));
    int saved_stone_count = stone_count;
    Stone saved_analysis_stones[MAX_MOVES];
    memcpy(saved_analysis_stones, analysis_stones, sizeof(analysis_stones));
    int saved_analysis_stone_count = analysis_stone_count;
    int saved_black_prisoners = black_prisoners;
    int saved_white_prisoners = white_prisoners;

    // Temporarily place the stone
    board[r][f] = (char)(color ? 1 : 2);

    // Remove any captured opponent stones (skip stones of the placed stone's color)
    remove_captured_stones(color, -1, -1, color);

    // Now check if our stone group has liberties
    int visited[BOARD_SIZE][BOARD_SIZE] = {0};
    int suicide = !has_liberties(r, f, color, visited);

    // Restore the entire board and stones state
    memcpy(board, saved_board, sizeof(board));
    memcpy(stones, saved_stones, sizeof(stones));
    stone_count = saved_stone_count;
    memcpy(analysis_stones, saved_analysis_stones, sizeof(analysis_stones));
    analysis_stone_count = saved_analysis_stone_count;
    black_prisoners = saved_black_prisoners;
    white_prisoners = saved_white_prisoners;

    return suicide;
}

void remove_captured_stones(int skip_color, int skip_r, int skip_f, int placed_stone_color) {
    // First pass: identify all stones to be captured
    int to_capture[BOARD_SIZE][BOARD_SIZE] = {0};
    int processed[BOARD_SIZE][BOARD_SIZE] = {0};
    int captured_count = 0;

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            if (board[r][f] != 0 && !processed[r][f] && !(r == skip_r && f == skip_f)) {
                int color = (board[r][f] == 1) ? 1 : 0;
                if (color == skip_color) {
                    // Skip stones of this color
                    int visited[BOARD_SIZE][BOARD_SIZE] = {0};
                    int group_r[BOARD_SIZE * BOARD_SIZE];
                    int group_f[BOARD_SIZE * BOARD_SIZE];
                    int group_count = 0;
                    get_group(r, f, color, visited, &group_count, group_r, group_f);
                    for (int i = 0; i < group_count; i++) {
                        processed[group_r[i]][group_f[i]] = 1;
                    }
                    continue;
                }
                if (is_surrounded(r, f, color)) {
                    // Mark the entire captured group for removal
                    int visited[BOARD_SIZE][BOARD_SIZE] = {0};
                    int group_r[BOARD_SIZE * BOARD_SIZE];
                    int group_f[BOARD_SIZE * BOARD_SIZE];
                    int group_count = 0;
                    get_group(r, f, color, visited, &group_count, group_r, group_f);

                    for (int i = 0; i < group_count; i++) {
                        int gr = group_r[i];
                        int gf = group_f[i];
                        if (!(gr == skip_r && gf == skip_f)) {
                            to_capture[gr][gf] = 1;
                            captured_count++;
                        }
                        processed[gr][gf] = 1;
                    }
                } else {
                    // Mark the group as processed
                    int visited[BOARD_SIZE][BOARD_SIZE] = {0};
                    int group_r[BOARD_SIZE * BOARD_SIZE];
                    int group_f[BOARD_SIZE * BOARD_SIZE];
                    int group_count = 0;
                    get_group(r, f, color, visited, &group_count, group_r, group_f);

                    for (int i = 0; i < group_count; i++) {
                        processed[group_r[i]][group_f[i]] = 1;
                    }
                }
            }
        }
    }

    // Update prisoner counts
    if (captured_count > 0) {
        if (placed_stone_color == 1) { // Black placed the stone, so white stones were captured
            black_prisoners += captured_count;
        } else { // White placed the stone, so black stones were captured
            white_prisoners += captured_count;
        }
    }

    // Second pass: remove all captured stones
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            if (to_capture[r][f]) {
                board[r][f] = 0;

                // Remove from stones array (only game stones, not analysis stones)
                for (int j = 0; j < stone_count; j++) {
                    if (stones[j].r == r && stones[j].f == f) {
                        // Shift remaining stones
                        for (int k = j; k < stone_count - 1; k++) {
                            stones[k] = stones[k + 1];
                        }
                        stone_count--;
                        break;
                    }
                }

                // Also remove from analysis stones if present
                for (int j = 0; j < analysis_stone_count; j++) {
                    if (analysis_stones[j].r == r && analysis_stones[j].f == f) {
                        // Shift remaining stones
                        for (int k = j; k < analysis_stone_count - 1; k++) {
                            analysis_stones[k] = analysis_stones[k + 1];
                        }
                        analysis_stone_count--;
                        break;
                    }
                }
            }
        }
    }

    // Check if selected group was captured
    if (selected_group_count > 0) {
        int still_valid = 1;
        for (int i = 0; i < selected_group_count; i++) {
            int r = selected_group_stones[i][0];
            int f = selected_group_stones[i][1];
            if (board[r][f] == 0) {
                still_valid = 0;
                break;
            }
        }
        if (!still_valid) {
            selected_group_count = 0;
        }
    }
}

void get_group(int r, int f, int color, int visited[BOARD_SIZE][BOARD_SIZE], int *group_count, int group_r[], int group_f[]) {
    if (r < 0 || r >= BOARD_SIZE || f < 0 || f >= BOARD_SIZE || visited[r][f] || board[r][f] != (color ? 1 : 2)) {
        return;
    }

    visited[r][f] = 1;
    group_r[*group_count] = r;
    group_f[*group_count] = f;
    (*group_count)++;

    // Check adjacent intersections
    get_group(r - 1, f, color, visited, group_count, group_r, group_f);
    get_group(r + 1, f, color, visited, group_count, group_r, group_f);
    get_group(r, f - 1, color, visited, group_count, group_r, group_f);
    get_group(r, f + 1, color, visited, group_count, group_r, group_f);
}

int has_liberties(int r, int f, int color, int visited[BOARD_SIZE][BOARD_SIZE]) {
    if (r < 0 || r >= BOARD_SIZE || f < 0 || f >= BOARD_SIZE || visited[r][f]) {
        return 0;
    }

    if (board[r][f] == 0) {
        return 1; // Empty intersection = liberty
    }

    if (board[r][f] != (color ? 1 : 2)) {
        return 0; // Opponent's stone
    }

    visited[r][f] = 1;

    // Check adjacent intersections
    if (has_liberties(r - 1, f, color, visited)) return 1;
    if (has_liberties(r + 1, f, color, visited)) return 1;
    if (has_liberties(r, f - 1, color, visited)) return 1;
    if (has_liberties(r, f + 1, color, visited)) return 1;

    return 0;
}

void get_liberties(int r, int f, int color, int visited[BOARD_SIZE][BOARD_SIZE]) {
    if (r < 0 || r >= BOARD_SIZE || f < 0 || f >= BOARD_SIZE || visited[r][f]) {
        return;
    }

    if (board[r][f] == 0) {
        // Found a liberty - add it to the list if not already present
        int already_present = 0;
        for (int i = 0; i < liberty_count; i++) {
            if (liberty_r[i] == r && liberty_f[i] == f) {
                already_present = 1;
                break;
            }
        }
        if (!already_present && liberty_count < BOARD_SIZE * BOARD_SIZE) {
            liberty_r[liberty_count] = r;
            liberty_f[liberty_count] = f;
            liberty_count++;
        }
        return;
    }

    if (board[r][f] != (color ? 1 : 2)) {
        return; // Opponent's stone
    }

    visited[r][f] = 1;

    // Check adjacent intersections
    get_liberties(r - 1, f, color, visited);
    get_liberties(r + 1, f, color, visited);
    get_liberties(r, f - 1, color, visited);
    get_liberties(r, f + 1, color, visited);
}

static const Glyph font_glyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08}},
    {'\'',{0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'/', {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}},
    {'(', {0x04, 0x08, 0x10, 0x10, 0x10, 0x08, 0x04}},
    {')', {0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x04}},
    {':', {0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}}
};

static const unsigned char *get_glyph_rows(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof(font_glyphs) / sizeof(font_glyphs[0]); i++) {
        if (font_glyphs[i].c == c) return font_glyphs[i].rows;
    }
    return font_glyphs[7].rows;  // '?'
}

int text_width_px(const char *text, int scale) {
    int len = (int)strlen(text);
    if (len <= 0) return 0;
    return (len * 6 - 1) * scale;
}

void draw_text(int x, int y, int scale, const char *text, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    int pen_x = x;
    for (const char *p = text; *p; p++) {
        const unsigned char *rows = get_glyph_rows(*p);
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (rows[r] & (1 << (4 - c))) {
                    SDL_Rect rect = {pen_x + c * scale, y + r * scale, scale, scale};
                    SDL_RenderFillRect(renderer, &rect);
                }
            }
        }
        pen_x += 6 * scale;
    }
}

void draw_color_swatch(int x, int y, int size, SDL_Color fill, SDL_Color outline) {
    SDL_Rect rect = {x, y, size, size};
    SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, outline.r, outline.g, outline.b, outline.a);
    SDL_RenderDrawRect(renderer, &rect);
}

int adjust_move_delay(int delta_ms, Uint32 now) {
    int new_delay = move_delay_ms + delta_ms;
    if (new_delay < MOVE_DELAY_MIN_MS) new_delay = MOVE_DELAY_MIN_MS;
    if (new_delay > MOVE_DELAY_MAX_MS) new_delay = MOVE_DELAY_MAX_MS;
    if (new_delay == move_delay_ms) return 0;
    move_delay_ms = new_delay;
    speed_message_until = now + SPEED_MESSAGE_MS;
    return 1;
}

void render_speed_label(const BoardView *view) {
    if (speed_message_until == 0) return;
    Uint32 now = SDL_GetTicks();
    if (now >= speed_message_until) {
        speed_message_until = 0;
        return;
    }

    char buf[32];
    int whole = move_delay_ms / 1000;
    int rem = move_delay_ms % 1000;
    if (rem == 0) {
        const char *unit = (whole == 1) ? "second" : "seconds";
        snprintf(buf, sizeof(buf), "%d %s/move", whole, unit);
    } else {
        snprintf(buf, sizeof(buf), "%d.%d seconds/move", whole, rem / 100);
    }

    int scale = (view->square >= 30) ? 3 : 2;
    int margin = (view->square >= 30) ? 16 : 8;
    int text_w = text_width_px(buf, scale);
    int text_h = 7 * scale;
    int x = view->offset_x + (view->board_px - text_w) / 2;
    if (x < view->offset_x + margin) x = view->offset_x + margin;
    int y = view->offset_y + margin;

    int pad = (scale >= 3) ? 4 : 3;
    SDL_Rect bg = {x - pad, y - pad, text_w + pad * 2, text_h + pad * 2};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 180);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color text_color = {255, 255, 255, 255};
    draw_text(x, y, scale, buf, text_color);
}

void render_help_overlay(const BoardView *view) {
    if (!show_help) return;

    const char *lines[] = {
        "GO VIEWER HELP",
        "ALL MODES:",
        "  Q: QUIT",
        "  N: NEXT GAME",
        "  P: PREV GAME",
        "  R: RESTART GAME",
        "  C: OPEN CATALOG",
        "  ESC: TOGGLE HELP",
        "  UP/DOWN: SPEED",
        "PLAYBACK:",
        "  SPACE: PAUSE/RESUME",
        "  A: TOGGLE ANALYSIS",
        "  U: TOGGLE CHAIN MODE",
        "  G: TOGGLE GUESS MODE",
        "PLAYBACK:",
        "  LEFT/RIGHT: STEP MOVES",
        "  LEFT CLICK STONE: TOGGLE LIBERTIES",
        "ANALYSIS (A):",
        "  LEFT CLICK STONE: TOGGLE LIBERTIES",
        "  LEFT CLICK EMPTY: PLACE STONE",
        "  HOLD B: FORCE BLACK STONES",
        "  HOLD W: FORCE WHITE STONES",
        "  RIGHT CLICK: REMOVE STONE",
        "GUESS MODE (G):",
        "  LEFT CLICK: GUESS MOVE",
        "  SCORE: 1 POINT IF MATCH",
        "CATALOG (C):",
        "  UP/DOWN: SELECT FILE",
        "  ENTER: OPEN",
        "  ESC: CLOSE"
    };
    int line_count = (int)(sizeof(lines) / sizeof(lines[0]));

    int scale = (view->square >= 30) ? 3 : 2;
    int line_gap = (scale >= 3) ? 4 : 3;
    int text_h = 7 * scale;
    int max_w = 0;
    for (int i = 0; i < line_count; i++) {
        int w = text_width_px(lines[i], scale);
        if (w > max_w) max_w = w;
    }

    int total_h = line_count * text_h + (line_count - 1) * line_gap;
    int pad = (scale >= 3) ? 10 : 8;
    int box_w = max_w + pad * 2;
    int box_h = total_h + pad * 2;
    int x = (view->screen_w - box_w) / 2;
    int y = (view->screen_h - box_h) / 2;

    SDL_Rect bg = {x, y, box_w, box_h};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 180);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color text_color = {255, 255, 255, 255};
    int text_x = x + pad;
    int text_y = y + pad;
    for (int i = 0; i < line_count; i++) {
        draw_text(text_x, text_y, scale, lines[i], text_color);
        text_y += text_h + line_gap;
    }
}

void render_guess_score(const BoardView *view) {
    if (!guess_mode) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "Score: %d", guess_score);

    int scale = (view->square >= 30) ? 3 : 2;
    int margin = (view->square >= 30) ? 16 : 8;
    int text_w = text_width_px(buf, scale);
    int text_h = 7 * scale;
    int swatch_size = text_h;
    if (swatch_size > 16) swatch_size = 16;
    int gap = (scale >= 3) ? 6 : 4;

    int x = view->offset_x + margin;
    int y = view->offset_y + view->board_px - margin - text_h;
    int swatch_x = x - gap - swatch_size;

    int left_space = view->offset_x;
    int right_space = view->screen_w - (view->offset_x + view->board_px);
    int need_w = text_w + swatch_size + gap + margin * 2;
    if (left_space >= need_w) {
        x = view->offset_x - margin - text_w;
        swatch_x = x - gap - swatch_size;
    } else if (right_space >= need_w) {
        swatch_x = view->offset_x + view->board_px + margin;
        x = swatch_x + swatch_size + gap;
    } else if (view->offset_y >= text_h + margin * 2) {
        swatch_x = view->offset_x + margin;
        x = swatch_x + swatch_size + gap;
        y = view->offset_y - margin - text_h;
    } else {
        swatch_x = view->offset_x + margin;
        x = swatch_x + swatch_size + gap;
    }
    if (y < view->offset_y + margin) {
        y = view->offset_y + margin;
    }

    SDL_Color fill = turn_is_black ? (SDL_Color){25, 25, 25, 255} : (SDL_Color){235, 235, 235, 255};
    SDL_Color outline = turn_is_black ? (SDL_Color){235, 235, 235, 255} : (SDL_Color){30, 30, 30, 255};
    draw_color_swatch(swatch_x, y + (text_h - swatch_size) / 2, swatch_size, fill, outline);

    SDL_Color text_color = {255, 255, 255, 255};
    draw_text(x, y, scale, buf, text_color);
}

static int filename_cmp(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
#ifdef _WIN32
    return _stricmp(sa, sb);
#else
    return strcasecmp(sa, sb);
#endif
}

static int catalog_entry_cmp(const void *a, const void *b) {
    const CatalogEntry *ea = (const CatalogEntry *)a;
    const CatalogEntry *eb = (const CatalogEntry *)b;
    if (ea->type != eb->type) return (ea->type < eb->type) ? -1 : 1;
#ifdef _WIN32
    return _stricmp(ea->name, eb->name);
#else
    return strcasecmp(ea->name, eb->name);
#endif
}

static int push_string(char ***items, int *count, int *cap, const char *str) {
    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 16 : (*cap * 2);
        char **next = (char **)realloc(*items, (size_t)new_cap * sizeof(*next));
        if (!next) return 0;
        *items = next;
        *cap = new_cap;
    }
    (*items)[*count] = copy_string(str);
    if (!(*items)[*count]) return 0;
    (*count)++;
    return 1;
}

static int push_catalog_entry(CatalogEntry **items, int *count, int *cap, const char *name, int type) {
    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 16 : (*cap * 2);
        CatalogEntry *next = (CatalogEntry *)realloc(*items, (size_t)new_cap * sizeof(*next));
        if (!next) return 0;
        *items = next;
        *cap = new_cap;
    }
    (*items)[*count].name = copy_string(name);
    if (!(*items)[*count].name) return 0;
    (*items)[*count].type = type;
    (*count)++;
    return 1;
}

static void catalog_set_dir(const char *new_dir) {
    if (!new_dir) {
        catalog_dir[0] = '\0';
        return;
    }
    strncpy(catalog_dir, new_dir, sizeof(catalog_dir) - 1);
    catalog_dir[sizeof(catalog_dir) - 1] = '\0';
}

static void catalog_dir_up(void) {
    size_t len = strlen(catalog_dir);
    if (len == 0) return;
    for (size_t i = len; i > 0; i--) {
        if (catalog_dir[i - 1] == '/' || catalog_dir[i - 1] == '\\') {
            catalog_dir[i - 1] = '\0';
            return;
        }
    }
    catalog_dir[0] = '\0';
}

static int catalog_load_entries(const char *games_dir) {
    CatalogEntry *entries = NULL;
    int count = 0;
    int cap = 0;
    char *dir_path = NULL;
    if (catalog_dir[0] == '\0') {
        dir_path = copy_string(games_dir);
    } else {
        dir_path = join_path(games_dir, catalog_dir);
    }
    if (!dir_path) return 0;

#ifdef _WIN32
    char *search = join_path(dir_path, "*");
    if (!search) {
        free(dir_path);
        return 0;
    }
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(search, &data);
    free(search);
    if (h == INVALID_HANDLE_VALUE) {
        free(dir_path);
        return 0;
    }
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!push_catalog_entry(&entries, &count, &cap, data.cFileName, 1)) {
                FindClose(h);
                free(dir_path);
                catalog_entries = entries;
                catalog_entry_count = count;
                return 0;
            }
        } else if (has_sgf_extension(data.cFileName)) {
            if (!push_catalog_entry(&entries, &count, &cap, data.cFileName, 0)) {
                FindClose(h);
                free(dir_path);
                catalog_entries = entries;
                catalog_entry_count = count;
                return 0;
            }
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *d = opendir(dir_path);
    if (!d) {
        free(dir_path);
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char *full = join_path(dir_path, ent->d_name);
        if (!full) {
            closedir(d);
            free(dir_path);
            return 0;
        }
        int is_dir = 0;
#ifdef DT_DIR
        if (ent->d_type == DT_DIR) is_dir = 1;
        if (ent->d_type == DT_UNKNOWN)
#endif
        {
            DIR *probe = opendir(full);
            if (probe) {
                is_dir = 1;
                closedir(probe);
            }
        }
        if (is_dir) {
            if (!push_catalog_entry(&entries, &count, &cap, ent->d_name, 1)) {
                free(full);
                closedir(d);
                free(dir_path);
                return 0;
            }
        } else if (has_sgf_extension(ent->d_name)) {
            if (!push_catalog_entry(&entries, &count, &cap, ent->d_name, 0)) {
                free(full);
                closedir(d);
                free(dir_path);
                return 0;
            }
        }
        free(full);
    }
    closedir(d);
#endif

    if (catalog_dir[0] != '\0') {
        push_catalog_entry(&entries, &count, &cap, "..", 2);
    }

    if (count > 1) {
        qsort(entries, (size_t)count, sizeof(entries[0]), catalog_entry_cmp);
    }

    for (int i = 0; i < catalog_entry_count; i++) {
        free(catalog_entries[i].name);
    }
    free(catalog_entries);
    catalog_entries = entries;
    catalog_entry_count = count;
    free(dir_path);
    return 1;
}

void catalog_free(void) {
    if (catalog_entries) {
        for (int i = 0; i < catalog_entry_count; i++) {
            free(catalog_entries[i].name);
        }
        free(catalog_entries);
    }
    catalog_entries = NULL;
    catalog_entry_count = 0;
    catalog_index = 0;
    catalog_scroll = 0;
    catalog_active = 0;
}

void catalog_open(const char *games_dir) {
    if (catalog_active) return;
    catalog_free();
    // Store the base directory for consistent navigation
    strncpy(catalog_base_dir, games_dir, sizeof(catalog_base_dir) - 1);
    catalog_base_dir[sizeof(catalog_base_dir) - 1] = '\0';
    catalog_set_dir("");
    if (!catalog_load_entries(games_dir)) {
        catalog_entries = NULL;
        catalog_entry_count = 0;
        return;
    }
    catalog_active = 1;
    catalog_selection_made = 0;
    catalog_index = 0;
    catalog_scroll = 0;
}

void catalog_select(const char *games_dir) {
    if (!catalog_active) return;
    int entry_index = catalog_index;
    if (entry_index >= 0 && entry_index < catalog_entry_count) {
        CatalogEntry *entry = &catalog_entries[entry_index];
        if (entry->type == 2) {
            catalog_dir_up();
            catalog_load_entries(games_dir);
            catalog_index = 0;
            catalog_scroll = 0;
            return;
        }
        if (entry->type == 1) {
            char next_dir[1024];
            if (catalog_dir[0] == '\0') {
                snprintf(next_dir, sizeof(next_dir), "%s", entry->name);
            } else {
                snprintf(next_dir, sizeof(next_dir), "%s%c%s", catalog_dir, PATH_SEP, entry->name);
            }
            catalog_set_dir(next_dir);
            catalog_load_entries(games_dir);
            catalog_index = 0;
            catalog_scroll = 0;
            return;
        }
        char *dir_path = NULL;
        if (catalog_dir[0] == '\0') {
            dir_path = copy_string(games_dir);
            // Reset sequential mode when selecting from root
            sequential_dir[0] = '\0';
            sequential_index = 0;
        } else {
            dir_path = join_path(games_dir, catalog_dir);
            // Set sequential mode when selecting from a subdirectory
            if (dir_path) {
                strncpy(sequential_dir, dir_path, sizeof(sequential_dir) - 1);
                sequential_dir[sizeof(sequential_dir) - 1] = '\0';
                sequential_index = 0;
            }
        }
        if (dir_path) {
            char *path = join_path(dir_path, entry->name);
            free(dir_path);
            if (path) {
                free(forced_sgf_path);
                forced_sgf_path = path;
            }
        }
    }
    catalog_selection_made = 1;
    catalog_active = 0;
}

int catalog_total_entries(void) {
    return catalog_entry_count;
}

void render_catalog_overlay(const BoardView *view) {
    if (!catalog_active) return;

    const char *title = "CATALOG";
    int total_entries = catalog_total_entries();

    int scale = (view->square >= 30) ? 3 : 2;
    int line_gap = (scale >= 3) ? 4 : 3;
    int text_h = 7 * scale;
    int pad = (scale >= 3) ? 10 : 8;
    int header_gap = line_gap + (scale >= 3 ? 4 : 2);

    int max_w = text_width_px(title, scale);
    for (int i = 0; i < catalog_entry_count; i++) {
        char label[1024];
        const CatalogEntry *entry = &catalog_entries[i];
        if (entry->type == 1) {
            snprintf(label, sizeof(label), "[DIR] %s", entry->name);
        } else if (entry->type == 2) {
            snprintf(label, sizeof(label), "[..]");
        } else {
            snprintf(label, sizeof(label), "%s", entry->name);
        }
        int w = text_width_px(label, scale);
        if (w > max_w) max_w = w;
    }

    int available_h = view->screen_h - pad * 4 - text_h - header_gap;
    int line_h = text_h + line_gap;
    int max_lines = (available_h > 0) ? (available_h / line_h) : 0;
    if (max_lines < 4) max_lines = 4;
    if (max_lines > total_entries) max_lines = total_entries;

    if (catalog_index < catalog_scroll) catalog_scroll = catalog_index;
    if (catalog_index >= catalog_scroll + max_lines) {
        catalog_scroll = catalog_index - max_lines + 1;
    }

    int list_h = max_lines * line_h - line_gap;
    int box_w = max_w + pad * 2;
    int box_h = text_h + header_gap + list_h + pad * 2;
    int x = (view->screen_w - box_w) / 2;
    int y = (view->screen_h - box_h) / 2;

    SDL_Rect bg = {x, y, box_w, box_h};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 190);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color text_color = {255, 255, 255, 255};
    int text_x = x + pad;
    int text_y = y + pad;
    draw_text(text_x, text_y, scale, title, text_color);
    text_y += text_h + header_gap;

    for (int i = 0; i < max_lines; i++) {
        int idx = catalog_scroll + i;
        if (idx >= total_entries) break;
        char label[1024];
        const CatalogEntry *entry = &catalog_entries[idx];
        if (entry->type == 1) {
            snprintf(label, sizeof(label), "[DIR] %s", entry->name);
        } else if (entry->type == 2) {
            snprintf(label, sizeof(label), "[..]");
        } else {
            snprintf(label, sizeof(label), "%s", entry->name);
        }
        if (idx == catalog_index) {
            SDL_Rect hi = {text_x - 3, text_y - 3, max_w + 6, text_h + 6};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 40, 120, 255, 190);
            SDL_RenderFillRect(renderer, &hi);
        }
        draw_text(text_x, text_y, scale, label, text_color);
        text_y += line_h;
    }
}

int handle_catalog_event(const SDL_Event *e, const char *games_dir) {
    if (!catalog_active) return 0;
    if (e->type == SDL_KEYDOWN) {
        SDL_Keycode key = e->key.keysym.sym;
        if (key == SDLK_ESCAPE || key == SDLK_c) {
            catalog_active = 0;
            return 1;
        } else if (key == SDLK_UP) {
            if (catalog_index > 0) catalog_index--;
            return 1;
        } else if (key == SDLK_DOWN) {
            int total = catalog_total_entries();
            if (catalog_index < total - 1) catalog_index++;
            return 1;
        } else if (key == SDLK_PAGEUP) {
            int step = 6;
            catalog_index -= step;
            if (catalog_index < 0) catalog_index = 0;
            return 1;
        } else if (key == SDLK_PAGEDOWN) {
            int step = 6;
            int total = catalog_total_entries();
            catalog_index += step;
            if (catalog_index > total - 1) catalog_index = total - 1;
            return 1;
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            catalog_select(catalog_base_dir);
            game_nav_request = GAME_NAV_SELECT;
            return 1;
        }
    }
    return 1;
}

void render_player_labels(const BoardView *view) {
    int margin = (view->square >= 30) ? 16 : 8;
    int right_x0 = view->offset_x + view->board_px + margin;
    int right_x1 = view->screen_w - margin;
    if (right_x1 <= right_x0) return;

    int swatch_size = (view->square >= 30) ? 16 : 12;
    int gap = 6;
    int avail_text_w = right_x1 - right_x0 - swatch_size - gap;
    if (avail_text_w <= 0) return;

    const char *black_name = (current_black_name[0] != '\0') ? current_black_name : "Black";
    const char *white_name = (current_white_name[0] != '\0') ? current_white_name : "White";
    int max_len = (int)strlen(black_name);
    int white_len = (int)strlen(white_name);
    if (white_len > max_len) max_len = white_len;

    // Create prisoner count strings
    char black_prisoner_str[32];
    char white_prisoner_str[32];
    snprintf(black_prisoner_str, sizeof(black_prisoner_str), "Prisoners: %d", black_prisoners);
    snprintf(white_prisoner_str, sizeof(white_prisoner_str), "Prisoners: %d", white_prisoners);

    int prisoner_len = (int)strlen(black_prisoner_str);
    int white_prisoner_len = (int)strlen(white_prisoner_str);
    if (white_prisoner_len > prisoner_len) prisoner_len = white_prisoner_len;
    if (prisoner_len > max_len) max_len = prisoner_len;

    int scale = 3;
    int need_w = (max_len > 0) ? text_width_px(black_name, scale) : 0;
    if (white_len > 0) {
        int white_w = text_width_px(white_name, scale);
        if (white_w > need_w) need_w = white_w;
    }
    int prisoner_w = text_width_px(black_prisoner_str, scale);
    if (prisoner_w > need_w) need_w = prisoner_w;
    if (need_w > avail_text_w && max_len > 0) {
        int denom = max_len * 6 - 1;
        scale = avail_text_w / denom;
        if (scale < 1) scale = 1;
    }

    int text_h = 7 * scale;
    if (swatch_size > text_h) swatch_size = text_h;
    int top_y = view->offset_y + margin;
    int bottom_y = view->offset_y + view->board_px - margin - text_h * 2 - 2; // Space for two lines
    if (bottom_y < top_y) bottom_y = top_y;

    SDL_Color text_color = {230, 230, 230, 255};
    SDL_Color prisoner_color = {120, 120, 120, 255}; // Even darker than text_color
    SDL_Color black_fill = {20, 20, 20, 255};
    SDL_Color white_fill = {230, 230, 230, 255};
    SDL_Color outline = {30, 30, 30, 255};

    int swatch_y_top = top_y + (text_h - swatch_size) / 2;
    int swatch_y_bottom = bottom_y + (text_h - swatch_size) / 2;

    draw_color_swatch(right_x0, swatch_y_top, swatch_size, black_fill, white_fill);
    draw_text(right_x0 + swatch_size + gap, top_y, scale, black_name, text_color);
    draw_text(right_x0 + swatch_size + gap, top_y + text_h + 8, scale, black_prisoner_str, prisoner_color);

    draw_color_swatch(right_x0, swatch_y_bottom, swatch_size, white_fill, outline);
    draw_text(right_x0 + swatch_size + gap, bottom_y, scale, white_name, text_color);
    draw_text(right_x0 + swatch_size + gap, bottom_y + text_h + 8, scale, white_prisoner_str, prisoner_color);
}

void reset_board(void) {
    memset(board, 0, sizeof(board));
    stone_count = 0;
    analysis_stone_count = 0;
    board_state_count = 0;
    game_finished = 0;
    game_finished_timer = 0;  // Reset timer for new games
    black_prisoners = 0;
    white_prisoners = 0;
    paused = 0;  // Ensure new games start unpaused
}

void add_stone(int r, int f, int is_black) {
    if (stone_count >= MAX_MOVES) return;
    board[r][f] = (char)(is_black ? 1 : 2);
    stones[stone_count].r = r;
    stones[stone_count].f = f;
    stones[stone_count].is_black = is_black;
    stone_count++;
}

void add_analysis_stone(int r, int f, int is_black) {
    if (analysis_stone_count >= MAX_MOVES) return;
    board[r][f] = (char)(is_black ? 1 : 2);
    analysis_stones[analysis_stone_count].r = r;
    analysis_stones[analysis_stone_count].f = f;
    analysis_stones[analysis_stone_count].is_black = is_black;
    analysis_stone_count++;
}

void remove_last_stone(void) {
    if (stone_count <= 0) return;
    stone_count--;
    int r = stones[stone_count].r;
    int f = stones[stone_count].f;
    board[r][f] = 0;
}

void remove_last_analysis_stone(void) {
    if (analysis_stone_count <= 0) return;
    analysis_stone_count--;
    int r = analysis_stones[analysis_stone_count].r;
    int f = analysis_stones[analysis_stone_count].f;
    board[r][f] = 0;
}

void clear_analysis_stones(void) {
    for (int i = 0; i < analysis_stone_count; i++) {
        int r = analysis_stones[i].r;
        int f = analysis_stones[i].f;
        board[r][f] = 0;
    }
    analysis_stone_count = 0;
}

void save_board_state(void) {
    if (board_state_count >= MAX_MOVES) return;
    memcpy(board_states[board_state_count].board, board, sizeof(board));
    memcpy(board_states[board_state_count].stones, stones, sizeof(stones));
    board_states[board_state_count].stone_count = stone_count;
    board_states[board_state_count].black_prisoners = black_prisoners;
    board_states[board_state_count].white_prisoners = white_prisoners;
    board_state_count++;
}

void restore_board_state(int state_index) {
    if (state_index < 0 || state_index >= board_state_count) return;
    memcpy(board, board_states[state_index].board, sizeof(board));
    memcpy(stones, board_states[state_index].stones, sizeof(stones));
    stone_count = board_states[state_index].stone_count;
    black_prisoners = board_states[state_index].black_prisoners;
    white_prisoners = board_states[state_index].white_prisoners;
    board_state_count = state_index + 1;  // Truncate states after this point
}

void draw_stone_circle(const BoardView *view, int r, int f, int is_black, Uint8 alpha) {
    int x = 0;
    int y = 0;
    board_to_screen(view, r, f, &x, &y);

    int center_x = x + view->square / 2;
    int center_y = y + view->square / 2;
    int radius = view->square / 2 - 2;

    // Draw filled circle using pixels
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx*dx + dy*dy <= radius*radius) {
                SDL_SetRenderDrawColor(renderer,
                    is_black ? 30 : 240,
                    is_black ? 30 : 240,
                    is_black ? 30 : 240,
                    alpha);
                SDL_RenderDrawPoint(renderer, center_x + dx, center_y + dy);
            }
        }
    }
}

void board_to_screen(const BoardView *view, int board_r, int board_f, int *out_x, int *out_y) {
    if (out_x) *out_x = view->offset_x + board_f * view->square;
    if (out_y) *out_y = view->offset_y + board_r * view->square;
}

int screen_to_board(const BoardView *view, int x, int y, int *out_r, int *out_f) {
    if (x < view->offset_x || y < view->offset_y) return 0;
    if (x >= view->offset_x + view->board_px || y >= view->offset_y + view->board_px) return 0;
    int rel_x = x - view->offset_x;
    int rel_y = y - view->offset_y;
    int board_f = rel_x / view->square;
    int board_r = rel_y / view->square;
    if (board_r < 0 || board_r >= BOARD_SIZE || board_f < 0 || board_f >= BOARD_SIZE) return 0;

    int inset = view->square / 8;  // center 75%
    int local_x = rel_x - board_f * view->square;
    int local_y = rel_y - board_r * view->square;
    if (local_x < inset || local_x >= view->square - inset) return 0;
    if (local_y < inset || local_y >= view->square - inset) return 0;

    if (out_r) *out_r = board_r;
    if (out_f) *out_f = board_f;
    return 1;
}

void draw_thick_line(int x1, int y1, int x2, int y2, int thickness, SDL_Color color) {
    if (thickness < 1) thickness = 1;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    float dx = (float)(x2 - x1);
    float dy = (float)(y2 - y1);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) {
        SDL_RenderDrawPoint(renderer, x1, y1);
        return;
    }
    float nx = dy / len;
    float ny = -dx / len;
    float half = (float)thickness * 0.5f;
    float ox = nx * half;
    float oy = ny * half;
    SDL_Vertex verts[4];
    verts[0].position.x = (float)x1 + ox;
    verts[0].position.y = (float)y1 + oy;
    verts[1].position.x = (float)x1 - ox;
    verts[1].position.y = (float)y1 - oy;
    verts[2].position.x = (float)x2 - ox;
    verts[2].position.y = (float)y2 - oy;
    verts[3].position.x = (float)x2 + ox;
    verts[3].position.y = (float)y2 + oy;
    for (int i = 0; i < 4; i++) {
        verts[i].color = color;
        verts[i].tex_coord.x = 0.0f;
        verts[i].tex_coord.y = 0.0f;
    }
    int indices[6] = {0, 1, 2, 2, 3, 0};
    SDL_RenderGeometry(renderer, NULL, verts, 4, indices, 6);
}

void get_board_view(BoardView *view) {
    int screen_w = SCREEN_SIZE;
    int screen_h = SCREEN_SIZE;
    if (SDL_GetRendererOutputSize(renderer, &screen_w, &screen_h) != 0) {
        screen_w = SCREEN_SIZE;
        screen_h = SCREEN_SIZE;
    }
    int min_dim = (screen_w < screen_h) ? screen_w : screen_h;
    view->square = min_dim / BOARD_SIZE;
    if (view->square < 1) view->square = 1;
    view->board_px = view->square * BOARD_SIZE;
    view->offset_x = (screen_w - view->board_px) / 2;
    view->offset_y = (screen_h - view->board_px) / 2;
    view->screen_w = screen_w;
    view->screen_h = screen_h;
}

void render_chain_connections(const BoardView *view) {
    if (!chain_mode) return;

    // Track which connections we've already drawn to avoid duplicates
    int connections_drawn[BOARD_SIZE][BOARD_SIZE][4] = {0}; // 4 directions: up, right, down, left

    // Iterate through all stones on the board
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            if (board[r][f] == 0) continue;

            int color = (board[r][f] == 1) ? 1 : 0;

            // Find the connected group for this stone
            int visited[BOARD_SIZE][BOARD_SIZE] = {0};
            int group_r[BOARD_SIZE * BOARD_SIZE];
            int group_f[BOARD_SIZE * BOARD_SIZE];
            int group_count = 0;
            get_group(r, f, color, visited, &group_count, group_r, group_f);

            // Draw connections between adjacent stones in the group
            for (int i = 0; i < group_count; i++) {
                int sr = group_r[i];
                int sf = group_f[i];

                // Check adjacent positions
                int adj_positions[4][2] = {
                    {sr - 1, sf}, // up
                    {sr, sf + 1}, // right
                    {sr + 1, sf}, // down
                    {sr, sf - 1}  // left
                };

                for (int dir = 0; dir < 4; dir++) {
                    int ar = adj_positions[dir][0];
                    int af = adj_positions[dir][1];

                    // Check if adjacent position is in bounds and has the same color stone
                    if (ar >= 0 && ar < BOARD_SIZE && af >= 0 && af < BOARD_SIZE &&
                        board[ar][af] != 0 && (board[ar][af] == board[sr][sf])) {

                        // Check if this connection has already been drawn from the other side
                        int reverse_dir = (dir + 2) % 4; // opposite direction
                        if (!connections_drawn[sr][sf][dir] && !connections_drawn[ar][af][reverse_dir]) {
                            // Mark this connection as drawn
                            connections_drawn[sr][sf][dir] = 1;
                            connections_drawn[ar][af][reverse_dir] = 1;

                            // Draw line between stone centers
                            int x1 = view->offset_x + sf * view->square + view->square / 2;
                            int y1 = view->offset_y + sr * view->square + view->square / 2;
                            int x2 = view->offset_x + af * view->square + view->square / 2;
                            int y2 = view->offset_y + ar * view->square + view->square / 2;

                            // Use stone color for the connection lines
                            SDL_Color line_color = color ? (SDL_Color){30, 30, 30, 255} : (SDL_Color){240, 240, 240, 255};
                            // Make lines 1/2 the diameter of the stone circles
                            int stone_diameter = view->square - 4;  // stone radius is square/2 - 2, so diameter is square - 4
                            int thickness = stone_diameter / 2;
                            draw_thick_line(x1, y1, x2, y2, thickness, line_color);
                        }
                    }
                }
            }
        }
    }
}

void render_board(const BoardView *view, const Overlay *overlay) {
    SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
    SDL_RenderClear(renderer);

    // Draw board background - different color when paused
    if (paused) {
        SDL_SetRenderDrawColor(renderer, 70, 80, 100, 255);  // Darker when paused
		}
		else if (analysis_mode){
        SDL_SetRenderDrawColor(renderer, 110, 115, 150, 255);  // Analysis color
    } else {
        SDL_SetRenderDrawColor(renderer, 95, 115, 150, 255);  // Normal color
    }
    SDL_Rect board_rect = {view->offset_x, view->offset_y, view->board_px, view->board_px};
    SDL_RenderFillRect(renderer, &board_rect);

    // Draw grid lines
    SDL_Color grid_color = {0, 0, 0, 255};
    int normal_thickness = (view->square >= 30) ? 2 : 1;
    int boundary_thickness = normal_thickness * 2;

    // Draw boundary lines first (extend exactly to board edges accounting for thickness)
    int boundary_indices[2] = {0, BOARD_SIZE - 1};
    for (int bi = 0; bi < 2; bi++) {
        int i = boundary_indices[bi];

        // Horizontal boundary lines - extend exactly to board edges
        int y = view->offset_y + i * view->square + view->square / 2;
        int x_start = view->offset_x + boundary_thickness / 2;
        int x_end = view->offset_x + view->board_px - boundary_thickness / 2;
        draw_thick_line(x_start, y, x_end, y, boundary_thickness, grid_color);

        // Vertical boundary lines - extend exactly to board edges
        int x = view->offset_x + i * view->square + view->square / 2;
        int y_start = view->offset_y + boundary_thickness / 2;
        int y_end = view->offset_y + view->board_px - boundary_thickness / 2;
        draw_thick_line(x, y_start, x, y_end, boundary_thickness, grid_color);
    }

    // Draw internal lines (span from first to last boundary positions)
    for (int i = 1; i < BOARD_SIZE - 1; i++) {
        // Horizontal internal lines - span between boundary lines
        int y = view->offset_y + i * view->square + view->square / 2;
        int x_start = view->offset_x + view->square / 2;  // First boundary position
        int x_end = view->offset_x + (BOARD_SIZE - 1) * view->square + view->square / 2;  // Last boundary position
        draw_thick_line(x_start, y, x_end, y, normal_thickness, grid_color);

        // Vertical internal lines - span between boundary lines
        int x = view->offset_x + i * view->square + view->square / 2;
        int y_start = view->offset_y + view->square / 2;  // First boundary position
        int y_end = view->offset_y + (BOARD_SIZE - 1) * view->square + view->square / 2;  // Last boundary position
        draw_thick_line(x, y_start, x, y_end, normal_thickness, grid_color);
    }



    // Draw star points (hoshi)
    int star_points[9][2] = {
        {3, 3}, {3, 9}, {3, 15},
        {9, 3}, {9, 9}, {9, 15},
        {15, 3}, {15, 9}, {15, 15}
    };
    int star_radius = (view->square >= 30) ? 4 : 3;
    for (int i = 0; i < 9; i++) {
        int r = star_points[i][0];
        int f = star_points[i][1];
        int x = view->offset_x + f * view->square + view->square / 2;
        int y = view->offset_y + r * view->square + view->square / 2;
        SDL_Rect star_rect = {x - star_radius, y - star_radius, star_radius * 2, star_radius * 2};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &star_rect);
    }

    // Draw chain connections (before stones so lines appear behind them)
    render_chain_connections(view);

    // Draw stones
    for (int i = 0; i < stone_count; i++) {
        Stone *stone = &stones[i];
        draw_stone_circle(view, stone->r, stone->f, stone->is_black, 255);
    }

    // Draw analysis stones (if in analysis mode)
    for (int i = 0; i < analysis_stone_count; i++) {
        Stone *stone = &analysis_stones[i];
        draw_stone_circle(view, stone->r, stone->f, stone->is_black, 255);
    }

    // Draw liberty dots
    render_liberties(view);

    if (overlay && overlay->active) {
        // Semi-transparent for preview - convert screen coordinates to board coordinates
        int r = -1, f = -1;
        if (screen_to_board(view, (int)overlay->x, (int)overlay->y, &r, &f)) {
            draw_stone_circle(view, r, f, overlay->is_black, 128);
        }
    }



    render_player_labels(view);
    render_speed_label(view);
    render_guess_score(view);
    render_mode_status(view);
    render_result_message(view);
    render_help_overlay(view);
    render_catalog_overlay(view);

    if (!suppress_present) {
        SDL_RenderPresent(renderer);
    }
}

void draw_board() {
    BoardView view;
    get_board_view(&view);
    render_board(&view, NULL);
}

int animate_move(int r, int f, int is_black) {
    // Check for suicide before placing
    if (would_be_suicide(r, f, is_black)) {
        // Suicide not allowed - don't place the stone
        return 1; // Indicate error
    }
    // Just place the stone immediately - no animation
    add_stone(r, f, is_black);
    // Remove captured stones (skip the placed stone)
    remove_captured_stones(-1, r, f, is_black);
    // Save the board state after the move
    save_board_state();
    draw_board();
    return 0;
}

int parse_sgf_move(const char *move_str, int *out_r, int *out_f) {
    if (!move_str || strlen(move_str) != 2) return 0;
    char f_char = move_str[0];
    char r_char = move_str[1];
    if (f_char < 'a' || f_char > 's' || r_char < 'a' || r_char > 's') return 0;
    *out_f = f_char - 'a';
    *out_r = r_char - 'a';
    return 1;
}

int load_sgf_game(const char *path, char moves[][MOVE_TEXT_LEN], int max_moves,
                  char *black_name, size_t black_name_size,
                  char *white_name, size_t white_name_size,
                  char *result, size_t result_size) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    if (black_name && black_name_size > 0) black_name[0] = '\0';
    if (white_name && white_name_size > 0) white_name[0] = '\0';
    if (result && result_size > 0) result[0] = '\0';

    char line[1024];
    int move_count = 0;
    int in_moves = 0;

    while (fgets(line, sizeof(line), fp)) {
        // Parse moves first (before cleaning brackets)
        char *p = line;
        while (*p) {
            if (*p == ';' && *(p + 1) == 'B' && *(p + 2) == '[') {
                p += 3;
                char *end = strchr(p, ']');
                if (end && move_count < max_moves) {
                    size_t len = (size_t)(end - p);
                    if (len >= MOVE_TEXT_LEN) len = MOVE_TEXT_LEN - 1;
                    memcpy(moves[move_count], p, len);
                    moves[move_count][len] = '\0';
                    move_count++;
                }
            } else if (*p == ';' && *(p + 1) == 'W' && *(p + 2) == '[') {
                p += 3;
                char *end = strchr(p, ']');
                if (end && move_count < max_moves) {
                    size_t len = (size_t)(end - p);
                    if (len >= MOVE_TEXT_LEN) len = MOVE_TEXT_LEN - 1;
                    memcpy(moves[move_count], p, len);
                    moves[move_count][len] = '\0';
                    move_count++;
                }
            }
            p++;
        }

        // Parse properties (these are also in brackets, so parse from original line)
        // Parse PB (black player)
        if (strstr(line, "PB[")) {
            char *start = strstr(line, "PB[");
            if (start) {
                start += 3;
                char *end = strchr(start, ']');
                if (end && black_name) {
                    size_t len = (size_t)(end - start);
                    if (len >= black_name_size) len = black_name_size - 1;
                    memcpy(black_name, start, len);
                    black_name[len] = '\0';
                }
            }
        }
        // Parse PW (white player)
        if (strstr(line, "PW[") || strstr(line, "pw[")) {
            char *start = strstr(line, "PW[");
            if (!start) start = strstr(line, "pw[");
            if (start) {
                start += 3;
                char *end = strchr(start, ']');
                if (end && white_name) {
                    size_t len = (size_t)(end - start);
                    if (len >= white_name_size) len = white_name_size - 1;
                    if (len > 0) {  // Only set if there's actual content
                        // Trim trailing whitespace
                        while (len > 0 && (start[len-1] == ' ' || start[len-1] == '\t' || start[len-1] == '\r' || start[len-1] == '\n')) {
                            len--;
                        }
                        // Trim leading whitespace
                        char *trimmed_start = start;
                        while (len > 0 && (*trimmed_start == ' ' || *trimmed_start == '\t')) {
                            trimmed_start++;
                            len--;
                        }
                        if (len > 0) {
                            memcpy(white_name, trimmed_start, len);
                            white_name[len] = '\0';
                        }
                    }
                }
            }
        }
        // Parse RE (result)
        if (strstr(line, "RE[")) {
            char *start = strstr(line, "RE[");
            if (start) {
                start += 3;
                char *end = strchr(start, ']');
                if (end && result) {
                    size_t len = (size_t)(end - start);
                    if (len >= result_size) len = result_size - 1;
                    memcpy(result, start, len);
                    result[len] = '\0';
                }
            }
        }
    }

    fclose(fp);
    return move_count;
}

int play_game(char moves[][MOVE_TEXT_LEN], int move_count, const char *result) {
    // Store the game result for display
    if (result && result[0]) {
        strncpy(result_message, result, sizeof(result_message) - 1);
        result_message[sizeof(result_message) - 1] = '\0';
    } else {
        result_message[0] = '\0';
    }

    reset_board();
    draw_board();

    int index = 0;
    int quit = 0;
    Uint32 last_move_tick = SDL_GetTicks();
    game_nav_request = GAME_NAV_NONE;
    int analysis_dragging = 0;
    int analysis_r = -1;
    int analysis_f = -1;
    int guess_dragging = 0;
    int guess_pending = 0;
    int guess_to_r = -1;
    int guess_to_f = -1;
    guess_score = 0;

    while (!quit) {
        Uint32 loop_now = SDL_GetTicks();
        update_cursor_auto_hide(loop_now);
        turn_is_black = (index % 2 == 0);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            note_mouse_activity_event(&e);
            if (handle_catalog_event(&e, catalog_active ? catalog_base_dir : games_dir_root)) {
                draw_board();
                if (game_nav_request == GAME_NAV_SELECT && catalog_selection_made) {
                    catalog_selection_made = 0;
                    quit = 1;
                }
                continue;
            }
            if (e.type == SDL_QUIT) {
                quit = 1;
            } else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode key = e.key.keysym.sym;
                if (key == SDLK_q) {
                    game_nav_request = GAME_NAV_NONE;
                    quit = 1;
                } else if (key == SDLK_n) {
                    game_nav_request = GAME_NAV_NEXT;
                    quit = 1;
                } else if (key == SDLK_p) {
                    game_nav_request = GAME_NAV_PREV;
                    quit = 1;
                } else if (key == SDLK_r) {
                    game_nav_request = GAME_NAV_RESTART;
                    quit = 1;
                } else if (key == SDLK_c) {
                    catalog_open(games_dir_root);
                    draw_board();
                } else if (key == SDLK_ESCAPE) {
                    show_help = !show_help;
                    draw_board();
                } else if (key == SDLK_UP || key == SDLK_DOWN) {
                    Uint32 now = SDL_GetTicks();
                    int prev = move_delay_ms;
                    int delta = (key == SDLK_UP) ? MOVE_DELAY_STEP_MS : -MOVE_DELAY_STEP_MS;
                    if (adjust_move_delay(delta, now)) {
                        if (!paused && move_delay_ms > prev) {
                            last_move_tick = now;
                        }
                        draw_board();
                    }
                } else if (key == SDLK_a) {
                    if (analysis_mode) {
                        analysis_mode = 0;
                        clear_analysis_stones();
                        // Clear liberty display when exiting analysis mode
                        liberty_count = 0;
                        // Restore prisoner counts from before analysis mode
                        black_prisoners = analysis_saved_black_prisoners;
                        white_prisoners = analysis_saved_white_prisoners;
                        turn_is_black = 1;  // Reset to black when exiting analysis
                        set_cursor_visible(1);  // Update cursor when exiting analysis mode
                    } else {
                        analysis_mode = 1;
                        analysis_dragging = 0;
                        // Clear liberty display when entering analysis mode
                        liberty_count = 0;
                        // Save current prisoner counts before entering analysis mode
                        analysis_saved_black_prisoners = black_prisoners;
                        analysis_saved_white_prisoners = white_prisoners;
                        turn_is_black = 1;  // Always start with Black in analysis mode
                        set_cursor_visible(1);  // Update cursor when entering analysis mode
                    }
                    draw_board();
                } else if (key == SDLK_u) {
                    chain_mode = !chain_mode;
                    draw_board();
                } else if (key == SDLK_g) {
                    if (guess_mode) {
                        guess_mode = 0;
                        guess_dragging = 0;
                        guess_pending = 0;
                        // Clear liberty display when exiting guess mode
                        liberty_count = 0;
                    } else {
                        if (analysis_mode) {
                            analysis_mode = 0;
                            clear_analysis_stones();
                            turn_is_black = 1;  // Reset to black when exiting analysis
                        }
                        guess_mode = 1;
                        guess_dragging = 0;
                        guess_pending = 0;
                        // Clear liberty display when entering guess mode
                        liberty_count = 0;
                    }
                    guess_score = 0;
                    draw_board();
                } else if (key == SDLK_SPACE) {
                    if (!analysis_mode && !guess_mode) {
                        paused = !paused;
                        last_move_tick = SDL_GetTicks();
                        draw_board();
                    }
                } else if (!analysis_mode && !guess_mode && key == SDLK_LEFT) {
                    if (index > 0) {
                        paused = 1; // Pause when stepping back
                        index--;
                        restore_board_state(index);
                        draw_board();
                    }
                } else if (!analysis_mode && !guess_mode && key == SDLK_RIGHT) {
                    if (index < move_count) {
                        int r, f;
                        if (parse_sgf_move(&moves[index][0], &r, &f)) {
                            if (animate_move(r, f, turn_is_black)) {
                                quit = 1;
                                break;
                            }
                            index++;
                            turn_is_black = (index % 2 == 0);
                        }
                    }
                }
            } else if (analysis_mode && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                BoardView view;
                get_board_view(&view);
                int r = -1;
                int f = -1;
                if (screen_to_board(&view, e.button.x, e.button.y, &r, &f)) {
                    if (board[r][f] != 0) {
                        // Click on existing stone - toggle liberties of the chain
                        if (is_stone_in_selected_group(r, f)) {
                            // Toggle off - clicked on a stone in the selected group
                            selected_group_count = 0;
                            liberty_count = 0;
                        } else {
                            // Show liberties of new group
                            store_selected_group(r, f);
                            calculate_chain_liberties(r, f);
                        }
                    } else {
                        // Check keyboard state for forced stone color
                        const Uint8 *keystate = SDL_GetKeyboardState(NULL);
                        int forced_color = -1; // -1 means alternate normally
                        if (keystate[SDL_SCANCODE_B]) {
                            forced_color = 1; // Force black
                        } else if (keystate[SDL_SCANCODE_W]) {
                            forced_color = 0; // Force white
                        }

                        int stone_color = (forced_color != -1) ? forced_color : analysis_turn_is_black;

                        // Check if move is allowed in analysis mode
                        // Save current state
                        char saved_board[BOARD_SIZE][BOARD_SIZE];
                        memcpy(saved_board, board, sizeof(board));
                        Stone saved_stones[MAX_MOVES];
                        memcpy(saved_stones, stones, sizeof(stones));
                        int saved_stone_count = stone_count;
                        Stone saved_analysis_stones[MAX_MOVES];
                        memcpy(saved_analysis_stones, analysis_stones, sizeof(analysis_stones));
                        int saved_analysis_stone_count = analysis_stone_count;
                        int saved_black_prisoners = black_prisoners;
                        int saved_white_prisoners = white_prisoners;

                        // Temporarily place the stone
                        board[r][f] = (char)(stone_color ? 1 : 2);

                        // Remove captured opponent stones (skip the placed stone's color)
                        remove_captured_stones(stone_color, -1, -1, stone_color);

                        // Check if the placed stone has liberties
                        int visited[BOARD_SIZE][BOARD_SIZE] = {0};
                        int has_libs = has_liberties(r, f, stone_color, visited);

                        // Count captured stones
                        int captured_count = (stone_color ? black_prisoners - saved_black_prisoners : white_prisoners - saved_white_prisoners);

                        // Allow if has liberties OR (no liberties but captured something)
                        int allow_move = has_libs || (!has_libs && captured_count > 0);

                        // Restore state
                        memcpy(board, saved_board, sizeof(board));
                        memcpy(stones, saved_stones, sizeof(stones));
                        stone_count = saved_stone_count;
                        memcpy(analysis_stones, saved_analysis_stones, sizeof(analysis_stones));
                        analysis_stone_count = saved_analysis_stone_count;
                        black_prisoners = saved_black_prisoners;
                        white_prisoners = saved_white_prisoners;

                        if (allow_move) {
                            // Place the stone
                            add_analysis_stone(r, f, stone_color);
                            // Remove captured stones
                            remove_captured_stones(stone_color, -1, -1, stone_color);
                        }
                        if (forced_color == -1) {
                            // Only alternate if not forcing a specific color
                            analysis_turn_is_black = !analysis_turn_is_black;
                        }
                        // Clear liberty display when placing a new stone
                        liberty_count = 0;
                        liberty_display_r = -1;
                        liberty_display_f = -1;
                    }
                }
            } else if (analysis_mode && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
                BoardView view;
                get_board_view(&view);
                int r = -1;
                int f = -1;
                if (screen_to_board(&view, e.button.x, e.button.y, &r, &f)) {
                    if (board[r][f] != 0) {
                        // Remove the stone
                        board[r][f] = 0;
                        // Remove from stones array
                        for (int j = 0; j < stone_count; j++) {
                            if (stones[j].r == r && stones[j].f == f) {
                                // Shift remaining stones
                                for (int k = j; k < stone_count - 1; k++) {
                                    stones[k] = stones[k + 1];
                                }
                                stone_count--;
                                break;
                            }
                        }
                        // Also check analysis stones
                        for (int j = 0; j < analysis_stone_count; j++) {
                            if (analysis_stones[j].r == r && analysis_stones[j].f == f) {
                                // Shift remaining stones
                                for (int k = j; k < analysis_stone_count - 1; k++) {
                                    analysis_stones[k] = analysis_stones[k + 1];
                                }
                                analysis_stone_count--;
                                break;
                            }
                        }
                        // Clear liberty display when removing a stone
                        liberty_count = 0;
                        liberty_display_r = -1;
                        liberty_display_f = -1;
                    }
                }
            } else if (!analysis_mode && !guess_mode && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                BoardView view;
                get_board_view(&view);
                int r = -1;
                int f = -1;
                if (screen_to_board(&view, e.button.x, e.button.y, &r, &f)) {
                    if (board[r][f] != 0) {
                        // Click on existing stone - toggle liberties of the chain
                        if (is_stone_in_selected_group(r, f)) {
                            // Toggle off - clicked on a stone in the selected group
                            selected_group_count = 0;
                            liberty_count = 0;
                        } else {
                            // Show liberties of new group
                            store_selected_group(r, f);
                            calculate_chain_liberties(r, f);
                        }
                        draw_board();
                    }
                }
            } else if (guess_mode && !analysis_mode && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                BoardView view;
                get_board_view(&view);
                if (screen_to_board(&view, e.button.x, e.button.y, &guess_to_r, &guess_to_f)) {
                    if (board[guess_to_r][guess_to_f] == 0) {
                        guess_pending = 1;
                    }
                }
            }
        }
        if (quit) break;

        if (guess_pending && index < move_count) {
            int expected_r, expected_f;
            if (parse_sgf_move(moves[index], &expected_r, &expected_f)) {
                if (expected_r == guess_to_r && expected_f == guess_to_f) {
                    guess_score++;
                } else {
                    guess_score--;
                }
                if (animate_move(expected_r, expected_f, turn_is_black)) {
                    quit = 1;
                    break;
                }
                index++;
                turn_is_black = (index % 2 == 0);
                last_move_tick = SDL_GetTicks();
            }
            guess_pending = 0;
        }

        if (analysis_mode) {
            BoardView view;
            get_board_view(&view);
            render_board(&view, NULL);
            SDL_Delay(10);
            continue;
        }

        if (guess_mode) {
            BoardView view;
            get_board_view(&view);
            render_board(&view, NULL);
            SDL_Delay(10);
            continue;
        }

        if (!paused && index < move_count) {
            Uint32 now = SDL_GetTicks();
            if (now - last_move_tick >= (Uint32)move_delay_ms) {
                int r, f;
                if (parse_sgf_move(moves[index], &r, &f)) {
                    if (animate_move(r, f, turn_is_black)) {
                        quit = 1;
                        break;
                    }
                    index++;
                    turn_is_black = (index % 2 == 0);
                } else {
                    printf("Failed to parse move: %s\n", moves[index]);
                }
                last_move_tick = now;
            }
        } else if (index >= move_count) {
            game_finished = 1;
            // Game finished - start timer for automatic next game
            if (game_finished_timer == 0) {
                game_finished_timer = SDL_GetTicks();
            }
            // Render the final board state with result message
            draw_board();
            // Automatically advance to next game after GAME_OVER_PAUSE_MS
            if (SDL_GetTicks() - game_finished_timer >= GAME_OVER_PAUSE_MS) {
                game_nav_request = GAME_NAV_NEXT;
                quit = 1;
            }
        }

        SDL_Delay(10);
    }

    int pause_ms = GAME_OVER_PAUSE_MS;
    Uint32 pause_start = SDL_GetTicks();
    while (!quit) {
        Uint32 now = SDL_GetTicks();
        update_cursor_auto_hide(now);
        if (now - pause_start >= (Uint32)pause_ms) {
            break;
        }

        // Render the final board state with result message
        BoardView view;
        get_board_view(&view);
        render_board(&view, NULL);
        render_result_message(&view);
        SDL_RenderPresent(renderer);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            note_mouse_activity_event(&e);
            if (handle_catalog_event(&e, games_dir_root)) {
                draw_board();
                if (game_nav_request == GAME_NAV_SELECT && catalog_selection_made) {
                    catalog_selection_made = 0;
                    quit = 1;
                }
                continue;
            }
            if (e.type == SDL_QUIT) {
                quit = 1;
            } else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode key = e.key.keysym.sym;
                if (key == SDLK_q) {
                    game_nav_request = GAME_NAV_NONE;
                    quit = 1;
                } else if (key == SDLK_n) {
                    game_nav_request = GAME_NAV_NEXT;
                    quit = 1;
                } else if (key == SDLK_p) {
                    game_nav_request = GAME_NAV_PREV;
                    quit = 1;
                } else if (key == SDLK_r) {
                    game_nav_request = GAME_NAV_RESTART;
                    quit = 1;
                } else if (key == SDLK_c) {
                    catalog_open(games_dir_root);
                    draw_board();
                } else if (key == SDLK_ESCAPE) {
                    show_help = !show_help;
                    draw_board();
                } else if (key == SDLK_SPACE) {
                    if (!analysis_mode && !guess_mode) {
                        paused = !paused;
                        draw_board();
                    }
                } else if (key == SDLK_UP || key == SDLK_DOWN) {
                    Uint32 tick_now = SDL_GetTicks();
                    int delta = (key == SDLK_UP) ? MOVE_DELAY_STEP_MS : -MOVE_DELAY_STEP_MS;
                    adjust_move_delay(delta, tick_now);
                }
            }
        }
        SDL_Delay(10);
    }

    return quit;
}

char *copy_string(const char *s) {
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len + 1);
    return out;
}

int has_sgf_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    if (dot[1] == '\0' || dot[2] == '\0' || dot[3] == '\0' || dot[4] != '\0') return 0;
    return (tolower((unsigned char)dot[1]) == 's' &&
            tolower((unsigned char)dot[2]) == 'g' &&
            tolower((unsigned char)dot[3]) == 'f');
}

char *join_path(const char *dir, const char *name) {
    size_t dir_len = strlen(dir);
    int need_sep = (dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\');
    size_t len = dir_len + (need_sep ? 1 : 0) + strlen(name) + 1;
    char *out = (char *)malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s%s%s", dir, need_sep ? PATH_SEP_STR : "", name);
    return out;
}

static int relpath_from_base(const char *base, const char *path, char *out, size_t out_size) {
    size_t base_len = strlen(base);
    const char *p = path;
    if (strncmp(path, base, base_len) == 0) {
        p = path + base_len;
        if (*p == '\\' || *p == '/') p++;
    }
    if (strlen(p) + 1 > out_size) return 0;
    strcpy(out, p);
    return 1;
}

int list_sgf_files(const char *dir, char ***out_files) {
    char **files = NULL;
    int count = 0;
    int cap = 0;
    if (list_sgf_files_recursive(dir, dir, &files, &count, &cap) < 0) {
        free_string_list(files, count);
        return -1;
    }
    *out_files = files;
    return count;
}

static int list_sgf_files_recursive(const char *dir, const char *base,
                                    char ***out_files, int *count, int *cap) {
#ifdef _WIN32
    char *search = join_path(dir, "*");
    if (!search) return -1;
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(search, &data);
    free(search);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        char *full = join_path(dir, data.cFileName);
        if (!full) {
            FindClose(h);
            return -1;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (list_sgf_files_recursive(full, base, out_files, count, cap) < 0) {
                free(full);
                FindClose(h);
                return -1;
            }
        } else if (has_sgf_extension(data.cFileName)) {
            char relbuf[1024];
            if (!relpath_from_base(base, full, relbuf, sizeof(relbuf))) {
                free(full);
                FindClose(h);
                return -1;
            }
            if (!push_string(out_files, count, cap, relbuf)) {
                free(full);
                FindClose(h);
                return -1;
            }
        }
        free(full);
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char *full = join_path(dir, ent->d_name);
        if (!full) {
            closedir(d);
            return -1;
        }
        int is_dir = 0;
#ifdef DT_DIR
        if (ent->d_type == DT_DIR) is_dir = 1;
        if (ent->d_type == DT_UNKNOWN)
#endif
        {
            DIR *probe = opendir(full);
            if (probe) {
                is_dir = 1;
                closedir(probe);
            }
        }
        if (is_dir) {
            if (list_sgf_files_recursive(full, base, out_files, count, cap) < 0) {
                free(full);
                closedir(d);
                return -1;
            }
        } else if (has_sgf_extension(ent->d_name)) {
            char relbuf[1024];
            if (!relpath_from_base(base, full, relbuf, sizeof(relbuf))) {
                free(full);
                closedir(d);
                return -1;
            }
            if (!push_string(out_files, count, cap, relbuf)) {
                free(full);
                closedir(d);
                return -1;
            }
        }
        free(full);
    }
    closedir(d);
#endif
    return 0;
}



void free_string_list(char **items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

SDL_Cursor *create_analysis_cursor(void) {
    const int size = 64;
    const int center = size / 2;
    const int thickness = 7;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return NULL;

    Uint32 transparent = SDL_MapRGBA(surface->format, 0, 0, 0, 0);
    Uint32 white = SDL_MapRGBA(surface->format, 255, 255, 255, 255);

    if (SDL_LockSurface(surface) != 0) {
        SDL_FreeSurface(surface);
        return NULL;
    }

    Uint32 *pixels = (Uint32 *)surface->pixels;
    int pitch = surface->pitch / 4;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            pixels[y * pitch + x] = transparent;
        }
    }

    int half = thickness / 2;
    // Draw horizontal white line
    for (int dx = -half; dx <= half; dx++) {
        int x = center + dx;
        if (x >= 0 && x < size) pixels[center * pitch + x] = white;
    }
    // Draw vertical white line
    for (int dy = -half; dy <= half; dy++) {
        int y = center + dy;
        if (y >= 0 && y < size) pixels[y * pitch + center] = white;
    }

    SDL_UnlockSurface(surface);
    SDL_Cursor *cursor = SDL_CreateColorCursor(surface, center, center);
    SDL_FreeSurface(surface);
    return cursor;
}

void set_cursor_visible(int visible) {
    // Always use the custom analysis cursor
    if (analysis_cursor) {
        SDL_SetCursor(analysis_cursor);
    }
    SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
    cursor_visible = visible;
}

void note_mouse_activity(Uint32 now) {
    last_mouse_activity = now;
    if (!cursor_visible) set_cursor_visible(1);
}

void update_cursor_auto_hide(Uint32 now) {
    // Cursor is always visible now - no auto-hide
    if (!cursor_visible) {
        set_cursor_visible(1);
    }
}

void note_mouse_activity_event(const SDL_Event *e) {
    if (e->type == SDL_MOUSEMOTION ||
        e->type == SDL_MOUSEBUTTONDOWN ||
        e->type == SDL_MOUSEBUTTONUP ||
        e->type == SDL_MOUSEWHEEL) {
        note_mouse_activity(SDL_GetTicks());
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    games_dir_root = DEFAULT_GAMES_DIR;

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL init error: %s\n", SDL_GetError());
        return 1;
    }

    // Enable bilinear texture filtering for smoother stone rendering
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    window = SDL_CreateWindow("Go Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              SCREEN_SIZE, SCREEN_SIZE, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window || !renderer) {
        printf("SDL window/renderer error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Stone textures are now rendered as circles, no PNG files needed

    analysis_cursor = create_analysis_cursor();
    if (!analysis_cursor) {
        analysis_cursor = SDL_GetDefaultCursor();
    }
    set_cursor_visible(1);
    note_mouse_activity(SDL_GetTicks());

    srand((unsigned int)time(NULL));

    int quit = 0;
    while (!quit) {
        char **files = NULL;
        int file_count;
        const char *selected_dir;
        int file_index;

        if (sequential_dir[0] != '\0') {
            // Sequential mode: play games from the selected directory
            file_count = list_sgf_files(sequential_dir, &files);
            selected_dir = sequential_dir;
            if (file_count <= 0) {
                // No files in sequential directory, fall back to random
                sequential_dir[0] = '\0';
                sequential_index = 0;
                file_count = list_sgf_files(games_dir_root, &files);
                selected_dir = games_dir_root;
                file_index = rand() % file_count;
            } else {
                file_index = sequential_index % file_count;
                sequential_index++;
            }
        } else {
            // Random mode: play random games from entire games directory
            file_count = list_sgf_files(games_dir_root, &files);
            selected_dir = games_dir_root;
            file_index = rand() % file_count;
        }

        if (file_count <= 0) {
            printf("No SGF files found in %s\n", selected_dir);
            break;
        }

        char *path = join_path(selected_dir, files[file_index]);
        free_string_list(files, file_count);
        if (!path) {
            printf("Failed to create path\n");
            break;
        }

        char moves[MAX_MOVES][MOVE_TEXT_LEN];
        char result[RESULT_LEN];
        int move_count = load_sgf_game(path, moves, MAX_MOVES,
                                       current_black_name, sizeof(current_black_name),
                                       current_white_name, sizeof(current_white_name),
                                       result, sizeof(result));
        free(path);

        if (move_count <= 0) {
            printf("Failed to load game\n");
            SDL_Delay(500);
            continue;
        }

        int stop = play_game(moves, move_count, result);
        if (stop && game_nav_request == GAME_NAV_NONE) {
            quit = 1;
            break;
        }
        if (game_nav_request == GAME_NAV_SELECT) {
            // Handle catalog selection - load the selected game
            if (forced_sgf_path) {
                char moves[MAX_MOVES][MOVE_TEXT_LEN];
                char result[RESULT_LEN];
                int move_count = load_sgf_game(forced_sgf_path, moves, MAX_MOVES,
                                               current_black_name, sizeof(current_black_name),
                                               current_white_name, sizeof(current_white_name),
                                               result, sizeof(result));
                free(forced_sgf_path);
                forced_sgf_path = NULL;

                if (move_count > 0) {
                    int stop = play_game(moves, move_count, result);
                    if (stop && game_nav_request == GAME_NAV_NONE) {
                        quit = 1;
                        break;
                    }
                    if (game_nav_request == GAME_NAV_SELECT) {
                        // Another catalog selection - continue the loop
                        continue;
                    }
                }
            }
            // If loading failed or no forced path, continue with random game
            game_nav_request = GAME_NAV_NONE;
            continue;
        }
        // Continue with next random game
    }

    // Cleanup
    if (analysis_cursor) {
        SDL_FreeCursor(analysis_cursor);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
