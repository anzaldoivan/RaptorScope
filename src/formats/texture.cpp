/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  Texture Rendering & Swizzle
 *═══════════════════════════════════════════════════════════════════*/
#include "formats/texture.h"
#include "core/lzss.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/*─── VRAM Deswizzle ─────────────────────────────────────────────*/
/* Row-major block order: for each by, iterate bx (standard PS1 VRAM) */
void deswizzle_8bpp(const u8* src, size_t src_size,
                    u8* dst, int vram_w, int vram_h)
{
    int pw = vram_w * 2;       /* byte width */
    int bw = 64, bh = 32;     /* block size in bytes/rows */
    int bx_count = pw / bw;
    int full_by  = vram_h / bh;
    int remain_h = vram_h % bh;

    memset(dst, 0, pw * vram_h);
    size_t si = 0;

    for (int by = 0; by < full_by; by++) {
        for (int bx = 0; bx < bx_count; bx++) {
            for (int y = 0; y < bh; y++) {
                size_t di = (size_t)(by * bh + y) * pw + bx * bw;
                size_t end = si + bw;
                if (end <= src_size && di + bw <= (size_t)(pw * vram_h))
                    memcpy(dst + di, src + si, bw);
                si += bw;
            }
        }
    }
    if (remain_h > 0) {
        for (int bx = 0; bx < bx_count; bx++) {
            for (int y = 0; y < remain_h; y++) {
                size_t di = (size_t)(full_by * bh + y) * pw + bx * bw;
                size_t end = si + bw;
                if (end <= src_size && di + bw <= (size_t)(pw * vram_h))
                    memcpy(dst + di, src + si, bw);
                si += bw;
            }
        }
    }
}

/* Remain-first block order: remainder rows appear FIRST in source,
   then full blocks in row-major order (bx inner, by outer).
   Used by Dino Crisis item sprite banks. */
void deswizzle_8bpp_remfirst(const u8* src, size_t src_size,
                              u8* dst, int vram_w, int vram_h)
{
    int pw = vram_w * 2;
    int bw = 64, bh = 32;
    int bx_count = pw / bw;
    int full_by  = vram_h / bh;
    int remain_h = vram_h % bh;

    memset(dst, 0, pw * vram_h);
    size_t si = 0;

    /* 1. Remainder rows first in source → placed at bottom of output */
    if (remain_h > 0) {
        for (int bx = 0; bx < bx_count; bx++) {
            for (int y = 0; y < remain_h; y++) {
                size_t di = (size_t)(full_by * bh + y) * pw + bx * bw;
                if (si + bw <= src_size && di + bw <= (size_t)(pw * vram_h))
                    memcpy(dst + di, src + si, bw);
                si += bw;
            }
        }
    }

    /* 2. Full blocks in row-major order → placed top-down */
    for (int by = 0; by < full_by; by++) {
        for (int bx = 0; bx < bx_count; bx++) {
            for (int y = 0; y < bh; y++) {
                size_t di = (size_t)(by * bh + y) * pw + bx * bw;
                if (si + bw <= src_size && di + bw <= (size_t)(pw * vram_h))
                    memcpy(dst + di, src + si, bw);
                si += bw;
            }
        }
    }
}

void swizzle_8bpp(const u8* src, size_t src_size,
                  u8* dst, int vram_w, int vram_h)
{
    int pw = vram_w * 2;
    int bw = 64, bh = 32;
    int bx_count = pw / bw;
    int full_by  = vram_h / bh;
    int remain_h = vram_h % bh;

    size_t di = 0;
    for (int by = 0; by < full_by; by++) {
        for (int bx = 0; bx < bx_count; bx++) {
            for (int y = 0; y < bh; y++) {
                size_t si2 = (size_t)(by * bh + y) * pw + bx * bw;
                if (si2 + bw <= src_size && di + bw <= src_size)
                    memcpy(dst + di, src + si2, bw);
                di += bw;
            }
        }
    }
    if (remain_h > 0) {
        for (int bx = 0; bx < bx_count; bx++) {
            for (int y = 0; y < remain_h; y++) {
                size_t si2 = (size_t)(full_by * bh + y) * pw + bx * bw;
                if (si2 + bw <= src_size && di + bw <= src_size)
                    memcpy(dst + di, src + si2, bw);
                di += bw;
            }
        }
    }
}

/* Un-tile for display.  Every tile is stored whole, so the data of a
   texture whose height is not a multiple of 32 has more rows than its
   header says (WEP_P000.DAT's weapon: 64x136, 160 rows stored; ST100's
   door textures likewise).  Un-tiled by the header height, the last tile
   row is read as short tiles and its columns come out shuffled.  Returns
   vram_w * 2 bytes a row, at least vram_h rows (malloc'd), or 0. */
static u8* deswizzle_stored(const u8* src, size_t src_size, int vram_w, int vram_h)
{
    int byte_w = vram_w * 2;
    int rows = byte_w > 0 ? (int)(src_size / byte_w) : 0;
    if (rows < vram_h) rows = vram_h;
    u8* linear = (u8*)malloc((size_t)byte_w * rows + 1);
    if (linear) deswizzle_8bpp(src, src_size, linear, vram_w, rows);
    return linear;
}

/*─── 8bpp Render ────────────────────────────────────────────────*/
void render_8bpp(const u8* pixels, size_t pix_size,
                 const RGBA8* palette,
                 int vram_w, int vram_h,
                 u8* out_rgba, bool do_deswizzle,
                 int pal_row)
{
    int pw = vram_w * 2;  /* pixel width = byte width for 8bpp */
    size_t total = (size_t)pw * vram_h;

    /* Deswizzle if needed */
    u8* linear = 0;
    if (do_deswizzle && pix_size > 0) {
        linear = deswizzle_stored(pixels, pix_size, vram_w, vram_h);
        if (!linear) return;
    } else {
        linear = (u8*)pixels;  /* use directly */
    }

    /* Render to RGBA */
    for (int y = 0; y < vram_h; y++) {
        for (int x = 0; x < pw; x++) {
            size_t si = (size_t)y * pw + x;
            size_t di = ((size_t)y * pw + x) * 4;
            u8 idx = (si < total && si < pix_size) ? linear[si] : 0;
            RGBA8 c = palette[pal_row * 256 + idx];
            out_rgba[di]     = c.r;
            out_rgba[di + 1] = c.g;
            out_rgba[di + 2] = c.b;
            out_rgba[di + 3] = c.a;
        }
    }

    if (do_deswizzle && linear != pixels)
        free(linear);
}

