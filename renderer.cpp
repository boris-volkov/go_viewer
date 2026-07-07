#include "renderer.hpp"
#include "palette.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Font data (5x7 pixel glyphs, MSB = leftmost column)

const Renderer::Glyph Renderer::font_glyphs[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'=', {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'*', {0x00,0x00,0x00,0x0C,0x0C,0x00,0x00}},  // centered dot — menu focus cursor
    // PlayStation face-button shapes, reachable via the GLYPH_PS_* placeholder
    // strings (renderer.hpp). Kept vertically centered with blank top/bottom rows
    // so the four read as a matched set at any scale.
    {'~', {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}},  // cross    (Xbox A)
    {'@', {0x00,0x0E,0x11,0x11,0x11,0x0E,0x00}},  // circle   (Xbox B)
    {'#', {0x00,0x1F,0x11,0x11,0x11,0x1F,0x00}},  // square   (Xbox X)
    {'^', {0x00,0x04,0x0A,0x0A,0x11,0x1F,0x00}},  // triangle (Xbox Y)
    {'%', {0x18,0x18,0x08,0x04,0x02,0x03,0x03}},
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
    for (const auto& g : font_glyphs)
        if (g.c == '?') return g.rows;
    return font_glyphs[0].rows; // unreachable: '?' is always present
}

// The bitmap font is single-byte/ASCII only. Status text throughout the app uses a
// UTF-8 em dash (U+2014, 3 bytes: E2 80 94) as a decorative separator; render it as
// a hyphen (same 5x7 cell) rather than 3 back-to-back missing-glyph boxes, and advance
// the read cursor past all 3 bytes. Any other multi-byte sequence just falls through
// to get_glyph_rows() byte-by-byte (rendered as the '?' fallback per byte).
// Caller guarantees p[0] != '\0'; each subsequent byte is only read once the prior
// one is confirmed non-null, so this never reads past the string's terminator.
static bool is_utf8_em_dash(const char* p) {
    if ((unsigned char)p[0] != 0xE2) return false;
    if (p[1] == 0 || (unsigned char)p[1] != 0x80) return false;
    if (p[2] == 0 || (unsigned char)p[2] != 0x94) return false;
    return true;
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
    int count = 0;
    for (const char* p = text; *p; count++)
        p += is_utf8_em_dash(p) ? 3 : 1;
    if (count <= 0) return 0;
    return (count * 6 - 1) * scale;
}

// PlayStation face buttons always render in their standard pad colors, regardless
// of the surrounding text color — matches the physical controller and makes the
// button symbols pop out of any prompt they appear in.
static bool ps_button_color(char c, SDL_Color& out) {
    switch (c) {
    case '~': out = {124, 178, 232, 255}; return true;  // cross    — blue
    case '@': out = {240, 105, 105, 255}; return true;  // circle   — red
    case '#': out = {232, 130, 190, 255}; return true;  // square   — pink
    case '^': out = { 80, 205, 150, 255}; return true;  // triangle — green
    default:  return false;
    }
}

void Renderer::draw_text(int x, int y, int scale, const char* text, SDL_Color color) {
    SDL_SetRenderDrawColor(sdl, color.r, color.g, color.b, color.a);
    int pen_x = x;
    for (const char* p = text; *p; ) {
        bool em_dash = is_utf8_em_dash(p);
        char c = em_dash ? '-' : *p;
        const unsigned char* rows = get_glyph_rows(c);
        p += em_dash ? 3 : 1;
        SDL_Color pc;
        bool ps = ps_button_color(c, pc);
        if (ps) SDL_SetRenderDrawColor(sdl, pc.r, pc.g, pc.b, pc.a);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (rows[row] & (1 << (4 - col))) {
                    SDL_Rect rect = {pen_x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(sdl, &rect);
                }
            }
        }
        if (ps) SDL_SetRenderDrawColor(sdl, color.r, color.g, color.b, color.a);
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

void Renderer::draw_circle(int cx, int cy, int radius) {
    if (radius < 0) return;
    int x = radius, y = 0, err = 0;
    while (x >= y) {
        SDL_RenderDrawPoint(sdl, cx + x, cy + y);
        SDL_RenderDrawPoint(sdl, cx + y, cy + x);
        SDL_RenderDrawPoint(sdl, cx - y, cy + x);
        SDL_RenderDrawPoint(sdl, cx - x, cy + y);
        SDL_RenderDrawPoint(sdl, cx - x, cy - y);
        SDL_RenderDrawPoint(sdl, cx - y, cy - x);
        SDL_RenderDrawPoint(sdl, cx + y, cy - x);
        SDL_RenderDrawPoint(sdl, cx + x, cy - y);
        if (err <= 0) { y++; err += 2 * y + 1; }
        if (err >  0) { x--; err -= 2 * x + 1; }
    }
}

void Renderer::fill_ring(int cx, int cy, int r_outer, int r_inner) {
    for (int dy = -r_outer; dy <= r_outer; dy++) {
        int dx_o = (int)sqrtf(float(r_outer * r_outer - dy * dy));
        int abs_dy = (dy < 0) ? -dy : dy;
        if (abs_dy < r_inner) {
            int dx_i = (int)sqrtf(float(r_inner * r_inner - dy * dy));
            SDL_RenderDrawLine(sdl, cx - dx_o, cy + dy, cx - dx_i, cy + dy);
            SDL_RenderDrawLine(sdl, cx + dx_i, cy + dy, cx + dx_o, cy + dy);
        } else {
            SDL_RenderDrawLine(sdl, cx - dx_o, cy + dy, cx + dx_o, cy + dy);
        }
    }
}

// Scanline fill of a rotated ellipse.
// ra = semi-axis along (ux,uy),  rb = semi-axis along the perpendicular (-uy,ux).
// Iterates over integer steps in the perpendicular direction.
void Renderer::fill_ellipse_rotated(int cx, int cy, float ux, float uy, int ra, int rb) {
    if (ra < 1 || rb < 1) return;
    float px = -uy, py = ux;   // perpendicular unit vector
    for (int t = -rb; t <= rb; t++) {
        float frac = (float)t / rb;
        float span = ra * sqrtf(std::max(0.f, 1.f - frac * frac));
        int   ispan = (int)span;
        float ox = px * t, oy = py * t;
        SDL_RenderDrawLine(sdl,
            cx + (int)(ox - ux * ispan), cy + (int)(oy - uy * ispan),
            cx + (int)(ox + ux * ispan), cy + (int)(oy + uy * ispan));
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
        return { {18, 20, 25, 255}, {72, 66, 58, 255}, {10, 12, 16, 255}, 0.14f, 0.22f, 0.16f };
    else
        return { {210, 214, 220, 255}, {255, 252, 240, 255}, {178, 182, 190, 255}, 0.35f, 0.28f, 0.20f };
}

void Renderer::shade_stone(int cx, int cy, int radius, int is_black, Uint8 alpha, bool shadow_pass) {
    // Square stone mode: flat tiles with a subtle top-left/bottom-right bevel,
    // same footprint and shadow offsets as the round stones.
    if (square_stones_) {
        SDL_Rect body = {cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1};
        if (shadow_pass) {
            if (radius >= 4) {
                int sox = radius / 5 + 1;
                SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.18f));
                SDL_Rect s1 = {body.x + sox + 1, body.y + sox + 1, body.w + 2, body.h + 2};
                SDL_RenderFillRect(sdl, &s1);
                SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.30f));
                SDL_Rect s2 = {body.x + sox, body.y + sox, body.w + 1, body.h + 1};
                SDL_RenderFillRect(sdl, &s2);
            }
            return;
        }
        auto c = stone_colors(is_black);
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sdl, c.base.r, c.base.g, c.base.b, alpha);
        SDL_RenderFillRect(sdl, &body);

        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        int bevel = std::max(1, radius / 4);
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha2));
        SDL_Rect top  = {body.x, body.y, body.w, bevel};
        SDL_Rect left = {body.x, body.y, bevel, body.h};
        SDL_RenderFillRect(sdl, &top);
        SDL_RenderFillRect(sdl, &left);
        SDL_SetRenderDrawColor(sdl, c.dark.r, c.dark.g, c.dark.b, (Uint8)(alpha * 0.8f));
        SDL_Rect bot   = {body.x, body.y + body.h - bevel, body.w, bevel};
        SDL_Rect right = {body.x + body.w - bevel, body.y, bevel, body.h};
        SDL_RenderFillRect(sdl, &bot);
        SDL_RenderFillRect(sdl, &right);
        return;
    }

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

    // Highlight layers clipped to the stone boundary — h1's offset+radius exceeds the
    // stone radius by ~7%, causing a lit halo outside the stone without this clipping.
    auto clip_fill = [&](int hx, int hy, int hr) {
        int r2 = radius * radius;
        for (int dy = -hr; dy <= hr; dy++) {
            int py = hy + dy;
            int hdx = (int)sqrtf((float)(hr * hr - dy * dy));
            int xl = hx - hdx, xr = hx + hdx;
            // Intersect with stone circle row
            int sdy = py - cy;
            int sclip2 = r2 - sdy * sdy;
            if (sclip2 < 0) continue;
            int sdx = (int)sqrtf((float)sclip2);
            xl = std::max(xl, cx - sdx);
            xr = std::min(xr, cx + sdx);
            if (xl <= xr) SDL_RenderDrawLine(sdl, xl, py, xr, py);
        }
    };

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    if (c.alpha1 > 0.f) {
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha1));
        clip_fill(h1x, h1y, radius * 5 / 6);
    }
    if (c.alpha2 > 0.f) {
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha2));
        clip_fill(h2x, h2y, radius * 2 / 3);
    }
    if (c.alpha3 > 0.f) {
        SDL_SetRenderDrawColor(sdl, c.lit.r, c.lit.g, c.lit.b, (Uint8)(alpha * c.alpha3));
        clip_fill(h3x, h3y, radius / 2);
    }
}

