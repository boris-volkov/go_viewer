#include "renderer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Font data (5x7 pixel glyphs, MSB = leftmost column)

const Renderer::Glyph Renderer::font_glyphs[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x00,0x04}},
    {',', {0x00,0x00,0x00,0x00,0x00,0x04,0x08}},
    {'\'',{0x04,0x04,0x00,0x00,0x00,0x00,0x00}},
    {'/', {0x01,0x02,0x04,0x08,0x10,0x00,0x00}},
    {'\\',{0x10,0x08,0x04,0x02,0x01,0x00,0x00}},
    {'[', {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
    {']', {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
    {'_', {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
    {'(', {0x04,0x08,0x10,0x10,0x10,0x08,0x04}},
    {')', {0x04,0x02,0x01,0x01,0x01,0x02,0x04}},
    {':', {0x00,0x04,0x00,0x00,0x04,0x00,0x00}},
    {'?', {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'A', {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x10,0x13,0x11,0x0F}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x01,0x01,0x01,0x01,0x01,0x11,0x0E}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
};

const unsigned char* Renderer::get_glyph_rows(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (const auto& g : font_glyphs)
        if (g.c == c) return g.rows;
    return font_glyphs[9].rows; // '?'
}

// ---------------------------------------------------------------------------
// Board view / coordinate helpers

void Renderer::get_board_view(BoardView& view) const {
    int w = SCREEN_SIZE, h = SCREEN_SIZE;
    SDL_GetRendererOutputSize(sdl, &w, &h);
    int min_dim  = (w < h) ? w : h;
    view.square  = min_dim / BOARD_SIZE;
    if (view.square < 1) view.square = 1;
    view.board_px = view.square * BOARD_SIZE;
    view.offset_x = (w - view.board_px) / 2;
    view.offset_y = (h - view.board_px) / 2;
    view.screen_w = w;
    view.screen_h = h;
}

void Renderer::board_to_screen(const BoardView& view, int br, int bf, int& x, int& y) const {
    x = view.offset_x + bf * view.square;
    y = view.offset_y + br * view.square;
}

bool Renderer::screen_to_board(const BoardView& view, int mx, int my, int& r, int& f) const {
    if (mx < view.offset_x || my < view.offset_y) return false;
    if (mx >= view.offset_x + view.board_px || my >= view.offset_y + view.board_px) return false;
    int rel_x = mx - view.offset_x;
    int rel_y = my - view.offset_y;
    int bf = rel_x / view.square;
    int br = rel_y / view.square;
    if (br < 0 || br >= BOARD_SIZE || bf < 0 || bf >= BOARD_SIZE) return false;
    int inset   = view.square / 8;
    int local_x = rel_x - bf * view.square;
    int local_y = rel_y - br * view.square;
    if (local_x < inset || local_x >= view.square - inset) return false;
    if (local_y < inset || local_y >= view.square - inset) return false;
    r = br; f = bf;
    return true;
}

// ---------------------------------------------------------------------------
// Primitive drawing

int Renderer::text_width_px(const char* text, int scale) const {
    int len = (int)strlen(text);
    if (len <= 0) return 0;
    return (len * 6 - 1) * scale;
}

void Renderer::draw_text(int x, int y, int scale, const char* text, SDL_Color color) {
    SDL_SetRenderDrawColor(sdl, color.r, color.g, color.b, color.a);
    int pen_x = x;
    for (const char* p = text; *p; p++) {
        const unsigned char* rows = get_glyph_rows(*p);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (rows[row] & (1 << (4 - col))) {
                    SDL_Rect rect = {pen_x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(sdl, &rect);
                }
            }
        }
        pen_x += 6 * scale;
    }
}

void Renderer::draw_color_swatch(int x, int y, int size, SDL_Color fill, SDL_Color outline) {
    SDL_Rect rect = {x, y, size, size};
    SDL_SetRenderDrawColor(sdl, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(sdl, &rect);
    SDL_SetRenderDrawColor(sdl, outline.r, outline.g, outline.b, outline.a);
    SDL_RenderDrawRect(sdl, &rect);
}

void Renderer::draw_thick_line(int x1, int y1, int x2, int y2, int thickness, SDL_Color color) {
    if (thickness < 1) thickness = 1;
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    float dx  = (float)(x2 - x1);
    float dy  = (float)(y2 - y1);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) {
        SDL_SetRenderDrawColor(sdl, color.r, color.g, color.b, color.a);
        SDL_RenderDrawPoint(sdl, x1, y1);
        return;
    }
    float nx   = dy / len;
    float ny   = -dx / len;
    float half = (float)thickness * 0.5f;
    float ox   = nx * half;
    float oy   = ny * half;
    SDL_Vertex verts[4];
    verts[0].position = {(float)x1 + ox, (float)y1 + oy};
    verts[1].position = {(float)x1 - ox, (float)y1 - oy};
    verts[2].position = {(float)x2 - ox, (float)y2 - oy};
    verts[3].position = {(float)x2 + ox, (float)y2 + oy};
    for (int i = 0; i < 4; i++) {
        verts[i].color     = color;
        verts[i].tex_coord = {0.f, 0.f};
    }
    int indices[6] = {0, 1, 2, 2, 3, 0};
    SDL_RenderGeometry(sdl, nullptr, verts, 4, indices, 6);
}

void Renderer::draw_stone_circle(const BoardView& view, int r, int f, int is_black, Uint8 alpha) {
    int bx = view.offset_x + f * view.square;
    int by = view.offset_y + r * view.square;
    int cx = bx + view.square / 2;
    int cy = by + view.square / 2;
    int radius = view.square / 2 - 2;
    SDL_SetRenderDrawBlendMode(sdl, alpha < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl,
        is_black ? 30  : 240,
        is_black ? 30  : 240,
        is_black ? 30  : 240,
        alpha);
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= radius * radius)
                SDL_RenderDrawPoint(sdl, cx + dx, cy + dy);
        }
    }
}

// ---------------------------------------------------------------------------
// Overlay helpers

void Renderer::render_chain_connections(const BoardView& view, const char board[][BOARD_SIZE],
                                        bool chain_mode) {
    if (!chain_mode) return;
    int drawn[BOARD_SIZE][BOARD_SIZE][4] = {};
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int f = 0; f < BOARD_SIZE; f++) {
            if (board[r][f] == 0) continue;
            int color   = (board[r][f] == 1) ? 1 : 0;
            int visited[BOARD_SIZE][BOARD_SIZE] = {};
            int gr[BOARD_SIZE * BOARD_SIZE], gf[BOARD_SIZE * BOARD_SIZE];
            int gc = 0;
            GoRules::get_group(board, r, f, color, visited, &gc, gr, gf);
            for (int i = 0; i < gc; i++) {
                int sr = gr[i], sf = gf[i];
                int adj[4][2] = {{sr-1,sf},{sr,sf+1},{sr+1,sf},{sr,sf-1}};
                for (int dir = 0; dir < 4; dir++) {
                    int ar = adj[dir][0], af = adj[dir][1];
                    if (ar < 0 || ar >= BOARD_SIZE || af < 0 || af >= BOARD_SIZE) continue;
                    if (board[ar][af] == 0 || board[ar][af] != board[sr][sf]) continue;
                    int rev = (dir + 2) % 4;
                    if (drawn[sr][sf][dir] || drawn[ar][af][rev]) continue;
                    drawn[sr][sf][dir] = drawn[ar][af][rev] = 1;
                    int x1 = view.offset_x + sf * view.square + view.square / 2;
                    int y1 = view.offset_y + sr * view.square + view.square / 2;
                    int x2 = view.offset_x + af * view.square + view.square / 2;
                    int y2 = view.offset_y + ar * view.square + view.square / 2;
                    SDL_Color lc = color ? SDL_Color{30,30,30,255} : SDL_Color{240,240,240,255};
                    int thick = (view.square - 4) / 2;
                    draw_thick_line(x1, y1, x2, y2, thick, lc);
                }
            }
        }
    }
}

