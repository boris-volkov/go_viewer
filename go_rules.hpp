#pragma once
#include "go_viewer.hpp"

// Pure Go rules engine. All methods operate on a board passed in — no global state.
// board[r][f]: 0 = empty, 1 = black, 2 = white
// n = board size (defaults to BOARD_SIZE = 19 for standard games)
struct GoRules {
    static void get_group(const char board[][MAX_BOARD_SIZE], int r, int f, int color,
                          int visited[][MAX_BOARD_SIZE],
                          int* count, int gr[], int gf[], int n = BOARD_SIZE);

    static bool has_liberties(const char board[][MAX_BOARD_SIZE], int r, int f, int color,
                              int visited[][MAX_BOARD_SIZE], int n = BOARD_SIZE);

    static void get_liberties(const char board[][MAX_BOARD_SIZE], int r, int f, int color,
                              int visited[][MAX_BOARD_SIZE],
                              int lib_r[], int lib_f[], int& lib_count, int n = BOARD_SIZE);

    static bool is_surrounded(const char board[][MAX_BOARD_SIZE], int r, int f, int color,
                              int n = BOARD_SIZE);

    static bool would_be_suicide(const char board[][MAX_BOARD_SIZE], int r, int f, int color,
                                 int n = BOARD_SIZE);

    static void find_captured(const char board[][MAX_BOARD_SIZE],
                              int placed_color, int placed_r, int placed_f,
                              int cap_r[], int cap_f[], int& cap_count, int n = BOARD_SIZE);
};
