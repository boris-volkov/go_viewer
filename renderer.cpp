#include "renderer.hpp"
#include "palette.hpp"
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

void Renderer::get_board_view(BoardView& view, int active_size) const {
    int w = SCREEN_SIZE, h = SCREEN_SIZE;
    SDL_GetRendererOutputSize(sdl, &w, &h);
    int min_dim      = (w < h) ? w : h;
    view.active_size = active_size;
    view.square      = min_dim / (active_size + 2);
    if (view.square < 1) view.square = 1;
    view.margin      = view.square;
    view.board_px    = view.square * active_size;
    int bg_size      = view.square * (active_size + 2);
    view.offset_x    = (w - bg_size) / 2 + view.margin;
    view.offset_y    = (h - bg_size) / 2 + view.margin;
    view.screen_w    = w;
    view.screen_h    = h;
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
    if (br < 0 || br >= view.active_size || bf < 0 || bf >= view.active_size) return false;
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

// Scanline fill of a circle — ~2*r SDL calls instead of ~π*r² point calls.
// Colour and blend mode must be set by the caller.
void Renderer::fill_circle(int cx, int cy, int radius) {
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrtf((float)(radius * radius - dy * dy));
        SDL_RenderDrawLine(sdl, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

void Renderer::draw_stone_circle(const BoardView& view, int r, int f, int is_black, Uint8 alpha, bool shadow_pass) {
    int cx     = view.offset_x + f * view.square + view.square / 2;
    int cy     = view.offset_y + r * view.square + view.square / 2;
    int radius = view.square / 2 - 2;
    if (radius < 2) radius = 2;
    shade_stone(cx, cy, radius, is_black, alpha, shadow_pass);
}

// ---------------------------------------------------------------------------
// Shared stone colour palette — edit here to reskin both stones and chain links.

Renderer::StoneColors Renderer::stone_colors(int is_black) {
    if (is_black)
        //        base                lit overlay          dark edge            a1      a2      a3
        return { {30, 32, 38, 255}, {95, 88, 78, 255}, {20, 22, 27, 255}, 0.14f, 0.00f, 0.10f };
    else
        return { {210, 214, 220, 255}, {255, 252, 240, 255}, {178, 182, 190, 255}, 0.35f, 0.28f, 0.20f };
}

void Renderer::shade_stone(int cx, int cy, int radius, int is_black, Uint8 alpha, bool shadow_pass) {
    if (shadow_pass) {
        // Only the cast shadow — two soft dark circles offset lower-right.
        // Blend mode is set by the caller (render_all_shadows uses max-alpha blend).
        if (radius >= 4) {
            int sox = radius / 5 + 1, soy = sox;
            SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.18f));
            fill_circle(cx + sox + 1, cy + soy + 1, radius + 2);
            SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.30f));
            fill_circle(cx + sox,     cy + soy,     radius + 1);
        }
        return;
    }

    // Stone fill + shading
    auto c = stone_colors(is_black);
    int h1x = cx - radius / 6,  h1y = cy - radius / 6;  // broad
    int h2x = cx - radius / 5,  h2y = cy - radius / 5;  // mid
    int h3x = cx - radius / 4,  h3y = cy - radius / 4;  // tight

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, c.base.r, c.base.g, c.base.b, alpha);
    fill_circle(cx, cy, radius);

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    if (c.alpha1 > 0.f) {
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha1));
        fill_circle(h1x, h1y, radius * 5 / 6);
    }
    if (c.alpha2 > 0.f) {
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha2));
        fill_circle(h2x, h2y, radius * 2 / 3);
    }
    if (c.alpha3 > 0.f) {
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha3));
        fill_circle(h3x, h3y, radius / 2);
    }
}

// Draw a chain link as a shaded cylinder.
// shadow_pass=true  → only the soft dark shadow quad (drawn first, over all board lines)
// shadow_pass=false → only the lit cylinder (drawn second, on top of all shadows)
void Renderer::draw_stone_link(int x1, int y1, int x2, int y2, int thickness, int is_black, bool shadow_pass) {
    if (thickness < 1) thickness = 1;

    float fdx = (float)(x2 - x1), fdy = (float)(y2 - y1);
    float len  = sqrtf(fdx * fdx + fdy * fdy);
    if (len < 1.f) return;

    float nx = fdy / len, ny = -fdx / len;

    if (shadow_pass) {
        // Two soft dark quads offset lower-right — same scale as shade_stone.
        // Blend mode is set by the caller (render_all_shadows uses max-alpha blend).
        if (thickness < 4) return;
        int shx = thickness / 5 + 1, shy = shx;
        for (int p = 0; p < 2; p++) {
            float offx = (float)(p == 0 ? shx + 1 : shx);
            float offy = (float)(p == 0 ? shy + 1 : shy);
            float ht   = (p == 0 ? thickness + 2 : thickness + 1) * 0.5f;
            Uint8 sa   = (p == 0) ? (Uint8)(255 * 0.18f) : (Uint8)(255 * 0.30f);
            SDL_Color sc = {0, 0, 0, sa};
            float pox = nx * ht, poy = ny * ht;
            SDL_Vertex sv[4] = {
                {{(float)x1 - pox + offx, (float)y1 - poy + offy}, sc, {0,0}},
                {{(float)x1 + pox + offx, (float)y1 + poy + offy}, sc, {0,0}},
                {{(float)x2 + pox + offx, (float)y2 + poy + offy}, sc, {0,0}},
                {{(float)x2 - pox + offx, (float)y2 - poy + offy}, sc, {0,0}},
            };
            int si[6] = {0,1,2, 0,2,3};
            SDL_RenderGeometry(sdl, nullptr, sv, 4, si, 6);
        }
        return;
    }

    // Cylinder pass — lit edge → base axis → shadow edge
    auto c = stone_colors(is_black);
    float ea = 1.f - (1.f - c.alpha1) * (1.f - c.alpha2) * (1.f - c.alpha3);
    SDL_Color lit_v = {
        (Uint8)(c.base.r + ea * (c.lit.r - c.base.r)),
        (Uint8)(c.base.g + ea * (c.lit.g - c.base.g)),
        (Uint8)(c.base.b + ea * (c.lit.b - c.base.b)),
        255
    };

    float half = (float)thickness * 0.5f;
    float ox = nx * half, oy = ny * half;

    bool flip = (nx * (-0.707f) + ny * (-0.707f)) < 0.f;
    float lox = flip ? -ox : ox,  loy = flip ? -oy : oy;
    float sox = flip ?  ox : -ox, soy = flip ?  oy : -oy;

    SDL_Vertex v[6];
    v[0] = {{(float)x1+lox, (float)y1+loy}, lit_v,  {0,0}};
    v[1] = {{(float)x1,     (float)y1     }, c.base, {0,0}};
    v[2] = {{(float)x1+sox, (float)y1+soy}, c.dark, {0,0}};
    v[3] = {{(float)x2+sox, (float)y2+soy}, c.dark, {0,0}};
    v[4] = {{(float)x2,     (float)y2     }, c.base, {0,0}};
    v[5] = {{(float)x2+lox, (float)y2+loy}, lit_v,  {0,0}};

    int idx[12] = {0,1,4, 0,4,5, 1,2,3, 1,3,4};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_RenderGeometry(sdl, nullptr, v, 6, idx, 12);
}

