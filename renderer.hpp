#pragma once
#include "go_viewer.hpp"
#include "game_state.hpp"
#include "analysis_state.hpp"
#include "catalog.hpp"
#include <cfloat>
#include <string>

// PlayStation face-button glyphs: draw_text() renders these placeholder characters
// as the button shapes (see font_glyphs in renderer.cpp). Use in control-hint strings
// via literal concatenation, e.g.  GLYPH_PS_CROSS "=SELECT".
#define GLYPH_PS_CROSS    "~"
#define GLYPH_PS_CIRCLE   "@"
#define GLYPH_PS_SQUARE   "#"
#define GLYPH_PS_TRIANGLE "^"

// A letter mark ("A", "B", …) placed on a board point during analysis.
struct BoardLabel { int r = 0; int f = 0; char ch = 'A'; };

// Describes one node in the analysis tree for rendering (produced by ogs_client,
// consumed by render_analysis_tree — a plain POD so renderer.hpp stays dependency-free).
struct AnalysisTreeRenderNode {
    int  depth        = 0;   // row in tree (0 = root)
    int  col          = 0;   // column (0 = main trunk, 1+ = branches)
    bool current      = false;
    int  parent_depth = -1;  // -1 for root
    int  parent_col   = 0;
    int  move_color   = -1;  // 1=black stone, 0=white stone, -1=root (no move)
    bool marked       = false; // flagged for special analysis attention (full-row highlight)
    bool goal         = false; // solution endpoint (puzzle trees) — green halo on the node
};

class Renderer {
public:
    SDL_Renderer* sdl = nullptr;

    explicit Renderer(SDL_Renderer* r) : sdl(r) {}
    ~Renderer() { if (board_cache_) SDL_DestroyTexture(board_cache_); }

    // All draw state needed for one frame
    struct DrawState {
        const GameState&      game;
        const AnalysisState*  analysis;      // null when not in analysis mode
        bool  analysis_mode  = false;
        bool  game_mode      = false;
        bool  guess_mode     = false;
        int   guess_score    = 0;
        bool  chain_mode         = false;
        bool  free_mode          = false;   // demo mode: board only, no HUD labels
        int   active_board_size  = BOARD_SIZE;  // passed to get_board_view
        bool  show_help      = false;
        const Catalog&        catalog;
        const std::string&    black_name;
        const std::string&    white_name;
        const std::string&    result_message;
        const std::string&    game_date;
        const std::string&    game_comment;
        int   move_delay_ms          = MOVE_DELAY_MS;
        Uint32 speed_message_until   = 0;
        bool  suppress_present       = false;
        // Territory estimation drill
        bool  territory_drill    = false;
        const char (*territory_board)[MAX_BOARD_SIZE] = nullptr;
        int   territory_b_score  = 0;
        int   territory_w_score  = 0;
        bool  territory_answered = false;
        bool  territory_correct  = false;
        // Stone visibility filter for playback (hold B or W key)
        int   stone_filter = 0;   // 0=all, 1=black only, 2=white only
        // Software cursor (drawn by renderer, bypasses OS DPI scaling entirely)
        int   cursor_x    = -1;
        int   cursor_y    = -1;
        int   cursor_type = 0;  // 0=hidden, 1=crosshair, 2=white stone, 3=black stone
        bool  show_move_numbers = false;  // overlay move-order numbers on stones
        // Playback: raw SGF arrays (renderer builds grid, independent of captures)
        const char (*sgf_moves)[MOVE_TEXT_LEN] = nullptr;
        const int*  sgf_colors     = nullptr;
        int         sgf_game_index = 0;
        // Analysis: persistent grids maintained by App, never modified on capture
        const int (*analysis_num_grid)[MAX_BOARD_SIZE] = nullptr;
        const int (*analysis_col_grid)[MAX_BOARD_SIZE] = nullptr;
        bool  quit_confirm   = false;
        // Box selection (shift+drag, additive)
        const bool (*box_sel_pts)[MAX_BOARD_SIZE] = nullptr;
        int   box_sel_count  = 0;
        bool  box_drag_active = false;
        int   box_drag_r1 = 0, box_drag_f1 = 0;  // drag start
        int   box_drag_r2 = 0, box_drag_f2 = 0;  // drag end (current mouse)
        // Catalog thumbnails: opening (first N moves) and final position
        bool        catalog_thumb_valid       = false;
        bool        catalog_thumb_single      = false;  // opening == final (e.g. a
                                                        // saved position) — draw one
                                                        // larger board instead of two
        const char (*catalog_thumb_open) [BOARD_SIZE] = nullptr;
        const char (*catalog_thumb_final)[BOARD_SIZE] = nullptr;
        int         catalog_thumb_board_size  = BOARD_SIZE;
        // Transient flash notification (e.g. save confirmation)
        const std::string& flash_message;
        Uint32 flash_message_until = 0;
        // Save-position text input (0=off, 1=name, 2=note)
        int                save_input_step = 0;
        const std::string& save_input_buf;

