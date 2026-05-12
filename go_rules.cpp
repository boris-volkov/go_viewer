#include "go_rules.hpp"
#include <cstring>

static constexpr int DIRS[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};

// ---------------------------------------------------------------------------
void GoRules::get_group(const char board[][BOARD_SIZE], int r, int f, int color,
                        int visited[][BOARD_SIZE],
                        int* count, int gr[], int gf[]) {
    *count = 0;
    int stack[BOARD_SIZE * BOARD_SIZE][2];
    int top = 0;
    if (r < 0 || r >= BOARD_SIZE || f < 0 || f >= BOARD_SIZE ||
        visited[r][f] || board[r][f] != (color ? 1 : 2))
        return;
    stack[top][0] = r; stack[top][1] = f; top++;
    while (top > 0) {
        top--;
        int cr = stack[top][0], cf = stack[top][1];
        if (visited[cr][cf]) continue;
        visited[cr][cf] = 1;
        gr[*count] = cr; gf[*count] = cf; (*count)++;
        for (auto& d : DIRS) {
            int nr = cr + d[0], nf = cf + d[1];
            if (nr >= 0 && nr < BOARD_SIZE && nf >= 0 && nf < BOARD_SIZE &&
                !visited[nr][nf] && board[nr][nf] == (color ? 1 : 2)) {
                stack[top][0] = nr; stack[top][1] = nf; top++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
bool GoRules::has_liberties(const char board[][BOARD_SIZE], int r, int f, int color,
                            int visited[][BOARD_SIZE]) {
    int stack[BOARD_SIZE * BOARD_SIZE][2];
    int top = 0;
    if (r < 0 || r >= BOARD_SIZE || f < 0 || f >= BOARD_SIZE ||
        visited[r][f] || board[r][f] != (color ? 1 : 2))
        return false;
    stack[top][0] = r; stack[top][1] = f; top++;
    while (top > 0) {
        top--;
        int cr = stack[top][0], cf = stack[top][1];
        if (visited[cr][cf]) continue;
        visited[cr][cf] = 1;
        for (auto& d : DIRS) {
            int nr = cr + d[0], nf = cf + d[1];
            if (nr < 0 || nr >= BOARD_SIZE || nf < 0 || nf >= BOARD_SIZE) continue;
            if (board[nr][nf] == 0) return true;
            if (board[nr][nf] == (color ? 1 : 2) && !visited[nr][nf]) {
                stack[top][0] = nr; stack[top][1] = nf; top++;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
void GoRules::get_liberties(const char board[][BOARD_SIZE], int r, int f, int color,
                            int visited[][BOARD_SIZE],
                            int lib_r[], int lib_f[], int& lib_count) {
    int stack[BOARD_SIZE * BOARD_SIZE][2];
    int top = 0;
    if (r < 0 || r >= BOARD_SIZE || f < 0 || f >= BOARD_SIZE ||
        visited[r][f] || board[r][f] != (color ? 1 : 2))
        return;
    stack[top][0] = r; stack[top][1] = f; top++;
    while (top > 0) {
        top--;
        int cr = stack[top][0], cf = stack[top][1];
        if (visited[cr][cf]) continue;
        visited[cr][cf] = 1;
        for (auto& d : DIRS) {
            int nr = cr + d[0], nf = cf + d[1];
            if (nr < 0 || nr >= BOARD_SIZE || nf < 0 || nf >= BOARD_SIZE) continue;
            if (board[nr][nf] == 0) {
                bool dup = false;
                for (int i = 0; i < lib_count; i++) {
                    if (lib_r[i] == nr && lib_f[i] == nf) { dup = true; break; }
                }
                if (!dup && lib_count < BOARD_SIZE * BOARD_SIZE) {
                    lib_r[lib_count] = nr; lib_f[lib_count] = nf; lib_count++;
                }
            } else if (board[nr][nf] == (color ? 1 : 2) && !visited[nr][nf]) {
                stack[top][0] = nr; stack[top][1] = nf; top++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
bool GoRules::is_surrounded(const char board[][BOARD_SIZE], int r, int f, int color) {
    int visited[BOARD_SIZE][BOARD_SIZE] = {};
    return !has_liberties(board, r, f, color, visited);
}

// ---------------------------------------------------------------------------
bool GoRules::would_be_suicide(const char board[][BOARD_SIZE], int r, int f, int color) {
    // Temporarily place the stone, simulate opponent captures, then check liberties.
    char test[BOARD_SIZE][BOARD_SIZE];
    memcpy(test, board, sizeof(test));
    test[r][f] = color ? 1 : 2;

    int opp = color ? 0 : 1;
    for (auto& d : DIRS) {
        int nr = r + d[0], nf = f + d[1];
        if (nr < 0 || nr >= BOARD_SIZE || nf < 0 || nf >= BOARD_SIZE) continue;
        if (test[nr][nf] != (opp ? 1 : 2)) continue;
        if (is_surrounded(test, nr, nf, opp)) {
            int visited[BOARD_SIZE][BOARD_SIZE] = {};
            int gr[BOARD_SIZE * BOARD_SIZE], gf[BOARD_SIZE * BOARD_SIZE];
            int gc = 0;
            get_group(test, nr, nf, opp, visited, &gc, gr, gf);
            for (int i = 0; i < gc; i++) test[gr[i]][gf[i]] = 0;
        }
    }
    int visited[BOARD_SIZE][BOARD_SIZE] = {};
    return !has_liberties(test, r, f, color, visited);
}

// ---------------------------------------------------------------------------
void GoRules::find_captured(const char board[][BOARD_SIZE],
                            int placed_color, int placed_r, int placed_f,
                            int cap_r[], int cap_f[], int& cap_count) {
    cap_count = 0;
    int processed[BOARD_SIZE][BOARD_SIZE] = {};

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            if (board[r][f] == 0 || processed[r][f]) continue;
            if (r == placed_r && f == placed_f) continue;

            int color = (board[r][f] == 1) ? 1 : 0;
            int visited[BOARD_SIZE][BOARD_SIZE] = {};
            int gr[BOARD_SIZE * BOARD_SIZE], gf[BOARD_SIZE * BOARD_SIZE];
            int gc = 0;
            get_group(board, r, f, color, visited, &gc, gr, gf);
            for (int i = 0; i < gc; i++) processed[gr[i]][gf[i]] = 1;

            // Only capture opponent stones
            if (color == placed_color) continue;
            if (is_surrounded(board, r, f, color)) {
                for (int i = 0; i < gc; i++) {
                    // Safety: never capture the placed stone's own position
                    if (gr[i] == placed_r && gf[i] == placed_f) continue;
                    cap_r[cap_count] = gr[i];
                    cap_f[cap_count] = gf[i];
                    cap_count++;
                }
            }
        }
    }
}
