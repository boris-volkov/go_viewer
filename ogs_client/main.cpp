#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cfloat>
#include "../go_viewer.hpp"
#include "../go_rules.hpp"
#include "../game_state.hpp"
#include "../analysis_state.hpp"
#include "../catalog.hpp"
#include "../renderer.hpp"
#include "ogs_client.hpp"
#include "ogs_net.hpp"
#include "katago.hpp"

#include "json.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <memory>
#include <algorithm>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#endif

// Registered once in main(); OgsNet reads this to push SDL events.
Uint32 g_net_event_type = 0;

// ── Opponent alias ────────────────────────────────────────────────────────────

static const char* random_dbz_name() {
    static const char* names[] = {
        "Goku", "Vegeta", "Piccolo", "Gohan", "Trunks",
        "Krillin", "Frieza", "Cell", "Buu", "Broly",
        "Goten", "Gotenks", "Bardock", "Raditz", "Nappa",
        "Android 17", "Android 18", "Tien", "Yamcha", "Beerus",
        "Whis", "Jiren", "Hit", "Caulifla", "Kale",
        "Cooler", "Turles", "Bojack", "Janemba", "Pikkon",
    };
    static constexpr int N = (int)(sizeof(names) / sizeof(names[0]));
    return names[rand() % N];
}

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
    int   board_size                      = BOARD_SIZE;
    char  black_name[NAME_LEN]            = "Black";
    char  white_name[NAME_LEN]            = "White";
    char  result[32]                      = {};
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

    FILE* fp = fopen(path.c_str(), "rb");
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
    // pushing moves to progressively deeper levels.
    bool branch_taken[256] = {};
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

    // Game catalog for reviewing saved OGS games
    Catalog      catalog_;
    std::string  my_username_;   // from config.ini; used to locate games/<name>/ dir

    // Catalog thumbnails (BOARD_SIZE stride matches DrawState::catalog_thumb_open type)
    std::string  thumb_path_;
    char         thumb_open_ [BOARD_SIZE][BOARD_SIZE] = {};
    char         thumb_final_[BOARD_SIZE][BOARD_SIZE] = {};
    bool         thumb_valid_      = false;
    int          thumb_board_size_ = BOARD_SIZE;

    void update_catalog_thumb() {
        std::string sel = catalog_.selected_entry_path();
        if (sel == thumb_path_) return;
        thumb_path_  = sel;
        thumb_valid_ = !sel.empty()
            && sgf_board_at(sel, thumb_open_,  &thumb_board_size_, THUMB_OPENING_MOVES)
            && sgf_board_at(sel, thumb_final_,  nullptr);
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

    // Resign confirm
    bool resign_confirm_ = false;
    bool pass_confirm_   = false;

    // Flash message
    std::string flash_;
    Uint32      flash_until_    = 0;
    Uint32      ko_flash_until_   = 0;
    Uint32      kata_query_after_      = 0;    // deferred KataGo query — fires after user settles
    bool        kata_analysis_enabled_ = true; // toggled by START in GAME_OVER

    // Match settings
    MatchPrefs            match_prefs_;
    Renderer::MatchMenu   match_menu_;

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

    // Chain mode toggle (Y button)
    bool chain_mode_       = true;

    // Help overlay / quit confirm
    bool show_help_    = false;
    bool quit_confirm_ = false;

    // Undo-request state
    bool undo_pending_     = false;
    int  undo_move_number_ = 0;

    // Stone removal: true once OGS has sent territory data; prevents KataGo from overwriting it
    bool stone_removal_has_ogs_territory_ = false;
    // True after user presses A but before server confirms our acceptance
    bool        my_accept_sent_           = false;
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
    int                bg_analysis_next_  = 0; // next main-line depth to submit
    int                bg_analysis_depth_ = -1;// depth of the currently-pending bg query
    bool               bg_analysis_busy_  = false;
    bool               fg_kata_pending_   = false; // foreground KataGo query in-flight
    std::string        companion_path_;            // path of .katago companion file

    // Returns the best available KataGo process for the given board size.
    KatagoProc& kata_for(int bs) {
        return (bs == 9 && kata_9_.running()) ? kata_9_ : kata_;
    }

    // KataGo GTP subprocess for local games vs the human SL model
    KataGoGtp   kata_gtp_;
    std::string kata_exe_;          // path from config (shared with analysis)
    std::string kata_model_;        // path from config (shared with analysis)
    std::string kata_human_model_;  // path to humanv0.bin.gz (enables VS KATAGO)

    // Local game state
    bool        is_local_game_       = false;
    bool        local_prev_was_pass_ = false;  // true if the last move (by either side) was a pass
    bool        local_result_pending_ = false;  // freeze on final board until user presses a button
    std::string local_game_score_;             // score string shown during stone removal, empty until ownership arrives

    void start_local_game();
    void handle_katago_gtp_move(int row, int col);
    // forced_result: non-empty means the result is already known (e.g. resignation).
    // In that case skip the GTP final_status query and go straight to territory display.
    void begin_local_stone_removal(const std::string& forced_result = "");
    void end_local_game(const std::string& result);

    // Left-stick joystick cursor state
    Sint16 js_left_x_  = 0;
    Sint16 js_left_y_  = 0;
    float  js_acc_x_   = 0.f;
    float  js_acc_y_   = 0.f;

    bool init();
    void cleanup();
    void event_loop();
    void handle_controller_button(Uint8 button);
    void handle_net_msg(const NetMsg& msg);
    // forced_color: -1 = use current turn (normal), 0/1 = force that color and don't flip turn
    void apply_move(int col, int row, int forced_color = -1);
    void step_history(int delta);  // delta=-1 back, +1 forward; sets history_pos
    void load_demo_game();
    void save_live_game();
    void save_companion();          // write .katago file alongside the SGF
    void load_companion();          // read .katago file if present
    void load_sgf_for_review(const std::string& path);
    void open_game_catalog();
    void draw();
    void build_analysis_tree();
    void build_analysis_tree_render();
    void apply_analysis_move(int col, int row);
    bool is_legal_analysis_move(int col, int row) const;

    void set_status(const std::string& s) { status_ = s; }

    Renderer::DrawState make_ds();
};

