#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cfloat>
#include <cmath>
#include "go_viewer.hpp"
#include "go_rules.hpp"
#include "game_state.hpp"
#include "analysis_state.hpp"
#include "catalog.hpp"
#include "renderer.hpp"
#include "ogs_client.hpp"
#include "ogs_net.hpp"
#include "katago.hpp"
#include "sound.hpp"
#include "ogs_puzzles.hpp"
#include <atomic>

#include "json.hpp"
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <memory>
#include <algorithm>
#include <thread>

// Registered once in main(); OgsNet reads this to push SDL events.
Uint32 g_net_event_type = 0;

// ── Credentials ──────────────────────────────────────────────────────────────

static std::string exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0) {
        std::string path(buf, n);
        auto slash = path.find_last_of("\\/");
        if (slash != std::string::npos) return path.substr(0, slash + 1);
    }
#endif
    return "";
}

// True for "C:\..." or a leading "\"/"/" (including UNC "\\server\...") — anything
// else is a relative path, meant to be resolved against the exe's own directory so
// a katago/ folder dropped next to ogs_client.exe works regardless of launch method
// (double-click, shortcut, terminal — not just the common case where cwd happens to
// already be the exe's folder).
static bool is_absolute_path(const std::string& p) {
    if (p.empty()) return false;
    if (p.size() >= 2 && p[1] == ':') return true;
    if (p[0] == '\\' || p[0] == '/') return true;
    return false;
}
static std::string resolve_path(const std::string& p) {
    if (p.empty() || is_absolute_path(p)) return p;
    return exe_dir() + p;
}

static bool load_config(std::string& username, std::string& password, std::string& jwt,
                        std::string& kata_exe, std::string& kata_model, std::string& kata_cfg,
                        std::string& kata_model_9x9, std::string& kata_human_model) {
    std::string path = exe_dir() + "config.ini";
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return false;
    char line[4096];  // jwt values can be long
    while (fgets(line, sizeof(line), fp)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* val = eq + 1;
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r'))
            val[--vlen] = '\0';
        if      (strcmp(line, "username")           == 0) username          = val;
        else if (strcmp(line, "password")           == 0) password          = val;
        else if (strcmp(line, "jwt")                == 0) jwt               = val;
        else if (strcmp(line, "katago_exe")         == 0) kata_exe          = val;
        else if (strcmp(line, "katago_model")       == 0) kata_model        = val;
        else if (strcmp(line, "katago_config")      == 0) kata_cfg          = val;
        else if (strcmp(line, "katago_model_9x9")   == 0) kata_model_9x9   = val;
        else if (strcmp(line, "katago_human_model") == 0) kata_human_model  = val;
    }
    fclose(fp);
    kata_exe         = resolve_path(kata_exe);
    kata_model       = resolve_path(kata_model);
    kata_cfg         = resolve_path(kata_cfg);
    kata_model_9x9   = resolve_path(kata_model_9x9);
    kata_human_model = resolve_path(kata_human_model);
    return !username.empty();  // password optional if jwt provided
}

// ── App state machine ─────────────────────────────────────────────────────────

enum class AppState {
    CREDENTIAL_PROMPT, // entering username / password
    CONNECTING,        // auth in progress
    LOBBY,             // authenticated, idle
    MATCH_MENU,        // match settings menu open
    SEARCHING,         // automatch in queue
    PLAYING,           // live game active
    STONE_REMOVAL,     // game ended, awaiting stone removal acceptance
    GAME_OVER,         // game finished, showing result
    PUZZLE_BROWSE,     // browsing OGS puzzle collections / puzzle lists
    PUZZLE_PLAY,       // solving an OGS puzzle against its authored solution tree
    JOSEKI,            // walking the OGS Joseki Explorer position graph
};

// Live game state
struct LiveGame {
    // History of board states after each stone placement (index 0 = start of game)
    std::vector<GameState> history;
    int history_pos = -1;  // -1 = live view, >=0 = reviewing history[history_pos]
    int         game_id     = 0;
    int         board_size  = 19;
    int         my_color    = 1;   // 1=black, 0=white
    int         my_player_id = 0;
    std::string black_name, white_name;
    std::string black_rank, white_rank;
    int         black_secs        = -1;
    int         white_secs        = -1;
    int         black_periods     = -1;  // -1 = not byo-yomi
    int         white_periods     = -1;
    int         black_period_secs = -1;
    int         white_period_secs = -1;
    bool        black_in_byo      = false;  // main time gone — living on byo-yomi periods
    bool        white_in_byo      = false;
    Uint32      clock_tick  = 0;   // SDL_GetTicks() when clock last updated
    bool        my_turn     = false;
    int         pending_col = -2;  // -2=none, -1=pass pending echo, >=0=move pending echo
    int         pending_row = -2;
    int         handicap    = 0;
    bool        free_handicap = false;
    int         cursor_r    = 9;
    int         cursor_f    = 9;
    GameState   board;
    std::string result;
    // Stone removal phase overlay
    bool dead_stones[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    int  ownership[MAX_BOARD_SIZE][MAX_BOARD_SIZE]   = {};
};

// ── Lobby screensaver: minimal SGF parser ────────────────────────────────────

struct SgfGame {
    char  moves[MAX_MOVES][MOVE_TEXT_LEN] = {};
    int   colors[MAX_MOVES]               = {};
    int   move_count                      = 0;
    int   setup_count                     = 0;  // leading entries from AB[]/AW[] setup stones, not real moves
    int   start_black                     = 1;  // from PL[] property: who's to move in the starting position
    bool  has_pl                          = false; // true if PL[] was present (start_black is authoritative)
    int   board_size                      = BOARD_SIZE;
    char  black_name[NAME_LEN]            = "Black";
    char  white_name[NAME_LEN]            = "White";
    char  result[32]                      = {};
    float komi                            = 7.5f;  // from KM[] property
};

static bool parse_sgf_move(const char* move_str, int& out_r, int& out_f) {
    if (!move_str || strlen(move_str) != 2) return false;
    char fc = move_str[0], rc = move_str[1];
    if (fc < 'a' || fc > 's' || rc < 'a' || rc > 's') return false;
    out_f = fc - 'a';
    out_r = rc - 'a';
    return true;
}

static bool load_sgf(const std::string& path, SgfGame& g) {
    g.move_count = 0;
    g.board_size = BOARD_SIZE;
    g.black_name[0] = g.white_name[0] = '\0';

    FILE* fp = Catalog::fopen_utf8(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    rewind(fp);
    if (fsz <= 0 || fsz > 8 * 1024 * 1024) { fclose(fp); return false; }
    std::vector<char> buf((size_t)fsz + 1);
    size_t nread = fread(buf.data(), 1, (size_t)fsz, fp);
    fclose(fp);
    buf[nread] = '\0';

    auto read_val = [](const char* src, char* dst, size_t dsz) -> const char* {
        size_t i = 0;
        while (*src && *src != ']') {
            if (*src == '\\') { src++; if (!*src) break; }
            if (i < dsz - 1) dst[i++] = *src;
            src++;
        }
        while (i > 0 && (dst[i-1]==' '||dst[i-1]=='\t'||dst[i-1]=='\r'||dst[i-1]=='\n')) i--;
        dst[i] = '\0';
        return (*src == ']') ? src + 1 : src;
    };

    const char* p = buf.data();
    int  depth = 0;
    bool in_prop = false;
    bool prev_alpha = false;
    // For each parent depth d, true once we've followed the first '(' child.
    // A second '(' at the same parent depth is a variation branch and gets skipped.
    // This handles both standard SGF (variations at depth 2) and the OGS auto-save
    // format where every move after the second is wrapped in its own '(' pair,
    // pushing moves to progressively deeper levels — so depth can grow roughly as
    // large as the move count. Sized past MAX_MOVES with margin (not just 256, which
    // any real OGS game past ~255 moves would silently overflow — this function runs
    // on every catalog cursor move for the thumbnail preview, so that was a reliable
    // stack-corruption crash on scrolling to any sufficiently long game).
    static constexpr int MAX_SGF_DEPTH = MAX_MOVES + 16;
    bool branch_taken[MAX_SGF_DEPTH] = {};
    int  skip_depth = 0;   // >0 = we're inside a sibling variation; skip until depth drops below this

    while (*p) {
        if (in_prop) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == ']')  { in_prop = false; }
            if (*p && !isspace((unsigned char)*p)) prev_alpha = (isalpha((unsigned char)*p) != 0);
            p++;
            continue;
        }
        if (*p == '(') {
            depth++;
            if (depth >= MAX_SGF_DEPTH) {
                // Pathologically deep nesting (or a malformed/hostile file) — stop
                // parsing rather than overflow branch_taken[]. Whatever was already
                // parsed is kept; the remainder of the file is dropped.
                break;
            }
            if (skip_depth == 0) {
                if (branch_taken[depth - 1]) {
                    skip_depth = depth;          // sibling branch → skip
                } else {
                    branch_taken[depth - 1] = true;  // first child → follow main line
                }
            }
            prev_alpha = false; p++; continue;
        }
        if (*p == ')') {
            if (skip_depth > 0 && depth == skip_depth) skip_depth = 0;
            branch_taken[depth] = false;
            depth--;
            prev_alpha = false; p++; continue;
        }
        if (*p == '[') { in_prop = true; prev_alpha = false; p++; continue; }

        if (skip_depth > 0) {
            if (*p && !isspace((unsigned char)*p)) prev_alpha = (isalpha((unsigned char)*p) != 0);
            p++;
            continue;
        }

        // AB[...][...] / AW[...] — setup stones (handicap). Multi-value: AB[pd][dp]...
        if (*p == 'A' && (*(p+1) == 'B' || *(p+1) == 'W') && *(p+2) == '[' && !prev_alpha) {
            int ab_color = (*(p+1) == 'B') ? 1 : 0;
            p += 2;  // skip 'A', 'B'/'W' — p now points to first '['
            while (*p == '[') {
                p++;  // skip '['
                const char* start = p;
                while (*p && *p != ']') { if (*p == '\\') p++; if (*p) p++; }
                if (g.move_count < MAX_MOVES && p > start) {
                    size_t len = (size_t)(p - start);
                    if (len >= MOVE_TEXT_LEN) len = MOVE_TEXT_LEN - 1;
                    memcpy(g.moves[g.move_count], start, len);
                    g.moves[g.move_count][len] = '\0';
                    g.colors[g.move_count] = ab_color;
                    g.move_count++;
                    g.setup_count++;
                }
                if (*p == ']') p++;
                while (*p && isspace((unsigned char)*p)) p++;  // skip whitespace between values
            }
            prev_alpha = false; continue;
        }
        if (*p == 'B' && *(p+1) == '[' && !prev_alpha) {
            p += 2;
            const char* start = p;
            while (*p && *p != ']') { if (*p == '\\') p++; if (*p) p++; }
            if (g.move_count < MAX_MOVES) {
                size_t len = (size_t)(p - start);
                if (len >= MOVE_TEXT_LEN) len = MOVE_TEXT_LEN - 1;
                memcpy(g.moves[g.move_count], start, len);
                g.moves[g.move_count][len] = '\0';
                g.colors[g.move_count] = 1;
                g.move_count++;
            }
            if (*p == ']') p++;
            prev_alpha = false; continue;
        }
        if (*p == 'W' && *(p+1) == '[' && !prev_alpha) {
            p += 2;
            const char* start = p;
            while (*p && *p != ']') { if (*p == '\\') p++; if (*p) p++; }
            if (g.move_count < MAX_MOVES) {
                size_t len = (size_t)(p - start);
                if (len >= MOVE_TEXT_LEN) len = MOVE_TEXT_LEN - 1;
                memcpy(g.moves[g.move_count], start, len);
                g.moves[g.move_count][len] = '\0';
                g.colors[g.move_count] = 0;
                g.move_count++;
            }
            if (*p == ']') p++;
            prev_alpha = false; continue;
        }
        if (depth == 1) {
            if (*p == 'S' && *(p+1) == 'Z' && *(p+2) == '[') {
                p += 3;
                int sz = (int)atoi(p);
                if (sz >= 2 && sz <= MAX_BOARD_SIZE) g.board_size = sz;
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
                prev_alpha = false; continue;
            }
            if (*p == 'K' && *(p+1) == 'M' && *(p+2) == '[') {
                p += 3;
                float km = (float)atof(p);
                // Fox Go Server encodes Chinese-rules komi without a decimal point
                // (e.g. "375" meaning 3.75) — KataGo doesn't understand that raw
                // value, so translate that specific case to the equivalent komi
                // it expects.
                if (km == 375.f) km = 3.5f;
                g.komi = km;
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
                prev_alpha = false; continue;
            }
            if (*p == 'P' && *(p+1) == 'L' && *(p+2) == '[') {
                p += 3;
                g.start_black = (*p == 'B') ? 1 : 0;
                g.has_pl      = true;
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
                prev_alpha = false; continue;
            }
            if (*p == 'P' && *(p+1) == 'B' && *(p+2) == '[') {
                p += 3; p = read_val(p, g.black_name, sizeof(g.black_name));
                prev_alpha = false; continue;
            }
            if (*p == 'P' && *(p+1) == 'W' && *(p+2) == '[') {
                p += 3; p = read_val(p, g.white_name, sizeof(g.white_name));
                prev_alpha = false; continue;
            }
            if (*p == 'R' && *(p+1) == 'E' && *(p+2) == '[') {
                p += 3; p = read_val(p, g.result, sizeof(g.result));
                prev_alpha = false; continue;
            }
        }

        if (!isspace((unsigned char)*p)) prev_alpha = (isalpha((unsigned char)*p) != 0);
        p++;
    }

    if (g.black_name[0] == '\0') strcpy(g.black_name, "Black");
    if (g.white_name[0] == '\0') strcpy(g.white_name, "White");
    return g.move_count > 0;
}

// ── Catalog thumbnails ────────────────────────────────────────────────────────

static constexpr int THUMB_OPENING_MOVES = 16;

// Replay up to max_moves from an SGF and write the resulting board positions.
// Used for catalog preview thumbnails — avoids the full GameState overhead.
static bool sgf_board_at(const std::string& path,
                          char board_out[BOARD_SIZE][BOARD_SIZE],
                          int* board_size_out,
                          int max_moves = -1) {
    SgfGame g;
    if (!load_sgf(path, g)) return false;
    int sz = g.board_size;
    if (board_size_out) *board_size_out = sz;
    int limit = (max_moves < 0 || max_moves > g.move_count) ? g.move_count : max_moves;
    if (limit < g.setup_count) limit = g.setup_count;  // never truncate mid-setup
    char board[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    for (int i = 0; i < limit; i++) {
        int r, f;
        if (!parse_sgf_move(g.moves[i], r, f)) continue;
        if (r < 0 || r >= sz || f < 0 || f >= sz) continue;
        if (board[r][f] != 0) continue;
        int is_black = (g.colors[i] == 1);
        if (GoRules::would_be_suicide(board, r, f, is_black, sz)) continue;
        board[r][f] = is_black ? 1 : 2;
        int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_count = 0;
        GoRules::find_captured(board, is_black, r, f, cap_r, cap_f, cap_count, sz);
        for (int j = 0; j < cap_count; j++) board[cap_r[j]][cap_f[j]] = 0;
    }
    // Copy row-by-row: internal buffer has MAX_BOARD_SIZE stride, output has BOARD_SIZE stride
    memset(board_out, 0, BOARD_SIZE * BOARD_SIZE);
    int copy_sz = sz < BOARD_SIZE ? sz : BOARD_SIZE;
    for (int r = 0; r < copy_sz; r++)
        memcpy(board_out[r], board[r], copy_sz);
    return true;
}

// ── Analysis tree ─────────────────────────────────────────────────────────────

struct AnalysisNode {
    GameState board;
    int   move_col   = -1;   // stone played to reach this node (-1 = root or pass)
    int   move_row   = -1;
    int   move_color = -1;   // 1=black, 0=white, -1=root
    int   depth      = 0;    // distance from root
    int   active_child = 0;  // which child RT navigates into
    float score_lead = FLT_MAX;  // KataGo score lead from Black's perspective; FLT_MAX = unknown
    // True for nodes that are part of the actual played game (children[0] chain from
    // root, set once by build_analysis_tree() and never altered). False for nodes
    // created by branching off into a hypothetical line. Distinct from active_child,
    // which just tracks navigation and gets repointed at whichever branch you last
    // explored — is_main_line is the stable, structural "is this the real game" fact.
    bool  is_main_line = true;
    // Marks this position as a correct ending of a life-and-death drill line
    // (rendered as a green halo in the tree panel; serialized as C[RIGHT]).
    bool  drill_correct = false;
    // Letter annotations ("A", "B", …) placed with the circle button — owned by the
    // position they were placed on, so navigating away and back restores them.
    std::vector<BoardLabel> labels;
    std::vector<std::unique_ptr<AnalysisNode>> children;
    AnalysisNode* parent = nullptr;
};

// ── Application ───────────────────────────────────────────────────────────────

class App {
public:
    int run();

private:
    SDL_Window*         window_   = nullptr;
    SDL_Renderer*       sdl_rend_ = nullptr;
    SDL_GameController* pad_      = nullptr;
    Renderer*           renderer_ = nullptr;

    AppState    state_    = AppState::CONNECTING;
    OgsNet      net_;
    LiveGame    game_;
    Sound       sound_;

    // Game catalog for reviewing saved OGS games
    Catalog      catalog_;
    bool         catalog_delete_confirm_ = false;  // Y pressed once, awaiting confirm
    // True while browsing the pro game library (open_pro_catalog()) — blocks the
    // Y-button delete path so a stray double-press can never remove a curated
    // professional-game SGF. False for the user's own games/<username>/ catalog.
    bool         catalog_readonly_ = false;
    std::string  my_username_;   // from config.ini; used to locate games/<name>/ dir

    // Catalog thumbnails (BOARD_SIZE stride matches DrawState::catalog_thumb_open type)
    std::string  thumb_path_;
    char         thumb_open_ [BOARD_SIZE][BOARD_SIZE] = {};
    char         thumb_final_[BOARD_SIZE][BOARD_SIZE] = {};
    bool         thumb_valid_      = false;
    bool         thumb_single_     = false;  // opening == final → show one board, not two
    int          thumb_board_size_ = BOARD_SIZE;

    void update_catalog_thumb() {
        std::string sel = catalog_.selected_entry_path();
        if (sel == thumb_path_) return;
        thumb_path_  = sel;
        thumb_valid_ = !sel.empty()
            && sgf_board_at(sel, thumb_open_,  &thumb_board_size_, THUMB_OPENING_MOVES)
            && sgf_board_at(sel, thumb_final_,  nullptr);
        // Single-position files (marked positions, study puzzles — all setup stones,
        // no moves) produce identical opening/final snapshots; drawing both is just
        // the same picture twice. Board equality is the cheapest reliable test.
        thumb_single_ = thumb_valid_ &&
                        memcmp(thumb_open_, thumb_final_, sizeof(thumb_open_)) == 0;
    }

    // DrawState anchors (refs must point to persistent objects)
    std::string  empty_str_;
    // Formatted player labels including rank, e.g. "Alice [5d]"
    std::string  black_label_, white_label_;
    AnalysisState* dummy_analysis_ = nullptr;

    // Credential prompt text input
    int         cred_step_ = 0;  // 0=off, 1=username, 2=password
    std::string cred_username_, cred_password_, cred_buf_;

    // Status line (changes with state)
    std::string status_;
    std::string hist_status_;  // scratch: "MOVE N/M" while reviewing history

    // Double-press confirms (pass on circle, mark on triangle, find-match on cross)
    bool pass_confirm_       = false;
    bool mark_confirm_       = false;
    bool find_match_confirm_ = false;

    // Flash message
    std::string flash_;
    Uint32      flash_until_    = 0;
    Uint32      ko_flash_until_   = 0;
    Uint32      kata_query_after_      = 0;    // deferred KataGo query — fires after user settles
    bool        kata_analysis_enabled_ = true; // toggled in the settings menu (BACK)
    float       review_komi_          = 7.5f; // komi used for GAME_OVER analysis queries;
                                               // set from a loaded SGF's KM[], else default

    // Match settings
    MatchPrefs            match_prefs_;
    Renderer::MatchMenu   match_menu_;
    AppState              pre_menu_state_ = AppState::LOBBY;  // state to return to when closing an in-game menu

    // Persisted across sessions to settings.txt: match_prefs_ (board size/speed/
    // KataGo mode/strength) plus the DISPLAY column toggles (show_coords_,
    // kata_analysis_enabled_, chain_mode_, square_stones_, square_grid_). Loaded
    // once at startup; saved whenever the settings menu closes, since that's
    // already the single commit point match_prefs_ itself uses (save_prefs()) —
    // by then any DISPLAY toggles pressed during this visit are live too.
    void load_settings();
    void save_settings();

    // Lobby screensaver: auto-plays a game from the games/ directory.
    // Moves are applied one per second into a single GameState — no bulk copying.
    struct DemoState {
        std::vector<int> rows, cols, colors;  // move list loaded from SGF
        int    pos        = 0;
        int    board_size = BOARD_SIZE;
        std::string black_name, white_name;
        Uint32 next_tick  = 0;
        GameState board;  // single board, updated move-by-move
    } demo_;
    bool demo_active_ = false;

    // Scoped screensaver playlist — set by start_catalog_autoplay() (X in the
    // catalog, on a game or a directory/player/year entry) to play a specific
    // ordered list of games in the lobby instead of the default random pick.
    // -1 = no scoped playlist (default, untouched random screensaver).
    std::vector<std::string> demo_playlist_;
    int                       demo_playlist_pos_ = -1;
    void start_catalog_autoplay(int start_at_catalog_index);

    // Visual links between chained stones — toggled in the settings menu's DISPLAY column
    bool chain_mode_       = true;
    // Stones drawn as beveled square tiles instead of circles (DISPLAY toggle, off by default)
    bool square_stones_    = false;
    // Points sit at the centre of a checkerboard cell instead of a line crossing
    // (DISPLAY toggle, off by default) — see renderer.cpp's render_board_content.
    bool square_grid_      = false;

    // Help overlay / quit confirm
    bool show_help_    = false;
    bool quit_confirm_ = false;

    // Board-edge coordinate labels toggle (RT during live play)
    bool show_coords_  = false;

    // START popup action menu: a context-sensitive command list for the current
    // state (resign, return to lobby, catalog, …) navigated with the dpad and
    // cross. Destructive items set `confirm` — the first press rearms the row as
    // "REALLY …?", the second executes. Closed by circle/OPTIONS, and by any
    // network event that changes the app state out from under it.
    struct PopupItem {
        std::string           label;
        bool                  confirm = false;
        std::function<void()> action;
    };
    std::vector<PopupItem>   popup_items_;
    std::vector<std::string> popup_labels_;  // display labels (armed row shows REALLY …?)
    std::string popup_title_;
    bool popup_active_ = false;
    int  popup_index_  = 0;
    int  popup_armed_  = -1;   // item index awaiting its confirm press; -1 = none
    void open_popup_menu();
    void close_popup_menu();
    void popup_sync_labels();
    void handle_popup_button(Uint8 btn);
    // Actions shared by the popup and direct button bindings
    void do_resign();
    void return_to_lobby();
    void accept_stone_removal();

    // Last-played stone position (for row/col crosshair highlight)
    int last_move_r_ = -1;
    int last_move_f_ = -1;

    // Undo-request state
    bool undo_pending_     = false;
    int  undo_move_number_ = 0;

    // Stone removal: true once OGS has sent territory data; prevents KataGo from overwriting it
    bool stone_removal_has_ogs_territory_ = false;
    // True after user presses A but before server confirms our acceptance
    bool        my_accept_sent_           = false;
    Uint32      accept_resend_at_         = 0;   // next time to proactively resend cmd_accept_stones
    std::string stone_removal_all_removed_;  // all_removed string from last removed_stones event

    // Post-game analysis tree (valid in GAME_OVER state)
    std::unique_ptr<AnalysisNode>           analysis_root_;
    AnalysisNode*                           analysis_cur_ = nullptr;
    std::vector<AnalysisTreeRenderNode>     analysis_tree_render_;

    // KataGo subprocesses: kata_ = general model, kata_9_ = 9x9-specialist model
    KatagoProc     kata_;
    KatagoProc     kata_9_;
    MoveSuggestion kata_suggestions_[MAX_SUGGESTIONS] = {};
    int            kata_suggestion_count_             = 0;
    float          kata_score_lead_               = FLT_MAX;  // FLT_MAX = no data

    // Score graph: KataGo score_lead per main-line depth, built during play and review.
    std::vector<float> move_scores_;           // indexed by depth; FLT_MAX = unknown
    std::vector<bool>  move_marked_;           // indexed by depth; true = flagged for review
    // Path of the standalone SGF written for a marked depth this session (empty = none/unknown —
    // e.g. marks restored from a companion file on load have no recorded path to clean up).
    std::vector<std::string> marked_paths_;
    int                bg_analysis_next_  = 0; // next main-line depth to submit
    int                bg_analysis_depth_ = -1;// depth of the currently-pending bg query
    bool               bg_analysis_busy_  = false;
    Uint32             bg_analysis_started_at_ = 0;  // for the stuck-query timeout below
    bool               fg_kata_pending_   = false; // foreground KataGo query in-flight
    std::string        companion_path_;            // path of .katago companion file

    // Auto-detected study puzzles: positions where exactly one move kept the game
    // and it wasn't played. Top-2 of each background-scoring response is kept here
    // (the sweep already queries every position; we previously discarded all of it
    // except the root score), then evaluated once the game is over — never during
    // play, so no engine information can leak into a live game.
    struct PuzzleEval {
        float best_sl   = FLT_MAX;   // best suggestion's scoreLead (Black's perspective)
        float second_sl = FLT_MAX;   // second-best; FLT_MAX = KataGo offered no alternative
        int   best_r = -1, best_f = -1;
    };
    std::map<int, PuzzleEval> puzzle_eval_;   // keyed by depth (position before the move)
    std::set<int>             puzzle_saved_;  // depths already written to puzzles/
    void check_puzzle(int d);
    void save_puzzle_position(int depth);

    // Path of the SGF currently open for review (catalog-loaded only; empty after a
    // live game). Enables L3/R3 cycling through sibling files without the catalog.
    std::string review_path_;
    void review_cycle(int dir);
    // Peek at the next/prev sibling .sgf in review_path_'s directory (alphabetical,
    // wraps) without loading it — shared by review_cycle() and next_review_sibling's
    // callers. Empty string = fewer than 2 sibling files. out_index/out_total
    // (1-based / count), if non-null, receive the target's position in the listing.
    std::string next_review_sibling(int dir, int* out_index = nullptr, int* out_total = nullptr) const;
    // Advance analysis_cur_ one ply along the active-child main line — shared by
    // the L2/R2 step-forward button handler.
    void analysis_step_forward();

    // ── OGS puzzle browsing / solving ─────────────────────────────────────────
    // Fetches run on detached worker threads (blocking libcurl); results land in a
    // shared buffer and wake the event loop with the net event. A superseded fetch's
    // buffer just gets dropped when its shared_ptr no longer matches pz_fetch_.
    struct PuzzleFetch {
        std::atomic<bool> ready{false};
        bool ok   = false;
        int  kind = 0;  // 1=collections page, 2=puzzle, 3=collection siblings
        std::vector<OgsPuzzleCollection> collections;
        int  total = 0;
        OgsPuzzle puzzle;
        std::vector<std::pair<int, std::string>> siblings;
    };
    std::shared_ptr<PuzzleFetch> pz_fetch_;
    bool pz_loading_ = false;

    enum class PzView { COLLECTIONS, PUZZLES };
    PzView pz_view_ = PzView::COLLECTIONS;
    static constexpr int PZ_PAGE_SIZE = 15;
    std::vector<OgsPuzzleCollection> pz_collections_;   // current page
    int  pz_col_total_ = 0;
    int  pz_col_page_  = 1;                             // 1-based
    std::vector<std::pair<int, std::string>> pz_list_;  // puzzles in open collection
    std::string pz_list_title_;
    int  pz_index_  = 0;                                // browser cursor (per view)
    int  pz_list_pos_ = -1;                             // playing puzzle's index in pz_list_

    // Puzzles solved across sessions (ids; persisted to solved_puzzles.txt) —
    // shown as [X] checkmarks in the puzzle list.
    std::set<int> pz_solved_ids_;
    // Puzzle id → collection id, driving the collections-list coloring (yellow =
    // started, green = every puzzle solved). 0 = unknown: solves recorded before
    // this mapping existed; backfilled whenever their collection is next opened.
    // Persisted as a second column in solved_puzzles.txt.
    std::map<int, int> pz_solved_col_;
    int pz_open_col_id_ = 0;   // collection id of the pending/most recent list fetch
    void load_solved_puzzles();
    void save_solved_puzzles();
    // Collection id → how many of its puzzles are solved (from pz_solved_col_)
    std::map<int, int> pz_solved_per_collection() const {
        std::map<int, int> counts;
        for (const auto& kv : pz_solved_col_)
            if (kv.second > 0) counts[kv.second]++;
        return counts;
    }
    // Every collection ever opened, persisted to puzzle_collections.txt — the
    // metadata source for the pinned MY SETS section, which must be able to show
    // (and reopen) in-progress collections that live on OGS pages not currently
    // fetched. Recorded/refreshed each time a collection is opened.
    std::map<int, OgsPuzzleCollection> pz_known_cols_;
    void load_known_collections();
    void save_known_collections();
    // What the COLLECTIONS view actually shows and indexes: pinned in-progress
    // sets (name order) first, then the current server page minus duplicates.
    std::vector<OgsPuzzleCollection> pz_display_cols_;
    void pz_rebuild_display();

    OgsPuzzle             pz_;                 // puzzle being solved
    const PuzzleMoveNode* pz_node_  = nullptr; // current position in the solution tree
    bool        pz_done_   = false;            // solved or failed — judging over
    bool        pz_solved_ = false;
    bool        pz_explore_ = false;           // off the authored tree — free sandbox, no judging
    int         pz_explore_anchor_ = 0;        // history length to restore to on "back to solving"
    // Opponent-branch visit counts for the loaded puzzle: retries walk the LEAST
    // visited resistance line, so repeated attempts sweep every authored variation
    // in order instead of sampling randomly. Cleared when a new puzzle loads.
    std::map<const PuzzleMoveNode*, int> pz_visits_;
    bool pz_subtree_unexplored(const PuzzleMoveNode* n) const;  // any never-traversed node below?
    bool pz_more_lines_ = false;               // somewhere in the tree, a line hasn't been seen yet
    const PuzzleMoveNode* pz_pending_reply_ = nullptr;  // opponent reply chosen, awaiting the delay
    Uint32 pz_reply_at_ = 0;                            // SDL_GetTicks() deadline to apply it
    // Solution tree rendered in the left panel via the analysis-tree renderer
    std::vector<AnalysisTreeRenderNode> pz_tree_render_;
    int  pz_cur_depth_ = 0;
    void pz_build_tree_render();
    std::vector<BoardLabel> pz_marks_;         // current node's author marks (letter renderer)
    void pz_refresh_marks() {
        pz_marks_.clear();
        if (!pz_node_) return;
        for (const auto& m : pz_node_->marks)
            if (m.y >= 0 && m.y < game_.board_size && m.x >= 0 && m.x < game_.board_size)
                pz_marks_.push_back({m.y, m.x, m.ch});
    }
    std::string pz_banner_;                    // "SOLVED!" / "WRONG" big banner
    std::string pz_comment_;                   // author's comment at the current node

    // ── Life-and-death drill trees (local, user-authored) ─────────────────────
    // Authored in analysis mode (drill_correct marks + SAVE AS DRILL), stored as
    // variation SGFs in my_games/<user>/drills/, drilled through the puzzle
    // player via a synthesized local OgsPuzzle (id=0 — no solved-persistence).
    bool        drill_browse_ = false;         // puzzle browser PUZZLES view lists local drills
    std::vector<std::string> drill_paths_;     // parallel to pz_list_ while drill_browse_
    std::string drill_play_path_;              // file behind pz_ ("" = network OGS puzzle)
    std::string drill_edit_path_;              // overwrite target for SAVE ("" = save as new)
    // Setup-stone placement in analysis: -1 = off (normal alternating moves),
    // 1/0 = cross places/removes raw black/white stones on the CURRENT node's
    // board without creating tree nodes — for laying out a drill's initial
    // shape before recording any lines.
    int         analysis_setup_color_ = -1;
    // [MY DRILLS] list preview: the selected drill's setup position, parsed on
    // cursor move and cached (same BOARD_SIZE stride as the catalog thumbs).
    char        drill_thumb_[BOARD_SIZE][BOARD_SIZE] = {};
    int         drill_thumb_idx_ = -1;   // drill_paths_ index the cache holds (-1 = none)
    int         drill_thumb_bs_  = 0;    // 0 = nothing valid to draw
    // Renaming a drill file (keyboard text entry in the [MY DRILLS] list,
    // same plain-keycode capture idiom as the credential prompt)
    bool        drill_rename_active_ = false;
    std::string drill_rename_buf_;
    void        drill_commit_rename();
    void        drill_delete_selected();
    void        delete_analysis_branch();      // remove analysis_cur_'s subtree (hypothetical only)
    std::string drills_dir(bool create);
    bool save_drill(const std::string& path);
    void save_drill_from_current();
    void open_drill_list();
    void drill_load_and_start(int idx);
    void drill_edit_current();

    void open_puzzle_browser();
    void pz_launch_fetch(int kind, int arg);   // spawn worker for one of the 3 fetchers
    void poll_puzzle_fetch();                  // consume a finished fetch (event loop)
    void pz_start();                           // (re)set the board to pz_'s initial position
    void pz_place(int r, int f);               // player move → tree matching + feedback
    void pz_advance(const PuzzleMoveNode* node, bool opponent_follows);
    void pz_fire_pending_reply();               // apply the delayed opponent reply, then keep judging
    void pz_enter_explore();                   // triangle: free sandbox from the current position
    void pz_return_to_solving();               // circle, mid-exploration: back to the judged anchor
    void pz_step(int dir);                     // prev/next puzzle within the collection
    void handle_puzzle_button(Uint8 btn);      // PUZZLE_BROWSE + PUZZLE_PLAY input
    void draw_puzzle_browser();

    // ── Joseki explorer (OGS "OJE" position graph) ────────────────────────────
    // Walked node by node from "root"; every visited node is kept on a path
    // stack with a parallel board snapshot, so stepping back is instant and
    // re-advancing hits the in-memory cache instead of the network.
    struct JosekiFetch {
        std::atomic<bool> ready{false};
        bool ok = false;
        std::string    node_id;
        JosekiPosition pos;
    };
    std::shared_ptr<JosekiFetch> jk_fetch_;
    bool jk_loading_ = false;
    std::vector<JosekiPosition> jk_path_;    // root .. current node
    std::vector<GameState>      jk_boards_;  // board after each path node
    std::map<std::string, JosekiPosition> jk_cache_;   // node_id → fetched node
    std::string              jk_comment_;    // scrubbed description of current node
    std::vector<PointMarker> jk_markers_;    // continuation dots for current node
    void open_joseki_explorer();
    void jk_fetch_node(const std::string& node_id);
    void poll_joseki_fetch();
    void jk_arrive(const JosekiPosition& pos);  // push node + apply its move
    void jk_show_current();                     // markers/comment/status refresh
    void jk_step_back();
    void handle_joseki_button(Uint8 btn);

    // Returns the best available KataGo process for the given board size.
    KatagoProc& kata_for(int bs) {
        return (bs == 9 && kata_9_.running()) ? kata_9_ : kata_;
    }

    // KataGo GTP subprocess for local games vs the human SL model
    KataGoGtp   kata_gtp_;
    std::string kata_exe_;          // path from config (shared with analysis)
    std::string kata_model_;        // path from config (shared with analysis)
    std::string kata_human_model_;  // path to humanv0.bin.gz (enables VS KATAGO)

    // Adaptive KataGo strength: fractional rank index (0=20k … 19=1k, 20=1d … 28=9d),
    // nudged up after each win and down after each loss (scaled by the score margin),
    // persisted to adaptive_level.txt so it tracks Boris's level across sessions.
    float adaptive_rank_ = 10.0f;   // starting point: 10 KYU
    bool  adaptive_game_ = false;   // current local game uses the adaptive profile
    void  load_adaptive();
    void  save_adaptive();
    void  update_adaptive(const std::string& result);

    // Local game state
    bool        is_local_game_       = false;
    bool        local_prev_was_pass_ = false;  // true if the last move (by either side) was a pass
    std::string local_game_score_;             // score string shown during stone removal, empty until ownership arrives
    // Komi the current local game is actually played at: 7.5 for fresh games, the
    // review komi for practice-from-position games. Written into the saved SGF and
    // restored into review_komi_ when the game ends, so post-game analysis queries
    // use the komi the game was scored with.
    float       local_game_komi_     = 7.5f;

    void start_local_game();
    // "Practice vs KataGo" from the review menu: fork the current analysis position
    // into a live local game. The player takes the side to move at that position.
    void start_practice_from_position();
    // Free analysis: an empty 19x19 board in the normal GAME_OVER analysis mode
    // (tree panel, branching, engine toggle) — entered from LOBBY with triangle.
    void start_free_analysis();
    // Board size is single-select (radio) vs KataGo but multi-select for OGS search;
    // collapses match_menu_.size_sel[] down to exactly one entry — the first one
    // already checked, or 19x19 if none were — when entering KataGo mode.
    void normalize_size_sel_for_katago();
    // Write an SGF from game_.history for a local game (no OGS copy exists to fetch).
    void save_local_sgf(const std::string& path);
    void handle_katago_gtp_move(int row, int col);
    // forced_result: non-empty means the result is already known (e.g. resignation).
    // In that case skip the GTP final_status query and go straight to territory display.
    void begin_local_stone_removal(const std::string& forced_result = "");
    void end_local_game(const std::string& result);

    // Left-stick joystick cursor state
    Sint16 js_left_x_  = 0;
    Sint16 js_left_y_  = 0;

    // RT held state — while held during live play, reveals the last-played stone
    bool rt_down_ = false;

    bool init();
    void cleanup();
    void event_loop();
    void handle_controller_button(Uint8 button);
    void handle_net_msg(const NetMsg& msg);
    // forced_color: -1 = use current turn (normal), 0/1 = force that color and don't flip turn
    void apply_move(int col, int row, int forced_color = -1);
    void apply_pass();  // flips turn and records a history snapshot, same bookkeeping as apply_move
    void reset_byo_countdowns(bool running_player_too);  // stored secs -> full period (byo players)
    void undo_local_move();  // pop back to your last turn in a local game vs KataGo
    void step_history(int delta);  // delta=-1 back, +1 forward; sets history_pos
    void load_demo_game();
    void load_demo_from_path(const std::string& path);
    void save_live_game();
    void save_companion();          // write .katago file alongside the SGF
    void load_companion();          // read .katago file if present
    std::string marked_position_path(int depth) const;  // deterministic path in games/<user>/marked/
    void save_marked_position(int depth);   // write flattened static-position SGF for the marked move
    void delete_marked_position(int depth); // remove that SGF when unmarked
    void load_sgf_for_review(const std::string& path);
    void open_game_catalog();
    void open_pro_catalog();   // curated professional-game library, read-only
    // Delete an SGF (and its .katago companion) from disk, then refresh the
    // catalog listing in place. Called via double-press Y confirm in the catalog.
    void delete_catalog_game(const std::string& sgf_path);
    void open_settings_menu();
    void draw();
    void build_analysis_tree();
    void build_analysis_tree_render();
    void apply_analysis_move(int col, int row);
    bool is_legal_analysis_move(int col, int row) const;
    float cached_analysis_score(const AnalysisNode* node) const;

    void set_status(const std::string& s) { status_ = s; }

    Renderer::DrawState make_ds();
};

// ── Init / cleanup ────────────────────────────────────────────────────────────

bool App::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) return false;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    sound_.init();  // best-effort — app runs silent if no audio device

    window_ = SDL_CreateWindow("OGS Live",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_SIZE, SCREEN_SIZE,
        SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    sdl_rend_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window_ || !sdl_rend_) return false;

    renderer_ = new Renderer(sdl_rend_);
    SDL_ShowCursor(SDL_DISABLE);

    // Open first available controller
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            pad_ = SDL_GameControllerOpen(i);
            break;
        }
    }

    return true;
}