void Renderer::render_liberties(const BoardView& view,
                                const int lib_r[], const int lib_f[], int lib_count) {
    if (lib_count == 0) return;
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < lib_count; i++) {
        int cx = view.offset_x + lib_f[i] * view.square + view.square / 2;
        int cy = view.offset_y + lib_r[i] * view.square + view.square / 2;
        int rad = view.square / 4;
        if (rad < 2) rad = 2;
        SDL_SetRenderDrawColor(sdl, 220, 50, 50, 200);
        for (int dy = -rad; dy <= rad; dy++)
            for (int dx = -rad; dx <= rad; dx++)
                if (dx * dx + dy * dy <= rad * rad)
                    SDL_RenderDrawPoint(sdl, cx + dx, cy + dy);
    }
}

// ---------------------------------------------------------------------------
// HUD overlays

const char* Renderer::format_result_message(const char* r) {
    if (!r || r[0] == '\0') return "";
    static char buf[64];
    if      (strcmp(r, "B+R") == 0 || strcmp(r, "B+Resign") == 0) return "Black Resigns";
    else if (strcmp(r, "W+R") == 0 || strcmp(r, "W+Resign") == 0) return "White Resigns";
    else if (strcmp(r, "B+T") == 0) return "Black Wins by Time";
    else if (strcmp(r, "W+T") == 0) return "White Wins by Time";
    else if (strcmp(r, "1/2-1/2") == 0 || strcmp(r, "Jigo") == 0) return "Draw";
    else if (strcmp(r, "Void") == 0) return "Game Void";
    else if (strcmp(r, "Unfinished") == 0) return "Unfinished";
    else if (r[0] == 'B' && r[1] == '+') { snprintf(buf, sizeof(buf), "Black Wins by %s", r+2); return buf; }
    else if (r[0] == 'W' && r[1] == '+') { snprintf(buf, sizeof(buf), "White Wins by %s", r+2); return buf; }
    strncpy(buf, r, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
    return buf;
}

void Renderer::render_turn_indicator(const BoardView& view, int is_black) {
    int radius = view.square / 2 - 2;
    int cx     = view.offset_x - radius - 6 - view.square;
    int cy     = view.offset_y + view.board_px / 2;
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl,
        is_black ? 30  : 240,
        is_black ? 30  : 240,
        is_black ? 30  : 240,
        255);
    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++)
            if (dx*dx + dy*dy <= radius*radius)
                SDL_RenderDrawPoint(sdl, cx + dx, cy + dy);
    // thin outline so white stone is visible against light backgrounds
    SDL_SetRenderDrawColor(sdl, is_black ? 80 : 120, is_black ? 80 : 120, is_black ? 80 : 120, 255);
    for (int angle = 0; angle < 360; angle++) {
        float rad = angle * 3.14159f / 180.0f;
        int ox = cx + (int)((radius) * cosf(rad));
        int oy = cy + (int)((radius) * sinf(rad));
        SDL_RenderDrawPoint(sdl, ox, oy);
    }
}

