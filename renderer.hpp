#pragma once
#include "go_viewer.hpp"
#include "game_state.hpp"
#include "analysis_state.hpp"
#include "catalog.hpp"
#include <string>

class Renderer {
public:
    SDL_Renderer* sdl = nullptr;

    explicit Renderer(SDL_Renderer* r) : sdl(r) {}

    // All draw state needed for one frame
    struct DrawState {
        const GameState&      game;
        const AnalysisState*  analysis;      // null when not in analysis mode
        bool  analysis_mode  = false;
        bool  game_mode      = false;
        bool  guess_mode     = false;
        int   guess_score    = 0;
        bool  chain_mode     = false;
        bool  show_help      = false;
        const Catalog&        catalog;
        const std::string&    black_name;
        const std::string&    white_name;
        const std::string&    result_message;
        int   move_delay_ms          = MOVE_DELAY_MS;
        Uint32 speed_message_until   = 0;
        bool  suppress_present       = false;
        // Territory estimation drill
        bool  territory_drill    = false;
        const char (*territory_board)[BOARD_SIZE] = nullptr;
        int   territory_b_score  = 0;
        int   territory_w_score  = 0;
        bool  territory_answered = false;
        bool  territory_correct  = false;
    };

    void get_board_view(BoardView& view) const;
    bool screen_to_board(const BoardView& view, int mx, int my, int& r, int& f) const;
    void board_to_screen(const BoardView& view, int br, int bf, int& x, int& y) const;

    void draw_board(const DrawState& ds);

private:
    void render_board(const BoardView& view, const Overlay* overlay, const DrawState& ds);
    void draw_stone_circle(const BoardView& view, int r, int f, int is_black, Uint8 alpha);
    void draw_thick_line(int x1, int y1, int x2, int y2, int thickness, SDL_Color color);
    int  text_width_px(const char* text, int scale) const;
    void draw_text(int x, int y, int scale, const char* text, SDL_Color color);
    void draw_color_swatch(int x, int y, int size, SDL_Color fill, SDL_Color outline);

    void render_chain_connections(const BoardView& view, const char board[][BOARD_SIZE], bool chain_mode);
    void render_liberties(const BoardView& view, const int lib_r[], const int lib_f[], int lib_count);
    void render_player_labels(const BoardView& view, const DrawState& ds);
    void render_speed_label(const BoardView& view, int delay_ms, Uint32 until);
    void render_guess_score(const BoardView& view, bool guess_mode, int score);
    void render_turn_indicator(const BoardView& view, int is_black);
    void render_mode_status(const BoardView& view, bool analysis_mode, bool game_mode, bool guess_mode, bool territory_drill, bool paused);
    void render_territory_overlay(const BoardView& view, const DrawState& ds);
    void render_result_message(const BoardView& view, const DrawState& ds);
    void render_help_overlay(const BoardView& view, bool show_help);
    void render_catalog_overlay(const BoardView& view, const Catalog& catalog);

    static const char* format_result_message(const char* sgf_result);

    struct Glyph { char c; unsigned char rows[7]; };
    static const Glyph       font_glyphs[];
    static const unsigned char* get_glyph_rows(char c);
};