/*─── 4bpp Render ────────────────────────────────────────────────*/
void render_4bpp(const u8* pixels, size_t pix_size,
                 const RGBA8* palette,
                 int vram_w, int vram_h,
                 u8* out_rgba, bool do_deswizzle,
                 int pal_row, int sub_pal)
{
    int byte_w = vram_w * 2;
    int pixel_w = byte_w * 2;  /* 4bpp: 2 pixels per byte */
    size_t total_bytes = (size_t)byte_w * vram_h;

    u8* linear = 0;
    if (do_deswizzle && pix_size > 0) {
        linear = deswizzle_stored(pixels, pix_size, vram_w, vram_h);
        if (!linear) return;
    } else {
        linear = (u8*)pixels;
    }

    int pal_base = pal_row * 256 + sub_pal * 16;

    for (int y = 0; y < vram_h; y++) {
        for (int bx = 0; bx < byte_w; bx++) {
            size_t si = (size_t)y * byte_w + bx;
            u8 byte_val = (si < total_bytes) ? linear[si] : 0;
            u8 lo = byte_val & 0x0F;
            u8 hi = (byte_val >> 4) & 0x0F;

            int px = bx * 2;
            size_t di0 = ((size_t)y * pixel_w + px) * 4;
            size_t di1 = ((size_t)y * pixel_w + px + 1) * 4;

            RGBA8 c0 = palette[pal_base + lo];
            RGBA8 c1 = palette[pal_base + hi];

            out_rgba[di0]     = c0.r; out_rgba[di0+1] = c0.g;
            out_rgba[di0+2]   = c0.b; out_rgba[di0+3] = c0.a;
            out_rgba[di1]     = c1.r; out_rgba[di1+1] = c1.g;
            out_rgba[di1+2]   = c1.b; out_rgba[di1+3] = c1.a;
        }
    }

    if (do_deswizzle && linear != pixels)
        free(linear);
}

/*─── 16bpp Render ───────────────────────────────────────────────*/
void render_16bpp(const u8* pixels, int w, int h, u8* out_rgba)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t si = ((size_t)y * w + x) * 2;
            u16 pix = rd_u16(pixels + si);
            RGBA8 c = bgr555_to_rgba(pix);
            size_t di = ((size_t)y * w + x) * 4;
            out_rgba[di]     = c.r;
            out_rgba[di + 1] = c.g;
            out_rgba[di + 2] = c.b;
            out_rgba[di + 3] = c.a;
        }
    }
}

/*─── General render dispatch ────────────────────────────────────*/
void texture_pixel_dims(int vram_w, int vram_h, int bpp,
                        int& out_w, int& out_h)
{
    out_h = vram_h;
    switch (bpp) {
        case 4:  out_w = vram_w * 4; break;
        case 8:  out_w = vram_w * 2; break;
        case 16: out_w = vram_w;     break;
        default: out_w = vram_w * 2; break;
    }
}

void render_texture(const u8* pixels, size_t pix_size,
                    const u8* pal_data, size_t pal_size,
                    int vram_w, int vram_h, int bpp,
                    u8* out_rgba, bool do_deswizzle,
                    int pal_row, int sub_pal)
{
    RGBA8 palette[4096];   /* up to 16 palette rows of 256 */
    for (int i = 0; i < 4096; i++) palette[i] = RGBA8(0, 0, 0, 0);
    if (pal_data && pal_size > 0) {
        int np = (int)(pal_size / 512);
        if (np < 1) np = 1;
        if (np > 16) np = 16;
        parse_palette(pal_data, pal_size, palette, np);
    }

    switch (bpp) {
        case 4:
            render_4bpp(pixels, pix_size, palette, vram_w, vram_h,
                        out_rgba, do_deswizzle, pal_row, sub_pal);
            break;
        case 16:
            render_16bpp(pixels, vram_w, vram_h, out_rgba);
            break;
        case 8:
        default:
            render_8bpp(pixels, pix_size, palette, vram_w, vram_h,
                        out_rgba, do_deswizzle, pal_row);
            break;
    }
}

/*─── Render as sampled by faces ─────────────────────────────────*/
#define TEXUSE_MAX_PALS   16
#define TEXUSE_DIM_ALPHA  80   /* alpha for texels no face covers */

/* How one texel is decoded: palette entry, row in it, CLUT offset in the
   row and colour depth.  bpp == 0 means "not set". */
struct TexelSrc {
    u8 pal, row, off, bpp;
};

static inline bool texel_src_eq(const TexelSrc& a, const TexelSrc& b) {
    return a.pal == b.pal && a.row == b.row && a.off == b.off && a.bpp == b.bpp;
}

/* Most common source among the set texels of a region (by texel count). */
static TexelSrc dominant_src(const TexelSrc* src, int stride,
                             int x0, int y0, int x1, int y1)
{
    TexelSrc cand[16]; int cnt[16]; int n = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            const TexelSrc& s = src[(size_t)y * stride + x];
            if (!s.bpp) continue;
            int k = 0;
            while (k < n && !texel_src_eq(cand[k], s)) k++;
            if (k == n) {
                if (n == 16) continue;
                cand[n] = s; cnt[n] = 0; n++;
            }
            cnt[k]++;
        }
    }
    TexelSrc best; memset(&best, 0, sizeof(best));
    int best_cnt = 0;
    for (int k = 0; k < n; k++)
        if (cnt[k] > best_cnt) { best_cnt = cnt[k]; best = cand[k]; }
    return best;
}

