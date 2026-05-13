#include "go_viewer.hpp"
#include "go_rules.hpp"
#include "game_state.hpp"
#include "analysis_state.hpp"
#include "catalog.hpp"
#include "renderer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SGF parser (self-contained, no global state)

struct SgfGame {
    char  moves[MAX_MOVES][MOVE_TEXT_LEN] = {};
    int   colors[MAX_MOVES]               = {};
    int   move_count                      = 0;
    char  black_name[NAME_LEN]            = "Black";
    char  white_name[NAME_LEN]            = "White";
    char  result[RESULT_LEN]              = {};
    char  date[32]                        = {};
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
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return false;
    g.move_count   = 0;
    g.black_name[0] = '\0';
    g.white_name[0] = '\0';
    g.result[0]    = '\0';
    g.date[0]      = '\0';
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        // Moves
        const char* p = line;
        while (*p) {
            if (*p == ';' && *(p+1) == 'B' && *(p+2) == '[') {
                p += 3;
                const char* end = strchr(p, ']');
                if (end && g.move_count < MAX_MOVES) {
                    size_t len = (size_t)(end - p);
                    if (len >= MOVE_TEXT_LEN) len = MOVE_TEXT_LEN - 1;
                    memcpy(g.moves[g.move_count], p, len);
                    g.moves[g.move_count][len] = '\0';
                    g.colors[g.move_count] = 1;
                    g.move_count++;
                }
            } else if (*p == ';' && *(p+1) == 'W' && *(p+2) == '[') {
                p += 3;
                const char* end = strchr(p, ']');
                if (end && g.move_count < MAX_MOVES) {
                    size_t len = (size_t)(end - p);
                    if (len >= MOVE_TEXT_LEN) len = MOVE_TEXT_LEN - 1;
                    memcpy(g.moves[g.move_count], p, len);
                    g.moves[g.move_count][len] = '\0';
                    g.colors[g.move_count] = 0;
                    g.move_count++;
                }
            }
            p++;
        }
        // Properties
        auto extract = [&](const char* tag, char* dst, size_t dsz) {
            const char* s = strstr(line, tag);
            if (!s) return;
            s += strlen(tag);
            const char* e = strchr(s, ']');
            if (!e) return;
            size_t len = (size_t)(e - s);
            while (len > 0 && (s[len-1]==' '||s[len-1]=='\t'||s[len-1]=='\r'||s[len-1]=='\n')) len--;
            while (len > 0 && (*s==' '||*s=='\t')) { s++; len--; }
            if (len >= dsz) len = dsz - 1;
            if (len > 0) { memcpy(dst, s, len); dst[len] = '\0'; }
        };
        extract("PB[", g.black_name, sizeof(g.black_name));
        extract("PW[", g.white_name, sizeof(g.white_name));
        if (!strstr(line, "PW[")) extract("pw[", g.white_name, sizeof(g.white_name));
        extract("RE[", g.result, sizeof(g.result));
        extract("DT[", g.date,   sizeof(g.date));
    }
    fclose(fp);
    if (g.black_name[0] == '\0') strcpy(g.black_name, "Black");
    if (g.white_name[0] == '\0') strcpy(g.white_name, "White");
    return g.move_count > 0;
}

// ---------------------------------------------------------------------------
// Territory estimation drill

struct TerritoryProblem {
    char board[BOARD_SIZE][BOARD_SIZE] = {};
    int  black_score = 0;
    int  white_score = 0;
    bool answered    = false;
    bool correct     = false;
};

// shape: 0=compact, 1=horizontal strip, 2=vertical strip
static void grow_blob(int r0, int c0, int r1, int c1, int target,
                      bool out[BOARD_SIZE][BOARD_SIZE], int shape = 0) {
    memset(out, 0, BOARD_SIZE * BOARD_SIZE * sizeof(bool));
    int sr = r0 + rand() % (r1 - r0 + 1);
    int sc = c0 + rand() % (c1 - c0 + 1);
    std::vector<std::pair<int,int>> cells;
    out[sr][sc] = true;
    cells.push_back({sr, sc});
    // dirs: up, down, left, right — weights vary by shape
    const int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    const int v_weight = (shape == 2) ? 6 : 1;  // up/down
    const int h_weight = (shape == 1) ? 6 : 1;  // left/right
    const int total_w  = 2*v_weight + 2*h_weight;
    for (int attempts = 0; (int)cells.size() < target && attempts < target * 100; attempts++) {
        auto& cell = cells[rand() % cells.size()];
        int rw = rand() % total_w;
        int d = (rw < v_weight) ? 0 : (rw < 2*v_weight) ? 1 : (rw < 2*v_weight + h_weight) ? 2 : 3;
        int nr = cell.first + dirs[d][0], nc = cell.second + dirs[d][1];
        if (nr >= r0 && nr <= r1 && nc >= c0 && nc <= c1 && !out[nr][nc]) {
            out[nr][nc] = true;
            cells.push_back({nr, nc});
        }
    }
}