void App::cleanup() {
    save_companion();  // persist any in-progress review scores/marks before exiting
    sound_.shutdown();
    kata_.stop();
    kata_9_.stop();
    kata_gtp_.stop();
    net_.stop();
    if (pad_)      { SDL_GameControllerClose(pad_); pad_ = nullptr; }
    delete renderer_; renderer_ = nullptr;
    if (sdl_rend_) { SDL_DestroyRenderer(sdl_rend_); sdl_rend_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);      window_   = nullptr; }
    SDL_Quit();
}

// ── Board management ──────────────────────────────────────────────────────────

void App::apply_move(int col, int row, int forced_color) {
    if (col < 0 || row < 0) return;  // pass — board unchanged
    last_move_r_ = row;
    last_move_f_ = col;
    int is_black = (forced_color >= 0) ? forced_color : (int)game_.board.turn_is_black;
    game_.board.place_stone(row, col, is_black);
    game_.board.save_snapshot();
    if (forced_color >= 0) {
        game_.history.push_back(game_.board);
        if (move_scores_.size() < game_.history.size()) {
            move_scores_.push_back(FLT_MAX); move_marked_.push_back(false); marked_paths_.push_back("");
        }
        return;  // pre-placed handicap stone: caller sets turn after
    }
    // Free handicap: first `handicap` stones are all black; after last one, white plays
    if (game_.free_handicap && game_.board.stone_count <= game_.handicap) {
        game_.board.turn_is_black = (game_.board.stone_count < game_.handicap) ? 1 : 0;
    } else {
        game_.board.turn_is_black = !is_black;
    }
    game_.history.push_back(game_.board);
    if (move_scores_.size() < game_.history.size()) {
        move_scores_.push_back(FLT_MAX); move_marked_.push_back(false); marked_paths_.push_back("");
    }
}

// A pass has no stone to place, but still needs its own history snapshot with the same
// bookkeeping as a real move — skipping it desyncs history depth from the true move
// count (ko-check's "2 states ago" comparison, move_scores_/marked_paths_ indexing, and
// build_analysis_tree()'s per-node depth all silently drift once a pass is missing).
void App::apply_pass() {
    game_.board.turn_is_black = !game_.board.turn_is_black;
    game_.board.save_snapshot();
    game_.history.push_back(game_.board);
    if (move_scores_.size() < game_.history.size()) {
        move_scores_.push_back(FLT_MAX); move_marked_.push_back(false); marked_paths_.push_back("");
    }
}

// Byo-yomi: a period resets the instant a move is played — the player who just
// moved parks with a full period banked, and the player now to move starts a
// fresh one. But server move events carry period_time_left as a snapshot of the
// mover's just-ended turn (the leftover, e.g. 8s of a 30s period), and that
// stale remainder was being stored verbatim — so when the turn flipped back,
// the countdown started from 8s, hit zero, and began eating banked periods on
// screen while the server-side clock was actually fine. Normalize stored secs
// to the full period for byo players: always for the parked player (their
// stored value is never meaningful); for the running player too when a move
// was just applied (running_player_too=true — at a turn flip they start a
// fresh period by rule). Mid-turn clock updates keep running_player_too=false
// so a genuine in-progress countdown (e.g. reconnect) is preserved.
void App::reset_byo_countdowns(bool running_player_too) {
    bool btm   = (game_.board.turn_is_black == 1);
    bool b_byo = game_.black_in_byo || (game_.black_secs <= 0 && game_.black_periods > 0);
    bool w_byo = game_.white_in_byo || (game_.white_secs <= 0 && game_.white_periods > 0);
    if (b_byo && game_.black_period_secs > 0 && (running_player_too || !btm))
        game_.black_secs = game_.black_period_secs;
    if (w_byo && game_.white_period_secs > 0 && (running_player_too || btm))
        game_.white_secs = game_.white_period_secs;
}

// Pops history back to your own last turn in a local game vs KataGo — your last move
// plus whatever KataGo played in response (generically walks back ply-by-ply checking
// whose turn each resulting position is, rather than assuming a hardcoded pair, so it
// still does the right thing across a free-handicap run of same-color moves). Only
// safe to call when it's genuinely your turn: if KataGo is still "thinking" (a genmove
// request is in flight), pulling the position out from under it would leave its reply
// arriving for a position that no longer exists.
void App::undo_local_move() {
    if (!is_local_game_ || state_ != AppState::PLAYING) return;
    if (!game_.my_turn) {
        flash_       = "CAN'T UNDO — KATAGO IS THINKING";
        flash_until_ = SDL_GetTicks() + 1500;
        draw();
        return;
    }
    if (game_.history.size() <= 1) {
        flash_       = "NOTHING TO UNDO";
        flash_until_ = SDL_GetTicks() + 1500;
        draw();
        return;
    }

    while (game_.history.size() > 1) {
        game_.history.pop_back();
        // Keep the score arrays in strict lockstep with history — only pop entries
        // that actually correspond to the popped plies. (Guarding on size, not just
        // non-empty, means a desynced-longer array can't be drained past the moves
        // being undone.)
        while (move_scores_.size() > game_.history.size()) {
            move_scores_.pop_back();
            move_marked_.pop_back();
            marked_paths_.pop_back();
        }
        kata_gtp_.send_undo();
        if (game_.history.back().turn_is_black == game_.my_color) break;
    }
    // Rewind the background-scoring sweep: its pointer only ever moves forward, so
    // without this, replayed moves after an undo would never get scored (the sweep
    // would still think it was past them) — the "dead space" gap in the score graph.
    // The skip-loop fast-forwards over already-scored depths, so restarting from 0
    // costs nothing.
    bg_analysis_next_ = 0;
    // Puzzle evals for the popped depths describe positions that no longer exist;
    // the replayed line gets fresh ones when the sweep re-scores it. (Evals for
    // surviving depths — including the position we landed on — stay valid.)
    puzzle_eval_.erase(puzzle_eval_.lower_bound((int)game_.history.size()),
                       puzzle_eval_.end());

    game_.board    = game_.history.back();
    game_.my_turn  = true;
    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;

    // Recompute "was the last move a pass" and the last-played-stone highlight from
    // the position we landed on, rather than leaving them stale from before the undo.
    local_prev_was_pass_ = false;
    last_move_r_ = last_move_f_ = -1;
    if (game_.history.size() >= 2) {
        const auto& cur  = game_.history.back();
        const auto& prev = game_.history[game_.history.size() - 2];
        local_prev_was_pass_ = (memcmp(cur.board, prev.board, sizeof(cur.board)) == 0);
        for (int r = 0; r < game_.board_size && last_move_r_ < 0; r++)
            for (int f = 0; f < game_.board_size; f++)
                if (cur.board[r][f] != 0 && prev.board[r][f] == 0) {
                    last_move_r_ = r; last_move_f_ = f; break;
                }
    }

    flash_       = "UNDID YOUR MOVE";
    flash_until_ = SDL_GetTicks() + 1500;
    set_status(std::string("YOUR TURN  (") + (game_.my_color == 1 ? "BLACK" : "WHITE") + ")");
    draw();
}

void App::step_history(int delta) {
    if (game_.history.empty()) return;
    int max_idx = (int)game_.history.size() - 1;
    if (delta < 0) {
        if (game_.history_pos < 0)      game_.history_pos = std::max(0, max_idx - 1);
        else if (game_.history_pos > 0) game_.history_pos--;
    } else if (delta > 0) {
        if (game_.history_pos < 0)                    return;  // already live
        if (game_.history_pos >= max_idx - 1) game_.history_pos = -1;  // skip last (=live board)
        else                                  game_.history_pos++;
    }
}

void App::build_analysis_tree() {
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    // Every non-drill analysis entry (live game end, catalog review, free
    // analysis) funnels through here — a fresh tree is never an edit of a
    // previously saved drill, so SAVE must create a new file, not overwrite.
    // (drill_edit_current() also passes through here for its root, then
    // re-sets drill_edit_path_ afterwards.)
    drill_edit_path_.clear();
    analysis_setup_color_ = -1;   // setup-stone mode never carries across sessions

    analysis_root_ = std::make_unique<AnalysisNode>();
    analysis_root_->board = game_.history.empty() ? game_.board : game_.history[0];
    analysis_root_->depth = 0;

    int sz = game_.board_size;
    AnalysisNode* cur = analysis_root_.get();
    for (int i = 1; i < (int)game_.history.size(); i++) {
        auto child = std::make_unique<AnalysisNode>();
        child->board  = game_.history[i];
        child->depth  = i;
        child->parent = cur;

        // Identify which cell was placed so apply_analysis_move can dedup against it
        int mv_r = -1, mv_f = -1;
        for (int r = 0; r < sz && mv_r < 0; r++)
            for (int f = 0; f < sz && mv_r < 0; f++)
                if (game_.history[i].board[r][f] != 0 && cur->board.board[r][f] == 0)
                    { mv_r = r; mv_f = f; }
        child->move_row = mv_r;
        child->move_col = mv_f;
        // Read actual stone color from the board — immune to turn_is_black carry-over
        // from a previous game or handicap sequences where turn doesn't alternate normally.
        // Fall back to turn_is_black only for pass moves (no stone placed).
        child->move_color = (mv_r >= 0 && mv_f >= 0)
            ? ((game_.history[i].board[mv_r][mv_f] == 1) ? 1 : 0)
            : (int)cur->board.turn_is_black;

        cur->children.push_back(std::move(child));
        cur->active_child = 0;
        cur = cur->children[0].get();
    }
    analysis_cur_ = analysis_root_.get();  // open at start of game

    // Seed nodes on the main line with any scores already collected during play.
    {
        AnalysisNode* n = analysis_root_.get();
        while (n) {
            int d = n->depth;
            if (d < (int)move_scores_.size() && move_scores_[d] != FLT_MAX)
                n->score_lead = move_scores_[d];
            if (n->children.empty()) break;
            n = n->children[0].get();
        }
    }

    build_analysis_tree_render();
}

void App::build_analysis_tree_render() {
    analysis_tree_render_.clear();
    if (!analysis_root_) return;

    int max_col = 0;
    std::function<void(AnalysisNode*, int, int, int)> dfs =
        [&](AnalysisNode* node, int col, int parent_col, int parent_depth) {
            AnalysisTreeRenderNode rn;
            rn.depth        = node->depth;
            rn.col          = col;
            rn.current      = (node == analysis_cur_);
            rn.parent_depth = parent_depth;
            rn.parent_col   = parent_col;
            rn.move_color   = node->move_color;
            rn.marked       = (node->depth < (int)move_marked_.size()) && move_marked_[node->depth];
            rn.goal         = node->drill_correct;   // green halo, same as puzzle solution endpoints
            analysis_tree_render_.push_back(rn);

            for (int i = 0; i < (int)node->children.size(); i++) {
                int child_col = (i == 0) ? col : ++max_col;
                dfs(node->children[i].get(), child_col, col, node->depth);
            }
        };

    dfs(analysis_root_.get(), 0, 0, -1);
}

bool App::is_legal_analysis_move(int col, int row) const {
    if (!analysis_cur_) return false;
    const GameState& gs = analysis_cur_->board;
    int bs = game_.board_size;
    if (row < 0 || row >= bs || col < 0 || col >= bs) return false;
    if (gs.board[row][col] != 0) return false;
    int is_black = (gs.turn_is_black == 1) ? 1 : 0;
    if (GoRules::would_be_suicide(gs.board, row, col, is_black, bs)) return false;
    // Ko: simulated result must not recreate the position from one move ago (parent).
    // Mirrors the live game check: history[size()-2] = board before the last move.
    const AnalysisNode* prev = analysis_cur_->parent;
    if (prev) {
        char test[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
        memcpy(test, gs.board, sizeof(test));
        test[row][col] = is_black ? 1 : 2;
        int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE], cap_count = 0;
        GoRules::find_captured(test, is_black, row, col, cap_r, cap_f, cap_count, bs);
        for (int j = 0; j < cap_count; j++) test[cap_r[j]][cap_f[j]] = 0;
        if (memcmp(test, prev->board.board, sizeof(test)) == 0) return false;
    }
    return true;
}

// Returns an already-known score for this node without querying KataGo — the node's
// own score_lead if a foreground query has landed on it before, else move_scores_[depth]
// (populated by background scoring; exact for main-line nodes, a same-depth proxy off
// it). FLT_MAX if nothing is cached yet. Used to show "PROJECTED: ..." instantly on
// navigation instead of blanking out until the (now debounced) fresh query completes.
float App::cached_analysis_score(const AnalysisNode* node) const {
    if (!node) return FLT_MAX;
    if (node->score_lead != FLT_MAX) return node->score_lead;
    // move_scores_[] is main-line-only (see AnalysisNode::is_main_line) — for a branch
    // node it would just be some unrelated main-line position at the same depth, not a
    // real preview of this position, so don't fall back to it off the main line.
    if (node->is_main_line && node->depth < (int)move_scores_.size())
        return move_scores_[node->depth];
    return FLT_MAX;
}

void App::apply_analysis_move(int col, int row) {
    if (!analysis_cur_) return;

    // Navigate to existing child if this move was already played (always legal)
    for (int i = 0; i < (int)analysis_cur_->children.size(); i++) {
        if (analysis_cur_->children[i]->move_col == col &&
            analysis_cur_->children[i]->move_row == row) {
            analysis_cur_->active_child = i;
            analysis_cur_ = analysis_cur_->children[i].get();
            build_analysis_tree_render();
            return;
        }
    }

    // Ko / suicide check for new moves
    if (!is_legal_analysis_move(col, row)) {
        ko_flash_until_ = SDL_GetTicks() + 400;
        return;
    }

    // Create new child node — always a hypothetical branch. build_analysis_tree() is
    // the only place that creates real is_main_line=true nodes, from the actual played
    // game; any move placed interactively here, even one continuing past the end of
    // the recorded game, is exploration, not the real game.
    auto child = std::make_unique<AnalysisNode>();
    child->board        = analysis_cur_->board;
    child->move_col     = col;
    child->move_row     = row;
    child->depth        = analysis_cur_->depth + 1;
    child->parent       = analysis_cur_;
    child->is_main_line = false;

    // Apply the move using GameState::place_stone (handles captures & prisoners)
    int is_black = (child->board.turn_is_black == 1) ? 1 : 0;
    if (!child->board.place_stone(row, col, is_black)) return;  // illegal (occupied or suicide)
    child->move_color = is_black;
    child->board.turn_is_black = !is_black;

    int new_idx = (int)analysis_cur_->children.size();
    analysis_cur_->active_child = new_idx;
    analysis_cur_->children.push_back(std::move(child));
    analysis_cur_ = analysis_cur_->children[new_idx].get();
    build_analysis_tree_render();
}

// Remove the current node and its entire subtree from the analysis tree,
// landing on the parent. Hypothetical branches only — the popup item is gated
// on !is_main_line, and this re-checks for safety (deleting real game moves
// would silently rewrite the record being reviewed).
void App::delete_analysis_branch() {
    AnalysisNode* cur = analysis_cur_;
    if (!cur || !cur->parent || cur->is_main_line) return;
    AnalysisNode* parent = cur->parent;
    auto& sibs = parent->children;
    for (size_t i = 0; i < sibs.size(); i++) {
        if (sibs[i].get() == cur) {
            sibs.erase(sibs.begin() + i);   // unique_ptr — frees the whole subtree
            break;
        }
    }
    if (parent->active_child >= (int)sibs.size())
        parent->active_child = 0;
    analysis_cur_ = parent;
    build_analysis_tree_render();
    kata_suggestion_count_ = 0;
    kata_score_lead_  = cached_analysis_score(analysis_cur_);
    kata_query_after_ = SDL_GetTicks() + 1000;
    flash_       = "BRANCH DELETED";
    flash_until_ = SDL_GetTicks() + 1200;
}

// Load a specific SGF into the lobby screensaver (board + names, no analysis
// UI) — shared by load_demo_game()'s random pick and start_catalog_autoplay()'s
// scoped playlist.
void App::load_demo_from_path(const std::string& path) {
    demo_active_ = false;
    demo_.rows.clear();
    demo_.cols.clear();
    demo_.colors.clear();
    demo_.pos = 0;

    SgfGame g;
    if (!load_sgf(path, g) || g.move_count == 0) return;

    demo_.board_size = g.board_size;
    demo_.black_name = g.black_name;
    demo_.white_name = g.white_name;

    // Store just the move list — board is updated one move per second at runtime
    for (int i = 0; i < g.move_count; i++) {
        int r, f;
        if (!parse_sgf_move(g.moves[i], r, f)) continue;
        demo_.rows.push_back(r);
        demo_.cols.push_back(f);
        demo_.colors.push_back(g.colors[i]);
    }

    // Reset the board for this game
    demo_.board.reset();
    demo_.board.board_size   = g.board_size;
    demo_.board.turn_is_black = 1;

    demo_.next_tick = SDL_GetTicks() + 1000;
    demo_active_ = !demo_.rows.empty();
}

void App::load_demo_game() {
    // Merge the pro library (games/) and the user's own games (my_games/) into one
    // pool so the ambient screensaver keeps the same mixed breadth it always had,
    // back when both lived under the same games/ tree.
    std::vector<std::string> all_paths;
    auto collect = [&](const std::string& dirname) {
        // Try next to exe first, then one level up (dev layout: exe is in ogs_client/)
        std::string dir = exe_dir() + dirname;
        std::vector<std::string> rel;
        if (!Catalog::list_sgf_files(dir, rel) || rel.empty()) {
            dir = exe_dir() + "../../" + dirname;
            rel.clear();
            if (!Catalog::list_sgf_files(dir, rel) || rel.empty()) return;
        }
        for (const auto& r : rel) all_paths.push_back(Catalog::join_path(dir, r));
    };
    collect("games");
    collect("my_games");
    if (all_paths.empty()) return;

    // Pick a random file, skipping the one we just played if possible.
    static std::string last_path;
    std::srand((unsigned)std::time(nullptr) ^ (unsigned)SDL_GetTicks());
    std::string path;
    if (all_paths.size() == 1) {
        path = all_paths[0];
    } else {
        do { path = all_paths[(size_t)std::rand() % all_paths.size()]; }
        while (path == last_path);
    }
    last_path = path;
    load_demo_from_path(path);
}

// X in the catalog, on a game or a directory/player/year entry: play every
// file-type row currently shown, in order, as the lobby screensaver — e.g. an
// entire pro player's folder back to back for a screencap — starting at the
// row that was pressed. Works uniformly across flat-directory, virtual
// player/year, and search-result views, since all of them populate
// catalog_.entries with file rows (type 0) the same way.
void App::start_catalog_autoplay(int start_at_catalog_index) {
    std::vector<std::string> paths;
    int start_pos = 0;
    for (int i = 0; i < (int)catalog_.entries.size(); i++) {
        if (catalog_.entries[i].type != 0) continue;
        if (i == start_at_catalog_index) start_pos = (int)paths.size();
        std::string p = catalog_.entry_path(i);
        if (!p.empty()) paths.push_back(p);
    }
    if (paths.empty()) return;

    demo_playlist_     = std::move(paths);
    demo_playlist_pos_ = std::min(start_pos, (int)demo_playlist_.size() - 1);
    load_demo_from_path(demo_playlist_[demo_playlist_pos_]);
    catalog_.close();
    state_ = AppState::LOBBY;
    set_status("");
}

// ── Controller ────────────────────────────────────────────────────────────────

// Defined with the joseki explorer below; used by the LT/RT handler here.
static bool jk_parse_placement(const std::string& p, int bs, int& r, int& f);