void Renderer::render_mode_status(const BoardView& view,
                                  bool analysis_mode, bool game_mode, bool guess_mode,
                                  bool territory_drill, bool /*paused*/) {
    const char* txt = nullptr;
    if      (territory_drill) txt = "TERRITORY DRILL";
    else if (game_mode)       txt = "GAME MODE";
    else if (analysis_mode)   txt = "ANALYSIS MODE";
    else if (guess_mode)      txt = "GUESS MODE";
    if (!txt) return;
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int tw = text_width_px(txt, scale);
    int x  = view.offset_x - margin - tw;
    int y  = view.offset_y + margin;
    SDL_Color c = {255, 255, 180, 255};
    draw_text(x, y, scale, txt, c);
}

void Renderer::render_result_message(const BoardView& view, const DrawState& ds) {
    if (!ds.game.game_finished || ds.result_message.empty()) return;
    const char* txt = format_result_message(ds.result_message.c_str());
    if (!txt || txt[0] == '\0') return;
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int tw     = text_width_px(txt, scale);
    int th     = 7 * scale;
    int x      = view.offset_x + view.board_px + margin;
    int y      = view.offset_y + (view.board_px - th) / 2;
    (void)tw;
    SDL_Color c = {255, 255, 180, 255};
    draw_text(x, y, scale, txt, c);
}

