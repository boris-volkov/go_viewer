#pragma once
#include "go_viewer.hpp"
#include "go_rules.hpp"

class AnalysisState {
public:
    char  board[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    Stone analysis_stones[MAX_MOVES]   = {};
    int   analysis_stone_count = 0;
    int   turn_is_black;
    int   black_prisoners;
    int   white_prisoners;
    int   board_size = BOARD_SIZE;  // active grid size; settable for free mode

    int liberty_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE] = {};
    int liberty_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE] = {};
    int liberty_count                              = 0;
    int liberty_display_r                          = -1;
    int liberty_display_f                          = -1;
    int selected_group_stones[MAX_BOARD_SIZE * MAX_BOARD_SIZE][2] = {};
    int selected_group_count                       = 0;

    explicit AnalysisState(const GameSnapshot& base, int board_size = BOARD_SIZE);

    void reset_to_base();
    bool place_stone(int r, int f, int is_black);
    bool remove_stone_at(int r, int f);

    void calculate_chain_liberties(int r, int f);
    void store_selected_group(int r, int f);
    bool is_stone_in_selected_group(int r, int f) const;

private:
    GameSnapshot base_snapshot;
    void remove_from_analysis_list(int r, int f);
};