// ---------------------------------------------------------------------------
// Overlay helpers

void Renderer::render_chain_connections(const BoardView& view, const char board[][MAX_BOARD_SIZE],
                                        bool chain_mode, int stone_filter, bool shadows_only) {
    if (!chain_mode) return;
    int n = view.active_size;
    int drawn[MAX_BOARD_SIZE][MAX_BOARD_SIZE][4] = {};
    for (int r = 0; r < n; r++) {
        for (int f = 0; f < n; f++) {
            if (board[r][f] == 0) continue;
            int color   = (board[r][f] == 1) ? 1 : 0;
            if (stone_filter == 1 && color == 0) continue;
            if (stone_filter == 2 && color == 1) continue;
            int visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
            int gr[MAX_BOARD_SIZE * MAX_BOARD_SIZE], gf[MAX_BOARD_SIZE * MAX_BOARD_SIZE];
            int gc = 0;
            GoRules::get_group(board, r, f, color, visited, &gc, gr, gf, n);
            for (int i = 0; i < gc; i++) {
                int sr = gr[i], sf = gf[i];
                int adj[4][2] = {{sr-1,sf},{sr,sf+1},{sr+1,sf},{sr,sf-1}};
                for (int dir = 0; dir < 4; dir++) {
                    int ar = adj[dir][0], af = adj[dir][1];
                    if (ar < 0 || ar >= n || af < 0 || af >= n) continue;
                    if (board[ar][af] == 0 || board[ar][af] != board[sr][sf]) continue;
                    int rev = (dir + 2) % 4;
                    if (drawn[sr][sf][dir] || drawn[ar][af][rev]) continue;
                    drawn[sr][sf][dir] = drawn[ar][af][rev] = 1;
                    int x1 = view.offset_x + sf * view.square + view.square / 2;
                    int y1 = view.offset_y + sr * view.square + view.square / 2;
                    int x2 = view.offset_x + af * view.square + view.square / 2;
                    int y2 = view.offset_y + ar * view.square + view.square / 2;
                    int thick = (view.square - 4) / 2;
                    draw_stone_link(x1, y1, x2, y2, thick, color, shadows_only);
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
        SDL_SetRenderDrawColor(sdl, Palette::LIBERTY_DOT.r, Palette::LIBERTY_DOT.g, Palette::LIBERTY_DOT.b, Palette::LIBERTY_DOT.a);
        fill_circle(cx, cy, rad);
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
    int pad    = (view.square >= 30) ? 16 : 8;
    int tw = text_width_px(txt, scale);
    // Anchor to background left/top edge (one view.margin outside the grid)
    int x  = view.offset_x - view.margin - pad - tw;
    int y  = view.offset_y - view.margin + pad;
    draw_text(x, y, scale, txt, Palette::ACCENT);
}

void Renderer::render_result_message(const BoardView& view, const DrawState& ds) {
    if (!ds.game.game_finished || ds.result_message.empty()) return;
    const char* txt = format_result_message(ds.result_message.c_str());
    if (!txt || txt[0] == '\0') return;
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int tw     = text_width_px(txt, scale);
    int th     = 7 * scale;
    int bg_h   = view.board_px + 2 * view.margin;
    // Anchor to background right/vertical-centre
    int x      = view.offset_x + view.board_px + view.margin + margin;
    int y      = view.offset_y - view.margin + (bg_h - th) / 2;
    (void)tw;
    draw_text(x, y, scale, txt, Palette::ACCENT);
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
    int bg_left = view.offset_x - view.margin;
    int bg_size = view.board_px + 2 * view.margin;
    int x      = bg_left + (bg_size - tw) / 2;
    if (x < bg_left + margin) x = bg_left + margin;
    int y = view.offset_y - view.margin + margin;
    int pad = (scale >= 3) ? 4 : 3;
    SDL_Rect bg = {x - pad, y - pad, tw + pad * 2, th + pad * 2};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, Palette::OVERLAY_SPEED.r, Palette::OVERLAY_SPEED.g, Palette::OVERLAY_SPEED.b, Palette::OVERLAY_SPEED.a);
    SDL_RenderFillRect(sdl, &bg);
    draw_text(x, y, scale, buf, Palette::TEXT_WHITE);
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
    int x = view.offset_x - view.margin - margin - tw_mode;
    int y = view.offset_y - view.margin + margin + th + gap;
    draw_text(x, y, scale, buf, Palette::TEXT_SECONDARY);
}

void Renderer::render_player_labels(const BoardView& view, const DrawState& ds) {
    int margin   = (view.square >= 30) ? 16 : 8;
    // Anchor to background right edge (one view.margin outside the grid)
    int right_x0 = view.offset_x + view.board_px + view.margin + margin;
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

    SDL_Color name_color     = Palette::TEXT_PRIMARY;
    SDL_Color prisoner_color = Palette::TEXT_DIM;

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
    // Anchor to background top edge (one view.margin above the grid)
    int top_y = view.offset_y - view.margin + margin;
    draw_stone(scx, top_y + block_h / 2, true);
    draw_text(tx, top_y,              scale, bn,    name_color);
    draw_text(tx, top_y + th + line_gap, scale, bpstr, prisoner_color);

    // White at the bottom — anchor to background bottom edge
    int bot_y = view.offset_y + view.board_px + view.margin - margin - 2 * th - line_gap;
    if (bot_y < top_y) bot_y = top_y;
    draw_stone(scx, bot_y + block_h / 2, false);
    draw_text(tx, bot_y,              scale, wn,    name_color);
    draw_text(tx, bot_y + th + line_gap, scale, wpstr, prisoner_color);
}

void Renderer::render_game_date(const BoardView& view, const std::string& date) {
    if (date.size() < 4) return;
    char year[5];
    memcpy(year, date.c_str(), 4);
    year[4] = '\0';
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int th     = 7 * scale;
    int tw     = text_width_px(year, scale);
    // Primary: left of background, sitting comfortably above the bottom edge
    int x = view.offset_x - view.margin - margin - tw;
    int y = view.offset_y + view.board_px + view.margin - margin - th;
    // Fallback: below the board left-aligned (only on non-widescreen layouts)
    if (x < 0) {
        x = view.offset_x - view.margin;
        y = view.offset_y + view.board_px + view.margin + margin / 2;
    }
    draw_text(x, y, scale, year, Palette::TEXT_DIM);
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
        {"S",          "SAVE POSITION"},
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
        {"F",          "FREE MODE"},
        {"",           ""},
        {nullptr,      "ANALYSIS MODE"},
        {"CLICK",      "PLACE STONE"},
        {"HOLD B/W",   "FORCE COLOR"},
        {"R.CLICK",    "REMOVE STONE"},
        {"X",          "CLEAR BOARD"},
        {"",           ""},
        {nullptr,      "CATALOG"},
        {"UP/DOWN",    "NAVIGATE"},
        {"ENTER",      "OPEN"},
        {"TYPE",       "SEARCH"},
        {"ESC",        "CLOSE / CLEAR SEARCH"},
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
    SDL_SetRenderDrawColor(sdl, Palette::OVERLAY_DARK.r, Palette::OVERLAY_DARK.g, Palette::OVERLAY_DARK.b, Palette::OVERLAY_DARK.a);
    SDL_RenderFillRect(sdl, &bg);

    SDL_Color col_title  = Palette::TEXT_WHITE;
    SDL_Color col_header = {160, 160, 160, 255};
    SDL_Color col_key    = Palette::ACCENT;
    SDL_Color col_desc   = Palette::TEXT_SECONDARY;

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

void Renderer::render_mini_board(int x, int y, int size,
                                  const char board[][BOARD_SIZE]) {
    // Background
    SDL_Rect bg = {x, y, size, size};
    SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
    SDL_RenderFillRect(sdl, &bg);

    int sq  = size / BOARD_SIZE;                  // pixels per intersection
    if (sq < 1) sq = 1;
    int margin = (size - BOARD_SIZE * sq) / 2;    // center the grid in the box
    int px0 = x + margin + sq / 2;               // top-left intersection (x)
    int py0 = y + margin + sq / 2;               // top-left intersection (y)
    int span = (BOARD_SIZE - 1) * sq;

    // Grid lines
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    for (int i = 0; i < BOARD_SIZE; i++) {
        SDL_RenderDrawLine(sdl, px0 + i * sq, py0, px0 + i * sq, py0 + span);
        SDL_RenderDrawLine(sdl, px0, py0 + i * sq, px0 + span, py0 + i * sq);
    }

    // Border
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 180);
    SDL_RenderDrawRect(sdl, &bg);

    // Stones
    int r_stone = sq / 2;
    if (r_stone < 1) r_stone = 1;
    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            int cell = board[row][col];
            if (cell == 0) continue;
            int cx = px0 + col * sq;
            int cy = py0 + row * sq;
            bool is_black = (cell == 1);
            SDL_Color c = is_black ? Palette::STONE_BLACK : Palette::STONE_WHITE;
            SDL_SetRenderDrawColor(sdl, c.r, c.g, c.b, 255);
            fill_circle(cx, cy, r_stone);
        }
    }
}

void Renderer::render_catalog_overlay(const BoardView& view, const DrawState& ds) {
    const Catalog& cat = ds.catalog;
    if (!cat.active) return;

    // Full-screen panel
    SDL_Rect bg = {0, 0, view.screen_w, view.screen_h};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    SDL_RenderFillRect(sdl, &bg);

    int total      = (int)cat.entries.size();
    int scale      = (view.square >= 30) ? 3 : 2;
    int line_gap   = (scale >= 3) ? 8 : 4;
    int th         = 7 * scale;
    int pad        = (scale >= 3) ? 10 : 8;
    int hpad       = 100;
    int header_gap = line_gap + (scale >= 3 ? 4 : 2);
    int line_h     = th + line_gap;

    // tx/ty track the current draw cursor for the left-side list column
    int tx = hpad;
    int ty = pad;

    // --- Header ---
    char title_buf[128];
    const char* title;
    if (cat.search_mode) {
        title = "CATALOG  (ESC to clear search)";
    } else if (cat.virtual_player_mode && !cat.virtual_player.empty()) {
        snprintf(title_buf, sizeof(title_buf), "GAMES: %s  (ESC to close)", cat.virtual_player.c_str());
        title = title_buf;
    } else {
        title = "CATALOG  (ESC to close)";
    }
    draw_text(tx, ty, scale, title, Palette::ACCENT);
    ty += th + header_gap;

    // --- Index status / search bar ---
    bool index_ready   = cat.game_index.loaded();
    bool index_loading = cat.game_index.is_loading();

    if (index_loading && !index_ready) {
        // Show a brief "building..." hint below the header
        draw_text(tx, ty, scale, "Building index...", Palette::TEXT_WHITE);
        ty += th + line_gap + (scale >= 3 ? 4 : 2);
    }

    if (!cat.search_query.empty() || cat.search_mode) {
        char search_lbl[256];
        if (!index_ready && !cat.search_query.empty()) {
            snprintf(search_lbl, sizeof(search_lbl), "SEARCH: %s_  (indexing...)", cat.search_query.c_str());
        } else {
            snprintf(search_lbl, sizeof(search_lbl), "SEARCH: %s_", cat.search_query.c_str());
        }
        draw_text(tx, ty, scale, search_lbl, Palette::ACCENT);
        ty += th + line_gap;
        char count_lbl[64];
        snprintf(count_lbl, sizeof(count_lbl), "%d result%s",
                 total, total == 1 ? "" : "s");
        draw_text(tx, ty, scale, count_lbl, Palette::TEXT_WHITE);
        ty += th + line_gap + (scale >= 3 ? 4 : 2);
    }

    // --- Measure list width and black-column width for aligned player names ---
    char lbl[1024];
    int vs_w     = text_width_px("vs", scale);
    int col_gap  = scale * 6;   // pixel gap between columns
    int max_w       = text_width_px(title, scale);
    int max_black_w = 0;        // widest black-player name across all entries

    for (int i = 0; i < total; i++) {
        const auto& e = cat.entries[i];
        int w;
        if (e.type == 0 && (!e.player_black.empty() || !e.player_white.empty())) {
            // Three-column: black  vs  white
            const char* bn = e.player_black.empty() ? "?" : e.player_black.c_str();
            const char* wn = e.player_white.empty() ? "?" : e.player_white.c_str();
            int bw = text_width_px(bn, scale);
            int ww = text_width_px(wn, scale);
            if (bw > max_black_w) max_black_w = bw;
            w = bw + col_gap + vs_w + col_gap + ww;
        } else {
            if      (e.type == 1) snprintf(lbl, sizeof(lbl), "[DIR] %s", e.display_name.c_str());
            else if (e.type == 2) snprintf(lbl, sizeof(lbl), "[..]");
            else if (e.type == 3) snprintf(lbl, sizeof(lbl), "[%s]", e.display_name.c_str());
            else                  snprintf(lbl, sizeof(lbl), "%s", e.display_name.c_str());
            w = text_width_px(lbl, scale);
        }
        if (w > max_w) max_w = w;
    }
    int list_right = hpad + max_w;

    // --- Thumbnails ---
    int thumb_inner_gap = 40;
    int thumb_vpad      = 40;
    int thumb_size      = (view.screen_h - thumb_vpad * 2 - thumb_inner_gap) * 4 / 10;
    int two_h           = thumb_size * 2 + thumb_inner_gap;
    int thumb_x         = list_right + (view.screen_w - list_right - thumb_size) / 2;
    int thumb_y_top     = (view.screen_h - two_h) / 2;

    bool has_thumb = ds.catalog_thumb_valid
                     && ds.catalog_thumb_open  != nullptr
                     && ds.catalog_thumb_final != nullptr;
    if (has_thumb) {
        render_mini_board(thumb_x, thumb_y_top,
                          thumb_size, ds.catalog_thumb_open);
        render_mini_board(thumb_x, thumb_y_top + thumb_size + thumb_inner_gap,
                          thumb_size, ds.catalog_thumb_final);
    }

    // --- Entry list ---
    int avail_h   = view.screen_h - ty - pad;
    int max_lines = (avail_h > 0) ? (avail_h / line_h) : 4;
    if (max_lines < 4)     max_lines = 4;
    if (max_lines > total) max_lines = total;

    int scroll = cat.scroll;
    int idx    = cat.index;
    if (idx < scroll) scroll = idx;
    if (idx >= scroll + max_lines) scroll = idx - max_lines + 1;

    for (int i = 0; i < max_lines; i++) {
        int ei = scroll + i;
        if (ei >= total) break;
        const auto& e = cat.entries[ei];

        // Highlight bar for the selected entry
        if (ei == idx) {
            SDL_Rect hi = {hpad - 3, ty - 3, max_w + 6, th + 6};
            SDL_SetRenderDrawColor(sdl, Palette::CATALOG_SELECT.r, Palette::CATALOG_SELECT.g,
                                   Palette::CATALOG_SELECT.b, Palette::CATALOG_SELECT.a);
            SDL_RenderFillRect(sdl, &hi);
        }

        if (e.type == 0 && (!e.player_black.empty() || !e.player_white.empty())) {
            // Three-column layout: [black ACCENT] [vs WHITE] [white ACCENT]
            const char* bn = e.player_black.empty() ? "?" : e.player_black.c_str();
            const char* wn = e.player_white.empty() ? "?" : e.player_white.c_str();
            draw_text(tx,                                    ty, scale, bn,   Palette::ACCENT);
            draw_text(tx + max_black_w + col_gap,            ty, scale, "vs", Palette::TEXT_WHITE);
            draw_text(tx + max_black_w + col_gap + vs_w + col_gap, ty, scale, wn, Palette::ACCENT);
        } else {
            if      (e.type == 1) snprintf(lbl, sizeof(lbl), "[DIR] %s", e.display_name.c_str());
            else if (e.type == 2) snprintf(lbl, sizeof(lbl), "[..]");
            else if (e.type == 3) snprintf(lbl, sizeof(lbl), "[%s]", e.display_name.c_str());
            else                  snprintf(lbl, sizeof(lbl), "%s", e.display_name.c_str());
            draw_text(tx, ty, scale, lbl, Palette::TEXT_WHITE);
        }

        ty += line_h;
    }
}

// Draw a dashed line between two pixel points, alternating drawn/skipped segments.
void Renderer::draw_dashed_line(int x1, int y1, int x2, int y2, int dash_len, int gap_len) {
    int dx = x2 - x1, dy = y2 - y1;
    int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps == 0) return;
    float sx = (float)dx / steps, sy = (float)dy / steps;
    int period = dash_len + gap_len;
    for (int i = 0; i <= steps; i++) {
        if ((i % period) < dash_len) {
            int px = x1 + (int)(sx * i);
            int py = y1 + (int)(sy * i);
            SDL_RenderDrawPoint(sdl, px, py);
        }
    }
}