bool render_texture_by_faces(const u8* pixels, size_t pix_size,
                             int vram_x, int vram_y, int vram_w, int vram_h,
                             const TexFaceUse* faces, int n_faces,
                             const DatArchive& archive,
                             u8** out_rgba, int* out_w, int* out_h)
{
    *out_rgba = 0; *out_w = 0; *out_h = 0;
    if (!pixels || pix_size == 0 || vram_w <= 0 || vram_h <= 0 || n_faces <= 0)
        return false;

    /* Palette entries: VRAM position, row count and decoded colours */
    struct PalRef { int x, y, w, rows; };
    PalRef pals[TEXUSE_MAX_PALS]; int n_pals = 0;
    RGBA8* pal_rgba = (RGBA8*)malloc(TEXUSE_MAX_PALS * 4096 * sizeof(RGBA8));
    if (!pal_rgba) return false;
    for (int i = 0; i < archive.count && n_pals < TEXUSE_MAX_PALS; i++) {
        const DatEntry& e = archive.entries[i];
        if (e.type != DAT_PALETTE || !e.data || e.size < 32) continue;
        int rows = (int)(e.size / 512);
        if (rows < 1) rows = 1;
        if (rows > 16) rows = 16;
        pals[n_pals].x = e.x; pals[n_pals].y = e.y; pals[n_pals].w = e.w; pals[n_pals].rows = rows;
        parse_palette(e.data, e.size, pal_rgba + (size_t)n_pals * 4096, rows);
        n_pals++;
    }

    /* Data can be taller than the header says (deswizzle needs full height) */
    int byte_w = vram_w * 2;
    int vh = vram_h;
    if ((int)(pix_size / byte_w) > vh) vh = (int)(pix_size / byte_w);

    /* Faces that sample this texture through a known palette */
    struct FaceRef { int fi; TexelSrc src; int page_x, page_y; };
    FaceRef* refs = (FaceRef*)malloc((size_t)n_faces * sizeof(FaceRef));
    if (!refs) { free(pal_rgba); return false; }
    int n_refs = 0;
    bool any_4bpp = false;
    for (int fi = 0; fi < n_faces; fi++) {
        const TexFaceUse& f = faces[fi];
        int tp = (f.tpage >> 7) & 3;
        if (tp > 1) continue;                       /* 16bpp: no CLUT */
        int page_x = (f.tpage & 0xF) * 64;
        int page_y = ((f.tpage >> 4) & 1) * 256;
        int page_hw = tp ? 128 : 64;                /* 256 texels wide */
        if (page_x + page_hw <= vram_x || page_x >= vram_x + vram_w) continue;
        if (page_y + 256 <= vram_y || page_y >= vram_y + vh) continue;

        int cx = (f.clut & 0x3F) * 16;
        int cy = (f.clut >> 6) & 0x1FF;
        /* DC2 packages put several short palettes on one row (CORE.DAT has
           five on row 503), so there the palette's own width decides. */
        int p = n_pals;
        if (archive.is_dc2) {
            p = 0;
            while (p < n_pals && !(cy >= pals[p].y && cy < pals[p].y + pals[p].rows &&
                                   cx >= pals[p].x && cx < pals[p].x + pals[p].w))
                p++;
        }
        if (p == n_pals) {
            p = 0;
            while (p < n_pals && !(cy >= pals[p].y && cy < pals[p].y + pals[p].rows &&
                                   cx >= pals[p].x && cx < pals[p].x + 256))
                p++;
        }
        if (p == n_pals) continue;

        FaceRef& r = refs[n_refs++];
        r.fi = fi;
        r.src.pal = (u8)p;
        r.src.row = (u8)(cy - pals[p].y);
        r.src.off = (u8)(cx - pals[p].x);
        r.src.bpp = (u8)(tp ? 8 : 4);
        r.page_x = page_x; r.page_y = page_y;
        if (!tp) any_4bpp = true;
    }
    if (n_refs == 0) { free(refs); free(pal_rgba); return false; }

    int ppw = any_4bpp ? 4 : 2;                     /* output px per halfword */
    int w = vram_w * ppw, h = vh;
    TexelSrc* src = (TexelSrc*)calloc((size_t)w * h, sizeof(TexelSrc));
    u8* linear = (u8*)calloc((size_t)byte_w * vh, 1);
    u8* rgba = (u8*)calloc((size_t)w * h, 4);
    if (!src || !linear || !rgba) {
        free(src); free(linear); free(rgba); free(refs); free(pal_rgba);
        return false;
    }
    deswizzle_8bpp(pixels, pix_size, linear, vram_w, vh);

    /* Mark every texel inside each face's UV triangle (texel centres
       within one texel of the edges, so shared edges are covered). */
    for (int ri = 0; ri < n_refs; ri++) {
        const FaceRef& r = refs[ri];
        const TexFaceUse& f = faces[r.fi];
        float us[3], vs[3];
        int u0 = 255, u1 = 0, v0 = 255, v1 = 0;
        for (int k = 0; k < 3; k++) {
            us[k] = f.uv[k][0]; vs[k] = f.uv[k][1];
            if (f.uv[k][0] < u0) u0 = f.uv[k][0];
            if (f.uv[k][0] > u1) u1 = f.uv[k][0];
            if (f.uv[k][1] < v0) v0 = f.uv[k][1];
            if (f.uv[k][1] > v1) v1 = f.uv[k][1];
        }
        float len[3];
        bool degenerate = false;
        for (int k = 0; k < 3; k++) {
            float dx = us[(k + 1) % 3] - us[k], dy = vs[(k + 1) % 3] - vs[k];
            len[k] = sqrtf(dx * dx + dy * dy);
            if (len[k] < 1e-3f) degenerate = true;
        }
        if (degenerate) continue;

        int texel_px = (r.src.bpp == 8) ? ppw / 2 : ppw / 4;
        int x_base = (r.page_x - vram_x) * ppw;
        for (int v = v0; v <= v1; v++) {
            int y = r.page_y + v - vram_y;
            if (y < 0 || y >= h) continue;
            float qy = v + 0.5f;
            for (int u = u0; u <= u1; u++) {
                float qx = u + 0.5f;
                float d[3];
                for (int k = 0; k < 3; k++) {
                    int k2 = (k + 1) % 3;
                    d[k] = ((us[k2] - us[k]) * (qy - vs[k]) -
                            (vs[k2] - vs[k]) * (qx - us[k])) / len[k];
                }
                bool inside = (d[0] >= -1.0f && d[1] >= -1.0f && d[2] >= -1.0f) ||
                              (d[0] <=  1.0f && d[1] <=  1.0f && d[2] <=  1.0f);
                if (!inside) continue;
                int x0 = x_base + u * texel_px;
                for (int s = 0; s < texel_px; s++) {
                    int x = x0 + s;
                    if (x >= 0 && x < w) src[(size_t)y * w + x] = r.src;
                }
            }
        }
    }

    /* Fallback for uncovered texels: per 64-halfword x 256-row page cell,
       then for the whole texture. */
    TexelSrc whole = dominant_src(src, w, 0, 0, w, h);
    if (!whole.bpp) {
        free(src); free(linear); free(rgba); free(refs); free(pal_rgba);
        return false;
    }
    int cell_w = 64 * ppw;
    int first_cell_x = vram_x - (vram_x % 64);
    int first_cell_y = vram_y - (vram_y % 256);
    for (int cy0 = first_cell_y; cy0 < vram_y + h; cy0 += 256) {
        for (int cx0 = first_cell_x; cx0 < vram_x + vram_w; cx0 += 64) {
            int x0 = (cx0 - vram_x) * ppw, y0 = cy0 - vram_y;
            int x1 = x0 + cell_w, y1 = y0 + 256;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;
            TexelSrc cell = dominant_src(src, w, x0, y0, x1, y1);
            if (!cell.bpp) cell = whole;

            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    TexelSrc s = src[(size_t)y * w + x];
                    bool dim = !s.bpp;
                    if (dim) s = cell;
                    u8 b = linear[(size_t)y * byte_w + (x * 2) / ppw];
                    int idx = (s.bpp == 4) ? ((x & 1) ? (b >> 4) : (b & 0xF)) : b;
                    RGBA8 c = pal_rgba[(size_t)s.pal * 4096 + s.row * 256 + ((s.off + idx) & 0xFF)];
                    if (dim) c.a = (u8)(c.a * TEXUSE_DIM_ALPHA / 255);
                    u8* o = rgba + ((size_t)y * w + x) * 4;
                    o[0] = c.r; o[1] = c.g; o[2] = c.b; o[3] = c.a;
                }
            }
        }
    }

    free(src); free(linear); free(refs); free(pal_rgba);
    *out_rgba = rgba; *out_w = w; *out_h = h;
    return true;
}