void App::handle_controller_button(Uint8 btn) {
    // Credential prompt: not controller-driven (keyboard only for now)
    if (state_ == AppState::CREDENTIAL_PROMPT) return;

    // Catalog overlay: intercept all input while open
    if (catalog_.active) {
        const int CAT_VIS = 15;
        // Delete confirm is armed by Y and cancelled by any other button
        if (catalog_delete_confirm_ && btn != SDL_CONTROLLER_BUTTON_Y)
            catalog_delete_confirm_ = false;

        auto set_cat_index = [&](int i) {
            int n = (int)catalog_.entries.size();
            if (n == 0) return;
            catalog_.index = std::max(0, std::min(n - 1, i));
            catalog_.scroll = std::min(catalog_.scroll, catalog_.index);
            if (catalog_.index >= catalog_.scroll + CAT_VIS)
                catalog_.scroll = catalog_.index - CAT_VIS + 1;
            update_catalog_thumb();
        };
        // First letter of a row's sortable name (case-insensitive) — used by the
        // L2/R2 letter-scrub below. Meta-entries like "[BY YEAR]" compare on their
        // own bracketed label, which is fine since scrubbing just needs "looks
        // different from here", not a guarantee of strict A-Z order (the BY PLAYER
        // list itself sorts by game count first, name only as a tiebreaker).
        auto row_letter = [&](int i) -> char {
            const std::string& s = catalog_.entries[i].name.empty()
                                   ? catalog_.entries[i].display_name
                                   : catalog_.entries[i].name;
            return s.empty() ? '\0' : (char)toupper((unsigned char)s[0]);
        };

        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (catalog_.index > 0) set_cat_index(catalog_.index - 1);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (catalog_.index + 1 < (int)catalog_.entries.size()) set_cat_index(catalog_.index + 1);
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            set_cat_index(catalog_.index - 10);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            set_cat_index(catalog_.index + 10);
            break;
        case 0xFD:   // L2: scrub back to the previous row whose leading letter differs
        case 0xFE: { // R2: scrub forward to the next row whose leading letter differs
            int n = (int)catalog_.entries.size();
            if (n > 1) {
                int dir   = (btn == 0xFE) ? 1 : -1;
                char start = row_letter(catalog_.index);
                int i = catalog_.index;
                for (int steps = 0; steps < n; steps++) {
                    i += dir;
                    if (i < 0 || i >= n) break;   // clamp at the ends, don't wrap
                    if (row_letter(i) != start) { set_cat_index(i); break; }
                }
            }
            break;
        }
        case SDL_CONTROLLER_BUTTON_A:
            catalog_.select();
            if (catalog_.selection_made) {
                load_sgf_for_review(catalog_.selected_path);
                catalog_.selection_made = false;
            }
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            // Delete the selected game — double-press confirm, same idiom as
            // resign/pass/mark. Only file entries (not directories/meta-entries),
            // and never in the read-only pro game library.
            if (catalog_.selected_entry_path().empty()) break;
            if (catalog_readonly_) {
                flash_       = "READ-ONLY — CAN'T DELETE";
                flash_until_ = SDL_GetTicks() + 1500;
            } else if (!catalog_delete_confirm_) {
                catalog_delete_confirm_ = true;
                flash_       = "PRESS " GLYPH_PS_TRIANGLE " AGAIN TO DELETE GAME";
                flash_until_ = SDL_GetTicks() + 2500;
            } else {
                catalog_delete_confirm_ = false;
                delete_catalog_game(catalog_.selected_entry_path());
            }
            break;
        case SDL_CONTROLLER_BUTTON_X: {
            // Autoplay from here, presented as the lobby screensaver. On a game,
            // plays the list it's part of starting there. On a directory/player/
            // year entry, drills in first (same as A) and, if that landed on a
            // list of games, starts autoplaying it from the top — so pressing X
            // on a player's name in the BY PLAYER root list is a single press to
            // "play this player's whole folder."
            if (catalog_.index < 0 || catalog_.index >= (int)catalog_.entries.size()) break;
            const CatalogEntry& e = catalog_.entries[catalog_.index];
            if (e.type == 0) {
                start_catalog_autoplay(catalog_.index);
            } else if (e.type != 2) {   // not ".."
                catalog_.select();
                bool has_files = false;
                for (const auto& ce : catalog_.entries)
                    if (ce.type == 0) { has_files = true; break; }
                if (has_files) start_catalog_autoplay(0);
            }
            break;
        }
        default:
            // B, START, or anything else — close catalog
            catalog_.close();
            break;
        }
        draw();
        return;
    }

    // START popup menu: intercepts all input while open
    if (popup_active_) {
        handle_popup_button(btn);
        return;
    }

    // Typing a drill name — keyboard owns all input until Enter/ESC
    if (drill_rename_active_ && state_ == AppState::PUZZLE_BROWSE)
        return;

    // History navigation: LT/RT work in any game state
    if (btn == 0xFD || btn == 0xFE) {
        if (state_ == AppState::JOSEKI) {
            if (btn == 0xFD) {
                jk_step_back();
            } else if (!jk_loading_ && !jk_path_.empty()) {
                // R2 follows the book's main line: first IDEAL, else first GOOD
                const JosekiNextMove* pick = nullptr;
                for (const auto& m : jk_path_.back().next_moves) {
                    int r, f;
                    if (!jk_parse_placement(m.placement, 19, r, f)) continue;
                    if (m.category == "IDEAL") { pick = &m; break; }
                    if (!pick && m.category == "GOOD") pick = &m;
                }
                if (pick) jk_fetch_node(pick->node_id);
            }
            return;
        }

        // Lobby screensaver: step the game currently being shown forward or back,
        // instead of waiting on the automatic one-move-per-second tick. Works for
        // both the default random screensaver and a scoped catalog playlist.
        if (demo_active_ && (state_ == AppState::LOBBY || state_ == AppState::SEARCHING ||
                             state_ == AppState::CONNECTING)) {
            if (btn == 0xFE) {   // R2: forward one move
                if (demo_.pos < (int)demo_.rows.size()) {
                    demo_.board.place_stone(demo_.rows[demo_.pos], demo_.cols[demo_.pos],
                                            demo_.colors[demo_.pos]);
                    demo_.pos++;
                }
            } else {   // L2: back one move — no stored history, so replay from the
                       // start up to the new position (cheap; a few hundred moves
                       // at most, and Go capture replay is fast).
                if (demo_.pos > 0) {
                    demo_.pos--;
                    demo_.board.reset();
                    demo_.board.board_size    = demo_.board_size;
                    demo_.board.turn_is_black = 1;
                    for (int i = 0; i < demo_.pos; i++)
                        demo_.board.place_stone(demo_.rows[i], demo_.cols[i], demo_.colors[i]);
                }
            }
            demo_.next_tick = SDL_GetTicks() + 1000;  // don't also auto-advance right after
            draw();
            return;
        }

        bool has_game = (state_ == AppState::PLAYING ||
                         state_ == AppState::STONE_REMOVAL ||
                         state_ == AppState::GAME_OVER);
        if (has_game) {
            if (state_ == AppState::GAME_OVER && analysis_cur_) {
                // Navigate analysis tree
                if (btn == 0xFD) {
                    if (analysis_cur_->parent) {
                        analysis_cur_ = analysis_cur_->parent;
                        analysis_cur_->active_child = 0;  // RT follows main line after stepping back
                    }
                    build_analysis_tree_render();
                    kata_suggestion_count_ = 0;
                    kata_score_lead_ = cached_analysis_score(analysis_cur_);
                    kata_query_after_ = SDL_GetTicks() + 1000;
                } else {
                    analysis_step_forward();
                }
            } else {
                step_history(btn == 0xFD ? -1 : +1);
            }
            draw();
        } else if (state_ == AppState::PUZZLE_PLAY && pz_explore_) {
            // Review the sandbox moves made so far. Only wired while exploring —
            // pz_node_ is frozen there, so scrubbing never needs to re-resolve a
            // tree position; placing a stone from mid-review branches off via the
            // history_pos handling already in pz_place().
            step_history(btn == 0xFD ? -1 : +1);
            draw();
        }
        return;
    }

    // Y button: mark/unmark the current move for analysis attention.
    // Marking saves a standalone SGF snapshot of the position to games/<user>/marked/,
    // and unmarking deletes it — both are file operations, so both require a confirm press.
    // Scoped to the states where marking exists — an unconditional catch here would
    // swallow triangle everywhere (it silently ate LOBBY's free-analysis binding).
    if (btn == SDL_CONTROLLER_BUTTON_Y &&
        (state_ == AppState::PLAYING || state_ == AppState::GAME_OVER)) {
        int depth = -1;
        if (state_ == AppState::PLAYING) {
            depth = (game_.history_pos >= 0) ? game_.history_pos
                                             : (int)game_.history.size() - 1;
        } else if (state_ == AppState::GAME_OVER && analysis_cur_) {
            depth = analysis_cur_->depth;
        }
        if (depth >= 0 && depth < (int)move_marked_.size()) {
            if (!mark_confirm_) {
                mark_confirm_ = true;
                flash_       = move_marked_[depth] ? "PRESS " GLYPH_PS_TRIANGLE " AGAIN TO UNMARK"
                                                   : "PRESS " GLYPH_PS_TRIANGLE " AGAIN TO MARK";
                flash_until_ = SDL_GetTicks() + 2000;
                draw();
                return;
            }
            mark_confirm_ = false;
            move_marked_[depth] = !move_marked_[depth];
            if (move_marked_[depth]) {
                save_marked_position(depth);
                flash_ = "MARKED";
            } else {
                delete_marked_position(depth);
                flash_ = "UNMARKED";
            }
            flash_until_ = SDL_GetTicks() + 1200;
            if (state_ == AppState::GAME_OVER) build_analysis_tree_render();
        }
        draw();
        return;
    }

    // Undo request from opponent: A = accept, B or START = deny
    if (undo_pending_ && state_ == AppState::PLAYING) {
        if (btn == SDL_CONTROLLER_BUTTON_A) {
            undo_pending_ = false;
            net_.cmd_accept_undo(game_.game_id, undo_move_number_);
            if ((int)game_.history.size() > 1) {
                game_.history.pop_back();
                game_.board = game_.history.back();
            }
            game_.history_pos = -1;
            game_.pending_col = -2;
            game_.pending_row = -2;
            pass_confirm_    = false;
            bool btp = (game_.board.turn_is_black == 1);
            game_.my_turn = (btp && game_.my_color == 1) || (!btp && game_.my_color == 0);
            set_status(game_.my_turn ? "YOUR TURN" : "WAITING...");
        } else if (btn == SDL_CONTROLLER_BUTTON_B || btn == SDL_CONTROLLER_BUTTON_START) {
            undo_pending_ = false;
            net_.cmd_reject_undo(game_.game_id, undo_move_number_);
        }
        draw();
        return;
    }

    // Cancel pass confirm on any non-B button
    if (pass_confirm_ && btn != SDL_CONTROLLER_BUTTON_B) {
        pass_confirm_ = false;
        draw();
        return;
    }
    // Cancel mark confirm on any non-Y button
    if (mark_confirm_ && btn != SDL_CONTROLLER_BUTTON_Y) {
        mark_confirm_ = false;
        draw();
        return;
    }
    // Cancel find-match confirm on any non-A button
    if (find_match_confirm_ && btn != SDL_CONTROLLER_BUTTON_A) {
        find_match_confirm_ = false;
        draw();
        return;
    }

    // BACK: open the settings menu from anywhere — the same button that used to open
    // it from the lobby, standardized across every state (catalog interception and
    // CREDENTIAL_PROMPT's early return above already rule those out; MATCH_MENU itself
    // handles BACK as its own close action, in that switch below).
    if (btn == SDL_CONTROLLER_BUTTON_BACK && state_ != AppState::MATCH_MENU) {
        open_settings_menu();
        return;
    }

    if (state_ == AppState::PUZZLE_BROWSE || state_ == AppState::PUZZLE_PLAY) {
        handle_puzzle_button(btn);
        return;
    }

    if (state_ == AppState::JOSEKI) {
        handle_joseki_button(btn);
        return;
    }

    if (state_ == AppState::LOBBY) {
        if (btn == SDL_CONTROLLER_BUTTON_X) {
            open_game_catalog();
            draw();
        } else if (btn == SDL_CONTROLLER_BUTTON_Y) {
            start_free_analysis();
        } else if (btn == SDL_CONTROLLER_BUTTON_B) {
            open_puzzle_browser();
        } else if (btn == SDL_CONTROLLER_BUTTON_START) {
            open_popup_menu();
        } else if (btn == SDL_CONTROLLER_BUTTON_A) {
            // Finding a match queues a real opponent immediately, so a single press
            // was too easy to trigger by accident — double-press confirm, same
            // idiom as resign/pass/mark. START -> FIND MATCH also still works.
            if (!find_match_confirm_) {
                find_match_confirm_ = true;
                flash_       = "PRESS " GLYPH_PS_CROSS " AGAIN TO SEARCH FOR MATCH";
                flash_until_ = SDL_GetTicks() + 2500;
            } else {
                find_match_confirm_ = false;
                net_.cmd_find_match(match_prefs_);
                state_ = AppState::SEARCHING;
                set_status("SEARCHING...");
            }
            draw();
        }
        return;
    }

    if (state_ == AppState::MATCH_MENU) {
        // Ingame: only the DISPLAY column exists (col 0) — board size/speed/mode don't
        // apply to the game already in progress. From LOBBY: 3 columns as before, plus
        // DISPLAY as a 3rd column so everything really is in "the one menu".
        int display_col  = match_menu_.ingame ? 0 : 2;
        int col_count    = match_menu_.ingame ? 1 : 3;
        const int DISPLAY_ROWS = 5;  // SHOW COORDINATES, ENGINE ANALYSIS, CHAIN LINKS, SQUARE STONES, SQUARE GRID
        int col_sizes[3];
        if (match_menu_.ingame) {
            col_sizes[0] = DISPLAY_ROWS;
        } else {
            col_sizes[0] = 3;
            col_sizes[1] = match_menu_.katago_mode ? 8 : 3;  // 7 fixed ranks + ADAPTIVE
            col_sizes[2] = DISPLAY_ROWS;
        }
        int n = col_sizes[match_menu_.focus_col];

        auto save_prefs = [&]() {
            for (int i = 0; i < 3; i++) match_prefs_.sizes[i]  = match_menu_.size_sel[i];
            for (int i = 0; i < 3; i++) match_prefs_.speeds[i] = match_menu_.speed_sel[i];
            match_prefs_.katago_mode = match_menu_.katago_mode;
            match_prefs_.katago_str  = match_menu_.katago_str;
        };
        auto close_menu = [&]() {
            save_prefs();
            save_settings();   // persist match prefs + this visit's DISPLAY toggles
            if (match_menu_.ingame) {
                state_ = pre_menu_state_;
            } else {
                state_ = AppState::LOBBY;
                set_status("");
            }
            draw();
        };
        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            match_menu_.focus_row = (match_menu_.focus_row - 1 + n) % n;
            renderer_->draw_match_menu(match_menu_);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            match_menu_.focus_row = (match_menu_.focus_row + 1) % n;
            renderer_->draw_match_menu(match_menu_);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            if (match_menu_.focus_col > 0) match_menu_.focus_col--;
            match_menu_.focus_row = std::min(match_menu_.focus_row, col_sizes[match_menu_.focus_col] - 1);
            renderer_->draw_match_menu(match_menu_);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            if (match_menu_.focus_col < col_count - 1) match_menu_.focus_col++;
            match_menu_.focus_row = std::min(match_menu_.focus_row, col_sizes[match_menu_.focus_col] - 1);
            renderer_->draw_match_menu(match_menu_);
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            // Toggle OGS / KataGo mode (only when human model is configured; doesn't
            // apply mid-game since only the DISPLAY column is shown there). Only two
            // categories exist, so LB and RB both just flip it either direction.
            if (!match_menu_.ingame && !kata_human_model_.empty()) {
                match_menu_.katago_mode = !match_menu_.katago_mode;
                if (match_menu_.katago_mode) normalize_size_sel_for_katago();
                // Clamp row when switching to OGS mode's smaller column
                if (!match_menu_.katago_mode && match_menu_.focus_col == 1
                        && match_menu_.focus_row >= 3)
                    match_menu_.focus_row = 2;
                renderer_->draw_match_menu(match_menu_);
            }
            break;
        case SDL_CONTROLLER_BUTTON_A: {
            int r = match_menu_.focus_row;
            if (match_menu_.focus_col == display_col) {
                if (r == 0) {
                    match_menu_.show_coords_sel = !match_menu_.show_coords_sel;
                    show_coords_ = match_menu_.show_coords_sel;
                } else if (r == 1) {
                    // Greyed out and inert when no KataGo process is running at all —
                    // matches VS KATAGO/PRACTICE VS KATAGO's existing pattern of not
                    // offering a control that can't do anything.
                    if (!match_menu_.analysis_available) { /* no-op */ }
                    else {
                        match_menu_.analysis_sel = !match_menu_.analysis_sel;
                        kata_analysis_enabled_   = match_menu_.analysis_sel;
                        if (kata_analysis_enabled_) {
                            kata_query_after_ = SDL_GetTicks() + 1000;
                        } else {
                            kata_suggestion_count_ = 0;
                            kata_score_lead_ = FLT_MAX;
                            kata_query_after_ = 0;
                        }
                    }
                } else if (r == 2) {
                    match_menu_.chain_sel = !match_menu_.chain_sel;
                    chain_mode_           = match_menu_.chain_sel;
                } else if (r == 3) {
                    match_menu_.square_sel = !match_menu_.square_sel;
                    square_stones_         = match_menu_.square_sel;
                } else {
                    match_menu_.square_grid_sel = !match_menu_.square_grid_sel;
                    square_grid_                = match_menu_.square_grid_sel;
                }
            } else if (match_menu_.focus_col == 0) {
                if (match_menu_.katago_mode) {
                    for (int i = 0; i < 3; i++) match_menu_.size_sel[i] = (i == r);  // radio
                } else {
                    match_menu_.size_sel[r] = !match_menu_.size_sel[r];
                }
            } else if (match_menu_.katago_mode) {
                match_menu_.katago_str = r;   // radio — single select
            } else {
                match_menu_.speed_sel[r] = !match_menu_.speed_sel[r];
            }
            renderer_->draw_match_menu(match_menu_);
            break;
        }
        case SDL_CONTROLLER_BUTTON_BACK:
        case SDL_CONTROLLER_BUTTON_B:
            close_menu();
            break;
        case SDL_CONTROLLER_BUTTON_START:
            // Starting a search/local game from here was too easy to trigger by
            // accident — this menu only edits preferences now; START just closes
            // it (same as B/BACK), matching how it already worked mid-game. FIND
            // MATCH is reachable only through the LOBBY's own popup/confirm.
            close_menu();
            break;
        default: break;
        }
        return;
    }

    if (state_ == AppState::SEARCHING) {
        if (btn == SDL_CONTROLLER_BUTTON_B) {
            net_.cmd_cancel_match();
            state_ = AppState::LOBBY;
            set_status("");
            draw();
        } else if (btn == SDL_CONTROLLER_BUTTON_START) {
            open_popup_menu();
        }
        return;
    }

    if (state_ == AppState::PLAYING) {
        int n = game_.board_size - 1;
        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            game_.cursor_r = std::max(0, game_.cursor_r - 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            game_.cursor_r = std::min(n, game_.cursor_r + 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            game_.cursor_f = std::max(0, game_.cursor_f - 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            game_.cursor_f = std::min(n, game_.cursor_f + 1); draw(); break;

        case SDL_CONTROLLER_BUTTON_A:
            if (game_.history_pos >= 0) { game_.history_pos = -1; draw(); break; }  // exit history
            if (!game_.my_turn) break;
            if (game_.board.board[game_.cursor_r][game_.cursor_f] != 0) break;
            if (GoRules::would_be_suicide(game_.board.board,
                    game_.cursor_r, game_.cursor_f, game_.my_color,
                    game_.board_size)) break;
            // Ko check: simulate the move and compare against the board 2 states ago
            if (game_.history.size() >= 2) {
                char test[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
                memcpy(test, game_.board.board, sizeof(test));
                test[game_.cursor_r][game_.cursor_f] = game_.my_color ? 1 : 2;
                int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
                int cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
                int cap_count = 0;
                GoRules::find_captured(test, game_.my_color, game_.cursor_r, game_.cursor_f,
                                       cap_r, cap_f, cap_count, game_.board_size);
                for (int i = 0; i < cap_count; i++) test[cap_r[i]][cap_f[i]] = 0;
                const auto& prev = game_.history[game_.history.size() - 2];
                if (memcmp(test, prev.board, sizeof(test)) == 0) {
                    ko_flash_until_ = SDL_GetTicks() + 400;
                    draw();
                    break;
                }
            }
            if (is_local_game_) {
                apply_move(game_.cursor_f, game_.cursor_r);
                local_prev_was_pass_ = false;
                game_.my_turn = false;
                kata_gtp_.send_play(game_.my_color, game_.cursor_r, game_.cursor_f,
                                    game_.board_size);
                kata_gtp_.request_genmove(1 - game_.my_color);
                set_status("KATAGO THINKING...");
            } else {
                net_.cmd_send_move(game_.game_id, game_.cursor_f, game_.cursor_r);
                // Apply optimistically so stone appears immediately; server echo skipped
                apply_move(game_.cursor_f, game_.cursor_r);
                game_.pending_col = game_.cursor_f;
                game_.pending_row = game_.cursor_r;
                game_.my_turn = false;
                // Fresh byo-yomi periods from the moment the move is played —
                // don't wait on the server echo to stop a stale countdown.
                reset_byo_countdowns(/*running_player_too=*/true);
                game_.clock_tick = SDL_GetTicks();
                set_status("WAITING...");
            }
            draw();
            break;

        case SDL_CONTROLLER_BUTTON_B:
            if (!game_.my_turn) break;
            if (pass_confirm_) {
                pass_confirm_ = false;
                if (is_local_game_) {
                    apply_pass();
                    kata_gtp_.send_play(game_.my_color, -1, -1, game_.board_size);
                    local_prev_was_pass_ = true;
                    game_.my_turn = false;
                    kata_gtp_.request_genmove(1 - game_.my_color);
                    set_status("PASSED — KATAGO THINKING...");
                } else {
                    net_.cmd_send_pass(game_.game_id);
                    apply_pass();  // optimistic, same pattern as apply_move for real moves
                    game_.pending_col = -1;
                    game_.pending_row = -1;
                    game_.my_turn = false;
                    // Same byo-yomi reset as a real move — a pass consumes the turn too
                    reset_byo_countdowns(/*running_player_too=*/true);
                    game_.clock_tick = SDL_GetTicks();
                    set_status("PASSED — WAITING...");
                }
            } else {
                pass_confirm_ = true;
            }
            draw();
            break;

        case SDL_CONTROLLER_BUTTON_START:
            open_popup_menu();
            break;

        case SDL_CONTROLLER_BUTTON_X:
            undo_local_move();
            break;

        default: break;
        }
        return;
    }

    if (state_ == AppState::STONE_REMOVAL) {
        if (btn == SDL_CONTROLLER_BUTTON_START)
            open_popup_menu();
        else if (btn == SDL_CONTROLLER_BUTTON_A)
            accept_stone_removal();
        return;
    }

    if (state_ == AppState::GAME_OVER) {
        int n = game_.board_size - 1;
        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            game_.cursor_r = std::max(0, game_.cursor_r - 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            game_.cursor_r = std::min(n, game_.cursor_r + 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            game_.cursor_f = std::max(0, game_.cursor_f - 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            game_.cursor_f = std::min(n, game_.cursor_f + 1); draw(); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            // Jump between ALL branches that reach the current move number, anywhere
            // in the tree — not just siblings of the nearest fork. (The old nearest-
            // fork version couldn't reach a branch whose common ancestor was farther
            // up than the last fork below the cursor.)
            if (analysis_cur_ && analysis_root_) {
                int target_depth = analysis_cur_->depth;
                // Collect every node at this depth, in DFS order (matches the tree
                // panel's column order, so the cycle direction feels consistent).
                std::vector<AnalysisNode*> peers;
                std::function<void(AnalysisNode*)> walk = [&](AnalysisNode* node) {
                    if (node->depth == target_depth) {
                        peers.push_back(node);
                        return;  // children are all deeper — no need to descend
                    }
                    for (auto& c : node->children) walk(c.get());
                };
                walk(analysis_root_.get());

                if (peers.size() > 1) {
                    int cur_idx = 0;
                    for (int i = 0; i < (int)peers.size(); i++)
                        if (peers[i] == analysis_cur_) { cur_idx = i; break; }
                    int delta = (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 1 : -1;
                    AnalysisNode* dest =
                        peers[(cur_idx + delta + (int)peers.size()) % (int)peers.size()];

                    // Re-aim every ancestor's active_child at the path to the new
                    // node, so LT/RT stepping follows the branch we just landed on.
                    for (AnalysisNode* p = dest; p->parent; p = p->parent)
                        for (int i = 0; i < (int)p->parent->children.size(); i++)
                            if (p->parent->children[i].get() == p) {
                                p->parent->active_child = i;
                                break;
                            }

                    analysis_cur_ = dest;
                    build_analysis_tree_render();
                    kata_suggestion_count_ = 0;
                    kata_score_lead_ = cached_analysis_score(analysis_cur_);
                    kata_query_after_ = SDL_GetTicks() + 1000;
                    draw();
                }
            }
            break;
        case SDL_CONTROLLER_BUTTON_A:
            if (analysis_cur_) {
                int col = game_.cursor_f, row = game_.cursor_r;
                // Setup-stone mode: edit the current node's board directly —
                // raw add/remove, no tree nodes, no captures, no turn change.
                // Gated on a childless position (children derive their boards
                // from this one; editing under them would desync the tree).
                if (analysis_setup_color_ >= 0) {
                    if (!analysis_cur_->children.empty()) {
                        flash_       = "POSITION HAS CONTINUATIONS — DELETE THEM FIRST";
                        flash_until_ = SDL_GetTicks() + 2000;
                    } else if (analysis_cur_->board.board[row][col] != 0) {
                        analysis_cur_->board.board[row][col] = 0;   // remove any stone
                    } else {
                        analysis_cur_->board.board[row][col] =
                            (analysis_setup_color_ == 1) ? 1 : 2;
                    }
                    draw();
                    break;
                }
                if (analysis_cur_->board.board[row][col] == 0) {
                    apply_analysis_move(col, row);
                    kata_suggestion_count_ = 0;
                    kata_score_lead_ = FLT_MAX;
                    if (kata_analysis_enabled_) {
                        kata_query_after_ = 0;
                        kata_for(game_.board_size).query_moves(
                            analysis_cur_->board.board, game_.board_size,
                            analysis_cur_->board.turn_is_black == 1, review_komi_);
                    }
                    draw();
                }
            }
            break;
        case SDL_CONTROLLER_BUTTON_B:
            // Letter labels: circle toggles an "A"/"B"/"C"… mark on the cursor point.
            // (Stepping back is L2's job — circle's old step-back was redundant.)
            if (analysis_cur_) {
                auto& labels = analysis_cur_->labels;
                int r = game_.cursor_r, f = game_.cursor_f;
                bool removed = false;
                for (size_t i = 0; i < labels.size(); i++)
                    if (labels[i].r == r && labels[i].f == f) {
                        labels.erase(labels.begin() + i);   // second press removes
                        removed = true;
                        break;
                    }
                if (!removed) {
                    bool used[26] = {};
                    for (const auto& l : labels)
                        if (l.ch >= 'A' && l.ch <= 'Z') used[l.ch - 'A'] = true;
                    for (int c = 0; c < 26; c++)
                        if (!used[c]) {                     // lowest unused letter
                            labels.push_back({r, f, char('A' + c)});
                            break;
                        }
                }
                draw();
            }
            break;
        case SDL_CONTROLLER_BUTTON_X:
            open_game_catalog();
            draw();
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            review_cycle(-1);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            // L3/R3: jump to the previous/next SGF in the same directory as the
            // currently open review — cycle marked positions or study puzzles
            // without a round trip through the catalog.
            review_cycle(+1);
            break;
        case SDL_CONTROLLER_BUTTON_START:
            open_popup_menu();
            break;
        }
        return;
    }
}

// ── START popup menu ─────────────────────────────────────────────────────────

// Rebuild the display labels from the item list; the armed row (awaiting its
// confirm press) is rewritten as a question so the state is visible in place.
void App::popup_sync_labels() {
    popup_labels_.clear();
    for (int i = 0; i < (int)popup_items_.size(); i++)
        popup_labels_.push_back(i == popup_armed_
                                    ? "REALLY " + popup_items_[i].label + "?"
                                    : popup_items_[i].label);
}

void App::close_popup_menu() {
    popup_active_ = false;
    popup_items_.clear();
    popup_labels_.clear();
    popup_index_ = 0;
    popup_armed_ = -1;
}

void App::open_popup_menu() {
    popup_items_.clear();
    auto add = [&](std::string label, std::function<void()> fn, bool confirm = false) {
        popup_items_.push_back({std::move(label), confirm, std::move(fn)});
    };
    auto lobby = [this]() {
        state_ = AppState::LOBBY;
        set_status("");
    };

    switch (state_) {
    case AppState::LOBBY:
        popup_title_ = "LOBBY";
        // Separate items rather than one relabeled by match_prefs_.katago_mode —
        // both are always available regardless of whichever mode the settings
        // menu last happened to be showing.
        add("FIND MATCH", [this]() {
            net_.cmd_find_match(match_prefs_);
            state_ = AppState::SEARCHING;
            set_status("SEARCHING...");
        });
        if (!kata_human_model_.empty())
            add("PLAY VS KATAGO", [this]() { start_local_game(); });
        add("MATCH SETTINGS",  [this]() { open_settings_menu(); });
        add("GAME CATALOG",    [this]() { open_game_catalog(); });
        add("PRO GAMES",       [this]() { open_pro_catalog(); });
        add("OGS PUZZLES",     [this]() { open_puzzle_browser(); });
        add("JOSEKI EXPLORER", [this]() { open_joseki_explorer(); });
        add("FREE ANALYSIS",   [this]() { start_free_analysis(); });
        break;

    case AppState::SEARCHING:
        popup_title_ = "SEARCHING";
        add("CANCEL SEARCH", [this, lobby]() {
            net_.cmd_cancel_match();
            lobby();
        });
        break;

    case AppState::PLAYING:
        popup_title_ = "GAME MENU";
        if (is_local_game_)
            add("UNDO MOVE", [this]() { undo_local_move(); });
        add("RESIGN", [this]() { do_resign(); }, /*confirm=*/true);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::STONE_REMOVAL:
        popup_title_ = "SCORING";
        add(is_local_game_ ? "SHOW RESULT" : "ACCEPT SCORE",
            [this]() { accept_stone_removal(); });
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::GAME_OVER:
        popup_title_ = "REVIEW MENU";
        add("RETURN TO LOBBY", [this]() { return_to_lobby(); });
        // Fork the position on screen into a live game vs KataGo (needs the human
        // SL model, same gate as the match menu's VS KATAGO mode). Strength comes
        // from the configured KataGo strength in match settings.
        if (analysis_cur_ && !kata_human_model_.empty())
            add("PRACTICE VS KATAGO", [this]() { start_practice_from_position(); });
        // ── Drill authoring (life-and-death drill trees) ──
        if (analysis_cur_ && analysis_cur_->parent)   // the root can't be an ending
            add(analysis_cur_->drill_correct ? "UNMARK CORRECT ENDING"
                                             : "MARK CORRECT ENDING",
                [this]() {
                    analysis_cur_->drill_correct = !analysis_cur_->drill_correct;
                    build_analysis_tree_render();
                });
        // Setup-stone mode: cross places/removes raw stones of one color on the
        // current node's board (no tree nodes, no alternation) — for laying out
        // a drill's initial shape. Only allowed while the position has no
        // continuations, since children's boards derive from this one.
        if (analysis_cur_ && analysis_cur_->children.empty()) {
            add(analysis_setup_color_ == 1 ? "SETUP BLACK STONES (ON — TURN OFF)"
                                           : "SETUP BLACK STONES",
                [this]() {
                    analysis_setup_color_ = (analysis_setup_color_ == 1) ? -1 : 1;
                    set_status(analysis_setup_color_ == 1
                                   ? "SETUP: " GLYPH_PS_CROSS " ADDS/REMOVES BLACK STONES"
                                   : "");
                });
            add(analysis_setup_color_ == 0 ? "SETUP WHITE STONES (ON — TURN OFF)"
                                           : "SETUP WHITE STONES",
                [this]() {
                    analysis_setup_color_ = (analysis_setup_color_ == 0) ? -1 : 0;
                    set_status(analysis_setup_color_ == 0
                                   ? "SETUP: " GLYPH_PS_CROSS " ADDS/REMOVES WHITE STONES"
                                   : "");
                });
        }
        // Flip whose turn it is at the current node (which side the drill's
        // first move belongs to). Only future children read this flag.
        if (analysis_cur_)
            add(analysis_cur_->board.turn_is_black == 1 ? "SWITCH SIDE TO MOVE (NOW: BLACK)"
                                                        : "SWITCH SIDE TO MOVE (NOW: WHITE)",
                [this]() {
                    analysis_cur_->board.turn_is_black =
                        (analysis_cur_->board.turn_is_black == 1) ? 0 : 1;
                });
        // Prune an accidental line: removes the current node and everything
        // below it, landing on the parent. Hypothetical branches only — the
        // real game's moves (is_main_line) are never deletable.
        if (analysis_cur_ && analysis_cur_->parent && !analysis_cur_->is_main_line)
            add("DELETE THIS BRANCH", [this]() { delete_analysis_branch(); },
                /*confirm=*/true);
        if (analysis_cur_ && !analysis_cur_->children.empty())
            add(drill_edit_path_.empty() ? "SAVE AS DRILL" : "OVERWRITE DRILL",
                [this]() { save_drill_from_current(); });
        add("GAME CATALOG",    [this]() { open_game_catalog(); });
        add("PRO GAMES",       [this]() { open_pro_catalog(); });
        add("SETTINGS",        [this]() { open_settings_menu(); });
        break;

    case AppState::PUZZLE_BROWSE:
        popup_title_ = "PUZZLES";
        // Drill file management, only with a drill highlighted in [MY DRILLS]
        if (drill_browse_ && pz_view_ == PzView::PUZZLES &&
            pz_index_ >= 0 && pz_index_ < (int)drill_paths_.size()) {
            add("RENAME DRILL (KEYBOARD)", [this]() {
                drill_rename_buf_    = pz_list_[pz_index_].second;   // prefill current name
                drill_rename_active_ = true;
            });
            add("DELETE DRILL", [this]() { drill_delete_selected(); },
                /*confirm=*/true);
        }
        add("RETURN TO LOBBY", lobby);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::JOSEKI:
        popup_title_ = "JOSEKI";
        add("RESTART FROM EMPTY BOARD", [this]() { open_joseki_explorer(); });
        add("RETURN TO LOBBY", lobby);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    case AppState::PUZZLE_PLAY:
        popup_title_ = "PUZZLE MENU";
        add("RETRY PUZZLE", [this]() { pz_start(); });
        if (!drill_play_path_.empty())
            add("EDIT DRILL", [this]() { drill_edit_current(); });
        add("PUZZLE LIST", [this]() {
            state_    = AppState::PUZZLE_BROWSE;
            pz_view_  = pz_list_.empty() ? PzView::COLLECTIONS : PzView::PUZZLES;
            pz_index_ = std::max(0, pz_list_pos_);
        });
        add("RETURN TO LOBBY", lobby);
        add("SETTINGS", [this]() { open_settings_menu(); });
        break;

    default:
        return;  // no popup in this state
    }

    popup_active_ = true;
    popup_index_  = 0;
    popup_armed_  = -1;
    popup_sync_labels();
    draw();
}

void App::handle_popup_button(Uint8 btn) {
    int n = (int)popup_items_.size();
    if (n == 0) { close_popup_menu(); draw(); return; }
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        popup_index_ = (popup_index_ - 1 + n) % n;
        popup_armed_ = -1;
        popup_sync_labels();
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        popup_index_ = (popup_index_ + 1) % n;
        popup_armed_ = -1;
        popup_sync_labels();
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_A: {
        PopupItem& it = popup_items_[popup_index_];
        if (it.confirm && popup_armed_ != popup_index_) {
            popup_armed_ = popup_index_;
            popup_sync_labels();
            draw();
            break;
        }
        auto action = it.action;  // keep alive — close_popup_menu clears the items
        close_popup_menu();
        if (action) action();
        draw();
        break;
    }
    case SDL_CONTROLLER_BUTTON_B:
    case SDL_CONTROLLER_BUTTON_BACK:
    case SDL_CONTROLLER_BUTTON_START:
        close_popup_menu();
        draw();
        break;
    default: break;
    }
}

void App::do_resign() {
    if (is_local_game_) {
        // Player resigned → the other color wins
        end_local_game(std::string(game_.my_color == 1 ? "W" : "B") + "+R");
    } else {
        net_.cmd_send_resign(game_.game_id);
        set_status("RESIGNED");
        draw();
    }
}

void App::accept_stone_removal() {
    if (is_local_game_) {
        if (!local_game_score_.empty())
            end_local_game(local_game_score_);
        // else still analyzing — ignore the press
        return;
    }
    if (!stone_removal_has_ogs_territory_) {
        // Server hasn't sent its own dead-stone detection yet — what's on screen
        // is only our local KataGo guess. Sending accept now would tell the
        // server we accept an empty/wrong stone set, not what's displayed.
        flash_       = "WAITING FOR SERVER SCORE...";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    net_.cmd_accept_stones(game_.game_id);
    my_accept_sent_   = true;
    accept_resend_at_ = SDL_GetTicks() + 6000;
    set_status("ACCEPTING... WAITING FOR OPPONENT");
    draw();
}

void App::return_to_lobby() {
    save_companion();  // persist scores + marks before leaving review
    is_local_game_ = false;
    state_ = AppState::LOBBY;
    game_.history_pos = -1;
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    analysis_tree_render_.clear();
    kata_suggestion_count_ = 0;
    kata_score_lead_ = FLT_MAX;
    kata_query_after_ = 0;
    kata_analysis_enabled_ = true;
    set_status("");
    game_.board.reset();
    load_demo_game();
}

// ── SGF save (fetched from OGS API) ──────────────────────────────────────────

static std::string sgf_sanitize(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum((unsigned char)c) || c == '-') out += c;
        else if (c == ' ' || c == '_')                  out += '_';
    }
    return out;
}

void App::save_live_game() {
    // Locate my_games/ directory — personal games, kept separate from the pro
    // library in games/ (same two-path probe as everywhere else)
    std::string games_dir = exe_dir() + "my_games";
    auto is_dir = [](const std::string& p) {
        DWORD a = GetFileAttributesW(Catalog::utf8_to_wide(p).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (!is_dir(games_dir)) games_dir = exe_dir() + "../../my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir();

    std::string my_name  = (game_.my_color == 1) ? game_.black_name : game_.white_name;
    std::string opp_name = (game_.my_color == 1) ? game_.white_name : game_.black_name;
    std::string player_dir = Catalog::join_path(games_dir, my_name);
    CreateDirectoryW(Catalog::utf8_to_wide(games_dir).c_str(), nullptr);
    CreateDirectoryW(Catalog::utf8_to_wide(player_dir).c_str(), nullptr);

    // Local games (vs KataGo) have game_id 0 — there is nothing to fetch from OGS
    // (the old code tried anyway, got HTTP 404, and silently saved no SGF at all,
    // leaving the .katago companion orphaned and the game impossible to reopen).
    // Write the SGF locally from our own move history instead — into a katago/
    // subdirectory so practice games don't mingle with the real OGS archive.
    // Local filenames get a time component: the opponent label is just the
    // strength ("15 KYU"), so two same-strength games on the same day would
    // otherwise overwrite each other.
    bool local = (game_.game_id == 0);
    std::string save_dir = player_dir;
    if (local) {
        save_dir = Catalog::join_path(player_dir, "katago");
        CreateDirectoryW(Catalog::utf8_to_wide(save_dir).c_str(), nullptr);
    }

    time_t t = time(nullptr);
    char date[32];
    strftime(date, sizeof(date), local ? "%Y%m%d-%H%M%S" : "%Y%m%d", localtime(&t));
    std::string filename = std::string(date) + "-"
                         + sgf_sanitize(my_name) + "-"
                         + sgf_sanitize(opp_name) + ".sgf";
    std::string path = Catalog::join_path(save_dir, filename);

    // Derive companion path before writing/fetching the SGF
    companion_path_ = path.substr(0, path.rfind('.')) + ".katago";

    if (local) {
        save_local_sgf(path);
        return;
    }

    int game_id = game_.game_id;
    std::thread([this, game_id, path] {
        net_.fetch_sgf(game_id, path);
    }).detach();
}

// Write an SGF for a locally-played game by diffing consecutive history snapshots
// (the same move-recovery approach build_analysis_tree() uses). A snapshot pair with
// no added stone is a pass.
void App::save_local_sgf(const std::string& path) {
    if (game_.history.size() < 2) return;  // no moves — nothing worth saving

    FILE* f = Catalog::fopen_utf8(path, "wb");
    if (!f) return;

    char today[16];
    time_t t = time(nullptr);
    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&t));
    fprintf(f, "(;GM[1]FF[4]CA[UTF-8]SZ[%d]KM[%.1f]PB[%s]PW[%s]DT[%s]",
            game_.board_size, local_game_komi_,
            game_.black_name.c_str(), game_.white_name.c_str(), today);
    if (!game_.result.empty())
        fprintf(f, "RE[%s]", game_.result.c_str());

    // Practice-from-position games start from a non-empty board: write those
    // stones as AB[]/AW[] setup (not moves), plus PL[] for the side to move —
    // exactly the shape load_sgf() already parses for marked positions.
    const GameState& start = game_.history[0];
    bool has_setup = false;
    for (int color = 1; color <= 2; color++) {
        bool wrote_prop = false;
        for (int r = 0; r < game_.board_size; r++)
            for (int fcol = 0; fcol < game_.board_size; fcol++)
                if (start.board[r][fcol] == color) {
                    if (!wrote_prop) {
                        fprintf(f, "%s", color == 1 ? "AB" : "AW");
                        wrote_prop = true;
                        has_setup  = true;
                    }
                    fprintf(f, "[%c%c]", 'a' + fcol, 'a' + r);
                }
    }
    if (has_setup)
        fprintf(f, "PL[%c]", start.turn_is_black == 1 ? 'B' : 'W');
    fprintf(f, "\n");

    for (size_t i = 1; i < game_.history.size(); i++) {
        const GameState& prev = game_.history[i - 1];
        const GameState& cur  = game_.history[i];
        int mr = -1, mf = -1;
        for (int r = 0; r < game_.board_size && mr < 0; r++)
            for (int fcol = 0; fcol < game_.board_size; fcol++)
                if (cur.board[r][fcol] != 0 && prev.board[r][fcol] == 0) {
                    mr = r; mf = fcol; break;
                }
        // Mover color: the stone value where one was added, else (for a pass) whoever's
        // turn it was in the pre-move snapshot. Board values: 1 = black, 2 = white.
        int is_black = (mr >= 0) ? (cur.board[mr][mf] == 1) : (prev.turn_is_black == 1);
        if (mr >= 0)
            fprintf(f, ";%c[%c%c]", is_black ? 'B' : 'W', 'a' + mf, 'a' + mr);
        else
            fprintf(f, ";%c[]", is_black ? 'B' : 'W');
    }
    fprintf(f, ")\n");
    fclose(f);
}

// ── Marked positions (standalone flattened-SGF snapshots) ───────────────────

std::string App::marked_position_path(int depth) const {
    std::string games_dir = exe_dir() + "my_games";
    auto is_dir = [](const std::string& p) {
        DWORD a = GetFileAttributesW(Catalog::utf8_to_wide(p).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (!is_dir(games_dir)) games_dir = exe_dir() + "../../my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir();

    std::string player_dir = Catalog::join_path(games_dir, my_username_.empty() ? "You" : my_username_);
    std::string marked_dir = Catalog::join_path(player_dir, "marked");

    std::string opp_name = (game_.my_color == 1) ? game_.white_name : game_.black_name;
    time_t t = time(nullptr);
    char date[16];
    strftime(date, sizeof(date), "%Y%m%d", localtime(&t));
    std::string filename = std::string(date) + "-" + sgf_sanitize(opp_name)
                          + "-move" + std::to_string(depth) + ".sgf";
    return Catalog::join_path(marked_dir, filename);
}

void App::save_marked_position(int depth) {
    const GameState* gs = nullptr;
    if (state_ == AppState::PLAYING) {
        if (depth >= 0 && depth < (int)game_.history.size()) gs = &game_.history[depth];
    } else if (state_ == AppState::GAME_OVER && analysis_cur_) {
        gs = &analysis_cur_->board;
    }
    if (!gs) return;

    std::string path = marked_position_path(depth);
    std::string marked_dir = path.substr(0, path.find_last_of("/\\"));
    std::string player_dir = marked_dir.substr(0, marked_dir.find_last_of("/\\"));
    std::string games_dir  = player_dir.substr(0, player_dir.find_last_of("/\\"));
    CreateDirectoryW(Catalog::utf8_to_wide(games_dir).c_str(), nullptr);
    CreateDirectoryW(Catalog::utf8_to_wide(player_dir).c_str(), nullptr);
    CreateDirectoryW(Catalog::utf8_to_wide(marked_dir).c_str(), nullptr);

    FILE* f = Catalog::fopen_utf8(path, "w");
    if (!f) return;

    auto sgf_escape = [](const std::string& s) {
        std::string out;
        for (char c : s) { if (c == ']' || c == '\\') out += '\\'; out += c; }
        return out;
    };

    int n = gs->board_size;
    fprintf(f, "(;GM[1]FF[4]CA[UTF-8]SZ[%d]", n);
    for (int color = 1; color >= 0; color--) {  // black setup stones first, then white
        bool any = false;
        for (int r = 0; r < n && !any; r++)
            for (int c = 0; c < n; c++)
                if (gs->board[r][c] == (color ? 1 : 2)) { any = true; break; }
        if (!any) continue;
        fprintf(f, "%s", color ? "AB" : "AW");
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++)
                if (gs->board[r][c] == (color ? 1 : 2))
                    fprintf(f, "[%c%c]", char('a' + c), char('a' + r));
    }
    fprintf(f, "PL[%s]", gs->turn_is_black ? "B" : "W");
    fprintf(f, "PB[%s]PW[%s]", sgf_escape(game_.black_name).c_str(), sgf_escape(game_.white_name).c_str());
    time_t t = time(nullptr);
    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", localtime(&t));
    fprintf(f, "DT[%s]", date);
    fprintf(f, "C[Marked position, move %d]", depth);
    fprintf(f, ")\n");
    fclose(f);

    if (depth < (int)marked_paths_.size()) marked_paths_[depth] = path;
}

void App::delete_marked_position(int depth) {
    if (depth < 0 || depth >= (int)marked_paths_.size()) return;
    if (marked_paths_[depth].empty()) return;  // marked in a prior session — no known path to clean up
    remove(marked_paths_[depth].c_str());
    marked_paths_[depth].clear();
}

// ── Life-and-death drill trees ───────────────────────────────────────────────
// Authored in analysis mode, saved as standard variation SGFs, drilled through
// the puzzle player. Format: root = AB/AW/PL setup (same shape as marked
// positions) + GN, tree = nested parens of ;B[xy]/;W[xy], correct endings
// marked C[RIGHT] (unmarked dead-ends are implicitly wrong — matches the
// puzzle player's judging).

static constexpr int DRILL_MAX_DEPTH = 400;

static std::string sgf_escape_text(const std::string& s) {
    std::string out;
    for (char c : s) { if (c == ']' || c == '\\') out += '\\'; out += c; }
    return out;
}

std::string App::drills_dir(bool create) {
    std::string games_dir = exe_dir() + "my_games";
    auto is_dir = [](const std::string& p) {
        DWORD a = GetFileAttributesW(Catalog::utf8_to_wide(p).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (!is_dir(games_dir)) games_dir = exe_dir() + "../../my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir();
    std::string player_dir = Catalog::join_path(games_dir, my_username_.empty() ? "You" : my_username_);
    std::string dir        = Catalog::join_path(player_dir, "drills");
    if (create) {
        CreateDirectoryW(Catalog::utf8_to_wide(games_dir).c_str(), nullptr);
        CreateDirectoryW(Catalog::utf8_to_wide(player_dir).c_str(), nullptr);
        CreateDirectoryW(Catalog::utf8_to_wide(dir).c_str(), nullptr);
    }
    return dir;
}

// Validate the drill subtree below `n`: strictly alternating colors, no passes,
// bounded depth; notes whether any node is marked as a correct ending.
static bool drill_validate(const AnalysisNode* n, int expect_color,
                           bool& any_correct, int depth) {
    if (depth > DRILL_MAX_DEPTH) return false;
    for (const auto& ch : n->children) {
        if (ch->move_col < 0 || ch->move_row < 0) return false;   // pass node
        if (ch->move_color != expect_color)       return false;   // non-alternating
        if (ch->drill_correct) any_correct = true;
        if (!drill_validate(ch.get(), 1 - expect_color, any_correct, depth + 1))
            return false;
    }
    return true;
}

// Emit the subtree below `n`: a single child chains inline (;B[..];W[..]),
// multiple children each get their own (...) variation.
static void drill_write(FILE* f, const AnalysisNode* n) {
    const AnalysisNode* cur = n;
    while (true) {
        if (cur->children.empty()) return;
        if (cur->children.size() == 1) {
            const AnalysisNode* ch = cur->children[0].get();
            fprintf(f, ";%c[%c%c]", ch->move_color == 1 ? 'B' : 'W',
                    char('a' + ch->move_col), char('a' + ch->move_row));
            if (ch->drill_correct) fprintf(f, "C[RIGHT]");
            for (const auto& lb : ch->labels)
                fprintf(f, "LB[%c%c:%c]", char('a' + lb.f), char('a' + lb.r), lb.ch);
            cur = ch;   // chain without recursing
        } else {
            for (const auto& chp : cur->children) {
                const AnalysisNode* ch = chp.get();
                fprintf(f, "(;%c[%c%c]", ch->move_color == 1 ? 'B' : 'W',
                        char('a' + ch->move_col), char('a' + ch->move_row));
                if (ch->drill_correct) fprintf(f, "C[RIGHT]");
                for (const auto& lb : ch->labels)
                    fprintf(f, "LB[%c%c:%c]", char('a' + lb.f), char('a' + lb.r), lb.ch);
                drill_write(f, ch);
                fprintf(f, ")");
            }
            return;
        }
    }
}

bool App::save_drill(const std::string& path) {
    if (!analysis_cur_ || analysis_cur_->children.empty()) {
        flash_       = "NOTHING TO SAVE — PLAY OUT SOME LINES FIRST";
        flash_until_ = SDL_GetTicks() + 2500;
        return false;
    }
    const GameState& root = analysis_cur_->board;   // reference — GameState is huge
    bool any_correct = false;
    int  first_color = (root.turn_is_black == 1) ? 1 : 0;
    if (!drill_validate(analysis_cur_, first_color, any_correct, 0)) {
        flash_       = "DRILL MUST ALTERNATE COLORS, NO PASSES";
        flash_until_ = SDL_GetTicks() + 2500;
        return false;
    }
    if (!any_correct) {
        flash_       = "MARK A CORRECT ENDING FIRST";
        flash_until_ = SDL_GetTicks() + 2500;
        return false;
    }

    FILE* f = Catalog::fopen_utf8(path, "w");
    if (!f) {
        flash_       = "COULDN'T WRITE DRILL FILE";
        flash_until_ = SDL_GetTicks() + 2500;
        return false;
    }
    int n = root.board_size;
    fprintf(f, "(;GM[1]FF[4]CA[UTF-8]SZ[%d]", n);
    for (int color = 1; color >= 0; color--) {  // black setup stones first, then white
        bool any = false;
        for (int r = 0; r < n && !any; r++)
            for (int c = 0; c < n; c++)
                if (root.board[r][c] == (color ? 1 : 2)) { any = true; break; }
        if (!any) continue;
        fprintf(f, "%s", color ? "AB" : "AW");
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++)
                if (root.board[r][c] == (color ? 1 : 2))
                    fprintf(f, "[%c%c]", char('a' + c), char('a' + r));
    }
    fprintf(f, "PL[%s]", root.turn_is_black ? "B" : "W");
    // GN = filename stem, for display in the drill list
    std::string stem = path;
    size_t slash = stem.find_last_of("/\\");
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    fprintf(f, "GN[%s]", sgf_escape_text(stem).c_str());
    time_t t = time(nullptr);
    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", localtime(&t));
    fprintf(f, "DT[%s]", date);
    fprintf(f, "C[%s TO PLAY]", root.turn_is_black ? "BLACK" : "WHITE");
    drill_write(f, analysis_cur_);
    fprintf(f, ")\n");
    fclose(f);
    return true;
}

void App::save_drill_from_current() {
    std::string path = drill_edit_path_;
    if (path.empty()) {
        time_t t = time(nullptr);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
        path = Catalog::join_path(drills_dir(true), std::string("drill-") + stamp + ".sgf");
    }
    if (save_drill(path)) {
        drill_edit_path_ = path;
        flash_       = "DRILL SAVED";
        flash_until_ = SDL_GetTicks() + 2000;
    }
    draw();
}

// ── Drill SGF parser (variation-preserving) ─────────────────────────────────
// A separate parser from load_sgf(), which deliberately flattens variations —
// drills need the whole tree. Handles the subset our writer emits plus sane
// hand edits: root setup props, nested (…) variations, ;B[xy]/;W[xy] chains,
// C[RIGHT] correct marks, LB[] labels. Rejects passes, non-alternating
// colors, and pathological nesting.

// Read one "[...]" property value (assumes *p == '['), unescaping \] and \\.
static const char* drill_read_value(const char* p, std::string& out) {
    out.clear();
    p++;  // '['
    while (*p && *p != ']') {
        if (*p == '\\') { p++; if (!*p) break; }
        out += *p++;
    }
    return (*p == ']') ? p + 1 : p;
}

static void drill_trim(std::string& s) {
    while (!s.empty() && isspace((unsigned char)s.back()))  s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    s.erase(0, i);
}

// Parse "Sequence {GameTree}" — everything between a consumed '(' and its ')'.
// New move nodes chain onto *attach; sibling (…) subtrees attach to the current
// chain end. expect_color = color the next move node must have (1=B, 0=W).
static bool drill_parse_seq(const char*& p, PuzzleMoveNode* attach, int expect_color,
                            int move_depth, int nest_depth, int bs, std::string& err) {
    if (nest_depth > DRILL_MAX_DEPTH) { err = "TOO DEEPLY NESTED"; return false; }
    bool seen_subtree = false;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ')') { p++; return true; }
        if (*p == '(') {
            p++;
            seen_subtree = true;
            if (!drill_parse_seq(p, attach, expect_color, move_depth,
                                 nest_depth + 1, bs, err))
                return false;
            continue;
        }
        if (*p == ';') {
            if (seen_subtree) { err = "NODE AFTER VARIATION"; return false; }
            p++;
            // One node: a run of properties, each IDENT + one or more [values]
            PuzzleMoveNode* made = nullptr;
            while (*p) {
                while (*p && isspace((unsigned char)*p)) p++;
                if (!isupper((unsigned char)*p)) break;   // node/tree boundary
                std::string ident;
                while (isalpha((unsigned char)*p)) ident += *p++;
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p != '[') { err = "PROPERTY WITHOUT VALUE"; return false; }
                // Collect all values of this property
                std::vector<std::string> vals;
                while (*p == '[') {
                    std::string v;
                    p = drill_read_value(p, v);
                    vals.push_back(std::move(v));
                    while (*p && isspace((unsigned char)*p)) p++;
                }
                if (ident == "B" || ident == "W") {
                    if (made) { err = "TWO MOVES IN ONE NODE"; return false; }
                    int color = (ident == "B") ? 1 : 0;
                    if (color != expect_color) { err = "NON-ALTERNATING DRILL TREE"; return false; }
                    const std::string& v = vals[0];
                    bool is_pass = v.empty() || (bs <= 19 && v == "tt");
                    if (is_pass) { err = "PASSES NOT SUPPORTED"; return false; }
                    if (v.size() != 2) { err = "BAD MOVE COORD"; return false; }
                    int x = v[0] - 'a', y = v[1] - 'a';
                    if (x < 0 || x >= bs || y < 0 || y >= bs) { err = "MOVE OFF BOARD"; return false; }
                    if (move_depth + 1 > DRILL_MAX_DEPTH) { err = "DRILL TOO DEEP"; return false; }
                    attach->branches.emplace_back();
                    made = &attach->branches.back();
                    made->x = x;
                    made->y = y;
                    attach = made;
                    expect_color = 1 - expect_color;
                    move_depth++;
                } else if (ident == "C" && made) {
                    std::string c = vals[0];
                    drill_trim(c);
                    if (c == "RIGHT") made->correct = true;
                    else              made->text    = vals[0];
                } else if (ident == "LB" && made) {
                    for (const auto& v : vals) {
                        // "xy:C"
                        if (v.size() >= 4 && v[2] == ':') {
                            int x = v[0] - 'a', y = v[1] - 'a';
                            if (x >= 0 && x < bs && y >= 0 && y < bs)
                                made->marks.push_back({x, y, v[3]});
                        }
                    }
                }
                // all other properties: values already consumed, ignore
            }
            continue;
        }
        // Unexpected character — malformed
        err = "MALFORMED DRILL SGF";
        return false;
    }
    err = "UNTERMINATED DRILL SGF";
    return false;
}

// Load a drill SGF into a synthesized local OgsPuzzle (id=0 — never persisted
// as an OGS solve). Returns false with no partial state on any parse error.
static bool load_drill_sgf(const std::string& path, OgsPuzzle& out_pz, std::string& err) {
    FILE* fp = Catalog::fopen_utf8(path, "rb");
    if (!fp) { err = "CAN'T OPEN"; return false; }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    rewind(fp);
    if (fsz <= 0 || fsz > 8 * 1024 * 1024) { fclose(fp); err = "BAD FILE SIZE"; return false; }
    std::vector<char> buf((size_t)fsz + 1);
    size_t nread = fread(buf.data(), 1, (size_t)fsz, fp);
    fclose(fp);
    buf[nread] = '\0';

    OgsPuzzle pz;   // build locally; assign to out_pz only on full success
    const char* p = buf.data();
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') { err = "NOT AN SGF"; return false; }
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ';') { err = "NO ROOT NODE"; return false; }
    p++;

    // Root node: setup properties
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!isupper((unsigned char)*p)) break;   // end of root properties
        std::string ident;
        while (isalpha((unsigned char)*p)) ident += *p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '[') { err = "PROPERTY WITHOUT VALUE"; return false; }
        std::vector<std::string> vals;
        while (*p == '[') {
            std::string v;
            p = drill_read_value(p, v);
            vals.push_back(std::move(v));
            while (*p && isspace((unsigned char)*p)) p++;
        }
        if (ident == "SZ") {
            int sz = atoi(vals[0].c_str());
            pz.width = pz.height = sz;
        } else if (ident == "AB" || ident == "AW") {
            std::string& dst = (ident == "AB") ? pz.initial_black : pz.initial_white;
            for (const auto& v : vals)
                if (v.size() == 2) dst += v;
        } else if (ident == "PL") {
            pz.black_to_play = !vals[0].empty() && (vals[0][0] == 'B' || vals[0][0] == 'b' || vals[0][0] == '1');
        } else if (ident == "GN") {
            pz.name = vals[0];
        } else if (ident == "C") {
            pz.description = vals[0];
        } else if (ident == "B" || ident == "W") {
            err = "MOVE IN ROOT NODE";   // our format keeps the root move-free
            return false;
        }
        // everything else (GM/FF/CA/DT/PB/PW/KM/...) ignored
    }

    if (pz.width != pz.height || pz.width < 2 || pz.width > MAX_BOARD_SIZE) {
        err = "BAD BOARD SIZE";
        return false;
    }

    // Variation tree: chained nodes + sibling subtrees until the closing ')'
    int first_color = pz.black_to_play ? 1 : 0;
    if (!drill_parse_seq(p, &pz.tree, first_color, 0, 0, pz.width, err))
        return false;
    if (pz.tree.branches.empty()) { err = "NO MOVES IN DRILL"; return false; }
    pz.tree.correct = false;   // a C[RIGHT] on the root would insta-solve

    pz.id              = 0;      // local — disables solved-persistence
    pz.opponent_auto   = true;
    pz.type            = "life_and_death";
    pz.rank            = 0;
    pz.collection_id   = 0;
    pz.collection_name = "MY DRILLS";
    if (pz.name.empty()) {
        std::string stem = path;
        size_t slash = stem.find_last_of("/\\");
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        pz.name = stem;
    }
    if (pz.description.empty())
        pz.description = pz.black_to_play ? "BLACK TO PLAY" : "WHITE TO PLAY";

    out_pz = std::move(pz);
    return true;
}