static TerritoryProblem generate_territory_problem() {
    const int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    TerritoryProblem prob;
    for (;;) {
        memset(prob.board, 0, sizeof(prob.board));
        prob.black_score = prob.white_score = 0;

        // Sizes: randomise independently but keep them broadly comparable
        int base   = 15 + rand() % 26;          // 15-40
        int bsize  = base - rand() % 8;
        int wsize  = base - rand() % 8;
        if (bsize < 8) bsize = 8;
        if (wsize < 8) wsize = 8;

        // Pick two different shapes (0=compact, 1=horizontal, 2=vertical)
        int shape1 = rand() % 3;
        int shape2 = (shape1 + 1 + rand() % 2) % 3;

        // Black blob in upper strip, white in lower strip (row 9 gap prevents overlap)
        // Ranges reach the board edges so corner/edge territories are generated naturally
        bool bblob[BOARD_SIZE][BOARD_SIZE];
        bool wblob[BOARD_SIZE][BOARD_SIZE];
        grow_blob(0, 0, 7, 18, bsize, bblob, shape1);
        grow_blob(11, 0, 18, 18, wsize, wblob, shape2);

        // Pass 1: orthogonal border (always placed — these fully enclose the blob)
        for (int r = 0; r < BOARD_SIZE; r++) {
            for (int c = 0; c < BOARD_SIZE; c++) {
                if (bblob[r][c]) {
                    for (auto& d : dirs) {
                        int nr=r+d[0], nc=c+d[1];
                        if (nr<0||nr>=BOARD_SIZE||nc<0||nc>=BOARD_SIZE) continue;
                        if (!bblob[nr][nc] && prob.board[nr][nc] == 0)
                            prob.board[nr][nc] = 1;
                    }
                }
                if (wblob[r][c]) {
                    for (auto& d : dirs) {
                        int nr=r+d[0], nc=c+d[1];
                        if (nr<0||nr>=BOARD_SIZE||nc<0||nc>=BOARD_SIZE) continue;
                        if (!wblob[nr][nc] && prob.board[nr][nc] == 0)
                            prob.board[nr][nc] = 2;
                    }
                }
            }
        }

        // Pass 2: diagonal corner fills — placed randomly, but no two adjacent
        // omissions (which would create a false eye).
        const int diags[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};

        auto add_corner_fills = [&](bool blob[BOARD_SIZE][BOARD_SIZE], char val) {
            // Collect candidates: diagonal-only blob neighbours not yet filled
            std::vector<std::pair<int,int>> cands;
            for (int r = 0; r < BOARD_SIZE; r++) {
                for (int c = 0; c < BOARD_SIZE; c++) {
                    if (blob[r][c] || prob.board[r][c] != 0) continue;
                    bool diag_blob = false;
                    for (auto& d : diags) {
                        int nr=r+d[0], nc=c+d[1];
                        if (nr>=0&&nr<BOARD_SIZE&&nc>=0&&nc<BOARD_SIZE&&blob[nr][nc])
                            { diag_blob=true; break; }
                    }
                    if (diag_blob) cands.push_back({r, c});
                }
            }
            // Shuffle so exclusions are spread randomly
            for (int i=(int)cands.size()-1; i>0; i--) {
                int j = rand()%(i+1); std::swap(cands[i], cands[j]);
            }
            bool omitted[BOARD_SIZE][BOARD_SIZE] = {};
            for (auto& [r, c] : cands) {
                // Try to omit with ~35% probability
                if (rand() % 10 < 4) {
                    // Only omit if no orthogonally adjacent corner is also omitted
                    bool adj_omit = false;
                    for (auto& d : dirs) {
                        int nr=r+d[0], nc=c+d[1];
                        if (nr>=0&&nr<BOARD_SIZE&&nc>=0&&nc<BOARD_SIZE&&omitted[nr][nc])
                            { adj_omit=true; break; }
                    }
                    if (!adj_omit) { omitted[r][c]=true; continue; }
                }
                prob.board[r][c] = val;
            }
        };

        add_corner_fills(bblob, 1);
        add_corner_fills(wblob, 2);

        // Scatter interior stones (same colour) to reduce apparent territory
        for (int r = 0; r < BOARD_SIZE; r++) {
            for (int c = 0; c < BOARD_SIZE; c++) {
                if (bblob[r][c] && prob.board[r][c] == 0 && rand() % 5 == 0)
                    prob.board[r][c] = 1;
                if (wblob[r][c] && prob.board[r][c] == 0 && rand() % 5 == 0)
                    prob.board[r][c] = 2;
            }
        }

        // Count empty interior points
        for (int r = 0; r < BOARD_SIZE; r++) {
            for (int c = 0; c < BOARD_SIZE; c++) {
                if (bblob[r][c] && prob.board[r][c] == 0) prob.black_score++;
                if (wblob[r][c] && prob.board[r][c] == 0) prob.white_score++;
            }
        }

        if (prob.black_score != prob.white_score) break;  // reject ties
    }
    return prob;
}