/*═══════════════════════════════════════════════════════════════════
 *  3D viewer atlas
 *
 *  The PS1 has one 1024x512 VRAM of 16-bit words.  A face's tpage
 *  selects a page of it and its clut a row of colours; a texture entry
 *  gives the VRAM position its pixels are loaded at, a palette entry the
 *  position of its colours.  The atlas is that VRAM decoded to RGBA,
 *  once per CLUT in use: every texture drawn where it loads, four
 *  pixels per halfword (8bpp texels doubled), in a 512-row slice.
 *═══════════════════════════════════════════════════════════════════*/
int build_texture_atlas(const DatArchive& ar,
                        const u16* used_cluts, const bool* used_is4bpp, int n_used,
                        int max_h, TexAtlasLayout& lay, u8** out_rgba)
{
    *out_rgba = 0;
    memset(&lay, 0, sizeof(lay));
    memset(lay.slice_map, -1, sizeof(lay.slice_map));

    /* Collect texture and palette entries */
    struct TexInfo { int idx; int vram_x, vram_w, vram_h; };
    struct PalInfo { int idx; int vram_x, vram_y, vram_w, vram_h; };
    TexInfo texes[16]; int n_tex = 0;
    PalInfo pals[16];  int n_pal = 0;

    for (int i = 0; i < ar.count && n_tex < 16 && n_pal < 16; i++) {
        const DatEntry& e = ar.entries[i];
        if (e.type == DAT_TEXTURE || e.type == DAT_LZSS1 || e.type == DAT_TEXTURE_LINEAR) {
            /* Skip entries with invalid VRAM coords or tiny dims —
               type 3+4 pairs can be EMD skeleton+body data, not textures.
               PS1 VRAM is 1024x512 halfwords; anything outside is bogus. */
            if (e.x > 1024 || (e.w < 2 && e.h < 2))
                continue;
            TexInfo ti;
            ti.idx = i;
            ti.vram_x = e.x;
            ti.vram_w = e.w;
            ti.vram_h = e.h;
            texes[n_tex++] = ti;
        }
        if (e.type == DAT_PALETTE) {
            PalInfo pi;
            pi.idx = i;
            pi.vram_x = e.x;
            pi.vram_y = e.y;
            pi.vram_w = e.w;
            pi.vram_h = e.h;
            pals[n_pal++] = pi;
        }
    }
    if (n_tex == 0) return 0;

    /* Item sprite banks (item.dat/item2.dat) have embedded CLUTs per block
       rather than separate DAT_PALETTE entries, which this cannot find.
       Their textures are shown one at a time by the image panel. */
    if (ar.is_item_bank)
        return -1;

    /* ─── Composite into shared VRAM image ─── */
    /* Determine the VRAM halfword X range. */
    int min_vx = 0x7FFF, max_vx_end = 0;
    for (int i = 0; i < n_tex; i++) {
        if (texes[i].vram_x < min_vx) min_vx = texes[i].vram_x;
        int end = texes[i].vram_x + texes[i].vram_w;
        if (end > max_vx_end) max_vx_end = end;
    }
    int total_vram_hw = max_vx_end - min_vx;
    if (total_vram_hw <= 0) return -1;

    /* Pixel width: always use 4bpp pixel width (widest) so that both 8bpp and
       4bpp faces can coexist.  8bpp columns are pixel-doubled horizontally;
       atlas_face_uv maps 8bpp face UVs across the doubled pixels
       (ast10c.dat: 8bpp faces on tx=5, 4bpp on tx=7). */
    int bpp = 4;
    int ppw = 4;
    int total_pw = total_vram_hw * ppw;

    /* Height: 512 (full VRAM height) per slice */
    int padded_h = 512;

    /* ─── Build CLUT atlas using absolute VRAM Y positions ─── */
    /* The mesh face CLUT values reference the palette's VRAM upload position
       (e.g. y=505), not post-relocation positions (y=0,1,2). */
    int clut_y_min = 0x1FF, clut_y_max = 0;
    for (int pi = 0; pi < n_pal; pi++) {
        int py = pals[pi].vram_y;
        int ph = pals[pi].vram_h;
        if (ph < 1) ph = 1;
        if (py < clut_y_min) clut_y_min = py;
        if (py + ph - 1 > clut_y_max) clut_y_max = py + ph - 1;
    }
    int total_clut_rows = clut_y_max - clut_y_min + 1;
    if (total_clut_rows < 1) total_clut_rows = 1;
    if (total_clut_rows > 16) total_clut_rows = 16;

    /* Match palette to each texture (forward adjacency) */
    struct TexPalPair {
        int tex_i;               /* index into texes[] */
        const DatEntry* pal;     /* matched palette entry */
        int pal_vram_y;          /* VRAM Y of matched palette */
        int pal_rows;            /* number of palette rows (height) */
        int own;                 /* index into pals[] of the palette that
                                    follows the texture, -1 = it has none */
    };
    TexPalPair pairs[16];

    for (int ti = 0; ti < n_tex; ti++) {
        pairs[ti].tex_i = ti;
        pairs[ti].pal = 0;
        pairs[ti].pal_vram_y = clut_y_min;
        pairs[ti].pal_rows = 1;
        pairs[ti].own = -1;

        /* Match palette: use the next palette entry after this texture */
        int tex_idx = texes[ti].idx;
        for (int pi = 0; pi < n_pal; pi++) {
            int pal_idx = pals[pi].idx;
            if (pal_idx > tex_idx) {
                bool intervening = false;
                for (int tj = 0; tj < n_tex; tj++) {
                    if (tj != ti && texes[tj].idx > tex_idx && texes[tj].idx < pal_idx)
                        intervening = true;
                }
                if (!intervening) {
                    pairs[ti].pal = &ar.entries[pal_idx];
                    pairs[ti].pal_vram_y = pals[pi].vram_y;
                    pairs[ti].pal_rows = pals[pi].vram_h;
                    if (pairs[ti].pal_rows < 1) pairs[ti].pal_rows = 1;
                    pairs[ti].own = pi;
                    break;
                }
            }
        }
        if (!pairs[ti].pal && n_pal > 0) {
            pairs[ti].pal = &ar.entries[pals[0].idx];
            pairs[ti].pal_vram_y = pals[0].vram_y;
            pairs[ti].pal_rows = pals[0].vram_h;
            if (pairs[ti].pal_rows < 1) pairs[ti].pal_rows = 1;
        }
    }

    /* ─── 2D sparse sub-palette atlas: (row, slot) pairs from used_cluts ─── */
    /* slice_map layout: row * 65 + slot.
       slot 0 = 8bpp (pixel-doubled) for this row.
       slot 1..64 = 4bpp sub-palette rel_sub = slot - 1.
       This prevents 4bpp sub=0 (CLUT X == base_x) from colliding with 8bpp. */
    int num_sub_pals = 1;
    int clut_base_x = 0;
    /* Dino Crisis 2: the palette a slice's CLUT lies in (index into pals,
       -1 = none) and the index of the CLUT's first colour in it. */
    int slice_pal[16 * 65], slice_base[16 * 65];
    bool row_8bpp[16];
    memset(slice_pal, -1, sizeof(slice_pal));
    memset(slice_base, 0, sizeof(slice_base));
    memset(row_8bpp, 0, sizeof(row_8bpp));
    if (n_pal > 0) {
        clut_base_x = pals[0].vram_x;
        /* DC2 packages put palettes at several x on a row (WEP_P000's is
           at x = 160), in any order: count sub-palettes from the leftmost */
        if (ar.is_dc2)
            for (int pi = 1; pi < n_pal; pi++)
                if (pals[pi].vram_x < clut_base_x) clut_base_x = pals[pi].vram_x;
        int base_sub = clut_base_x / 16;

        int compact = 0;
        /* Always allocate 8bpp slice for row 0 */
        lay.slice_map[0 * 65 + 0] = compact++;
        for (int ci = 0; ci < n_used; ci++) {
            u16 cl = used_cluts[ci];
            int cy = (cl >> 6) & 0x1FF;
            int cx = (cl & 0x3F) * 16;
            int row = cy - clut_y_min;
            if (row < 0) row = 0;
            if (row >= 16) row = 15;
            int rel = cx / 16 - base_sub;
            if (rel < 0) rel = 0;
            if (rel >= 64) rel = 63;

            /* Allocate 8bpp slice for this row (slot 0) */
            int key8 = row * 65 + 0;
            if (lay.slice_map[key8] < 0)
                lay.slice_map[key8] = compact++;

            /* If any 4bpp face uses this CLUT, allocate 4bpp slice (slot rel+1) */
            int key4 = row * 65 + rel + 1;  /* +1 to avoid 8bpp slot */
            if (used_is4bpp[ci]) {
                if (lay.slice_map[key4] < 0)
                    lay.slice_map[key4] = compact++;
            }

            /* DC2: the palette loaded where the CLUT points, for textures
               that have no palette of their own (see the render loop).
               DC1 keeps the pairing by entry order alone (B28). */
            if (!ar.is_dc2) continue;
            int found = -1;
            for (int pi = 0; pi < n_pal && found < 0; pi++) {
                int ph = pals[pi].vram_h < 1 ? 1 : pals[pi].vram_h;
                if (cx >= pals[pi].vram_x && cx < pals[pi].vram_x + pals[pi].vram_w &&
                    cy >= pals[pi].vram_y && cy < pals[pi].vram_y + ph)
                    found = pi;
            }
            if (found < 0) continue;
            int first = (cy - pals[found].vram_y) * pals[found].vram_w + cx - pals[found].vram_x;
            if (first + 256 > 4096) continue;
            /* The row's one 8bpp slice goes to an 8bpp face's CLUT if any */
            if (slice_pal[key8] < 0 || (!used_is4bpp[ci] && !row_8bpp[row])) {
                slice_pal[key8] = found; slice_base[key8] = first;
                if (!used_is4bpp[ci]) row_8bpp[row] = true;
            }
            if (used_is4bpp[ci] && slice_pal[key4] < 0) {
                slice_pal[key4] = found; slice_base[key4] = first;
            }
        }
        num_sub_pals = compact > 0 ? compact : 1;
    }

    int atlas_h = padded_h * num_sub_pals;
    if (atlas_h > max_h) {
        int max_slices = max_h / padded_h;
        if (max_slices < 1) max_slices = 1;
        num_sub_pals = max_slices;
        atlas_h = padded_h * num_sub_pals;
        /* Invalidate slices that got cut */
        for (int i = 0; i < 16 * 65; i++)
            if (lay.slice_map[i] >= num_sub_pals)
                lay.slice_map[i] = -1;
    }

    lay.w = total_pw;
    lay.h = atlas_h;
    lay.bpp = bpp;
    lay.vram_x = min_vx;
    lay.vram_y = 0;  /* atlas covers full TY pages from y=0 */
    lay.num_pal_rows = total_clut_rows;
    lay.clut_base_y = clut_y_min;
    lay.num_slices = num_sub_pals;
    lay.clut_base_x = clut_base_x;

    u8* rgba = (u8*)malloc((size_t)total_pw * atlas_h * 4);
    if (!rgba) return -1;
    /* Fill with opaque black (not transparent) so that faces with UVs
       landing in unpopulated atlas regions render as solid black instead
       of being discarded by the alpha test. */
    {   size_t total_px = (size_t)total_pw * atlas_h;
        for (size_t px = 0; px < total_px; px++) {
            rgba[px*4]   = 0;    /* R */
            rgba[px*4+1] = 0;    /* G */
            rgba[px*4+2] = 0;    /* B */
            rgba[px*4+3] = 255;  /* A = opaque */
        }
    }

    /* Render each texture for EACH (CLUT Y, sub_pal) combo.
       Strategy for mixed 8bpp/4bpp rooms:
       - Atlas pixel width uses 4bpp convention (4 pixels per halfword)
       - 8bpp TPage columns: render at 8bpp, pixel-double horizontally
       - 4bpp TPage columns: render at 4bpp with per-sub-palette variants
       The sub-pal variants only affect 4bpp regions; 8bpp regions are
       identical across all sub-palette slices (pixel-doubled). */
    RGBA8* palette = (RGBA8*)malloc(4096 * sizeof(RGBA8));   /* 16 rows of 256 */
    if (!palette) { free(rgba); return -1; }
    for (int ti = 0; ti < n_tex; ti++) {
        const DatEntry& tex = ar.entries[texes[ti].idx];
        const u8* pix = tex.data; size_t pix_size = tex.size;
        Buffer dec;
        if (tex.type == DAT_LZSS1) {
            if (lzss_decompress(tex.data, tex.size, dec))
                { pix = dec.data; pix_size = dec.size; }
        }

        int vw = tex.w, vh = tex.h;
        if (vw == 0 || vh == 0) continue;

        /* Compute actual height from data size — DAT header h can be smaller
           than the real data (e.g. door textures: h=40 but data = 64 rows).
           Deswizzle block tiling requires the full height to decode correctly. */
        int byte_w = vw * 2;
        int actual_vh = (byte_w > 0 && pix_size > 0) ? (int)(pix_size / byte_w) : vh;
        if (actual_vh < vh) actual_vh = vh;  /* never shrink below header */
        vh = actual_vh;

        int x_off_hw = texes[ti].vram_x - min_vx;

        /* Deswizzle once for this texture */
        size_t total_bytes = (size_t)byte_w * vh;
        u8* linear = (u8*)calloc(total_bytes, 1);
        if (!linear) continue;
        deswizzle_8bpp(pix, pix_size, linear, vw, vh);

        const DatEntry* parsed = 0;   /* the palette entry in palette[] */
        for (int pi2 = 0; pi2 < 4096; pi2++) palette[pi2] = RGBA8(0,0,0,0);

        for (int row = 0; row < total_clut_rows; row++) {
            for (int slot = 0; slot < 65; slot++) {
                int map_key = row * 65 + slot;
                int slice_idx = lay.slice_map[map_key];
                if (slice_idx < 0) continue;

                /* slot 0 = 8bpp rendering, slot 1..64 = 4bpp with rel_sub = slot-1 */
                bool is_8bpp_slot = (slot == 0);
                int rel_sub = is_8bpp_slot ? 0 : slot - 1;

                /* The palette for this texture in this slice, and the index
                   of the CLUT's first colour in it (base8 for 8bpp, base4
                   for 4bpp).

                   DC1: the palette that follows the texture, by row, and
                   the sub-palette counted from the first palette's x.

                   DC2: the palette that follows the texture too, with the
                   sub-palette counted from that palette's own x (WEP_P000's
                   weapon palette is at x = 160).  The palette loaded where
                   the CLUT points is used when no palette follows the
                   texture, or a 4bpp CLUT lies outside the one that does.
                   It is not preferred, although it is what the hardware
                   reads: the two differ for one model of the game,
                   M_RESULT.DAT's, whose header has the CLUT at (0, 511),
                   the palette of the "RESULT" picture.  Its page (896, 256)
                   is a gold swirl that decodes cleanly with the palette
                   after it (y = 510) and to noise with that one, so the
                   game must set that model's CLUT itself. */
                const DatEntry* pal;
                int base8, base4;
                int own = ar.is_dc2 ? pairs[ti].own : -1;
                int own_sub = rel_sub;
                if (own >= 0 && !is_8bpp_slot)
                    own_sub = (clut_base_x + rel_sub * 16 - pals[own].vram_x) / 16;
                bool own_fits = own >= 0 && own_sub >= 0 &&
                                (is_8bpp_slot || own_sub * 16 < pals[own].vram_w);
                if (slice_pal[map_key] >= 0 && !own_fits) {
                    pal = &ar.entries[pals[slice_pal[map_key]].idx];
                    base8 = base4 = slice_base[map_key];
                } else {
                    pal = pairs[ti].pal;
                    int this_pal_rows = pairs[ti].pal_rows;
                    int pal_row_offset = clut_y_min + row - pairs[ti].pal_vram_y;
                    if (pal_row_offset < 0 || pal_row_offset >= this_pal_rows) {
                        if (this_pal_rows == 1) pal_row_offset = 0;
                        else continue;
                    }
                    base8 = pal_row_offset * 256;
                    base4 = base8 + (own_fits ? own_sub : rel_sub) * 16;
                }
                if (pal != parsed) {
                    for (int pi2 = 0; pi2 < 4096; pi2++) palette[pi2] = RGBA8(0,0,0,0);
                    if (pal && pal->data && pal->size > 0) {
                        int np = (int)(pal->size / 512);
                        if (np < 1) np = 1;
                        if (np > 16) np = 16;
                        parse_palette(pal->data, pal->size, palette, np);
                    }
                    parsed = pal;
                }

                /* Classify 4bpp sub-palette as decal overlay vs opaque texture.
                   Decal = majority of 16 entries have STP=1 (blood splatters, overlays).
                   Opaque = most entries STP=0 (character textures, walls).
                   Only decal sub-palettes get STP alpha; opaque ones stay fully solid. */
                bool is_4bpp_decal = false;
                if (!is_8bpp_slot && pal && pal->data) {
                    int stp_count = 0;
                    for (int ci = 1; ci < 16; ci++) { /* skip index 0 */
                        size_t raw_off = (size_t)(base4 + ci) * 2;
                        if (raw_off + 2 <= pal->size) {
                            u16 raw = rd_u16(pal->data + raw_off);
                            if (raw & 0x8000) stp_count++;
                        }
                    }
                    is_4bpp_decal = (stp_count >= 8); /* majority threshold: 8+ out of 15 */
                }

                bool is_8bpp_pal = (pal && pal->size >= 512);
                int y_off_in_slice = tex.y;
                for (int y = 0; y < vh && (y + y_off_in_slice) < padded_h; y++) {
                    int dst_y = slice_idx * padded_h + y_off_in_slice + y;
                    if (dst_y >= atlas_h) break;

                    for (int bx = 0; bx < byte_w; bx++) {
                        size_t si = (size_t)y * byte_w + bx;
                        u8 byte_val = (si < total_bytes) ? linear[si] : 0;

                        int atlas_x = (x_off_hw * 2 + bx) * 2;
                        if (atlas_x + 1 >= total_pw) continue;

                        size_t di0 = ((size_t)dst_y * total_pw + atlas_x) * 4;
                        size_t di1 = ((size_t)dst_y * total_pw + atlas_x + 1) * 4;

                        if (is_8bpp_slot && is_8bpp_pal) {
                            /* 8bpp: one pixel per byte, pixel-doubled */
                            RGBA8 c8 = palette[base8 + byte_val];
                            /* 8bpp index 0 is the background/mask index.
                               0x0000 already has alpha=0 from parse_palette.
                               0x8000 (STP black) at index 0 is also a mask —
                               semi-trans primitives like blood trails need it
                               transparent so the floor shows through.
                               Not in DC2: its models are drawn opaque, where
                               0x8000 is black.  E20, E30, E50, E90, EA0 and
                               SC05 have it at index 0 and use the index for
                               dark texels, which showed as holes. */
                            if (!ar.is_dc2 && byte_val == 0 && c8.a > 0 && pal && pal->data) {
                                size_t raw_off = (size_t)base8 * 2;
                                if (raw_off + 2 <= pal->size) {
                                    u16 raw = rd_u16(pal->data + raw_off);
                                    if (raw == 0x8000) c8.a = 0;
                                }
                            }
                            rgba[di0]   = c8.r; rgba[di0+1] = c8.g;
                            rgba[di0+2] = c8.b; rgba[di0+3] = c8.a;
                            rgba[di1]   = c8.r; rgba[di1+1] = c8.g;
                            rgba[di1+2] = c8.b; rgba[di1+3] = c8.a;
                        } else {
                            /* 4bpp: two nibbles per byte */
                            u8 lo = byte_val & 0x0F;
                            u8 hi = (byte_val >> 4) & 0x0F;
                            RGBA8 c4_lo = palette[base4 + lo];
                            RGBA8 c4_hi = palette[base4 + hi];
                            /* Apply STP semi-transparency only for decal sub-palettes.
                               Decal = majority of the 16 entries have STP=1.
                               For opaque sub-palettes (shirt, character), STP is just
                               a VRAM artifact and 0x8000 at index 0 = opaque black. */
                            if (pal && pal->data && is_4bpp_decal) {
                                size_t raw_off_lo = (size_t)(base4 + lo) * 2;
                                size_t raw_off_hi = (size_t)(base4 + hi) * 2;
                                if (raw_off_lo + 2 <= pal->size) {
                                    u16 raw_lo = rd_u16(pal->data + raw_off_lo);
                                    if (raw_lo == 0x8000) c4_lo.a = 0;
                                    else if ((raw_lo & 0x8000) && c4_lo.a > 0) c4_lo.a = 128;
                                }
                                if (raw_off_hi + 2 <= pal->size) {
                                    u16 raw_hi = rd_u16(pal->data + raw_off_hi);
                                    if (raw_hi == 0x8000) c4_hi.a = 0;
                                    else if ((raw_hi & 0x8000) && c4_hi.a > 0) c4_hi.a = 128;
                                }
                            }
                            rgba[di0]   = c4_lo.r; rgba[di0+1] = c4_lo.g;
                            rgba[di0+2] = c4_lo.b; rgba[di0+3] = c4_lo.a;
                            rgba[di1]   = c4_hi.r; rgba[di1+1] = c4_hi.g;
                            rgba[di1+2] = c4_hi.b; rgba[di1+3] = c4_hi.a;
                        }
                    }
                }
            }
        }
        free(linear);
    }
    free(palette);

    *out_rgba = rgba;
    return 1;
}