// ── Init / cleanup ────────────────────────────────────────────────────────────

bool App::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) return false;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

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
    int is_black = (forced_color >= 0) ? forced_color : (int)game_.board.turn_is_black;
    game_.board.place_stone(row, col, is_black);
    game_.board.save_snapshot();
    if (forced_color >= 0) {
        game_.history.push_back(game_.board);
        if (move_scores_.size() < game_.history.size()) { move_scores_.push_back(FLT_MAX); move_marked_.push_back(false); }
        return;  // pre-placed handicap stone: caller sets turn after
    }
    // Free handicap: first `handicap` stones are all black; after last one, white plays
    if (game_.free_handicap && game_.board.stone_count <= game_.handicap) {
        game_.board.turn_is_black = (game_.board.stone_count < game_.handicap) ? 1 : 0;
    } else {
        game_.board.turn_is_black = !is_black;
    }
    game_.history.push_back(game_.board);
    if (move_scores_.size() < game_.history.size()) { move_scores_.push_back(FLT_MAX); move_marked_.push_back(false); }
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

    // Create new child node
    auto child = std::make_unique<AnalysisNode>();
    child->board    = analysis_cur_->board;
    child->move_col = col;
    child->move_row = row;
    child->depth    = analysis_cur_->depth + 1;
    child->parent   = analysis_cur_;

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

