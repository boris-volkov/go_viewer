#include "analysis_state.hpp"
#include <cstring>

AnalysisState::AnalysisState(const GameSnapshot& base) {
    memcpy(board, base.board, sizeof(board));
    turn_is_black        = base.turn_is_black;
    black_prisoners      = base.black_prisoners;
    white_prisoners      = base.white_prisoners;
    analysis_stone_count = 0;
    liberty_count        = 0;
    selected_group_count = 0;
    liberty_display_r    = -1;
    liberty_display_f    = -1;
}

bool AnalysisState::place_stone(int r, int f, int is_black) {
    if (r < 0 || r >= BOARD_SIZE || f < 0 || f >= BOARD_SIZE) return false;
    if (board[r][f] != 0) return false;
    if (GoRules::would_be_suicide(board, r, f, is_black)) return false;

    board[r][f] = is_black ? 1 : 2;
    if (analysis_stone_count < MAX_MOVES)
        analysis_stones[analysis_stone_count++] = {r, f, is_black};

    int cap_r[BOARD_SIZE * BOARD_SIZE], cap_f[BOARD_SIZE * BOARD_SIZE], cap_count = 0;
    GoRules::find_captured(board, is_black, r, f, cap_r, cap_f, cap_count);

    for (int i = 0; i < cap_count; i++) {
        board[cap_r[i]][cap_f[i]] = 0;
        remove_from_analysis_list(cap_r[i], cap_f[i]);
    }

    if (is_black) black_prisoners += cap_count;
    else          white_prisoners += cap_count;

    return true;
}

bool AnalysisState::remove_stone_at(int r, int f) {
    if (r < 0 || r >= BOARD_SIZE || f < 0 || f >= BOARD_SIZE) return false;
    if (board[r][f] == 0) return false;
    board[r][f] = 0;
    remove_from_analysis_list(r, f);
    return true;
}

void AnalysisState::remove_from_analysis_list(int r, int f) {
    for (int j = 0; j < analysis_stone_count; j++) {
        if (analysis_stones[j].r == r && analysis_stones[j].f == f) {
            memmove(&analysis_stones[j], &analysis_stones[j + 1],
                    (analysis_stone_count - j - 1) * sizeof(Stone));
            analysis_stone_count--;
            return;
        }
    }
}

void AnalysisState::calculate_chain_liberties(int r, int f) {
    liberty_count = 0;
    if (board[r][f] == 0) return;
    int color = (board[r][f] == 1) ? 1 : 0;
    int visited[BOARD_SIZE][BOARD_SIZE] = {};
    GoRules::get_liberties(board, r, f, color, visited, liberty_r, liberty_f, liberty_count);
}

void AnalysisState::store_selected_group(int r, int f) {
    if (board[r][f] == 0) { selected_group_count = 0; return; }
    int color = (board[r][f] == 1) ? 1 : 0;
    int visited[BOARD_SIZE][BOARD_SIZE] = {};
    int gr[BOARD_SIZE * BOARD_SIZE], gf[BOARD_SIZE * BOARD_SIZE];
    int gc = 0;
    GoRules::get_group(board, r, f, color, visited, &gc, gr, gf);
    selected_group_count = gc;
    for (int i = 0; i < gc; i++) {
        selected_group_stones[i][0] = gr[i];
        selected_group_stones[i][1] = gf[i];
    }
}

bool AnalysisState::is_stone_in_selected_group(int r, int f) const {
    for (int i = 0; i < selected_group_count; i++)
        if (selected_group_stones[i][0] == r && selected_group_stones[i][1] == f)
            return true;
    return false;
}