/* 2D sparse slice lookup: slot 0 = 8bpp, slot rel+1 = 4bpp sub-palette. */
int atlas_face_slice(const TexAtlasLayout& a, u16 tpage, u16 clut)
{
    if (a.num_slices <= 1) return 0;
    int clut_y = (clut >> 6) & 0x1FF;
    int row = clut_y - a.clut_base_y;
    if (row < 0) row = 0;
    if (row >= 16) row = 15;

    int slice_idx;
    if (((tpage >> 7) & 3) == 0) { /* 4bpp face → slot rel+1 */
        int clut_x = (clut & 0x3F) * 16;
        int rel = (clut_x - a.clut_base_x) / 16;
        if (rel < 0) rel = 0;
        if (rel >= 64) rel = 63;
        slice_idx = a.slice_map[row * 65 + rel + 1];
        if (slice_idx < 0) slice_idx = a.slice_map[row * 65 + 0]; /* fallback: 8bpp */
    } else { /* 8bpp face → slot 0 */
        slice_idx = a.slice_map[row * 65 + 0];
    }
    if (slice_idx < 0) slice_idx = 0; /* ultimate fallback */
    return slice_idx;
}

void atlas_face_uv(const TexAtlasLayout& a, u16 tpage, u16 clut,
                   const u8 uv[3][2], float out[3][2])
{
    /* tex_ppw: pixels per VRAM halfword in the atlas */
    int tex_ppw = (a.bpp == 4) ? 4 : (a.bpp == 16) ? 1 : 2;
    int total_v_slices = a.num_slices;
    int single_h = (total_v_slices > 1) ? a.h / total_v_slices : a.h;

    /* PS1 tpage UV mapping */
    int tx = tpage & 0xF;
    int ty = (tpage >> 4) & 1;
    int tp = (tpage >> 7) & 3;
    int face_ppw = (tp == 0) ? 4 : (tp == 2) ? 1 : 2;

    float tw = (a.w > 0) ? (float)a.w : 256.0f;
    float th = (a.h > 0) ? (float)a.h : 256.0f;
    float u_off = (float)((tx * 64 - a.vram_x) * tex_ppw) / tw;
    float v_off = (float)(ty * 256 - a.vram_y) / (float)single_h;  /* within one slice */
    float u_scl = (float)tex_ppw / ((float)face_ppw * tw);

    float clut_v_off = 0.0f;
    if (total_v_slices > 1) {
        int slice_idx = atlas_face_slice(a, tpage, clut);
        clut_v_off = (float)(slice_idx * single_h) / th;
        v_off = v_off * ((float)single_h / th);
    }

    /* Sample texel centres.  The PSX reads texel (u, v) at UV (u, v);
       bilinear filtering does so only at the texel's centre, and at its
       corner takes a quarter from each of the texels around it.  Shifted
       by half a texel, a face blends only texels inside its own UV
       range.  Unshifted, a face mapped to one texel at the corner of a
       solid patch took in the masked texels beside it and came out see-
       through: ST108's wall box samples (240, 176) of a grey patch with
       0x0000 left of and above it. */
    for (int vi = 0; vi < 3; vi++) {
        out[vi][0] = u_off + ((float)uv[vi][0] + 0.5f) * u_scl;
        out[vi][1] = clut_v_off + v_off + ((float)uv[vi][1] + 0.5f) / th;
    }
}

