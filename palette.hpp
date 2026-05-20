#pragma once
#include <SDL2/SDL.h>

// Central colour palette for go_viewer.
// All hard-coded colours in the renderer should be pulled from here so that
// tweaking the look of the program means editing one file.

namespace Palette {

// ---------------------------------------------------------------------------
// Screen / board surfaces

constexpr SDL_Color BACKGROUND      = {52,  60,  72,  255};  // letterbox area behind the board
constexpr SDL_Color BOARD           = {95,  115, 150, 255};  // board surface (normal playback)
constexpr SDL_Color BOARD_ANALYSIS  = {110, 115, 150, 255};  // board surface in analysis/game mode
constexpr SDL_Color GRID            = {45,  55,  70,  255};  // grid lines (dark blue-gray)

// ---------------------------------------------------------------------------
// Stones

constexpr SDL_Color STONE_BLACK     = {30,  30,  30,  255};
constexpr SDL_Color STONE_WHITE     = {240, 240, 240, 255};
constexpr SDL_Color STONE_OUTLINE   = {80,  80,  80,  255};  // subtle ring on white stones

// ---------------------------------------------------------------------------
// UI accent

// Primary accent — mode-status labels, cursor ring, crosshair arms.
// A soft warm yellow that reads clearly on the blue-gray board without
// being as harsh as a fully-saturated yellow.
constexpr SDL_Color ACCENT          = {255, 255, 180, 255};

// ---------------------------------------------------------------------------
// Text

constexpr SDL_Color TEXT_PRIMARY    = {230, 230, 230, 255};  // player names
constexpr SDL_Color TEXT_SECONDARY  = {200, 200, 200, 255};  // secondary labels / help descriptions
constexpr SDL_Color TEXT_DIM        = {120, 120, 120, 255};  // prisoner counts, game year
constexpr SDL_Color TEXT_WHITE      = {255, 255, 255, 255};  // brightest white (help overlay title, catalog)

// ---------------------------------------------------------------------------
// Overlays / panels

constexpr SDL_Color OVERLAY_DARK    = {30,  30,  30,  210};  // help overlay background
constexpr SDL_Color OVERLAY_MID     = {80,  80,  80,  190};  // catalog overlay background
constexpr SDL_Color OVERLAY_SPEED   = {80,  80,  80,  180};  // speed-change flash

// ---------------------------------------------------------------------------
// Interactive / game elements

constexpr SDL_Color CATALOG_SELECT  = {40,  120, 255, 190};  // catalog highlighted row
constexpr SDL_Color LIBERTY_DOT     = {220, 50,  50,  200};  // liberty indicator dots
constexpr SDL_Color BOX_SELECT      = {255, 255, 180, 200};  // box-selection highlight (matches ACCENT)

// ---------------------------------------------------------------------------
// Territory drill

constexpr SDL_Color TERRITORY_BLACK = {100, 100, 100, 180};  // black territory shading
constexpr SDL_Color TERRITORY_WHITE = {200, 200, 200, 180};  // white territory shading
constexpr SDL_Color SCORE_CORRECT   = {100, 220, 100, 255};  // correct-answer feedback (green)
constexpr SDL_Color SCORE_TEXT      = {230, 230, 230, 255};  // score label

} // namespace Palette