// ---------------------------------------------------------------------------
// Application

enum NavRequest { NAV_NONE, NAV_NEXT, NAV_PREV, NAV_RESTART, NAV_SELECT };

class App {
public:
    int run(int argc, char* argv[]);

private:
    // SDL
    SDL_Window*   window   = nullptr;
    SDL_Renderer* sdl_rend = nullptr;
    SDL_Cursor*   stone_cursors[2] = {nullptr, nullptr}; // [0]=white, [1]=black
    int           cursor_square    = 0;                  // square size used when cursors were last built

    // Core state
    GameState                      game;
    std::unique_ptr<AnalysisState> analysis; // non-null only while in analysis mode

    // File browser
    Catalog catalog;

    // Rendering
    Renderer* renderer = nullptr;

    // Current game data
    SgfGame sgf;
    int     game_index = 0; // current move index within sgf.moves[]

    // Playback controls
    int    move_delay_ms       = MOVE_DELAY_MS;
    bool   game_mode           = false;
    bool   guess_mode          = false;
    int    guess_score         = 0;
    bool   territory_drill_active = false;
    std::unique_ptr<TerritoryProblem> territory_problem;
    bool   chain_mode          = false;
    bool   show_help           = false;
    bool   cursor_visible      = true;
    Uint32 last_mouse_activity = 0;
    Uint32 speed_message_until = 0;
    bool   suppress_present    = false;
    std::string result_message;
    std::string game_date;
    std::string black_name, white_name;
    std::string games_dir;
    std::string forced_path;  // set by catalog selection

    // Navigation
    NavRequest nav_request     = NAV_NONE;

    // Helpers
    bool in_analysis() const { return analysis != nullptr; }

    void enter_analysis() {
        // Take a clean snapshot of game state and give it to AnalysisState.
        // GameState is never touched again until exit_analysis().
        analysis = std::make_unique<AnalysisState>(game.take_snapshot());
        // Analysis always starts with black's turn in the C original; replicate that.
        analysis->turn_is_black = 1;
        game.liberty_count = 0;
        game.selected_group_count = 0;
    }

    void exit_analysis() {
        // Simply destroy the analysis state.  The game board is exactly as it was.
        analysis.reset();
        game.liberty_count = 0;
        game.selected_group_count = 0;
    }

    void enter_game_mode() {
        if (in_analysis()) exit_analysis();
        guess_mode = false;
        GameSnapshot empty{};          // blank board, black goes first
        analysis = std::make_unique<AnalysisState>(empty);
        analysis->turn_is_black = 1;
        game.liberty_count = 0;
        game.selected_group_count = 0;
        game_mode   = true;
        black_name  = "Black";
        white_name  = "White";
    }

    void enter_territory_drill() {
        if (game_mode)      exit_game_mode();
        if (in_analysis())  exit_analysis();
        guess_mode = false;
        territory_drill_active = true;
        territory_problem = std::make_unique<TerritoryProblem>(generate_territory_problem());
    }

    void exit_territory_drill() {
        territory_drill_active = false;
        territory_problem.reset();
    }