/*─── Linear 8bpp Render (no VRAM block swizzle) ────────────────*/
/* If src_stride < pixel_w, data is stored as sequential vertical
   column strips of src_stride width (e.g. item sprite bank:
   src_stride=64, pixel_w=128 → two 64-wide columns stacked). */
void render_linear_8bpp(const u8* pixels, size_t pix_size,
                        const u8* pal_data, size_t pal_size,
                        int pixel_w, int pixel_h,
                        u8* out_rgba, int src_stride)
{
    /* Decode palette */
    RGBA8 palette[256];
    for (int i = 0; i < 256; i++) palette[i] = RGBA8(0, 0, 0, 0);
    if (pal_data && pal_size >= 2) {
        int ncolors = (int)(pal_size / 2);
        if (ncolors > 256) ncolors = 256;
        for (int i = 0; i < ncolors; i++) {
            u16 c = rd_u16(pal_data + i * 2);
            palette[i].r = (u8)((c & 0x1F) << 3);
            palette[i].g = (u8)(((c >> 5) & 0x1F) << 3);
            palette[i].b = (u8)(((c >> 10) & 0x1F) << 3);
            palette[i].a = (i == 0) ? 0 : ((c == 0) ? 0 : 255);
        }
    }

    if (src_stride <= 0) src_stride = pixel_w;

    for (int y = 0; y < pixel_h; y++) {
        for (int x = 0; x < pixel_w; x++) {
            size_t si;
            if (src_stride >= pixel_w) {
                /* Simple linear: src_stride == pixel_w */
                si = (size_t)y * pixel_w + x;
            } else {
                /* Column-strip layout: data stored as sequential
                   vertical columns of src_stride width */
                int col = x / src_stride;
                int lx  = x % src_stride;
                si = (size_t)col * pixel_h * src_stride
                   + (size_t)y * src_stride + lx;
            }
            size_t di = ((size_t)y * pixel_w + x) * 4;
            u8 idx = (si < pix_size) ? pixels[si] : 0;
            RGBA8 c = palette[idx];
            out_rgba[di]     = c.r;
            out_rgba[di + 1] = c.g;
            out_rgba[di + 2] = c.b;
            out_rgba[di + 3] = c.a;
        }
    }
}