void Renderer::render_box_selection(const BoardView& view, const DrawState& ds) {
    bool has_committed = (ds.box_sel_pts != nullptr && ds.box_sel_count > 0);
    if (!has_committed && !ds.box_drag_active) return;

    int half   = view.square / 2;
    int dot_r  = std::max(2, view.square / 5);

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);

    // --- Draw committed intersection dots ---
    if (has_committed) {
        SDL_SetRenderDrawColor(sdl, Palette::BOX_SELECT.r, Palette::BOX_SELECT.g,
                               Palette::BOX_SELECT.b, Palette::BOX_SELECT.a);
        for (int r = 0; r < view.active_size; r++) {
            for (int f = 0; f < view.active_size; f++) {
                if (!ds.box_sel_pts[r][f]) continue;
                int x, y;
                board_to_screen(view, r, f, x, y);
                fill_circle(x + half, y + half, dot_r);
            }
        }
    }

    // --- Draw dashed rubber-band rectangle for active drag ---
    if (ds.box_drag_active) {
        int r1 = ds.box_drag_r1, f1 = ds.box_drag_f1;
        int r2 = ds.box_drag_r2, f2 = ds.box_drag_f2;
        int rmin = std::min(r1, r2), rmax = std::max(r1, r2);
        int fmin = std::min(f1, f2), fmax = std::max(f1, f2);

        int x1, y1, x2, y2;
        board_to_screen(view, rmin, fmin, x1, y1);
        board_to_screen(view, rmax, fmax, x2, y2);
        // Shift to intersection centers
        x1 += half; y1 += half;
        x2 += half; y2 += half;

        // Thick-ish dashed rect: draw 2px wide by offsetting
        SDL_Color dc = Palette::BOX_SELECT;
        SDL_SetRenderDrawColor(sdl, dc.r, dc.g, dc.b, dc.a);
        int dash = 6, gap = 4;
        for (int t = 0; t <= 1; t++) {
            draw_dashed_line(x1-t, y1-t, x2+t, y1-t, dash, gap);  // top
            draw_dashed_line(x1-t, y2+t, x2+t, y2+t, dash, gap);  // bottom
            draw_dashed_line(x1-t, y1-t, x1-t, y2+t, dash, gap);  // left
            draw_dashed_line(x2+t, y1-t, x2+t, y2+t, dash, gap);  // right
        }
    }

    // --- Count label on the right side of the board ---
    int total = ds.box_sel_count;
    // Add drag preview intersections to the count display
    if (ds.box_drag_active) {
        int r1 = ds.box_drag_r1, f1 = ds.box_drag_f1;
        int r2 = ds.box_drag_r2, f2 = ds.box_drag_f2;
        int rmin = std::min(r1, r2), rmax = std::max(r1, r2);
        int fmin = std::min(f1, f2), fmax = std::max(f1, f2);
        total += (rmax - rmin + 1) * (fmax - fmin + 1);
    }
    if (total > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", total);
        int scale  = (view.square >= 30) ? 3 : 2;
        int margin = (view.square >= 30) ? 16 : 8;
        int tx = view.offset_x + view.board_px + view.margin + margin;
        int ty = view.offset_y - view.margin + (view.board_px + 2 * view.margin) / 2 - 7 * scale / 2;
        draw_text(tx, ty, scale, buf, Palette::ACCENT);
    }
}