    void exit_game_mode() {
        analysis.reset();
        game.liberty_count = 0;
        game.selected_group_count = 0;
        game_mode  = false;
        black_name = sgf.black_name;
        white_name = sgf.white_name;
    }

    bool init();
    void cleanup();

    // Returns true if the game loop should quit the application entirely.
    bool play_current_game();

    // Input handling
    void handle_key(SDL_Keycode key, const Uint8* kb_state, bool& quit);
    void handle_analysis_lclick(const BoardView& view, int mx, int my, const Uint8* kb);
    void handle_analysis_rclick(const BoardView& view, int mx, int my);
    void handle_playback_lclick(const BoardView& view, int mx, int my);
    void handle_guess_lclick(const BoardView& view, int mx, int my,
                             bool& guess_pending, int& guess_r, int& guess_f);

    bool animate_move(int r, int f, int is_black);

    int  adjust_move_delay(int delta_ms, Uint32 now);
    void set_cursor_visible(bool v);
    void sync_cursor();
    void note_mouse_activity(Uint32 now);
    void note_mouse_activity_event(const SDL_Event& e);
    void update_cursor_auto_hide(Uint32 now);

    SDL_Cursor* create_stone_cursor(bool is_black, int square);

    Renderer::DrawState make_draw_state() {
        const TerritoryProblem* tp = territory_problem.get();
        return Renderer::DrawState{
            game,
            analysis.get(),
            in_analysis(),
            game_mode,
            guess_mode,
            guess_score,
            chain_mode,
            show_help,
            catalog,
            black_name,
            white_name,
            result_message,
            game_date,
            move_delay_ms,
            speed_message_until,
            suppress_present,
            territory_drill_active,
            tp ? tp->board : nullptr,
            tp ? tp->black_score : 0,
            tp ? tp->white_score : 0,
            tp ? tp->answered    : false,
            tp ? tp->correct     : false,
        };
    }

    void draw_board() {
        sync_cursor();
        auto ds = make_draw_state();
        renderer->draw_board(ds);
    }

    // Pick a file to load, respecting sequential / random / forced selection
    std::string pick_next_file(NavRequest req);
};

// ---------------------------------------------------------------------------
// SDL cursor

SDL_Cursor* App::create_stone_cursor(bool is_black, int square) {
    const int sz     = square;
    const int mid    = sz / 2;
    const float radius = (float)(square / 2 - 2);
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, sz, sz, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;
    if (SDL_LockSurface(surf) != 0) { SDL_FreeSurface(surf); return nullptr; }
    Uint32* px    = (Uint32*)surf->pixels;
    int     pitch = surf->pitch / 4;
    Uint8 r = is_black ?  30 : 240;
    Uint8 g = is_black ?  30 : 240;
    Uint8 b = is_black ?  30 : 240;
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            float dx = x - mid + 0.5f, dy = y - mid + 0.5f;
            float dist = sqrtf(dx*dx + dy*dy);
            // smoothstep over a 1-pixel band at the edge
            float t = radius + 0.5f - dist;
            Uint8 a = (t <= 0.0f) ? 0 : (t >= 1.0f) ? 255 : (Uint8)(t * 255.0f);
            px[y*pitch+x] = SDL_MapRGBA(surf->format, r, g, b, a);
        }
    }
    SDL_UnlockSurface(surf);
    SDL_Cursor* c = SDL_CreateColorCursor(surf, mid, mid);
    SDL_FreeSurface(surf);
    return c;
}

