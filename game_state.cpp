#include "game_state.hpp"
#include <cstring>

void GameState::reset() {
    memset(board, 0, sizeof(board));
    stone_count          = 0;
    history_count        = 0;
    black_prisoners      = 0;
    white_prisoners      = 0;
    game_finished        = 0;
    game_finished_timer  = 0;
    liberty_count        = 0;
    liberty_display_r    = -1;
    liberty_display_f    = -1;
    selected_group_count = 0;
}

bool GameState::place_stone(int r, int f, int is_black) {
    if (r < 0 || r >= BOARD_SIZE || f < 0 || f >= BOARD_SIZE) return false;  // regular games always 19×19
    if (board[r][f] != 0) return false;
    if (GoRules::would_be_suicide(board, r, f, is_black)) return false;

    board[r][f] = is_black ? 1 : 2;
    stones[stone_count++] = {r, f, is_black};

    int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_count = 0;
    GoRules::find_captured(board, is_black, r, f, cap_r, cap_f, cap_count);

    for (int i = 0; i < cap_count; i++) {
        board[cap_r[i]][cap_f[i]] = 0;
        for (int j = 0; j < stone_count; j++) {
            if (stones[j].r == cap_r[i] && stones[j].f == cap_f[i]) {
                memmove(&stones[j], &stones[j + 1], (stone_count - j - 1) * sizeof(Stone));
                stone_count--;
                break;
            }
        }
    }

    if (is_black) black_prisoners += cap_count;
    else          white_prisoners += cap_count;

    return true;
}

void GameState::save_snapshot() {
    if (history_count >= MAX_MOVES) return;
    GameSnapshot& s = history[history_count++];
    memcpy(s.board, board, sizeof(board));
    memcpy(s.stones, stones, stone_count * sizeof(Stone));
    s.stone_count     = stone_count;
    s.black_prisoners = black_prisoners;
    s.white_prisoners = white_prisoners;
    s.turn_is_black   = turn_is_black;
}

void GameState::restore_snapshot(int index) {
    if (index < 0 || index >= history_count) return;
    const GameSnapshot& s = history[index];
    memcpy(board, s.board, sizeof(board));
    memcpy(stones, s.stones, s.stone_count * sizeof(Stone));
    stone_count          = s.stone_count;
    black_prisoners      = s.black_prisoners;
    white_prisoners      = s.white_prisoners;
    turn_is_black        = s.turn_is_black;
    history_count        = index + 1;
    liberty_count        = 0;
    selected_group_count = 0;
    liberty_display_r    = -1;
    liberty_display_f    = -1;
}

GameSnapshot GameState::take_snapshot() const {
    GameSnapshot s;
    memcpy(s.board, board, sizeof(board));
    memcpy(s.stones, stones, stone_count * sizeof(Stone));
    s.stone_count     = stone_count;
    s.black_prisoners = black_prisoners;
    s.white_prisoners = white_prisoners;
    s.turn_is_black   = turn_is_black;
    return s;
}

void GameState::calculate_chain_liberties(int r, int f) {
    liberty_count = 0;
    if (board[r][f] == 0) return;
    int color = (board[r][f] == 1) ? 1 : 0;
    int visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    GoRules::get_liberties(board, r, f, color, visited, liberty_r, liberty_f, liberty_count);
}

void GameState::store_selected_group(int r, int f) {
    if (board[r][f] == 0) { selected_group_count = 0; return; }
    int color = (board[r][f] == 1) ? 1 : 0;
    int visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    int gr[MAX_BOARD_SIZE * MAX_BOARD_SIZE], gf[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
    int gc = 0;
    GoRules::get_group(board, r, f, color, visited, &gc, gr, gf);
    selected_group_count = gc;
    for (int i = 0; i < gc; i++) {
        selected_group_stones[i][0] = gr[i];
        selected_group_stones[i][1] = gf[i];
    }
}

bool GameState::is_stone_in_selected_group(int r, int f) const {
    for (int i = 0; i < selected_group_count; i++)
        if (selected_group_stones[i][0] == r && selected_group_stones[i][1] == f)
            return true;
    return false;
}