        // Live game (ogs_client) — all default to off; go_viewer never sets these.
        bool        live_mode       = false;
        int         live_cursor_r   = -1;   // board cursor row; -1 = not shown
        int         live_cursor_f   = -1;   // board cursor col
        int         live_my_color   = 1;    // 1=black, 0=white (for cursor tint)
        bool        live_my_turn    = false;
        int         live_black_secs        = -1;  // -1 = no clock displayed
        int         live_white_secs        = -1;
        int         live_black_periods     = -1;  // -1 = not byo-yomi
        int         live_white_periods     = -1;
        int         live_black_period_secs = -1;
        int         live_white_period_secs = -1;
        bool        live_black_in_byo      = false;  // main time gone, byo-yomi period counting
        bool        live_white_in_byo      = false;
        const char* live_status            = nullptr;
        // Stone removal overlay (STONE_REMOVAL phase only)
        const bool (*live_dead_stones)[MAX_BOARD_SIZE] = nullptr;  // greyed-out stones
        const int  (*live_ownership)[MAX_BOARD_SIZE]   = nullptr;  // territory: 1=black, -1=white
        bool        live_in_history    = false;   // true while reviewing past moves
        // KataGo move suggestions (GAME_OVER history review)
        const MoveSuggestion* live_suggestions        = nullptr;
        int                   live_suggestion_count   = 0;
        int                   live_hovered_suggestion = -1;  // index into live_suggestions, -1 = none
        // Ko violation: cursor flashes red when the move would recreate a prior board state
        bool                  live_cursor_ko          = false;
        // KataGo expected score lead from Black's perspective; FLT_MAX = not available
        float                         live_kata_score_lead         = FLT_MAX;
        // Actual game move from current analysis position (children[0]); -1 = none/pass
        int   live_actual_move_r     = -1;
        int   live_actual_move_f     = -1;
        float live_actual_move_score = FLT_MAX;  // KataGo score_lead for that move; FLT_MAX = unknown
        // Post-game analysis tree (GAME_OVER) — null when not in analysis
        const AnalysisTreeRenderNode* live_analysis_tree          = nullptr;
        int                           live_analysis_tree_count     = 0;
        int                           live_analysis_tree_cur_depth = 0;
        // Score graph — KataGo score_lead (Black's perspective) per main-line depth
        const float* live_score_graph     = nullptr;   // FLT_MAX = no data for that depth
        int          live_score_graph_len = 0;
        int          live_score_graph_cur = 0;          // current depth → yellow scan line
        // Last-played stone — row/col highlight crosshair (-1 = none)
        int          live_last_move_r = -1;
        int          live_last_move_f = -1;
        // Board-edge coordinate labels (toggled by RT during live play)
        bool         live_show_coords = false;
        // Letter labels placed with circle during analysis (per-position, live client)
        const BoardLabel* live_labels      = nullptr;
        int               live_label_count = 0;
        // Big result banner (e.g. "W+42.5") drawn above the status line in double
        // scale — local-game score screen ("result big, prompt normal")
        const char*       live_result_banner = nullptr;
        // Square stone mode: board stones render as beveled tiles instead of
        // shaded circles (settings menu DISPLAY toggle; off = classic round)
        bool              square_stones = false;
        // OGS puzzle solving: solution tree occupies the left panel (via the
        // live_analysis_tree fields), so the comment box moves to the RIGHT
        // gutter and the player labels/clocks are suppressed.
        bool              puzzle_mode = false;
    };

    // Match search settings menu (live client only) — also doubles as a general
    // settings menu accessible mid-game (see `ingame`), for display toggles that
    // don't belong in the network-facing MatchPrefs payload.
    struct MatchMenu {
        int  focus_col    = 0;     // 0=board size, 1=time control or strength, 2=display
        int  focus_row    = 0;     // row within focused column
        bool size_sel[3]  = {};    // 9x9, 13x13, 19x19
        bool speed_sel[3] = {};    // fast/blitz, medium/rapid, slow/live  (OGS mode)
        bool katago_mode  = false; // true = play vs KataGo locally
        int  katago_str   = 2;     // strength index 0-6 fixed ranks, 7 = adaptive (default 10k)
        std::string adaptive_label; // display text for the adaptive row, e.g. "ADAPTIVE (8 KYU)"
        bool show_coords_sel = false; // mirrors App::show_coords_ while the menu is open
        bool analysis_sel    = false; // mirrors App::kata_analysis_enabled_ while the menu is open
        bool chain_sel       = false; // mirrors App::chain_mode_ (visual links between chained stones)
        bool square_sel      = false; // mirrors App::square_stones_ (tile-style stones)
        bool ingame       = false; // opened mid-game — hide search/mode controls, board
                                   // size/speed shown read-only for reference only
    };
    void draw_match_menu(const MatchMenu& menu);

    // Full-screen scrollable list with a highlighted row and a footer hint line —
    // generic (used by the OGS puzzle browser). Presents the frame itself.
    void draw_list_screen(const char* title, const std::vector<std::string>& lines,
                          int index, const char* footer);