void App::sync_cursor() {
    // Rebuild cursors if the board square size changed (e.g. window resize or different monitor)
    BoardView view; renderer->get_board_view(view);
    if (view.square != cursor_square && view.square > 0) {
        for (int i = 0; i < 2; i++) {
            if (stone_cursors[i]) { SDL_FreeCursor(stone_cursors[i]); stone_cursors[i] = nullptr; }
        }
        // Cursors use logical pixels; the renderer uses physical pixels (SDL_GetRendererOutputSize).
        // Divide by the DPI scale so the cursor matches the visual stone size on HiDPI screens.
        int win_w = 1, win_h = 1;
        SDL_GetWindowSize(window, &win_w, &win_h);
        int out_w = 1;
        SDL_GetRendererOutputSize(sdl_rend, &out_w, nullptr);
        float dpi = (win_w > 0) ? (float)out_w / win_w : 1.0f;
        int csz = std::max(8, (int)(view.square / dpi));
        stone_cursors[0] = create_stone_cursor(false, csz);
        stone_cursors[1] = create_stone_cursor(true,  csz);
        cursor_square = view.square;
    }

    if ((in_analysis() || game_mode || guess_mode) && cursor_visible) {
        const Uint8* kb = SDL_GetKeyboardState(nullptr);
        int is_black;
        if (in_analysis() && kb[SDL_SCANCODE_B])      is_black = 1;
        else if (in_analysis() && kb[SDL_SCANCODE_W]) is_black = 0;
        else if (in_analysis() || game_mode)          is_black = analysis ? analysis->turn_is_black : 1;
        else                                          is_black = game.turn_is_black;
        SDL_Cursor* c = stone_cursors[is_black ? 1 : 0];
        if (c) SDL_SetCursor(c);
    } else {
        SDL_SetCursor(SDL_GetDefaultCursor());
    }
}

void App::set_cursor_visible(bool v) {
    SDL_ShowCursor(v ? SDL_ENABLE : SDL_DISABLE);
    cursor_visible = v;
    sync_cursor();
}

void App::note_mouse_activity(Uint32 now) {
    last_mouse_activity = now;
    if (!cursor_visible) set_cursor_visible(true);
}

void App::note_mouse_activity_event(const SDL_Event& e) {
    if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN ||
        e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEWHEEL)
        note_mouse_activity(SDL_GetTicks());
}

void App::update_cursor_auto_hide(Uint32 /*now*/) {
    if (!cursor_visible) set_cursor_visible(true);
}

int App::adjust_move_delay(int delta_ms, Uint32 now) {
    int nd = move_delay_ms + delta_ms;
    if (nd < MOVE_DELAY_MIN_MS) nd = MOVE_DELAY_MIN_MS;
    if (nd > MOVE_DELAY_MAX_MS) nd = MOVE_DELAY_MAX_MS;
    if (nd == move_delay_ms) return 0;
    move_delay_ms      = nd;
    speed_message_until = now + SPEED_MESSAGE_MS;
    return 1;
}

// ---------------------------------------------------------------------------
// init / cleanup

bool App::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    window = SDL_CreateWindow("Go Viewer",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              SCREEN_SIZE, SCREEN_SIZE,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    sdl_rend = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window || !sdl_rend) return false;
    renderer = new Renderer(sdl_rend);
    set_cursor_visible(true);
    note_mouse_activity(SDL_GetTicks());
    srand((unsigned int)time(nullptr));
    return true;
}

