#pragma once
#include "go_viewer.hpp"
#include "go_rules.hpp"

// Owns the authoritative game board and its undo history.
// Never touched while an AnalysisState is active.
class GameState {
public:
    char  board[BOARD_SIZE][BOARD_SIZE] = {};
    Stone stones[MAX_MOVES]             = {};
    int   stone_count                   = 0;
    int   black_prisoners               = 0;
    int   white_prisoners               = 0;
    int   turn_is_black                 = 1;
    int   game_finished                 = 0;
    Uint32 game_finished_timer          = 0;

    // Board history for left/right stepping
    GameSnapshot history[MAX_MOVES];
    int          history_count = 0;

    // Liberty display state (playback mode stone-click)
    int liberty_r[BOARD_SIZE * BOARD_SIZE] = {};
    int liberty_f[BOARD_SIZE * BOARD_SIZE] = {};
    int liberty_count                      = 0;
    int liberty_display_r                  = -1;
    int liberty_display_f                  = -1;
    int selected_group_stones[BOARD_SIZE * BOARD_SIZE][2] = {};
    int selected_group_count               = 0;

    void reset();

    // Place a stone, run Go capture rules, update prisoners.
    // Returns false if the position is occupied or the move is suicide.
    bool place_stone(int r, int f, int is_black);

    void save_snapshot();
    void restore_snapshot(int index);  // also truncates history after index

    // Returns a deep copy of the current board state (for AnalysisState construction).
    GameSnapshot take_snapshot() const;

    // Liberty/group display helpers
    void calculate_chain_liberties(int r, int f);
    void store_selected_group(int r, int f);
    bool is_stone_in_selected_group(int r, int f) const;
};