void Renderer::render_game_comment(const BoardView& view, const DrawState& ds) {
    if (ds.game_comment.empty() || ds.catalog.active) return;

    int scale    = 2;
    int th       = 7 * scale;
    int line_gap = scale + 2;
    int lh       = th + line_gap;
    int pad      = 10;

    // Space to the left of the board background
    int right_x = view.offset_x - view.margin - pad;
    int left_x  = pad;
    int avail_w = right_x - left_x;
    if (avail_w < text_width_px("XXXX", scale)) return;  // too narrow to be useful

    // Word-wrap the comment into lines that fit avail_w
    std::vector<std::string> lines;
    {
        std::string current, word;
        auto flush_word = [&]() {
            if (word.empty()) return;
            std::string candidate = current.empty() ? word : current + " " + word;
            if (text_width_px(candidate.c_str(), scale) <= avail_w) {
                current = std::move(candidate);
            } else {
                if (!current.empty()) lines.push_back(current);
                current = word;
            }
            word.clear();
        };
        for (char c : ds.game_comment) {
            if (c == ' ' || c == '\t') {
                flush_word();
            } else if (c == '\n' || c == '\r') {
                flush_word();
                lines.push_back(current);
                current.clear();
            } else {
                // If a single word is wider than the column, break it mid-word
                word += c;
                if (text_width_px(word.c_str(), scale) > avail_w && word.size() > 1) {
                    word.pop_back();
                    lines.push_back(word);
                    word.clear();
                    word += c;
                }
            }
        }
        flush_word();
        if (!current.empty()) lines.push_back(current);
    }
    if (lines.empty()) return;

    // Vertically centre the block in the board background area
    int bg_top    = view.offset_y - view.margin;
    int bg_bottom = view.offset_y + view.board_px + view.margin;
    int total_h   = (int)lines.size() * lh - line_gap;
    int ty        = bg_top + (bg_bottom - bg_top - total_h) / 2;

    for (const auto& line : lines) {
        draw_text(left_x, ty, scale, line.c_str(), Palette::TEXT_DIM);
        ty += lh;
    }
}