void App::load_demo_game() {
    demo_active_ = false;
    demo_.rows.clear();
    demo_.cols.clear();
    demo_.colors.clear();
    demo_.pos = 0;

    // Try next to exe first, then one level up (dev layout: exe is in ogs_client/)
    std::string games_dir = exe_dir() + "games";
    std::vector<std::string> files;
    if (!Catalog::list_sgf_files(games_dir, files) || files.empty()) {
        games_dir = exe_dir() + "../games";
        files.clear();
        if (!Catalog::list_sgf_files(games_dir, files) || files.empty()) return;
    }

    // Pick a random file, skipping the one we just played if possible.
    // list_sgf_files returns paths relative to games_dir, so build absolute path.
    static std::string last_rel;
    std::srand((unsigned)std::time(nullptr) ^ (unsigned)SDL_GetTicks());
    std::string rel;
    if (files.size() == 1) {
        rel = files[0];
    } else {
        do { rel = files[(size_t)std::rand() % files.size()]; }
        while (rel == last_rel);
    }
    last_rel = rel;
    std::string path = Catalog::join_path(games_dir, rel);

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

// ── Controller ────────────────────────────────────────────────────────────────

void App::handle_controller_button(Uint8 btn) {
    // Credential prompt: not controller-driven (keyboard only for now)
    if (state_ == AppState::CREDENTIAL_PROMPT) return;

    // Catalog overlay: intercept all input while open
    if (catalog_.active) {
        const int CAT_VIS = 15;
        switch (btn) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (catalog_.index > 0) {
                catalog_.index--;
                if (catalog_.index < catalog_.scroll)
                    catalog_.scroll = catalog_.index;
                catalog_.ensure_names_loaded(catalog_.index - 2, 6);
                update_catalog_thumb();
            }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (catalog_.index + 1 < (int)catalog_.entries.size()) {
                catalog_.index++;
                if (catalog_.index >= catalog_.scroll + CAT_VIS)
                    catalog_.scroll = catalog_.index - CAT_VIS + 1;
                catalog_.ensure_names_loaded(catalog_.index - 2, 6);
                update_catalog_thumb();
            }
            break;
        case SDL_CONTROLLER_BUTTON_A:
            catalog_.select();
            if (catalog_.selection_made) {
                load_sgf_for_review(catalog_.selected_path);
                catalog_.selection_made = false;
            }
            break;
        default:
            // B, START, X, or anything else — close catalog
            catalog_.close();
            break;
        }
        draw();
        return;
    }

    // History navigation: LT/RT work in any game state
    if (btn == 0xFD || btn == 0xFE) {
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
                } else {
                    if (!analysis_cur_->children.empty())
                        analysis_cur_ = analysis_cur_->children[analysis_cur_->active_child].get();
                }
                build_analysis_tree_render();
                kata_suggestion_count_ = 0;
                kata_score_lead_ = FLT_MAX;
                kata_query_after_ = SDL_GetTicks() + 1000;
            } else {
                step_history(btn == 0xFD ? -1 : +1);
            }
            draw();
        }
        return;
    }

    // Y button: mark/unmark the current move for analysis attention
    if (btn == SDL_CONTROLLER_BUTTON_Y) {
        int depth = -1;
        if (state_ == AppState::PLAYING) {
            depth = (game_.history_pos >= 0) ? game_.history_pos
                                             : (int)game_.history.size() - 1;
        } else if (state_ == AppState::GAME_OVER && analysis_cur_) {
            depth = analysis_cur_->depth;
        }
        if (depth >= 0 && depth < (int)move_marked_.size()) {
            move_marked_[depth] = !move_marked_[depth];
            if (state_ == AppState::GAME_OVER) build_analysis_tree_render();
            flash_       = move_marked_[depth] ? "MARKED" : "UNMARKED";
            flash_until_ = SDL_GetTicks() + 1200;
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
            resign_confirm_  = false;
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

    // Cancel resign confirm on any non-Start button
    if (resign_confirm_ && btn != SDL_CONTROLLER_BUTTON_START) {
        resign_confirm_ = false;
        draw();
        return;
    }
    // Cancel pass confirm on any non-B button
    if (pass_confirm_ && btn != SDL_CONTROLLER_BUTTON_B) {
        pass_confirm_ = false;
        draw();
        return;
    }

    if (state_ == AppState::LOBBY) {
        if (btn == SDL_CONTROLLER_BUTTON_X) {
            open_game_catalog();
            draw();
        } else if (btn == SDL_CONTROLLER_BUTTON_START || btn == SDL_CONTROLLER_BUTTON_A) {
            net_.cmd_find_match(match_prefs_);
            state_ = AppState::SEARCHING;
            set_status("SEARCHING...");
            draw();
        } else if (btn == SDL_CONTROLLER_BUTTON_BACK) {
            // Open match settings menu
            match_menu_.focus_col    = 0;
            match_menu_.focus_row    = 0;
            match_menu_.katago_mode  = match_prefs_.katago_mode;
            match_menu_.katago_str   = match_prefs_.katago_str;
            for (int i = 0; i < 3; i++) match_menu_.size_sel[i]  = match_prefs_.sizes[i];
            for (int i = 0; i < 3; i++) match_menu_.speed_sel[i] = match_prefs_.speeds[i];
            state_ = AppState::MATCH_MENU;
            renderer_->draw_match_menu(match_menu_);
        }
        return;
    }

    if (state_ == AppState::MATCH_MENU) {
        // Col 1 has 3 items in OGS mode, 7 in KataGo mode
        int col1_size = match_menu_.katago_mode ? 7 : 3;
        int col_sizes[2] = {3, col1_size};
        int n = col_sizes[match_menu_.focus_col];

        auto save_prefs = [&]() {
            for (int i = 0; i < 3; i++) match_prefs_.sizes[i]  = match_menu_.size_sel[i];
            for (int i = 0; i < 3; i++) match_prefs_.speeds[i] = match_menu_.speed_sel[i];
            match_prefs_.katago_mode = match_menu_.katago_mode;
            match_prefs_.katago_str  = match_menu_.katago_str;
        };
        auto close_menu = [&]() {
            save_prefs();
            state_ = AppState::LOBBY;
            set_status("PRESS START TO FIND GAME");
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
            match_menu_.focus_col = 0;
            if (match_menu_.focus_row >= 3) match_menu_.focus_row = 2;
            renderer_->draw_match_menu(match_menu_);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            match_menu_.focus_col = 1;
            renderer_->draw_match_menu(match_menu_);
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            // Toggle OGS / KataGo mode (only when human model is configured)
            if (!kata_human_model_.empty()) {
                match_menu_.katago_mode = !match_menu_.katago_mode;
                // Clamp row when switching to OGS mode's smaller column
                if (!match_menu_.katago_mode && match_menu_.focus_col == 1
                        && match_menu_.focus_row >= 3)
                    match_menu_.focus_row = 2;
                renderer_->draw_match_menu(match_menu_);
            }
            break;
        case SDL_CONTROLLER_BUTTON_A: {
            int r = match_menu_.focus_row;
            if (match_menu_.focus_col == 0) {
                match_menu_.size_sel[r] = !match_menu_.size_sel[r];
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
            save_prefs();
            if (match_menu_.katago_mode) {
                start_local_game();
            } else {
                net_.cmd_find_match(match_prefs_);
                state_ = AppState::SEARCHING;
                set_status("SEARCHING...");
                draw();
            }
            break;
        default: break;
        }
        return;
    }

    if (state_ == AppState::SEARCHING) {
        if (btn == SDL_CONTROLLER_BUTTON_B || btn == SDL_CONTROLLER_BUTTON_BACK) {
            net_.cmd_cancel_match();
            state_ = AppState::LOBBY;
            set_status("PRESS START TO FIND GAME");
            draw();
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
                set_status("WAITING...");
            }
            draw();
            break;

        case SDL_CONTROLLER_BUTTON_B:
            if (!game_.my_turn) break;
            if (pass_confirm_) {
                pass_confirm_ = false;
                if (is_local_game_) {
                    game_.board.turn_is_black = !game_.board.turn_is_black;
                    game_.history.push_back(game_.board);
                    kata_gtp_.send_play(game_.my_color, -1, -1, game_.board_size);
                    local_prev_was_pass_ = true;
                    game_.my_turn = false;
                    kata_gtp_.request_genmove(1 - game_.my_color);
                    set_status("PASSED — KATAGO THINKING...");
                } else {
                    net_.cmd_send_pass(game_.game_id);
                    game_.board.turn_is_black = !game_.board.turn_is_black;
                    game_.pending_col = -1;
                    game_.pending_row = -1;
                    game_.my_turn = false;
                    set_status("PASSED — WAITING...");
                }
            } else {
                pass_confirm_ = true;
            }
            draw();
            break;

        case SDL_CONTROLLER_BUTTON_START:
            if (resign_confirm_) {
                resign_confirm_ = false;
                if (is_local_game_) {
                    end_local_game("W+R");  // player resigned → White wins
                } else {
                    net_.cmd_send_resign(game_.game_id);
                    set_status("RESIGNED");
                    draw();
                }
            } else {
                resign_confirm_ = true;
                draw();
            }
            break;

        case SDL_CONTROLLER_BUTTON_BACK:
            if (is_local_game_) {
                kata_gtp_.stop();
                is_local_game_ = false;
                state_ = AppState::LOBBY;
                game_.board.reset();
                load_demo_game();
                set_status("PRESS START TO FIND GAME");
                draw();
                return;
            }
            break;

        default: break;
        }
        return;
    }

    if (state_ == AppState::STONE_REMOVAL) {
        if (is_local_game_) {
            if (btn == SDL_CONTROLLER_BUTTON_A || btn == SDL_CONTROLLER_BUTTON_START) {
                if (!local_game_score_.empty())
                    end_local_game(local_game_score_);
                // else still analyzing — ignore the press
            }
            return;
        }
        if (btn == SDL_CONTROLLER_BUTTON_A || btn == SDL_CONTROLLER_BUTTON_START) {
            net_.cmd_accept_stones(game_.game_id);
            my_accept_sent_ = true;
            set_status("ACCEPTING...");
            draw();
        }
        return;
    }

    if (state_ == AppState::GAME_OVER) {
        // Freeze on final board after local game — any button dismisses into analysis
        if (local_result_pending_) {
            local_result_pending_ = false;
            if (btn == SDL_CONTROLLER_BUTTON_BACK) {
                save_companion();
                state_ = AppState::LOBBY;
                game_.board.reset();
                load_demo_game();
                set_status("PRESS START TO FIND GAME");
            } else {
                build_analysis_tree();
                kata_analysis_enabled_ = true;
                set_status("GAME OVER — " + game_.result);
                kata_for(game_.board_size).query_moves(
                    analysis_cur_ ? analysis_cur_->board.board : game_.board.board,
                    game_.board_size,
                    analysis_cur_ ? (analysis_cur_->board.turn_is_black == 1)
                                  : (game_.board.turn_is_black == 1));
            }
            draw();
            return;
        }

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
            if (analysis_cur_) {
                // Find nearest ancestor whose parent has multiple children (the fork point).
                // branch_root ends up as the immediate child of that fork.
                AnalysisNode* branch_root = analysis_cur_;
                while (branch_root->parent &&
                       (int)branch_root->parent->children.size() <= 1)
                    branch_root = branch_root->parent;

                if (branch_root->parent &&
                    (int)branch_root->parent->children.size() > 1) {
                    AnalysisNode* fork_node = branch_root->parent;
                    auto& siblings = fork_node->children;

                    int cur_idx = 0;
                    for (int i = 0; i < (int)siblings.size(); i++)
                        if (siblings[i].get() == branch_root) { cur_idx = i; break; }

                    int delta    = (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 1 : -1;
                    int next_idx = (cur_idx + delta + (int)siblings.size()) % (int)siblings.size();
                    fork_node->active_child = next_idx;

                    // Navigate into the new sibling branch as deep as possible.
                    int steps = analysis_cur_->depth - fork_node->depth - 1;
                    AnalysisNode* dest = siblings[next_idx].get();
                    for (int i = 0; i < steps; i++) {
                        if (dest->children.empty()) break;
                        dest = dest->children[dest->active_child].get();
                    }
                    analysis_cur_ = dest;
                    build_analysis_tree_render();
                    kata_suggestion_count_ = 0;
                    kata_score_lead_ = FLT_MAX;
                    kata_query_after_ = SDL_GetTicks() + 1000;
                    draw();
                }
            }
            break;
        case SDL_CONTROLLER_BUTTON_A:
            if (analysis_cur_) {
                int col = game_.cursor_f, row = game_.cursor_r;
                if (analysis_cur_->board.board[row][col] == 0) {
                    apply_analysis_move(col, row);
                    kata_suggestion_count_ = 0;
                    kata_score_lead_ = FLT_MAX;
                    if (kata_analysis_enabled_) {
                        kata_query_after_ = 0;
                        kata_for(game_.board_size).query_moves(
                            analysis_cur_->board.board, game_.board_size,
                            analysis_cur_->board.turn_is_black == 1);
                    }
                    draw();
                }
            }
            break;
        case SDL_CONTROLLER_BUTTON_B:
            if (analysis_cur_ && analysis_cur_->parent) {
                analysis_cur_ = analysis_cur_->parent;
                analysis_cur_->active_child = 0;  // RT follows main line after stepping back
                build_analysis_tree_render();
                kata_suggestion_count_ = 0;
                kata_score_lead_ = FLT_MAX;
                kata_query_after_ = SDL_GetTicks() + 1000;
                draw();
            }
            break;
        case SDL_CONTROLLER_BUTTON_X:
            open_game_catalog();
            draw();
            break;
        case SDL_CONTROLLER_BUTTON_START:
            kata_analysis_enabled_ = !kata_analysis_enabled_;
            if (kata_analysis_enabled_) {
                flash_       = "ANALYSIS ON";
                flash_until_ = SDL_GetTicks() + 1500;
                kata_query_after_ = SDL_GetTicks() + 1000;
            } else {
                flash_       = "ANALYSIS OFF";
                flash_until_ = SDL_GetTicks() + 1500;
                kata_suggestion_count_ = 0;
                kata_score_lead_ = FLT_MAX;
                kata_query_after_ = 0;
            }
            draw();
            break;
        case SDL_CONTROLLER_BUTTON_BACK:
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
            set_status("PRESS START TO FIND GAME");
            game_.board.reset();
            load_demo_game();
            draw();
            break;
        }
        return;
    }
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
    // Locate games/ directory (same two-path probe as demo-game loader)
    std::string games_dir = exe_dir() + "games";
    auto is_dir = [](const std::string& p) {
#ifdef _WIN32
        DWORD a = GetFileAttributesA(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
        struct stat st;
        return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
    };
    if (!is_dir(games_dir)) games_dir = exe_dir() + "../games";
    if (!is_dir(games_dir)) games_dir = exe_dir();

    std::string my_name  = (game_.my_color == 1) ? game_.black_name : game_.white_name;
    std::string opp_name = (game_.my_color == 1) ? game_.white_name : game_.black_name;
    std::string player_dir = Catalog::join_path(games_dir, my_name);
#ifdef _WIN32
    CreateDirectoryA(games_dir.c_str(), nullptr);
    CreateDirectoryA(player_dir.c_str(), nullptr);
#else
    mkdir(games_dir.c_str(), 0755);
    mkdir(player_dir.c_str(), 0755);
#endif

    time_t t = time(nullptr);
    char date[16];
    strftime(date, sizeof(date), "%Y%m%d", localtime(&t));
    std::string filename = std::string(date) + "-"
                         + sgf_sanitize(my_name) + "-"
                         + sgf_sanitize(opp_name) + ".sgf";
    std::string path = Catalog::join_path(player_dir, filename);

    // Derive companion path before launching async fetch
    companion_path_ = path.substr(0, path.rfind('.')) + ".katago";

    int game_id = game_.game_id;
    std::thread([this, game_id, path] {
        net_.fetch_sgf(game_id, path);
    }).detach();
}

void App::save_companion() {
    if (companion_path_.empty()) return;
    if (move_scores_.empty() && move_marked_.empty()) return;

    FILE* f = fopen(companion_path_.c_str(), "w");
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
    FILE* f = fopen(companion_path_.c_str(), "r");
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

void App::start_local_game() {
    // Pick board size (first checked, default 9x9)
    int bs = 19;
    if (match_prefs_.sizes[0]) bs = 9;
    else if (match_prefs_.sizes[1]) bs = 13;

    int str = match_prefs_.katago_str;
    if (str < 0 || str >= 7) str = 2;

    if (!kata_gtp_.start(kata_exe_, kata_model_, kata_human_model_,
                         KATA_GTP_PROFILES[str], bs, 7.5f)) {
        flash_       = "FAILED TO START KATAGO";
        flash_until_ = SDL_GetTicks() + 3000;
        state_ = AppState::LOBBY;
        set_status("PRESS START TO FIND GAME");
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
    game_.my_color      = 1;   // player is Black
    game_.my_player_id  = 0;
    game_.black_name    = my_username_.empty() ? "You" : my_username_;
    game_.white_name    = KATA_GTP_NAMES[str];
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

    black_label_ = game_.black_name;
    white_label_ = game_.white_name;

    is_local_game_        = true;
    local_prev_was_pass_  = false;
    local_result_pending_ = false;
    game_.my_turn         = true;

    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;
    kata_analysis_enabled_ = false;   // analysis overlay off during live play

    state_ = AppState::PLAYING;
    set_status("YOUR TURN  (BLACK)");
    draw();
}

void App::handle_katago_gtp_move(int row, int col) {
    if (row == -2) {
        // KataGo resigned — show territory view before going to analysis
        flash_       = "KATAGO RESIGNED — YOU WIN!";
        flash_until_ = SDL_GetTicks() + 4000;
        kata_gtp_.stop();  // no more GTP commands needed
        begin_local_stone_removal("B+R");
        return;
    }
    if (row == -1) {
        // KataGo passed
        flash_       = "KATAGO PASSED";
        flash_until_ = SDL_GetTicks() + 3000;
        game_.board.turn_is_black = !game_.board.turn_is_black;
        game_.history.push_back(game_.board);
        if (local_prev_was_pass_) {
            begin_local_stone_removal();
            return;
        }
        local_prev_was_pass_ = true;
        game_.my_turn = true;
        set_status("YOUR TURN  (BLACK)");
        draw();
        return;
    }
    // Normal move
    apply_move(col, row);
    local_prev_was_pass_ = false;
    pass_confirm_        = false;
    game_.my_turn        = true;
    set_status("YOUR TURN  (BLACK)");
    draw();
}

void App::begin_local_stone_removal(const std::string& forced_result) {
    state_ = AppState::STONE_REMOVAL;
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

void App::end_local_game(const std::string& result) {
    kata_gtp_.stop();
    is_local_game_        = false;
    local_result_pending_ = true;
    state_ = AppState::GAME_OVER;
    game_.result = result;
    save_live_game();
    set_status(result + "  —  PRESS ANY BUTTON TO REVIEW");
    kata_suggestion_count_ = 0;
    kata_score_lead_       = FLT_MAX;
    kata_analysis_enabled_ = false;
    // Keep analysis_cur_ = nullptr so make_ds() shows game_.board (final position)
    // and doesn't override the status with "ANALYSIS - MOVE N".
    // build_analysis_tree() is deferred until the user dismisses this screen.
    analysis_root_.reset();
    analysis_cur_ = nullptr;
    draw();
}

// ── Game catalog ──────────────────────────────────────────────────────────────

void App::open_game_catalog() {
    if (catalog_.active) return;

    // Locate games/<username>/ — same two-path probe as demo loader
    std::string gdir = exe_dir() + "games";
    auto is_dir = [](const std::string& p) {
#ifdef _WIN32
        DWORD a = GetFileAttributesA(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
        struct stat st;
        return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
    };
    if (!is_dir(gdir)) gdir = exe_dir() + "../games";

    std::string my_dir = Catalog::join_path(gdir, my_username_);

    // Open in flat filesystem mode (skip the virtual player browser)
    catalog_.open(my_dir);
    catalog_.virtual_player_mode = false;
    catalog_.virtual_year_mode   = false;
    catalog_.current_subdir      = "";
    catalog_.load_entries();
    catalog_.index  = 0;
    catalog_.scroll = 0;
    catalog_.ensure_names_loaded(0, 17);  // prefill first visible page (15 rows + margin)
    thumb_path_ = "";                     // force thumbnail reload on first draw
    update_catalog_thumb();
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

    // Push initial empty position
    game_.history.push_back(game_.board);

    // Replay moves: trust the explicit color tags in the SGF
    for (int i = 0; i < g.move_count; i++) {
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

    // Reset score graph / mark storage and derive companion path
    move_scores_.assign(game_.history.size(), FLT_MAX);
    move_marked_.assign(game_.history.size(), false);
    bg_analysis_next_  = 0;
    bg_analysis_depth_ = -1;
    bg_analysis_busy_  = false;
    companion_path_    = path.substr(0, path.rfind('.')) + ".katago";
    load_companion();   // pre-populate scores and marks if the file exists

    // Build analysis tree from the replayed history
    build_analysis_tree();

    // Enter GAME_OVER analysis mode
    state_ = AppState::GAME_OVER;
    set_status("REVIEW — " + std::string(g.result));
    kata_suggestion_count_ = 0;
    kata_score_lead_ = FLT_MAX;
    if (analysis_cur_) {
        fg_kata_pending_ = true;
        kata_for(game_.board_size).query_moves(
            analysis_cur_->board.board, game_.board_size,
            analysis_cur_->board.turn_is_black == 1);
    }
}

// ── Network message handler ───────────────────────────────────────────────────

void App::handle_net_msg(const NetMsg& msg) {
    switch (msg.type) {
    case NetMsgType::AUTH_OK:
        state_ = AppState::LOBBY;
        set_status("PRESS START TO FIND GAME");
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
        // Show opponent as a random DBZ character so we play the board, not the name
        std::string alias = random_dbz_name();
        black_label_ = (msg.my_color == 1) ? "boris" : alias;
        white_label_ = (msg.my_color == 0) ? "boris" : alias;
        game_.black_secs        = msg.black_secs;
        game_.white_secs        = msg.white_secs;
        game_.black_periods     = msg.black_periods;
        game_.white_periods     = msg.white_periods;
        game_.black_period_secs = msg.black_period_secs;
        game_.white_period_secs = msg.white_period_secs;
        game_.clock_tick        = SDL_GetTicks();
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

        // Initialise score-graph / mark storage
        move_scores_.resize(game_.history.size(), FLT_MAX);
        move_marked_.resize(game_.history.size(), false);
        bg_analysis_next_  = 0;
        bg_analysis_depth_ = -1;
        bg_analysis_busy_  = false;

        state_ = AppState::PLAYING;
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
                game_.clock_tick = SDL_GetTicks();
            }
            if (msg.white_secs >= 0) {
                game_.white_secs        = msg.white_secs;
                game_.white_periods     = msg.white_periods;
                game_.white_period_secs = msg.white_period_secs;
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
                    game_.board.turn_is_black = !game_.board.turn_is_black;
                    flash_       = "OPPONENT PASSED";
                    flash_until_ = SDL_GetTicks() + 3000;
                }
                game_.my_turn  = true;
                pass_confirm_  = false;
                resign_confirm_ = false;
                set_status("YOUR TURN");
            }
            draw();
        }
        break;

    case NetMsgType::CLOCK_UPDATE:
        if (msg.black_secs >= 0) {
            game_.black_secs        = msg.black_secs;
            game_.black_periods     = msg.black_periods;
            game_.black_period_secs = msg.black_period_secs;
            game_.clock_tick = SDL_GetTicks();
        }
        if (msg.white_secs >= 0) {
            game_.white_secs        = msg.white_secs;
            game_.white_periods     = msg.white_periods;
            game_.white_period_secs = msg.white_period_secs;
        }
        draw();
        break;

    case NetMsgType::STONE_REMOVAL: {
        state_ = AppState::STONE_REMOVAL;
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
            set_status("ACCEPTING...");
        } else {
            my_accept_sent_ = false;
            set_status("PRESS A TO ACCEPT DEAD STONES");
        }
        stone_removal_all_removed_ = msg.text;
        // Ask KataGo for territory estimate only when OGS hasn't provided its own data yet.
        // Once OGS territory arrives, KataGo results are discarded (see poll loop).
        if (!stone_removal_has_ogs_territory_ && kata_for(game_.board_size).running())
            kata_for(game_.board_size).query_ownership(game_.board.board, game_.board_size,
                                                       game_.dead_stones, 7.5f);
        draw();
        break;
    }

    case NetMsgType::GAME_OVER:
        state_ = AppState::GAME_OVER;
        game_.result = msg.text;
        save_live_game();
        set_status("GAME OVER — " + msg.text);
        kata_suggestion_count_ = 0;
        kata_score_lead_ = FLT_MAX;
        build_analysis_tree();  // populates analysis_root_ and analysis_cur_
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

    case NetMsgType::ACCEPT_STATUS:
        if (state_ == AppState::STONE_REMOVAL) {
            if (msg.my_accepted == 1 && msg.opp_accepted == 0)
                set_status("WAITING FOR OPPONENT TO ACCEPT...");
            else if (msg.my_accepted == 0 && msg.opp_accepted == 1)
                set_status("OPPONENT ACCEPTED — PRESS A TO ACCEPT");
            else if (msg.my_accepted == 0 && !my_accept_sent_)
                set_status("PRESS A TO ACCEPT DEAD STONES");
            // If my_accept_sent_ && server still shows my_accepted=0: keep "ACCEPTING..."
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
            resign_confirm_   = false;
            // If we had a pending pass that the server rejected (phase reverted to play),
            // undo the optimistic turn flip so my_turn is computed from the correct board state.
            if (game_.pending_col == -1 && game_.pending_row == -1)
                game_.board.turn_is_black = !game_.board.turn_is_black;
            game_.pending_col = -2;
            game_.pending_row = -2;
            bool btp = (game_.board.turn_is_black == 1);
            game_.my_turn = (btp && game_.my_color == 1) || (!btp && game_.my_color == 0);
            set_status(game_.my_turn ? "YOUR TURN" : "WAITING...");
            draw();
        }
        break;

    case NetMsgType::DISCONNECTED:
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
    if (state_ == AppState::PLAYING && game_.clock_tick > 0) {
        int elapsed = (int)((SDL_GetTicks() - game_.clock_tick) / 1000);
        auto tick = [](int secs, int periods, int period_secs, int elapsed,
                       int& out_secs, int& out_periods) {
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
            } else {
                out_secs    = std::max(0, secs - elapsed);
                out_periods = periods;
            }
        };
        if (game_.board.turn_is_black == 1)
            tick(game_.black_secs, game_.black_periods, game_.black_period_secs, elapsed,
                 b_secs, b_periods);
        else if (game_.board.turn_is_black == 0)
            tick(game_.white_secs, game_.white_periods, game_.white_period_secs, elapsed,
                 w_secs, w_periods);
    }

    // GAME_OVER is navigated through the analysis tree; PLAYING/STONE_REMOVAL use flat history.
    bool in_history = (state_ == AppState::PLAYING || state_ == AppState::STONE_REMOVAL)
                      && (game_.history_pos >= 0 && !game_.history.empty());

    const char* status_cstr = status_.empty() ? nullptr : status_.c_str();
    if (pass_confirm_)   status_cstr = "PRESS B AGAIN TO PASS";
    if (resign_confirm_) status_cstr = "PRESS START AGAIN TO RESIGN";
    if (undo_pending_)   status_cstr = "UNDO REQUEST: A=Accept  B=Deny";
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

    bool playing = (state_ == AppState::PLAYING || state_ == AppState::STONE_REMOVAL || state_ == AppState::GAME_OVER);
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
        /* game            */ disp_board,
        /* analysis        */ nullptr,
        /* analysis_mode   */ false,
        /* game_mode       */ false,
        /* guess_mode      */ false,
        /* guess_score     */ 0,
        /* chain_mode      */ chain_mode_,
        /* free_mode       */ false,
        /* active_board_size */ active_bs,
        /* show_help       */ show_help_,
        /* catalog         */ catalog_,
        /* black_name      */ bname,
        /* white_name      */ wname,
        /* result_message  */ (state_ == AppState::GAME_OVER) ? game_.result : empty_str_,
        /* game_date       */ empty_str_,
        /* game_comment    */ empty_str_,
        /* move_delay_ms   */ MOVE_DELAY_MS,
        /* speed_until     */ 0,
        /* suppress_present*/ false,
        /* territory_drill */ false,
        /* territory_board */ nullptr,
        /* territory_b     */ 0,
        /* territory_w     */ 0,
        /* territory_ans   */ false,
        /* territory_cor   */ false,
        /* stone_filter    */ 0,
        /* cursor_x        */ -1,
        /* cursor_y        */ -1,
        /* cursor_type     */ 0,
        /* show_move_nums  */ false,
        /* sgf_moves       */ nullptr,
        /* sgf_colors      */ nullptr,
        /* sgf_game_index  */ 0,
        /* analysis_num    */ nullptr,
        /* analysis_col    */ nullptr,
        /* quit_confirm    */ quit_confirm_,
        /* box_sel_pts     */ nullptr,
        /* box_sel_count   */ 0,
        /* box_drag_active */ false,
        /* box_drag_r1     */ 0,
        /* box_drag_f1     */ 0,
        /* box_drag_r2     */ 0,
        /* box_drag_f2     */ 0,
        /* thumb_valid     */ thumb_valid_,
        /* thumb_open      */ thumb_valid_ ? thumb_open_  : nullptr,
        /* thumb_final     */ thumb_valid_ ? thumb_final_ : nullptr,
        /* thumb_board_sz  */ thumb_board_size_,
        /* flash_message   */ flash_,
        /* flash_until     */ flash_until_,
        /* save_input_step */ 0,
        /* save_input_buf  */ empty_str_,
        // Live fields
        /* live_mode       */ live,
        /* live_cursor_r   */ (state_ == AppState::PLAYING && !in_history) ? game_.cursor_r :
                             (state_ == AppState::GAME_OVER)              ? game_.cursor_r : -1,
        /* live_cursor_f   */ (state_ == AppState::PLAYING && !in_history) ? game_.cursor_f :
                             (state_ == AppState::GAME_OVER)              ? game_.cursor_f : -1,
        /* live_my_color   */ game_.my_color,
        /* live_my_turn    */ game_.my_turn,
        /* live_black_secs        */ playing ? b_secs : -1,
        /* live_white_secs        */ playing ? w_secs : -1,
        /* live_black_periods     */ playing ? b_periods : -1,
        /* live_white_periods     */ playing ? w_periods : -1,
        /* live_black_period_secs */ playing ? game_.black_period_secs : -1,
        /* live_white_period_secs */ playing ? game_.white_period_secs : -1,
        /* live_status     */ live ? status_cstr : nullptr,
        /* live_dead_stones     */ (state_ == AppState::STONE_REMOVAL ||
                                    (state_ == AppState::GAME_OVER && local_result_pending_))
                                       ? game_.dead_stones : nullptr,
        /* live_ownership       */ (state_ == AppState::STONE_REMOVAL ||
                                    (state_ == AppState::GAME_OVER && local_result_pending_))
                                       ? game_.ownership   : nullptr,
        /* live_in_history      */ in_history,
        /* live_suggestions     */ (state_ == AppState::GAME_OVER && kata_suggestion_count_ > 0)
                                       ? kata_suggestions_ : nullptr,
        /* live_suggestion_count*/ (state_ == AppState::GAME_OVER) ? kata_suggestion_count_ : 0,
        /* live_hovered_suggestion */ [&]() -> int {
            if (state_ != AppState::GAME_OVER) return -1;
            for (int i = 0; i < std::min(kata_suggestion_count_, 3); i++)
                if (kata_suggestions_[i].row == game_.cursor_r &&
                    kata_suggestions_[i].col == game_.cursor_f) return i;
            return -1;
        }(),
        /* live_cursor_ko       */ (ko_flash_until_ > SDL_GetTicks()),
        /* live_kata_score_lead    */ (state_ == AppState::GAME_OVER) ? kata_score_lead_ : FLT_MAX,
        /* live_actual_move_r */ [&]() -> int {
            if (state_ != AppState::GAME_OVER || !analysis_cur_ || analysis_cur_->children.empty())
                return -1;
            return analysis_cur_->children[0]->move_row;
        }(),
        /* live_actual_move_f */ [&]() -> int {
            if (state_ != AppState::GAME_OVER || !analysis_cur_ || analysis_cur_->children.empty())
                return -1;
            return analysis_cur_->children[0]->move_col;
        }(),
        /* live_actual_move_score */ [&]() -> float {
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
        /* live_analysis_tree       */ analysis_tree_render_.empty() ? nullptr : analysis_tree_render_.data(),
        /* live_analysis_tree_count */ (int)analysis_tree_render_.size(),
        /* live_analysis_tree_cur_depth */ analysis_cur_ ? analysis_cur_->depth : 0,
        // Score graph
        /* live_score_graph     */ (state_ == AppState::GAME_OVER && !move_scores_.empty())
                                       ? move_scores_.data() : nullptr,
        /* live_score_graph_len */ (state_ == AppState::GAME_OVER) ? (int)move_scores_.size() : 0,
        /* live_score_graph_cur */ analysis_cur_ ? analysis_cur_->depth : 0,
    };
}

void App::draw() {
    if (state_ == AppState::MATCH_MENU) {
        renderer_->draw_match_menu(match_menu_);
        return;
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
    bool lt_down = false, rt_down = false;  // trigger axis states
    Uint32 js_prev_ms = SDL_GetTicks();    // for joystick dt calculation

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
        if (repeat_btn != 0xFF && repeat_next_ms > now)
            wait_ms = std::min(wait_ms, (int)(repeat_next_ms - now));
        if (demo_active_ && demo_.next_tick > now)
            wait_ms = std::min(wait_ms, (int)(demo_.next_tick - now));
        // Poll at 50 ms when the left stick is deflected so cursor moves smoothly
        if (state_ == AppState::PLAYING && game_.history_pos < 0 &&
            (std::abs(js_left_x_) > 8192 || std::abs(js_left_y_) > 8192))
            wait_ms = std::min(wait_ms, 50);

        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, wait_ms)) {
            do {
                if (e.type == SDL_QUIT) {
                    quit = true;

                } else if (e.type == g_net_event_type) {
                    NetMsg msg;
                    while (net_.poll_msg(msg))
                        handle_net_msg(msg);
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
                                if (is_local_game_ && !local_game_score_.empty())
                                    set_status(local_game_score_ + "  —  PRESS A FOR ANALYSIS");
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
                                    bg_analysis_busy_ = false;
                                    if (state_ == AppState::GAME_OVER) draw();
                                } else {
                                    fg_kata_pending_ = false;
                                    kata_score_lead_ = sl;
                                    // Persist score in the current analysis node and graph
                                    if (analysis_cur_ && sl != FLT_MAX) {
                                        analysis_cur_->score_lead = sl;
                                        int d = analysis_cur_->depth;
                                        if (d < (int)move_scores_.size())
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
                        } else if (repeat_btn == vbtn) {
                            repeat_btn = 0xFF;
                        }
                    };
                    if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
                        on_trigger(0xFD, lt_down, e.caxis.value);
                    else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
                        on_trigger(0xFE, rt_down, e.caxis.value);
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
                    // Keyboard shortcut mirrors for controller buttons:
                    else if (k == SDLK_RETURN) {
                        SDL_Event fake{};
                        fake.type = SDL_CONTROLLERBUTTONDOWN;
                        fake.cbutton.button = SDL_CONTROLLER_BUTTON_A;
                        handle_controller_button(fake.cbutton.button);
                    } else if (k == SDLK_p) {
                        handle_controller_button(SDL_CONTROLLER_BUTTON_B);
                    } else if (k == SDLK_r) {
                        handle_controller_button(SDL_CONTROLLER_BUTTON_START);
                    } else if (k == SDLK_f) {
                        handle_controller_button(SDL_CONTROLLER_BUTTON_START);
                    } else if (k == SDLK_UP) {
                        handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_UP);
                    } else if (k == SDLK_DOWN) {
                        handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
                    } else if (k == SDLK_LEFT) {
                        handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
                    } else if (k == SDLK_RIGHT) {
                        handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
                    } else if (k == SDLK_TAB) {
                        handle_controller_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
                    }

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

        // Left-stick joystick cursor (PLAYING state, live view only)
        if (state_ == AppState::PLAYING && game_.history_pos < 0) {
            const Sint16 DEAD  = 8192;
            const float  RANGE = (float)(32767 - DEAD);
            float dt = (float)(now - js_prev_ms) * 0.001f;
            if (dt > 0.2f) dt = 0.2f;  // clamp in case of long pause
            auto accum = [&](Sint16 val, float& acc) {
                if (std::abs(val) > DEAD) {
                    float spd = ((float)(std::abs(val) - DEAD) / RANGE) * 15.f * dt;
                    acc += (val > 0) ? spd : -spd;
                } else {
                    acc = 0.f;
                }
            };
            accum(js_left_x_, js_acc_x_);
            accum(js_left_y_, js_acc_y_);
            int dx = (int)js_acc_x_, dy = (int)js_acc_y_;
            // Diagonal assist: when one axis fires a full step, pull the other along
            // if it's already past halfway — gives natural diagonal movement.
            bool x_active = std::abs(js_left_x_) > DEAD;
            bool y_active = std::abs(js_left_y_) > DEAD;
            if (dx != 0 && y_active && dy == 0 && std::abs(js_acc_y_) >= 0.5f) {
                dy = (js_acc_y_ >= 0.f) ? 1 : -1;
                js_acc_y_ -= (float)dy;
            }
            if (dy != 0 && x_active && dx == 0 && std::abs(js_acc_x_) >= 0.5f) {
                dx = (js_acc_x_ >= 0.f) ? 1 : -1;
                js_acc_x_ -= (float)dx;
            }
            if (dx || dy) {
                int n = game_.board_size - 1;
                game_.cursor_f = std::max(0, std::min(n, game_.cursor_f + dx));
                game_.cursor_r = std::max(0, std::min(n, game_.cursor_r + dy));
                js_acc_x_ -= (float)dx;
                js_acc_y_ -= (float)dy;
            }
        } else {
            js_acc_x_ = js_acc_y_ = 0.f;
        }
        js_prev_ms = now;

        // Advance demo screensaver when idle (not in a live game)
        bool idle = (state_ == AppState::LOBBY || state_ == AppState::SEARCHING ||
                     state_ == AppState::CONNECTING);
        if (demo_active_ && idle && now >= demo_.next_tick) {
            if (demo_.pos < (int)demo_.rows.size()) {
                demo_.board.place_stone(demo_.rows[demo_.pos], demo_.cols[demo_.pos],
                                        demo_.colors[demo_.pos]);
                demo_.pos++;
                demo_.next_tick = now + 1000;
            } else {
                load_demo_game();  // finished — pick a new random game
            }
        }

        // Fire deferred KataGo query once the user has settled on a position
        if (kata_query_after_ > 0 && now >= kata_query_after_) {
            kata_query_after_ = 0;
            if (state_ == AppState::GAME_OVER && analysis_cur_ && kata_analysis_enabled_) {
                bg_analysis_busy_ = false;  // cancel any in-flight background query
                fg_kata_pending_  = true;
                kata_for(game_.board_size).query_moves(
                    analysis_cur_->board.board, game_.board_size,
                    analysis_cur_->board.turn_is_black == 1);
            }
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
                    hs.board, game_.board_size, hs.turn_is_black == 1, 7.5f, 50);
                bg_analysis_depth_ = bg_analysis_next_++;
                bg_analysis_busy_  = true;
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
        return EXCEPTION_CONTINUE_SEARCH;
    });
#endif
    App* app = new App();
    int r = app->run();
    delete app;
    return r;
}
