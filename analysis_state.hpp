#pragma once
#include "go_viewer.hpp"
#include "go_rules.hpp"

// Owns an isolated copy of the board for experimental play.
// Constructed from a GameSnapshot — GameState is never touched again until
// the AnalysisState is destroyed (by resetting the owning unique_ptr).
class AnalysisState {
public:
    char  board[BOARD_SIZE][BOARD_SIZE]; // working board: snapshot + analysis moves
    Stone analysis_stones[MAX_MOVES];   // only stones placed this analysis session
    int   analysis_stone_count = 0;
    int   turn_is_black;
    int   black_prisoners;  // starts as snapshot value, accumulates analysis captures
    int   white_prisoners;

    // Liberty/group display
    int liberty_r[BOARD_SIZE * BOARD_SIZE] = {};
    int liberty_f[BOARD_SIZE * BOARD_SIZE] = {};
    int liberty_count                      = 0;
    int liberty_display_r                  = -1;
    int liberty_display_f                  = -1;
    int selected_group_stones[BOARD_SIZE * BOARD_SIZE][2] = {};
    int selected_group_count               = 0;

    explicit AnalysisState(const GameSnapshot& base);

    // Try to place a stone (respects go rules). Returns true if successful.
    bool place_stone(int r, int f, int is_black);

    // Right-click remove: removes any stone at (r,f) from board.
    // Returns true if a stone was there.
    bool remove_stone_at(int r, int f);

    void calculate_chain_liberties(int r, int f);
    void store_selected_group(int r, int f);
    bool is_stone_in_selected_group(int r, int f) const;

private:
    void remove_from_analysis_list(int r, int f);
};