// Apply a random rotation/reflection (one of the 8 board symmetries) to a
// loaded drill — setup stones, every tree move, and author marks together —
// so a shape isn't only ever learned in the corner it was authored in.
// The board cursor (cur_f/cur_r, may be null) rides along through the same
// map, so the player's hand stays on the shape instead of chasing it.
// Exact integer maps; composing another one on a later retry is still a
// symmetry, so repeated in-place application never drifts.
static void drill_random_transform(OgsPuzzle& pz, int* cur_f, int* cur_r) {
    int op = rand() % 8;
    if (op == 0) return;   // identity
    int n = pz.width;
    auto tf = [op, n](int& x, int& y) {
        int ox = x, oy = y;
        switch (op) {
        case 1: x = n - 1 - ox; y = oy;          break;  // mirror horizontally
        case 2: x = ox;         y = n - 1 - oy;  break;  // mirror vertically
        case 3: x = n - 1 - ox; y = n - 1 - oy;  break;  // rotate 180
        case 4: x = oy;         y = ox;          break;  // transpose
        case 5: x = oy;         y = n - 1 - ox;  break;  // rotate 90
        case 6: x = n - 1 - oy; y = ox;          break;  // rotate 270
        case 7: x = n - 1 - oy; y = n - 1 - ox;  break;  // anti-transpose
        }
    };
    auto tf_coords = [&](std::string& s) {
        for (size_t i = 0; i + 1 < s.size(); i += 2) {
            int x = s[i] - 'a', y = s[i + 1] - 'a';
            if (x < 0 || x >= n || y < 0 || y >= n) continue;
            tf(x, y);
            s[i]     = char('a' + x);
            s[i + 1] = char('a' + y);
        }
    };
    tf_coords(pz.initial_black);
    tf_coords(pz.initial_white);
    std::function<void(PuzzleMoveNode&)> rec = [&](PuzzleMoveNode& nd) {
        if (nd.x >= 0 && nd.y >= 0) tf(nd.x, nd.y);
        for (auto& m : nd.marks)
            if (m.x >= 0 && m.y >= 0) tf(m.x, m.y);
        for (auto& b : nd.branches) rec(b);
    };
    rec(pz.tree);
    if (cur_f && cur_r &&
        *cur_f >= 0 && *cur_f < n && *cur_r >= 0 && *cur_r < n)
        tf(*cur_f, *cur_r);
}

// ── Auto-detected study puzzles ──────────────────────────────────────────────

// Evaluate depth d ("position before the move played at d") against the study-
// puzzle criterion: it was my turn, exactly one move kept the game, and I played
// something else that lost it. Uses only data the background sweep already
// produced — no extra KataGo queries. Called only once the game is over.
void App::check_puzzle(int d) {
    if (game_.my_color != 0 && game_.my_color != 1) return;   // review — not my game
    if (puzzle_saved_.count(d)) return;
    if (d < 0 || d + 1 >= (int)game_.history.size()) return;
    auto it = puzzle_eval_.find(d);
    if (it == puzzle_eval_.end() || it->second.best_sl == FLT_MAX) return;
    const GameState& pos = game_.history[d];
    if ((int)pos.turn_is_black != game_.my_color) return;     // opponent's move
    if (d + 1 >= (int)move_scores_.size() || move_scores_[d + 1] == FLT_MAX) return;

    const float SAVABLE    = -2.0f;  // best move leaves me at worst ~2 pts behind
    const float ONLY_MOVE  =  8.0f;  // every alternative loses at least this much more
    const float AFTER_LOST = -7.0f;  // my actual move left me at least this far behind

    float mp        = (game_.my_color == 1) ? 1.f : -1.f;     // to my perspective
    float best_my   = mp * it->second.best_sl;
    float second_my = (it->second.second_sl == FLT_MAX) ? -1e9f : mp * it->second.second_sl;
    float after_my  = mp * move_scores_[d + 1];

    if (best_my < SAVABLE)               return;  // game was already gone
    if (second_my > best_my - ONLY_MOVE) return;  // more than one way to save it
    if (after_my > AFTER_LOST)           return;  // my move didn't actually lose it

    // Direct coordinate check as a final guard: if I somehow played the saving
    // move itself (and the score drop came from elsewhere/noise), don't mark.
    int pr = -1, pf = -1;
    const GameState& nxt = game_.history[d + 1];
    for (int r = 0; r < pos.board_size && pr < 0; r++)
        for (int c = 0; c < pos.board_size; c++)
            if (nxt.board[r][c] != 0 && pos.board[r][c] == 0) { pr = r; pf = c; break; }
    if (pr == it->second.best_r && pf == it->second.best_f) return;

    save_puzzle_position(d);
    puzzle_saved_.insert(d);
    flash_       = "STUDY PUZZLE SAVED (MOVE " + std::to_string(d) + ")";
    flash_until_ = SDL_GetTicks() + 2500;
}

// Write the puzzle as a flattened setup SGF (same format as manual marked
// positions) into games/<user>/puzzles/. The saving move is deliberately NOT
// recorded anywhere in the file — reviewing the puzzle with engine analysis
// reveals the answer on demand instead of spoiling it up front.
void App::save_puzzle_position(int depth) {
    if (depth < 0 || depth >= (int)game_.history.size()) return;
    const GameState* gs = &game_.history[depth];

    std::string games_dir = exe_dir() + "my_games";
    auto is_dir = [](const std::string& p) {
        DWORD a = GetFileAttributesW(Catalog::utf8_to_wide(p).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (!is_dir(games_dir)) games_dir = exe_dir() + "../../my_games";
    if (!is_dir(games_dir)) games_dir = exe_dir();
    std::string player_dir = Catalog::join_path(games_dir, my_username_.empty() ? "You" : my_username_);
    std::string puzzle_dir = Catalog::join_path(player_dir, "puzzles");
    CreateDirectoryW(Catalog::utf8_to_wide(player_dir).c_str(), nullptr);
    CreateDirectoryW(Catalog::utf8_to_wide(puzzle_dir).c_str(), nullptr);

    std::string opp_name = (game_.my_color == 1) ? game_.white_name : game_.black_name;
    time_t t = time(nullptr);
    char date[32];
    strftime(date, sizeof(date), "%Y%m%d-%H%M%S", localtime(&t));
    std::string path = Catalog::join_path(puzzle_dir,
        std::string(date) + "-" + sgf_sanitize(opp_name) + "-move" + std::to_string(depth) + ".sgf");

    FILE* f = Catalog::fopen_utf8(path, "w");
    if (!f) return;
    auto sgf_escape = [](const std::string& s) {
        std::string out;
        for (char c : s) { if (c == ']' || c == '\\') out += '\\'; out += c; }
        return out;
    };
    int n = gs->board_size;
    fprintf(f, "(;GM[1]FF[4]CA[UTF-8]SZ[%d]", n);
    for (int color = 1; color >= 0; color--) {  // black setup stones first, then white
        bool any = false;
        for (int r = 0; r < n && !any; r++)
            for (int c = 0; c < n; c++)
                if (gs->board[r][c] == (color ? 1 : 2)) { any = true; break; }
        if (!any) continue;
        fprintf(f, "%s", color ? "AB" : "AW");
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++)
                if (gs->board[r][c] == (color ? 1 : 2))
                    fprintf(f, "[%c%c]", char('a' + c), char('a' + r));
    }
    fprintf(f, "PL[%s]", gs->turn_is_black ? "B" : "W");
    fprintf(f, "PB[%s]PW[%s]", sgf_escape(game_.black_name).c_str(), sgf_escape(game_.white_name).c_str());
    char day[16];
    strftime(day, sizeof(day), "%Y-%m-%d", localtime(&t));
    fprintf(f, "DT[%s]", day);
    fprintf(f, "C[Study puzzle: one move keeps the game. From move %d.]", depth);
    fprintf(f, ")\n");
    fclose(f);
}

void App::save_companion() {
    if (companion_path_.empty()) return;
    if (move_scores_.empty() && move_marked_.empty()) return;

    FILE* f = Catalog::fopen_utf8(companion_path_, "w");
    if (!f) return;

    // Scores line: "S val val val ..." (nan for unknown)
    fprintf(f, "S");
    for (int i = 0; i < (int)move_scores_.size(); i++) {
        float v = move_scores_[i];
        if (v == FLT_MAX) fprintf(f, " nan");
        else              fprintf(f, " %.4g", v);
    }
    fprintf(f, "\n");

    // Marked line: "M depth depth ..."
    fprintf(f, "M");
    for (int i = 0; i < (int)move_marked_.size(); i++)
        if (move_marked_[i]) fprintf(f, " %d", i);
    fprintf(f, "\n");

    fclose(f);
}

void App::load_companion() {
    if (companion_path_.empty()) return;
    FILE* f = Catalog::fopen_utf8(companion_path_, "r");
    if (!f) return;

    char line[65536];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'S') {
            char* p = line + 1;
            int idx = 0;
            while (*p && idx < (int)move_scores_.size()) {
                while (*p == ' ') p++;
                if (strncmp(p, "nan", 3) == 0) { p += 3; idx++; }
                else { move_scores_[idx++] = (float)strtod(p, &p); }
            }
        } else if (line[0] == 'M') {
            char* p = line + 1;
            while (*p) {
                while (*p == ' ' || *p == '\n' || *p == '\r') p++;
                if (!*p) break;
                int d = (int)strtol(p, &p, 10);
                if (d >= 0 && d < (int)move_marked_.size())
                    move_marked_[d] = true;
            }
        }
    }
    fclose(f);
}

// ── Local game vs KataGo ─────────────────────────────────────────────────────

static const char* const KATA_GTP_PROFILES[7] = {
    "preaz_20k", "preaz_15k", "preaz_10k", "preaz_5k",
    "preaz_1k",  "preaz_1d",  "preaz_5d"
};
static const char* const KATA_GTP_NAMES[7] = {
    "20 KYU", "15 KYU", "10 KYU", "5 KYU",
    "1 KYU",  "1 DAN",  "5 DAN"
};

// Adaptive strength: a fractional position on KataGo's full human-profile ladder,
// rank index 0..28 → 20k..1k (0..19) then 1d..9d (20..28).
static constexpr int KATA_RANK_MAX = 28;
static std::string kata_rank_profile(int idx) {
    return idx <= 19 ? "preaz_" + std::to_string(20 - idx) + "k"
                     : "preaz_" + std::to_string(idx - 19) + "d";
}
static std::string kata_rank_label(int idx) {
    return idx <= 19 ? std::to_string(20 - idx) + " KYU"
                     : std::to_string(idx - 19) + " DAN";
}

void App::normalize_size_sel_for_katago() {
    int keep = 2;  // default 19x19
    for (int i = 0; i < 3; i++)
        if (match_menu_.size_sel[i]) { keep = i; break; }
    for (int i = 0; i < 3; i++)
        match_menu_.size_sel[i] = (i == keep);
}

void App::start_local_game() {
    // Pick board size (first checked, default 9x9)
    int bs = 19;
    if (match_prefs_.sizes[0]) bs = 9;
    else if (match_prefs_.sizes[1]) bs = 13;

    int str = match_prefs_.katago_str;
    if (str < 0 || str > 7) str = 2;

    // Index 7 = ADAPTIVE: resolve the fractional rating to the nearest profile rank
    std::string profile, opp_label;
    adaptive_game_ = (str == 7);
    if (adaptive_game_) {
        int idx = (int)std::lround(adaptive_rank_);
        idx = std::max(0, std::min(KATA_RANK_MAX, idx));
        profile   = kata_rank_profile(idx);
        opp_label = "ADAPTIVE (" + kata_rank_label(idx) + ")";
    } else {
        profile   = KATA_GTP_PROFILES[str];
        opp_label = KATA_GTP_NAMES[str];
    }

    if (!kata_gtp_.start(kata_exe_, kata_model_, kata_human_model_,
                         profile, bs, 7.5f)) {
        flash_       = "FAILED TO START KATAGO";
        flash_until_ = SDL_GetTicks() + 3000;
        state_ = AppState::LOBBY;
        set_status("");
        draw();
        return;
    }

    game_.result.clear();
    game_.pending_col   = -2;
    game_.pending_row   = -2;
    game_.history.clear();
    game_.history_pos   = -1;
    memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
    memset(game_.ownership,   0, sizeof(game_.ownership));
    game_.game_id       = 0;
    game_.board_size    = bs;
    game_.my_color      = rand() % 2;   // randomly Black or White each game
    game_.my_player_id  = 0;
    std::string my_name = my_username_.empty() ? "You" : my_username_;
    game_.black_name    = (game_.my_color == 1) ? my_name : opp_label;
    game_.white_name    = (game_.my_color == 0) ? my_name : opp_label;
    game_.black_rank    = game_.white_rank = "";
    game_.black_secs = game_.white_secs = -1;
    game_.black_periods = game_.white_periods = -1;
    game_.black_period_secs = game_.white_period_secs = -1;
    game_.clock_tick    = 0;
    game_.cursor_r = game_.cursor_f = bs / 2;
    game_.board.reset();
    game_.board.board_size    = bs;
    game_.board.turn_is_black = 1;
    game_.handicap      = 0;
    game_.free_handicap = false;
    game_.history.push_back(game_.board);

    // Fresh score-graph / mark storage — never inherit the previous game's arrays.
    // (Leftovers made the new game's graph show the *previous* game's scores, made
    // the background sweep think those depths were already done, and let undo pop
    // scores that belonged to a different game entirely.)
    move_scores_.assign(game_.history.size(), FLT_MAX);
    move_marked_.assign(game_.history.size(), false);
    marked_paths_.assign(game_.history.size(), "");
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    puzzle_eval_.clear();
    puzzle_saved_.clear();
    review_path_.clear();  // this GAME_OVER will be a live game's, not a loaded file

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;

    is_local_game_        = true;
    local_prev_was_pass_  = false;
    game_.my_turn         = (game_.my_color == 1);   // Black always moves first

    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;
    kata_analysis_enabled_ = false;   // analysis overlay off during live play
    review_komi_           = 7.5f;    // fresh local games use the standard komi
    local_game_komi_       = 7.5f;

    state_ = AppState::PLAYING;
    sound_.play_game_start();
    if (game_.my_turn) {
        set_status(std::string("YOUR TURN  (") + (game_.my_color == 1 ? "BLACK" : "WHITE") + ")");
    } else {
        // Player is White — KataGo (Black) opens
        kata_gtp_.request_genmove(1 - game_.my_color);
        set_status("KATAGO THINKING...");
    }
    draw();
}