void App::cleanup() {
    delete renderer; renderer = nullptr;
    for (int i = 0; i < 2; i++) {
        if (stone_cursors[i]) { SDL_FreeCursor(stone_cursors[i]); stone_cursors[i] = nullptr; }
    }
    if (sdl_rend) { SDL_DestroyRenderer(sdl_rend); sdl_rend = nullptr; }
    if (window)   { SDL_DestroyWindow(window);      window   = nullptr; }
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Game-level logic

bool App::animate_move(int r, int f, int is_black) {
    if (!game.place_stone(r, f, is_black)) return false;
    game.save_snapshot();
    draw_board();
    return true;
}

// ---------------------------------------------------------------------------
// Input handlers

void App::handle_key(SDL_Keycode key, const Uint8* /*kb*/, bool& quit) {
    Uint32 now = SDL_GetTicks();

    if (key == SDLK_q) {
        nav_request = NAV_NONE; quit = true; return;
    }

    // Territory drill intercepts most keys
    if (territory_drill_active) {
        if (key == SDLK_t) {
            exit_territory_drill(); draw_board();
        } else if (!territory_problem->answered) {
            if (key == SDLK_b || key == SDLK_w) {
                bool user_says_black = (key == SDLK_b);
                bool black_bigger    = territory_problem->black_score > territory_problem->white_score;
                territory_problem->answered = true;
                territory_problem->correct  = (user_says_black == black_bigger);
                draw_board();
            }
        } else if (key == SDLK_SPACE || key == SDLK_RETURN) {
            territory_problem = std::make_unique<TerritoryProblem>(generate_territory_problem());
            draw_board();
        }
        return;
    }

    if (key == SDLK_t) {
        enter_territory_drill(); draw_board(); return;
    }

    if (key == SDLK_n) { nav_request = NAV_NEXT;    quit = true; return; }
    if (key == SDLK_r) { nav_request = NAV_RESTART; quit = true; return; }

    if (key == SDLK_c) {
        catalog.open(games_dir);
        draw_board();
        return;
    }
    if (key == SDLK_ESCAPE) {
        show_help = !show_help;
        draw_board();
        return;
    }

    if (key == SDLK_UP || key == SDLK_DOWN) {
        int delta = (key == SDLK_UP) ? MOVE_DELAY_STEP_MS : -MOVE_DELAY_STEP_MS;
        if (adjust_move_delay(delta, now))
            draw_board();
        return;
    }

    // Space and A both toggle analysis mode
    if (key == SDLK_SPACE || key == SDLK_a) {
        if (game_mode) {
            // Space/A exits game mode back to playback
            exit_game_mode();
        } else if (in_analysis()) {
            exit_analysis();
        } else {
            if (guess_mode) { guess_mode = false; }
            enter_analysis();
        }
        set_cursor_visible(true);
        draw_board();
        return;
    }

    if (key == SDLK_g) {
        if (guess_mode) {
            guess_mode = false;
            game.liberty_count = 0;
        } else {
            if (in_analysis()) exit_analysis();
            if (game_mode) exit_game_mode();
            guess_mode  = true;
            guess_score = 0;
        }
        draw_board();
        return;
    }

    if (key == SDLK_p) {
        if (game_mode)
            exit_game_mode();
        else
            enter_game_mode();
        draw_board();
        return;
    }

    if (key == SDLK_u) {
        chain_mode = !chain_mode;
        draw_board();
        return;
    }

    if (!guess_mode) {
        if (key == SDLK_LEFT && game_index > 0) {
            if (in_analysis()) exit_analysis();
            game_index--;
            game.restore_snapshot(game_index);
            draw_board();
            return;
        }
        if (key == SDLK_RIGHT && game_index < sgf.move_count) {
            if (in_analysis()) exit_analysis();
            int r, f;
            if (parse_sgf_move(sgf.moves[game_index], r, f)) {
                bool is_black = (sgf.colors[game_index] == 1);
                if (animate_move(r, f, is_black)) {
                    game_index++;
                    if (game_index < sgf.move_count)
                        game.turn_is_black = sgf.colors[game_index];
                }
            } else {
                game_index++;
            }
            draw_board();
            return;
        }
    }
}

void App::handle_analysis_lclick(const BoardView& view, int mx, int my, const Uint8* kb) {
    int r = -1, f = -1;
    if (!renderer->screen_to_board(view, mx, my, r, f)) return;
    if (!analysis) return;

    if (analysis->board[r][f] != 0) {
        // Toggle liberty display for the clicked stone's chain
        if (analysis->is_stone_in_selected_group(r, f)) {
            analysis->selected_group_count = 0;
            analysis->liberty_count        = 0;
        } else {
            analysis->store_selected_group(r, f);
            analysis->calculate_chain_liberties(r, f);
        }
        draw_board();
        return;
    }

    // Place a new stone
    int forced_color = -1;
    if (kb[SDL_SCANCODE_B]) forced_color = 1;
    if (kb[SDL_SCANCODE_W]) forced_color = 0;
    int stone_color = (forced_color != -1) ? forced_color : analysis->turn_is_black;
    if (analysis->place_stone(r, f, stone_color)) {
        if (forced_color == -1)
            analysis->turn_is_black = !analysis->turn_is_black;
        analysis->liberty_count        = 0;
        analysis->liberty_display_r    = -1;
        analysis->liberty_display_f    = -1;
        analysis->selected_group_count = 0;
    }
    draw_board();
}

void App::handle_analysis_rclick(const BoardView& view, int mx, int my) {
    int r = -1, f = -1;
    if (!renderer->screen_to_board(view, mx, my, r, f)) return;
    if (!analysis) return;
    if (analysis->remove_stone_at(r, f)) {
        analysis->liberty_count        = 0;
        analysis->liberty_display_r    = -1;
        analysis->liberty_display_f    = -1;
        analysis->selected_group_count = 0;
        draw_board();
    }
}

void App::handle_playback_lclick(const BoardView& view, int mx, int my) {
    int r = -1, f = -1;
    if (!renderer->screen_to_board(view, mx, my, r, f)) return;
    if (game.board[r][f] == 0) return;
    if (game.is_stone_in_selected_group(r, f)) {
        game.selected_group_count = 0;
        game.liberty_count        = 0;
    } else {
        game.store_selected_group(r, f);
        game.calculate_chain_liberties(r, f);
    }
    draw_board();
}

void App::handle_guess_lclick(const BoardView& view, int mx, int my,
                              bool& guess_pending, int& gr, int& gf) {
    int r = -1, f = -1;
    if (!renderer->screen_to_board(view, mx, my, r, f)) return;
    if (game.board[r][f] == 0) {
        gr = r; gf = f;
        guess_pending = true;
    }
}

// ---------------------------------------------------------------------------
// File selection

std::string App::pick_next_file(NavRequest req) {
    // Forced selection from catalog
    if (!forced_path.empty() && req == NAV_SELECT) {
        std::string p = forced_path;
        forced_path.clear();
        return p;
    }

    std::vector<std::string> files;
    std::string dir;

    if (!catalog.sequential_dir.empty()) {
        dir = catalog.sequential_dir;
        Catalog::list_sgf_files(dir, files);
        if (files.empty()) {
            catalog.sequential_dir   = "";
            catalog.sequential_index = 0;
        }
    }

    if (files.empty()) {
        dir = games_dir;
        Catalog::list_sgf_files(dir, files);
    }
    if (files.empty()) return {};

    int idx = 0;
    if (!catalog.sequential_dir.empty()) {
        idx = catalog.sequential_index % (int)files.size();
        catalog.sequential_index++;
    } else {
        if (req == NAV_PREV) {
            // not tracked in random mode — just pick random
        }
        idx = rand() % (int)files.size();
    }

    // Build full path (files[] may already be relative to dir)
    std::string p = files[idx];
    if (p.size() < 2 || (p[0] != '/' && p[1] != ':' && p[0] != '\\'))
        p = dir + PATH_SEP_STR + p;
    return p;
}

// ---------------------------------------------------------------------------
// Main game loop

bool App::play_current_game() {
    result_message = sgf.result;
    game_date      = sgf.date;
    black_name     = sgf.black_name;
    white_name     = sgf.white_name;

    game.reset();
    game_index = 0;
    nav_request = NAV_NONE;
    if (territory_drill_active) exit_territory_drill();
    if (game_mode) exit_game_mode();
    else if (in_analysis()) exit_analysis();
    guess_mode  = false;
    guess_score = 0;
    draw_board();

    bool quit          = false;
    bool guess_pending = false;
    int  guess_r = -1, guess_f = -1;
    Uint32 last_move_tick = SDL_GetTicks();

    while (!quit) {
        Uint32 now = SDL_GetTicks();
        update_cursor_auto_hide(now);

        // Set current turn from SGF
        if (game_index < sgf.move_count)
            game.turn_is_black = sgf.colors[game_index];

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            note_mouse_activity_event(e);

            // Catalog events intercept everything
            if (catalog.active) {
                if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode key = e.key.keysym.sym;
                    int total = (int)catalog.entries.size();
                    if (key == SDLK_ESCAPE || key == SDLK_c) {
                        catalog.close();
                        draw_board();
                    } else if (key == SDLK_UP && catalog.index > 0) {
                        catalog.index--; draw_board();
                    } else if (key == SDLK_DOWN && catalog.index < total - 1) {
                        catalog.index++; draw_board();
                    } else if (key == SDLK_PAGEUP) {
                        catalog.index = (catalog.index > 6) ? catalog.index - 6 : 0;
                        draw_board();
                    } else if (key == SDLK_PAGEDOWN) {
                        catalog.index = (catalog.index + 6 < total) ? catalog.index + 6 : total - 1;
                        draw_board();
                    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                        catalog.select();
                        if (catalog.selection_made) {
                            forced_path  = catalog.selected_path;
                            catalog.selection_made = false;
                            nav_request  = NAV_SELECT;
                            quit         = true;
                        }
                        draw_board();
                    }
                }
                continue;
            }

            if (e.type == SDL_QUIT) {
                nav_request = NAV_NONE; quit = true;
            } else if (e.type == SDL_KEYDOWN) {
                const Uint8* kb = SDL_GetKeyboardState(nullptr);
                handle_key(e.key.keysym.sym, kb, quit);
            } else if (in_analysis() && e.type == SDL_MOUSEBUTTONDOWN) {
                BoardView view; renderer->get_board_view(view);
                const Uint8* kb = SDL_GetKeyboardState(nullptr);
                if (e.button.button == SDL_BUTTON_LEFT)
                    handle_analysis_lclick(view, e.button.x, e.button.y, kb);
                else if (e.button.button == SDL_BUTTON_RIGHT)
                    handle_analysis_rclick(view, e.button.x, e.button.y);
            } else if (!in_analysis() && !guess_mode && e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_LEFT) {
                BoardView view; renderer->get_board_view(view);
                handle_playback_lclick(view, e.button.x, e.button.y);
            } else if (guess_mode && !in_analysis() && e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_LEFT) {
                BoardView view; renderer->get_board_view(view);
                handle_guess_lclick(view, e.button.x, e.button.y, guess_pending, guess_r, guess_f);
            }
        }
        if (quit) break;

        // Guess mode: compare and advance
        if (guess_pending && game_index < sgf.move_count) {
            int er, ef;
            if (parse_sgf_move(sgf.moves[game_index], er, ef)) {
                if (er == guess_r && ef == guess_f) guess_score += 5;
                else                                guess_score -= 5;
                bool is_black = (sgf.colors[game_index] == 1);
                animate_move(er, ef, is_black);
                game_index++;
                if (game_index < sgf.move_count) game.turn_is_black = sgf.colors[game_index];
                last_move_tick = SDL_GetTicks();
            }
            guess_pending = false;
        }

        // Analysis and guess modes freeze auto-advance
        if (in_analysis() || guess_mode) {
            draw_board();
            SDL_Delay(10);
            continue;
        }

        // Auto-advance playback
        if (!in_analysis() && !territory_drill_active && game_index < sgf.move_count) {
            if (now - last_move_tick >= (Uint32)move_delay_ms) {
                int r, f;
                if (parse_sgf_move(sgf.moves[game_index], r, f)) {
                    bool is_black = (sgf.colors[game_index] == 1);
                    animate_move(r, f, is_black);
                }
                game_index++;
                if (game_index < sgf.move_count) game.turn_is_black = sgf.colors[game_index];
                last_move_tick = now;
            }
        } else if (game_index >= sgf.move_count) {
            game.game_finished = 1;
            if (game.game_finished_timer == 0)
                game.game_finished_timer = SDL_GetTicks();
            draw_board();
            if (SDL_GetTicks() - game.game_finished_timer >= GAME_OVER_PAUSE_MS) {
                nav_request = NAV_NEXT;
                quit = true;
            }
        }
        SDL_Delay(10);
    }

    return quit && nav_request == NAV_NONE;
}