void Renderer::render_save_input(const BoardView& view, const DrawState& ds) {
    if (ds.save_input_step == 0) return;

    int scale    = (view.square >= 30) ? 3 : 2;
    int th       = 7 * scale;
    int line_gap = scale + 2;
    int lh       = th + line_gap;
    int pad      = (scale >= 3) ? 16 : 10;

    // Lines to display
    const char* title   = "SAVE POSITION";
    const char* prompt  = (ds.save_input_step == 1) ? "Name:" : "Note:";
    const char* hint1   = "ENTER to continue";
    const char* hint2   = "ESC to cancel";
    if (ds.save_input_step == 2) hint1 = "ENTER to save";

    // Cursor-appended input text
    std::string input_display = ds.save_input_buf + "_";

    // Measure
    int max_w = text_width_px(title, scale);
    auto mw = [&](const char* s) { int w = text_width_px(s, scale); if (w > max_w) max_w = w; };
    mw(hint1); mw(hint2);
    {
        // "Name: text_" or "Note: text_"
        std::string full = std::string(prompt) + " " + input_display;
        mw(full.c_str());
    }

    int bw = max_w + pad * 2;
    // Layout: title, blank, prompt+input, blank, hint1, hint2
    // Each "blank" is one lh advance; hint2 ends with th (no trailing gap).
    int bh = lh * 5 + th + pad * 2;
    int bx = (view.screen_w - bw) / 2;
    int by = (view.screen_h - bh) / 2;

    SDL_Rect bg = {bx, by, bw, bh};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, Palette::OVERLAY_DARK.r, Palette::OVERLAY_DARK.g,
                           Palette::OVERLAY_DARK.b, Palette::OVERLAY_DARK.a);
    SDL_RenderFillRect(sdl, &bg);

    int tx = bx + pad;
    int ty = by + pad;
    draw_text(tx, ty, scale, title, Palette::ACCENT);
    ty += lh * 2;  // blank line after title

    // "Name: typed_text_"
    draw_text(tx, ty, scale, prompt, Palette::TEXT_WHITE);
    int prompt_w = text_width_px(prompt, scale) + scale * 2;
    draw_text(tx + prompt_w, ty, scale, input_display.c_str(), Palette::ACCENT);
    ty += lh * 2;  // blank line

    draw_text(tx, ty, scale, hint1, Palette::TEXT_DIM);
    ty += lh;
    draw_text(tx, ty, scale, hint2, Palette::TEXT_DIM);
}

void Renderer::render_flash_message(const BoardView& view, const DrawState& ds) {
    if (ds.flash_message.empty() || ds.flash_message_until == 0) return;
    Uint32 now = SDL_GetTicks();
    if (now >= ds.flash_message_until) return;

    const char* txt = ds.flash_message.c_str();
    int scale = (view.square >= 30) ? 3 : 2;
    int tw    = text_width_px(txt, scale);
    int th    = 7 * scale;
    int pad   = (scale >= 3) ? 10 : 7;
    int bw    = tw + pad * 2;
    int bh    = th + pad * 2;
    int bx    = (view.screen_w - bw) / 2;
    int by    = view.screen_h - bh - pad * 2;

    SDL_Rect bg = {bx, by, bw, bh};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, 20, 90, 20, 220);
    SDL_RenderFillRect(sdl, &bg);
    draw_text(bx + pad, by + pad, scale, txt, Palette::TEXT_WHITE);
}

void Renderer::render_quit_confirm(const BoardView& view) {
    int scale = (view.square >= 30) ? 3 : 2;
    int th    = 7 * scale;
    int pad   = scale >= 3 ? 16 : 12;

    const char* line1 = "QUIT?";
    const char* line2 = "press Q again to quit";
    const char* line3 = "or ESC to cancel";
    int gap   = scale >= 3 ? 6 : 4;
    int lh    = th + gap;
    int w     = text_width_px(line2, scale);
    int bw    = w + pad * 2;
    int bh    = th + lh * 2 + pad * 2;
    int bx    = (view.screen_w - bw) / 2;
    int by    = (view.screen_h - bh) / 2;

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    SDL_Rect bg = {bx, by, bw, bh};
    SDL_RenderFillRect(sdl, &bg);

    int tx = bx + pad;
    int ty = by + pad;
    draw_text(tx, ty, scale, line1, Palette::ACCENT);
    ty += lh;
    draw_text(tx, ty, scale, line2, Palette::TEXT_PRIMARY);
    ty += lh;
    draw_text(tx, ty, scale, line3, Palette::TEXT_DIM);
}