void Renderer::render_speed_label(const BoardView& view, int delay_ms, Uint32 until) {
    if (until == 0) return;
    Uint32 now = SDL_GetTicks();
    if (now >= until) return;
    char buf[32];
    int whole = delay_ms / 1000;
    int rem   = delay_ms % 1000;
    if (rem == 0) {
        const char* unit = (whole == 1) ? "second" : "seconds";
        snprintf(buf, sizeof(buf), "%d %s/move", whole, unit);
    } else {
        snprintf(buf, sizeof(buf), "%d.%d seconds/move", whole, rem / 100);
    }
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int tw     = text_width_px(buf, scale);
    int th     = 7 * scale;
    int x      = view.offset_x + (view.board_px - tw) / 2;
    if (x < view.offset_x + margin) x = view.offset_x + margin;
    int y = view.offset_y + margin;
    int pad = (scale >= 3) ? 4 : 3;
    SDL_Rect bg = {x - pad, y - pad, tw + pad * 2, th + pad * 2};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, 80, 80, 80, 180);
    SDL_RenderFillRect(sdl, &bg);
    SDL_Color c = {255, 255, 255, 255};
    draw_text(x, y, scale, buf, c);
}

void Renderer::render_guess_score(const BoardView& view, bool guess_mode, int score) {
    if (!guess_mode) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "Score: %d", score);
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int th     = 7 * scale;
    int gap    = scale + 2;
    // Render just below the "GUESS MODE" label (same left-aligned x, one line down)
    int tw_mode = text_width_px("GUESS MODE", scale);
    int x = view.offset_x - margin - tw_mode;
    int y = view.offset_y + margin + th + gap;
    SDL_Color c = {200, 200, 200, 255};
    draw_text(x, y, scale, buf, c);
}

void Renderer::render_player_labels(const BoardView& view, const DrawState& ds) {
    int margin   = (view.square >= 30) ? 16 : 8;
    int right_x0 = view.offset_x + view.board_px + margin;
    int right_x1 = view.screen_w - margin;
    if (right_x1 <= right_x0) return;

    // Use board stone radius as a column-width estimate for the scale check
    int board_radius = view.square / 2 - 2;
    int gap          = 12;
    if (right_x1 - right_x0 - board_radius * 2 - gap <= 0) return;

    const char* bn = ds.black_name.empty() ? "Black" : ds.black_name.c_str();
    const char* wn = ds.white_name.empty() ? "White" : ds.white_name.c_str();

    int bp = ds.analysis ? ds.analysis->black_prisoners : ds.game.black_prisoners;
    int wp = ds.analysis ? ds.analysis->white_prisoners : ds.game.white_prisoners;

    char bpstr[32], wpstr[32];
    snprintf(bpstr, sizeof(bpstr), "Prisoners: %d", bp);
    snprintf(wpstr, sizeof(wpstr), "Prisoners: %d", wp);

    int scale = 3;
    int need_w = text_width_px(bn, scale);
    { int ww = text_width_px(wn, scale); if (ww > need_w) need_w = ww; }
    { int pw = text_width_px(bpstr, scale); if (pw > need_w) need_w = pw; }
    { int pw = text_width_px(wpstr, scale); if (pw > need_w) need_w = pw; }

    int avail = right_x1 - right_x0 - board_radius * 2 - gap;
    if (need_w > avail) {
        scale = 2;
        need_w = text_width_px(bn, scale);
        { int ww = text_width_px(wn, scale); if (ww > need_w) need_w = ww; }
        { int pw = text_width_px(bpstr, scale); if (pw > need_w) need_w = pw; }
        { int pw = text_width_px(wpstr, scale); if (pw > need_w) need_w = pw; }
        if (need_w > avail) return;
    }

    int th       = 7 * scale;
    int line_gap = scale + 2;
    int block_h  = 2 * th + line_gap;

    // Cap radius to fit within the text block height; matches board size on smaller screens
    int radius    = std::min(board_radius, block_h / 2);
    int stone_dim = radius * 2;
    int tx        = right_x0 + stone_dim + gap;
    int scx       = right_x0 + radius;

    SDL_Color name_color     = {230, 230, 230, 255};
    SDL_Color prisoner_color = {120, 120, 120, 255};

    // Helper: draw a filled circle for the player stone
    auto draw_stone = [&](int cx, int cy, bool is_black) {
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sdl,
            is_black ?  30 : 240,
            is_black ?  30 : 240,
            is_black ?  30 : 240,
            255);
        for (int dy = -radius; dy <= radius; dy++)
            for (int dx = -radius; dx <= radius; dx++)
                if (dx*dx + dy*dy <= radius*radius)
                    SDL_RenderDrawPoint(sdl, cx + dx, cy + dy);
    };

    // Black at the top — stone centered over the two-line text block
    int top_y = view.offset_y + margin;
    draw_stone(scx, top_y + block_h / 2, true);
    draw_text(tx, top_y,              scale, bn,    name_color);
    draw_text(tx, top_y + th + line_gap, scale, bpstr, prisoner_color);

    // White at the bottom
    int bot_y = view.offset_y + view.board_px - margin - 2 * th - line_gap;
    if (bot_y < top_y) bot_y = top_y;
    draw_stone(scx, bot_y + block_h / 2, false);
    draw_text(tx, bot_y,              scale, wn,    name_color);
    draw_text(tx, bot_y + th + line_gap, scale, wpstr, prisoner_color);
}