/*─── Background extraction stub ─────────────────────────────────*/
bool is_background_data(const u8* dec_data, size_t dec_size)
{
    if (dec_size < 0x800) return false;
    /* Check for tile map signature at typical offset */
    /* Detailed heuristic matches Python is_lzss0_background() */
    u32 first = rd_u32(dec_data);
    if (first < 0x80000000u || first > 0x80200000u) return false;
    return true;  /* simplified check */
}

bool extract_background(const u8* dec_data, size_t dec_size,
                        Background& out)
{
    out.valid = false;
    /* TODO: Full background extraction matching Python */
    /* extract_lzss0_background() logic */
    (void)dec_data; (void)dec_size;
    return false;
}

/*─── 4× Texture Upscale (Scale2x cascade + alpha smoothing) ────*/

/* Pixel access helper (clamped edges) */
static inline void px_get(const u8* img, int w, int h, int x, int y, u8 out[4]) {
    if (x < 0) x = 0; if (x >= w) x = w - 1;
    if (y < 0) y = 0; if (y >= h) y = h - 1;
    const u8* p = img + ((size_t)y * w + x) * 4;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
}

static inline bool px_eq(const u8 a[4], const u8 b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static inline bool px_neq(const u8 a[4], const u8 b[4]) {
    return !px_eq(a, b);
}

static inline void px_set(u8* img, int w, int x, int y, const u8 c[4]) {
    u8* p = img + ((size_t)y * w + x) * 4;
    p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3];
}