void App::start_practice_from_position() {
    if (state_ != AppState::GAME_OVER || !analysis_cur_) return;

    // Reference the position in place — GameState is ~12MB (it embeds a full
    // GameSnapshot[MAX_MOVES] by value), so it must never be a local/by-value
    // copy (that mistake stack-overflowed the joseki explorer; see jk_arrive's
    // fix). analysis_cur_ stays valid until analysis_root_.reset() below, so
    // everything that needs the board data must happen before that point.
    const GameState& pos = analysis_cur_->board;
    int       bs   = game_.board_size;
    float     komi = review_komi_;
    bool      pos_turn_is_black = (pos.turn_is_black == 1);

    int str = match_prefs_.katago_str;
    if (str < 0 || str > 7) str = 2;
    std::string profile, opp_label;
    if (str == 7) {
        int idx = std::max(0, std::min(KATA_RANK_MAX, (int)std::lround(adaptive_rank_)));
        profile   = kata_rank_profile(idx);
        opp_label = "ADAPTIVE (" + kata_rank_label(idx) + ")";
    } else {
        profile   = KATA_GTP_PROFILES[str];
        opp_label = KATA_GTP_NAMES[str];
    }
    // Practice positions can be arbitrarily lopsided — a win or loss says nothing
    // about rank, so never let one move the adaptive level.
    adaptive_game_ = false;

    if (!kata_gtp_.start(kata_exe_, kata_model_, kata_human_model_,
                         profile, bs, komi)) {
        flash_       = "FAILED TO START KATAGO";
        flash_until_ = SDL_GetTicks() + 3000;
        draw();
        return;
    }
    // Seed the position stone by stone as raw GTP play commands (GTP doesn't
    // require alternating colors). Order can't matter: any subset of a legal
    // position is itself legal — a partially-placed group's missing stones are
    // liberties — so no placement can ever trigger a capture mid-seed.
    for (int r = 0; r < bs; r++)
        for (int f = 0; f < bs; f++)
            if (pos.board[r][f] != 0)
                kata_gtp_.send_play(pos.board[r][f] == 1 ? 1 : 0, r, f, bs);

    // Copy the position into its final destination now, while `pos` (a
    // reference into the analysis tree) is still valid — the tree is torn
    // down right below.
    game_.board            = pos;
    game_.board.board_size = bs;

    save_companion();  // persist the review's scores + marks before leaving it
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    analysis_tree_render_.clear();

    game_.result.clear();
    game_.pending_col   = -2;
    game_.pending_row   = -2;
    game_.history.clear();
    game_.history_pos   = -1;
    memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
    memset(game_.ownership,   0, sizeof(game_.ownership));
    game_.game_id       = 0;
    game_.board_size    = bs;
    game_.my_color      = pos_turn_is_black ? 1 : 0;  // player takes the side to move
    game_.my_player_id  = 0;
    std::string my_name = my_username_.empty() ? "You" : my_username_;
    game_.black_name    = (game_.my_color == 1) ? my_name : opp_label;
    game_.white_name    = (game_.my_color == 0) ? my_name : opp_label;
    game_.black_rank    = game_.white_rank = "";
    game_.black_secs = game_.white_secs = -1;
    game_.black_periods = game_.white_periods = -1;
    game_.black_period_secs = game_.white_period_secs = -1;
    game_.clock_tick    = 0;
    // game_.board was already copied from pos above, before the analysis tree
    // that backs it was torn down
    game_.handicap      = 0;
    game_.free_handicap = false;
    game_.history.push_back(game_.board);

    // Fresh score-graph / mark storage, same as start_local_game
    move_scores_.assign(game_.history.size(), FLT_MAX);
    move_marked_.assign(game_.history.size(), false);
    marked_paths_.assign(game_.history.size(), "");
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    puzzle_eval_.clear();
    puzzle_saved_.clear();
    review_path_.clear();      // the coming GAME_OVER belongs to the practice game
    companion_path_.clear();   // save_live_game assigns the practice game its own

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;

    is_local_game_        = true;
    local_prev_was_pass_  = false;
    local_game_score_.clear();
    local_game_komi_      = komi;
    game_.my_turn         = true;   // by construction — player took the side to move

    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;
    kata_query_after_      = 0;
    kata_analysis_enabled_ = false;   // analysis overlay off during live play

    state_ = AppState::PLAYING;
    sound_.play_game_start();
    set_status(std::string("PRACTICE — YOUR TURN  (")
               + (game_.my_color == 1 ? "BLACK" : "WHITE") + ")");
    draw();
}

void App::handle_katago_gtp_move(int row, int col) {
    const char* my_col_str = (game_.my_color == 1) ? "B" : "W";
    std::string my_turn_status = std::string("YOUR TURN  (")
                                + (game_.my_color == 1 ? "BLACK" : "WHITE") + ")";
    if (row == -2) {
        // KataGo resigned — show territory view before going to analysis
        flash_       = "KATAGO RESIGNED — YOU WIN!";
        flash_until_ = SDL_GetTicks() + 4000;
        kata_gtp_.stop();  // no more GTP commands needed
        begin_local_stone_removal(std::string(my_col_str) + "+R");
        return;
    }
    if (row == -1) {
        // KataGo passed
        flash_       = "KATAGO PASSED";
        flash_until_ = SDL_GetTicks() + 3000;
        apply_pass();
        if (local_prev_was_pass_) {
            begin_local_stone_removal();
            return;
        }
        local_prev_was_pass_ = true;
        game_.my_turn = true;
        set_status(my_turn_status);
        draw();
        return;
    }
    // Normal move
    apply_move(col, row);
    local_prev_was_pass_ = false;
    pass_confirm_        = false;
    game_.my_turn        = true;
    set_status(my_turn_status);
    draw();
}

void App::begin_local_stone_removal(const std::string& forced_result) {
    state_ = AppState::STONE_REMOVAL;
    close_popup_menu();  // a GAME MENU popup is stale once scoring starts
    stone_removal_has_ogs_territory_ = false;
    game_.history_pos = -1;
    memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
    memset(game_.ownership,   0, sizeof(game_.ownership));
    my_accept_sent_    = false;
    local_game_score_  = forced_result;  // empty = double-pass, non-empty = resignation

    if (forced_result.empty()) {
        // Double-pass: ask GTP for dead stones first; ownership query follows in the poll loop.
        if (kata_gtp_.running())
            kata_gtp_.request_final_status();
    } else {
        // Resignation: result already known, no dead stones.
        // Query territory directly so the user can see the board state.
        if (kata_for(game_.board_size).running())
            kata_for(game_.board_size).query_ownership(
                game_.board.board, game_.board_size, game_.dead_stones, 7.5f, 100);
    }
    set_status("ANALYZING...");
    draw();
}

// ── Persisted match/display settings ──────────────────────────────────────────
// Simple whitespace-separated "key value" lines — order-independent and
// forward/backward compatible (unknown keys ignored on load; keys missing from
// an older file just leave that field at its compiled-in default).

void App::load_settings() {
    FILE* f = fopen((exe_dir() + "settings.txt").c_str(), "r");
    if (!f) return;
    char key[64];
    int val;
    while (fscanf(f, "%63s %d", key, &val) == 2) {
        std::string k = key;
        if      (k == "size_9")        match_prefs_.sizes[0]   = val != 0;
        else if (k == "size_13")       match_prefs_.sizes[1]   = val != 0;
        else if (k == "size_19")       match_prefs_.sizes[2]   = val != 0;
        else if (k == "speed_fast")    match_prefs_.speeds[0]  = val != 0;
        else if (k == "speed_medium")  match_prefs_.speeds[1]  = val != 0;
        else if (k == "speed_slow")    match_prefs_.speeds[2]  = val != 0;
        else if (k == "katago_mode")   match_prefs_.katago_mode = val != 0;
        else if (k == "katago_str")    match_prefs_.katago_str  = val;
        else if (k == "show_coords")   show_coords_            = val != 0;
        else if (k == "analysis")      kata_analysis_enabled_  = val != 0;
        else if (k == "chain_mode")    chain_mode_             = val != 0;
        else if (k == "square_stones") square_stones_          = val != 0;
        else if (k == "square_grid")   square_grid_            = val != 0;
    }
    fclose(f);
}