void Renderer::render_territory_overlay(const BoardView& view, const DrawState& ds) {
    if (!ds.territory_drill) return;
    int scale  = (view.square >= 30) ? 3 : 2;
    int margin = (view.square >= 30) ? 16 : 8;
    int th     = 7 * scale;
    int lh     = th + 4;
    int tx     = view.offset_x + view.board_px + view.margin + margin;
    int ty     = view.offset_y - view.margin + (view.board_px + 2 * view.margin) / 2 - lh * 2;

    SDL_Color white  = Palette::SCORE_TEXT;
    SDL_Color yellow = Palette::ACCENT;
    SDL_Color green  = Palette::SCORE_CORRECT;
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
    get_board_view(view, ds.active_board_size);
    render_board(view, nullptr, ds);
}

// ---------------------------------------------------------------------------
// Board cache hash — covers everything that affects the visual output
// except the cursor position (cursor_x/y/type), which is composited on top
// each frame without touching the cache.

uint64_t Renderer::compute_cache_hash(const DrawState& ds) const {
    // FNV-1a 64-bit
    uint64_t h = 14695981039346656037ULL;
    auto mix8  = [&](uint8_t  v) { h ^= v; h *= 1099511628211ULL; };
    auto mix64 = [&](uint64_t v) {
        for (int i = 0; i < 8; i++, v >>= 8) mix8(uint8_t(v));
    };
    auto mix_str = [&](const std::string& s) {
        for (char c : s) mix8(uint8_t(c));
        mix8(0);
    };

    // Active board array
    const char (*board)[MAX_BOARD_SIZE] =
        ds.territory_board ? ds.territory_board :
        (ds.analysis_mode && ds.analysis ? ds.analysis->board : ds.game.board);
    for (int r = 0; r < ds.active_board_size; r++)
        for (int f = 0; f < ds.active_board_size; f++)
            mix8(uint8_t(board[r][f]));

    // Stone filter
    mix8(uint8_t(ds.stone_filter));

    // Quit confirmation
    mix8(uint8_t(ds.show_move_numbers));
    mix64(uint64_t(ds.sgf_game_index));
    mix8(uint8_t(ds.quit_confirm));

    // Box selection
    mix64(uint64_t(ds.box_sel_count));
    mix8(uint8_t(ds.box_drag_active));
    if (ds.box_drag_active) {
        mix64(uint64_t(ds.box_drag_r1)); mix64(uint64_t(ds.box_drag_f1));
        mix64(uint64_t(ds.box_drag_r2)); mix64(uint64_t(ds.box_drag_f2));
    }
    // Hash the committed points grid (only if non-empty to save time)
    if (ds.box_sel_pts && ds.box_sel_count > 0) {
        for (int r = 0; r < BOARD_SIZE; r++)
            for (int f = 0; f < BOARD_SIZE; f++)
                mix8(uint8_t(ds.box_sel_pts[r][f]));
    }

    // Mode flags
    mix8(uint8_t(ds.analysis_mode));
    mix8(uint8_t(ds.game_mode));
    mix8(uint8_t(ds.guess_mode));
    mix8(uint8_t(ds.chain_mode));
    mix8(uint8_t(ds.free_mode));
    mix64(uint64_t(ds.active_board_size));
    mix8(uint8_t(ds.show_help));
    mix8(uint8_t(ds.territory_drill));
    mix64(uint64_t(ds.guess_score));

    // Turn / liberty overlay
    if (ds.analysis) {
        mix8(uint8_t(ds.analysis->turn_is_black));
        mix64(uint64_t(ds.analysis->liberty_count));
        mix64(uint64_t(ds.analysis->liberty_display_r));
        mix64(uint64_t(ds.analysis->liberty_display_f));
        mix64(uint64_t(ds.analysis->selected_group_count));
    }
    mix64(uint64_t(ds.game.liberty_count));
    mix8(uint8_t(ds.game.game_finished));

    // Catalog (only need to track when active)
    mix8(uint8_t(ds.catalog.active));
    if (ds.catalog.active) {
        mix64(uint64_t(ds.catalog.index));
        mix64(uint64_t(ds.catalog.scroll));
        mix8(uint8_t(ds.catalog_thumb_valid));
        mix8(uint8_t(ds.catalog.search_mode));
        mix8(uint8_t(ds.catalog.game_index.loaded()));
        mix8(uint8_t(ds.catalog.game_index.is_loading()));
        mix8(uint8_t(ds.catalog.virtual_year_mode));
        mix_str(ds.catalog.virtual_year);
        mix8(uint8_t(ds.catalog.virtual_player_mode));
        mix_str(ds.catalog.virtual_player);
        mix_str(ds.catalog.current_subdir);
        mix_str(ds.catalog.search_query);
        mix64(uint64_t(ds.catalog.entries.size()));
    }

    // HUD text
    mix_str(ds.black_name);
    mix_str(ds.white_name);
    mix_str(ds.result_message);
    mix_str(ds.game_date);
    mix_str(ds.game_comment);

    // Speed message: hash whether it is currently visible (and the delay value)
    Uint32 now = SDL_GetTicks();
    bool speed_on = ds.speed_message_until > 0 && now < ds.speed_message_until;
    mix8(uint8_t(speed_on));
    if (speed_on) mix64(uint64_t(ds.move_delay_ms));

    // Flash message
    bool flash_on = ds.flash_message_until > 0 && now < ds.flash_message_until;
    mix8(uint8_t(flash_on));
    if (flash_on) mix_str(ds.flash_message);

    // Save input overlay
    mix8(uint8_t(ds.save_input_step));
    if (ds.save_input_step) mix_str(ds.save_input_buf);

    // Territory drill answer state
    if (ds.territory_drill) {
        mix64(uint64_t(ds.territory_b_score));
        mix64(uint64_t(ds.territory_w_score));
        mix8(uint8_t(ds.territory_answered));
        mix8(uint8_t(ds.territory_correct));
    }

    return h;
}