// Draw a chain link as a shaded cylinder.
// shadow_pass=true  → only the soft dark shadow quad (drawn first, over all board lines)
// shadow_pass=false → only the lit cylinder (drawn second, on top of all shadows)
void Renderer::draw_stone_link(int x1, int y1, int x2, int y2, int thickness, int is_black, bool shadow_pass, int stone_radius) {
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

    // Cylinder pass — discrete layered shading matching the stone:
    //   base fill (solid c.base) + lit overlay strips narrower and shifted toward the lit side,
    //   using the same alpha1/2/3 fractions as shade_stone.
    auto c = stone_colors(is_black);

    float ux = fdx / len, uy = fdy / len;  // unit vector A→B

    float overlap = thickness * 0.08f + 1.f;
    float shorten = std::max(0.f, stone_radius - overlap);
    float sx = x1 + ux * shorten, sy = y1 + uy * shorten;
    float ex = x2 - ux * shorten, ey = y2 - uy * shorten;

    float half = thickness * 0.5f;
    float bx = nx * half, by = ny * half;   // full half-width perpendicular vectors

    // Determine which perpendicular side faces the upper-left light
    bool flip = (nx * (-0.707f) + ny * (-0.707f)) < 0.f;
    float lit_nx = flip ? -nx : nx, lit_ny = flip ? -ny : ny;  // lit-side unit vector

    // Layer descriptors.  off_frac is centre offset toward lit side as a fraction of
    // half-thickness; w_frac is half-width as the same fraction.  Strips are painted
    // outermost first (col1) then progressively narrower ones on top, with BLENDMODE_NONE,
    // so they don't interact.
    //
    // The broad first strip (off=0.25, w=0.75) extends 0.5*half into the DARK side — just
    // like the stone's large lit circle which covers ~83% of the sphere and leaves only a
    // thin dark rim.  This means ~75% of the bar has at least col1 brightness, matching the
    // stone's proportions and making all three tones clearly visible at any board size.
    struct Layer { float off_frac; float w_frac; float alpha; };
    const Layer layers[] = {
        { 0.25f, 0.75f, c.alpha1 },   // broad:  ~75% of bar (extends into dark side)
        { 0.55f, 0.45f, c.alpha2 },   // medium: lit side of centre
        { 0.80f, 0.20f, c.alpha3 },   // tight:  near lit edge
    };

    // 1. Base fill — solid c.base rectangle
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    {
        SDL_Color col = { c.base.r, c.base.g, c.base.b, 255 };
        SDL_Vertex v[4] = {
            {{sx - bx, sy - by}, col, {0,0}},
            {{sx + bx, sy + by}, col, {0,0}},
            {{ex + bx, ey + by}, col, {0,0}},
            {{ex - bx, ey - by}, col, {0,0}},
        };
        int idx[6] = {0,1,2, 0,2,3};
        SDL_RenderGeometry(sdl, nullptr, v, 4, idx, 6);
    }

    // Pre-compute compounded opaque colors for each layer — same compound as shade_stone
    // uses with BLENDMODE_BLEND, but expressed as flat opaque values so no triangle-edge
    // double-compositing can create stripe artefacts.
    auto compose = [](SDL_Color base, SDL_Color lit, float alpha) -> SDL_Color {
        return { (Uint8)(base.r + (lit.r - base.r) * alpha),
                 (Uint8)(base.g + (lit.g - base.g) * alpha),
                 (Uint8)(base.b + (lit.b - base.b) * alpha), 255 };
    };
    SDL_Color col1 = compose(c.base, c.lit, c.alpha1);
    SDL_Color col2 = compose(col1,   c.lit, c.alpha2);   // compound: layer2 on top of layer1
    SDL_Color col3 = compose(col2,   c.lit, c.alpha3);   // compound: layer3 on top of both
    const SDL_Color layer_cols[3] = { col1, col2, col3 };

    // 2. Lit overlay strips — narrower, shifted toward lit side, drawn opaque outermost→innermost
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    for (int i = 0; i < 3; i++) {
        const Layer& L = layers[i];
        if (L.alpha <= 0.f) continue;
        float perp_off = half * L.off_frac;
        float lhalf    = thickness * L.w_frac * 0.5f;
        float lbx = nx * lhalf, lby = ny * lhalf;
        float ox2 = lit_nx * perp_off, oy2 = lit_ny * perp_off;
        SDL_Vertex v[4] = {
            {{sx + ox2 - lbx, sy + oy2 - lby}, layer_cols[i], {0,0}},
            {{sx + ox2 + lbx, sy + oy2 + lby}, layer_cols[i], {0,0}},
            {{ex + ox2 + lbx, ey + oy2 + lby}, layer_cols[i], {0,0}},
            {{ex + ox2 - lbx, ey + oy2 - lby}, layer_cols[i], {0,0}},
        };
        int idx[6] = {0,1,2, 0,2,3};
        SDL_RenderGeometry(sdl, nullptr, v, 4, idx, 6);
    }

    // 3. End caps — ellipses at bar endpoints, shaded with the same discrete layers.
    // ra = semi-axis along the bar direction (controls roundness; small = flat disc).
    // rb = half-width of the bar (perpendicular to bar).
    // Using BLENDMODE_NONE + pre-computed flat colors avoids any per-scanline gradient
    // banding (that was the old ribbing cause).
    {
        int cap_rb = (int)half;                            // full bar half-width
        int cap_ra = std::max(1, (int)(half * 0.35f));    // roundness along bar axis

        auto draw_cap = [&](float cx, float cy) {
            // Base disc
            SDL_SetRenderDrawColor(sdl, c.base.r, c.base.g, c.base.b, 255);
            fill_ellipse_rotated((int)cx, (int)cy, ux, uy, cap_ra, cap_rb);
            // Lit overlay ellipses — same perpendicular offsets/widths as bar strips
            for (int i = 0; i < 3; i++) {
                const Layer& L = layers[i];
                if (L.alpha <= 0.f) continue;
                float perp_off = half * L.off_frac;
                int lrb = std::max(1, (int)(half * L.w_frac));
                float ocx = cx + lit_nx * perp_off;
                float ocy = cy + lit_ny * perp_off;
                SDL_SetRenderDrawColor(sdl, layer_cols[i].r, layer_cols[i].g, layer_cols[i].b, 255);
                fill_ellipse_rotated((int)ocx, (int)ocy, ux, uy, cap_ra, lrb);
            }
        };

        draw_cap(sx, sy);
        draw_cap(ex, ey);
    }
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
                    int thick    = (view.square - 4) / 2;
                    int stone_r  = view.square / 2 - 2;
                    draw_stone_link(x1, y1, x2, y2, thick, color, shadows_only, stone_r);
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
    char b_pris[32], w_pris[32];
    if (ds.live_mode) {
        // Show clock on line 2; for byo-yomi show period info when main time is gone
        auto fmt_clock = [](int secs, int periods, int period_secs, char* buf, size_t n) {
            if (secs < 0) { snprintf(buf, n, "--:--"); return; }
            if (secs > 0 || periods <= 0) {
                // Main time remaining (or no period info)
                snprintf(buf, n, "%d:%02d", secs / 60, secs % 60);
                if (periods > 0)  // in byo-yomi but period countdown hasn't hit zero
                    snprintf(buf + strlen(buf), n - strlen(buf), " x%d", periods);
            } else {
                // Main time exhausted, in byo-yomi
                if (period_secs > 0)
                    snprintf(buf, n, "%ds x%d", period_secs, periods);
                else
                    snprintf(buf, n, "BYO x%d", periods);
            }
        };
        fmt_clock(ds.live_black_secs, ds.live_black_periods, ds.live_black_period_secs, bpstr, sizeof(bpstr));
        fmt_clock(ds.live_white_secs, ds.live_white_periods, ds.live_white_period_secs, wpstr, sizeof(wpstr));
        // Prisoners on their own line 3
        snprintf(b_pris, sizeof(b_pris), "Prisoners: %d", bp);
        snprintf(w_pris, sizeof(w_pris), "Prisoners: %d", wp);
    } else {
        snprintf(bpstr, sizeof(bpstr), "Prisoners: %d", bp);
        snprintf(wpstr, sizeof(wpstr), "Prisoners: %d", wp);
        b_pris[0] = '\0';
        w_pris[0] = '\0';
    }

    // Clocks render two steps larger than the name/prisoner lines in live mode —
    // the countdown is what actually gets read at a glance mid-game.
    int scale = 3;
    int avail = right_x1 - right_x0 - board_radius * 2 - gap;
    auto measure = [&](int sc, int clock_sc) {
        int w = text_width_px(bn, sc);
        { int ww = text_width_px(wn, sc); if (ww > w) w = ww; }
        // In live mode bpstr/wpstr are the clock lines (bigger scale);
        // otherwise they're the prisoner lines (normal scale).
        int line2_sc = ds.live_mode ? clock_sc : sc;
        { int pw = text_width_px(bpstr, line2_sc); if (pw > w) w = pw; }
        { int pw = text_width_px(wpstr, line2_sc); if (pw > w) w = pw; }
        if (b_pris[0]) { int pw = text_width_px(b_pris, sc); if (pw > w) w = pw; }
        if (w_pris[0]) { int pw = text_width_px(w_pris, sc); if (pw > w) w = pw; }
        return w;
    };
    int clock_scale = scale + 2;
    if (measure(scale, clock_scale) > avail) {
        scale       = 2;
        clock_scale = 4;
        if (measure(scale, clock_scale) > avail) {
            clock_scale = 3;  // last resort: shrink the clock before giving up
            if (measure(scale, clock_scale) > avail) return;
        }
    }

    int th       = 7 * scale;
    int clock_th = ds.live_mode ? 7 * clock_scale : th;
    int line_gap = scale + 2;
    int block_h  = ds.live_mode ? (th + line_gap + clock_th + line_gap + th)
                                : (2 * th + line_gap);

    // Cap radius to fit within the text block height; matches board size on smaller screens
    int radius    = std::min(board_radius, block_h / 2);
    int stone_dim = radius * 2;
    int tx        = right_x0 + stone_dim + gap;
    int scx       = right_x0 + radius;

    // In live mode, highlight whichever player's turn it is.
    SDL_Color black_name_color = (ds.live_mode && ds.game.turn_is_black == 1)
                                 ? Palette::ACCENT : Palette::TEXT_PRIMARY;
    SDL_Color white_name_color = (ds.live_mode && ds.game.turn_is_black == 0)
                                 ? Palette::ACCENT : Palette::TEXT_PRIMARY;
    SDL_Color prisoner_color = Palette::TEXT_DIM;
    // Red whenever the player is actually in byo-yomi — both while a period is
    // counting down (live_*_in_byo, computed by the clock tick, since the displayed
    // numbers alone can't distinguish a period countdown from low main time) and
    // while parked between moves with main time exhausted (secs<=0, periods>0).
    auto byo_color = [&](bool in_byo, int secs, int periods) -> SDL_Color {
        return (ds.live_mode && (in_byo || (secs <= 0 && periods > 0)))
               ? SDL_Color{220, 60, 60, 255}
               : Palette::TEXT_DIM;
    };
    SDL_Color b_clock_color = byo_color(ds.live_black_in_byo, ds.live_black_secs, ds.live_black_periods);
    SDL_Color w_clock_color = byo_color(ds.live_white_in_byo, ds.live_white_secs, ds.live_white_periods);

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

    // Line 2 (the clock in live mode) draws at clock_scale; line 3 sits below it.
    int line2_sc = ds.live_mode ? clock_scale : scale;
    int line3_y  = th + line_gap + clock_th + line_gap;

    // Black at the top — stone centered over the text block
    // Anchor to background top edge (one view.margin above the grid)
    int top_y = view.offset_y - view.margin + margin;
    draw_stone(scx, top_y + block_h / 2, true);
    draw_text(tx, top_y,                  scale,    bn,    black_name_color);
    draw_text(tx, top_y + th + line_gap,  line2_sc, bpstr, b_clock_color);
    if (b_pris[0])
        draw_text(tx, top_y + line3_y,    scale,    b_pris, prisoner_color);

    // White at the bottom — anchor to background bottom edge
    int bot_y = view.offset_y + view.board_px + view.margin - margin - block_h;
    if (bot_y < top_y) bot_y = top_y;
    draw_stone(scx, bot_y + block_h / 2, false);
    draw_text(tx, bot_y,                  scale,    wn,    white_name_color);
    draw_text(tx, bot_y + th + line_gap,  line2_sc, wpstr, w_clock_color);
    if (w_pris[0])
        draw_text(tx, bot_y + line3_y,    scale,    w_pris, prisoner_color);
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

void Renderer::render_help_overlay(const BoardView& view, bool show_help, bool live_mode) {
    if (!show_help) return;

    // key=nullptr  → section header (desc is label text)
    // key=""       → blank spacer
    // otherwise    → key in yellow, desc in white
    struct Row { const char* key; const char* desc; };
    static const Row rows_go[] = {
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
    // Live client: three columns — controller binding, keyboard/mouse binding,
    // action — so both input schemes read side by side in the same context.
    if (live_mode) {
        struct Row3 { const char* pad; const char* kb; const char* desc; };
        static const Row3 rows[] = {
            {nullptr,       nullptr,        "OGS CLIENT HELP"},
            {"",            "",             ""},
            {"PAD",         "KEYBOARD",     ""},   // column captions (desc empty → gray)
            {"",            "",             ""},
            {nullptr,       nullptr,        "LOBBY"},
            {GLYPH_PS_CROSS,"ENTER / F",    "FIND GAME"},
            {GLYPH_PS_SQUARE,"C",           "OPEN CATALOG"},
            {GLYPH_PS_TRIANGLE,"M",         "FREE ANALYSIS"},
            {"SHARE",       "S",            "MATCH SETTINGS"},
            {"",            "",             ""},
            {nullptr,       nullptr,        "GAME"},
            {"D-PAD/STICK", "ARROWS/MOUSE", "MOVE CURSOR"},
            {GLYPH_PS_CROSS,"ENTER/CLICK",  "PLACE STONE"},
            {GLYPH_PS_CIRCLE " x2","B x2",  "PASS"},
            {"OPT x2",      "R x2",         "RESIGN"},
            {GLYPH_PS_TRIANGLE " x2","M x2","MARK MOVE FOR REVIEW"},
            {GLYPH_PS_SQUARE,"C",           "UNDO (VS KATAGO)"},
            {"L2/R2",       ", . WHEEL",    "STEP MOVE HISTORY"},
            {"R2 HOLD",     "",             "SHOW LAST MOVE"},
            {"",            "",             ""},
            {nullptr,       nullptr,        "GAME OVER / REVIEW"},
            {"L2/R2",       ", . WHEEL",    "STEP MAIN LINE"},
            {GLYPH_PS_CIRCLE,"B / R.CLICK", "LABEL POINT (A-Z)"},
            {"L1/R1",       "[ ]",          "SWITCH BRANCH"},
            {GLYPH_PS_CROSS,"ENTER/CLICK",  "PLAY BRANCH STONE"},
            {GLYPH_PS_TRIANGLE " x2","M x2","MARK/UNMARK MOVE"},
            {GLYPH_PS_SQUARE,"C",           "OPEN CATALOG"},
            {"L3/R3",       "PGUP/PGDN",    "PREV/NEXT FILE"},
            {"OPT x2",      "R x2",         "BACK TO LOBBY"},
            {"SHARE",       "S",            "SETTINGS MENU"},
            {"",            "",             ""},
            {nullptr,       nullptr,        "CATALOG"},
            {"UP/DOWN",     "ARROWS/WHEEL", "NAVIGATE"},
            {GLYPH_PS_CROSS,"ENTER/CLICK",  "OPEN GAME"},
            {GLYPH_PS_TRIANGLE " x2","M x2","DELETE GAME"},
            {GLYPH_PS_CIRCLE,"B",           "CLOSE"},
            {"",            "",             ""},
            {"",            "ESC",          "TOGGLE HELP"},
            {"",            "Q x2",         "QUIT"},
        };
        int n = (int)(sizeof(rows) / sizeof(rows[0]));

        int scale    = (view.square >= 30) ? 3 : 2;
        int line_gap = (scale >= 3) ? 4 : 3;
        int th       = 7 * scale;
        int col_gap  = 6 * scale;

        int pad_w = 0, kb_w = 0;
        for (int i = 0; i < n; i++) {
            if (!rows[i].pad) continue;
            if (rows[i].pad[0]) pad_w = std::max(pad_w, text_width_px(rows[i].pad, scale));
            if (rows[i].kb[0])  kb_w  = std::max(kb_w,  text_width_px(rows[i].kb,  scale));
        }
        int desc_x = pad_w + col_gap + kb_w + col_gap;
        int max_w  = 0;
        for (int i = 0; i < n; i++) {
            int w = !rows[i].pad ? text_width_px(rows[i].desc, scale)
                                 : desc_x + text_width_px(rows[i].desc, scale);
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
        SDL_SetRenderDrawColor(sdl, Palette::OVERLAY_DARK.r, Palette::OVERLAY_DARK.g,
                               Palette::OVERLAY_DARK.b, Palette::OVERLAY_DARK.a);
        SDL_RenderFillRect(sdl, &bg);

        SDL_Color col_title  = Palette::TEXT_WHITE;
        SDL_Color col_header = {160, 160, 160, 255};
        SDL_Color col_key    = Palette::ACCENT;
        SDL_Color col_desc   = Palette::TEXT_SECONDARY;

        int ty = y + pad;
        for (int i = 0; i < n; i++) {
            if (!rows[i].pad) {
                SDL_Color hc = (i == 0) ? col_title : col_header;
                draw_text(x + pad, ty, scale, rows[i].desc, hc);
            } else {
                // Caption rows (empty desc) draw their key columns in gray
                SDL_Color kc = rows[i].desc[0] ? col_key : col_header;
                if (rows[i].pad[0])
                    draw_text(x + pad, ty, scale, rows[i].pad, kc);
                if (rows[i].kb[0])
                    draw_text(x + pad + pad_w + col_gap, ty, scale, rows[i].kb, kc);
                if (rows[i].desc[0])
                    draw_text(x + pad + desc_x, ty, scale, rows[i].desc, col_desc);
            }
            ty += th + line_gap;
        }
        return;
    }

    // go_viewer keeps the original two-column table
    const Row* rows = rows_go;
    int n = (int)(sizeof(rows_go) / sizeof(rows_go[0]));

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
                                  const char board[][BOARD_SIZE], int board_size) {
    // Background
    SDL_Rect bg = {x, y, size, size};
    SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
    SDL_RenderFillRect(sdl, &bg);

    int sq  = size / board_size;                    // pixels per intersection
    if (sq < 1) sq = 1;
    int margin = (size - board_size * sq) / 2;      // center the grid in the box
    int px0 = x + margin + sq / 2;                 // top-left intersection (x)
    int py0 = y + margin + sq / 2;                 // top-left intersection (y)
    int span = (board_size - 1) * sq;

    // Grid lines
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    for (int i = 0; i < board_size; i++) {
        SDL_RenderDrawLine(sdl, px0 + i * sq, py0, px0 + i * sq, py0 + span);
        SDL_RenderDrawLine(sdl, px0, py0 + i * sq, px0 + span, py0 + i * sq);
    }

    // Border
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 180);
    SDL_RenderDrawRect(sdl, &bg);

    // Stones
    int r_stone = sq / 2;
    if (r_stone < 1) r_stone = 1;
    for (int row = 0; row < board_size; row++) {
        for (int col = 0; col < board_size; col++) {
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

void Renderer::draw_match_menu(const MatchMenu& menu) {
    int sw, sh;
    SDL_GetRendererOutputSize(sdl, &sw, &sh);

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::GRID.r, Palette::GRID.g, Palette::GRID.b, 255);
    SDL_Rect bg = {0, 0, sw, sh};
    SDL_RenderFillRect(sdl, &bg);

    int scale    = (sw >= 900) ? 3 : 2;
    int th       = 7 * scale;
    int line_gap = (scale >= 3) ? 8 : 5;
    int line_h   = th + line_gap;
    int hpad     = 80;
    int col_w    = (sw - hpad * 2) / 3;
    int display_col = menu.ingame ? 0 : 2;

    int ty = hpad / 2;
    draw_text(hpad, ty, scale, menu.ingame ? "SETTINGS" : "MATCH SETTINGS", Palette::ACCENT);
    ty += th + line_gap * 3;

    // Mode toggle row — not shown mid-game (only DISPLAY settings apply there)
    if (!menu.ingame) {
        const char* ogs_lbl  = menu.katago_mode ? "[ ] OGS MATCH"    : "[X] OGS MATCH";
        const char* kata_lbl = menu.katago_mode ? "[X] VS KATAGO"     : "[ ] VS KATAGO";
        SDL_Color ogs_c  = menu.katago_mode ? Palette::TEXT_DIM : Palette::ACCENT;
        SDL_Color kata_c = menu.katago_mode ? Palette::ACCENT    : Palette::TEXT_DIM;
        draw_text(hpad,          ty, scale, ogs_lbl,  ogs_c);
        draw_text(hpad + col_w,  ty, scale, kata_lbl, kata_c);
        ty += th + line_gap * 2;
    }

    // Hint line (PlayStation button glyphs — Boris plays on a DualShock)
    if (menu.ingame)
        draw_text(hpad, ty, scale, GLYPH_PS_CROSS "=TOGGLE   DPAD=NAVIGATE   " GLYPH_PS_CIRCLE "=CLOSE", Palette::TEXT_DIM);
    else if (menu.katago_mode)
        draw_text(hpad, ty, scale, GLYPH_PS_CROSS "=SELECT   DPAD=NAVIGATE   L1=OGS MODE   OPT=PLAY", Palette::TEXT_DIM);
    else
        draw_text(hpad, ty, scale, GLYPH_PS_CROSS "=TOGGLE   DPAD=NAVIGATE   L1=VS KATAGO  OPT=SEARCH", Palette::TEXT_DIM);
    ty += th + line_gap * 4;

    int col_ty[3] = {ty, ty, ty};

    static const char* size_labels[3]  = {"9x9", "13x13", "19x19"};
    static const char* speed_labels[3] = {"FAST   (30s + 5x10)", "MEDIUM (5m + 5x30)", "SLOW   (20m + 5x30)"};
    static const char* str_labels[7]   = {
        "20 KYU", "15 KYU", "10 KYU", "5 KYU", "1 KYU", "1 DAN", "5 DAN"
    };
    static const char* display_labels[4] = {"SHOW COORDINATES", "ENGINE ANALYSIS", "CHAIN LINKS",
                                            "SQUARE STONES"};

    if (!menu.ingame) {
        // Column 0 header: BOARD SIZE
        draw_text(hpad,         col_ty[0], scale, "BOARD SIZE",   Palette::ACCENT);
        col_ty[0] += line_h + line_gap;

        // Column 1 header: TIME CONTROL or STRENGTH
        draw_text(hpad + col_w, col_ty[1], scale,
                  menu.katago_mode ? "STRENGTH" : "TIME CONTROL", Palette::ACCENT);
        col_ty[1] += line_h + line_gap;
    }

    // Column 2 (or 0 when ingame) header: DISPLAY
    draw_text(hpad + display_col * col_w, col_ty[display_col], scale, "DISPLAY", Palette::ACCENT);
    col_ty[display_col] += line_h + line_gap;

    // checkbox item (multi-select)
    auto draw_check = [&](int col, int row, const char* label, bool selected) {
        int x = hpad + col * col_w;
        int y = col_ty[col] + row * line_h;
        bool focused = (menu.focus_col == col && menu.focus_row == row);
        draw_text(x, y, scale, focused ? "*" : " ", focused ? Palette::ACCENT : Palette::TEXT_DIM);
        int cx = x + (scale >= 3 ? 14 : 10);
        draw_text(cx, y, scale, selected ? "[X]" : "[ ]",
                  selected ? Palette::ACCENT : Palette::TEXT_WHITE);
        int lx = cx + text_width_px("[X]", scale) + scale * 3;
        draw_text(lx, y, scale, label,
                  focused ? Palette::ACCENT : selected ? Palette::TEXT_WHITE : Palette::TEXT_DIM);
    };

    // radio item (single-select)
    auto draw_radio = [&](int col, int row, const char* label, bool selected) {
        int x = hpad + col * col_w;
        int y = col_ty[col] + row * line_h;
        bool focused = (menu.focus_col == col && menu.focus_row == row);
        draw_text(x, y, scale, focused ? "*" : " ", focused ? Palette::ACCENT : Palette::TEXT_DIM);
        int cx = x + (scale >= 3 ? 14 : 10);
        draw_text(cx, y, scale, selected ? "[X]" : "[ ]",
                  selected ? Palette::ACCENT : Palette::TEXT_WHITE);
        int lx = cx + text_width_px("[X]", scale) + scale * 3;
        draw_text(lx, y, scale, label,
                  focused ? Palette::ACCENT : selected ? Palette::TEXT_WHITE : Palette::TEXT_DIM);
    };

    if (!menu.ingame) {
        // Board size is single-select vs KataGo (one local game needs exactly one
        // size) but multi-select for OGS search (multiple acceptable sizes widens
        // the automatch pool) — same distinction as STRENGTH vs TIME CONTROL below.
        if (menu.katago_mode) {
            for (int i = 0; i < 3; i++)
                draw_radio(0, i, size_labels[i], menu.size_sel[i]);
        } else {
            for (int i = 0; i < 3; i++)
                draw_check(0, i, size_labels[i], menu.size_sel[i]);
        }

        if (menu.katago_mode) {
            for (int i = 0; i < 7; i++)
                draw_radio(1, i, str_labels[i], menu.katago_str == i);
            // Row 7: adaptive strength — label carries the current resolved rank
            draw_radio(1, 7,
                       menu.adaptive_label.empty() ? "ADAPTIVE" : menu.adaptive_label.c_str(),
                       menu.katago_str == 7);
        } else {
            for (int i = 0; i < 3; i++)
                draw_check(1, i, speed_labels[i], menu.speed_sel[i]);
        }
    }

    draw_check(display_col, 0, display_labels[0], menu.show_coords_sel);
    draw_check(display_col, 1, display_labels[1], menu.analysis_sel);
    draw_check(display_col, 2, display_labels[2], menu.chain_sel);
    draw_check(display_col, 3, display_labels[3], menu.square_sel);

    // ── Controls reference — fills the otherwise-unused bottom of the menu ──────
    // Three columns mirroring the ESC help overlay, in PlayStation button glyphs,
    // so the bindings can be checked from the controller without a keyboard.
    {
        struct CRow { const char* key; const char* desc; };
        static const CRow game_rows[] = {
            {GLYPH_PS_CROSS,        "PLACE STONE"},
            {GLYPH_PS_CIRCLE " x2", "PASS"},
            {"OPT x2",          "RESIGN"},
            {GLYPH_PS_TRIANGLE " x2","MARK MOVE"},
            {GLYPH_PS_SQUARE,       "UNDO (VS KATAGO)"},
            {"L2/R2",               "STEP HISTORY"},
            {"R2 HOLD",             "SHOW LAST MOVE"},
            {"SHARE",               "THIS MENU"},
        };
        static const CRow review_rows[] = {
            {"L2/R2",               "STEP MAIN LINE"},
            {GLYPH_PS_CIRCLE,       "LABEL POINT (A-Z)"},
            {"L1/R1",               "SWITCH BRANCH"},
            {GLYPH_PS_CROSS,        "PLAY BRANCH STONE"},
            {GLYPH_PS_TRIANGLE " x2","MARK/UNMARK MOVE"},
            {GLYPH_PS_SQUARE,       "OPEN CATALOG"},
            {"L3/R3",               "PREV/NEXT FILE"},
            {"OPT x2",          "BACK TO LOBBY"},
        };
        static const CRow catalog_rows[] = {
            {"UP/DOWN",             "NAVIGATE"},
            {GLYPH_PS_CROSS,        "OPEN GAME"},
            {GLYPH_PS_TRIANGLE " x2","DELETE GAME"},
            {GLYPH_PS_CIRCLE,       "CLOSE"},
        };
        struct CCol { const char* header; const CRow* rows; int n; };
        const CCol cols[3] = {
            {"GAME",     game_rows,    (int)(sizeof(game_rows)    / sizeof(game_rows[0]))},
            {"REVIEW",   review_rows,  (int)(sizeof(review_rows)  / sizeof(review_rows[0]))},
            {"CATALOG",  catalog_rows, (int)(sizeof(catalog_rows) / sizeof(catalog_rows[0]))},
        };

        int max_rows = 0;
        for (const auto& c : cols) max_rows = std::max(max_rows, c.n);
        int block_h  = (max_rows + 1) * line_h + line_gap * 2;  // +1 for column headers
        int cy_top   = sh - block_h - hpad / 2;

        // Longest option column above is STRENGTH (header + 8 radio rows); skip the
        // reference entirely rather than draw over it when the window is too short.
        int options_bottom = ty + (line_h + line_gap) + 8 * line_h + line_gap * 2;
        if (cy_top < options_bottom) { SDL_RenderPresent(sdl); return; }

        SDL_Color col_header = {160, 160, 160, 255};
        for (int c = 0; c < 3; c++) {
            int cx = hpad + c * col_w;
            int cy = cy_top;
            draw_text(cx, cy, scale, cols[c].header, col_header);
            cy += line_h + line_gap;
            // Align descriptions within the column to its widest key
            int key_w = 0;
            for (int r = 0; r < cols[c].n; r++)
                key_w = std::max(key_w, text_width_px(cols[c].rows[r].key, scale));
            for (int r = 0; r < cols[c].n; r++) {
                draw_text(cx, cy, scale, cols[c].rows[r].key, Palette::ACCENT);
                draw_text(cx + key_w + scale * 6, cy, scale, cols[c].rows[r].desc, Palette::TEXT_DIM);
                cy += line_h;
            }
        }
    }

    SDL_RenderPresent(sdl);
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
    // Player rank is only ever interesting outside of live play (catalog review
    // has no "in-progress game" to protect), so this always includes it.
    auto name_rank = [](const std::string& name, const std::string& rank) -> std::string {
        std::string n = name.empty() ? "?" : name;
        if (!rank.empty()) n += " [" + rank + "]";
        return n;
    };
    int vs_w     = text_width_px("vs", scale);
    int col_gap  = scale * 6;   // pixel gap between columns
    int max_w       = text_width_px(title, scale);
    int max_black_w = 0;        // widest black-player name across all entries
    int max_white_w = 0;        // widest white-player name across all entries

    for (int i = 0; i < total; i++) {
        const auto& e = cat.entries[i];
        int w;
        if (e.type == 0 && (!e.player_black.empty() || !e.player_white.empty())) {
            // Four-column: black  vs  white  date
            std::string bn = name_rank(e.player_black, e.player_black_rank);
            std::string wn = name_rank(e.player_white, e.player_white_rank);
            int bw = text_width_px(bn.c_str(), scale);
            int ww = text_width_px(wn.c_str(), scale);
            if (bw > max_black_w) max_black_w = bw;
            if (ww > max_white_w) max_white_w = ww;
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
    // Date column starts after the white name with extra gap
    int date_col_x = tx + max_black_w + col_gap + vs_w + col_gap + max_white_w + col_gap * 3;

    // The loop above measured each row against its OWN name widths, but rows are
    // drawn column-aligned (every white name sits at the widest-black-name offset,
    // etc.) with a trailing date column — so a drawn row can extend past that
    // per-row max_w. Widen max_w to the real aligned extent, so the selection
    // highlight bar and the thumbnail area both clear the full drawn line.
    if (max_black_w > 0 || max_white_w > 0) {
        bool any_date = false;
        for (int i = 0; i < total && !any_date; i++)
            any_date = (cat.entries[i].type == 0 && !cat.entries[i].date.empty());
        int aligned_w = max_black_w + col_gap + vs_w + col_gap + max_white_w;
        if (any_date)
            aligned_w = (date_col_x - tx) + text_width_px("0000-00-00", scale);
        max_w = std::max(max_w, aligned_w);
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
        if (ds.catalog_thumb_single) {
            // Single-position file (opening == final): one larger centered board
            // instead of the same picture twice.
            int single = std::min((view.screen_h - thumb_vpad * 2) * 6 / 10,
                                  view.screen_w - list_right - 80);
            if (single > 0) {
                int sx = list_right + (view.screen_w - list_right - single) / 2;
                render_mini_board(sx, (view.screen_h - single) / 2,
                                  single, ds.catalog_thumb_open, ds.catalog_thumb_board_size);
            }
        } else {
            render_mini_board(thumb_x, thumb_y_top,
                              thumb_size, ds.catalog_thumb_open,  ds.catalog_thumb_board_size);
            render_mini_board(thumb_x, thumb_y_top + thumb_size + thumb_inner_gap,
                              thumb_size, ds.catalog_thumb_final, ds.catalog_thumb_board_size);
        }
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
            // Four-column layout: [black ACCENT] [vs WHITE] [white ACCENT] [date DIM]
            std::string bn = name_rank(e.player_black, e.player_black_rank);
            std::string wn = name_rank(e.player_white, e.player_white_rank);
            draw_text(tx,                                         ty, scale, bn.c_str(),   Palette::ACCENT);
            draw_text(tx + max_black_w + col_gap,                 ty, scale, "vs", Palette::TEXT_WHITE);
            draw_text(tx + max_black_w + col_gap + vs_w + col_gap, ty, scale, wn.c_str(),  Palette::ACCENT);
            if (!e.date.empty()) {
                // Show first 10 chars (covers YYYY-MM-DD)
                char dbuf[11];
                int dlen = (int)e.date.size();
                int show = dlen < 10 ? dlen : 10;
                memcpy(dbuf, e.date.c_str(), show);
                dbuf[show] = '\0';
                draw_text(date_col_x, ty, scale, dbuf, Palette::TEXT_DIM);
            }
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
// Analysis tree panel (post-game review, ogs_client only)

void Renderer::render_analysis_tree(const BoardView& view, const DrawState& ds) {
    if (!ds.live_analysis_tree || ds.live_analysis_tree_count == 0) return;

    int avail_w = view.offset_x - view.margin;
    if (avail_w < 60) return;

    // Panel: 80% of left panel width, 55% of screen height.
    // Positioned so tree + gap + score graph are vertically centred together.
    int panel_w    = avail_w * 4 / 5;
    int panel_h    = view.screen_h * 11 / 20;
    int graph_h    = view.screen_h / 5;
    int tree_gap   = 20;
    int combo_h    = panel_h + tree_gap + graph_h;
    int panel_left = (avail_w - panel_w) / 2;
    int panel_top  = (view.screen_h - combo_h) / 2;

    // Panel background — match board colour so black stones read clearly
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
    SDL_Rect panel_rect = {panel_left, panel_top, panel_w, panel_h};
    SDL_RenderFillRect(sdl, &panel_rect);
    SDL_SetRenderDrawColor(sdl, 55, 65, 80, 255);
    SDL_RenderDrawRect(sdl, &panel_rect);

    SDL_RenderSetClipRect(sdl, &panel_rect);

    const int pad    = 10;
    const int row_h  = 45;   // taller rows for bigger nodes
    const int col_w  = 42;
    const int r_node = 15;   // 50% larger than before

    int inner_h    = panel_h - 2 * pad;
    int visible_rows = inner_h / row_h;
    int scroll     = std::max(0, ds.live_analysis_tree_cur_depth - visible_rows * 2 / 3);

    // Column 0 x-centre
    int x_origin = panel_left + pad + r_node;

    auto node_x = [&](int col) { return x_origin + col * col_w; };
    auto node_y = [&](int depth) {
        return panel_top + pad + (depth - scroll) * row_h + row_h / 2;
    };

    int vis_top    = panel_top + pad - row_h;
    int vis_bottom = panel_top + pad + visible_rows * row_h + row_h;

    // Yellow row highlights for marked moves (drawn first, behind everything)
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl, 255, 220, 40, 35);
    for (int i = 0; i < ds.live_analysis_tree_count; i++) {
        const auto& n = ds.live_analysis_tree[i];
        if (!n.marked) continue;
        int cy = node_y(n.depth);
        if (cy < vis_top || cy > vis_bottom) continue;
        SDL_Rect row = {panel_left + 1, cy - row_h / 2, panel_w - 2, row_h};
        SDL_RenderFillRect(sdl, &row);
    }

    // Lines (bold, drawn before nodes so circles appear on top)
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_Color line_col = {150, 168, 195, 255};
    for (int i = 0; i < ds.live_analysis_tree_count; i++) {
        const auto& n = ds.live_analysis_tree[i];
        if (n.parent_depth < 0) continue;

        int ny = node_y(n.depth);
        int py = node_y(n.parent_depth);
        int nx = node_x(n.col);
        int px = node_x(n.parent_col);

        if (ny < vis_top && py < vis_top) continue;
        if (ny > vis_bottom && py > vis_bottom) continue;

        if (n.col == n.parent_col) {
            draw_thick_line(nx, py, nx, ny, 3, line_col);
        } else {
            int bend_y = py + row_h / 2;
            draw_thick_line(px, py, px, bend_y, 3, line_col);
            draw_thick_line(px, bend_y, nx, bend_y, 3, line_col);
            draw_thick_line(nx, bend_y, nx, ny, 3, line_col);
        }
    }

    // Node circles
    for (int i = 0; i < ds.live_analysis_tree_count; i++) {
        const auto& n = ds.live_analysis_tree[i];
        int ny = node_y(n.depth);
        int nx = node_x(n.col);

        if (ny < vis_top || ny > vis_bottom) continue;

        if (n.current) {
            SDL_SetRenderDrawColor(sdl, 255, 235, 80, 255);
            fill_circle(nx, ny, r_node + 4);
        }

        if (n.move_color == 1) {
            SDL_SetRenderDrawColor(sdl, 38, 38, 38, 255);
            fill_circle(nx, ny, r_node);
        } else if (n.move_color == 0) {
            SDL_SetRenderDrawColor(sdl, 215, 215, 215, 255);
            fill_circle(nx, ny, r_node);
        } else {
            // Root — hollow ring
            SDL_SetRenderDrawColor(sdl, 85, 100, 120, 255);
            fill_circle(nx, ny, r_node);
            SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
            fill_circle(nx, ny, r_node - 3);
        }
    }

    SDL_RenderSetClipRect(sdl, nullptr);
}

// ---------------------------------------------------------------------------
// Score graph (post-game review, below the analysis tree)

void Renderer::render_score_graph(const BoardView& view, const DrawState& ds) {
    if (!ds.live_score_graph || ds.live_score_graph_len == 0) return;

    int avail_w = view.offset_x - view.margin;
    if (avail_w < 60) return;

    // Mirror the layout constants from render_analysis_tree so they stay aligned.
    int panel_w   = avail_w * 4 / 5;
    int panel_h   = view.screen_h * 11 / 20;
    int graph_h   = view.screen_h / 5;
    int tree_gap  = 20;
    int combo_h   = panel_h + tree_gap + graph_h;
    int panel_left = (avail_w - panel_w) / 2;
    int graph_top  = (view.screen_h - combo_h) / 2 + panel_h + tree_gap;

    if (graph_h < 30) return;

    // Panel background
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
    SDL_Rect bg = {panel_left, graph_top, panel_w, graph_h};
    SDL_RenderFillRect(sdl, &bg);
    SDL_SetRenderDrawColor(sdl, 55, 65, 80, 255);
    SDL_RenderDrawRect(sdl, &bg);

    SDL_RenderSetClipRect(sdl, &bg);

    const int pad     = 6;
    const int inner_l = panel_left + pad;
    const int inner_r = panel_left + panel_w - pad;
    const int inner_t = graph_top  + pad;
    const int inner_b = graph_top  + graph_h - pad;
    const int inner_w = inner_r - inner_l;
    const int inner_h = inner_b - inner_t;
    const int mid_y   = inner_t + inner_h / 2;   // y = zero line

    int n = ds.live_score_graph_len;

    // Auto-scale: find max |score| among known values, round up to 10-point multiple
    float max_abs = 10.f;
    for (int i = 0; i < n; i++) {
        float v = ds.live_score_graph[i];
        if (v != FLT_MAX) {
            float a = v < 0.f ? -v : v;
            if (a > max_abs) max_abs = a;
        }
    }
    max_abs = floorf(max_abs / 10.f + 1.f) * 10.f;

    auto score_to_y = [&](float s) -> int {
        float c = s < -max_abs ? -max_abs : (s > max_abs ? max_abs : s);
        return mid_y - (int)(c / max_abs * (float)(inner_h / 2));
    };
    auto depth_to_x = [&](int d) -> int {
        if (n <= 1) return inner_l;
        return inner_l + (int)((float)d / (float)(n - 1) * (float)inner_w);
    };

    // Riemann-sum-style area fill: one discrete rectangle per move (dx = inner_w / n),
    // full box height — white from the top edge down to the curve, black from the
    // curve down to the bottom edge. Not relative to the zero line: white's region
    // grows as White's lead grows (curve moves down, away from the top edge), black's
    // grows as Black's lead grows (curve moves up, away from the bottom edge) — same
    // "each side grows with its own superiority" property, just measured against the
    // box edges rather than the midline. Gaps (unknown depth) are left unfilled.
    if (n > 0) {
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
        SDL_Color black_fill = {20, 20, 22, 255};
        SDL_Color white_fill = {218, 218, 218, 255};
        for (int i = 0; i < n; i++) {
            float v = ds.live_score_graph[i];
            if (v == FLT_MAX) continue;
            int x0 = inner_l + (int)((float)i     / (float)n * (float)inner_w);
            int x1 = inner_l + (int)((float)(i+1) / (float)n * (float)inner_w);
            if (x1 <= x0) x1 = x0 + 1;
            int y = score_to_y(v);
            SDL_Rect white_rect = {x0, inner_t, x1 - x0, y - inner_t};
            SDL_Rect black_rect = {x0, y, x1 - x0, inner_b - y};
            SDL_SetRenderDrawColor(sdl, white_fill.r, white_fill.g, white_fill.b, white_fill.a);
            SDL_RenderFillRect(sdl, &white_rect);
            SDL_SetRenderDrawColor(sdl, black_fill.r, black_fill.g, black_fill.b, black_fill.a);
            SDL_RenderFillRect(sdl, &black_rect);
        }
    }

    // Zero line — drawn after the fill so it stays visible as a crisp reference line
    draw_thick_line(inner_l, mid_y, inner_r, mid_y, 3, {130, 155, 180, 255});

    // Current-position scan line: vertical yellow stripe
    int cur = ds.live_score_graph_cur;
    if (cur >= 0 && cur < n) {
        int cx = depth_to_x(cur);
        draw_thick_line(cx, inner_t, cx, inner_b, 2, {Palette::ACCENT.r, Palette::ACCENT.g, Palette::ACCENT.b, 200});
    }

    // Axis labels (small, only when there is room). Shadowed now that the fill
    // beneath them can be solid black or white rather than the mid-tone board colour.
    if (inner_h >= 40 && inner_w >= 50) {
        char buf[16];
        snprintf(buf, sizeof(buf), "B+%.0f", max_abs);
        draw_text(inner_l + 3, inner_t + 3, 1, buf, {0, 0, 0, 180});
        draw_text(inner_l + 2, inner_t + 2, 1, buf, {140, 220, 180, 230});
        snprintf(buf, sizeof(buf), "W+%.0f", max_abs);
        draw_text(inner_l + 3, inner_b - 8, 1, buf, {0, 0, 0, 180});
        draw_text(inner_l + 2, inner_b - 9, 1, buf, {230, 140, 160, 230});
    }

    SDL_RenderSetClipRect(sdl, nullptr);
}

// ---------------------------------------------------------------------------
// Main draw entry points

void Renderer::draw_board(const DrawState& ds) {
    BoardView view;
    get_board_view(view, ds.active_board_size);
    square_stones_ = ds.square_stones;  // read by shade_stone (also hashed for the cache)
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

    // Box selection is drawn outside the cache (like the cursor), so none of its
    // state goes into the hash — dragging never triggers a cache rebuild.

    // Mode flags
    mix8(uint8_t(ds.analysis_mode));
    mix8(uint8_t(ds.game_mode));
    mix8(uint8_t(ds.guess_mode));
    mix8(uint8_t(ds.chain_mode));
    mix8(uint8_t(ds.free_mode));
    mix8(uint8_t(ds.square_stones));
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

    // Live game (clocks + status; cursor is drawn outside the cache)
    mix8(uint8_t(ds.live_mode));
    if (ds.live_mode) {
        mix64(uint64_t(ds.live_black_secs));
        mix64(uint64_t(ds.live_white_secs));
        mix64(uint64_t(ds.live_black_periods));
        mix64(uint64_t(ds.live_white_periods));
        mix8(uint8_t(ds.live_my_turn));
        mix8(uint8_t(ds.live_my_color));
        if (ds.live_status) mix_str(ds.live_status);
        if (ds.live_result_banner) mix_str(ds.live_result_banner);
        // Stone removal overlay — must be hashed or cache won't rebuild when data arrives
        int n = ds.active_board_size;
        if (ds.live_dead_stones) {
            for (int r = 0; r < n; r++)
                for (int f = 0; f < n; f++)
                    mix8(uint8_t(ds.live_dead_stones[r][f]));
        }
        if (ds.live_ownership) {
            for (int r = 0; r < n; r++)
                for (int f = 0; f < n; f++)
                    mix8(uint8_t(ds.live_ownership[r][f]));
        }
        for (int i = 0; i < ds.live_suggestion_count; i++) {
            mix8(uint8_t(ds.live_suggestions[i].row));
            mix8(uint8_t(ds.live_suggestions[i].col));
            mix8(uint8_t(int(ds.live_suggestions[i].score_lead * 10)));
        }
        mix8(uint8_t(ds.live_hovered_suggestion + 1));  // invalidate cache when hover changes
        if (ds.live_kata_score_lead != FLT_MAX)
            mix64(uint64_t(int(ds.live_kata_score_lead * 10)));
        if (ds.live_actual_move_r >= 0) {
            mix8(uint8_t(ds.live_actual_move_r));
            mix8(uint8_t(ds.live_actual_move_f));
            if (ds.live_actual_move_score != FLT_MAX)
                mix64(uint64_t(int(ds.live_actual_move_score * 10)));
        }
        // Letter labels (analysis annotations)
        mix64(uint64_t(ds.live_label_count));
        for (int i = 0; i < ds.live_label_count; i++) {
            mix8(uint8_t(ds.live_labels[i].r));
            mix8(uint8_t(ds.live_labels[i].f));
            mix8(uint8_t(ds.live_labels[i].ch));
        }
        // Analysis tree (topology + current position)
        mix64(uint64_t(ds.live_analysis_tree_count));
        mix64(uint64_t(ds.live_analysis_tree_cur_depth));
        for (int i = 0; i < ds.live_analysis_tree_count; i++) {
            const auto& tn = ds.live_analysis_tree[i];
            mix8(uint8_t(tn.col));
            mix8(uint8_t(tn.current));
            mix8(uint8_t(tn.marked));
        }
        // Score graph
        if (ds.live_score_graph) {
            mix64(uint64_t(ds.live_score_graph_len));
            mix64(uint64_t(ds.live_score_graph_cur));
            for (int i = 0; i < ds.live_score_graph_len; i++) {
                float v = ds.live_score_graph[i];
                mix64(v == FLT_MAX ? UINT64_MAX
                                   : uint64_t((int)(v * 10.f + 0.5f) + 50000));
            }
        }
        mix8(uint8_t(ds.live_last_move_r + 1));
        mix8(uint8_t(ds.live_last_move_f + 1));
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

    // Layered render:
    //   1. All shadows (max-alpha composite — no stacking)
    //   2. Stone fills
    //   3. Link cylinders with elliptical caps on top (bars drawn over stones,
    //      shortened to stone edge so caps give cylinder-meets-sphere illusion)
    render_all_shadows(view, active_board, ds.chain_mode, ds.stone_filter, n);
    for (int r = 0; r < n; r++)
        for (int f = 0; f < n; f++) {
            int cell = active_board[r][f];
            if (cell == 0) continue;
            int is_black = (cell == 1);
            if (ds.stone_filter == 1 && !is_black) continue;
            if (ds.stone_filter == 2 &&  is_black) continue;
            draw_stone_circle(view, r, f, is_black, 255, /*shadow_pass=*/false);
        }
    render_chain_connections(view, active_board, ds.chain_mode, ds.stone_filter, /*shadows_only=*/false);

    // Stone-removal overlay: grey out dead stones, mark territory
    if (ds.live_dead_stones || ds.live_ownership) {
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        int sq    = view.square;
        int sq2   = sq / 2;
        int tmark = std::max(3, sq / 5);  // territory square half-size

        for (int r = 0; r < n; r++) {
            for (int f = 0; f < n; f++) {
                int cx = view.offset_x + f * sq + sq2;
                int cy = view.offset_y + r * sq + sq2;

                // Territory squares: empty cells AND dead-stone cells.
                // Dead stones are removed for scoring, so their cells count as the
                // capturing player's territory.  Draw the square first so the X marker
                // for dead stones is rendered on top of it.
                if (ds.live_ownership) {
                    bool show_sq = (active_board[r][f] == 0) ||
                                   (ds.live_dead_stones && ds.live_dead_stones[r][f]);
                    if (show_sq) {
                        int ow = ds.live_ownership[r][f];
                        if (ow != 0) {
                            SDL_Rect sq_rect = {cx - tmark, cy - tmark, tmark*2, tmark*2};
                            if (ow > 0)  // black territory
                                SDL_SetRenderDrawColor(sdl, 30, 30, 30, 220);
                            else         // white territory
                                SDL_SetRenderDrawColor(sdl, 220, 220, 220, 220);
                            SDL_RenderFillRect(sdl, &sq_rect);
                            // Outline
                            SDL_SetRenderDrawColor(sdl, 0, 0, 0, 180);
                            SDL_RenderDrawRect(sdl, &sq_rect);
                        }
                    }
                }

                // Dead stone: thick X in contrasting colour, drawn on top of territory square
                if (ds.live_dead_stones && ds.live_dead_stones[r][f] && active_board[r][f] != 0) {
                    bool dead_is_black = (active_board[r][f] == 1);
                    int arm   = sq * 2 / 5;
                    int gap   = std::max(2, sq / 9);
                    int thick = std::max(2, sq / 8);
                    // White X over black stones, black X over white stones
                    if (dead_is_black)
                        SDL_SetRenderDrawColor(sdl, 255, 255, 255, 230);
                    else
                        SDL_SetRenderDrawColor(sdl, 20, 20, 20, 230);
                    for (int d = -thick/2; d <= thick/2; d++) {
                        SDL_RenderDrawLine(sdl, cx-gap-d, cy-gap+d, cx-arm-d, cy-arm+d);
                        SDL_RenderDrawLine(sdl, cx+gap+d, cy-gap+d, cx+arm+d, cy-arm+d);
                        SDL_RenderDrawLine(sdl, cx-gap-d, cy+gap-d, cx-arm-d, cy+arm-d);
                        SDL_RenderDrawLine(sdl, cx+gap+d, cy+gap-d, cx+arm+d, cy+arm-d);
                    }
                }
            }
        }
    }

    // Last-played-stone marker: teal ring, shown only while RT is held (see
    // App::make_draw_state — deliberately opt-in rather than always-on, since a
    // permanent row/column highlight was found too distracting during live play).
    if (ds.live_last_move_r >= 0 && ds.live_last_move_f >= 0) {
        int sq   = view.square;
        int half = sq / 2;
        int rad  = std::max(4, sq * 3 / 8);
        int lr   = ds.live_last_move_r;
        int lf   = ds.live_last_move_f;
        if (lr < n && lf < n) {
            int cx = view.offset_x + lf * sq + half;
            int cy = view.offset_y + lr * sq + half;
            SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
            // Thin dark fringe (1 px) so the ring reads against any background
            SDL_SetRenderDrawColor(sdl, 0, 0, 0, 130);
            fill_ring(cx, cy, rad + 6, rad + 5);
            // Teal ring (5 px thick) — centre left transparent so the stone still reads
            SDL_SetRenderDrawColor(sdl, 0, 210, 190, 255);
            fill_ring(cx, cy, rad + 5, rad);
        }
    }

    // Actual game move marker: yellow filled circle (larger than suggestions),
    // drawn first so suggestion circles appear on top and form a visible ring.
    // Hidden while hovering a suggestion (PV line is shown instead).
    if (ds.live_actual_move_r >= 0 && ds.live_actual_move_f >= 0 &&
        ds.live_hovered_suggestion < 0) {
        int sq   = view.square;
        int half = sq / 2;
        int rad  = std::max(4, sq * 3 / 8);
        int ar   = ds.live_actual_move_r;
        int af   = ds.live_actual_move_f;
        if (ar < n && af < n) {
            int cx = view.offset_x + af * sq + half;
            int cy = view.offset_y + ar * sq + half;
            SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
            // Thin dark fringe (1 px) so the ring reads against any background
            SDL_SetRenderDrawColor(sdl, 0, 0, 0, 130);
            fill_ring(cx, cy, rad + 6, rad + 5);
            // Yellow ring (5 px thick) — centre left transparent
            SDL_SetRenderDrawColor(sdl, Palette::ACCENT.r, Palette::ACCENT.g, Palette::ACCENT.b, 230);
            fill_ring(cx, cy, rad + 5, rad);
            if (ds.live_actual_move_score != FLT_MAX && rad >= 10) {
                float lead = ds.live_actual_move_score;
                char buf[8];
                if (std::abs(lead) < 10.f)
                    snprintf(buf, sizeof(buf), "%.1f", lead);
                else
                    snprintf(buf, sizeof(buf), "%d", (int)(lead + (lead >= 0 ? 0.5f : -0.5f)));
                int tw = text_width_px(buf, 2);
                draw_text(cx - tw / 2, cy - 7, 2, buf, SDL_Color{255, 255, 255, 255});
            }
        }
    }

    // KataGo move suggestions (drawn during history review in GAME_OVER)
    if (ds.live_suggestions && ds.live_suggestion_count > 0) {
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        int sq   = view.square;
        int half = sq / 2;
        int rad  = std::max(4, sq * 3 / 8);

        for (int i = 0; i < std::min(ds.live_suggestion_count, 3); i++) {
            if (ds.live_hovered_suggestion >= 0 && i != ds.live_hovered_suggestion) continue;
            const MoveSuggestion& s = ds.live_suggestions[i];
            if (s.row < 0 || s.col < 0 || s.row >= n || s.col >= n) continue;
            if (active_board[s.row][s.col] != 0) continue;  // stone already there

            int cx = view.offset_x + s.col * sq + half;
            int cy = view.offset_y + s.row * sq + half;

            if (i == ds.live_hovered_suggestion) {
                // Hovered: show as the player's actual stone, numbered 1
                bool is_black = (ds.game.turn_is_black == 1);
                SDL_SetRenderDrawColor(sdl, 0, 0, 0, 200);
                fill_circle(cx, cy, rad + 1);
                SDL_SetRenderDrawColor(sdl, is_black ? 30 : 215, is_black ? 30 : 215, is_black ? 30 : 215, 230);
                fill_circle(cx, cy, rad);
                {
                    int sc = 1;
                    if (rad >= 14) sc = 2;
                    if (rad >= 20) sc = 3;
                    int tw = text_width_px("1", sc), th = 7 * sc;
                    SDL_Color tc = is_black ? SDL_Color{230,230,230,255} : SDL_Color{40,40,40,255};
                    draw_text(cx - tw/2, cy - th/2, sc, "1", tc);
                }
            } else {
                // Brightness fades with rank: best = 210, each step -25
                Uint8 alpha = (Uint8)std::max(80, 210 - i * 25);
                Uint8 green = (Uint8)std::max(130, 210 - i * 15);

                SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(alpha * 0.7f));
                fill_circle(cx, cy, rad + 1);
                SDL_SetRenderDrawColor(sdl, 10, green, 150, alpha);
                fill_circle(cx, cy, rad);

                // Score lead label (points ahead for the player to move)
                if (rad >= 10) {
                    char buf[8];
                    float lead = s.score_lead;
                    if (std::abs(lead) < 10.f)
                        snprintf(buf, sizeof(buf), "%.1f", lead);
                    else
                        snprintf(buf, sizeof(buf), "%d", (int)(lead + (lead >= 0 ? 0.5f : -0.5f)));
                    int tw = text_width_px(buf, 2);
                    draw_text(cx - tw / 2, cy - 7, 2, buf, SDL_Color{240, 240, 240, 255});
                }
            }
        }

        // PV overlay for the hovered suggestion (stones 2..N)
        if (ds.live_hovered_suggestion >= 0 &&
            ds.live_hovered_suggestion < ds.live_suggestion_count) {
            const MoveSuggestion& hs = ds.live_suggestions[ds.live_hovered_suggestion];
            bool black_plays_first = (ds.game.turn_is_black == 1);
            for (int pv = 0; pv < hs.pv_count; pv++) {
                int pr = hs.pv_row[pv];
                int pf = hs.pv_col[pv];
                if (pr < 0 || pr >= n || pf < 0 || pf >= n) continue;
                int pcx = view.offset_x + pf * sq + half;
                int pcy = view.offset_y + pr * sq + half;
                // pv[0] = opponent's response, pv[1] = suggestion player again, etc.
                bool is_black = (pv % 2 == 0) ? !black_plays_first : black_plays_first;
                SDL_SetRenderDrawColor(sdl, 0, 0, 0, 180);
                fill_circle(pcx, pcy, rad + 1);
                SDL_SetRenderDrawColor(sdl, is_black ? 30 : 210, is_black ? 30 : 210, is_black ? 30 : 210, 220);
                fill_circle(pcx, pcy, rad);
                char pbuf[4];
                snprintf(pbuf, sizeof(pbuf), "%d", pv + 2);  // hovered stone = 1, so PV starts at 2
                int ndigits = (int)strlen(pbuf);
                int sc = 1;
                if (rad >= 14 && ndigits <= 2) sc = 2;
                if (rad >= 20 && ndigits == 1)  sc = 3;
                int ptw = text_width_px(pbuf, sc), pth = 7 * sc;
                SDL_Color nc = is_black ? SDL_Color{230,230,230,255} : SDL_Color{40,40,40,255};
                draw_text(pcx - ptw/2, pcy - pth/2, sc, pbuf, nc);
            }
        }
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
        if (!ds.live_mode) {
            render_speed_label(view, ds.move_delay_ms, ds.speed_message_until);
            render_result_message(view, ds);
            render_game_date(view, ds.game_date);
        }
        render_guess_score(view, ds.guess_mode, ds.guess_score);
    }
    if (!ds.free_mode) {
        if (ds.live_mode) {
            // Status line in the same top-left spot as mode status
            if (ds.live_status && ds.live_status[0]) {
                int scale = (view.square >= 30) ? 3 : 2;
                int pad   = (view.square >= 30) ? 16 : 8;
                int avail = view.offset_x - view.margin - pad - 4;
                // Scale down if the text overflows the left panel
                while (scale > 1 && text_width_px(ds.live_status, scale) > avail)
                    scale--;
                int y = view.offset_y - view.margin + pad;
                // Big result banner (local game end): the score in double scale, with
                // the normal-size status line ("PRESS X FOR ANALYSIS") beneath it.
                if (ds.live_result_banner && ds.live_result_banner[0]) {
                    int bscale = scale * 2;
                    while (bscale > scale && text_width_px(ds.live_result_banner, bscale) > avail)
                        bscale--;
                    int bw = text_width_px(ds.live_result_banner, bscale);
                    draw_text(view.offset_x - view.margin - pad - bw, y, bscale,
                              ds.live_result_banner, Palette::ACCENT);
                    y += bscale * 7 + 10;
                }
                int tw = text_width_px(ds.live_status, scale);
                int x  = view.offset_x - view.margin - pad - tw;
                SDL_Color col = ds.live_my_turn ? Palette::ACCENT : Palette::TEXT_SECONDARY;
                draw_text(x, y, scale, ds.live_status, col);

                // Result and KataGo projected score below the status (GAME_OVER)
                int next_y = y + scale * 8 + 3;
                int lscale = scale;
                int lright = view.offset_x - view.margin - pad;

                if (!ds.result_message.empty()) {
                    char rbuf[48];
                    const char* rm = ds.result_message.c_str();
                    if ((rm[0] == 'B' || rm[0] == 'W') && rm[1] == '+') {
                        const char* who = (rm[0] == 'B') ? "BLACK" : "WHITE";
                        const char* margin = rm + 2;
                        if (margin[0] == 'R' || margin[0] == 'r') {
                            // who = winner; the *loser* resigned
                            const char* loser = (rm[0] == 'B') ? "WHITE" : "BLACK";
                            snprintf(rbuf, sizeof(rbuf), "RESULT: %s RESIGNED", loser);
                        } else if (margin[0] == 'T' || margin[0] == 't')
                            snprintf(rbuf, sizeof(rbuf), "RESULT: %s TIME", who);
                        else
                            snprintf(rbuf, sizeof(rbuf), "RESULT: %s +%s", who, margin);
                    } else {
                        snprintf(rbuf, sizeof(rbuf), "RESULT: %s", rm);
                    }
                    draw_text(lright - text_width_px(rbuf, lscale), next_y, lscale, rbuf, Palette::ACCENT);
                    next_y += lscale * 8 + 3;
                }
                if (ds.live_kata_score_lead != FLT_MAX) {
                    float sl = ds.live_kata_score_lead;
                    const char* who = sl >= 0.f ? "BLACK" : "WHITE";
                    char sbuf[48];
                    snprintf(sbuf, sizeof(sbuf), "PROJECTED: %s +%.1f", who, std::fabsf(sl));
                    draw_text(lright - text_width_px(sbuf, lscale), next_y, lscale, sbuf, Palette::PROJECTED);
                }
            }
        } else {
            render_mode_status(view, ds.analysis_mode, ds.game_mode, ds.guess_mode, ds.territory_drill, false);
        }
    }
    // Letter labels placed during analysis (circle button). On an empty point the
    // letter gets a board-colored backing disc so grid lines don't cross it; on a
    // stone it's drawn directly in the contrasting color.
    for (int i = 0; i < ds.live_label_count; i++) {
        const BoardLabel& l = ds.live_labels[i];
        if (l.r < 0 || l.r >= n || l.f < 0 || l.f >= n) continue;
        int cx = view.offset_x + l.f * view.square + view.square / 2;
        int cy = view.offset_y + l.r * view.square + view.square / 2;
        int cell = active_board[l.r][l.f];
        int scale = (view.square >= 40) ? 3 : 2;
        char buf[2] = { l.ch, '\0' };
        int tw = text_width_px(buf, scale), th = 7 * scale;
        SDL_Color txt;
        if (cell == 0) {
            SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(sdl, Palette::BOARD.r, Palette::BOARD.g, Palette::BOARD.b, 255);
            fill_circle(cx, cy, view.square * 2 / 5);
            txt = Palette::ACCENT;  // yellow reads far better on the board than dark gray
        } else {
            txt = (cell == 1) ? SDL_Color{235, 235, 235, 255} : SDL_Color{30, 30, 30, 255};
        }
        draw_text(cx - tw / 2, cy - th / 2, scale, buf, txt);
    }

    render_territory_overlay(view, ds);
    render_analysis_tree(view, ds);
    render_score_graph(view, ds);

    render_help_overlay(view, ds.show_help, ds.live_mode);
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

    // Blit cached board, then draw drag-overlay + cursor on top (always, every frame,
    // without touching the cache — so dragging never triggers a cache rebuild)
    SDL_RenderCopy(sdl, board_cache_, nullptr, nullptr);

    // Full-screen overlays (catalog, help) live INSIDE the cache; everything drawn
    // after the blit would land on top of them. Skip the board-level decorations
    // (dim, box selection, live cursor) while one of those overlays is up.
    bool board_on_screen = !ds.catalog.active && !ds.show_help;

    // History review: dim the board so it's clear we're not in the live game
    if (board_on_screen && ds.live_in_history) {
        int bx = view.offset_x - view.margin;
        int by = view.offset_y - view.margin;
        int bw = view.board_px + view.margin * 2;
        int bh = view.board_px + view.margin * 2;
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdl, 140, 140, 140, 60);
        SDL_Rect r = {bx, by, bw, bh};
        SDL_RenderFillRect(sdl, &r);
    }

    if (board_on_screen) render_box_selection(view, ds);

    // Live board cursor — yellow X drawn at the grid intersection (45° rotated, bold).
    if (board_on_screen && ds.live_mode && ds.live_cursor_r >= 0 && ds.live_cursor_f >= 0) {
        int cx = view.offset_x + ds.live_cursor_f * view.square + view.square / 2;
        int cy = view.offset_y + ds.live_cursor_r * view.square + view.square / 2;
        int arm = view.square * 2 / 5;
        int gap = std::max(2, view.square / 9);
        int thick = std::max(2, view.square / 10);  // boldness
        Uint8 alpha = ds.live_my_turn ? 230 : 130;
        SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
        if (ds.live_cursor_ko)
            SDL_SetRenderDrawColor(sdl, 255, 60, 60, alpha);
        else
            SDL_SetRenderDrawColor(sdl, 255, 220, 0, alpha);
        // Draw each of the 4 diagonal arms with thickness via parallel offset lines
        for (int d = -thick/2; d <= thick/2; d++) {
            // top-left arm
            SDL_RenderDrawLine(sdl, cx - gap - d, cy - gap + d, cx - arm - d, cy - arm + d);
            // top-right arm
            SDL_RenderDrawLine(sdl, cx + gap + d, cy - gap + d, cx + arm + d, cy - arm + d);
            // bottom-left arm
            SDL_RenderDrawLine(sdl, cx - gap - d, cy + gap - d, cx - arm - d, cy + arm - d);
            // bottom-right arm
            SDL_RenderDrawLine(sdl, cx + gap + d, cy + gap - d, cx + arm + d, cy + arm - d);
        }
    }

    render_board_coordinates(view, ds);
    render_software_cursor(view, ds);
    if (!ds.suppress_present)
        SDL_RenderPresent(sdl);
}

// ---------------------------------------------------------------------------
// Board-edge coordinate labels (toggled by RT during live play)

void Renderer::render_board_coordinates(const BoardView& view, const DrawState& ds) {
    if (!ds.live_show_coords) return;
    // Only draw when the board is actually what's on screen — not behind a
    // full-screen overlay like the catalog or help screen.
    if (ds.catalog.active || ds.show_help) return;
    int n = ds.active_board_size;
    if (n <= 0) return;

    // Standard Go lettering skips 'I' (looks too much like '1').
    auto col_letter = [](int f) -> char {
        char c = (char)('A' + f);
        if (c >= 'I') c++;
        return c;
    };

    int sq    = view.square;
    int scale = std::max(1, sq / 16);
    int th    = 7 * scale;
    SDL_Color dim    = {150, 165, 180, 230};
    SDL_Color yellow = {255, 220, 0, 255};

    int cur_r = ds.live_cursor_r, cur_f = ds.live_cursor_f;

    // Top & bottom edges: column letters
    for (int f = 0; f < n; f++) {
        char buf[2] = { col_letter(f), '\0' };
        int  tw  = text_width_px(buf, scale);
        int  cx  = view.offset_x + f * sq + sq / 2;
        bool hi  = (f == cur_f && cur_r >= 0);
        SDL_Color col = hi ? yellow : dim;
        draw_text(cx - tw / 2, view.offset_y - view.margin + (view.margin - th) / 2, scale, buf, col);
        draw_text(cx - tw / 2, view.offset_y + n * sq   + (view.margin - th) / 2, scale, buf, col);
    }

    // Left & right edges: row numbers — size at the top, 1 at the bottom (Go convention)
    for (int r = 0; r < n; r++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", n - r);
        int  tw  = text_width_px(buf, scale);
        int  cy  = view.offset_y + r * sq + sq / 2;
        bool hi  = (r == cur_r && cur_f >= 0);
        SDL_Color col = hi ? yellow : dim;
        draw_text(view.offset_x - view.margin + (view.margin - tw) / 2, cy - th / 2, scale, buf, col);
        draw_text(view.offset_x + n * sq   + (view.margin - tw) / 2, cy - th / 2, scale, buf, col);
    }
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
