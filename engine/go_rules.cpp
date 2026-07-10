#include "go_rules.hpp"
#include <cstring>

static constexpr int DIRS[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};

// ---------------------------------------------------------------------------
void GoRules::get_group(const char board[][MAX_BOARD_SIZE], int r, int f, int color,
                        int visited[][MAX_BOARD_SIZE],
                        int* count, int gr[], int gf[], int n) {
    *count = 0;
    int stack[MAX_BOARD_SIZE * MAX_BOARD_SIZE][2];
    int top = 0;
    if (r < 0 || r >= n || f < 0 || f >= n ||
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
            if (nr >= 0 && nr < n && nf >= 0 && nf < n &&
                !visited[nr][nf] && board[nr][nf] == (color ? 1 : 2)) {
                stack[top][0] = nr; stack[top][1] = nf; top++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
bool GoRules::has_liberties(const char board[][MAX_BOARD_SIZE], int r, int f, int color,
                            int visited[][MAX_BOARD_SIZE], int n) {
    int stack[MAX_BOARD_SIZE * MAX_BOARD_SIZE][2];
    int top = 0;
    if (r < 0 || r >= n || f < 0 || f >= n ||
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
            if (nr < 0 || nr >= n || nf < 0 || nf >= n) continue;
            if (board[nr][nf] == 0) return true;
            if (board[nr][nf] == (color ? 1 : 2) && !visited[nr][nf]) {
                stack[top][0] = nr; stack[top][1] = nf; top++;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
void GoRules::get_liberties(const char board[][MAX_BOARD_SIZE], int r, int f, int color,
                            int visited[][MAX_BOARD_SIZE],
                            int lib_r[], int lib_f[], int& lib_count, int n) {
    int stack[MAX_BOARD_SIZE * MAX_BOARD_SIZE][2];
    int top = 0;
    if (r < 0 || r >= n || f < 0 || f >= n ||
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
            if (nr < 0 || nr >= n || nf < 0 || nf >= n) continue;
            if (board[nr][nf] == 0) {
                bool dup = false;
                for (int i = 0; i < lib_count; i++) {
                    if (lib_r[i] == nr && lib_f[i] == nf) { dup = true; break; }
                }
                if (!dup && lib_count < n * n) {
                    lib_r[lib_count] = nr; lib_f[lib_count] = nf; lib_count++;
                }
            } else if (board[nr][nf] == (color ? 1 : 2) && !visited[nr][nf]) {
                stack[top][0] = nr; stack[top][1] = nf; top++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
bool GoRules::is_surrounded(const char board[][MAX_BOARD_SIZE], int r, int f, int color, int n) {
    int visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    return !has_liberties(board, r, f, color, visited, n);
}

// ---------------------------------------------------------------------------
bool GoRules::would_be_suicide(const char board[][MAX_BOARD_SIZE], int r, int f, int color, int n) {
    char test[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
    memcpy(test, board, sizeof(test));
    test[r][f] = color ? 1 : 2;

    int opp = color ? 0 : 1;
    for (auto& d : DIRS) {
        int nr = r + d[0], nf = f + d[1];
        if (nr < 0 || nr >= n || nf < 0 || nf >= n) continue;
        if (test[nr][nf] != (opp ? 1 : 2)) continue;
        if (is_surrounded(test, nr, nf, opp, n)) {
            int visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
            int gr[MAX_BOARD_SIZE * MAX_BOARD_SIZE], gf[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
            int gc = 0;
            get_group(test, nr, nf, opp, visited, &gc, gr, gf, n);
            for (int i = 0; i < gc; i++) test[gr[i]][gf[i]] = 0;
        }
    }
    int visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    return !has_liberties(test, r, f, color, visited, n);
}

// ---------------------------------------------------------------------------
void GoRules::find_captured(const char board[][MAX_BOARD_SIZE],
                            int placed_color, int placed_r, int placed_f,
                            int cap_r[], int cap_f[], int& cap_count, int n) {
    cap_count = 0;
    int processed[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};

    for (int r = 0; r < n; r++) {
        for (int f = 0; f < n; f++) {
            if (board[r][f] == 0 || processed[r][f]) continue;
            if (r == placed_r && f == placed_f) continue;

            int color = (board[r][f] == 1) ? 1 : 0;
            int visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
            int gr[MAX_BOARD_SIZE * MAX_BOARD_SIZE], gf[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
            int gc = 0;
            get_group(board, r, f, color, visited, &gc, gr, gf, n);
            for (int i = 0; i < gc; i++) processed[gr[i]][gf[i]] = 1;

            if (color == placed_color) continue;
            if (is_surrounded(board, r, f, color, n)) {
                for (int i = 0; i < gc; i++) {
                    if (gr[i] == placed_r && gf[i] == placed_f) continue;
                    cap_r[cap_count] = gr[i];
                    cap_f[cap_count] = gf[i];
                    cap_count++;
                }
            }
        }
    }
}