void Renderer::render_game_date(const BoardView& view, const std::string& date) {
    if (date.size() < 4) return;
    // Show year only
    char year[5];
    memcpy(year, date.c_str(), 4);
    year[4] = '\0';
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int th     = 7 * scale;
    int x = view.offset_x;
    int y = view.offset_y + view.board_px - th - margin / 2;
    if (y + th > view.screen_h - margin) {
        int tw = text_width_px(year, scale);
        x = view.offset_x - margin - tw;
        y = view.offset_y + view.board_px - th;
    }
    SDL_Color c = {120, 120, 120, 255};
    draw_text(x, y, scale, year, c);
}

void Renderer::render_help_overlay(const BoardView& view, bool show_help) {
    if (!show_help) return;

    // key=nullptr  → section header (desc is label text)
    // key=""       → blank spacer
    // otherwise    → key in yellow, desc in white
    struct Row { const char* key; const char* desc; };
    static const Row rows[] = {
        {nullptr,      "GO VIEWER HELP"},
        {"",           ""},
        {nullptr,      "NAVIGATION"},
        {"Q",          "QUIT"},
        {"N",          "NEXT GAME"},
        {"R",          "RESTART"},
        {"C",          "CATALOG"},
        {"ESC",        "TOGGLE HELP"},
        {"",           ""},
        {nullptr,      "PLAYBACK"},
        {"UP/DOWN",    "ADJUST SPEED"},
        {"LEFT/RIGHT", "STEP MOVES"},
        {"CLICK STONE","SHOW LIBERTIES"},
        {"",           ""},
        {nullptr,      "MODES"},
        {"SPACE/A",    "ANALYSIS MODE"},
        {"G",          "GUESS MODE"},
        {"P",          "PLAY (2 PLAYERS)"},
        {"T",          "TERRITORY DRILL"},
        {"U",          "CHAIN MODE"},
        {"",           ""},
        {nullptr,      "ANALYSIS MODE"},
        {"CLICK",      "PLACE STONE"},
        {"HOLD B/W",   "FORCE COLOR"},
        {"R.CLICK",    "REMOVE STONE"},
        {"",           ""},
        {nullptr,      "CATALOG"},
        {"UP/DOWN",    "NAVIGATE"},
        {"ENTER",      "OPEN"},
        {"ESC",        "CLOSE"},
    };
    int n = (int)(sizeof(rows) / sizeof(rows[0]));

    int scale    = (view.square >= 30) ? 3 : 2;
    int line_gap = (scale >= 3) ? 4 : 3;
    int th       = 7 * scale;
    int col_gap  = 6 * scale;

    int key_col_w = 0;
    for (int i = 0; i < n; i++)
        if (rows[i].key && rows[i].key[0])
            key_col_w = std::max(key_col_w, text_width_px(rows[i].key, scale));

    int max_w = 0;
    for (int i = 0; i < n; i++) {
        int w = 0;
        if (!rows[i].key)
            w = text_width_px(rows[i].desc, scale);
        else if (rows[i].key[0])
            w = key_col_w + col_gap + text_width_px(rows[i].desc, scale);
        max_w = std::max(max_w, w);
    }

    int total_h = n * th + (n - 1) * line_gap;
    int pad     = (scale >= 3) ? 12 : 8;
    int bw = max_w + pad * 2;
    int bh = total_h + pad * 2;
    int x  = (view.screen_w - bw) / 2;
    int y  = (view.screen_h - bh) / 2;

    SDL_Rect bg = {x, y, bw, bh};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, 30, 30, 30, 210);
    SDL_RenderFillRect(sdl, &bg);

    SDL_Color col_title  = {255, 255, 255, 255};
    SDL_Color col_header = {160, 160, 160, 255};
    SDL_Color col_key    = {255, 220,  50, 255};
    SDL_Color col_desc   = {200, 200, 200, 255};

    int ty = y + pad;
    for (int i = 0; i < n; i++) {
        if (!rows[i].key) {
            SDL_Color hc = (i == 0) ? col_title : col_header;
            draw_text(x + pad, ty, scale, rows[i].desc, hc);
        } else if (rows[i].key[0]) {
            draw_text(x + pad,                        ty, scale, rows[i].key,  col_key);
            draw_text(x + pad + key_col_w + col_gap,  ty, scale, rows[i].desc, col_desc);
        }
        ty += th + line_gap;
    }
}

