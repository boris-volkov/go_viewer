#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <SDL2/SDL.h>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cstdio>

static constexpr int BOARD_SIZE         = 19;
static constexpr int SCREEN_SIZE        = 800;
static constexpr int MAX_MOVES          = 1000;
static constexpr int MOVE_TEXT_LEN      = 8;
static constexpr int MOVE_DELAY_MS      = 4000;
static constexpr int MOVE_DELAY_MIN_MS  = 200;
static constexpr int MOVE_DELAY_MAX_MS  = 10000;
static constexpr int MOVE_DELAY_STEP_MS = 200;
static constexpr int GAME_OVER_PAUSE_MS = 20000;
static constexpr int SPEED_MESSAGE_MS   = 1500;
static constexpr int NAME_LEN           = 128;
static constexpr int RESULT_LEN         = 16;

#ifdef _WIN32
static constexpr char        PATH_SEP     = '\\';
static constexpr const char* PATH_SEP_STR = "\\";
#else
static constexpr char        PATH_SEP     = '/';
static constexpr const char* PATH_SEP_STR = "/";
#endif

static const char DEFAULT_GAMES_DIR[] = "games/";

struct Stone {
    int r = 0, f = 0, is_black = 0;
};

// Complete snapshot of board + stone-tracking arrays (used for history / analysis init)
struct GameSnapshot {
    char  board[BOARD_SIZE][BOARD_SIZE] = {};
    Stone stones[MAX_MOVES]             = {};
    int   stone_count                   = 0;
    int   black_prisoners               = 0;
    int   white_prisoners               = 0;
    int   turn_is_black                 = 1;
};

struct BoardView {
    int square   = 0;
    int offset_x = 0, offset_y = 0;
    int screen_w = 0, screen_h = 0;
    int board_px = 0;
};

struct Overlay {
    bool  active   = false;
    int   is_black = 0;
    float x = 0.f, y = 0.f;
    int   skip_r1 = -1, skip_f1 = -1;
};