// ---------------------------------------------------------------------------
// Top-level run

int App::run(int /*argc*/, char* /*argv*/[]) {
    games_dir = DEFAULT_GAMES_DIR;

    // If test.sgf exists, load it first
    if (FILE* fp = fopen("test.sgf", "r")) {
        fclose(fp);
        forced_path = "test.sgf";
        nav_request = NAV_SELECT;
    }

    if (!init()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Go Viewer",
            "Failed to initialise SDL.", nullptr);
        return 1;
    }

    bool quit = false;
    while (!quit) {
        std::string path;

        if (!forced_path.empty()) {
            path = forced_path;
            forced_path.clear();
        } else {
            path = pick_next_file(nav_request);
        }
        nav_request = NAV_NONE;

        if (path.empty()) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "No SGF files found.\n\nLooked in: %s\n\n"
                     "Place .sgf files in a 'games/' folder next to the executable,\n"
                     "or put a test.sgf next to the executable.",
                     games_dir.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Go Viewer", msg, window);
            break;
        }

        if (!load_sgf(path, sgf)) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Failed to load:\n%s", path.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Go Viewer", msg, window);
            SDL_Delay(500);
            continue;
        }

        bool should_quit = play_current_game();
        if (should_quit) { quit = true; break; }

        if (nav_request == NAV_SELECT && !forced_path.empty()) {
            // Another catalog selection happened inside play_current_game — loop back
            continue;
        }
        if (nav_request == NAV_RESTART) {
            // Reload the same file — path is already gone, so re-set forced_path
            forced_path = path;
        }
    }

    cleanup();
    return 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    App* app = new App();
    int r = app->run(argc, argv);
    delete app;
    return r;
}