void Renderer::render_catalog_overlay(const BoardView& view, const Catalog& cat) {
    if (!cat.active) return;
    const char* title = "CATALOG";
    int total = (int)cat.entries.size();
    int scale     = (view.square >= 30) ? 3 : 2;
    int line_gap  = (scale >= 3) ? 4 : 3;
    int th        = 7 * scale;
    int pad       = (scale >= 3) ? 10 : 8;
    int header_gap = line_gap + (scale >= 3 ? 4 : 2);
    int max_w     = text_width_px(title, scale);
    char lbl[1024];
    for (int i = 0; i < total; i++) {
        const auto& e = cat.entries[i];
        if      (e.type == 1) snprintf(lbl, sizeof(lbl), "[DIR] %s", e.name.c_str());
        else if (e.type == 2) snprintf(lbl, sizeof(lbl), "[..]");
        else                  snprintf(lbl, sizeof(lbl), "%s", e.name.c_str());
        int w = text_width_px(lbl, scale);
        if (w > max_w) max_w = w;
    }
    int avail_h  = view.screen_h - pad * 4 - th - header_gap;
    int line_h   = th + line_gap;
    int max_lines = (avail_h > 0) ? (avail_h / line_h) : 4;
    if (max_lines < 4)     max_lines = 4;
    if (max_lines > total) max_lines = total;

    int scroll = cat.scroll;
    int idx    = cat.index;
    if (idx < scroll) scroll = idx;
    if (idx >= scroll + max_lines) scroll = idx - max_lines + 1;

    int list_h = max_lines * line_h - line_gap;
    int bw = max_w + pad * 2;
    int bh = th + header_gap + list_h + pad * 2;
    int bx = (view.screen_w - bw) / 2;
    int by = (view.screen_h - bh) / 2;
    SDL_Rect bg = {bx, by, bw, bh};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, 80, 80, 80, 190);
    SDL_RenderFillRect(sdl, &bg);
    SDL_Color white = {255, 255, 255, 255};
    int tx = bx + pad;
    int ty = by + pad;
    draw_text(tx, ty, scale, title, white);
    ty += th + header_gap;
    for (int i = 0; i < max_lines; i++) {
        int ei = scroll + i;
        if (ei >= total) break;
        const auto& e = cat.entries[ei];
        if      (e.type == 1) snprintf(lbl, sizeof(lbl), "[DIR] %s", e.name.c_str());
        else if (e.type == 2) snprintf(lbl, sizeof(lbl), "[..]");
        else                  snprintf(lbl, sizeof(lbl), "%s", e.name.c_str());
        if (ei == idx) {
            SDL_Rect hi = {tx - 3, ty - 3, max_w + 6, th + 6};
            SDL_SetRenderDrawColor(sdl, 40, 120, 255, 190);
            SDL_RenderFillRect(sdl, &hi);
        }
        draw_text(tx, ty, scale, lbl, white);
        ty += line_h;
    }
}