// Render all cast shadows (stones + chain bars) into a temporary texture using
// max-alpha blending, then composite once onto the current render target.
// This prevents shadows from stacking darker at intersections.
void Renderer::render_all_shadows(const BoardView& view,
                                   const char board[][MAX_BOARD_SIZE],
                                   bool chain_mode, int stone_filter, int n) {
    int w, h;
    SDL_GetRendererOutputSize(sdl, &w, &h);

    SDL_Texture* prev_target = SDL_GetRenderTarget(sdl);

    SDL_Texture* shadow_tex = SDL_CreateTexture(sdl, SDL_PIXELFORMAT_ARGB8888,
                                                SDL_TEXTUREACCESS_TARGET, w, h);
    if (!shadow_tex) return;
    SDL_SetTextureBlendMode(shadow_tex, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(sdl, shadow_tex);
    SDL_SetRenderDrawColor(sdl, 0, 0, 0, 0);
    SDL_RenderClear(sdl);

    // Max-alpha blend: overlapping shadows take the maximum darkness, not the sum.
    SDL_BlendMode max_blend = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_MAXIMUM,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_MAXIMUM);
    SDL_SetRenderDrawBlendMode(sdl, max_blend);

    // Stone shadows
    for (int r = 0; r < n; r++)
        for (int f = 0; f < n; f++) {
            int cell = board[r][f];
            if (cell == 0) continue;
            int is_black = (cell == 1);
            if (stone_filter == 1 && !is_black) continue;
            if (stone_filter == 2 &&  is_black) continue;
            draw_stone_circle(view, r, f, is_black, 255, /*shadow_pass=*/true);
        }

    // Link shadows (blend mode already set; draw_stone_link shadow pass won't override it)
    render_chain_connections(view, board, chain_mode, stone_filter, /*shadows_only=*/true);

    // Composite shadow texture onto board with standard BLEND (darkens board by shadow alpha)
    SDL_SetRenderTarget(sdl, prev_target);
    SDL_RenderCopy(sdl, shadow_tex, nullptr, nullptr);
    SDL_DestroyTexture(shadow_tex);
}

