#pragma once
#include "go_viewer.hpp"

// Pure Go rules engine. All methods operate on a board passed in — no global state.
// board[r][f]: 0 = empty, 1 = black, 2 = white
struct GoRules {
    // Flood-fill to find all stones in the connected group containing (r,f).
    // Results written into gr[]/gf[], count into *count.
    static void get_group(const char board[][BOARD_SIZE], int r, int f, int color,
                          int visited[][BOARD_SIZE],
                          int* count, int gr[], int gf[]);

    // Returns true if the group at (r,f) has at least one empty adjacent intersection.
    static bool has_liberties(const char board[][BOARD_SIZE], int r, int f, int color,
                              int visited[][BOARD_SIZE]);

    // Appends all liberty positions for the group at (r,f) into lib_r[]/lib_f[].
    // lib_count is incremented; caller initialises it to 0 before the first call.
    static void get_liberties(const char board[][BOARD_SIZE], int r, int f, int color,
                              int visited[][BOARD_SIZE],
                              int lib_r[], int lib_f[], int& lib_count);

    static bool is_surrounded(const char board[][BOARD_SIZE], int r, int f, int color);

    // True if placing 'color' at (r,f) would be an illegal suicide move.
    static bool would_be_suicide(const char board[][BOARD_SIZE], int r, int f, int color);

    // After placing placed_color at (placed_r,placed_f) on board, identify all opponent
    // stones that now have no liberties.  The captured positions are written into
    // cap_r[]/cap_f[] and cap_count is set.  The board is NOT modified here; callers
    // must remove the stones themselves.
    static void find_captured(const char board[][BOARD_SIZE],
                              int placed_color, int placed_r, int placed_f,
                              int cap_r[], int cap_f[], int& cap_count);
};