    void get_board_view(BoardView& view, int active_size = BOARD_SIZE) const;
    bool screen_to_board(const BoardView& view, int mx, int my, int& r, int& f) const;
    void board_to_screen(const BoardView& view, int br, int bf, int& x, int& y) const;

    void draw_board(const DrawState& ds);

private:
    void render_board(const BoardView& view, const Overlay* overlay, const DrawState& ds);
    void draw_stone_circle(const BoardView& view, int r, int f, int is_black, Uint8 alpha, bool shadow_pass = false);
    void draw_thick_line(int x1, int y1, int x2, int y2, int thickness, SDL_Color color);
    void draw_dashed_line(int x1, int y1, int x2, int y2, int dash_len, int gap_len);
    int  text_width_px(const char* text, int scale) const;
    void draw_text(int x, int y, int scale, const char* text, SDL_Color color);
    void draw_color_swatch(int x, int y, int size, SDL_Color fill, SDL_Color outline);

    void render_chain_connections(const BoardView& view, const char board[][MAX_BOARD_SIZE], bool chain_mode, int stone_filter, bool shadows_only = false);
    void render_all_shadows(const BoardView& view, const char board[][MAX_BOARD_SIZE], bool chain_mode, int stone_filter, int n);
    void render_liberties(const BoardView& view, const int lib_r[], const int lib_f[], int lib_count);
    void render_player_labels(const BoardView& view, const DrawState& ds);
    void render_speed_label(const BoardView& view, int delay_ms, Uint32 until);
    void render_guess_score(const BoardView& view, bool guess_mode, int score);
    void render_mode_status(const BoardView& view, bool analysis_mode, bool game_mode, bool guess_mode, bool territory_drill, bool paused);
    void render_territory_overlay(const BoardView& view, const DrawState& ds);
    void render_result_message(const BoardView& view, const DrawState& ds);
    void render_game_date(const BoardView& view, const std::string& date);
    void render_help_overlay(const BoardView& view, bool show_help, bool live_mode = false);
    void render_catalog_overlay(const BoardView& view, const DrawState& ds);
    void render_mini_board(int x, int y, int size, const char board[][BOARD_SIZE], int board_size = BOARD_SIZE);
    void render_software_cursor(const BoardView& view, const DrawState& ds);
    void render_board_coordinates(const BoardView& view, const DrawState& ds);
    void render_quit_confirm(const BoardView& view);
    void render_box_selection(const BoardView& view, const DrawState& ds);
    void render_flash_message(const BoardView& view, const DrawState& ds);
    void render_analysis_tree(const BoardView& view, const DrawState& ds);
    void render_score_graph(const BoardView& view, const DrawState& ds);
    void render_save_input(const BoardView& view, const DrawState& ds);
    void render_game_comment(const BoardView& view, const DrawState& ds);
    void draw_stone_at_px(int cx, int cy, int radius, int is_black, Uint8 alpha);
    void shade_stone(int cx, int cy, int radius, int is_black, Uint8 alpha, bool shadow_pass = false);
    void draw_stone_link(int x1, int y1, int x2, int y2, int thickness, int is_black, bool shadow_pass = false, int stone_radius = 0);

    // Shared palette so stones and chain links always shade consistently.
    struct StoneColors {
        SDL_Color base;    // shadow tone — drawn solid as the stone base
        SDL_Color lit;     // overlay colour applied at low alpha in shade_stone
        SDL_Color dark;    // cylinder shadow-edge only (slightly darker than base)
        float     alpha1;  // broad layer alpha
        float     alpha2;  // mid layer alpha  (0 = skip)
        float     alpha3;  // tight layer alpha
    };
    static StoneColors stone_colors(int is_black);
    void fill_circle(int cx, int cy, int radius);               // scanline fill, color already set
    void draw_circle(int cx, int cy, int radius);               // Bresenham outline only, color already set
    void fill_ring(int cx, int cy, int r_outer, int r_inner);  // scanline fill of annular region only
    // Scanline fill of a rotated ellipse. ra = semi-axis along (ux,uy), rb = semi-axis along perpendicular.
    void fill_ellipse_rotated(int cx, int cy, float ux, float uy, int ra, int rb);

    // Board cache: the board+HUD are rendered to a texture and only rebuilt when
    // board state changes.  Cursor is composited on top each frame for free.
    void render_board_content(const BoardView& view, const Overlay* overlay, const DrawState& ds);
    uint64_t compute_cache_hash(const DrawState& ds) const;

    bool square_stones_ = false;   // mirrored from DrawState each draw_board call
    SDL_Texture* board_cache_ = nullptr;
    int          cache_w_     = 0;
    int          cache_h_     = 0;
    uint64_t     cache_hash_  = ~0ULL;   // initialised so first frame always rebuilds

    static const char* format_result_message(const char* sgf_result);

    struct Glyph { char c; unsigned char rows[7]; };
    static const Glyph       font_glyphs[];
    static const unsigned char* get_glyph_rows(char c);
};