void Renderer::render_territory_overlay(const BoardView& view, const DrawState& ds) {
    if (!ds.territory_drill) return;
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int th     = 7 * scale;
    int lh     = th + 4;
    int tx     = view.offset_x + view.board_px + margin;
    int ty     = view.offset_y + view.board_px / 2 - lh * 2;

    SDL_Color white  = {230, 230, 230, 255};
    SDL_Color yellow = {255, 255, 100, 255};
    SDL_Color green  = {100, 220, 100, 255};
    SDL_Color red    = {255, 100, 100, 255};

    if (!ds.territory_answered) {
        draw_text(tx, ty,      scale, "WHICH IS",  white);
        draw_text(tx, ty + lh, scale, "LARGER?",   white);
        draw_text(tx, ty + lh*3, scale, "B OR W",  yellow);
    } else {
        draw_text(tx, ty, scale,
                  ds.territory_correct ? "CORRECT!" : "WRONG",
                  ds.territory_correct ? green : red);
        char buf[32];
        snprintf(buf, sizeof(buf), "BLACK %d", ds.territory_b_score);
        draw_text(tx, ty + lh*2, scale, buf, white);
        snprintf(buf, sizeof(buf), "WHITE %d", ds.territory_w_score);
        draw_text(tx, ty + lh*3, scale, buf, white);
        draw_text(tx, ty + lh*5, scale, "SPACE",    white);
        draw_text(tx, ty + lh*6, scale, "FOR NEXT", white);
    }
}

// ---------------------------------------------------------------------------
// Main draw entry points

void Renderer::draw_board(const DrawState& ds) {
    BoardView view;
    get_board_view(view);
    render_board(view, nullptr, ds);
}