void App::save_settings() {
    FILE* f = fopen((exe_dir() + "settings.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "size_9 %d\n",        match_prefs_.sizes[0]    ? 1 : 0);
    fprintf(f, "size_13 %d\n",       match_prefs_.sizes[1]    ? 1 : 0);
    fprintf(f, "size_19 %d\n",       match_prefs_.sizes[2]    ? 1 : 0);
    fprintf(f, "speed_fast %d\n",    match_prefs_.speeds[0]   ? 1 : 0);
    fprintf(f, "speed_medium %d\n",  match_prefs_.speeds[1]   ? 1 : 0);
    fprintf(f, "speed_slow %d\n",    match_prefs_.speeds[2]   ? 1 : 0);
    fprintf(f, "katago_mode %d\n",   match_prefs_.katago_mode ? 1 : 0);
    fprintf(f, "katago_str %d\n",    match_prefs_.katago_str);
    fprintf(f, "show_coords %d\n",   show_coords_             ? 1 : 0);
    fprintf(f, "analysis %d\n",      kata_analysis_enabled_   ? 1 : 0);
    fprintf(f, "chain_mode %d\n",    chain_mode_              ? 1 : 0);
    fprintf(f, "square_stones %d\n", square_stones_           ? 1 : 0);
    fprintf(f, "square_grid %d\n",   square_grid_             ? 1 : 0);
    fclose(f);
}

// ── Adaptive strength bookkeeping ─────────────────────────────────────────────

void App::load_adaptive() {
    FILE* f = fopen((exe_dir() + "adaptive_level.txt").c_str(), "r");
    if (!f) return;
    float v = 0.f;
    if (fscanf(f, "%f", &v) == 1 && v >= 0.f && v <= (float)KATA_RANK_MAX)
        adaptive_rank_ = v;
    fclose(f);
}

void App::save_adaptive() {
    FILE* f = fopen((exe_dir() + "adaptive_level.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "%.3f\n", adaptive_rank_);
    fclose(f);
}

// Nudge the adaptive rating after a finished adaptive game. The player is always
// Black in local games, so "B+..." is a win. Step size scales with the score
// margin — a 40-point blowout moves a full rank, a nail-biter barely moves —
// with resignations counted as a solid but not extreme result.
void App::update_adaptive(const std::string& result) {
    if (result.size() < 3 || (result[0] != 'B' && result[0] != 'W')) return;
    bool won = (result[0] == 'B');

    float step;
    std::string margin = result.substr(2);
    if (!margin.empty() && (margin[0] == 'R' || margin[0] == 'T')) {
        step = 0.6f;
    } else {
        float pts = (float)atof(margin.c_str());
        step = std::max(0.15f, std::min(1.0f, pts / 25.0f));
    }

    adaptive_rank_ += won ? step : -step;
    adaptive_rank_  = std::max(0.f, std::min((float)KATA_RANK_MAX, adaptive_rank_));
    save_adaptive();

    int idx = std::max(0, std::min(KATA_RANK_MAX, (int)std::lround(adaptive_rank_)));
    flash_       = std::string("ADAPTIVE LEVEL: ") + kata_rank_label(idx)
                 + (won ? " (UP)" : " (DOWN)");
    flash_until_ = SDL_GetTicks() + 3500;
}

void App::end_local_game(const std::string& result) {
    kata_gtp_.stop();
    if (adaptive_game_) {
        update_adaptive(result);
        adaptive_game_ = false;
    }
    is_local_game_        = false;
    // Restart the background sweep from move 0 so any depths that missed scoring
    // during play (lost responses, mid-game undos) get filled in during review.
    bg_analysis_next_ = 0;
    close_popup_menu();  // popup items built for PLAYING/STONE_REMOVAL are stale now
    review_komi_           = local_game_komi_;  // 7.5 for fresh games; inherited for practice games
    state_ = AppState::GAME_OVER;
    game_.result = result;
    save_live_game();
    // Scan for auto study puzzles (remaining depths get checked as the background
    // sweep fills them in during review)
    for (int d = 0; d + 1 < (int)game_.history.size(); d++) check_puzzle(d);
    // Straight into analysis — the score was already shown big on the stone-removal
    // screen, so a second result-confirmation screen here was redundant.
    build_analysis_tree();
    kata_suggestion_count_ = 0;
    kata_score_lead_       = cached_analysis_score(analysis_cur_);
    kata_analysis_enabled_ = false;
    set_status("GAME OVER — " + result);
    draw();
}

// ── Game catalog ──────────────────────────────────────────────────────────────

void App::open_game_catalog() {
    if (catalog_.active) return;
    save_companion();  // persist any in-progress review scores/marks before switching away

    // Locate my_games/<username>/ — personal games, separate from the games/ pro
    // library (same two-path probe as demo loader)
    std::string gdir = exe_dir() + "my_games";
    auto is_dir = [](const std::string& p) {
        DWORD a = GetFileAttributesW(Catalog::utf8_to_wide(p).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (!is_dir(gdir)) gdir = exe_dir() + "../../my_games";

    std::string my_dir = Catalog::join_path(gdir, my_username_);

    // Reopening the catalog resumes where it was last time (same subdirectory —
    // e.g. puzzles/ or marked/ — and roughly the same cursor spot) instead of
    // resetting to the root on every visit. First open of the session still
    // starts fresh at the root.
    bool resume = catalog_.base_dir == my_dir;
    int  old_index  = catalog_.index;
    int  old_scroll = catalog_.scroll;
    if (!resume) {
        catalog_.open(my_dir);   // full reset (also kicks off the search index)
        catalog_.current_subdir = "";
    } else {
        catalog_.selection_made = false;
        catalog_.selected_path.clear();
        catalog_.search_query.clear();
        catalog_.search_mode = false;
        catalog_.active      = true;
    }
    // Flat filesystem mode (skip the virtual player browser)
    catalog_.virtual_player_mode = false;
    catalog_.virtual_year_mode   = false;
    catalog_.load_entries();     // refresh — new games/puzzles may have appeared

    int n = (int)catalog_.entries.size();
    catalog_.index  = 0;
    catalog_.scroll = 0;
    if (resume && n > 0) {
        catalog_.index  = std::min(old_index, n - 1);
        catalog_.scroll = old_scroll;
        // If a review is open and it lives in the displayed directory, park the
        // cursor on it — after L3/R3 cycling this lands on the current file.
        if (!review_path_.empty()) {
            size_t sep = review_path_.find_last_of("/\\");
            std::string rdir  = (sep == std::string::npos) ? "" : review_path_.substr(0, sep);
            std::string rname = (sep == std::string::npos) ? review_path_ : review_path_.substr(sep + 1);
            std::string shown = catalog_.current_subdir.empty()
                                ? catalog_.base_dir
                                : Catalog::join_path(catalog_.base_dir, catalog_.current_subdir);
            if (rdir == shown)
                for (int i = 0; i < n; i++)
                    if (catalog_.entries[i].type == 0 && catalog_.entries[i].name == rname) {
                        catalog_.index = i;
                        break;
                    }
        }
        // Keep the cursor row within the visible window (15 rows)
        catalog_.scroll = std::min(catalog_.scroll, catalog_.index);
        if (catalog_.index >= catalog_.scroll + 15)
            catalog_.scroll = catalog_.index - 14;
    }
    catalog_delete_confirm_ = false;
    catalog_readonly_       = false;   // the user's own games are always deletable
    // Parse player names for every entry up front, not just the first visible page —
    // each parse only reads a 4KB SGF header, so even a few hundred games open fast,
    // and the whole list shows "Black vs White" immediately instead of raw filenames
    // trickling in as rows scroll into view.
    catalog_.ensure_names_loaded(0, (int)catalog_.entries.size());
    thumb_path_ = "";                     // force thumbnail reload on first draw
    update_catalog_thumb();
}

// Curated professional-game library — games/ itself (one subdirectory per pro
// player). The user's own games live separately under my_games/<username>/
// (see open_game_catalog()), so this tree is pure pro content. Browsed with
// virtual player/year views. Read-only: see catalog_readonly_.
void App::open_pro_catalog() {
    if (catalog_.active) return;
    save_companion();  // persist any in-progress review scores/marks before switching away

    // Locate games/ — same two-path probe as open_game_catalog()/demo loader
    std::string gdir = exe_dir() + "games";
    auto is_dir = [](const std::string& p) {
        DWORD a = GetFileAttributesW(Catalog::utf8_to_wide(p).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (!is_dir(gdir)) gdir = exe_dir() + "../../games";

    // The pro database never changes mid-session, so unlike open_game_catalog()
    // there is nothing to refresh on resume — just leave `entries` exactly as
    // they were (whatever player/year/directory view was open) and reactivate.
    bool resume = catalog_.base_dir == gdir;
    if (!resume) {
        catalog_.open(gdir);   // full reset — lands on BY PLAYER (Catalog::open()'s default)
    } else {
        catalog_.selection_made = false;
        catalog_.selected_path.clear();
        catalog_.search_query.clear();
        catalog_.search_mode = false;
        catalog_.active      = true;
    }
    catalog_readonly_       = true;
    catalog_delete_confirm_ = false;
    catalog_.ensure_names_loaded(0, (int)catalog_.entries.size());
    thumb_path_ = "";
    update_catalog_thumb();
}

void App::delete_catalog_game(const std::string& sgf_path) {
    if (sgf_path.empty()) return;
    if (_wremove(Catalog::utf8_to_wide(sgf_path).c_str()) != 0) {
        flash_       = "DELETE FAILED";
        flash_until_ = SDL_GetTicks() + 2000;
        return;
    }
    // Companion score/mark file goes with it (fails silently if there isn't one)
    size_t dot = sgf_path.rfind('.');
    if (dot != std::string::npos) {
        std::string companion = sgf_path.substr(0, dot) + ".katago";
        _wremove(Catalog::utf8_to_wide(companion).c_str());
        // If the deleted game is the one currently loaded behind the catalog,
        // forget its companion path so exiting review can't resurrect the file.
        if (companion == companion_path_) companion_path_.clear();
    }
    flash_       = "GAME DELETED";
    flash_until_ = SDL_GetTicks() + 1500;

    // Refresh the listing in place, keeping the cursor near where it was
    int old_index = catalog_.index;
    catalog_.load_entries();
    int n = (int)catalog_.entries.size();
    catalog_.index  = (n == 0) ? 0 : std::min(old_index, n - 1);
    catalog_.scroll = std::min(catalog_.scroll, std::max(0, catalog_.index));
    catalog_.ensure_names_loaded(0, n);
    thumb_path_ = "";
    update_catalog_thumb();
}

void App::open_settings_menu() {
    pre_menu_state_          = state_;
    match_menu_.ingame       = (state_ != AppState::LOBBY);
    match_menu_.focus_col    = 0;
    match_menu_.focus_row    = 0;
    match_menu_.katago_mode  = match_prefs_.katago_mode;
    match_menu_.katago_str   = match_prefs_.katago_str;
    {
        int idx = std::max(0, std::min(KATA_RANK_MAX, (int)std::lround(adaptive_rank_)));
        match_menu_.adaptive_label = "ADAPTIVE (" + kata_rank_label(idx) + ")";
    }
    for (int i = 0; i < 3; i++) match_menu_.size_sel[i]  = match_prefs_.sizes[i];
    for (int i = 0; i < 3; i++) match_menu_.speed_sel[i] = match_prefs_.speeds[i];
    if (match_menu_.katago_mode) normalize_size_sel_for_katago();
    match_menu_.show_coords_sel = show_coords_;
    match_menu_.analysis_sel    = kata_analysis_enabled_;
    match_menu_.analysis_available = kata_.running() || kata_9_.running();
    match_menu_.chain_sel       = chain_mode_;
    match_menu_.square_sel      = square_stones_;
    match_menu_.square_grid_sel = square_grid_;
    state_ = AppState::MATCH_MENU;
    renderer_->draw_match_menu(match_menu_);
}

// Triangle from LOBBY: fresh empty 19x19 board in the standard analysis mode —
// same tree panel, branching, labels, and engine toggle as a game review, just
// with nothing played yet.
void App::start_free_analysis() {
    game_.history.clear();
    game_.history_pos = -1;
    game_.board_size  = 19;
    game_.board.reset();
    game_.board.board_size    = 19;
    game_.board.turn_is_black = 1;
    game_.cursor_r = game_.cursor_f = 9;
    game_.black_name = "BLACK";
    game_.white_name = "WHITE";
    game_.black_rank = game_.white_rank = "";
    game_.result.clear();
    game_.my_color   = -1;   // pure analysis — no "my side"
    game_.game_id    = 0;
    game_.black_secs = game_.white_secs = -1;
    game_.history.push_back(game_.board);

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;
    review_komi_ = 7.5f;

    move_scores_.assign(game_.history.size(), FLT_MAX);
    move_marked_.assign(game_.history.size(), false);
    marked_paths_.assign(game_.history.size(), "");
    puzzle_eval_.clear();
    puzzle_saved_.clear();
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    companion_path_.clear();   // nothing on disk to persist scores/marks to
    review_path_.clear();      // L3/R3 file cycling doesn't apply here
    is_local_game_ = false;

    build_analysis_tree();
    state_ = AppState::GAME_OVER;
    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;
    kata_analysis_enabled_ = false;  // toggle on via the settings menu as usual
    set_status("FREE ANALYSIS");
    draw();
}

// ── OGS puzzle browser / player ───────────────────────────────────────────────

void App::load_solved_puzzles() {
    FILE* f = fopen((exe_dir() + "solved_puzzles.txt").c_str(), "r");
    if (!f) return;
    // One puzzle per line: "id" (legacy) or "id collection_id". Parse line-wise —
    // a bare %d-%d scan would pair up ids across lines in a legacy file.
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int id = 0, col = 0;
        int n = sscanf(line, "%d %d", &id, &col);
        if (n >= 1 && id > 0) {
            pz_solved_ids_.insert(id);
            pz_solved_col_[id] = (n >= 2 && col > 0) ? col : 0;
        }
    }
    fclose(f);
}

void App::save_solved_puzzles() {
    FILE* f = fopen((exe_dir() + "solved_puzzles.txt").c_str(), "w");
    if (!f) return;
    for (int id : pz_solved_ids_) {
        auto it = pz_solved_col_.find(id);
        fprintf(f, "%d %d\n", id, it != pz_solved_col_.end() ? it->second : 0);
    }
    fclose(f);
}

// puzzle_collections.txt: one collection per line, tab-separated —
// id, starting_puzzle_id, puzzle_count, min_rank, max_rank, rating, owner, name.
// Name is last so it may contain anything except a tab.
void App::load_known_collections() {
    FILE* f = fopen((exe_dir() + "puzzle_collections.txt").c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        std::vector<std::string> parts;
        size_t pos = 0;
        for (int i = 0; i < 7; i++) {
            size_t tab = s.find('\t', pos);
            if (tab == std::string::npos) break;
            parts.push_back(s.substr(pos, tab - pos));
            pos = tab + 1;
        }
        if (parts.size() != 7) continue;   // malformed line
        OgsPuzzleCollection c;
        c.id                 = atoi(parts[0].c_str());
        c.starting_puzzle_id = atoi(parts[1].c_str());
        c.puzzle_count       = atoi(parts[2].c_str());
        c.min_rank           = atoi(parts[3].c_str());
        c.max_rank           = atoi(parts[4].c_str());
        c.rating             = (float)atof(parts[5].c_str());
        c.owner              = parts[6];
        c.name               = s.substr(pos);
        if (c.id > 0) pz_known_cols_[c.id] = c;
    }
    fclose(f);
}

void App::save_known_collections() {
    FILE* f = fopen((exe_dir() + "puzzle_collections.txt").c_str(), "w");
    if (!f) return;
    for (const auto& kv : pz_known_cols_) {
        const OgsPuzzleCollection& c = kv.second;
        fprintf(f, "%d\t%d\t%d\t%d\t%d\t%.2f\t%s\t%s\n",
                c.id, c.starting_puzzle_id, c.puzzle_count,
                c.min_rank, c.max_rank, c.rating,
                c.owner.c_str(), c.name.c_str());
    }
    fclose(f);
}

void App::pz_rebuild_display() {
    pz_display_cols_.clear();
    std::map<int, int> solved = pz_solved_per_collection();

    // Pinned section: every known collection with progress but not finished
    std::vector<const OgsPuzzleCollection*> pinned;
    for (const auto& kv : pz_known_cols_) {
        auto it = solved.find(kv.first);
        int s = (it == solved.end()) ? 0 : it->second;
        if (s > 0 && (kv.second.puzzle_count <= 0 || s < kv.second.puzzle_count))
            pinned.push_back(&kv.second);
    }
    std::sort(pinned.begin(), pinned.end(),
              [](const OgsPuzzleCollection* a, const OgsPuzzleCollection* b) {
                  return _stricmp(a->name.c_str(), b->name.c_str()) < 0;
              });
    std::set<int> pinned_ids;
    for (const OgsPuzzleCollection* p : pinned) {
        pz_display_cols_.push_back(*p);
        pinned_ids.insert(p->id);
    }

    // Then the fetched page, minus collections already pinned above
    for (const auto& c : pz_collections_)
        if (!pinned_ids.count(c.id))
            pz_display_cols_.push_back(c);

    // Local life-and-death drills, pinned at the very top (id = -1 sentinel,
    // routed to open_drill_list() instead of a network fetch). Only shown when
    // at least one drill file exists.
    std::vector<std::string> drill_files;
    if (Catalog::list_sgf_files(drills_dir(false), drill_files) && !drill_files.empty()) {
        OgsPuzzleCollection dc;
        dc.id           = -1;
        dc.name         = "[MY DRILLS]";
        dc.owner        = my_username_.empty() ? "You" : my_username_;
        dc.puzzle_count = (int)drill_files.size();
        pz_display_cols_.insert(pz_display_cols_.begin(), dc);
    }
}

// Enter the drill list: the puzzle browser's PUZZLES view backed by local
// files instead of a fetched collection.
void App::open_drill_list() {
    std::string dir = drills_dir(false);
    std::vector<std::string> files;
    Catalog::list_sgf_files(dir, files);
    if (files.empty()) {
        flash_       = "NO DRILLS YET — SAVE ONE FROM ANALYSIS";
        flash_until_ = SDL_GetTicks() + 2500;
        draw();
        return;
    }
    std::sort(files.begin(), files.end(),
              [](const std::string& a, const std::string& b) {
                  return _stricmp(a.c_str(), b.c_str()) < 0;
              });
    drill_paths_.clear();
    pz_list_.clear();
    for (int i = 0; i < (int)files.size(); i++) {
        drill_paths_.push_back(Catalog::join_path(dir, files[i]));
        std::string stem = files[i];
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        pz_list_.push_back({-(i + 1), stem});
    }
    drill_browse_    = true;
    pz_view_         = PzView::PUZZLES;
    pz_list_title_   = "MY DRILLS";
    pz_index_        = 0;
    pz_open_col_id_  = 0;
    drill_thumb_idx_ = -1;   // file list may have changed — recompute the preview
    draw();
}

// Load drill_paths_[idx] into pz_ and start it — the local (no network)
// counterpart of pz_launch_fetch(2, id).
void App::drill_load_and_start(int idx) {
    if (idx < 0 || idx >= (int)drill_paths_.size()) return;
    std::string err;
    if (!load_drill_sgf(drill_paths_[idx], pz_, err)) {
        flash_       = "BAD DRILL FILE: " + err;
        flash_until_ = SDL_GetTicks() + 2500;
        draw();
        return;
    }
    pz_visits_.clear();
    pz_more_lines_   = false;
    pz_list_pos_     = idx;
    drill_play_path_ = drill_paths_[idx];
    pz_start();
}

// Commit the keyboard-entered new name for the selected drill file.
void App::drill_commit_rename() {
    drill_rename_active_ = false;
    std::string name = sgf_sanitize(drill_rename_buf_);
    drill_rename_buf_.clear();
    if (pz_index_ < 0 || pz_index_ >= (int)drill_paths_.size()) { draw(); return; }
    if (name.empty()) {
        flash_       = "INVALID NAME";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    std::string old_path = drill_paths_[pz_index_];
    std::string new_path = Catalog::join_path(drills_dir(false), name + ".sgf");
    if (new_path == old_path) { draw(); return; }
    if (GetFileAttributesW(Catalog::utf8_to_wide(new_path).c_str()) != INVALID_FILE_ATTRIBUTES) {
        flash_       = "NAME ALREADY TAKEN";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    if (_wrename(Catalog::utf8_to_wide(old_path).c_str(),
                 Catalog::utf8_to_wide(new_path).c_str()) != 0) {
        flash_       = "RENAME FAILED";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    if (drill_play_path_ == old_path) drill_play_path_ = new_path;
    open_drill_list();   // re-list; park the cursor back on the renamed file
    for (int i = 0; i < (int)drill_paths_.size(); i++)
        if (drill_paths_[i] == new_path) { pz_index_ = i; break; }
    flash_       = "RENAMED";
    flash_until_ = SDL_GetTicks() + 1500;
    draw();
}

// Delete the selected drill file (reached via the popup's confirmed item).
void App::drill_delete_selected() {
    if (pz_index_ < 0 || pz_index_ >= (int)drill_paths_.size()) return;
    std::string path = drill_paths_[pz_index_];
    if (_wremove(Catalog::utf8_to_wide(path).c_str()) != 0) {
        flash_       = "DELETE FAILED";
        flash_until_ = SDL_GetTicks() + 2000;
        draw();
        return;
    }
    if (drill_play_path_ == path) drill_play_path_.clear();
    flash_       = "DRILL DELETED";
    flash_until_ = SDL_GetTicks() + 1500;
    int old_index = pz_index_;
    std::vector<std::string> remaining;
    Catalog::list_sgf_files(drills_dir(false), remaining);
    if (remaining.empty()) {
        // Last drill gone — the [MY DRILLS] collection itself disappears
        drill_browse_ = false;
        drill_paths_.clear();
        pz_view_  = PzView::COLLECTIONS;
        pz_index_ = 0;
        pz_rebuild_display();
        draw();
        return;
    }
    open_drill_list();
    pz_index_ = std::min(old_index, (int)drill_paths_.size() - 1);
    draw();
}

// Reopen the drill being played as an editable analysis tree: the drill's
// setup becomes the analysis root, its variation tree becomes AnalysisNode
// children (correct-marks and labels intact), and SAVE overwrites the file.
void App::drill_edit_current() {
    if (drill_play_path_.empty()) return;
    // Reload from disk: the in-memory pz_ may carry a random drill orientation
    // (see pz_start) — editing must happen in the file's authored orientation
    // or every save would rewrite the drill rotated.
    std::string reload_err;
    if (!load_drill_sgf(drill_play_path_, pz_, reload_err)) {
        flash_       = "CAN'T RELOAD DRILL: " + reload_err;
        flash_until_ = SDL_GetTicks() + 2500;
        draw();
        return;
    }

    // Rebuild game_ at the drill's root, same steps as pz_start()
    game_.history.clear();
    game_.history_pos = -1;
    game_.board_size  = pz_.width;
    game_.board.reset();
    game_.board.board_size    = pz_.width;
    game_.board.turn_is_black = pz_.black_to_play ? 1 : 0;
    game_.black_name = "BLACK";
    game_.white_name = "WHITE";
    game_.black_rank = game_.white_rank = "";
    game_.result.clear();
    game_.my_color   = -1;   // analysis — no "my side", no study-puzzle detection
    game_.game_id    = 0;
    game_.black_secs = game_.white_secs = -1;
    game_.cursor_r = std::max(0, std::min(pz_.width - 1, game_.cursor_r));
    game_.cursor_f = std::max(0, std::min(pz_.width - 1, game_.cursor_f));
    auto apply_stones = [&](const std::string& coords, char stone) {
        for (size_t i = 0; i + 1 < coords.size(); i += 2) {
            int f = coords[i]     - 'a';
            int r = coords[i + 1] - 'a';
            if (r >= 0 && r < pz_.width && f >= 0 && f < pz_.width)
                game_.board.board[r][f] = stone;
        }
    };
    apply_stones(pz_.initial_black, 1);
    apply_stones(pz_.initial_white, 2);
    game_.board.save_snapshot();
    game_.history.push_back(game_.board);
    last_move_r_ = last_move_f_ = -1;

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;
    review_komi_ = 7.5f;
    move_scores_.assign(1, FLT_MAX);
    move_marked_.assign(1, false);
    marked_paths_.assign(1, "");
    puzzle_eval_.clear();
    puzzle_saved_.clear();
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    companion_path_.clear();
    review_path_.clear();
    is_local_game_ = false;

    // Root from the 1-entry history (also clears drill_edit_path_ — reset below)
    build_analysis_tree();

    // Convert the puzzle tree into analysis children. Every node is heap-owned
    // (unique_ptr chain) and boards are copied member-to-member inside those
    // heap blocks — no GameState ever touches the stack (see the CRITICAL
    // FOOTGUN note: sizeof(GameState) is ~12MB).
    std::function<void(const PuzzleMoveNode&, AnalysisNode*, int)> conv =
        [&](const PuzzleMoveNode& src, AnalysisNode* parent, int color) {
            for (const auto& b : src.branches) {
                if (b.x < 0 || b.y < 0) continue;
                auto ch = std::make_unique<AnalysisNode>();
                ch->board = parent->board;
                if (!ch->board.place_stone(b.y, b.x, color)) continue;  // illegal in file — drop
                ch->board.turn_is_black = color ? 0 : 1;
                ch->move_col      = b.x;
                ch->move_row      = b.y;
                ch->move_color    = color;
                ch->depth         = parent->depth + 1;
                ch->parent        = parent;
                ch->is_main_line  = false;
                ch->drill_correct = b.correct;
                for (const auto& m : b.marks)
                    ch->labels.push_back({m.y, m.x, m.ch});
                parent->children.push_back(std::move(ch));
                conv(b, parent->children.back().get(), 1 - color);
            }
        };
    conv(pz_.tree, analysis_root_.get(), pz_.black_to_play ? 1 : 0);

    analysis_cur_ = analysis_root_.get();
    build_analysis_tree_render();
    drill_edit_path_ = drill_play_path_;   // SAVE becomes OVERWRITE DRILL

    state_ = AppState::GAME_OVER;
    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;
    kata_analysis_enabled_ = false;
    set_status("EDITING DRILL — SAVE VIA OPTIONS MENU");
    draw();
}

// ── Joseki explorer ───────────────────────────────────────────────────────────

// OJE "pretty" coordinate ("Q16") → our (row, col). Columns skip 'I'; row 1 is
// the bottom edge. Returns false for "pass"/"root"/anything unparseable.
static bool jk_parse_placement(const std::string& p, int bs, int& r, int& f) {
    if (p.size() < 2) return false;
    char c = (char)toupper((unsigned char)p[0]);
    if (c < 'A' || c > 'T' || c == 'I') return false;
    f = c - 'A' - (c > 'I' ? 1 : 0);
    int num = atoi(p.c_str() + 1);
    if (num < 1 || num > bs || f < 0 || f >= bs) return false;
    r = bs - num;
    return true;
}

// OJE move-quality category → marker color (matches the website's palette)
static SDL_Color jk_category_color(const std::string& cat) {
    if (cat == "IDEAL")    return {0,   195, 40,  235};
    if (cat == "GOOD")     return {150, 190, 0,   235};
    if (cat == "MISTAKE")  return {210, 30,  45,  235};
    if (cat == "TRICK")    return {255, 215, 0,   235};
    if (cat == "QUESTION") return {0,   180, 220, 235};
    return {160, 160, 160, 235};
}

// Strip the markdown-isms OJE descriptions carry (headings, emphasis, code
// ticks) so the plain-text comment box doesn't render them as noise.
static std::string jk_scrub_markdown(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '#' || c == '*' || c == '`' || c == '\r') continue;
        out += c;
    }
    return out;
}

void App::open_joseki_explorer() {
    state_ = AppState::JOSEKI;
    jk_path_.clear();
    jk_boards_.clear();
    jk_markers_.clear();
    jk_comment_.clear();
    game_.history.clear();
    game_.history_pos = -1;
    game_.board_size  = 19;
    game_.board.reset();
    game_.board.board_size    = 19;
    game_.board.turn_is_black = 1;
    game_.my_color   = -1;   // explorer — no "my side"
    game_.my_turn    = true; // full-brightness cursor
    game_.black_secs = game_.white_secs = -1;
    // Start the cursor on Q16 — the database's canonical corner
    game_.cursor_r = 3;
    game_.cursor_f = 15;
    set_status("JOSEKI — LOADING...");
    jk_fetch_node("root");
    draw();
}

void App::jk_fetch_node(const std::string& node_id) {
    auto it = jk_cache_.find(node_id);
    if (it != jk_cache_.end()) {
        jk_arrive(it->second);
        return;
    }
    if (jk_loading_) return;   // one in-flight fetch at a time
    jk_loading_ = true;
    auto res = std::make_shared<JosekiFetch>();
    res->node_id = node_id;
    jk_fetch_ = res;
    std::thread([res] {
        res->ok = ogs_fetch_joseki(res->node_id, res->pos);
        res->ready.store(true);
        SDL_Event ev{};
        ev.type = g_net_event_type;   // wake the event loop
        SDL_PushEvent(&ev);
    }).detach();
}

void App::poll_joseki_fetch() {
    if (!jk_fetch_ || !jk_fetch_->ready.load()) return;
    auto res = jk_fetch_;
    jk_fetch_.reset();
    jk_loading_ = false;
    if (state_ != AppState::JOSEKI) return;   // user left while it was in flight
    if (!res->ok) {
        flash_       = "OJE FETCH FAILED";
        flash_until_ = SDL_GetTicks() + 2500;
        if (jk_path_.empty()) set_status("JOSEKI — OFFLINE?");
        draw();
        return;
    }
    jk_cache_[res->node_id] = res->pos;
    jk_arrive(res->pos);
}

// Land on a node: apply its placement to the current board (with captures —
// trick lines do capture), snapshot, and refresh the overlay.
//
// GameState is ~12MB (GameState::history embeds a full GameSnapshot[MAX_MOVES]
// by value, not a vector) — it must NEVER be declared as a local/stack variable
// or by-value parameter; the very first version of this function did exactly
// that (`GameState b;`) and stack-overflowed on the spot (confirmed via the
// crash's fault address landing inside ___chkstk_ms). Everything below works
// through a reference into jk_boards_ instead, which is heap-backed.
void App::jk_arrive(const JosekiPosition& pos) {
    if (jk_boards_.empty()) {
        jk_boards_.emplace_back();
        GameState& b0 = jk_boards_.back();
        b0.reset();
        b0.board_size    = 19;
        b0.turn_is_black = 1;
    } else {
        // Two-step (emplace fresh, then assign) rather than push_back(back()):
        // avoids relying on push_back's self-referencing-argument guarantee for
        // a type this size, at the cost of one harmless extra default-construct.
        jk_boards_.emplace_back();
        jk_boards_.back() = jk_boards_[jk_boards_.size() - 2];
    }
    GameState& b = jk_boards_.back();

    if (!jk_path_.empty()) {   // not root — this node is a move on the parent board
        int r, f;
        if (jk_parse_placement(pos.placement, 19, r, f)) {
            int color = b.turn_is_black ? 1 : 0;
            b.board[r][f] = b.turn_is_black ? 1 : 2;
            int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
            int cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
            int cap_count = 0;
            GoRules::find_captured(b.board, color, r, f, cap_r, cap_f, cap_count, 19);
            for (int i = 0; i < cap_count; i++) b.board[cap_r[i]][cap_f[i]] = 0;
            b.turn_is_black = !b.turn_is_black;
            game_.cursor_r = r;   // cursor follows the line being walked
            game_.cursor_f = f;
        } else if (pos.placement == "pass") {
            b.turn_is_black = !b.turn_is_black;
        }
    }
    jk_path_.push_back(pos);
    game_.board = b;   // safe: copies INTO an existing member, not a new stack frame
    jk_show_current();
}

void App::jk_show_current() {
    const JosekiPosition& cur = jk_path_.back();

    jk_markers_.clear();
    for (const auto& m : cur.next_moves) {
        int r, f;
        if (!jk_parse_placement(m.placement, 19, r, f)) continue;   // skips "pass"
        if (game_.board.board[r][f] != 0) continue;
        jk_markers_.push_back({r, f, jk_category_color(m.category)});
    }

    jk_comment_ = jk_scrub_markdown(cur.description);
    if (!cur.source_desc.empty())
        jk_comment_ += "\n\nSOURCE: " + cur.source_desc;

    int depth = (int)jk_path_.size() - 1;
    std::string st = "JOSEKI — ";
    st += (depth == 0) ? "EMPTY BOARD" : "MOVE " + std::to_string(depth)
                                          + "  [" + cur.placement + "]";
    if (!cur.category.empty() && cur.category != "IDEAL" && depth > 0)
        st += "  " + cur.category;
    st += "  —  " + std::to_string((int)cur.next_moves.size()) + " LINES";
    set_status(st);
    draw();
}

void App::jk_step_back() {
    if (jk_path_.size() <= 1) return;
    jk_path_.pop_back();
    jk_boards_.pop_back();
    game_.board = jk_boards_.back();
    jk_show_current();
}

void App::handle_joseki_button(Uint8 btn) {
    int n = game_.board_size - 1;
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        game_.cursor_r = std::max(0, game_.cursor_r - 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        game_.cursor_r = std::min(n, game_.cursor_r + 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        game_.cursor_f = std::max(0, game_.cursor_f - 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        game_.cursor_f = std::min(n, game_.cursor_f + 1); draw(); break;

    case SDL_CONTROLLER_BUTTON_A: {
        // Play the book move under the cursor
        if (jk_loading_ || jk_path_.empty()) break;
        const JosekiPosition& cur = jk_path_.back();
        for (const auto& m : cur.next_moves) {
            int r, f;
            if (jk_parse_placement(m.placement, 19, r, f) &&
                r == game_.cursor_r && f == game_.cursor_f) {
                jk_fetch_node(m.node_id);
                return;
            }
        }
        flash_       = "NOT IN BOOK";
        flash_until_ = SDL_GetTicks() + 1200;
        draw();
        break;
    }

    case SDL_CONTROLLER_BUTTON_B:
        jk_step_back();
        break;

    case SDL_CONTROLLER_BUTTON_START:
        open_popup_menu();
        break;

    default: break;
    }
}

// OGS rank number → display string: 1..29 = 29k..1k, 30+ = 1d+. 0 = unrated.
static std::string ogs_rank_str(int r) {
    if (r <= 0)  return "?";
    if (r < 30)  return std::to_string(30 - r) + "K";
    return std::to_string(r - 29) + "D";
}

void App::open_puzzle_browser() {
    save_companion();  // persist any open review before switching away
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    analysis_tree_render_.clear();
    is_local_game_ = false;
    state_    = AppState::PUZZLE_BROWSE;
    pz_view_  = PzView::COLLECTIONS;
    pz_index_ = 0;
    drill_browse_ = false;   // always land on the collections view, drills re-entered from there
    pz_rebuild_display();   // pinned sets show immediately, even before any fetch
    if (pz_collections_.empty())
        pz_launch_fetch(1, pz_col_page_);   // first visit — load page 1
    else
        draw();                              // reuse the cached page
}

void App::pz_launch_fetch(int kind, int arg) {
    if (pz_loading_) return;
    pz_loading_ = true;
    auto res  = std::make_shared<PuzzleFetch>();
    pz_fetch_ = res;
    int page_size = PZ_PAGE_SIZE;
    std::thread([res, kind, arg, page_size] {
        switch (kind) {
        case 1: res->ok = ogs_fetch_puzzle_collections(arg, page_size,
                                                       res->collections, res->total); break;
        case 2: res->ok = ogs_fetch_puzzle(arg, res->puzzle);            break;
        case 3: res->ok = ogs_fetch_collection_puzzles(arg, res->siblings); break;
        }
        res->kind = kind;
        res->ready.store(true);
        SDL_Event ev{};
        ev.type = g_net_event_type;   // wake the event loop
        SDL_PushEvent(&ev);
    }).detach();
    draw();   // repaint with the LOADING row
}

void App::poll_puzzle_fetch() {
    if (!pz_fetch_ || !pz_fetch_->ready.load()) return;
    auto res = pz_fetch_;
    pz_fetch_.reset();
    pz_loading_ = false;
    // User may have left for another state while the fetch was in flight
    if (state_ != AppState::PUZZLE_BROWSE && state_ != AppState::PUZZLE_PLAY) return;
    if (!res->ok) {
        flash_       = "OGS FETCH FAILED";
        flash_until_ = SDL_GetTicks() + 2500;
        draw();
        return;
    }
    switch (res->kind) {
    case 1:
        pz_collections_ = std::move(res->collections);
        pz_rebuild_display();
        pz_col_total_   = res->total;
        pz_view_        = PzView::COLLECTIONS;
        pz_index_       = std::min(pz_index_, std::max(0, (int)pz_display_cols_.size() - 1));
        break;
    case 2:
        pz_ = std::move(res->puzzle);
        pz_visits_.clear();   // fresh puzzle — line coverage starts over
        drill_play_path_.clear();   // a network puzzle displaced any local drill
        pz_list_pos_ = -1;
        for (int i = 0; i < (int)pz_list_.size(); i++)
            if (pz_list_[i].first == pz_.id) { pz_list_pos_ = i; break; }
        pz_start();
        return;   // pz_start draws
    case 3:
        pz_list_  = std::move(res->siblings);
        pz_view_  = PzView::PUZZLES;
        pz_index_ = 0;
        // Backfill collection ids for solves recorded before the mapping existed,
        // so old progress starts coloring the collections list too.
        if (pz_open_col_id_ > 0) {
            bool changed = false;
            for (const auto& p : pz_list_)
                if (pz_solved_ids_.count(p.first) &&
                    pz_solved_col_[p.first] != pz_open_col_id_) {
                    pz_solved_col_[p.first] = pz_open_col_id_;
                    changed = true;
                }
            if (changed) save_solved_puzzles();
        }
        break;
    }
    draw();
}

// Set the board to the puzzle's initial position and enter PUZZLE_PLAY.
// Also serves as "retry" — everything is rebuilt from pz_.
void App::pz_start() {
    if (pz_.width != pz_.height || pz_.width < 2 || pz_.width > MAX_BOARD_SIZE) {
        flash_       = "UNSUPPORTED BOARD SIZE";
        flash_until_ = SDL_GetTicks() + 2500;
        state_ = AppState::PUZZLE_BROWSE;
        draw();
        return;
    }
    // Local drills get a fresh random orientation on every attempt (including
    // circle-retry) so the shape is learned, not its screen position. Tree
    // pointers are untouched — pz_visits_ line coverage survives the reshuffle.
    // The cursor is mapped along with the stones so it stays on the shape.
    // (EDIT DRILL reloads from disk, so saves stay in authored orientation.)
    if (!drill_play_path_.empty())
        drill_random_transform(pz_, &game_.cursor_f, &game_.cursor_r);
    game_.history.clear();
    game_.history_pos = -1;
    game_.board_size  = pz_.width;
    game_.board.reset();
    game_.board.board_size    = pz_.width;
    game_.board.turn_is_black = pz_.black_to_play ? 1 : 0;
    game_.my_color   = pz_.black_to_play ? 1 : 0;
    game_.my_turn    = true;
    game_.black_name = "BLACK";
    game_.white_name = "WHITE";
    game_.black_rank = game_.white_rank = "";
    game_.black_secs = game_.white_secs = -1;
    // Keep the cursor where it was (clamped to this board) — recentering on every
    // puzzle jump/retry forces a re-approach when grinding through a collection.
    game_.cursor_r = std::max(0, std::min(pz_.width - 1, game_.cursor_r));
    game_.cursor_f = std::max(0, std::min(pz_.width - 1, game_.cursor_f));
    auto apply_stones = [&](const std::string& coords, char stone) {
        for (size_t i = 0; i + 1 < coords.size(); i += 2) {
            int f = coords[i]     - 'a';
            int r = coords[i + 1] - 'a';
            if (r >= 0 && r < pz_.width && f >= 0 && f < pz_.width)
                game_.board.board[r][f] = stone;
        }
    };
    apply_stones(pz_.initial_black, 1);
    apply_stones(pz_.initial_white, 2);
    game_.board.save_snapshot();
    game_.history.push_back(game_.board);
    last_move_r_ = last_move_f_ = -1;

    pz_node_            = &pz_.tree;
    pz_done_            = false;
    pz_solved_          = false;
    pz_explore_         = false;
    pz_explore_anchor_  = 0;
    pz_more_lines_      = false;
    pz_pending_reply_   = nullptr;
    pz_refresh_marks();       // root-node marks annotate the initial position
    pz_build_tree_render();   // solution tree in the left panel
    pz_banner_.clear();
    pz_comment_  = pz_.description;   // the author's task statement
    black_label_ = game_.black_name;
    white_label_ = game_.white_name;
    state_ = AppState::PUZZLE_PLAY;
    std::string rank = (pz_.rank > 0) ? "  (" + ogs_rank_str(pz_.rank) + ")" : "";
    set_status("YOU ARE " + std::string(pz_.black_to_play ? "BLACK" : "WHITE") + rank);
    draw();
}

// Project the puzzle's solution tree into the analysis-tree renderer's node
// format (same column-assignment walk as build_analysis_tree_render). Correct-
// answer nodes get the "marked" highlight so solution endpoints are visible.
void App::pz_build_tree_render() {
    pz_tree_render_.clear();
    pz_cur_depth_ = 0;
    int max_col      = 0;
    int player_black = pz_.black_to_play ? 1 : 0;
    std::function<void(const PuzzleMoveNode*, int, int, int, int)> dfs =
        [&](const PuzzleMoveNode* node, int depth, int col, int parent_col, int parent_depth) {
            AnalysisTreeRenderNode rn;
            rn.depth        = depth;
            rn.col          = col;
            rn.current      = (node == pz_node_);
            if (rn.current) pz_cur_depth_ = depth;
            rn.parent_depth = parent_depth;
            rn.parent_col   = parent_col;
            // Root has no move; odd depths are the solver's moves
            rn.move_color   = (depth == 0) ? -1
                            : (depth % 2 == 1) ? player_black : 1 - player_black;
            rn.goal         = node->correct;   // green halo on the node itself —
                                               // full-row marked highlights read as
                                               // noise when several goals coexist
            pz_tree_render_.push_back(rn);
            for (int i = 0; i < (int)node->branches.size(); i++) {
                int child_col = (i == 0) ? col : ++max_col;
                dfs(&node->branches[i], depth + 1, child_col, col, depth);
            }
        };
    dfs(&pz_.tree, 0, 0, 0, -1);
}

static constexpr int PZ_OPPONENT_REPLY_DELAY_MS = 500;

// Land on a solution-tree node (just reached by whoever moved), judge it, and
// let the automatic opponent respond when the line continues.
// Does this subtree contain a correct ending at all? Wrong lines and judged
// dead-ends exist to punish mistakes — they shouldn't be required viewing for
// coverage, and the opponent shouldn't steer toward them.
static bool pz_leads_to_correct(const PuzzleMoveNode* n) {
    if (n->correct) return true;
    for (const auto& b : n->branches)
        if (pz_leads_to_correct(&b)) return true;
    return false;
}

// True if a never-traversed node lies on some path to a correct ending within
// this subtree — drives both the opponent's steer-toward-unseen-lines choice
// and the "MORE LINES REMAIN" / "ALL LINES SEEN" verdict after a solve.
bool App::pz_subtree_unexplored(const PuzzleMoveNode* n) const {
    if (!pz_leads_to_correct(n)) return false;
    auto it = pz_visits_.find(n);
    if (it == pz_visits_.end() || it->second == 0) return true;
    for (const auto& b : n->branches)
        if (pz_subtree_unexplored(&b)) return true;
    return false;
}

void App::pz_advance(const PuzzleMoveNode* node, bool opponent_follows) {
    pz_node_ = node;
    pz_visits_[node]++;   // every traversed node counts toward line coverage
    if (!node->text.empty()) pz_comment_ = node->text;
    if (!node->marks.empty()) pz_refresh_marks();   // new annotations replace the old
    pz_build_tree_render();                          // move the tree-panel highlight

    if (node->correct) {
        pz_done_   = true;
        pz_solved_ = true;
        pz_banner_ = "SOLVED!";
        if (pz_.id > 0) {
            bool fresh = pz_solved_ids_.insert(pz_.id).second;
            // Record/repair the collection mapping too — legacy solves have 0 here
            bool remap = pz_.collection_id > 0 &&
                         pz_solved_col_[pz_.id] != pz_.collection_id;
            if (remap) pz_solved_col_[pz_.id] = pz_.collection_id;
            if (fresh || remap) save_solved_puzzles();
        }
        // Whole-tree truth, not just this run's path: anything anywhere in the
        // authored tree never yet traversed (opponent resistance you haven't
        // faced, or your own alternative lines you haven't tried) keeps the
        // "more lines" nudge alive.
        pz_more_lines_ = false;
        for (const auto& b : pz_.tree.branches)
            if (pz_subtree_unexplored(&b)) { pz_more_lines_ = true; break; }
        set_status(pz_more_lines_
                       ? "MORE LINES REMAIN — " GLYPH_PS_CIRCLE ": RETRY"
                       : "ALL LINES SEEN   R3: NEXT   " GLYPH_PS_CIRCLE ": RETRY   " GLYPH_PS_SQUARE ": LIST");
        draw();
        return;
    }
    if (node->wrong || node->branches.empty()) {
        pz_done_   = true;
        pz_solved_ = false;
        pz_banner_ = "WRONG";
        set_status("PRESS " GLYPH_PS_CIRCLE " TO RETRY");
        draw();
        return;
    }
    if (opponent_follows && pz_.opponent_auto) {
        // Opponent resistance choice. First preference: a reply whose subtree
        // still contains something never traversed — raw least-visited counts
        // alone can ping-pong between an already-exhausted shallow branch and
        // a deep bushy one, re-treading known lines while unseen ones wait.
        // Only when everything below this node has been seen does it fall back
        // to least-visited for variety.
        const PuzzleMoveNode* reply = nullptr;
        int fewest = INT_MAX;
        for (const auto& b : node->branches) {
            if (!pz_subtree_unexplored(&b)) continue;
            auto it = pz_visits_.find(&b);
            int v = (it == pz_visits_.end()) ? 0 : it->second;
            if (v < fewest) { fewest = v; reply = &b; }
        }
        if (!reply) {
            for (const auto& b : node->branches) {
                auto it = pz_visits_.find(&b);
                int v = (it == pz_visits_.end()) ? 0 : it->second;
                if (v < fewest) { fewest = v; reply = &b; }
            }
        }

        // Delay placing it slightly so the player's own move is visible on
        // its own for a beat, instead of both stones landing in the same
        // frame — fired from the main loop tick via pz_fire_pending_reply().
        pz_pending_reply_ = reply;
        pz_reply_at_      = SDL_GetTicks() + PZ_OPPONENT_REPLY_DELAY_MS;
        draw();
        return;
    }
    set_status("YOUR MOVE");
    draw();
}

// Apply the opponent's chosen reply once the visibility delay has elapsed,
// then keep judging from there (it may itself be solved/wrong/branch again).
void App::pz_fire_pending_reply() {
    const PuzzleMoveNode* reply = pz_pending_reply_;
    pz_pending_reply_ = nullptr;
    if (!reply) return;

    if (reply->x >= 0 && reply->y >= 0 &&
        reply->y < game_.board_size && reply->x < game_.board_size &&
        game_.board.board[reply->y][reply->x] == 0) {
        int opp_black = (game_.board.turn_is_black == 1);
        game_.board.save_snapshot();
        // Only mutate turn/history if the placement actually happened —
        // a refused move must never flip whose turn it is
        if (game_.board.place_stone(reply->y, reply->x, opp_black)) {
            game_.board.turn_is_black = opp_black ? 0 : 1;
            game_.history.push_back(game_.board);
            last_move_r_ = reply->y;
            last_move_f_ = reply->x;
        }
    }
    pz_advance(reply, false);
}

// Player plays at (r, f): match against the current node's branches — or, once
// exploring (triangle, off the authored tree, or after a verdict), free
// exploration: stones alternate colors with no judging and no auto-opponent, so
// lines the author didn't cover can be tested by playing both sides. Triggers
// review the moves made so far; circle returns to the judged position explore
// mode branched off from (or retries the whole puzzle if not exploring).
void App::pz_place(int r, int f) {
    if (!pz_node_) return;
    if (pz_pending_reply_) return;  // opponent's reply is mid-delay — not your turn yet
    if (r < 0 || f < 0 || r >= game_.board_size || f >= game_.board_size) return;

    if (game_.history_pos >= 0) {
        // Placing from a reviewed (non-live) position discards whatever came
        // after, same as a normal undo-then-move — only ever reachable while
        // already exploring (triggers are gated on pz_explore_), so pz_node_
        // (frozen since exploration began) never needs to move here. If this
        // rewinds past the explore anchor (scrubbed back into the judged
        // prefix, then branched), pull the anchor in too — it must never sit
        // beyond the live history length, or "back to solving" would try to
        // GROW game_.history, default-constructing GameState elements (each
        // ~12MB — see the CRITICAL FOOTGUN note on why that's dangerous).
        game_.history.resize(game_.history_pos + 1);
        game_.board         = game_.history.back();
        game_.history_pos   = -1;
        pz_explore_anchor_  = std::min(pz_explore_anchor_, (int)game_.history.size());
    }

    if (game_.board.board[r][f] != 0) return;

    int is_black = (game_.board.turn_is_black == 1);

    // Legality first — an illegal move must change NOTHING (a rejected placement
    // previously still flipped the turn, silently swapping colors for the next click).
    if (GoRules::would_be_suicide(game_.board.board, r, f, is_black, game_.board_size)) {
        ko_flash_until_ = SDL_GetTicks() + 400;   // red cursor flash, like live play
        draw();
        return;
    }
    // Simple ko: simulated result must not recreate the position before the last move
    if (game_.history.size() >= 2) {
        char test[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
        memcpy(test, game_.board.board, sizeof(test));
        test[r][f] = is_black ? 1 : 2;
        int cap_r[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
        int cap_f[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
        int cap_count = 0;
        GoRules::find_captured(test, is_black, r, f, cap_r, cap_f, cap_count, game_.board_size);
        for (int i = 0; i < cap_count; i++) test[cap_r[i]][cap_f[i]] = 0;
        if (memcmp(test, game_.history[game_.history.size() - 2].board, sizeof(test)) == 0) {
            ko_flash_until_ = SDL_GetTicks() + 400;
            draw();
            return;
        }
    }

    // Place the stone (captures apply) so the player sees their move either way
    game_.board.save_snapshot();
    game_.board.place_stone(r, f, is_black);
    game_.board.turn_is_black = is_black ? 0 : 1;
    game_.history.push_back(game_.board);
    last_move_r_ = r;
    last_move_f_ = f;

    if (pz_explore_ || pz_done_) {
        // Already exploring (or continuing past a verdict) — sandbox move
        if (!pz_explore_) {
            pz_explore_anchor_ = (int)game_.history.size() - 1;  // the verdict position
            pz_explore_ = true;
            pz_banner_.clear();   // drop the SOLVED!/WRONG banner once exploring
        }
        set_status("EXPLORING — " GLYPH_PS_CIRCLE " BACK TO SOLVING\nTRIGGERS: REVIEW");
        draw();
        return;
    }

    const PuzzleMoveNode* hit = nullptr;
    for (const auto& b : pz_node_->branches)
        if (b.x == f && b.y == r) { hit = &b; break; }

    if (!hit) {
        // Off the authored tree: not judged, just warned — free exploration from
        // here on (both sides played manually; there is no authored reply anyway).
        pz_explore_anchor_ = (int)game_.history.size() - 1;  // undo this off-tree move
        pz_explore_ = true;
        pz_comment_ = "OFF THE SOLUTION TREE — FREE PLAY, BOTH SIDES";
        set_status("OFF TREE — " GLYPH_PS_CIRCLE " BACK TO SOLVING\nTRIGGERS: REVIEW");
        draw();
        return;
    }
    pz_advance(hit, /*opponent_follows=*/true);
}

// Triangle: drop into the free sandbox right where you stand, without playing
// a move first. Only meaningful pre-verdict — once pz_done_, any placement
// already falls into the same sandbox (see pz_place).
void App::pz_enter_explore() {
    if (pz_explore_ || pz_done_ || pz_pending_reply_) return;
    pz_explore_anchor_ = (int)game_.history.size();
    pz_explore_ = true;
    pz_banner_.clear();
    pz_comment_ = "FREE PLAY — BOTH SIDES";
    set_status("EXPLORING — " GLYPH_PS_CIRCLE " BACK TO SOLVING\nTRIGGERS: REVIEW");
    draw();
}

// Circle, mid-exploration: discard the sandbox detour and resume at the judged
// position explore mode branched off from. pz_node_ never moves while
// pz_explore_ is set, so it's already sitting at the right spot — this just
// rewinds the board/history to match and restores the verdict banner, if any.
void App::pz_return_to_solving() {
    pz_pending_reply_ = nullptr;  // defensive — unreachable in practice, see pz_place()/pz_enter_explore() guards
    // Defense in depth: this resize must only ever shrink. pz_place() keeps the
    // anchor clamped to <= history size already, but a stale/out-of-range anchor
    // here would otherwise GROW game_.history, default-constructing GameState
    // elements (~12MB each — see the CRITICAL FOOTGUN note on why that's unsafe).
    int target = std::min((int)game_.history.size(), std::max(1, pz_explore_anchor_));
    game_.history.resize(target);
    game_.board       = game_.history.back();
    game_.history_pos = -1;
    pz_explore_        = false;
    pz_build_tree_render();
    if (pz_done_) {
        pz_banner_ = pz_solved_ ? "SOLVED!" : "WRONG";
        set_status(pz_solved_
            ? (pz_more_lines_ ? GLYPH_PS_CIRCLE ": MORE RESISTANCE LINES REMAIN"
                               : "R3: NEXT   " GLYPH_PS_CIRCLE ": RETRY   " GLYPH_PS_SQUARE ": LIST")
            : "PRESS " GLYPH_PS_CIRCLE " TO RETRY");
    } else {
        pz_banner_.clear();
        pz_comment_ = (pz_node_ && !pz_node_->text.empty()) ? pz_node_->text : pz_.description;
        std::string rank = (pz_.rank > 0) ? "  (" + ogs_rank_str(pz_.rank) + ")" : "";
        set_status("YOU ARE " + std::string(pz_.black_to_play ? "BLACK" : "WHITE") + rank);
    }
    draw();
}

// L3/R3: previous/next puzzle within the open collection.
void App::pz_step(int dir) {
    if (pz_list_.empty() || pz_list_pos_ < 0 || pz_loading_) return;
    int n    = (int)pz_list_.size();
    int next = (pz_list_pos_ + dir + n) % n;
    if (drill_browse_ && !drill_paths_.empty()) {
        drill_load_and_start(next);   // local files — no network fetch
        return;
    }
    pz_launch_fetch(2, pz_list_[next].first);
}

void App::draw_puzzle_browser() {
    // Progress coloring: collections turn yellow once any of their puzzles is
    // solved and green once all of them are; solved puzzles show green in the
    // list view. Alpha 0 = keep the list screen's normal white/accent colors.
    static constexpr SDL_Color PZ_NO_COLOR = {0,   0,   0,   0};
    static constexpr SDL_Color PZ_STARTED  = {255, 213, 74,  255};
    static constexpr SDL_Color PZ_DONE     = {105, 220, 130, 255};

    std::vector<std::string> lines;
    std::vector<SDL_Color>   colors;
    std::string title, footer;
    if (pz_view_ == PzView::COLLECTIONS) {
        int pages = std::max(1, (pz_col_total_ + PZ_PAGE_SIZE - 1) / PZ_PAGE_SIZE);
        title = "OGS PUZZLES — PAGE " + std::to_string(pz_col_page_)
              + "/" + std::to_string(pages);
        std::map<int, int> solved_per_col = pz_solved_per_collection();
        for (const auto& c : pz_display_cols_) {
            std::string ln = c.name + "  —  " + std::to_string(c.puzzle_count) + " PUZZLES";
            if (c.min_rank > 0 || c.max_rank > 0)
                ln += "  " + ogs_rank_str(c.min_rank) + "-" + ogs_rank_str(c.max_rank);
            if (c.rating > 0.f) {
                char rb[16];
                snprintf(rb, sizeof(rb), "  R%.1f", c.rating);
                ln += rb;
            }
            ln += "  BY " + c.owner;
            lines.push_back(ln);
            auto it = solved_per_col.find(c.id);
            int solved = (it != solved_per_col.end()) ? it->second : 0;
            colors.push_back(c.puzzle_count > 0 && solved >= c.puzzle_count ? PZ_DONE
                             : solved > 0                                   ? PZ_STARTED
                                                                            : PZ_NO_COLOR);
        }
        footer = GLYPH_PS_CROSS " OPEN   LEFT/RIGHT: PAGE   " GLYPH_PS_CIRCLE " LOBBY";
    } else if (drill_browse_) {
        // Local drills: no solved tracking (ids are negative sentinels)
        title = "MY DRILLS  (" + std::to_string((int)pz_list_.size()) + ")";
        for (const auto& p : pz_list_) {
            lines.push_back(p.second);
            colors.push_back(PZ_NO_COLOR);
        }
        footer = drill_rename_active_
                     ? "NEW NAME: " + drill_rename_buf_ + "_   ENTER=OK   ESC=CANCEL"
                     : GLYPH_PS_CROSS " DRILL   " GLYPH_PS_CIRCLE " COLLECTIONS";
        // Refresh the setup-position preview when the cursor moved to a new file
        if (pz_index_ != drill_thumb_idx_) {
            drill_thumb_idx_ = pz_index_;
            drill_thumb_bs_  = 0;
            if (pz_index_ >= 0 && pz_index_ < (int)drill_paths_.size()) {
                OgsPuzzle tmp;
                std::string err;
                if (load_drill_sgf(drill_paths_[pz_index_], tmp, err) &&
                    tmp.width <= BOARD_SIZE) {
                    memset(drill_thumb_, 0, sizeof(drill_thumb_));
                    auto apply = [&](const std::string& s, char stone) {
                        for (size_t i = 0; i + 1 < s.size(); i += 2) {
                            int f = s[i] - 'a', r = s[i + 1] - 'a';
                            if (r >= 0 && r < tmp.width && f >= 0 && f < tmp.width)
                                drill_thumb_[r][f] = stone;
                        }
                    };
                    apply(tmp.initial_black, 1);
                    apply(tmp.initial_white, 2);
                    drill_thumb_bs_ = tmp.width;
                }
            }
        }
    } else {
        int solved_here = 0;
        for (const auto& p : pz_list_)
            if (pz_solved_ids_.count(p.first)) solved_here++;
        title = "PUZZLES — " + pz_list_title_ + "  (" + std::to_string(solved_here)
              + "/" + std::to_string((int)pz_list_.size()) + " SOLVED)";
        for (const auto& p : pz_list_) {
            bool solved = pz_solved_ids_.count(p.first) != 0;
            lines.push_back((solved ? "[X] " : "[ ] ") + p.second);
            colors.push_back(solved ? PZ_DONE : PZ_NO_COLOR);
        }
        footer = GLYPH_PS_CROSS " SOLVE   " GLYPH_PS_CIRCLE " COLLECTIONS";
    }
    if (pz_loading_) {
        lines.push_back("LOADING...");
        colors.push_back(PZ_NO_COLOR);
    }
    // The list screen presents itself unless something needs to layer on top
    // (the START popup, or the drill setup-position preview board).
    bool want_thumb = drill_browse_ && pz_view_ == PzView::PUZZLES && drill_thumb_bs_ > 0;
    bool self_present = !popup_active_ && !want_thumb;
    renderer_->draw_list_screen(title.c_str(), lines, pz_index_, footer.c_str(),
                                self_present, colors.data());
    if (want_thumb) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer_->sdl, &w, &h);
        int size = std::min(w / 3, h / 2);
        renderer_->render_mini_board(w - size - 40, (h - size) / 2, size,
                                     drill_thumb_, drill_thumb_bs_);
    }
    if (popup_active_)
        renderer_->draw_popup_menu(popup_title_.c_str(), popup_labels_.data(),
                                   (int)popup_labels_.size(), popup_index_);
    if (!self_present)
        SDL_RenderPresent(renderer_->sdl);
}

// Input for both puzzle states (called from handle_controller_button).
void App::handle_puzzle_button(Uint8 btn) {
    if (state_ == AppState::PUZZLE_BROWSE) {
        int total = (pz_view_ == PzView::COLLECTIONS) ? (int)pz_display_cols_.size()
                                                      : (int)pz_list_.size();
        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (pz_index_ > 0) { pz_index_--; draw(); }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (pz_index_ + 1 < total) { pz_index_++; draw(); }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            if (pz_view_ == PzView::COLLECTIONS && pz_col_page_ > 1 && !pz_loading_) {
                pz_col_page_--;
                pz_index_ = 0;
                pz_launch_fetch(1, pz_col_page_);
            }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            if (pz_view_ == PzView::COLLECTIONS && !pz_loading_ &&
                pz_col_page_ * PZ_PAGE_SIZE < pz_col_total_) {
                pz_col_page_++;
                pz_index_ = 0;
                pz_launch_fetch(1, pz_col_page_);
            }
            break;
        case SDL_CONTROLLER_BUTTON_A:
            if (pz_loading_ || total == 0 || pz_index_ >= total) break;
            if (pz_view_ == PzView::COLLECTIONS) {
                const auto& c = pz_display_cols_[pz_index_];
                if (c.id == -1) {           // [MY DRILLS] — local, no fetch
                    open_drill_list();
                    break;
                }
                if (c.starting_puzzle_id > 0) {
                    drill_browse_ = false;  // entering a real OGS collection
                    drill_paths_.clear();
                    pz_list_title_  = c.name;
                    pz_open_col_id_ = c.id;   // for the solved-mapping backfill
                    // Remember this collection (metadata refresh included) so the
                    // pinned MY SETS section can show it from any page, any session
                    pz_known_cols_[c.id] = c;
                    save_known_collections();
                    pz_launch_fetch(3, c.starting_puzzle_id);
                }
            } else if (drill_browse_) {
                drill_load_and_start(pz_index_);
            } else {
                pz_launch_fetch(2, pz_list_[pz_index_].first);
            }
            break;
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_X:
            if (pz_view_ == PzView::PUZZLES) {
                pz_view_  = PzView::COLLECTIONS;
                pz_index_ = 0;
                drill_browse_ = false;
                pz_rebuild_display();   // solves made in this collection may pin it
                draw();
            } else {
                drill_browse_ = false;
                state_ = AppState::LOBBY;
                set_status("");
                draw();
            }
            break;
        case SDL_CONTROLLER_BUTTON_START:
            open_popup_menu();
            break;
        default: break;
        }
        return;
    }

    // PUZZLE_PLAY
    int n = game_.board_size - 1;
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        game_.cursor_r = std::max(0, game_.cursor_r - 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        game_.cursor_r = std::min(n, game_.cursor_r + 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        game_.cursor_f = std::max(0, game_.cursor_f - 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        game_.cursor_f = std::min(n, game_.cursor_f + 1); draw(); break;
    case SDL_CONTROLLER_BUTTON_A:
        pz_place(game_.cursor_r, game_.cursor_f);
        break;
    case SDL_CONTROLLER_BUTTON_B:
        if (pz_explore_) pz_return_to_solving();  // back to where exploring branched off
        else             pz_start();              // retry from the top
        break;
    case SDL_CONTROLLER_BUTTON_X:
        state_    = AppState::PUZZLE_BROWSE;
        pz_view_  = pz_list_.empty() ? PzView::COLLECTIONS : PzView::PUZZLES;
        pz_index_ = std::max(0, pz_list_pos_);
        draw();
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        pz_enter_explore();
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        pz_step(-1);
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        pz_step(+1);
        break;
    case SDL_CONTROLLER_BUTTON_START:
        open_popup_menu();
        break;
    default: break;
    }
}

// L3/R3 while reviewing: jump straight to the previous/next SGF in the same
// directory as the open file — cycling marked positions or study puzzles without
// a round trip through the catalog. Files are ordered by name (case-insensitive);
// both the marked/ and puzzles/ naming schemes start with a date+time, so name
// order is chronological order.
std::string App::next_review_sibling(int dir, int* out_index, int* out_total) const {
    if (review_path_.empty()) return "";
    size_t sep = review_path_.find_last_of("/\\");
    if (sep == std::string::npos) return "";
    std::string dir_path = review_path_.substr(0, sep);
    std::string cur_name = review_path_.substr(sep + 1);

    std::vector<std::string> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(
        Catalog::utf8_to_wide(Catalog::join_path(dir_path, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = Catalog::wide_to_utf8(fd.cFileName);
        if (name.size() < 4) continue;
        std::string ext = name.substr(name.size() - 4);
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".sgf") files.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (files.size() < 2) return "";
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    int idx = 0;
    for (int i = 0; i < (int)files.size(); i++)
        if (files[i] == cur_name) { idx = i; break; }
    int next = (idx + dir + (int)files.size()) % (int)files.size();
    if (out_index) *out_index = next + 1;
    if (out_total) *out_total = (int)files.size();
    return Catalog::join_path(dir_path, files[next]);
}

void App::review_cycle(int dir) {
    if (state_ != AppState::GAME_OVER || review_path_.empty()) return;
    int next_idx = 0, total = 0;
    std::string next_path = next_review_sibling(dir, &next_idx, &total);
    if (next_path.empty()) {
        flash_       = "NO OTHER FILES HERE";
        flash_until_ = SDL_GetTicks() + 1500;
        draw();
        return;
    }
    save_companion();  // persist this file's scores/marks before moving on
    load_sgf_for_review(next_path);
    flash_       = "FILE " + std::to_string(next_idx) + "/" + std::to_string(total);
    flash_until_ = SDL_GetTicks() + 1500;
    draw();
}

// Advance analysis_cur_ one ply along the active-child main line — shared by
// the L2/R2 step-forward button handler and the autoplay tick. No-op at a
// leaf (game end) or with no open analysis.
void App::analysis_step_forward() {
    if (!analysis_cur_ || analysis_cur_->children.empty()) return;
    analysis_cur_ = analysis_cur_->children[analysis_cur_->active_child].get();
    build_analysis_tree_render();
    kata_suggestion_count_ = 0;
    kata_score_lead_ = cached_analysis_score(analysis_cur_);
    kata_query_after_ = SDL_GetTicks() + 1000;
}

void App::load_sgf_for_review(const std::string& path) {
    SgfGame g;
    if (!load_sgf(path, g)) return;

    // Reset game state — no live network, just a board position sequence
    game_.history.clear();
    game_.history_pos = -1;
    game_.board_size  = g.board_size;
    game_.board.board_size = g.board_size;
    game_.board.reset();
    game_.board.turn_is_black = 1;
    game_.cursor_r = game_.cursor_f = g.board_size / 2;
    game_.black_name  = g.black_name;
    game_.white_name  = g.white_name;
    game_.result      = g.result[0] ? g.result : "?";
    game_.my_color    = -1;  // review only — no "my side"
    game_.black_secs  = -1;
    game_.white_secs  = -1;

    // Apply AB[]/AW[] setup stones directly to the board as a single starting
    // position — they're facts about the position, not sequential moves, so no
    // capture logic runs and they collapse into one history entry, not one per stone.
    for (int i = 0; i < g.setup_count; i++) {
        int r, f;
        if (parse_sgf_move(g.moves[i], r, f))
            game_.board.board[r][f] = (g.colors[i] == 1) ? 1 : 2;
    }
    if (g.has_pl) {
        game_.board.turn_is_black = g.start_black;
    } else if (g.setup_count > 0) {
        // No explicit PL[]: fall back to the standard handicap convention —
        // whoever placed the last setup stone, the other color moves first.
        game_.board.turn_is_black = (g.colors[g.setup_count - 1] == 1) ? 0 : 1;
    } else {
        game_.board.turn_is_black = 1;
    }
    game_.history.push_back(game_.board);

    // Replay actual moves: trust the explicit color tags in the SGF
    for (int i = g.setup_count; i < g.move_count; i++) {
        int r, f;
        bool is_black = (g.colors[i] == 1);
        game_.board.turn_is_black = is_black ? 1 : 0;
        if (parse_sgf_move(g.moves[i], r, f)) {
            game_.board.save_snapshot();  // required for KO detection in place_stone
            game_.board.place_stone(r, f, is_black);
        }
        game_.board.turn_is_black = is_black ? 0 : 1;
        game_.history.push_back(game_.board);
    }

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;
    review_komi_ = g.komi;  // from KM[] (with the Fox-Go 375→3.5 guard applied at parse time)

    // Reset score graph / mark storage and derive companion path
    move_scores_.assign(game_.history.size(), FLT_MAX);
    move_marked_.assign(game_.history.size(), false);
    marked_paths_.assign(game_.history.size(), "");
    puzzle_eval_.clear();   // reviews never generate puzzles (my_color = -1),
    puzzle_saved_.clear();  // but don't let live-game leftovers linger either
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    companion_path_    = path.substr(0, path.rfind('.')) + ".katago";
    review_path_       = path;  // enables L3/R3 cycling through sibling files
    load_companion();   // pre-populate scores and marks if the file exists

    // Build analysis tree from the replayed history
    build_analysis_tree();

    // Enter GAME_OVER analysis mode
    state_ = AppState::GAME_OVER;
    set_status("REVIEW — " + std::string(g.result));
    kata_suggestion_count_ = 0;
    kata_score_lead_ = cached_analysis_score(analysis_cur_);  // instant if the companion file had it
    kata_analysis_enabled_ = false;
}

// ── Network message handler ───────────────────────────────────────────────────

void App::handle_net_msg(const NetMsg& msg) {
    switch (msg.type) {
    case NetMsgType::AUTH_OK:
        state_ = AppState::LOBBY;
        set_status("");
        load_demo_game();
        draw();
        break;

    case NetMsgType::AUTH_FAIL:
        // Reset to credential prompt so the user can re-enter
        state_    = AppState::CREDENTIAL_PROMPT;
        cred_step_ = 1;
        cred_username_.clear();
        cred_password_.clear();
        cred_buf_.clear();
        flash_       = "Login failed: " + msg.text;
        flash_until_ = SDL_GetTicks() + 4000;
        draw();
        break;

    case NetMsgType::MATCH_FOUND:
        set_status("MATCH FOUND — CONNECTING...");
        draw();
        break;

    case NetMsgType::GAME_CONNECTED: {
        game_.result.clear();
        game_.pending_col  = -2;
        game_.pending_row  = -2;
        game_.history.clear();
        game_.history_pos  = -1;
        memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
        memset(game_.ownership,   0, sizeof(game_.ownership));
        game_.game_id      = msg.game_id;
        game_.board_size   = msg.board_size;
        game_.my_color     = msg.my_color;
        game_.my_player_id = msg.my_player_id;
        game_.black_name = msg.black_name;
        game_.white_name = msg.white_name;
        game_.black_rank = msg.black_rank;
        game_.white_rank = msg.white_rank;
        // Real player names. The opponent's rank stays hidden while the game is
        // live — play the board, not the rating — and is revealed at game over.
        black_label_ = msg.black_name;
        white_label_ = msg.white_name;
        if (msg.my_color == 1 && !msg.black_rank.empty())
            black_label_ += " [" + msg.black_rank + "]";
        if (msg.my_color == 0 && !msg.white_rank.empty())
            white_label_ += " [" + msg.white_rank + "]";
        game_.black_secs        = msg.black_secs;
        game_.white_secs        = msg.white_secs;
        game_.black_periods     = msg.black_periods;
        game_.white_periods     = msg.white_periods;
        game_.black_period_secs = msg.black_period_secs;
        game_.white_period_secs = msg.white_period_secs;
        game_.black_in_byo      = msg.black_in_byo;
        game_.white_in_byo      = msg.white_in_byo;
        game_.clock_tick        = SDL_GetTicks();
        // Reconnect: the running player's countdown is genuinely mid-turn, but
        // the parked player's stored value is their last turn's leftover.
        reset_byo_countdowns(/*running_player_too=*/false);
        game_.cursor_r     = msg.board_size / 2;
        game_.cursor_f     = msg.board_size / 2;
        game_.board.reset();
        game_.board.board_size = msg.board_size;
        game_.handicap      = msg.handicap;
        game_.free_handicap = msg.free_handicap;

        // Snapshot empty board as history[0] (before any stones)
        game_.history.push_back(game_.board);

        // Pre-placed handicap stones (non-free handicap): always black, don't alter turn
        for (auto& [col, row] : msg.initial_handicap_stones)
            apply_move(col, row, 1);
        // Now set who plays the first real move
        game_.board.turn_is_black = msg.initial_player;

        // Apply moves from reconnect / ongoing game
        for (auto& [col, row] : msg.initial_moves)
            apply_move(col, row);

        // Determine whose turn it is
        bool black_to_play = (game_.board.turn_is_black == 1);
        game_.my_turn = (black_to_play && game_.my_color == 1) ||
                        (!black_to_play && game_.my_color == 0);

        // Initialise score-graph / mark storage. assign(), not resize() — resize
        // never shrinks and keeps old values, which leaked the previous game's
        // scores into this one whenever the previous game was longer.
        move_scores_.assign(game_.history.size(), FLT_MAX);
        move_marked_.assign(game_.history.size(), false);
        marked_paths_.assign(game_.history.size(), "");
        puzzle_eval_.clear();
        puzzle_saved_.clear();
        review_path_.clear();  // this GAME_OVER will be a live game's, not a loaded file
        bg_analysis_next_  = 0;
        bg_analysis_depth_ = -1;
        bg_analysis_busy_  = false;
        review_komi_       = 7.5f;  // OGS standard komi; not read from server game data here

        state_ = AppState::PLAYING;
        close_popup_menu();  // a SEARCHING popup is stale once the game starts
        sound_.play_game_start();
        set_status(game_.my_turn ? "YOUR TURN" : "WAITING...");
        draw();
        break;
    }

    case NetMsgType::OPPONENT_MOVE:
        if (state_ != AppState::PLAYING) break;
        {
            // Update clocks regardless of whose move it was
            if (msg.black_secs >= 0) {
                game_.black_secs        = msg.black_secs;
                game_.black_periods     = msg.black_periods;
                game_.black_period_secs = msg.black_period_secs;
                game_.black_in_byo      = msg.black_in_byo;
                game_.clock_tick = SDL_GetTicks();
            }
            if (msg.white_secs >= 0) {
                game_.white_secs        = msg.white_secs;
                game_.white_periods     = msg.white_periods;
                game_.white_period_secs = msg.white_period_secs;
                game_.white_in_byo      = msg.white_in_byo;
            }

            if (game_.pending_col != -2) {
                // Server echo of our own move — already applied optimistically, just clear
                game_.pending_col = -2;
                game_.pending_row = -2;
            } else {
                // Opponent's move — apply it and give us the turn
                if (msg.col >= 0) {
                    apply_move(msg.col, msg.row);
                } else {
                    apply_pass();
                    flash_       = "OPPONENT PASSED";
                    flash_until_ = SDL_GetTicks() + 3000;
                }
                game_.my_turn  = true;
                pass_confirm_  = false;
                set_status("YOUR TURN");
            }
            // A move was just played: both byo-yomi countdowns start a fresh
            // period, whatever leftover the event's clock snapshot carried.
            reset_byo_countdowns(/*running_player_too=*/true);
            draw();
        }
        break;

    case NetMsgType::CLOCK_UPDATE:
        if (msg.black_secs >= 0) {
            game_.black_secs        = msg.black_secs;
            game_.black_periods     = msg.black_periods;
            game_.black_period_secs = msg.black_period_secs;
            game_.black_in_byo      = msg.black_in_byo;
            game_.clock_tick = SDL_GetTicks();
        }
        if (msg.white_secs >= 0) {
            game_.white_secs        = msg.white_secs;
            game_.white_periods     = msg.white_periods;
            game_.white_period_secs = msg.white_period_secs;
            game_.white_in_byo      = msg.white_in_byo;
        }
        // No move here — trust the running player's countdown (it's genuinely
        // in progress), but the parked player's value is stale leftover.
        reset_byo_countdowns(/*running_player_too=*/false);
        draw();
        break;

    case NetMsgType::STONE_REMOVAL: {
        state_ = AppState::STONE_REMOVAL;
        close_popup_menu();  // a GAME MENU popup is stale once scoring starts
        stone_removal_has_ogs_territory_ = false;  // reset; will be set once OGS sends territory
        game_.history_pos = -1;  // always show live board during stone removal
        // Decode dead stones from all_removed string (pairs of chars: col, row, 'a'=0)
        memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
        memset(game_.ownership,   0, sizeof(game_.ownership));
        const std::string& ar = msg.text;
        for (size_t i = 0; i + 1 < ar.size(); i += 2) {
            int col = ar[i]   - 'a';
            int row = ar[i+1] - 'a';
            if (row >= 0 && row < game_.board_size && col >= 0 && col < game_.board_size)
                game_.dead_stones[row][col] = true;
        }
        // Decode ownership 2D array from OGS — treat as authoritative once received
        if (!msg.ownership_json.empty()) {
            stone_removal_has_ogs_territory_ = true;
            try {
                auto ow = nlohmann::json::parse(msg.ownership_json);
                if (ow.is_array()) {
                    for (int r = 0; r < (int)ow.size() && r < game_.board_size; r++) {
                        auto& row_arr = ow[r];
                        if (!row_arr.is_array()) continue;
                        for (int f = 0; f < (int)row_arr.size() && f < game_.board_size; f++)
                            game_.ownership[r][f] = row_arr[f].get<int>();
                    }
                }
            } catch (...) {}
        }
        // If we already accepted this exact dead-stone set, re-send and keep ACCEPTING status.
        // Only reset if the stone set actually changed (opponent modified the markings).
        if (my_accept_sent_ && msg.text == stone_removal_all_removed_) {
            net_.cmd_accept_stones(game_.game_id);
            accept_resend_at_ = SDL_GetTicks() + 6000;
            set_status("ACCEPTING... WAITING FOR OPPONENT");
        } else {
            my_accept_sent_ = false;
            set_status(stone_removal_has_ogs_territory_
                           ? "PRESS " GLYPH_PS_CROSS " TO ACCEPT DEAD STONES"
                           : "WAITING FOR SERVER SCORE...");
        }
        stone_removal_all_removed_ = msg.text;
        draw();
        break;
    }

    case NetMsgType::GAME_OVER:
        state_ = AppState::GAME_OVER;
        close_popup_menu();  // popup items built for PLAYING/STONE_REMOVAL are stale now
        review_komi_ = 7.5f;  // OGS standard komi; not read from server game data here
        game_.result = msg.text;
        // Game's over — reveal the opponent's rank (hidden during live play)
        black_label_ = game_.black_name +
                       (game_.black_rank.empty() ? "" : " [" + game_.black_rank + "]");
        white_label_ = game_.white_name +
                       (game_.white_rank.empty() ? "" : " [" + game_.white_rank + "]");
        save_live_game();
        // Scan for auto study puzzles now that the game is over (positions the
        // background sweep hasn't reached yet get checked as their results arrive)
        for (int d = 0; d + 1 < (int)game_.history.size(); d++) check_puzzle(d);
        set_status("GAME OVER — " + msg.text);
        kata_suggestion_count_ = 0;
        build_analysis_tree();  // populates analysis_root_ and analysis_cur_
        kata_score_lead_ = cached_analysis_score(analysis_cur_);  // instant if already scored during play
        kata_for(game_.board_size).query_moves(
            analysis_cur_ ? analysis_cur_->board.board : game_.board.board,
            game_.board_size,
            analysis_cur_ ? analysis_cur_->board.turn_is_black == 1
                          : game_.board.turn_is_black == 1);
        draw();
        break;

    case NetMsgType::UNDO_REQUESTED:
        if (state_ == AppState::PLAYING) {
            undo_pending_     = true;
            undo_move_number_ = msg.undo_move_number;
            draw();
        }
        break;

    case NetMsgType::RESUME_PLAY:
        if (state_ == AppState::STONE_REMOVAL || state_ == AppState::PLAYING) {
            stone_removal_has_ogs_territory_ = false;
            my_accept_sent_ = false;
            stone_removal_all_removed_.clear();
            state_ = AppState::PLAYING;
            memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
            memset(game_.ownership,   0, sizeof(game_.ownership));
            game_.history_pos = -1;
            undo_pending_     = false;
            pass_confirm_     = false;
            close_popup_menu();  // a SCORING popup is stale once play resumes
            // If we had a pending pass that the server rejected (phase reverted to play),
            // undo the optimistic turn flip and the history snapshot apply_pass() recorded
            // for it, so my_turn and history depth are computed from the correct state.
            if (game_.pending_col == -1 && game_.pending_row == -1) {
                game_.board.turn_is_black = !game_.board.turn_is_black;
                if (!game_.history.empty()) game_.history.pop_back();
                if (!move_scores_.empty()) {
                    move_scores_.pop_back();
                    move_marked_.pop_back();
                    marked_paths_.pop_back();
                }
            }
            game_.pending_col = -2;
            game_.pending_row = -2;
            bool btp = (game_.board.turn_is_black == 1);
            game_.my_turn = (btp && game_.my_color == 1) || (!btp && game_.my_color == 0);
            set_status(game_.my_turn ? "YOUR TURN" : "WAITING...");
            draw();
        }
        break;

    case NetMsgType::DISCONNECTED:
        // Neither a local game vs KataGo nor GAME_OVER analysis review (regardless of
        // whether the reviewed game was local, online, or loaded from the catalog) has
        // any live dependency on the OGS websocket — KataGo is a local subprocess and
        // review is just walking data already in memory/on disk. Don't yank the player
        // out of either just because the background lobby connection dropped; note it
        // and leave everything undisturbed.
        if (is_local_game_ || state_ == AppState::GAME_OVER) {
            flash_       = "OGS CONNECTION LOST (unaffected)";
            flash_until_ = SDL_GetTicks() + 3000;
            draw();
            break;
        }
        analysis_root_.reset();
        analysis_cur_ = nullptr;
        analysis_tree_render_.clear();
        kata_suggestion_count_ = 0;
        kata_score_lead_ = FLT_MAX;
        kata_query_after_ = 0;
        close_popup_menu();
        pass_confirm_ = false;
        mark_confirm_ = false;
        find_match_confirm_ = false;
        state_ = AppState::CONNECTING;
        set_status("DISCONNECTED: " + msg.text);
        draw();
        break;
    }
}

// ── Drawing ───────────────────────────────────────────────────────────────────

Renderer::DrawState App::make_ds() {
    // Clocks: count down in real-time based on SDL_GetTicks() delta
    int b_secs    = game_.black_secs;
    int w_secs    = game_.white_secs;
    int b_periods = game_.black_periods;
    int w_periods = game_.white_periods;
    // "In byo-yomi" can't be derived from the displayed numbers alone (a period
    // countdown of 0:27 x5 looks identical to 27s of main time with 5 periods
    // banked) — use the flags the network layer determined at parse time, plus
    // the legacy secs<=0 heuristic as a safety net.
    bool b_in_byo = game_.black_in_byo || (game_.black_secs <= 0 && game_.black_periods > 0);
    bool w_in_byo = game_.white_in_byo || (game_.white_secs <= 0 && game_.white_periods > 0);
    // Byo-yomi periods reset the instant a move is played, so the player NOT to
    // move always has a full period waiting. Without this, the clock froze at
    // whatever the server last reported mid-period (e.g. stuck showing 0:24 after
    // using 6s of a 30s period) until it became that player's turn again.
    bool black_to_move = (game_.board.turn_is_black == 1);
    if (b_in_byo && !black_to_move && game_.black_period_secs > 0)
        b_secs = game_.black_period_secs;
    if (w_in_byo && black_to_move && game_.white_period_secs > 0)
        w_secs = game_.white_period_secs;
    if (state_ == AppState::PLAYING && game_.clock_tick > 0) {
        int elapsed = (int)((SDL_GetTicks() - game_.clock_tick) / 1000);
        auto tick = [](int secs, int periods, int period_secs, int elapsed,
                       int& out_secs, int& out_periods, bool& out_in_byo) {
            if (secs > 0 && elapsed < secs) {
                // Still within the period_time_left (or main time) window.
                out_secs    = secs - elapsed;
                out_periods = periods;
            } else if (periods > 0 && period_secs > 0) {
                // Byo-yomi counting. Two entry paths:
                //   a) secs == 0: server sent thinking_time=0 at period start.
                //   b) secs > 0: period_time_left just ran out; the current period
                //      (included in `periods`) is now consumed — use periods-1.
                int byo_elapsed = (secs > 0) ? elapsed - secs : elapsed;
                int byo_periods = (secs > 0) ? periods - 1   : periods;
                int lost        = byo_elapsed / period_secs;
                out_periods     = std::max(0, byo_periods - lost);
                out_secs        = out_periods > 0
                                    ? period_secs - byo_elapsed % period_secs
                                    : 0;
                out_in_byo      = true;
            } else {
                out_secs    = std::max(0, secs - elapsed);
                out_periods = periods;
            }
        };
        if (game_.board.turn_is_black == 1)
            tick(game_.black_secs, game_.black_periods, game_.black_period_secs, elapsed,
                 b_secs, b_periods, b_in_byo);
        else if (game_.board.turn_is_black == 0)
            tick(game_.white_secs, game_.white_periods, game_.white_period_secs, elapsed,
                 w_secs, w_periods, w_in_byo);
    }

    // GAME_OVER is navigated through the analysis tree; PLAYING/STONE_REMOVAL/
    // PUZZLE_PLAY (while exploring) use flat history.
    bool in_history = (state_ == AppState::PLAYING || state_ == AppState::STONE_REMOVAL ||
                        state_ == AppState::PUZZLE_PLAY)
                      && (game_.history_pos >= 0 && !game_.history.empty());

    const char* status_cstr = status_.empty() ? nullptr : status_.c_str();
    if (pass_confirm_)   status_cstr = "PRESS " GLYPH_PS_CIRCLE " AGAIN TO PASS";
    if (undo_pending_)   status_cstr = "UNDO REQUEST: " GLYPH_PS_CROSS "=ACCEPT  " GLYPH_PS_CIRCLE "=DENY";
    if (in_history) {
        hist_status_ = "MOVE " + std::to_string(game_.history_pos) + "/" +
                       std::to_string((int)game_.history.size() - 1);
        status_cstr = hist_status_.c_str();
    }
    if (state_ == AppState::GAME_OVER && analysis_cur_) {
        hist_status_ = "ANALYSIS - MOVE " + std::to_string(analysis_cur_->depth);
        if (analysis_cur_->children.size() > 1)
            hist_status_ += " [FORK]";
        status_cstr = hist_status_.c_str();
    }

    bool playing = (state_ == AppState::PLAYING || state_ == AppState::STONE_REMOVAL ||
                    state_ == AppState::GAME_OVER || state_ == AppState::PUZZLE_PLAY ||
                    state_ == AppState::JOSEKI);
    bool live    = (state_ != AppState::CREDENTIAL_PROMPT);

    // When reviewing history, display the historical board state;
    // in GAME_OVER display the current analysis node;
    // otherwise in idle states show the demo screensaver game if loaded.
    bool show_demo = demo_active_ && !playing && state_ != AppState::CREDENTIAL_PROMPT;
    const GameState& disp_board =
        (state_ == AppState::GAME_OVER && analysis_cur_) ? analysis_cur_->board
        : in_history                                      ? game_.history[game_.history_pos]
        : show_demo                                       ? demo_.board
        :                                                   game_.board;

    // References bound here must outlive this call — all are App member variables.
    const std::string& bname = playing    ? black_label_
                             : show_demo  ? demo_.black_name
                             :              empty_str_;
    const std::string& wname = playing    ? white_label_
                             : show_demo  ? demo_.white_name
                             :              empty_str_;

    int active_bs = playing   ? game_.board_size
                  : show_demo ? demo_.board_size
                  :             BOARD_SIZE;

    return Renderer::DrawState{
        .game                   = disp_board,
        .analysis               = nullptr,
        .analysis_mode          = false,
        .game_mode              = false,
        .guess_mode             = false,
        .guess_score            = 0,
        .chain_mode             = chain_mode_,
        .free_mode              = false,
        .active_board_size      = active_bs,
        .show_help              = show_help_,
        .catalog                = catalog_,
        .catalog_readonly       = catalog_readonly_,
        .black_name             = bname,
        .white_name             = wname,
        .result_message         = (state_ == AppState::GAME_OVER) ? game_.result : empty_str_,
        .game_date              = empty_str_,
        .game_comment           = (state_ == AppState::PUZZLE_PLAY) ? pz_comment_
                                : (state_ == AppState::JOSEKI)      ? jk_comment_ : empty_str_,
        .move_delay_ms          = MOVE_DELAY_MS,
        .speed_message_until    = 0,
        .suppress_present       = false,
        .territory_drill        = false,
        .territory_board        = nullptr,
        .territory_b_score      = 0,
        .territory_w_score      = 0,
        .territory_answered     = false,
        .territory_correct      = false,
        .stone_filter           = 0,
        .cursor_x               = -1,
        .cursor_y               = -1,
        .cursor_type            = 0,
        .show_move_numbers      = false,
        .sgf_moves              = nullptr,
        .sgf_colors             = nullptr,
        .sgf_game_index         = 0,
        .analysis_num_grid      = nullptr,
        .analysis_col_grid      = nullptr,
        .quit_confirm           = quit_confirm_,
        .box_sel_pts            = nullptr,
        .box_sel_count          = 0,
        .box_drag_active        = false,
        .box_drag_r1            = 0,
        .box_drag_f1            = 0,
        .box_drag_r2            = 0,
        .box_drag_f2            = 0,
        .catalog_thumb_valid    = thumb_valid_,
        .catalog_thumb_single   = thumb_single_,
        .catalog_thumb_open     = thumb_valid_ ? thumb_open_  : nullptr,
        .catalog_thumb_final    = thumb_valid_ ? thumb_final_ : nullptr,
        .catalog_thumb_board_size = thumb_board_size_,
        .flash_message          = flash_,
        .flash_message_until    = flash_until_,
        .save_input_step        = 0,
        .save_input_buf         = empty_str_,

        // Live fields
        .live_mode       = live,
        .live_cursor_r   = (state_ == AppState::PLAYING && !in_history) ? game_.cursor_r :
                            (state_ == AppState::GAME_OVER)              ? game_.cursor_r :
                            (state_ == AppState::PUZZLE_PLAY)            ? game_.cursor_r :
                            (state_ == AppState::JOSEKI)                 ? game_.cursor_r : -1,
        .live_cursor_f   = (state_ == AppState::PLAYING && !in_history) ? game_.cursor_f :
                            (state_ == AppState::GAME_OVER)              ? game_.cursor_f :
                            (state_ == AppState::PUZZLE_PLAY)            ? game_.cursor_f :
                            (state_ == AppState::JOSEKI)                 ? game_.cursor_f : -1,
        .live_my_color   = game_.my_color,
        .live_my_turn    = game_.my_turn,
        .live_black_secs        = playing ? b_secs : -1,
        .live_white_secs        = playing ? w_secs : -1,
        .live_black_periods     = playing ? b_periods : -1,
        .live_white_periods     = playing ? w_periods : -1,
        .live_black_period_secs = playing ? game_.black_period_secs : -1,
        .live_white_period_secs = playing ? game_.white_period_secs : -1,
        .live_black_in_byo      = playing && b_in_byo,
        .live_white_in_byo      = playing && w_in_byo,
        .live_status     = live ? status_cstr : nullptr,
        .live_dead_stones     = (state_ == AppState::STONE_REMOVAL) ? game_.dead_stones : nullptr,
        .live_ownership       = (state_ == AppState::STONE_REMOVAL) ? game_.ownership   : nullptr,
        .live_suggestions     = (state_ == AppState::GAME_OVER && kata_suggestion_count_ > 0)
                                     ? kata_suggestions_ : nullptr,
        .live_suggestion_count = (state_ == AppState::GAME_OVER) ? kata_suggestion_count_ : 0,
        .live_hovered_suggestion = [&]() -> int {
            if (state_ != AppState::GAME_OVER) return -1;
            for (int i = 0; i < std::min(kata_suggestion_count_, 3); i++)
                if (kata_suggestions_[i].row == game_.cursor_r &&
                    kata_suggestions_[i].col == game_.cursor_f) return i;
            return -1;
        }(),
        .live_cursor_ko       = (ko_flash_until_ > SDL_GetTicks()),
        .live_kata_score_lead    = (state_ == AppState::GAME_OVER) ? kata_score_lead_ : FLT_MAX,
        .live_actual_move_r = [&]() -> int {
            if (state_ != AppState::GAME_OVER || !analysis_cur_ || analysis_cur_->children.empty())
                return -1;
            return analysis_cur_->children[0]->move_row;
        }(),
        .live_actual_move_f = [&]() -> int {
            if (state_ != AppState::GAME_OVER || !analysis_cur_ || analysis_cur_->children.empty())
                return -1;
            return analysis_cur_->children[0]->move_col;
        }(),
        .live_actual_move_score = [&]() -> float {
            if (state_ != AppState::GAME_OVER || !analysis_cur_ || analysis_cur_->children.empty())
                return FLT_MAX;
            int ar = analysis_cur_->children[0]->move_row;
            int af = analysis_cur_->children[0]->move_col;
            if (ar < 0 || af < 0) return FLT_MAX;
            for (int i = 0; i < kata_suggestion_count_; i++)
                if (kata_suggestions_[i].row == ar && kata_suggestions_[i].col == af)
                    return kata_suggestions_[i].score_lead;
            return FLT_MAX;
        }(),
        .live_analysis_tree       = (state_ == AppState::GAME_OVER && !analysis_tree_render_.empty())
                                         ? analysis_tree_render_.data()
                                  : (state_ == AppState::PUZZLE_PLAY && !pz_tree_render_.empty())
                                         ? pz_tree_render_.data() : nullptr,
        .live_analysis_tree_count = (state_ == AppState::GAME_OVER)   ? (int)analysis_tree_render_.size()
                                  : (state_ == AppState::PUZZLE_PLAY) ? (int)pz_tree_render_.size() : 0,
        .live_analysis_tree_cur_depth = (state_ == AppState::PUZZLE_PLAY) ? pz_cur_depth_
                                      : analysis_cur_ ? analysis_cur_->depth : 0,

        // Score graph
        .live_score_graph     = (state_ == AppState::GAME_OVER && !move_scores_.empty())
                                     ? move_scores_.data() : nullptr,
        .live_score_graph_len = (state_ == AppState::GAME_OVER) ? (int)move_scores_.size() : 0,
        .live_score_graph_cur = analysis_cur_ ? analysis_cur_->depth : 0,
        // Last-played-stone indicator: only shown while RT is held (previously always-on
        // row/column lines were too distracting; now an opt-in teal circle on demand).
        .live_last_move_r = [&]() -> int {
            if (!rt_down_) return -1;
            if (state_ == AppState::GAME_OVER && analysis_cur_ && analysis_cur_->parent)
                return analysis_cur_->move_row;
            if ((state_ == AppState::PLAYING || state_ == AppState::STONE_REMOVAL)
                && game_.history_pos < 0)
                return last_move_r_;
            return -1;
        }(),
        .live_last_move_f = [&]() -> int {
            if (!rt_down_) return -1;
            if (state_ == AppState::GAME_OVER && analysis_cur_ && analysis_cur_->parent)
                return analysis_cur_->move_col;
            if ((state_ == AppState::PLAYING || state_ == AppState::STONE_REMOVAL)
                && game_.history_pos < 0)
                return last_move_f_;
            return -1;
        }(),
        .live_show_coords = show_coords_,
        .live_labels      = (state_ == AppState::GAME_OVER && analysis_cur_ &&
                             !analysis_cur_->labels.empty())
                                ? analysis_cur_->labels.data()
                          : (state_ == AppState::PUZZLE_PLAY && !pz_marks_.empty())
                                ? pz_marks_.data() : nullptr,
        .live_label_count = (state_ == AppState::GAME_OVER && analysis_cur_)
                                ? (int)analysis_cur_->labels.size()
                          : (state_ == AppState::PUZZLE_PLAY)
                                ? (int)pz_marks_.size() : 0,
        .live_result_banner = (state_ == AppState::STONE_REMOVAL && is_local_game_ &&
                               !local_game_score_.empty())
                                  ? local_game_score_.c_str()
                            : (state_ == AppState::PUZZLE_PLAY && pz_done_ &&
                               !pz_banner_.empty())
                                  ? pz_banner_.c_str() : nullptr,
        .square_stones      = square_stones_,
        .square_grid        = square_grid_,
        // JOSEKI reuses the puzzle layout: comment box in the right gutter,
        // player labels and clocks suppressed
        .puzzle_mode        = (state_ == AppState::PUZZLE_PLAY ||
                               state_ == AppState::JOSEKI),
        // START popup menu (PUZZLE_BROWSE draws its own copy over the list screen)
        .popup_items        = popup_active_ ? popup_labels_.data() : nullptr,
        .popup_count        = popup_active_ ? (int)popup_labels_.size() : 0,
        .popup_index        = popup_index_,
        .popup_title        = popup_active_ ? popup_title_.c_str() : nullptr,
        // Joseki continuation dots
        .live_markers       = (state_ == AppState::JOSEKI && !jk_markers_.empty())
                                  ? jk_markers_.data() : nullptr,
        .live_marker_count  = (state_ == AppState::JOSEKI) ? (int)jk_markers_.size() : 0,
    };
}

void App::draw() {
    if (state_ == AppState::MATCH_MENU) {
        renderer_->draw_match_menu(match_menu_);
        return;
    }
    if (state_ == AppState::PUZZLE_BROWSE) {
        draw_puzzle_browser();
        return;
    }
    if (catalog_.active) {
        // Parse names for the entire listing, not just the visible window. The first
        // frame after entering a directory pays one pass of 4KB header reads; every
        // frame after that is a no-op flag scan (ensure_names_loaded skips entries
        // already loaded). Covers subdirectory navigation, which doesn't go through
        // open_game_catalog()'s own full-parse call.
        if (!catalog_.search_mode && !catalog_.virtual_year_mode && !catalog_.virtual_player_mode)
            catalog_.ensure_names_loaded(0, (int)catalog_.entries.size());
        // Refresh the [BY YEAR] virtual list once the background index finishes —
        // never called anywhere else in this file, so without it that screen was
        // stuck on "Building index..." forever if opened before indexing completed.
        catalog_.tick();
    }
    auto ds = make_ds();
    renderer_->draw_board(ds);
}

// ── Credential prompt ─────────────────────────────────────────────────────────

// Returns true when credentials are complete and accepted.
static bool handle_cred_key(SDL_Keycode key, int& step,
                             std::string& buf,
                             std::string& username,
                             std::string& password)
{
    if (step == 0) return false;
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (step == 1) { username = buf; buf.clear(); step = 2; }
        else           { password = buf; buf.clear(); step = 0; return true; }
        return false;
    }
    if (key == SDLK_BACKSPACE) {
        if (!buf.empty()) buf.pop_back();
        return false;
    }
    // Printable ASCII
    if (key >= SDLK_SPACE && key <= SDLK_z && key < 127) {
        buf += (char)key;
    }
    return false;
}

// ── Main event loop ───────────────────────────────────────────────────────────

void App::event_loop() {
    bool quit = false;

    // D-pad + trigger key-repeat state
    static const Uint8 DPAD_BTNS[] = {
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        0xFD, 0xFE  // virtual: LT (history back), RT (history forward)
    };
    Uint8  repeat_btn      = 0xFF;   // 0xFF = none held
    Uint32 repeat_next_ms  = 0;
    const int REPEAT_DELAY = 400;    // ms before first repeat
    const int REPEAT_RATE  = 80;     // ms between subsequent repeats
    // Joystick cursor repeat — snappier than the dpad's, since the whole point of
    // using the stick (per Boris) is moving the cursor fast.
    const int JS_REPEAT_DELAY = 130;
    const int JS_REPEAT_RATE  = 30;
    bool lt_down = false;  // trigger axis state (rt_down_ is a member — read by make_draw_state)
    int    js_dir_x = 0, js_dir_y = 0;      // last committed joystick cursor direction
    Uint32 js_move_next_ms = 0;             // next time that direction is allowed to step again

    while (!quit) {
        Uint32 now = SDL_GetTicks();

        // Recompute wait time: wake every second to tick the clock display
        int wait_ms = 1000;
        if (flash_until_ > now)
            wait_ms = std::min(wait_ms, (int)(flash_until_ - now));
        if (ko_flash_until_ > now)
            wait_ms = std::min(wait_ms, (int)(ko_flash_until_ - now));
        if (kata_query_after_ > 0 && kata_query_after_ > now)
            wait_ms = std::min(wait_ms, (int)(kata_query_after_ - now));
        if (pz_pending_reply_ && pz_reply_at_ > now)
            wait_ms = std::min(wait_ms, (int)(pz_reply_at_ - now));
        if (repeat_btn != 0xFF && repeat_next_ms > now)
            wait_ms = std::min(wait_ms, (int)(repeat_next_ms - now));
        if (demo_active_ && demo_.next_tick > now)
            wait_ms = std::min(wait_ms, (int)(demo_.next_tick - now));
        // Wake up in time for the joystick cursor's next repeat step (same states
        // as the js_cursor_ok gate below)
        if ((state_ == AppState::PLAYING || state_ == AppState::GAME_OVER ||
             state_ == AppState::PUZZLE_PLAY || state_ == AppState::JOSEKI) &&
            (js_dir_x != 0 || js_dir_y != 0) && js_move_next_ms > now)
            wait_ms = std::min(wait_ms, (int)(js_move_next_ms - now));

        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, wait_ms)) {
            do {
                if (e.type == SDL_QUIT) {
                    quit = true;

                } else if (e.type == g_net_event_type) {
                    NetMsg msg;
                    while (net_.poll_msg(msg))
                        handle_net_msg(msg);
                    // OGS puzzle fetch results (worker threads wake us with this event)
                    poll_puzzle_fetch();
                    // OJE joseki node fetches, same worker/wake pattern
                    poll_joseki_fetch();
                    // KataGo GTP move (local game)
                    if (is_local_game_ && state_ == AppState::PLAYING) {
                        int gtp_row, gtp_col;
                        if (kata_gtp_.poll_genmove(gtp_row, gtp_col))
                            handle_katago_gtp_move(gtp_row, gtp_col);
                    }
                    // GTP final_score + final_status_list dead (local stone removal)
                    if (is_local_game_ && state_ == AppState::STONE_REMOVAL) {
                        std::string fscore;
                        int dead_rows[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
                        int dead_cols[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
                        int dead_count = 0;
                        if (kata_gtp_.poll_final_status(fscore, dead_rows, dead_cols, dead_count)) {
                            memset(game_.dead_stones, 0, sizeof(game_.dead_stones));
                            for (int i = 0; i < dead_count; i++)
                                game_.dead_stones[dead_rows[i]][dead_cols[i]] = true;
                            local_game_score_ = fscore;
                            // Now query territory with dead stones excluded so that
                            // the colored squares inside dead groups are correct.
                            if (kata_for(game_.board_size).running())
                                kata_for(game_.board_size).query_ownership(
                                    game_.board.board, game_.board_size,
                                    game_.dead_stones, 7.5f, 100);
                            // Status updates to "PRESS A" after ownership arrives (below).
                            draw();
                        }
                    }
                    // KataGo analysis results — poll both processes
                    for (KatagoProc* kp : { &kata_, &kata_9_ }) {
                        // Territory result (stone removal phase) — ownership fills color squares
                        if (state_ == AppState::STONE_REMOVAL) {
                            int kata_own[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
                            int bs = 0;
                            if (kp->poll_ownership(kata_own, bs) && !stone_removal_has_ogs_territory_) {
                                for (int r = 0; r < bs; r++)
                                    for (int f = 0; f < bs; f++)
                                        game_.ownership[r][f] = kata_own[r][f];
                                // Territory is now ready; show the score + prompt
                                // Score itself is shown as the big banner (make_ds
                                // passes local_game_score_ as live_result_banner)
                                if (is_local_game_ && !local_game_score_.empty())
                                    set_status("PRESS " GLYPH_PS_CROSS " FOR ANALYSIS");
                                draw();
                            }
                        }
                        // Move suggestions / background scoring
                        {
                            int count = 0;
                            float sl = FLT_MAX;
                            if (kp->poll_moves(kata_suggestions_, count, sl)) {
                                if (bg_analysis_busy_) {
                                    // Background scoring result — store in the graph array
                                    if (sl != FLT_MAX && bg_analysis_depth_ >= 0 &&
                                        bg_analysis_depth_ < (int)move_scores_.size())
                                        move_scores_[bg_analysis_depth_] = sl;
                                    // Keep the top-2 suggestions for study-puzzle detection
                                    if (count > 0 && bg_analysis_depth_ >= 0) {
                                        PuzzleEval pe;
                                        pe.best_sl = kata_suggestions_[0].score_lead;
                                        pe.best_r  = kata_suggestions_[0].row;
                                        pe.best_f  = kata_suggestions_[0].col;
                                        if (count > 1) pe.second_sl = kata_suggestions_[1].score_lead;
                                        puzzle_eval_[bg_analysis_depth_] = pe;
                                        // Evaluate only after the game — a fresh score at depth D
                                        // can also complete the check for D-1 (needs score[D]).
                                        if (state_ == AppState::GAME_OVER) {
                                            check_puzzle(bg_analysis_depth_);
                                            check_puzzle(bg_analysis_depth_ - 1);
                                        }
                                    }
                                    bg_analysis_busy_ = false;
                                    if (state_ == AppState::GAME_OVER) draw();
                                } else {
                                    fg_kata_pending_ = false;
                                    kata_score_lead_ = sl;
                                    // Persist score on the node always; only feed the
                                    // depth-indexed graph array when this node is genuinely
                                    // on the main line — a branch node at the same depth as
                                    // a main-line move is a different position and would
                                    // otherwise clobber that move's real graph value.
                                    if (analysis_cur_ && sl != FLT_MAX) {
                                        analysis_cur_->score_lead = sl;
                                        int d = analysis_cur_->depth;
                                        if (analysis_cur_->is_main_line && d < (int)move_scores_.size())
                                            move_scores_[d] = sl;
                                    }
                                    // Filter out illegal suggestions (ko, suicide, occupied)
                                    kata_suggestion_count_ = 0;
                                    for (int i = 0; i < count; i++) {
                                        int sr = kata_suggestions_[i].row, sf = kata_suggestions_[i].col;
                                        if (sr < 0 || sf < 0 ||
                                            is_legal_analysis_move(sf, sr))
                                            kata_suggestions_[kata_suggestion_count_++] = kata_suggestions_[i];
                                    }
                                    if (state_ == AppState::GAME_OVER) draw();
                                }
                            }
                        }
                    }

                } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                    Uint8 btn = e.cbutton.button;
                    handle_controller_button(btn);
                    // Start key-repeat tracking for dpad buttons
                    for (Uint8 db : DPAD_BTNS) {
                        if (btn == db) {
                            repeat_btn     = btn;
                            repeat_next_ms = SDL_GetTicks() + REPEAT_DELAY;
                            break;
                        }
                    }

                } else if (e.type == SDL_CONTROLLERBUTTONUP) {
                    if (e.cbutton.button == repeat_btn)
                        repeat_btn = 0xFF;

                } else if (e.type == SDL_CONTROLLERAXISMOTION) {
                    // Treat LT/RT triggers as virtual buttons 0xFD/0xFE with key-repeat
                    auto on_trigger = [&](Uint8 vbtn, bool& down, Sint16 val) {
                        bool pressed = (val >= 8192);
                        if (pressed == down) return;
                        down = pressed;
                        if (pressed) {
                            handle_controller_button(vbtn);
                            repeat_btn     = vbtn;
                            repeat_next_ms = SDL_GetTicks() + REPEAT_DELAY;
                        } else {
                            if (repeat_btn == vbtn) repeat_btn = 0xFF;
                            draw();  // e.g. RT release should hide the last-move indicator promptly
                        }
                    };
                    if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
                        on_trigger(0xFD, lt_down, e.caxis.value);
                    else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
                        on_trigger(0xFE, rt_down_, e.caxis.value);
                    else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
                        js_left_x_ = e.caxis.value;
                    else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
                        js_left_y_ = e.caxis.value;

                } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                    if (!pad_)
                        pad_ = SDL_GameControllerOpen(e.cdevice.which);

                } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                    if (pad_ && SDL_GameControllerGetJoystick(pad_) ==
                            SDL_JoystickFromInstanceID(e.cdevice.which)) {
                        SDL_GameControllerClose(pad_);
                        pad_ = nullptr;
                    }

                } else if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode k = e.key.keysym.sym;

                    if (state_ == AppState::CREDENTIAL_PROMPT) {
                        if (handle_cred_key(k, cred_step_, cred_buf_,
                                            cred_username_, cred_password_)) {
                            // Both credentials entered — start connecting
                            state_ = AppState::CONNECTING;
                            set_status("CONNECTING...");
                            net_.start(cred_username_, cred_password_);
                        }
                        draw();
                    } else if (drill_rename_active_ && state_ == AppState::PUZZLE_BROWSE) {
                        // Typing a new drill name — capture everything (same
                        // plain-keycode entry as the credential prompt)
                        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                            drill_commit_rename();
                        } else if (k == SDLK_ESCAPE) {
                            drill_rename_active_ = false;
                            drill_rename_buf_.clear();
                            draw();
                        } else if (k == SDLK_BACKSPACE) {
                            if (!drill_rename_buf_.empty()) drill_rename_buf_.pop_back();
                            draw();
                        } else if (k >= SDLK_SPACE && k <= SDLK_z && k < 127) {
                            drill_rename_buf_ += (char)toupper((int)k);
                            draw();
                        }
                    } else if (k == SDLK_q) {
                        if (quit_confirm_) { quit = true; }
                        else { quit_confirm_ = true; draw(); }
                    } else if (k == SDLK_ESCAPE) {
                        if (quit_confirm_) { quit_confirm_ = false; draw(); }
                        else { show_help_ = !show_help_; draw(); }
                    }
                    // Any other key cancels quit confirm without acting
                    else if (quit_confirm_) {
                        quit_confirm_ = false;
                        draw();
                    }
                    // Keyboard shortcut mirrors for controller buttons — a full
                    // substitute for when the pad is unplugged or out of battery.
                    else {
                        Uint8 mapped = 0xFF;
                        switch (k) {
                        case SDLK_RETURN: case SDLK_SPACE:
                                              mapped = SDL_CONTROLLER_BUTTON_A;             break;
                        case SDLK_b: case SDLK_p:
                                              mapped = SDL_CONTROLLER_BUTTON_B;             break;
                        case SDLK_c:          mapped = SDL_CONTROLLER_BUTTON_X;             break;
                        case SDLK_m:          mapped = SDL_CONTROLLER_BUTTON_Y;             break;
                        case SDLK_s:          mapped = SDL_CONTROLLER_BUTTON_BACK;          break;
                        case SDLK_r: case SDLK_f:
                                              mapped = SDL_CONTROLLER_BUTTON_START;         break;
                        case SDLK_UP:         mapped = SDL_CONTROLLER_BUTTON_DPAD_UP;       break;
                        case SDLK_DOWN:       mapped = SDL_CONTROLLER_BUTTON_DPAD_DOWN;     break;
                        case SDLK_LEFT:       mapped = SDL_CONTROLLER_BUTTON_DPAD_LEFT;     break;
                        case SDLK_RIGHT:      mapped = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;    break;
                        case SDLK_TAB: case SDLK_LEFTBRACKET:
                                              mapped = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;  break;
                        case SDLK_RIGHTBRACKET:
                                              mapped = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
                        case SDLK_COMMA:      mapped = 0xFD; break;  // LT — step back
                        case SDLK_PERIOD:     mapped = 0xFE; break;  // RT — step forward
                        case SDLK_PAGEUP:     mapped = SDL_CONTROLLER_BUTTON_LEFTSTICK;     break;
                        case SDLK_PAGEDOWN:   mapped = SDL_CONTROLLER_BUTTON_RIGHTSTICK;    break;
                        default: break;
                        }
                        if (mapped != 0xFF) handle_controller_button(mapped);
                    }

                } else if (e.type == SDL_MOUSEMOTION) {
                    // Snap the board cursor to the hovered grid point (mouse and pad
                    // coexist — the cursor just follows whichever moved last)
                    if (!catalog_.active && state_ != AppState::MATCH_MENU &&
                        ((state_ == AppState::PLAYING && game_.history_pos < 0) ||
                         state_ == AppState::GAME_OVER ||
                         state_ == AppState::PUZZLE_PLAY)) {
                        BoardView view;
                        renderer_->get_board_view(view, game_.board_size);
                        int mr, mf;
                        if (renderer_->screen_to_board(view, e.motion.x, e.motion.y, mr, mf) &&
                            (mr != game_.cursor_r || mf != game_.cursor_f)) {
                            game_.cursor_r = mr;
                            game_.cursor_f = mf;
                            draw();
                        }
                    }

                } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        bool board_state = !catalog_.active && state_ != AppState::MATCH_MENU &&
                            (state_ == AppState::PLAYING || state_ == AppState::GAME_OVER ||
                             state_ == AppState::PUZZLE_PLAY);
                        if (board_state) {
                            // Only place when the click actually lands on a grid point —
                            // clicking panels/dead space must not drop a stone at the
                            // last cursor position. (STONE_REMOVAL deliberately excluded:
                            // accepting a score shouldn't happen from a stray click.)
                            BoardView view;
                            renderer_->get_board_view(view, game_.board_size);
                            int mr, mf;
                            if (renderer_->screen_to_board(view, e.button.x, e.button.y, mr, mf)) {
                                game_.cursor_r = mr;
                                game_.cursor_f = mf;
                                handle_controller_button(SDL_CONTROLLER_BUTTON_A);
                            }
                        } else if (catalog_.active || state_ == AppState::MATCH_MENU ||
                                   state_ == AppState::PUZZLE_BROWSE) {
                            handle_controller_button(SDL_CONTROLLER_BUTTON_A);
                        }
                    } else if (e.button.button == SDL_BUTTON_RIGHT) {
                        handle_controller_button(SDL_CONTROLLER_BUTTON_B);
                    }

                } else if (e.type == SDL_MOUSEWHEEL) {
                    // Wheel: scroll lists in the catalog/menu/browser, step moves on a board
                    bool list_ctx = catalog_.active || state_ == AppState::MATCH_MENU ||
                                    state_ == AppState::PUZZLE_BROWSE;
                    Uint8 up_btn   = list_ctx ? (Uint8)SDL_CONTROLLER_BUTTON_DPAD_UP   : (Uint8)0xFD;
                    Uint8 down_btn = list_ctx ? (Uint8)SDL_CONTROLLER_BUTTON_DPAD_DOWN : (Uint8)0xFE;
                    for (int s = e.wheel.y; s > 0; s--) handle_controller_button(up_btn);
                    for (int s = e.wheel.y; s < 0; s++) handle_controller_button(down_btn);

                } else if (e.type == SDL_WINDOWEVENT) {
                    if (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                        e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                        draw();
                }

            } while (!quit && SDL_PollEvent(&e));
        }

        // Fire key-repeat for held dpad button
        now = SDL_GetTicks();
        if (repeat_btn != 0xFF && now >= repeat_next_ms) {
            handle_controller_button(repeat_btn);
            repeat_next_ms = now + REPEAT_RATE;
        }

        // Left-stick joystick cursor — live play (at the live edge) and GAME_OVER
        // analysis (placing branch stones / labels), but not while the catalog or
        // the result freeze-screen is up.
        // Snap the stick to one of 8 directions (45-degree sectors) so a tilt
        // near a diagonal commits firmly to that diagonal instead of racing
        // between axes, then step the cursor at a fixed repeat rate (same
        // delay/rate as the dpad buttons) instead of accelerating continuously.
        bool js_cursor_ok =
            !catalog_.active && !popup_active_ &&
            ((state_ == AppState::PLAYING && game_.history_pos < 0) ||
             state_ == AppState::GAME_OVER ||
             state_ == AppState::PUZZLE_PLAY ||
             state_ == AppState::JOSEKI);
        if (js_cursor_ok) {
            const float DEAD    = 8192.f;
            const float TAN22_5 = 0.41421356f;  // tan(22.5 deg)
            float x = (float)js_left_x_, y = (float)js_left_y_;
            float ax = std::fabs(x), ay = std::fabs(y);
            int dx = 0, dy = 0;
            if (std::sqrt(x * x + y * y) > DEAD) {
                dx = (x > 0.f) ? 1 : (x < 0.f ? -1 : 0);
                dy = (y > 0.f) ? 1 : (y < 0.f ? -1 : 0);
                if (ax > 0.f && ay / ax < TAN22_5) dy = 0;       // within 22.5 deg of horizontal
                else if (ay > 0.f && ax / ay < TAN22_5) dx = 0;  // within 22.5 deg of vertical
            }
            if (dx != 0 || dy != 0) {
                bool changed = (dx != js_dir_x || dy != js_dir_y);
                if (changed || now >= js_move_next_ms) {
                    int n = game_.board_size - 1;
                    game_.cursor_f = std::max(0, std::min(n, game_.cursor_f + dx));
                    game_.cursor_r = std::max(0, std::min(n, game_.cursor_r + dy));
                    js_move_next_ms = now + (changed ? JS_REPEAT_DELAY : JS_REPEAT_RATE);
                }
            }
            js_dir_x = dx;
            js_dir_y = dy;
        } else {
            js_dir_x = js_dir_y = 0;
        }

        // Advance demo screensaver when idle (not in a live game)
        bool idle = (state_ == AppState::LOBBY || state_ == AppState::SEARCHING ||
                     state_ == AppState::CONNECTING);
        if (demo_active_ && idle && now >= demo_.next_tick) {
            if (demo_.pos < (int)demo_.rows.size()) {
                demo_.board.place_stone(demo_.rows[demo_.pos], demo_.cols[demo_.pos],
                                        demo_.colors[demo_.pos]);
                demo_.pos++;
                demo_.next_tick = now + 1000;
            } else if (demo_playlist_pos_ >= 0) {
                // Scoped autoplay (started from the catalog): advance through the
                // remembered list in order; after one full lap, fall back to the
                // default random screensaver rather than looping forever.
                demo_playlist_pos_++;
                if (demo_playlist_pos_ < (int)demo_playlist_.size()) {
                    load_demo_from_path(demo_playlist_[demo_playlist_pos_]);
                } else {
                    demo_playlist_.clear();
                    demo_playlist_pos_ = -1;
                    load_demo_game();
                }
            } else {
                load_demo_game();  // finished — pick a new random game
            }
        }

        // Stone removal: our accept may not have reached the server (seen in practice —
        // no protocol error, the send just doesn't always take effect first try). Don't
        // wait solely on the server's own removed_stones re-broadcast (can take 20-30s);
        // proactively resend a few seconds after our own accept if nothing's changed yet.
        if (state_ == AppState::STONE_REMOVAL && my_accept_sent_ &&
            now >= accept_resend_at_) {
            net_.cmd_accept_stones(game_.game_id);
            accept_resend_at_ = now + 6000;
        }

        // Fire deferred KataGo query once the user has settled on a position
        if (kata_query_after_ > 0 && now >= kata_query_after_) {
            kata_query_after_ = 0;
            if (state_ == AppState::GAME_OVER && analysis_cur_ && kata_analysis_enabled_) {
                bg_analysis_busy_ = false;  // cancel any in-flight background query
                fg_kata_pending_  = true;
                kata_for(game_.board_size).query_moves(
                    analysis_cur_->board.board, game_.board_size,
                    analysis_cur_->board.turn_is_black == 1, review_komi_);
            }
        }

        // Fire the puzzle opponent's delayed reply once it's had its moment
        if (pz_pending_reply_ && now >= pz_reply_at_ && state_ == AppState::PUZZLE_PLAY) {
            pz_fire_pending_reply();
            draw();
        }

        // If a background query's response never arrives (lost/corrupted on the way
        // back from KataGo — has happened; see katago.cpp's stderr-pipe fix), bg_analysis_busy_
        // would otherwise stay stuck true forever, silently blocking every subsequent
        // depth for the rest of the session. Give up on it after a generous timeout so
        // the sweep can move on — that one depth just stays unscored instead of
        // everything after it.
        if (bg_analysis_busy_ && now - bg_analysis_started_at_ > 15000) {
            bg_analysis_busy_ = false;
        }

        // Background scoring — fill move_scores_[] with low-visit KataGo queries.
        // Runs during PLAYING (so the graph is mostly ready at game-over) and during
        // GAME_OVER (fills in positions the user hasn't explicitly navigated to yet).
        // Guards: no foreground query in-flight, no deferred query pending.
        if ((state_ == AppState::PLAYING || state_ == AppState::GAME_OVER) &&
            !bg_analysis_busy_ && !fg_kata_pending_ && kata_query_after_ == 0 &&
            kata_for(game_.board_size).running()) {
            // Skip depths that already have a score
            while (bg_analysis_next_ < (int)move_scores_.size() &&
                   move_scores_[bg_analysis_next_] != FLT_MAX)
                bg_analysis_next_++;
            if (bg_analysis_next_ < (int)move_scores_.size() &&
                bg_analysis_next_ < (int)game_.history.size()) {
                const GameState& hs = game_.history[bg_analysis_next_];
                kata_for(game_.board_size).query_moves(
                    hs.board, game_.board_size, hs.turn_is_black == 1, review_komi_, 50);
                bg_analysis_depth_      = bg_analysis_next_++;
                bg_analysis_busy_       = true;
                bg_analysis_started_at_ = now;
            }
        }

        // Tick clock display every second even with no events
        if (!quit) draw();
    }
}

// ── Entry point ───────────────────────────────────────────────────────────────

int App::run() {
    if (!init()) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Failed to initialise SDL:\n%s", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OGS Client", buf, nullptr);
        return 1;
    }

    g_net_event_type = SDL_RegisterEvents(1);
    if (g_net_event_type == (Uint32)-1) {
        cleanup();
        return 1;
    }

    std::string username, password, jwt;
    std::string kata_exe, kata_model, kata_cfg, kata_model_9x9, kata_human_model;
    load_config(username, password, jwt, kata_exe, kata_model, kata_cfg,
                kata_model_9x9, kata_human_model);
    my_username_       = username;
    kata_exe_          = kata_exe;
    kata_model_        = kata_model;
    kata_human_model_  = kata_human_model;
    load_adaptive();        // restore the adaptive KataGo strength from previous sessions
    load_solved_puzzles();     // restore the solved-puzzle checklist
    load_known_collections();  // restore metadata for the pinned MY SETS section
    load_settings();           // restore match/display settings from previous sessions
    if (!kata_exe.empty() && !kata_model.empty() && !kata_cfg.empty())
        kata_.start(kata_exe, kata_model, kata_cfg);
    if (!kata_exe.empty() && !kata_model_9x9.empty() && !kata_cfg.empty())
        kata_9_.start(kata_exe, kata_model_9x9, kata_cfg);
    if (!username.empty()) {
        set_status("CONNECTING...");
        draw();
        net_.start(username, password, jwt);
    } else {
        state_     = AppState::CREDENTIAL_PROMPT;
        cred_step_ = 1;
        set_status("ENTER OGS USERNAME (ENTER to confirm, then password)");
        draw();
    }

    event_loop();
    cleanup();
    return 0;
}

int main(int /*argc*/, char* /*argv*/[]) {
    setvbuf(stderr, nullptr, _IONBF, 0);  // unbuffered even when piped
#ifdef _WIN32
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        fprintf(stderr, "[CRASH] SEH 0x%08lX at %p thread=%lu\n",
                (unsigned long)ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress,
                (unsigned long)GetCurrentThreadId());
        fflush(stderr);
        // stderr alone is lost when the app is launched without an attached console
        // (e.g. double-clicked) — persist the same line to disk so a crash leaves a
        // forensic trail regardless of how it was started. Opened/closed immediately
        // (rather than an fd kept open for the app's lifetime) since this only ever
        // fires once, right before the process goes down.
        time_t t = time(nullptr);
        struct tm* tm_info = localtime(&t);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
        FILE* cf = fopen("crash.log", "a");
        if (cf) {
            fprintf(cf, "[%s] SEH 0x%08lX at %p thread=%lu\n",
                    ts,
                    (unsigned long)ep->ExceptionRecord->ExceptionCode,
                    ep->ExceptionRecord->ExceptionAddress,
                    (unsigned long)GetCurrentThreadId());
            fclose(cf);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    });
#endif
    App* app = new App();
    int r = app->run();
    delete app;
    return r;
}