// Draws board+HUD to whatever render target is currently active.
// Does NOT draw the software cursor or call SDL_RenderPresent.
void Renderer::render_board_content(const BoardView& view, const Overlay* overlay, const DrawState& ds) {
    SDL_SetRenderDrawColor(sdl, Palette::BACKGROUND.r, Palette::BACKGROUND.g, Palette::BACKGROUND.b, 255);
    SDL_RenderClear(sdl);

    // Board background
    int bg = view.board_px + 2 * view.margin;
    SDL_Rect board_rect = {view.offset_x - view.margin, view.offset_y - view.margin, bg, bg};
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
    SDL_RenderFillRect(sdl, &board_rect);

    // Grid lines
    SDL_Color grid_color = Palette::GRID;
    int n          = view.active_size;
    int normal_t   = (view.square >= 30) ? 2 : 1;
    int boundary_t = normal_t * 2;
    int boundary_idx[2] = {0, n - 1};
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
    for (int i = 1; i < n - 1; i++) {
        int y  = view.offset_y + i * view.square + view.square / 2;
        int x0 = view.offset_x + view.square / 2;
        int x1 = view.offset_x + (n - 1) * view.square + view.square / 2;
        draw_thick_line(x0, y, x1, y, normal_t, grid_color);
        int x  = view.offset_x + i * view.square + view.square / 2;
        int y0 = view.offset_y + view.square / 2;
        int y1 = view.offset_y + (n - 1) * view.square + view.square / 2;
        draw_thick_line(x, y0, x, y1, normal_t, grid_color);
    }

    // Star points (hoshi) — computed for the active board size
    {
        int star_r = (view.square >= 30) ? 4 : 3;
        SDL_SetRenderDrawColor(sdl, 0, 0, 0, 255);
        // offset from edge (0-indexed); also used for mid-edge and tengen
        int ho   = (n >= 13) ? 3 : 2;
        int ctr  = n / 2;
        // Build the set of hoshi positions
        int pts[9][2]; int npts = 0;
        if (n >= 7) {
            // Four corner hoshi
            pts[npts][0] = ho;      pts[npts][1] = ho;      npts++;
            pts[npts][0] = ho;      pts[npts][1] = n-1-ho;  npts++;
            pts[npts][0] = n-1-ho;  pts[npts][1] = ho;      npts++;
            pts[npts][0] = n-1-ho;  pts[npts][1] = n-1-ho;  npts++;
            // Tengen (centre) if n is odd
            if (n % 2 == 1) { pts[npts][0] = ctr; pts[npts][1] = ctr; npts++; }
        }
        // For 19×19 and 13×13: mid-edge hoshi
        if (n == 19) {
            pts[npts][0] = ho;  pts[npts][1] = ctr;  npts++;
            pts[npts][0] = n-1-ho; pts[npts][1] = ctr; npts++;
            pts[npts][0] = ctr; pts[npts][1] = ho;   npts++;
            pts[npts][0] = ctr; pts[npts][1] = n-1-ho; npts++;
        }
        for (int i = 0; i < npts; i++) {
            int x = view.offset_x + pts[i][1] * view.square + view.square / 2;
            int y = view.offset_y + pts[i][0] * view.square + view.square / 2;
            SDL_Rect sr = {x - star_r, y - star_r, star_r * 2, star_r * 2};
            SDL_RenderFillRect(sdl, &sr);
        }
    }

    // Choose board array and liberty state depending on mode
    const char (*active_board)[MAX_BOARD_SIZE] =
        ds.territory_board ? ds.territory_board :
        (ds.analysis_mode && ds.analysis ? ds.analysis->board : ds.game.board);
    const int* lib_r   = ds.analysis_mode && ds.analysis ? ds.analysis->liberty_r  : ds.game.liberty_r;
    const int* lib_f   = ds.analysis_mode && ds.analysis ? ds.analysis->liberty_f  : ds.game.liberty_f;
    int lib_count      = (ds.territory_drill || !(ds.analysis_mode && ds.analysis))
                         ? (ds.territory_drill ? 0 : ds.game.liberty_count)
                         : ds.analysis->liberty_count;

    // Layered render — all shadows composited once (no additive stacking),
    // then cylinders, then stone fills on top.
    render_all_shadows(view, active_board, ds.chain_mode, ds.stone_filter, n);
    render_chain_connections(view, active_board, ds.chain_mode, ds.stone_filter, /*shadows_only=*/false);
    for (int r = 0; r < n; r++)
        for (int f = 0; f < n; f++) {
            int cell = active_board[r][f];
            if (cell == 0) continue;
            int is_black = (cell == 1);
            if (ds.stone_filter == 1 && !is_black) continue;
            if (ds.stone_filter == 2 &&  is_black) continue;
            draw_stone_circle(view, r, f, is_black, 255, /*shadow_pass=*/false);
        }

    // Move-number overlay
    if (ds.show_move_numbers) {
        // Build num/col grids from the appropriate source:
        //   analysis mode → App's persistent grids (never touched by captures)
        //   playback      → raw SGF arrays (position-based, also capture-immune)
        int num_grid[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
        int col_grid[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};

        if (ds.analysis_mode && ds.analysis_num_grid && ds.analysis_col_grid) {
            for (int r = 0; r < n; r++)
                for (int f = 0; f < n; f++) {
                    num_grid[r][f] = ds.analysis_num_grid[r][f];
                    col_grid[r][f] = ds.analysis_col_grid[r][f];
                }
        } else if (!ds.analysis_mode && ds.sgf_moves && ds.sgf_colors) {
            for (int i = 0; i < ds.sgf_game_index; i++) {
                const char* mv = ds.sgf_moves[i];
                if (strlen(mv) == 2
                    && mv[0] >= 'a' && mv[0] <= 's'
                    && mv[1] >= 'a' && mv[1] <= 's') {
                    int f = mv[0] - 'a';
                    int r = mv[1] - 'a';
                    num_grid[r][f] = i + 1;
                    col_grid[r][f] = ds.sgf_colors[i];
                }
            }
        }

        int half   = view.square / 2;
        int radius = view.square / 2 - 2;
        for (int r = 0; r < n; r++) {
            for (int f = 0; f < n; f++) {
                if (num_grid[r][f] == 0) continue;
                int  num      = num_grid[r][f];
                int  is_black = col_grid[r][f];
                bool captured = (active_board[r][f] == 0);
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", num);
                int ndigits = (int)strlen(buf);
                int scale   = 1;
                if (radius >= 14 && ndigits <= 2) scale = 2;
                if (radius >= 20 && ndigits == 1) scale = 3;
                int tw = text_width_px(buf, scale);
                int th = 7 * scale;
                int cx = view.offset_x + f * view.square + half;
                int cy = view.offset_y + r * view.square + half;
                int tx = cx - tw / 2;
                int ty = cy - th / 2;
                if (captured) {
                    // Faint ghost circle so the number reads at an empty intersection
                    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
                    Uint8 bv = is_black ? 30 : 200;
                    SDL_SetRenderDrawColor(sdl, bv, bv, bv, 110);
                    fill_circle(cx, cy, radius * 3 / 4);
                }
                SDL_Color tc = captured
                    ? Palette::ACCENT
                    : (is_black ? SDL_Color{230, 230, 230, 255}
                                : SDL_Color{40,  40,  40,  255});
                draw_text(tx, ty, scale, buf, tc);
            }
        }
    }

    // Liberty dots
    render_liberties(view, lib_r, lib_f, lib_count);

    // Stone preview overlay
    if (overlay && overlay->active) {
        int r = -1, f = -1;
        if (screen_to_board(view, (int)overlay->x, (int)overlay->y, r, f))
            draw_stone_circle(view, r, f, overlay->is_black, 128);
    }

    if (!ds.territory_drill && !ds.free_mode) {
        render_game_comment(view, ds);
        render_player_labels(view, ds);
        render_speed_label(view, ds.move_delay_ms, ds.speed_message_until);
        render_guess_score(view, ds.guess_mode, ds.guess_score);
        render_result_message(view, ds);
        render_game_date(view, ds.game_date);
    }
    if (!ds.free_mode)
        render_mode_status(view, ds.analysis_mode, ds.game_mode, ds.guess_mode, ds.territory_drill, false);
    render_territory_overlay(view, ds);

    render_box_selection(view, ds);
    render_help_overlay(view, ds.show_help);
    render_catalog_overlay(view, ds);
    render_save_input(view, ds);
    render_flash_message(view, ds);
    if (ds.quit_confirm) render_quit_confirm(view);
}

// Public entry point: uses a cached texture for the board+HUD so that
// cursor-only frames (mouse movement) are essentially free.
void Renderer::render_board(const BoardView& view, const Overlay* overlay, const DrawState& ds) {
    uint64_t h = compute_cache_hash(ds);

    if (h != cache_hash_) {
        // (Re)create the cache texture if the output size changed
        int out_w = 0, out_h = 0;
        SDL_GetRendererOutputSize(sdl, &out_w, &out_h);
        if (!board_cache_ || cache_w_ != out_w || cache_h_ != out_h) {
            if (board_cache_) SDL_DestroyTexture(board_cache_);
            board_cache_ = SDL_CreateTexture(sdl,
                SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                out_w, out_h);
            cache_w_ = out_w;
            cache_h_ = out_h;
        }
        SDL_SetRenderTarget(sdl, board_cache_);
        render_board_content(view, overlay, ds);
        SDL_SetRenderTarget(sdl, nullptr);
        cache_hash_ = h;
    }

    // Blit cached board, then draw the cursor on top (always, every frame)
    SDL_RenderCopy(sdl, board_cache_, nullptr, nullptr);
    render_software_cursor(view, ds);
    if (!ds.suppress_present)
        SDL_RenderPresent(sdl);
}

// ---------------------------------------------------------------------------
// Software cursor (drawn directly in renderer — no OS scaling involved)

void Renderer::draw_stone_at_px(int cx, int cy, int radius, int is_black, Uint8 alpha) {
    shade_stone(cx, cy, radius, is_black, alpha);
}

void Renderer::render_software_cursor(const BoardView& view, const DrawState& ds) {
    if (ds.cursor_type == 0 || ds.cursor_x < 0 || ds.cursor_y < 0) return;
    int cx = ds.cursor_x, cy = ds.cursor_y;
    int sq = view.square > 0 ? view.square : 32;

    if (ds.cursor_type >= 2) {
        // Stone cursor: filled circle with a yellow border ring
        int is_black = (ds.cursor_type == 3);
        int radius   = sq / 2 - 2;
        if (radius < 2) radius = 2;
        draw_stone_at_px(cx, cy, radius, is_black, 255);
    } else {
        // Crosshair cursor: yellow with dark shadow, center gap, 3/4-length arms
        int arm = sq * 3 / 8;            // half-arm length from centre
        int gap = std::max(1, sq / 12);  // gap around the hotspot
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        // Draw shadow (offset 1,1) then accent colour on top
        const struct { int ox, oy; Uint8 r, g, b, a; } passes[2] = {
            {1, 1,   0,   0,  0, 140},
            {0, 0, Palette::ACCENT.r, Palette::ACCENT.g, Palette::ACCENT.b, 255},
        };
        for (auto& p : passes) {
            SDL_Color col = {p.r, p.g, p.b, p.a};
            // left arm
            draw_thick_line(cx+p.ox-arm, cy+p.oy, cx+p.ox-gap-1, cy+p.oy, 2, col);
            // right arm
            draw_thick_line(cx+p.ox+gap, cy+p.oy, cx+p.ox+arm,   cy+p.oy, 2, col);
            // top arm
            draw_thick_line(cx+p.ox, cy+p.oy-arm, cx+p.ox, cy+p.oy-gap-1, 2, col);
            // bottom arm
            draw_thick_line(cx+p.ox, cy+p.oy+gap, cx+p.ox, cy+p.oy+arm,   2, col);
        }
    }
}