void Renderer::render_board(const BoardView& view, const Overlay* overlay, const DrawState& ds) {
    SDL_SetRenderDrawColor(sdl, 50, 50, 50, 255);
    SDL_RenderClear(sdl);

    // Board background colour varies by mode
    if (ds.analysis_mode)
        SDL_SetRenderDrawColor(sdl, 110, 115, 150, 255);
    else
        SDL_SetRenderDrawColor(sdl, 95, 115, 150, 255);
    SDL_Rect board_rect = {view.offset_x, view.offset_y, view.board_px, view.board_px};
    SDL_RenderFillRect(sdl, &board_rect);

    // Grid lines
    SDL_Color grid_color = {0, 0, 0, 255};
    int normal_t   = (view.square >= 30) ? 2 : 1;
    int boundary_t = normal_t * 2;
    int boundary_idx[2] = {0, BOARD_SIZE - 1};
    for (int bi = 0; bi < 2; bi++) {
        int i = boundary_idx[bi];
        int y = view.offset_y + i * view.square + view.square / 2;
        draw_thick_line(view.offset_x + boundary_t/2, y,
                        view.offset_x + view.board_px - boundary_t/2, y,
                        boundary_t, grid_color);
        int x = view.offset_x + i * view.square + view.square / 2;
        draw_thick_line(x, view.offset_y + boundary_t/2,
                        x, view.offset_y + view.board_px - boundary_t/2,
                        boundary_t, grid_color);
    }
    for (int i = 1; i < BOARD_SIZE - 1; i++) {
        int y  = view.offset_y + i * view.square + view.square / 2;
        int x0 = view.offset_x + view.square / 2;
        int x1 = view.offset_x + (BOARD_SIZE - 1) * view.square + view.square / 2;
        draw_thick_line(x0, y, x1, y, normal_t, grid_color);
        int x  = view.offset_x + i * view.square + view.square / 2;
        int y0 = view.offset_y + view.square / 2;
        int y1 = view.offset_y + (BOARD_SIZE - 1) * view.square + view.square / 2;
        draw_thick_line(x, y0, x, y1, normal_t, grid_color);
    }

    // Star points (hoshi)
    int stars[9][2] = {{3,3},{3,9},{3,15},{9,3},{9,9},{9,15},{15,3},{15,9},{15,15}};
    int star_r = (view.square >= 30) ? 4 : 3;
    SDL_SetRenderDrawColor(sdl, 0, 0, 0, 255);
    for (auto& s : stars) {
        int x = view.offset_x + s[1] * view.square + view.square / 2;
        int y = view.offset_y + s[0] * view.square + view.square / 2;
        SDL_Rect sr = {x - star_r, y - star_r, star_r * 2, star_r * 2};
        SDL_RenderFillRect(sdl, &sr);
    }

    // Choose board array and liberty state depending on mode
    const char (*active_board)[BOARD_SIZE] =
        ds.territory_board ? ds.territory_board :
        (ds.analysis_mode && ds.analysis ? ds.analysis->board : ds.game.board);
    const int* lib_r   = ds.analysis_mode && ds.analysis ? ds.analysis->liberty_r  : ds.game.liberty_r;
    const int* lib_f   = ds.analysis_mode && ds.analysis ? ds.analysis->liberty_f  : ds.game.liberty_f;
    int lib_count      = (ds.territory_drill || !(ds.analysis_mode && ds.analysis))
                         ? (ds.territory_drill ? 0 : ds.game.liberty_count)
                         : ds.analysis->liberty_count;

    // Chain connections (behind stones)
    render_chain_connections(view, active_board, ds.chain_mode);

    // Stones
    for (int r = 0; r < BOARD_SIZE; r++)
        for (int f = 0; f < BOARD_SIZE; f++)
            if (active_board[r][f] != 0)
                draw_stone_circle(view, r, f, active_board[r][f] == 1, 255);

    // Liberty dots
    render_liberties(view, lib_r, lib_f, lib_count);

    // Stone preview overlay
    if (overlay && overlay->active) {
        int r = -1, f = -1;
        if (screen_to_board(view, (int)overlay->x, (int)overlay->y, r, f))
            draw_stone_circle(view, r, f, overlay->is_black, 128);
    }

    if (!ds.territory_drill) {
        render_player_labels(view, ds);
        render_speed_label(view, ds.move_delay_ms, ds.speed_message_until);
        render_guess_score(view, ds.guess_mode, ds.guess_score);
        render_result_message(view, ds);
        render_game_date(view, ds.game_date);
    }
    render_mode_status(view, ds.analysis_mode, ds.game_mode, ds.guess_mode, ds.territory_drill, false);
    render_territory_overlay(view, ds);

    // TODO: turn indicator disabled — cursor color now conveys whose turn it is (consider deleting)
    // if (ds.analysis_mode || ds.game_mode || ds.guess_mode) {
    //     int is_black = ds.analysis ? ds.analysis->turn_is_black : ds.game.turn_is_black;
    //     render_turn_indicator(view, is_black);
    // }
    render_help_overlay(view, ds.show_help);
    render_catalog_overlay(view, ds.catalog);

    if (!ds.suppress_present)
        SDL_RenderPresent(sdl);
}