/* Scale2x (EPX): 1 pixel → 2×2 block.
   For each pixel P with neighbors B(up), D(left), F(right), H(down):
     E0 = D==B && D!=H && B!=F ? D : P
     E1 = B==F && B!=D && F!=H ? F : P
     E2 = D==H && D!=B && H!=F ? D : P
     E3 = H==F && H!=D && F!=B ? F : P  */
static u8* scale2x(const u8* src, int w, int h) {
    int w2 = w * 2, h2 = h * 2;
    u8* dst = (u8*)calloc((size_t)w2 * h2, 4);
    if (!dst) return 0;

    u8 P[4], B[4], D[4], F[4], H[4];

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            px_get(src, w, h, x, y, P);
            px_get(src, w, h, x, y-1, B);
            px_get(src, w, h, x-1, y, D);
            px_get(src, w, h, x+1, y, F);
            px_get(src, w, h, x, y+1, H);

            u8 E0[4], E1[4], E2[4], E3[4];
            for (int c = 0; c < 4; c++) E0[c]=E1[c]=E2[c]=E3[c]=P[c];

            if (px_eq(D,B) && px_neq(D,H) && px_neq(B,F))
                { E0[0]=D[0]; E0[1]=D[1]; E0[2]=D[2]; E0[3]=D[3]; }
            if (px_eq(B,F) && px_neq(B,D) && px_neq(F,H))
                { E1[0]=F[0]; E1[1]=F[1]; E1[2]=F[2]; E1[3]=F[3]; }
            if (px_eq(D,H) && px_neq(D,B) && px_neq(H,F))
                { E2[0]=D[0]; E2[1]=D[1]; E2[2]=D[2]; E2[3]=D[3]; }
            if (px_eq(H,F) && px_neq(H,D) && px_neq(F,B))
                { E3[0]=F[0]; E3[1]=F[1]; E3[2]=F[2]; E3[3]=F[3]; }

            px_set(dst, w2, x*2,   y*2,   E0);
            px_set(dst, w2, x*2+1, y*2,   E1);
            px_set(dst, w2, x*2,   y*2+1, E2);
            px_set(dst, w2, x*2+1, y*2+1, E3);
        }
    }
    return dst;
}

/* Smooth alpha: 3×3 box blur on the alpha channel only.
   This creates anti-aliased transitions at transparency boundaries
   while keeping opaque/transparent interiors crisp. Only affects
   pixels near alpha edges (where neighbors have different alpha). */
static void smooth_alpha(u8* rgba, int w, int h) {
    /* Allocate temp alpha buffer */
    u8* tmp = (u8*)malloc((size_t)w * h);
    if (!tmp) return;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u8 center_a = rgba[((size_t)y * w + x) * 4 + 3];

            /* Check if this pixel is near an alpha edge */
            bool is_edge = false;
            int sum = 0, count = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0) nx = 0; if (nx >= w) nx = w - 1;
                    if (ny < 0) ny = 0; if (ny >= h) ny = h - 1;
                    u8 na = rgba[((size_t)ny * w + nx) * 4 + 3];
                    sum += na;
                    count++;
                    if (na != center_a) is_edge = true;
                }
            }

            if (is_edge) {
                /* Smooth: weighted average (center 4× + neighbors 1×) */
                tmp[y * w + x] = (u8)((center_a * 4 + (sum - center_a)) / (count + 3));
            } else {
                tmp[y * w + x] = center_a;
            }
        }
    }

    /* Write back smoothed alpha */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            rgba[((size_t)y * w + x) * 4 + 3] = tmp[y * w + x];

    free(tmp);
}

u8* upscale_4x_smooth(const u8* rgba, int w, int h,
                       int& out_w, int& out_h)
{
    /* Pass 1: Scale2x (1× → 2×) */
    u8* s2 = scale2x(rgba, w, h);
    if (!s2) { out_w = w; out_h = h; return 0; }
    int w2 = w * 2, h2 = h * 2;

    /* Pass 2: Scale2x again (2× → 4×) */
    u8* s4 = scale2x(s2, w2, h2);
    free(s2);
    if (!s4) { out_w = w; out_h = h; return 0; }
    int w4 = w * 4, h4 = h * 4;

    /* Smooth alpha edges for clean transparency boundaries */
    smooth_alpha(s4, w4, h4);

    out_w = w4;
    out_h = h4;
    return s4;
}
