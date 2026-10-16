/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  UI Panel Implementations
 *  Uses global T theme struct for colors
 *═══════════════════════════════════════════════════════════════════*/
#include "ui/panels.h"
#include "ui/app.h"
#include "ui/uv_editor.h"
#include "formats/texture.h"
#include "formats/mesh.h"
#include "core/audio.h"
#include "core/lzss.h"
#include "core/color.h"
#include <GL/gl.h>
#include <GL/glu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* GET_X_LPARAM / GET_Y_LPARAM — avoid dependency on windowsx.h */
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))
#endif

extern HexPanel      g_hex;
extern ImagePanel    g_image;
extern PalettePanel  g_palette;
extern AudioPanel    g_audio;
extern ViewerPanel3D g_viewer3d;

/*═══════════════════════════════════════════════════════════════════
 *  Hex Panel
 *═══════════════════════════════════════════════════════════════════*/
void HexPanel::set_data(const u8* d, size_t sz) {
    data = d; data_size = sz; scroll_pos = 0;
    if (d && sz > 0) {
        stop_idle();
        if (hwnd) {
            int lines = (int)((sz + bytes_per_row - 1) / bytes_per_row);
            SCROLLINFO si; memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si); si.fMask = SIF_RANGE|SIF_PAGE|SIF_POS;
            si.nMin = 0; si.nMax = lines; si.nPage = 30; si.nPos = 0;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            InvalidateRect(hwnd, 0, TRUE);
        }
    } else {
        start_idle();
    }
}

void HexPanel::start_idle() {
    if (!hwnd || idle_timer) return;
    idle_frame = 0;
    idle_timer = SetTimer(hwnd, 7777, 33, 0); /* ~30fps */
}

void HexPanel::stop_idle() {
    if (idle_timer && hwnd) { KillTimer(hwnd, idle_timer); idle_timer = 0; }
    if (idle_buf) { free(idle_buf); idle_buf = 0; idle_w = idle_h = 0; }
}

/*─── Idle animation: flowing wave (PSP/PS3 XMB style) ────────────
 *  Layered translucent sine waves with slow phase drift.
 *  Writes every pixel every frame via DIB → zero flicker.
 *  Cost: ~0.3ms for 800×600 on any modern CPU.
 *─────────────────────────────────────────────────────────────────*/
void HexPanel::paint_idle(HDC hdc, RECT& rc)
{
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 8 || h < 8) return;

    /* (Re)allocate backbuffer if size changed */
    if (w != idle_w || h != idle_h || !idle_buf) {
        free(idle_buf);
        idle_w = w; idle_h = h;
        idle_buf = (u32*)calloc((size_t)w * h, sizeof(u32));
        if (!idle_buf) return;
    }

    /* Ease-in: ramp opacity from 0→1 over first ~90 frames (~3 seconds) */
    float ease = (idle_frame < 90) ? (float)idle_frame / 90.0f : 1.0f;
    ease = ease * ease * (3.0f - 2.0f * ease);

    float t = (float)idle_frame * 0.04f;

    /* ── Theme-matched base colors ──
       Dark:  hexBg = RGB(15,15,30)  panelBg = RGB(38,38,42)
       Light: hexBg = RGB(255,255,255) panelBg = RGB(248,248,248) */
    float base_r, base_g, base_b;     /* center background */
    float vig_r, vig_g, vig_b;        /* vignette edge color (panelBg) */
    if (T.isDark) {
        base_r = 15.0f/255.0f;  base_g = 15.0f/255.0f;  base_b = 30.0f/255.0f;
        vig_r  = 38.0f/255.0f;  vig_g  = 38.0f/255.0f;  vig_b  = 42.0f/255.0f;
    } else {
        base_r = 1.0f;  base_g = 1.0f;  base_b = 1.0f;
        vig_r  = 248.0f/255.0f; vig_g = 248.0f/255.0f; vig_b = 248.0f/255.0f;
    }

    struct WaveLayer {
        float amp, freq, speed, y_center;
        float r, g, b, alpha;
    };

    /* Dark: subtle cool blues that read against RGB(15,15,30) */
    WaveLayer layers_dark[] = {
        { 0.08f, 0.7f, 0.60f, 0.72f,  0.08f, 0.10f, 0.18f, 0.65f },
        { 0.07f, 0.9f, 0.85f, 0.60f,  0.10f, 0.13f, 0.24f, 0.50f },
        { 0.06f, 1.2f, 1.10f, 0.50f,  0.12f, 0.17f, 0.30f, 0.38f },
        { 0.05f, 1.6f, 1.40f, 0.42f,  0.15f, 0.22f, 0.38f, 0.28f },
        { 0.04f, 2.1f, 1.80f, 0.35f,  0.20f, 0.28f, 0.48f, 0.20f },
    };

    /* Light: soft slate/lavender against white */
    WaveLayer layers_light[] = {
        { 0.08f, 0.7f, 0.60f, 0.72f,  0.82f, 0.83f, 0.88f, 0.50f },
        { 0.07f, 0.9f, 0.85f, 0.60f,  0.76f, 0.77f, 0.84f, 0.40f },
        { 0.06f, 1.2f, 1.10f, 0.50f,  0.70f, 0.71f, 0.80f, 0.32f },
        { 0.05f, 1.6f, 1.40f, 0.42f,  0.64f, 0.66f, 0.76f, 0.25f },
        { 0.04f, 2.1f, 1.80f, 0.35f,  0.58f, 0.60f, 0.72f, 0.18f },
    };

    WaveLayer* layers = T.isDark ? layers_dark : layers_light;
    int n_layers = 5;

    float inv_w = 1.0f / (float)w;
    float inv_h = 1.0f / (float)h;
    float TWO_PI = 6.2831853f;

    for (int y = 0; y < h; y++) {
        float ny = (float)y * inv_h;
        u32* row = idle_buf + y * w;

        for (int x = 0; x < w; x++) {
            float nx = (float)x * inv_w;
            float cr = base_r, cg = base_g, cb = base_b;

            for (int li = 0; li < n_layers; li++) {
                const WaveLayer& L = layers[li];
                float phase1 = t * L.speed;
                float phase2 = t * L.speed * 0.63f + 2.1f;

                float wave_y = L.y_center
                    + L.amp * sinf(nx * L.freq * TWO_PI + phase1)
                    + L.amp * 0.4f * sinf(nx * L.freq * TWO_PI * 2.1f + phase2);

                float dist = ny - wave_y;
                float fill;
                if (dist > 0.0f) {
                    float fade_d = dist * 3.5f;
                    fill = (fade_d < 1.0f) ? 1.0f - fade_d * 0.3f : 0.7f * expf(-(fade_d - 1.0f) * 1.5f);
                } else {
                    float d = -dist * 14.0f;
                    fill = expf(-d * d);
                }

                float a = L.alpha * fill * ease;
                if (a > 0.002f) {
                    float inv_a = 1.0f - a;
                    cr = cr * inv_a + L.r * a;
                    cg = cg * inv_a + L.g * a;
                    cb = cb * inv_a + L.b * a;
                }
            }

            /* Desaturate ~30% */
            float lum = cr * 0.299f + cg * 0.587f + cb * 0.114f;
            cr = cr + (lum - cr) * 0.30f;
            cg = cg + (lum - cg) * 0.30f;
            cb = cb + (lum - cb) * 0.30f;

            /* Vignette: blend toward panelBg at edges */
            float vx = (nx - 0.5f) * 2.0f;
            float vy = (ny - 0.5f) * 2.0f;
            float d2 = vx * vx * 0.7f + vy * vy;
            float vig_strength = d2 * 0.55f;
            if (vig_strength > 1.0f) vig_strength = 1.0f;
            cr = cr * (1.0f - vig_strength) + vig_r * vig_strength;
            cg = cg * (1.0f - vig_strength) + vig_g * vig_strength;
            cb = cb * (1.0f - vig_strength) + vig_b * vig_strength;

            int pr = (int)(cr * 255.0f + 0.5f); if (pr > 255) pr = 255; if (pr < 0) pr = 0;
            int pg = (int)(cg * 255.0f + 0.5f); if (pg > 255) pg = 255; if (pg < 0) pg = 0;
            int pb = (int)(cb * 255.0f + 0.5f); if (pb > 255) pb = 255; if (pb < 0) pb = 0;
            row[x] = 0xFF000000u | ((u32)pr << 16) | ((u32)pg << 8) | (u32)pb;
        }
    }

    /* ── Blit to screen ── */
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, 0, 0, w, h, 0, 0, 0, h, idle_buf, &bmi, DIB_RGB_COLORS);

    /* ── Text overlay ── */
    SetBkMode(hdc, TRANSPARENT);
    HFONT hf = CreateFontA(22, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, hf ? hf : font);

    const char* label = "Open a .DAT Archive   (Ctrl+O)";
    if (T.isDark) {
        int tr = (int)(60.0f + 80.0f * ease);
        int tg = (int)(60.0f + 80.0f * ease);
        int tb = (int)(80.0f + 100.0f * ease);
        SetTextColor(hdc, RGB(tr, tg, tb));
    } else {
        int tr = (int)(150.0f - 40.0f * ease);
        int tg = (int)(150.0f - 40.0f * ease);
        int tb = (int)(170.0f - 40.0f * ease);
        SetTextColor(hdc, RGB(tr, tg, tb));
    }
    DrawTextA(hdc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    if (hf) DeleteObject(hf);

    idle_frame++;
}

void HexPanel::paint(HDC hdc, RECT& rc) {
    HFONT old_font = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    if (!data || data_size == 0) {
        paint_idle(hdc, rc);
        SelectObject(hdc, old_font); return;
    }
    HBRUSH bgb = CreateSolidBrush(T.hexBg); FillRect(hdc, &rc, bgb); DeleteObject(bgb);
    TEXTMETRIC tm; GetTextMetrics(hdc, &tm);
    int line_h = tm.tmHeight + 2, visible = (rc.bottom - rc.top) / line_h;
    char line[256]; int y = 2;
    for (int row = scroll_pos; row < scroll_pos + visible; row++) {
        size_t off = (size_t)row * bytes_per_row;
        if (off >= data_size) break;
        SetTextColor(hdc, T.hexAddr);
        _snprintf(line, 255, "%08X  ", (unsigned)off);
        TextOutA(hdc, 4, y, line, (int)strlen(line));
        SetTextColor(hdc, T.hexByte);
        int hx = 4 + tm.tmAveCharWidth * 10;
        for (int i = 0; i < bytes_per_row; i++) {
            if (off + i < data_size) _snprintf(line, 4, "%02X ", data[off + i]);
            else _snprintf(line, 4, "   ");
            TextOutA(hdc, hx + i * tm.tmAveCharWidth * 3, y, line, 3);
        }
        SetTextColor(hdc, T.hexAscii);
        int ax = hx + bytes_per_row * tm.tmAveCharWidth * 3 + tm.tmAveCharWidth * 2;
        for (int i = 0; i < bytes_per_row; i++) {
            if (off + i < data_size) { u8 c = data[off + i]; line[i] = (c >= 32 && c < 127) ? (char)c : '.'; }
            else line[i] = ' ';
        }
        line[bytes_per_row] = '\0';
        TextOutA(hdc, ax, y, line, bytes_per_row);
        y += line_h;
    }
    SelectObject(hdc, old_font);
}

LRESULT CALLBACK HexPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc); g_hex.paint(hdc, rc);
        EndPaint(hwnd, &ps); } return 0;
    case WM_TIMER:
        if (wp == 7777 && (!g_hex.data || g_hex.data_size == 0)) {
            InvalidateRect(hwnd, 0, FALSE);
        }
        return 0;
    case WM_SHOWWINDOW:
        if (wp && (!g_hex.data || g_hex.data_size == 0))
            g_hex.start_idle();
        else if (!wp)
            g_hex.stop_idle();
        return 0;
    case WM_SIZE:
        if (g_hex.idle_buf) { free(g_hex.idle_buf); g_hex.idle_buf = 0; g_hex.idle_w = g_hex.idle_h = 0; }
        return 0;
    case WM_LBUTTONDOWN:
        /* Track that button-down happened inside this panel */
        if (!g_hex.data || g_hex.data_size == 0)
            g_hex.click_armed = true;
        return 0;
    case WM_LBUTTONUP:
        /* Click on idle panel → open file dialog.
           Only if button-down also happened here (not from title bar double-click). */
        if (g_hex.click_armed && (!g_hex.data || g_hex.data_size == 0))
            SendMessage(g_app.hMain, WM_COMMAND, IDM_FILE_OPEN, 0);
        g_hex.click_armed = false;
        return 0;
    case WM_SETCURSOR:
        /* Hand cursor when idle (clickable) */
        if (!g_hex.data || g_hex.data_size == 0) {
            SetCursor(LoadCursor(0, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_VSCROLL: {
        SCROLLINFO si; si.cbSize = sizeof(si); si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos--; break; case SB_LINEDOWN: pos++; break;
        case SB_PAGEUP: pos -= si.nPage; break; case SB_PAGEDOWN: pos += si.nPage; break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        }
        if (pos < 0) pos = 0;
        if (pos > si.nMax - (int)si.nPage) pos = si.nMax - si.nPage;
        g_hex.scroll_pos = pos;
        si.fMask = SIF_POS; si.nPos = pos;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        InvalidateRect(hwnd, 0, TRUE);
    } return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        g_hex.scroll_pos -= delta * 3;
        if (g_hex.scroll_pos < 0) g_hex.scroll_pos = 0;
        SCROLLINFO si; si.cbSize = sizeof(si); si.fMask = SIF_POS; si.nPos = g_hex.scroll_pos;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        InvalidateRect(hwnd, 0, TRUE);
    } return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        extern AppTheme T;
        if (T.isDark) {
            SetBkColor((HDC)wp, T.listBg);
            SetTextColor((HDC)wp, T.listText);
            static HBRUSH hBrHexEdit = 0;
            if (!hBrHexEdit) hBrHexEdit = CreateSolidBrush(T.listBg);
            return (LRESULT)hBrHexEdit;
        }
    } break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/*═══════════════════════════════════════════════════════════════════
 *  Image Panel
 *═══════════════════════════════════════════════════════════════════*/
void ImagePanel::clear_face_usage() {
    free(face_uses);
    face_uses = 0;
    n_face_uses = 0;
    face_uses_ready = false;
}

void ImagePanel::set_bitmap(const u8* rgba, int w, int h) {
    if (hBmp) { DeleteObject(hBmp); hBmp = 0; }
    img_w = w; img_h = h;
    showing_face_cluts = false;
    fitted = true;  /* auto-fit on next paint */
    if (!rgba || w <= 0 || h <= 0) return;
    /* Create a DIB section with premultiplied alpha for AlphaBlend */
    BITMAPINFO bmi; memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = 0;
    hBmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hBmp || !bits) return;
    u8* dst = (u8*)bits;
    for (int i = 0; i < w * h; i++) {
        u8 r = rgba[i*4], g = rgba[i*4+1], b = rgba[i*4+2], a = rgba[i*4+3];
        /* Premultiply alpha for AlphaBlend */
        dst[i*4]   = (u8)((b * a + 127) / 255);  /* B */
        dst[i*4+1] = (u8)((g * a + 127) / 255);  /* G */
        dst[i*4+2] = (u8)((r * a + 127) / 255);  /* R */
        dst[i*4+3] = a;
    }
}

/* Copy one entry and its bytes; rerender() passes the copies back in, so
   an entry that already is the copy is left alone. */
static void keep_entry(const DatEntry& e, DatEntry& copy, Buffer& bytes) {
    if (&e == &copy) return;
    copy = e;
    bytes.clear();
    if (e.data && e.size) bytes.append(e.data, e.size);
    copy.data = bytes.data;
    copy.owned = false;
}

void ImagePanel::keep_source(const DatEntry& tex, const DatEntry* pal) {
    keep_entry(tex, src_tex, src_tex_data);
    if (pal) keep_entry(*pal, src_pal, src_pal_data);
    cur_tex = &src_tex;
    cur_pal = pal ? &src_pal : 0;
}

void ImagePanel::render_entry(const DatEntry& tex, const DatEntry* pal, bool do_deswizzle) {
    keep_source(tex, pal);
    cur_is_linear = false;
    if (pal && pal->size > 0) {
        int row_bytes = (bpp == 4) ? 32 : 512;
        max_pal_rows = (int)(pal->size / row_bytes);
        if (max_pal_rows < 1) max_pal_rows = 1;
    } else {
        max_pal_rows = 1;
    }
    if (pal_row >= max_pal_rows) pal_row = max_pal_rows - 1;

    if (!tex.data || tex.size == 0) return;
    const u8* pix = tex.data; size_t pix_size = tex.size;
    Buffer dec;
    if (tex.type == DAT_LZSS1 || tex.type == DAT_DC2_LZSS2) {
        if (lzss_decompress(tex.data, tex.size, dec))
            { pix = dec.data; pix_size = dec.size; }
    }
    /* DC2 type 8 (the sprites of ST000.DBS) is stored row by row */
    if (tex.type == DAT_DC2_LZSS2) do_deswizzle = false;

    /* Decode the way the archive's faces sample this texture, if any do */
    if (face_cluts && do_deswizzle && g_app.archive && !g_app.archive->is_item_bank &&
        tex.w > 0 && tex.h > 0 && tex.x <= 1024) {
        if (!face_uses_ready) {
            n_face_uses = collect_texture_usage(*g_app.archive, &face_uses);
            face_uses_ready = true;
        }
        u8* rgba = 0; int fw = 0, fh = 0;
        if (render_texture_by_faces(pix, pix_size, tex.x, tex.y, tex.w, tex.h,
                                    face_uses, n_face_uses, *g_app.archive,
                                    &rgba, &fw, &fh)) {
            set_bitmap(rgba, fw, fh); free(rgba);
            showing_face_cluts = true;
            return;
        }
    }

    int vw = tex.w, vh = tex.h;
    if (vw == 0 || vh == 0) { vw = 128; vh = 128; }
    int pw, ph;
    texture_pixel_dims(vw, vh, bpp, pw, ph);
    u8* rgba = (u8*)calloc(pw * ph, 4);
    render_texture(pix, pix_size, pal ? pal->data : 0, pal ? pal->size : 0,
                   vw, vh, bpp, rgba, do_deswizzle, pal_row, sub_pal);
    set_bitmap(rgba, pw, ph); free(rgba);
}

void ImagePanel::render_linear(const DatEntry& tex, const DatEntry* pal) {
    keep_source(tex, pal);
    cur_is_linear = true;
    if (pal && pal->size > 0) {
        int row_bytes = (bpp == 4) ? 32 : 512;
        max_pal_rows = (int)(pal->size / row_bytes);
        if (max_pal_rows < 1) max_pal_rows = 1;
    } else {
        max_pal_rows = 1;
    }
    if (pal_row >= max_pal_rows) pal_row = max_pal_rows - 1;

    if (!tex.data || tex.size == 0) return;
    int vw = tex.w, vh = tex.h;
    if (vw <= 0 || vh <= 0) return;
    int pw = vw * 2;  /* pixel width for 8bpp */
    size_t total = (size_t)pw * vh;

    u8* linear = (u8*)calloc(total, 1);
    if (!linear) return;
    deswizzle_8bpp_remfirst(tex.data, tex.size, linear, vw, vh);

    u8* rgba = (u8*)calloc(total, 4);
    if (!rgba) { free(linear); return; }
    render_texture(linear, total, pal ? pal->data : 0, pal ? pal->size : 0,
                   vw, vh, bpp, rgba, false, pal_row, sub_pal);
    set_bitmap(rgba, pw, vh);
    free(rgba);
    free(linear);
}

void ImagePanel::rerender() {
    if (!cur_tex) return;
    /* Preserve zoom/pan across re-render */
    double sz = zoom, spx = pan_x, spy = pan_y;
    bool sf = fitted;
    if (cur_is_linear)
        render_linear(*cur_tex, cur_pal);
    else
        render_entry(*cur_tex, cur_pal);
    zoom = sz; pan_x = spx; pan_y = spy; fitted = sf;
}

void ImagePanel::reset_view() {
    fitted = true;
    if (!hwnd || !hBmp || img_w <= 0 || img_h <= 0) return;
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    if (fit_to_screen) {
        double sx = (double)cw / img_w, sy = (double)ch / img_h;
        zoom = (sx < sy) ? sx : sy;
    } else {
        zoom = 1.0;
    }
    pan_x = (cw - img_w * zoom) * 0.5;
    pan_y = (ch - img_h * zoom) * 0.5;
}

void ImagePanel::zoom_at(int cx, int cy, double factor) {
    double new_zoom = zoom * factor;
    if (new_zoom < 0.1) new_zoom = 0.1;
    if (new_zoom > 64.0) new_zoom = 64.0;
    if (new_zoom == zoom) return;

    pan_x = cx - (cx - pan_x) * (new_zoom / zoom);
    pan_y = cy - (cy - pan_y) * (new_zoom / zoom);
    zoom = new_zoom;
    fitted = false;
}

void ImagePanel::paint(HDC hdc, RECT& rc) {
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    /* ── Double-buffer ── */
    HDC buf = CreateCompatibleDC(hdc);
    HBITMAP bufBmp = CreateCompatibleBitmap(hdc, cw, ch);
    HGDIOBJ bufOld = SelectObject(buf, bufBmp);

    /* ── Checkerboard across entire panel ── */
    {
        COLORREF ca = T.isDark ? RGB(42, 42, 46)  : RGB(225, 225, 228);
        COLORREF cb = T.isDark ? RGB(54, 54, 58)  : RGB(240, 240, 244);
        HBRUSH brA = CreateSolidBrush(ca);
        HBRUSH brB = CreateSolidBrush(cb);
        int csz = 16;
        for (int y = 0; y < ch; y += csz) {
            for (int x = 0; x < cw; x += csz) {
                RECT c = { x, y, x + csz, y + csz };
                if (c.right > cw) c.right = cw;
                if (c.bottom > ch) c.bottom = ch;
                FillRect(buf, &c, ((x / csz + y / csz) & 1) ? brB : brA);
            }
        }
        DeleteObject(brA);
        DeleteObject(brB);
    }

    if (!hBmp || img_w <= 0 || img_h <= 0) {
        SetTextColor(buf, T.textDim); SetBkMode(buf, TRANSPARENT);
        RECT tr = rc;
        DrawTextA(buf, "Select a texture entry", -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        goto blit_out;
    }

    /* Auto-fit/center when flagged */
    if (fitted) {
        if (fit_to_screen) {
            double sx = (double)cw / img_w, sy = (double)ch / img_h;
            zoom = (sx < sy) ? sx : sy;
        } else {
            zoom = 1.0;
        }
        pan_x = (cw - img_w * zoom) * 0.5;
        pan_y = (ch - img_h * zoom) * 0.5;
    }

    {
        int dw = (int)(img_w * zoom + 0.5);
        int dh = (int)(img_h * zoom + 0.5);
        int dx = (int)pan_x;
        int dy = (int)pan_y;

        HDC mem = CreateCompatibleDC(buf);
        HGDIOBJ old = SelectObject(mem, hBmp);
        SetStretchBltMode(buf, (zoom >= 2.0) ? COLORONCOLOR : HALFTONE);
        if (zoom < 2.0) SetBrushOrgEx(buf, 0, 0, NULL);
        BLENDFUNCTION bf; memset(&bf, 0, sizeof(bf));
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;  /* premultiplied source */
        AlphaBlend(buf, dx, dy, dw, dh, mem, 0, 0, img_w, img_h, bf);
        SelectObject(mem, old);
        DeleteDC(mem);

        /* Info overlay */
        {
            char info[128];
            int pct = (int)(zoom * 100.0 + 0.5);
            const char* face_hint = (!face_cluts && n_face_uses > 0) ? "  [C: room CLUTs]" : "";
            if (showing_face_cluts)
                _snprintf(info, 127, "%dx%d  %d%%  CLUTs from room faces, areas no 3D face uses dimmed [C: single CLUT]",
                          img_w, img_h, pct);
            else if (max_pal_rows > 1)
                _snprintf(info, 127, "%dx%d  %d%%  CLUT %d/%d [Up/Down]%s",
                          img_w, img_h, pct, pal_row + 1, max_pal_rows, face_hint);
            else
                _snprintf(info, 127, "%dx%d  %d%%%s", img_w, img_h, pct, face_hint);
            SetBkMode(buf, TRANSPARENT);
            SetTextColor(buf, RGB(0, 0, 0));
            TextOutA(buf, 7, ch - 19, info, (int)strlen(info));
            SetTextColor(buf, T.isDark ? RGB(180, 180, 180) : RGB(60, 60, 60));
            TextOutA(buf, 6, ch - 20, info, (int)strlen(info));
        }
    }

blit_out:
    BitBlt(hdc, 0, 0, cw, ch, buf, 0, 0, SRCCOPY);
    SelectObject(buf, bufOld);
    DeleteObject(bufBmp);
    DeleteDC(buf);
}

LRESULT CALLBACK ImagePanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        g_image.paint(hdc, rc);
        EndPaint(hwnd, &ps);
    } return 0;

    case WM_ERASEBKGND: return 1;

    case WM_MOUSEWHEEL: {
        short delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
        ScreenToClient(hwnd, &pt);
        double factor = (delta > 0) ? 1.15 : (1.0 / 1.15);
        g_image.zoom_at(pt.x, pt.y, factor);
        InvalidateRect(hwnd, 0, FALSE);
    } return 0;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        SetFocus(hwnd);
        g_image.dragging = true;
        g_image.drag_x = GET_X_LPARAM(lp);
        g_image.drag_y = GET_Y_LPARAM(lp);
        g_image.drag_pan_x = g_image.pan_x;
        g_image.drag_pan_y = g_image.pan_y;
        SetCapture(hwnd);
    } return 0;

    case WM_MOUSEMOVE: {
        if (g_image.dragging) {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            g_image.pan_x = g_image.drag_pan_x + (mx - g_image.drag_x);
            g_image.pan_y = g_image.drag_pan_y + (my - g_image.drag_y);
            g_image.fitted = false;
            InvalidateRect(hwnd, 0, FALSE);
        }
    } return 0;

    case WM_LBUTTONUP:
    case WM_MBUTTONUP: {
        if (g_image.dragging) {
            g_image.dragging = false;
            ReleaseCapture();
        }
    } return 0;

    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: {
        /* Double-click or right-click: reset to fit */
        SetFocus(hwnd);
        g_image.reset_view();
        InvalidateRect(hwnd, 0, FALSE);
    } return 0;

    case WM_SIZE: {
        if (g_image.fitted)
            InvalidateRect(hwnd, 0, FALSE);
    } return 0;

    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, g_image.dragging ? IDC_SIZEALL : IDC_ARROW));
            return TRUE;
        }
    } break;

    case WM_KEYDOWN: {
        /* C toggles room-face CLUTs; Up/Down picks a single CLUT row, so
           it leaves room-face mode first. */
        bool to_single = g_image.showing_face_cluts && (wp == VK_UP || wp == VK_DOWN);
        if (wp == 'C' || to_single) {
            g_image.face_cluts = to_single ? false : !g_image.face_cluts;
            g_app.refresh_selection();
            InvalidateRect(hwnd, 0, FALSE);
            return 0;
        }
        if (g_image.max_pal_rows > 1) {
            if (wp == VK_UP) {
                if (g_image.pal_row > 0) {
                    g_image.pal_row--;
                    g_image.rerender();
                    InvalidateRect(hwnd, 0, FALSE);
                }
                return 0;
            }
            if (wp == VK_DOWN) {
                if (g_image.pal_row < g_image.max_pal_rows - 1) {
                    g_image.pal_row++;
                    g_image.rerender();
                    InvalidateRect(hwnd, 0, FALSE);
                }
                return 0;
            }
        }
    } break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/*═══════════════════════════════════════════════════════════════════
 *  Palette Panel
 *═══════════════════════════════════════════════════════════════════*/
void PalettePanel::set_palette(const u8* data, size_t sz) {
    num_rows = (int)(sz / 512);
    if (num_rows < 1) num_rows = 1;
    if (num_rows > 16) num_rows = 16;
    parse_palette(data, sz, colors, num_rows);
    selected_row = 0;
    scroll_y = 0;
    if (hwnd) {
        RECT rc; GetClientRect(hwnd, &rc);
        update_scroll(rc.bottom - rc.top);
        InvalidateRect(hwnd, 0, TRUE);
    }
}

void PalettePanel::update_scroll(int panel_h) {
    /* Compute cell size matching paint() logic */
    RECT rc; GetClientRect(hwnd, &rc);
    int avail_w = (rc.right - rc.left) - 16;
    int cell = avail_w / 16;
    if (cell < 6) cell = 6; if (cell > 28) cell = 28;
    int row_h = 18 + 16 * cell + 6;
    content_h = num_rows * row_h + 8;

    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = content_h;
    si.nPage = panel_h;
    si.nPos = scroll_y;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

void PalettePanel::paint(HDC hdc, RECT& rc) {
    int pw = rc.right - rc.left;
    int ph = rc.bottom - rc.top;

    HBRUSH bgbr = CreateSolidBrush(T.isDark ? RGB(25, 25, 32) : RGB(242, 242, 245));
    FillRect(hdc, &rc, bgbr); DeleteObject(bgbr);

    if (num_rows == 0) {
        SetTextColor(hdc, T.textDim);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextA(hdc, "No palette data", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    /* Layout: 16 columns of color swatches, sized to fit panel width */
    int margin = 8;
    int avail_w = pw - margin * 2;
    int cell = avail_w / 16;
    if (cell < 6) cell = 6;
    if (cell > 28) cell = 28;
    int grid_w = cell * 16;
    int x_off = margin + (avail_w - grid_w) / 2; /* center grid */

    int label_h = 18;
    int row_h = label_h + 16 * cell + 6;

    SetBkMode(hdc, TRANSPARENT);

    HFONT hf = CreateFontA(13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT old_font = (HFONT)SelectObject(hdc, hf ? hf : (HFONT)GetStockObject(DEFAULT_GUI_FONT));

    for (int row = 0; row < num_rows && row < 16; row++) {
        int ry = 4 + row * row_h - scroll_y;

        /* Skip if entirely off-screen */
        if (ry + row_h < 0) continue;
        if (ry > ph) break;

        /* Selection highlight background */
        if (row == selected_row) {
            RECT sel_rc = { x_off - 4, ry, x_off + grid_w + 4, ry + row_h - 4 };
            HBRUSH sel_br = CreateSolidBrush(T.isDark ? RGB(40, 45, 60) : RGB(215, 225, 240));
            FillRect(hdc, &sel_rc, sel_br); DeleteObject(sel_br);

            /* Thin accent border on selected row */
            HPEN pen = CreatePen(PS_SOLID, 1, T.accent);
            HPEN old_pen = (HPEN)SelectObject(hdc, pen);
            HBRUSH null_br = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH old_br = (HBRUSH)SelectObject(hdc, null_br);
            Rectangle(hdc, sel_rc.left, sel_rc.top, sel_rc.right, sel_rc.bottom);
            SelectObject(hdc, old_pen); SelectObject(hdc, old_br);
            DeleteObject(pen);
        }

        /* Row label */
        char label[32];
        _snprintf(label, 31, "Row %d  (%d colors)", row, 256);
        SetTextColor(hdc, (row == selected_row) ? T.text : T.textDim);
        TextOutA(hdc, x_off, ry + 1, label, (int)strlen(label));

        /* Color grid: 16×16 */
        int base = row * 256;
        int gy = ry + label_h;
        for (int ci = 0; ci < 256; ci++) {
            int cx = x_off + (ci % 16) * cell;
            int cy = gy + (ci / 16) * cell;
            if (cy + cell < 0 || cy > ph) continue;
            RGBA8 c = colors[base + ci];
            HBRUSH cbr = CreateSolidBrush(RGB(c.r, c.g, c.b));
            RECT cr2 = { cx, cy, cx + cell - 1, cy + cell - 1 };
            FillRect(hdc, &cr2, cbr); DeleteObject(cbr);
        }
    }

    /* Info footer */
    {
        char info[64];
        _snprintf(info, 63, "%d palette row%s, 256 colors each",
            num_rows, num_rows == 1 ? "" : "s");
        SetTextColor(hdc, T.textDim);
        RECT frc = { margin, ph - 20, pw - margin, ph };
        DrawTextA(hdc, info, -1, &frc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, old_font);
    if (hf) DeleteObject(hf);
}

LRESULT CALLBACK PalettePanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc); g_palette.paint(hdc, rc);
        EndPaint(hwnd, &ps); } return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        g_palette.update_scroll(rc.bottom - rc.top);
    } return 0;
    case WM_VSCROLL: {
        SCROLLINFO si; si.cbSize = sizeof(si); si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP:    pos -= 20; break;
        case SB_LINEDOWN:  pos += 20; break;
        case SB_PAGEUP:    pos -= (int)si.nPage; break;
        case SB_PAGEDOWN:  pos += (int)si.nPage; break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        }
        if (pos < 0) pos = 0;
        int max_scroll = g_palette.content_h - (int)si.nPage;
        if (max_scroll < 0) max_scroll = 0;
        if (pos > max_scroll) pos = max_scroll;
        g_palette.scroll_y = pos;
        si.fMask = SIF_POS; si.nPos = pos;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        InvalidateRect(hwnd, 0, TRUE);
    } return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        g_palette.scroll_y -= delta * 40;
        if (g_palette.scroll_y < 0) g_palette.scroll_y = 0;
        RECT rc; GetClientRect(hwnd, &rc);
        int max_scroll = g_palette.content_h - (rc.bottom - rc.top);
        if (max_scroll < 0) max_scroll = 0;
        if (g_palette.scroll_y > max_scroll) g_palette.scroll_y = max_scroll;
        SCROLLINFO si; si.cbSize = sizeof(si); si.fMask = SIF_POS;
        si.nPos = g_palette.scroll_y;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        InvalidateRect(hwnd, 0, TRUE);
    } return 0;
    case WM_LBUTTONDOWN: {
        /* Click to select a palette row */
        int my = (int)(short)HIWORD(lp) + g_palette.scroll_y;
        int cell = 14; /* must match paint */
        RECT rc; GetClientRect(hwnd, &rc);
        int avail_w = (rc.right - rc.left) - 16;
        int c = avail_w / 16;
        if (c < 6) c = 6; if (c > 28) c = 28;
        cell = c;
        int row_h = 18 + 16 * cell + 6;
        int clicked_row = (my - 4) / row_h;
        if (clicked_row >= 0 && clicked_row < g_palette.num_rows) {
            g_palette.selected_row = clicked_row;
            /* Update the image panel's palette row if viewing a texture */
            g_image.pal_row = clicked_row;
            if (g_image.cur_tex && g_image.cur_tex->data &&
                g_image.cur_pal && g_image.cur_pal->data) {
                if (g_image.cur_is_linear)
                    g_image.render_linear(*g_image.cur_tex, g_image.cur_pal);
                else
                    g_image.render_entry(*g_image.cur_tex, g_image.cur_pal);
            }
            InvalidateRect(hwnd, 0, TRUE);
        }
    } return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/*═══════════════════════════════════════════════════════════════════
 *  Audio Panel
 *═══════════════════════════════════════════════════════════════════*/
void AudioPanel::free_samples() {
    for (int i = 0; i < num_samples; i++) free(samples[i].pcm);
    num_samples = 0;
    pcm_data = 0; pcm_count = 0;
}

/* An SNDB is loaded into SPU RAM at the address in its x/y fields, so a
   Gian tone's VAG starts at (tone SPU address - that address) in the SNDB. */
static u32 sndb_spu_base(const DatEntry& sndb) {
    return ((u32)sndb.y << 16) | sndb.x;
}

/* Archives can hold several SNDH/SNDB pairs: the SNDB's header is the
   SNDH whose tones point into it. */
static bool find_sndb_gian(const DatEntry& sndb, GianHeader& out) {
    if (!g_app.archive) return false;
    u32 base = sndb_spu_base(sndb);
    int best = 0;
    for (int i = 0; i < g_app.archive->count; i++) {
        const DatEntry& e = g_app.archive->entries[i];
        if (e.type != DAT_SNDH || !e.data) continue;
        GianHeader gh;
        if (!parse_gian_header(e.data, e.size, gh)) continue;
        int hits = 0;
        for (int t = 0; t < gh.tone_count; t++)
            if (gh.tones[t].spu_addr - base < sndb.size) hits++;
        if (hits > best) { best = hits; out = gh; }
    }
    return best > 0;
}

/* First tone of gh that plays the sample starting at off in the SNDB. */
static const GianTone* find_sample_tone(const GianHeader& gh, u32 base, int off) {
    for (int t = 0; t < gh.tone_count; t++)
        if (gh.tones[t].spu_addr - base == (u32)off) return &gh.tones[t];
    return 0;
}

void AudioPanel::decode_sndb(const DatEntry& entry) {
    stop(); free_samples();
    mode = AMODE_SNDB;
    if (entry.type != DAT_SNDB || !entry.data) return;

    const u8* d = entry.data;
    int sz = (int)entry.size;
    int sample_start = 0;
    int i = 0;

    while (i < sz - 15 && num_samples < MAX_AUDIO_SAMPLES) {
        u8 flags = d[i + 1];
        if (flags & 1) {
            int sample_end = i + 16;
            if (sample_end + 16 <= sz && d[sample_end + 1] == 7) sample_end += 16;
            if (sample_end - sample_start >= 16) {
                int blk_bytes = sample_end - sample_start;
                int max_pcm = (blk_bytes / 16) * 28 + 64;
                s16* pcm = (s16*)malloc(max_pcm * sizeof(s16));
                int n = decode_vag(d, sample_start, sample_end, pcm, max_pcm);
                if (n > 0) {
                    AudioSample& s = samples[num_samples++];
                    s.pcm = pcm; s.count = n;
                    s.start_off = sample_start; s.end_off = sample_end;
                } else free(pcm);
            }
            sample_start = sample_end; i = sample_end;
        } else i += 16;
    }

    /* VAGs have no rate of their own: the SPU plays each at the pitch of
       the tone that keys it. A sound effect's tone has a single key, so
       its rate is exact. An instrument spans a range of keys and is shown
       at middle C, and samples no tone uses get the game's most common
       rate; both are marked as guesses. */
    GianHeader gh;
    bool have_gh = find_sndb_gian(entry, gh);
    u32 base = sndb_spu_base(entry);
    for (int k = 0; k < num_samples; k++) {
        AudioSample& s = samples[k];
        const GianTone* tone = have_gh ? find_sample_tone(gh, base, s.start_off) : 0;
        s.rate = tone ? gian_tone_sample_rate(*tone) : 11025;
        s.rate_known = tone && tone->key_lo == tone->key_hi;
    }

    cur_sample = 0; scroll_off = 0;
    if (num_samples > 0) select_sample(0);
    if (hwnd) InvalidateRect(hwnd, 0, FALSE);
}

void AudioPanel::decode_sndh(const DatEntry& entry) {
    stop(); free_samples();
    mode = AMODE_SNDH;
    memset(&gian, 0, sizeof(gian));
    if (entry.data && entry.size >= 12)
        parse_gian_header(entry.data, entry.size, gian);
    scroll_off = 0;
    if (hwnd) InvalidateRect(hwnd, 0, FALSE);
}

void AudioPanel::decode_snde(const DatEntry& entry) {
    stop(); free_samples();
    mode = AMODE_SNDE;
    memset(&seq_hdr, 0, sizeof(seq_hdr));
    free(seq_notes); seq_notes = 0;
    seq_note_count = 0; seq_duration = 0; seq_ch_mask = 0;
    free(seq_pcm); seq_pcm = 0; seq_frames = 0; seq_rendered = false;
    active_snde = &entry;
    sample_rate = 22050; /* synth output rate */

    if (entry.data && entry.size >= 15) {
        parse_seq_header(entry.data, entry.size, seq_hdr);
        if (seq_hdr.valid) {
            seq_notes = (SeqNote*)malloc(sizeof(SeqNote) * 8192);
            seq_note_count = parse_seq_notes(entry.data, entry.size, 15,
                seq_hdr.initial_tempo, seq_hdr.resolution,
                seq_notes, 8192, &seq_duration, &seq_ch_mask);
        }
    }
    scroll_off = 0;
    if (hwnd) InvalidateRect(hwnd, 0, FALSE);
}

void AudioPanel::render_seq() {
    /* Find SNDH, SNDB, and SNDE in the archive to synthesize */
    if (!g_app.archive || seq_rendered) return;

    const DatEntry* sndb_e = 0;
    const DatEntry* snde_e = active_snde;

    for (int i = 0; i < g_app.archive->count; i++) {
        const DatEntry& e = g_app.archive->entries[i];
        if (e.type == DAT_SNDB && e.data) { sndb_e = &e; }
    }
    if (!sndb_e || !snde_e) return;

    /* Gian header whose tones play this SNDB */
    GianHeader gh;
    if (!find_sndb_gian(*sndb_e, gh)) return;

    /* Decode each tone's VAG into the synth's slot for it (vag_bank) */
    const int MAX_VAG = 128;
    DecodedSample vag[MAX_VAG];
    memset(vag, 0, sizeof(vag));
    int nvag = 0;

    const u8* sd = sndb_e->data;
    size_t sz = sndb_e->size;
    u32 base = sndb_spu_base(*sndb_e);
    for (int t = 0; t < gh.tone_count; t++) {
        int vi = gh.tones[t].vag_bank;
        u32 off = gh.tones[t].spu_addr - base;
        if (vi >= MAX_VAG || vag[vi].pcm || off >= sz) continue;
        size_t end = off;
        while (end + 16 <= sz) { end += 16; if (sd[end - 15] & 1) break; }
        int maxp = (int)((end - off) / 16) * 28 + 64;
        vag[vi].pcm = (s16*)malloc(maxp * sizeof(s16));
        vag[vi].count = decode_vag(sd, off, end, vag[vi].pcm, maxp);
        if (vag[vi].count <= 0) { free(vag[vi].pcm); vag[vi].pcm = 0; vag[vi].count = 0; }
        if (vi >= nvag) nvag = vi + 1;
    }

    /* Render */
    int max_frames = sample_rate * 300; /* 5 min max */
    if (seq_duration > 0 && seq_duration < 300.0f)
        max_frames = (int)(seq_duration + 3.0f) * sample_rate;

    free(seq_pcm);
    seq_pcm = (s16*)malloc(max_frames * 2 * sizeof(s16));
    if (seq_pcm) {
        seq_frames = render_seq_to_stereo(snde_e->data, snde_e->size,
            gh, vag, nvag, seq_pcm, max_frames, sample_rate, 300.0f);
    }

    /* Free decoded VAG samples (synthesizer already consumed them) */
    for (int i = 0; i < nvag; i++) free(vag[i].pcm);

    seq_rendered = true;

    /* Set up for playback */
    if (seq_frames > 0) {
        pcm_data = seq_pcm;
        pcm_count = seq_frames;
        channels = 2;
    }
    if (hwnd) InvalidateRect(hwnd, 0, FALSE);
}

void AudioPanel::select_sample(int idx) {
    if (idx < 0 || idx >= num_samples) return;
    stop();
    cur_sample = idx;
    pcm_data = samples[idx].pcm;
    pcm_count = samples[idx].count;
    sample_rate = samples[idx].rate;
    channels = 1;
    if (hwnd) InvalidateRect(hwnd, 0, FALSE);
}

void AudioPanel::play() {
    if (!pcm_data || pcm_count <= 0 || playing) return;
    WAVEFORMATEX wfx; memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)channels;
    wfx.nSamplesPerSec = sample_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(channels * 2);
    wfx.nAvgBytesPerSec = sample_rate * channels * 2;
    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, 0) != MMSYSERR_NOERROR) return;
    static WAVEHDR whdr; memset(&whdr, 0, sizeof(whdr));
    whdr.lpData = (LPSTR)pcm_data;
    whdr.dwBufferLength = pcm_count * channels * 2;
    waveOutPrepareHeader(hWaveOut, &whdr, sizeof(whdr));
    waveOutWrite(hWaveOut, &whdr, sizeof(whdr));
    playing = true;
    play_time = 0.0f;
    /* Start a ~30fps timer to poll playback position */
    if (hwnd && !audio_timer)
        audio_timer = SetTimer(hwnd, 8001, 33, 0);
}

void AudioPanel::stop() {
    if (hWaveOut) { waveOutReset(hWaveOut); waveOutClose(hWaveOut); hWaveOut = 0; }
    playing = false;
    play_time = 0.0f;
    if (audio_timer && hwnd) { KillTimer(hwnd, audio_timer); audio_timer = 0; }
    if (hwnd) InvalidateRect(hwnd, 0, FALSE);
}

bool AudioPanel::load_wav(const u8* data, size_t size, const char* filename) {
    stop();
    free_samples();
    mode = AMODE_NONE;
    wav_total_samples = 0;
    wav_name[0] = 0;

    if (size < 44) return false;
    /* Validate RIFF/WAVE header */
    if (data[0]!='R' || data[1]!='I' || data[2]!='F' || data[3]!='F') return false;
    if (data[8]!='W' || data[9]!='A' || data[10]!='V' || data[11]!='E') return false;

    /* Find fmt chunk */
    size_t pos = 12;
    int fmt_channels = 0, fmt_rate = 0, fmt_bps = 0, fmt_tag = 0;
    const u8* pcm_src = 0;
    size_t pcm_bytes = 0;

    while (pos + 8 <= size) {
        u32 ckid = *(u32*)(data + pos);
        u32 cksz = *(u32*)(data + pos + 4);
        pos += 8;
        if (pos + cksz > size) break;

        if (ckid == 0x20746D66) { /* 'fmt ' */
            if (cksz >= 16) {
                fmt_tag      = *(u16*)(data + pos);
                fmt_channels = *(u16*)(data + pos + 2);
                fmt_rate     = *(u32*)(data + pos + 4);
                fmt_bps      = *(u16*)(data + pos + 14);
            }
        } else if (ckid == 0x61746164) { /* 'data' */
            pcm_src = data + pos;
            pcm_bytes = cksz;
        }
        pos += (cksz + 1) & ~1; /* chunks are word-aligned */
    }

    if (fmt_tag != 1 || fmt_channels < 1 || fmt_rate < 1 || fmt_bps < 8)
        return false;
    if (!pcm_src || pcm_bytes == 0)
        return false;

    /* Convert to s16 PCM */
    int frame_size = fmt_channels * (fmt_bps / 8);
    int n_frames = (int)(pcm_bytes / frame_size);
    if (n_frames < 1) return false;

    int total_samples = n_frames * fmt_channels;
    s16* out = (s16*)malloc(total_samples * sizeof(s16));
    if (!out) return false;

    if (fmt_bps == 16) {
        memcpy(out, pcm_src, n_frames * fmt_channels * 2);
    } else if (fmt_bps == 8) {
        /* 8-bit unsigned → 16-bit signed */
        for (int i = 0; i < total_samples; i++)
            out[i] = (s16)((int)pcm_src[i] - 128) * 256;
    } else if (fmt_bps == 24) {
        for (int i = 0; i < total_samples; i++) {
            int s = (int)pcm_src[i*3+2] << 16 | pcm_src[i*3+1] << 8 | pcm_src[i*3];
            if (s & 0x800000) s |= ~0xFFFFFF;
            out[i] = (s16)(s >> 8);
        }
    } else {
        free(out);
        return false;
    }

    pcm_data = out;
    pcm_count = n_frames;
    channels = fmt_channels;
    sample_rate = fmt_rate;
    wav_total_samples = n_frames;
    mode = AMODE_WAV;

    if (filename) {
        const char* bn = strrchr(filename, '\\');
        if (!bn) bn = strrchr(filename, '/');
        bn = bn ? bn + 1 : filename;
        _snprintf(wav_name, MAX_PATH - 1, "%s", bn);
    }

    return true;
}

/* Forward declarations for audio drawing helpers (defined below) */
static COLORREF audio_accent();
static COLORREF audio_dim();
static COLORREF audio_wave();
static COLORREF audio_wave_dim();
static COLORREF audio_panel_bg();
static HFONT audio_font(int size, bool bold = false, int zoom = 100);

void AudioPanel::paint_wav(HDC buf, int cw, int ch) {
    HFONT f12 = audio_font(12);
    HFONT f14 = audio_font(14);
    HGDIOBJ old_font = GetCurrentObject(buf, OBJ_FONT);

    /* Header bar */
    int hh = 28;
    RECT hdr_rc = { 0, 0, cw, hh };
    HBRUSH hdr_bg = CreateSolidBrush(T.isDark ? RGB(22, 28, 36) : RGB(230, 232, 238));
    FillRect(buf, &hdr_rc, hdr_bg); DeleteObject(hdr_bg);

    SelectObject(buf, f14);
    SetTextColor(buf, audio_accent());
    char title[512];
    float dur = (float)wav_total_samples / (float)(sample_rate > 0 ? sample_rate : 1);
    _snprintf(title, 511, "  WAV: %s  |  %d Hz, %d-ch, %d samples (%.1fs)  |  Space = Play/Stop",
              wav_name, sample_rate, channels, wav_total_samples, dur);
    TextOutA(buf, 4, 5, title, (int)strlen(title));

    /* Playback progress bar */
    int bar_y = hh + 4;
    int bar_h = 18;
    RECT bar_rc = { 8, bar_y, cw - 8, bar_y + bar_h };
    HBRUSH bar_bg = CreateSolidBrush(T.isDark ? RGB(25, 30, 40) : RGB(215, 218, 225));
    FillRect(buf, &bar_rc, bar_bg); DeleteObject(bar_bg);

    if (playing && wav_total_samples > 0) {
        float progress = play_time * sample_rate / (float)wav_total_samples;
        if (progress > 1.0f) progress = 1.0f;
        int pw = (int)((cw - 16) * progress);
        RECT prog_rc = { 8, bar_y, 8 + pw, bar_y + bar_h };
        HBRUSH prog_fill = CreateSolidBrush(audio_accent());
        FillRect(buf, &prog_rc, prog_fill); DeleteObject(prog_fill);
    }

    SelectObject(buf, f12);
    SetTextColor(buf, T.isDark ? RGB(160, 170, 180) : RGB(80, 80, 80));
    char time_str[64];
    float cur_time = playing ? play_time : 0;
    _snprintf(time_str, 63, " %.1f / %.1fs", cur_time, dur);
    TextOutA(buf, 10, bar_y + 1, time_str, (int)strlen(time_str));

    /* Waveform display */
    int wave_top = bar_y + bar_h + 8;
    int wave_h = ch - wave_top - 8;
    if (wave_h < 40) wave_h = 40;
    int mid_y = wave_top + wave_h / 2;

    /* Center line */
    HPEN dim_pen = CreatePen(PS_SOLID, 1, audio_wave_dim());
    HPEN old_pen = (HPEN)SelectObject(buf, dim_pen);
    MoveToEx(buf, 0, mid_y, NULL); LineTo(buf, cw, mid_y);

    /* Waveform — draw peak envelope */
    if (pcm_data && pcm_count > 0) {
        HPEN wave_pen = CreatePen(PS_SOLID, 1, audio_wave());
        SelectObject(buf, wave_pen);

        int draw_w = cw - 16;
        int x_off = 8;
        for (int px = 0; px < draw_w; px++) {
            /* Map pixel to sample range */
            int s0 = (int)((s64)px * pcm_count / draw_w);
            int s1 = (int)((s64)(px + 1) * pcm_count / draw_w);
            if (s1 <= s0) s1 = s0 + 1;
            if (s1 > pcm_count) s1 = pcm_count;

            /* Find min/max in range (use first channel only for stereo) */
            s16 vmin = 0, vmax = 0;
            for (int si = s0; si < s1; si++) {
                s16 v = pcm_data[si * channels];
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }

            int y_top = mid_y - (int)((float)vmax / 32768.0f * (wave_h / 2));
            int y_bot = mid_y - (int)((float)vmin / 32768.0f * (wave_h / 2));
            if (y_top == y_bot) y_bot++;
            MoveToEx(buf, x_off + px, y_top, NULL);
            LineTo(buf, x_off + px, y_bot);
        }

        /* Draw playback cursor */
        if (playing && wav_total_samples > 0) {
            float progress = play_time * sample_rate / (float)wav_total_samples;
            if (progress > 1.0f) progress = 1.0f;
            int cx = x_off + (int)(draw_w * progress);
            HPEN cur_pen = CreatePen(PS_SOLID, 2, RGB(255, 80, 50));
            SelectObject(buf, cur_pen);
            MoveToEx(buf, cx, wave_top, NULL);
            LineTo(buf, cx, wave_top + wave_h);
            SelectObject(buf, wave_pen);
            DeleteObject(cur_pen);
        }

        SelectObject(buf, dim_pen);
        DeleteObject(wave_pen);
    }

    SelectObject(buf, old_pen);
    DeleteObject(dim_pen);
    SelectObject(buf, old_font);  /* a font still selected can't be deleted */
    DeleteObject(f14);
    DeleteObject(f12);
}

/* ── Shared helpers for custom drawing ── */
static COLORREF audio_accent()   { return T.isDark ? RGB(0,180,90)    : RGB(0,120,60); }
static COLORREF audio_dim()      { return T.isDark ? RGB(80,90,100)   : RGB(140,140,150); }
static COLORREF audio_wave()     { return T.isDark ? RGB(50,200,100)  : RGB(0,150,60); }
static COLORREF audio_wave_dim() { return T.isDark ? RGB(35,80,55)    : RGB(140,200,160); }
static COLORREF audio_panel_bg() { return T.isDark ? RGB(18,22,30)    : RGB(245,245,248); }
static COLORREF audio_row_sel()  { return T.isDark ? RGB(28,42,58)    : RGB(215,228,245); }
static COLORREF audio_row_bg()   { return T.isDark ? RGB(12,14,22)    : RGB(235,235,240); }
static COLORREF audio_border()   { return T.isDark ? RGB(50,55,65)    : RGB(190,190,200); }

static HFONT audio_font(int size, bool bold, int zoom) {
    int scaled = size * zoom / 100;
    if (scaled < 8) scaled = 8;
    return CreateFontA(-scaled, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
}

/* Scale a pixel dimension by zoom */
static int zs(int val, int zoom) { return val * zoom / 100; }

static void draw_wave_minmax(HDC buf, s16* pcm, int count,
                             int x, int y, int w, int h, COLORREF col)
{
    if (count <= 0 || w < 2 || h < 4) return;
    int mid = y + h / 2;
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HPEN old = (HPEN)SelectObject(buf, pen);
    for (int px = 0; px < w; px++) {
        int i0 = (int)((s64)px * count / w);
        int i1 = (int)((s64)(px + 1) * count / w);
        if (i1 > count) i1 = count;
        if (i0 >= count) i0 = count - 1;
        s16 mn = 32767, mx = -32768;
        for (int j = i0; j < i1; j++) {
            if (pcm[j] < mn) mn = pcm[j];
            if (pcm[j] > mx) mx = pcm[j];
        }
        int y0 = mid - (int)(mx * (f32)(h/2 - 1) / 32768.0f);
        int y1 = mid - (int)(mn * (f32)(h/2 - 1) / 32768.0f);
        if (y0 == y1) y1++;
        MoveToEx(buf, x + px, y0, 0);
        LineTo(buf, x + px, y1);
    }
    SelectObject(buf, old); DeleteObject(pen);
}

/* ── SNDB: sample list with waveforms ── */
void AudioPanel::paint_sndb(HDC buf, int cw, int ch)
{
    int z = zoom;
    HFONT fTitle = audio_font(13, true, z);
    HFONT fLabel = audio_font(11, false, z);
    HFONT fSmall = audio_font(9, false, z);
    HGDIOBJ old_font = GetCurrentObject(buf, OBJ_FONT);
    int hdr_h = zs(26, z);

    if (num_samples <= 0) {
        SelectObject(buf, fTitle);
        SetTextColor(buf, audio_dim());
        RECT r = {0, 0, cw, ch};
        DrawTextA(buf, "No audio samples found", -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        goto sndb_cleanup;
    }

    {
        /* Title bar */
        RECT hdr_rc = {0, 0, cw, hdr_h};
        HBRUSH hdr_bg = CreateSolidBrush(T.isDark ? RGB(22,28,38) : RGB(235,238,245));
        FillRect(buf, &hdr_rc, hdr_bg); DeleteObject(hdr_bg);

        SelectObject(buf, fTitle); SetTextColor(buf, audio_accent());
        char title[128];
        _snprintf(title, 127, "  VAG Sound Bank  -  %d sample%s",
            num_samples, num_samples > 1 ? "s" : "");
        TextOutA(buf, zs(6,z), zs(5,z), title, (int)strlen(title));

        SetTextColor(buf, audio_dim()); SelectObject(buf, fSmall);
        const char* hint = "[Click] select+play   [Up/Dn] navigate   [Space] play/stop";
        int hw = 0; { SIZE sz; GetTextExtentPoint32A(buf, hint, (int)strlen(hint), &sz); hw = sz.cx; }
        TextOutA(buf, cw - hw - zs(10,z), zs(8,z), hint, (int)strlen(hint));

        /* Separator */
        HPEN sep = CreatePen(PS_SOLID, 1, audio_border());
        HGDIOBJ old_sep = SelectObject(buf, sep);
        MoveToEx(buf, 0, hdr_h, 0); LineTo(buf, cw, hdr_h);
        SelectObject(buf, old_sep);
        DeleteObject(sep);

        /* Sample rows */
        int row_h = (ch - hdr_h - 2) / num_samples;
        if (row_h > zs(56, z)) row_h = zs(56, z);
        if (row_h < zs(22, z)) row_h = zs(22, z);
        int y_off = hdr_h + 2;
        int label_w = zs(110, z);
        int wave_pad = zs(6, z);

        for (int si = 0; si < num_samples; si++) {
            if (y_off >= ch) break;
            AudioSample& s = samples[si];
            bool sel = (si == cur_sample);
            f32 dur = (f32)s.count / s.rate;
            int rh = (y_off + row_h > ch) ? (ch - y_off) : row_h;

            /* Row background */
            RECT row_rc = {0, y_off, cw, y_off + rh};
            HBRUSH rbg = CreateSolidBrush(sel ? audio_row_sel() : ((si & 1) ? audio_row_bg() : audio_panel_bg()));
            FillRect(buf, &row_rc, rbg); DeleteObject(rbg);

            /* Selection indicator */
            if (sel) {
                HBRUSH ind = CreateSolidBrush(audio_accent());
                RECT ind_rc = {0, y_off, 3, y_off + rh};
                FillRect(buf, &ind_rc, ind); DeleteObject(ind);
            }

            /* Label area */
            SelectObject(buf, fLabel);
            SetTextColor(buf, sel ? (T.isDark ? RGB(220,235,255) : RGB(0,30,80)) : T.text);
            char lbl[32]; _snprintf(lbl, 31, "#%d", si);
            TextOutA(buf, zs(10,z), y_off + zs(2,z), lbl, (int)strlen(lbl));

            SelectObject(buf, fSmall);
            SetTextColor(buf, audio_dim());
            char info[48]; _snprintf(info, 47, "%.2fs  %d Hz%s", dur, s.rate, s.rate_known ? "" : "?");
            TextOutA(buf, zs(10,z), y_off + rh - zs(14,z), info, (int)strlen(info));

            /* Play indicator */
            if (sel && playing) {
                SetTextColor(buf, audio_accent());
                SelectObject(buf, fLabel);
                TextOutA(buf, label_w - zs(24,z), y_off + (rh/2) - zs(6,z), ">", 1);
            }

            /* Waveform */
            int wx = label_w;
            int ww = cw - wx - wave_pad;
            int wy = y_off + 3;
            int wh = rh - 6;
            if (ww > 10 && wh > 4) {
                RECT wr = {wx, wy, wx + ww, wy + wh};
                HBRUSH wbg = CreateSolidBrush(T.isDark ? RGB(8,10,18) : RGB(228,228,232));
                FillRect(buf, &wr, wbg); DeleteObject(wbg);

                /* Center line */
                HPEN cline = CreatePen(PS_DOT, 1, T.isDark ? RGB(35,40,50) : RGB(200,200,210));
                HGDIOBJ old_cline = SelectObject(buf, cline);
                MoveToEx(buf, wx, wy + wh/2, 0); LineTo(buf, wx + ww, wy + wh/2);
                SelectObject(buf, old_cline);
                DeleteObject(cline);

                draw_wave_minmax(buf, s.pcm, s.count, wx, wy, ww, wh,
                    sel ? audio_wave() : audio_wave_dim());
            }

            /* Row separator */
            HPEN rsep = CreatePen(PS_SOLID, 1, T.isDark ? RGB(30,35,45) : RGB(220,220,225));
            HGDIOBJ old_rsep = SelectObject(buf, rsep);
            MoveToEx(buf, 0, y_off + rh - 1, 0); LineTo(buf, cw, y_off + rh - 1);
            SelectObject(buf, old_rsep);
            DeleteObject(rsep);

            y_off += rh;
        }
    }

sndb_cleanup:
    SelectObject(buf, old_font);  /* a font still selected can't be deleted */
    DeleteObject(fTitle); DeleteObject(fLabel); DeleteObject(fSmall);
}

/* ── SNDH: Gian header tone table ── */
void AudioPanel::paint_sndh(HDC buf, int cw, int ch)
{
    int z = zoom;
    HFONT fTitle = audio_font(13, true, z);
    HFONT fLabel = audio_font(11, false, z);
    HFONT fSmall = audio_font(9, false, z);
    HGDIOBJ old_font = GetCurrentObject(buf, OBJ_FONT);
    int hdr_h = zs(26, z);

    if (!gian.valid) {
        SelectObject(buf, fTitle); SetTextColor(buf, audio_dim());
        RECT r = {0, 0, cw, ch};
        DrawTextA(buf, "Not a valid Gian header", -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        goto sndh_cleanup;
    }

    {
        /* Header bar */
        RECT hdr_rc = {0, 0, cw, hdr_h};
        HBRUSH hdr_bg = CreateSolidBrush(T.isDark ? RGB(22,28,38) : RGB(235,238,245));
        FillRect(buf, &hdr_rc, hdr_bg); DeleteObject(hdr_bg);

        SelectObject(buf, fTitle); SetTextColor(buf, audio_accent());
        char title[128];
        _snprintf(title, 127, "  Gian Sound Header  -  %d prog  %d tones  %d VAGs",
            gian.num_programs, gian.num_tones, gian.num_vag);
        TextOutA(buf, zs(6,z), zs(5,z), title, (int)strlen(title));

        HPEN sep = CreatePen(PS_SOLID, 1, audio_border());
        HGDIOBJ old_sep = SelectObject(buf, sep);
        MoveToEx(buf, 0, hdr_h, 0); LineTo(buf, cw, hdr_h);
        SelectObject(buf, old_sep);
        DeleteObject(sep);

        int y = hdr_h + zs(4, z);
        int line_h = zs(16, z);
        int tone_h = zs(13, z);

        /* Programs */
        SelectObject(buf, fLabel);
        SetTextColor(buf, T.isDark ? RGB(130,190,255) : RGB(30,80,160));
        for (int p = 0; p < gian.num_programs && p < 8; p++) {
            char pstr[8] = "C";
            if (gian.programs[p].pan < 64) _snprintf(pstr, 7, "L%d", gian.programs[p].pan);
            else if (gian.programs[p].pan > 64) _snprintf(pstr, 7, "R%d", gian.programs[p].pan - 64);
            char line[80];
            _snprintf(line, 79, "Program %d:  Vol=%d  Pan=%s", p, gian.programs[p].volume, pstr);
            TextOutA(buf, zs(10,z), y, line, (int)strlen(line));
            y += line_h;
        }
        y += zs(4, z);

        /* Tone table header */
        SetTextColor(buf, T.isDark ? RGB(255,200,50) : RGB(160,120,0));
        SelectObject(buf, fTitle);
        TextOutA(buf, zs(10,z), y, "Tone Map", 8);
        y += zs(18, z);

        SelectObject(buf, fSmall);
        SetTextColor(buf, audio_dim());
        const char* hdr_txt = " P  T   Vol  Pan  Note   Key     ADSR1  ADSR2    SPU";
        TextOutA(buf, zs(10,z), y, hdr_txt, (int)strlen(hdr_txt));
        y += tone_h;

        HPEN hsep = CreatePen(PS_SOLID, 1, audio_border());
        HGDIOBJ old_hsep = SelectObject(buf, hsep);
        MoveToEx(buf, zs(10,z), y, 0); LineTo(buf, cw - zs(10,z), y);
        SelectObject(buf, old_hsep);
        DeleteObject(hsep);
        y += 3;

        static const char* note_names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

        for (int ti = 0; ti < gian.tone_count; ti++) {
            if (y + tone_h > ch) {
                char more[32]; _snprintf(more, 31, "... +%d more", gian.tone_count - ti);
                SetTextColor(buf, audio_dim());
                TextOutA(buf, zs(10,z), y, more, (int)strlen(more));
                break;
            }

            const GianTone& tn = gian.tones[ti];
            int g = tn.volume > 50 ? 180 : (int)(tn.volume * 3.5f);
            COLORREF tcol = T.isDark
                ? RGB(g, (g > 80 ? g : 80), 80)
                : RGB(0, g/2, 0);
            SetTextColor(buf, tcol);

            char note_str[8] = "---";
            if (tn.pitch > 0) _snprintf(note_str, 7, "%s%d", note_names[tn.pitch % 12], tn.pitch / 12 - 1);

            char pan_str[8] = "C";
            if (tn.pan < 0x40) _snprintf(pan_str, 7, "L%d", tn.pan);
            else if (tn.pan > 0x40) _snprintf(pan_str, 7, "R%d", tn.pan - 0x40);

            char line[128];
            _snprintf(line, 127, " %d %2d  %4d  %4s  %5s  %02X-%02X   %04X   %04X  %06X",
                tn.program, tn.tone_idx, tn.volume, pan_str, note_str,
                tn.key_lo, tn.key_hi, tn.adsr1, tn.adsr2, tn.spu_addr);
            TextOutA(buf, zs(10,z), y, line, (int)strlen(line));

            /* Volume bar */
            int bar_x = cw - zs(80,z);
            int bar_w = (int)(tn.volume / 127.0f * zs(56,z));
            int bar_h2 = zs(8, z);
            RECT bar_bg = {bar_x, y + 1, bar_x + zs(56,z), y + 1 + bar_h2};
            HBRUSH bbg = CreateSolidBrush(T.isDark ? RGB(15,15,30) : RGB(220,220,225));
            FillRect(buf, &bar_bg, bbg); DeleteObject(bbg);
            if (bar_w > 0) {
                RECT bar_fg = {bar_x, y + 1, bar_x + bar_w, y + 1 + bar_h2};
                HBRUSH bfg = CreateSolidBrush(tcol);
                FillRect(buf, &bar_fg, bfg); DeleteObject(bfg);
            }

            y += tone_h;
        }
    }

sndh_cleanup:
    SelectObject(buf, old_font);
    DeleteObject(fTitle); DeleteObject(fLabel); DeleteObject(fSmall);
}

/* -- SNDE: SEQ piano roll with playback -- */
void AudioPanel::paint_snde(HDC buf, int cw, int ch)
{
    int z = zoom;
    HFONT fTitle = audio_font(13, true, z);
    HFONT fSmall = audio_font(9, false, z);
    HFONT fTiny  = audio_font(8, false, z);
    HGDIOBJ old_font = GetCurrentObject(buf, OBJ_FONT);
    int hdr_h = zs(26, z);

    if (!seq_hdr.valid || seq_note_count <= 0) {
        SelectObject(buf, fTitle); SetTextColor(buf, audio_dim());
        RECT r = {0, 0, cw, ch};
        DrawTextA(buf, "No notes found in sequence", -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        goto snde_cleanup;
    }

    {
        /* Title bar */
        RECT hdr_rc = {0, 0, cw, hdr_h};
        HBRUSH hdr_bg = CreateSolidBrush(T.isDark ? RGB(22,28,38) : RGB(235,238,245));
        FillRect(buf, &hdr_rc, hdr_bg); DeleteObject(hdr_bg);

        SelectObject(buf, fTitle); SetTextColor(buf, audio_accent());

        int nch = 0; char ch_str[64] = "";
        for (int c = 0; c < 16; c++) {
            if (seq_ch_mask & (1 << c)) {
                char tmp[8]; _snprintf(tmp, 7, "%s%d", nch ? "," : "", c);
                strcat(ch_str, tmp); nch++;
            }
        }

        char title[160];
        _snprintf(title, 159, "  SEQ Music  -  %.0f BPM  |  %d notes  |  %.1fs  |  ch: %s",
            seq_hdr.bpm, seq_note_count, seq_duration, ch_str);
        TextOutA(buf, zs(6,z), zs(5,z), title, (int)strlen(title));

        /* Playback status on right side of header */
        SelectObject(buf, fSmall);
        if (seq_rendered && seq_frames > 0) {
            const char* hint = playing ? "[Space] Stop" : "[Space] Play";
            SetTextColor(buf, playing ? audio_accent() : audio_dim());
            int hw2 = 0; { SIZE sz2; GetTextExtentPoint32A(buf, hint, (int)strlen(hint), &sz2); hw2 = sz2.cx; }
            TextOutA(buf, cw - hw2 - zs(10,z), zs(8,z), hint, (int)strlen(hint));
        } else if (!seq_rendered) {
            const char* hint = "[Space] Render + Play";
            SetTextColor(buf, T.isDark ? RGB(200,180,80) : RGB(120,100,0));
            int hw2 = 0; { SIZE sz2; GetTextExtentPoint32A(buf, hint, (int)strlen(hint), &sz2); hw2 = sz2.cx; }
            TextOutA(buf, cw - hw2 - zs(10,z), zs(8,z), hint, (int)strlen(hint));
        }

        HPEN sep_pen = CreatePen(PS_SOLID, 1, audio_border());
        HGDIOBJ old_sep = SelectObject(buf, sep_pen);
        MoveToEx(buf, 0, hdr_h, 0); LineTo(buf, cw, hdr_h);
        SelectObject(buf, old_sep);
        DeleteObject(sep_pen);

        /* Piano roll area */
        int roll_top = hdr_h + 2;
        int roll_bot = ch - 4;
        int roll_h = roll_bot - roll_top;
        int roll_left = zs(36, z);
        int roll_right = cw - zs(6, z);
        int roll_w = roll_right - roll_left;

        if (roll_w < 20 || roll_h < 20) goto snde_cleanup;

        /* Find note range */
        int note_lo = 127, note_hi = 0;
        for (int i = 0; i < seq_note_count; i++) {
            if (seq_notes[i].note < note_lo) note_lo = seq_notes[i].note;
            if (seq_notes[i].note > note_hi) note_hi = seq_notes[i].note;
        }
        note_lo = (note_lo > 2) ? note_lo - 2 : 0;
        note_hi = (note_hi < 125) ? note_hi + 2 : 127;
        int note_range = note_hi - note_lo;
        if (note_range < 1) note_range = 1;

        /* Roll background */
        RECT roll_rc = {roll_left, roll_top, roll_right, roll_bot};
        HBRUSH rbg = CreateSolidBrush(T.isDark ? RGB(8,10,20) : RGB(230,232,238));
        FillRect(buf, &roll_rc, rbg); DeleteObject(rbg);

        /* Horizontal grid at octave boundaries */
        HPEN grid_pen = CreatePen(PS_DOT, 1, T.isDark ? RGB(25,28,40) : RGB(210,210,218));
        HGDIOBJ old_grid = SelectObject(buf, grid_pen);
        static const char* note_names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        SelectObject(buf, fTiny);
        int label_step = note_range > 48 ? 12 : (note_range > 24 ? 6 : 1);
        for (int n = note_lo; n <= note_hi; n++) {
            if (n % label_step != 0 && n != note_lo) continue;
            int ny = roll_bot - (int)((f32)(n - note_lo) / note_range * roll_h);
            if (n % 12 == 0) {
                MoveToEx(buf, roll_left, ny, 0); LineTo(buf, roll_right, ny);
            }
            char nl[8]; _snprintf(nl, 7, "%s%d", note_names[n % 12], n / 12 - 1);
            SetTextColor(buf, audio_dim());
            TextOutA(buf, 2, ny - 5, nl, (int)strlen(nl));
        }
        SelectObject(buf, old_grid);
        DeleteObject(grid_pen);

        /* Channel colors */
        static const COLORREF ch_cols[] = {
            RGB(0,200,100), RGB(200,100,0), RGB(0,136,255), RGB(255,68,136),
            RGB(170,68,255), RGB(255,200,0), RGB(0,200,200), RGB(255,100,100),
            RGB(136,255,68), RGB(68,136,200), RGB(255,136,200), RGB(68,255,200),
            RGB(200,200,0), RGB(200,68,255), RGB(68,200,255), RGB(255,170,68)
        };

        /* Draw notes */
        f32 dur = seq_duration > 0.0f ? seq_duration : 1.0f;
        int bar_h = roll_h / note_range;
        if (bar_h < 1) bar_h = 1;
        if (bar_h > 8) bar_h = 8;

        for (int i = 0; i < seq_note_count; i++) {
            const SeqNote& sn = seq_notes[i];
            int x0 = roll_left + (int)(sn.start_time / dur * roll_w);
            int x1 = roll_left + (int)(sn.end_time / dur * roll_w);
            if (x1 <= x0) x1 = x0 + 1;
            int ny = roll_bot - (int)((f32)(sn.note - note_lo) / note_range * roll_h);
            COLORREF col = ch_cols[sn.channel % 16];
            HBRUSH nb = CreateSolidBrush(col);
            RECT nr = {x0, ny - bar_h/2, x1, ny + bar_h/2 + 1};
            FillRect(buf, &nr, nb); DeleteObject(nb);
        }

        /* ── Playback cursor (vertical line at current position) ── */
        if (playing && play_time >= 0.0f && seq_duration > 0.0f) {
            f32 frac = play_time / seq_duration;
            if (frac > 1.0f) frac = 1.0f;
            int cx = roll_left + (int)(frac * roll_w);
            HPEN cur_pen = CreatePen(PS_SOLID, 2, RGB(255, 60, 60));
            HPEN old_pen = (HPEN)SelectObject(buf, cur_pen);
            MoveToEx(buf, cx, roll_top, 0);
            LineTo(buf, cx, roll_bot);
            SelectObject(buf, old_pen);
            DeleteObject(cur_pen);
        }

        /* Channel legend */
        SelectObject(buf, fTiny);
        int lx = roll_left + 4;
        for (int c = 0; c < 16; c++) {
            if (!(seq_ch_mask & (1 << c))) continue;
            if (lx + 40 > roll_right) break;
            HBRUSH lb = CreateSolidBrush(ch_cols[c % 16]);
            RECT lr = {lx, roll_top + 2, lx + 8, roll_top + 10};
            FillRect(buf, &lr, lb); DeleteObject(lb);
            char cl[8]; _snprintf(cl, 7, "ch%d", c);
            SetTextColor(buf, audio_dim());
            TextOutA(buf, lx + 10, roll_top + 1, cl, (int)strlen(cl));
            lx += 42;
        }
    }

snde_cleanup:
    SelectObject(buf, old_font);
    DeleteObject(fTitle); DeleteObject(fSmall); DeleteObject(fTiny);
}

/* ── Main paint dispatcher with double-buffer ── */
void AudioPanel::paint(HDC hdc, RECT& rc)
{
    int cw = rc.right - rc.left, ch_h = rc.bottom - rc.top;

    HDC buf = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch_h);
    HGDIOBJ old = SelectObject(buf, bmp);

    /* Background */
    HBRUSH bg = CreateSolidBrush(audio_panel_bg());
    FillRect(buf, &rc, bg); DeleteObject(bg);
    SetBkMode(buf, TRANSPARENT);

    switch (mode) {
    case AMODE_SNDB: paint_sndb(buf, cw, ch_h); break;
    case AMODE_SNDH: paint_sndh(buf, cw, ch_h); break;
    case AMODE_SNDE: paint_snde(buf, cw, ch_h); break;
    case AMODE_WAV:  paint_wav(buf, cw, ch_h);  break;
    default: {
        HFONT f = audio_font(12);
        HGDIOBJ old_font = SelectObject(buf, f); SetTextColor(buf, audio_dim());
        DrawTextA(buf, "Select an audio entry", -1, &rc, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(buf, old_font);
        DeleteObject(f);
    } break;
    }

    BitBlt(hdc, 0, 0, cw, ch_h, buf, 0, 0, SRCCOPY);
    SelectObject(buf, old); DeleteObject(bmp); DeleteDC(buf);
}

LRESULT CALLBACK AudioPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc); g_audio.paint(hdc, rc);
        EndPaint(hwnd, &ps); } return 0;
    case WM_KEYDOWN:
        if (g_audio.mode == AMODE_SNDB) {
            if (wp == VK_SPACE) {
                if (g_audio.playing) g_audio.stop(); else g_audio.play();
                InvalidateRect(hwnd, 0, FALSE);
            } else if (wp == VK_UP && g_audio.cur_sample > 0) {
                g_audio.select_sample(g_audio.cur_sample - 1);
            } else if (wp == VK_DOWN && g_audio.cur_sample < g_audio.num_samples - 1) {
                g_audio.select_sample(g_audio.cur_sample + 1);
            }
        } else if (g_audio.mode == AMODE_SNDE) {
            if (wp == VK_SPACE) {
                if (g_audio.playing) {
                    g_audio.stop();
                } else {
                    if (!g_audio.seq_rendered) g_audio.render_seq();
                    g_audio.play();
                }
                InvalidateRect(hwnd, 0, FALSE);
            }
        } else if (g_audio.mode == AMODE_WAV) {
            if (wp == VK_SPACE) {
                if (g_audio.playing) g_audio.stop(); else g_audio.play();
                InvalidateRect(hwnd, 0, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        if (g_audio.mode == AMODE_SNDB && g_audio.num_samples > 0) {
            int my = (short)HIWORD(lp);
            int hh = zs(26, g_audio.zoom) + 2;
            if (my >= hh) {
                RECT rc; GetClientRect(hwnd, &rc);
                int ch = rc.bottom - rc.top;
                int row_h = (ch - hh) / g_audio.num_samples;
                if (row_h > zs(56, g_audio.zoom)) row_h = zs(56, g_audio.zoom);
                if (row_h < zs(22, g_audio.zoom)) row_h = zs(22, g_audio.zoom);
                int clicked = (my - hh) / row_h;
                if (clicked >= 0 && clicked < g_audio.num_samples) {
                    g_audio.select_sample(clicked);
                    g_audio.play();
                }
            }
        } else if (g_audio.mode == AMODE_WAV && g_audio.pcm_data && g_audio.wav_total_samples > 0) {
            int mx = (short)LOWORD(lp);
            RECT rc; GetClientRect(hwnd, &rc);
            int cw = rc.right;
            int draw_w = cw - 16;
            int x_off = 8;
            if (mx >= x_off && mx < x_off + draw_w) {
                float frac = (float)(mx - x_off) / (float)draw_w;
                /* Just click to play from start — full seek requires
                   PCM buffer offsetting which complicates free().
                   For now: click waveform = play/restart. */
                g_audio.stop();
                g_audio.play_time = 0;
                g_audio.play();
                InvalidateRect(hwnd, 0, FALSE);
            }
        }
    } return 0;
    case WM_TIMER:
        if (wp == 8001 && g_audio.playing && g_audio.hWaveOut) {
            /* Poll playback position */
            MMTIME mmt; memset(&mmt, 0, sizeof(mmt));
            mmt.wType = TIME_SAMPLES;
            if (waveOutGetPosition(g_audio.hWaveOut, &mmt, sizeof(mmt)) == MMSYSERR_NOERROR) {
                u32 sample_pos = mmt.u.sample;
                g_audio.play_time = (f32)sample_pos / (f32)g_audio.sample_rate;
                /* Auto-stop when done */
                if ((int)sample_pos >= g_audio.pcm_count) {
                    g_audio.stop();
                }
            }
            InvalidateRect(hwnd, 0, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        bool ctrl = (LOWORD(wp) & MK_CONTROL) != 0;
        if (ctrl) {
            int step = (delta > 0) ? 10 : -10;
            g_audio.zoom += step;
            if (g_audio.zoom < 50) g_audio.zoom = 50;
            if (g_audio.zoom > 250) g_audio.zoom = 250;
            InvalidateRect(hwnd, 0, FALSE);
        }
    } return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/*═══════════════════════════════════════════════════════════════════
 *  3D Viewer (OpenGL)  -  display modes: solid/wireframe, lighting,
 *  bone skeleton overlay.  Controls:
 *    LMB drag = orbit,  RMB drag = pan,  Scroll = zoom
 *    W = wireframe,  L = lighting,  B = bones,  R = reset view
 *═══════════════════════════════════════════════════════════════════*/
bool ViewerPanel3D::init_gl(HWND parent) {
    hwnd = parent; hDC = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd; memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 24; pfd.cDepthBits = 24;
    int fmt = ChoosePixelFormat(hDC, &pfd);
    if (!fmt) return false;
    SetPixelFormat(hDC, fmt, &pfd);
    hRC = wglCreateContext(hDC);
    if (!hRC) return false;
    wglMakeCurrent(hDC, hRC);

    /* Detect software renderer (VM, no GPU passthrough) */
    const char* gl_renderer = (const char*)glGetString(GL_RENDERER);
    const char* gl_vendor   = (const char*)glGetString(GL_VENDOR);
    sw_renderer = false;
    gl_renderer_name[0] = 0;
    if (gl_renderer) {
        strncpy(gl_renderer_name, gl_renderer, 127);
        gl_renderer_name[127] = 0;
        /* Known software renderers */
        if (strstr(gl_renderer, "GDI Generic") ||
            strstr(gl_renderer, "llvmpipe") ||
            strstr(gl_renderer, "softpipe") ||
            strstr(gl_renderer, "swrast") ||
            strstr(gl_renderer, "Software") ||
            strstr(gl_renderer, "Mesa") ||
            strstr(gl_renderer, "Gallium") ||
            strstr(gl_renderer, "SVGA") ||
            strstr(gl_renderer, "VirtualBox") ||
            strstr(gl_renderer, "VMware") ||
            strstr(gl_renderer, "Parallels") ||
            strstr(gl_renderer, "QEMU") ||
            strstr(gl_renderer, "D3D")) {
            sw_renderer = true;
        }
    }
    if (gl_vendor) {
        if (strstr(gl_vendor, "Microsoft") ||
            strstr(gl_vendor, "VMware") ||
            strstr(gl_vendor, "Mesa") ||
            strstr(gl_vendor, "Humper") ||  /* Mesa/Gallium on Windows */
            strstr(gl_vendor, "Oracle")) {
            sw_renderer = true;
        }
    }
    /* Also check GL version — software renderers often report GL 1.1 */
    const char* gl_ver = (const char*)glGetString(GL_VERSION);
    if (gl_ver && (gl_ver[0] == '1' && gl_ver[1] == '.' && gl_ver[2] <= '1')) {
        sw_renderer = true;
    }

    /* Detect VM environment — affects mouse warp and GL state robustness.
       Even with real GPU passthrough, VM mouse integration breaks SetCursorPos
       and some GL drivers have display list / COLOR_MATERIAL quirks. */
    in_vm = sw_renderer; /* software renderer implies VM */
    if (!in_vm && gl_renderer) {
        if (strstr(gl_renderer, "VMware") || strstr(gl_renderer, "SVGA") ||
            strstr(gl_renderer, "VirtualBox") || strstr(gl_renderer, "Parallels") ||
            strstr(gl_renderer, "QEMU") || strstr(gl_renderer, "Hyper-V") ||
            strstr(gl_renderer, "virgl") || strstr(gl_renderer, "D3D12")) {
            in_vm = true;
        }
    }
    if (!in_vm && gl_vendor) {
        if (strstr(gl_vendor, "VMware") || strstr(gl_vendor, "Oracle") ||
            strstr(gl_vendor, "Parallels") || strstr(gl_vendor, "Red Hat")) {
            in_vm = true;
        }
    }
    /* Registry-based VM detection (catches GPU passthrough VMs where GL strings look normal) */
    if (!in_vm) {
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Services\\Disk\\Enum", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char val[256] = {};
            DWORD sz = 255;
            if (RegQueryValueExA(hKey, "0", 0, 0, (BYTE*)val, &sz) == ERROR_SUCCESS) {
                /* VM disk devices contain vendor names */
                if (strstr(val, "VMware") || strstr(val, "VBOX") ||
                    strstr(val, "Virtual") || strstr(val, "QEMU") ||
                    strstr(val, "Hyper-V") || strstr(val, "Xen") ||
                    strstr(val, "Msft") || strstr(val, "Red Hat")) {
                    in_vm = true;
                }
            }
            RegCloseKey(hKey);
        }
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glShadeModel(GL_SMOOTH);
    glClearColor(0.08f, 0.08f, 0.15f, 1.0f);

    if (sw_renderer || in_vm) {
        /* VM / Software fallback: disable all fragile fixed-function state.
           Immediate-mode drawing handles everything without display lists. */
        glDisable(GL_LIGHTING);
        glDisable(GL_LIGHT0); glDisable(GL_LIGHT1); glDisable(GL_LIGHT2);
        glDisable(GL_NORMALIZE);
        glDisable(GL_LINE_SMOOTH);
        glDisable(GL_CULL_FACE);
        glDisable(GL_COLOR_MATERIAL);
    } else {
        glEnable(GL_LIGHTING);

        /* 3-point light setup for model inspection */
        glEnable(GL_LIGHT0);
        { float a[] = {0.15f,0.15f,0.15f,1}, d[] = {0.70f,0.70f,0.68f,1};
          glLightfv(GL_LIGHT0, GL_AMBIENT, a);
          glLightfv(GL_LIGHT0, GL_DIFFUSE, d); }
        glEnable(GL_LIGHT1);
        { float a[] = {0.0f,0.0f,0.0f,1}, d[] = {0.35f,0.30f,0.25f,1};
          float p[] = {-0.5f, 0.8f, 0.3f, 0.0f};
          glLightfv(GL_LIGHT1, GL_AMBIENT, a);
          glLightfv(GL_LIGHT1, GL_DIFFUSE, d);
          glLightfv(GL_LIGHT1, GL_POSITION, p); }
        glEnable(GL_LIGHT2);
        { float a[] = {0.0f,0.0f,0.0f,1}, d[] = {0.20f,0.22f,0.30f,1};
          float p[] = {0.3f, -0.4f, -0.8f, 0.0f};
          glLightfv(GL_LIGHT2, GL_AMBIENT, a);
          glLightfv(GL_LIGHT2, GL_DIFFUSE, d);
          glLightfv(GL_LIGHT2, GL_POSITION, p); }

        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        glEnable(GL_NORMALIZE);
        glEnable(GL_LINE_SMOOTH);
        glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    }

    /* Create bitmap font from system fixed-width font */
    gl_font_base = glGenLists(128);
    HFONT hf = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    HFONT old = (HFONT)SelectObject(hDC, hf);
    wglUseFontBitmapsA(hDC, 0, 128, gl_font_base);
    SelectObject(hDC, old);
    DeleteObject(hf);

    wglMakeCurrent(0, 0);
    return true;
}

void ViewerPanel3D::shutdown_gl() {
    free(walk_grid); walk_grid = 0;
    if (has_mesh) {
        wglMakeCurrent(hDC, hRC);
        if (gl_list_id) glDeleteLists(gl_list_id, 1);
        if (gl_alt_id) glDeleteLists(gl_alt_id, 1);
        if (gl_2side_id) { glDeleteLists(gl_2side_id, 1); gl_2side_id = 0; }
        if (gl_wire_id) glDeleteLists(gl_wire_id, 1);
        if (gl_bone_id) glDeleteLists(gl_bone_id, 1);
        if (gl_blend_id) glDeleteLists(gl_blend_id, 1);
        if (gl_sub_id) glDeleteLists(gl_sub_id, 1);
        if (gl_font_base) glDeleteLists(gl_font_base, 128);
        if (gl_tex_id) glDeleteTextures(1, &gl_tex_id);
        wglMakeCurrent(0, 0);
    }
    if (hRC) { wglDeleteContext(hRC); hRC = 0; }
    if (hDC && hwnd) { ReleaseDC(hwnd, hDC); hDC = 0; }
}

void ViewerPanel3D::resize(int w, int h) {
    if (!hRC) return;
    wglMakeCurrent(hDC, hRC);
    /* Leave room for animation control bar at bottom when active */
    int bar_h = anim_model ? 66 : 0;
    int vh = h - bar_h;
    if (vh < 1) vh = 1;
    w = view_w(w);
    glViewport(0, bar_h, w, vh);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    gluPerspective(45.0, (double)w / (double)vh, 1.0, 100000.0);
    glMatrixMode(GL_MODELVIEW);
    wglMakeCurrent(0, 0);
}

/* Atlas slice holding a face's CLUT: 2D sparse slice lookup,
   slot 0 = 8bpp, slot rel+1 = 4bpp sub-palette. */
static int face_atlas_slice(const MeshTri& t, int clut_base_y, int clut_base_x,
                            int total_v_slices)
{
    if (total_v_slices <= 1) return 0;
    int clut_y = (t.clut >> 6) & 0x1FF;
    int row = clut_y - clut_base_y;
    if (row < 0) row = 0;
    if (row >= 16) row = 15;

    int slice_idx;
    if (((t.tpage >> 7) & 3) == 0) { /* 4bpp face → slot rel+1 */
        int clut_x = (t.clut & 0x3F) * 16;
        int rel = (clut_x - clut_base_x) / 16;
        if (rel < 0) rel = 0; if (rel >= 64) rel = 63;
        slice_idx = g_viewer3d.slice_map[row * 65 + rel + 1];
        if (slice_idx < 0) slice_idx = g_viewer3d.slice_map[row * 65 + 0]; /* fallback: 8bpp */
    } else { /* 8bpp face → slot 0 */
        slice_idx = g_viewer3d.slice_map[row * 65 + 0];
    }
    if (slice_idx < 0) slice_idx = 0; /* ultimate fallback */
    return slice_idx;
}

/* Marks the faces whose texture has see-through texels (alpha 0, which the
   alpha test discards) inside the triangle: fences, grilles, and signs
   painted on fence panels.  The game draws room faces from both sides, and
   ST112's signs face away from the camera that shows them, so these faces
   are drawn without culling; with culling the viewer only showed them from
   behind, mirrored.  Solid faces keep culling so walls still let the camera
   see into a room from outside. */
static void mark_see_through(const Mesh& mesh, u8* out, const u8* atlas,
                             int atlas_w, int atlas_h, u16 tex_vram_x, int tex_bpp,
                             int clut_base_y, u16 tex_vram_y, int num_sub_pals,
                             int clut_base_x)
{
    int tex_ppw = (tex_bpp == 4) ? 4 : (tex_bpp == 16) ? 1 : 2;
    int total_v_slices = num_sub_pals;
    int single_h = (total_v_slices > 1) ? atlas_h / total_v_slices : atlas_h;

    for (int i = 0; i < mesh.tri_count; i++) {
        const MeshTri& t = mesh.tris[i];
        out[i] = 0;
        if (!atlas) continue;
        int tp = (t.tpage >> 7) & 3;
        int face_ppw = (tp == 0) ? 4 : (tp == 2) ? 1 : 2;
        int x0 = ((t.tpage & 0xF) * 64 - (int)tex_vram_x) * tex_ppw;
        int y0 = face_atlas_slice(t, clut_base_y, clut_base_x, total_v_slices) * single_h
               + ((t.tpage >> 4) & 1) * 256 - (int)tex_vram_y;

        /* Texel centres inside the triangle in UV space */
        int ua = t.uv[0][0], va = t.uv[0][1];
        int ub = t.uv[1][0], vb = t.uv[1][1];
        int uc = t.uv[2][0], vc = t.uv[2][1];
        int area = (ub - ua) * (vc - va) - (uc - ua) * (vb - va);
        if (area == 0) continue;
        int umin = ua < ub ? ua : ub; if (uc < umin) umin = uc;
        int umax = ua > ub ? ua : ub; if (uc > umax) umax = uc;
        int vmin = va < vb ? va : vb; if (vc < vmin) vmin = vc;
        int vmax = va > vb ? va : vb; if (vc > vmax) vmax = vc;
        for (int v = vmin; v < vmax && !out[i]; v++) {
            for (int u = umin; u < umax; u++) {
                /* doubled coordinates: centre of texel (u, v) */
                int pu = 2 * u + 1, pv = 2 * v + 1;
                long w0 = (long)(2*ub - pu) * (2*vc - pv) - (long)(2*uc - pu) * (2*vb - pv);
                long w1 = (long)(2*uc - pu) * (2*va - pv) - (long)(2*ua - pu) * (2*vc - pv);
                long w2 = (long)(2*ua - pu) * (2*vb - pv) - (long)(2*ub - pu) * (2*va - pv);
                if (area > 0 ? (w0 < 0 || w1 < 0 || w2 < 0) : (w0 > 0 || w1 > 0 || w2 > 0))
                    continue;
                int px = x0 + u * tex_ppw / face_ppw;
                int py = y0 + v;
                if (px < 0 || px >= atlas_w || py < 0 || py >= atlas_h) continue;
                if (atlas[((size_t)py * atlas_w + px) * 4 + 3] == 0) { out[i] = 1; break; }
            }
        }
    }
}

/* Build all display lists from mesh + optional skeleton */
/* Build display list from mesh */
static void build_solid_list(int list_id, const Mesh& mesh,
                             int tex_w, int tex_h, u16 tex_vram_x, int tex_bpp,
                             int num_pal_rows = 1, int clut_base_y = 0,
                             bool flip_normals = true, u16 tex_vram_y = 0,
                             int alt_mode = 0 /* 0=all, 1=primary only, 2=alt only */,
                             int num_sub_pals = 1, int clut_base_x = 0,
                             int abr_filter = -1 /* -1=all, 0..3=only that ABR */,
                             const u8* see_through = 0,
                             int side_filter = -1 /* -1=all, 0=solid only, 1=see-through only */) {
    /* tex_ppw: pixels per VRAM halfword at the TEXTURE's rendered BPP */
    int tex_ppw = (tex_bpp == 4) ? 4 : (tex_bpp == 16) ? 1 : 2;
    /* With 2D sparse atlas: num_sub_pals = total compact slices.
       Each slice is padded_h=512 tall. single_h = one slice height. */
    int total_v_slices = num_sub_pals;
    int single_h = (total_v_slices > 1) ? tex_h / total_v_slices : tex_h;

    glNewList(list_id, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    /* Within a section, last face first: the game adds a section's quads and
       then its triangles to its ordering table (DINO.exe 0x44DA6D), each at
       the head of its bucket (0x5F2DD0), so of two faces at the same depth
       the earlier one is drawn last and ends up on top.  mesh_apply_xforms
       emits faces in that order.  Signs are coplanar quads over fence quads
       (ST112), and the passes draw with GL_LEQUAL, where the face drawn last
       wins the tie.  Sections keep their order. */
    for (int run = 0; run < mesh.tri_count; ) {
    int run_end = run + 1;
    while (run_end < mesh.tri_count && mesh.tris[run_end].src_off == mesh.tris[run].src_off)
        run_end++;
    for (int i = run_end - 1; i >= run; i--) {
        const MeshTri& t = mesh.tris[i];

        /* Filter by alt_mode: 1=primary only (skip alt), 2=alt only (skip primary) */
        if (alt_mode == 1 && t.alt) continue;
        if (alt_mode == 2 && !t.alt) continue;
        if (see_through && side_filter >= 0 && (see_through[i] != 0) != (side_filter == 1)) continue;

        /* Filter by ABR (semi-transparency mode):
           -1=all, 0=opaque only, 1=additive (ABR 1+3), 2=subtractive (ABR 2) */
        if (abr_filter >= 0) {
            int face_abr = (t.tpage >> 5) & 3;
            if (abr_filter == 0 && face_abr != 0) continue;           /* opaque only */
            if (abr_filter == 1 && face_abr != 1 && face_abr != 3) continue; /* additive only */
            if (abr_filter == 2 && face_abr != 2) continue;           /* subtractive only */
        }

        if ((int)t.idx[0] >= mesh.vert_count || (int)t.idx[1] >= mesh.vert_count ||
            (int)t.idx[2] >= mesh.vert_count) continue;
        const MeshVert& v0 = mesh.verts[t.idx[0]];
        const MeshVert& v1 = mesh.verts[t.idx[1]];
        const MeshVert& v2 = mesh.verts[t.idx[2]];

        /* Compute face normal as fallback */
        float ax = v1.x-v0.x, ay = v1.y-v0.y, az = v1.z-v0.z;
        float bx = v2.x-v0.x, by = v2.y-v0.y, bz = v2.z-v0.z;
        float fnx = ay*bz-az*by, fny = az*bx-ax*bz, fnz = ax*by-ay*bx;
        float flen = sqrtf(fnx*fnx+fny*fny+fnz*fnz);
        if (flen > 0.0001f) { fnx /= flen; fny /= flen; fnz /= flen; }
        if (flip_normals) { fnx = -fnx; fny = -fny; fnz = -fnz; }

        /* Check if vertices have smooth normals */
        bool has_vnorms = (v0.nx != 0 || v0.ny != 0 || v0.nz != 0);

        /* If no per-vertex normals, set face normal once (flat shading) */
        if (!has_vnorms) glNormal3f(fnx, fny, fnz);

        /* PSX GPU textured color math: result = (tex * vtx * 2) >> 8
           This means vtx=128 → 1.0x modulation (neutral).
           GL_MODULATE does: result = tex * (glColor / 255).
           To match PSX, scale vertex colors by 2: glColor = min(vtx*2, 255)
           so that GL gives tex * min(vtx*2,255)/255 ≈ tex * vtx/128. */
        u8 cr = (u8)(t.color.r < 128 ? t.color.r * 2 : 255);
        u8 cg = (u8)(t.color.g < 128 ? t.color.g * 2 : 255);
        u8 cb = (u8)(t.color.b < 128 ? t.color.b * 2 : 255);
        if (t.alt) { cr = 255; cg = 96; cb = 255; }  /* unplaced: magenta tint */
        glColor3ub(cr, cg, cb);

        /* Room and door faces are Gouraud-shaded from their corner colours,
           taken as stored rather than from the brightened face average in
           t.color.  The GPU brightens a texel up to 2x for colours above 128;
           GL_MODULATE stops at 1x, so those clamp.  ST108's wall box is one
           grey texel shaded 30..121 corner to corner, flat white without. */
        bool gouraud = t.gouraud && !t.alt;

        /* PS1 tpage UV mapping */
        int tx = t.tpage & 0xF;
        int ty = (t.tpage >> 4) & 1;
        int tp = (t.tpage >> 7) & 3;
        int face_ppw = (tp == 0) ? 4 : (tp == 2) ? 1 : 2;

        float tw = (tex_w > 0) ? (float)tex_w : 256.0f;
        float th = (tex_h > 0) ? (float)tex_h : 256.0f;
        float u_off = (float)((tx * 64 - (int)tex_vram_x) * tex_ppw) / tw;
        float v_off = (float)(ty * 256 - (int)tex_vram_y) / (float)single_h;  /* within one slice */
        float u_scl = (float)tex_ppw / ((float)face_ppw * tw);

        /* CLUT atlas V offset: 2D sparse slice lookup.
           slot 0 = 8bpp, slot rel+1 = 4bpp sub-palette. */
        float clut_v_off = 0.0f;
        if (total_v_slices > 1) {
            int slice_idx = face_atlas_slice(t, clut_base_y, clut_base_x, total_v_slices);
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
            float u = u_off + ((float)t.uv[vi][0] + 0.5f) * u_scl;
            float v = clut_v_off + v_off + ((float)t.uv[vi][1] + 0.5f) / th;
            glTexCoord2f(u, v);
            if (gouraud) {
                const u8* c = t.vcol[vi];
                glColor3ub((u8)(c[0] < 128 ? c[0] * 2 : 255),
                           (u8)(c[1] < 128 ? c[1] * 2 : 255),
                           (u8)(c[2] < 128 ? c[2] * 2 : 255));
            }
            const MeshVert& vv = mesh.verts[t.idx[vi]];
            if (has_vnorms) {
                float vnx = vv.nx, vny = vv.ny, vnz = vv.nz;
                if (flip_normals) { vnx = -vnx; vny = -vny; vnz = -vnz; }
                glNormal3f(vnx, vny, vnz);
            }
            glVertex3f(vv.x, vv.y, vv.z);
        }
    }
    run = run_end;
    }
    glEnd();
    glEndList();
}

static void build_wire_list(int list_id, const Mesh& mesh) {
    glNewList(list_id, GL_COMPILE);
    glBegin(GL_LINES);
    for (int i = 0; i < mesh.tri_count; i++) {
        const MeshTri& t = mesh.tris[i];
        if ((int)t.idx[0] >= mesh.vert_count || (int)t.idx[1] >= mesh.vert_count ||
            (int)t.idx[2] >= mesh.vert_count) continue;
        if (t.alt) continue;
        glColor3ub(t.color.r, t.color.g, t.color.b);
        for (int e = 0; e < 3; e++) {
            int a = t.idx[e], b = t.idx[(e+1)%3];
            glVertex3f(mesh.verts[a].x, mesh.verts[a].y, mesh.verts[a].z);
            glVertex3f(mesh.verts[b].x, mesh.verts[b].y, mesh.verts[b].z);
        }
    }
    glEnd(); glEndList();
}

static void build_normals_list(int list_id, const Mesh& mesh) {
    /* Compute line length: 2% of bounding box diagonal */
    float minx=1e9f, miny=1e9f, minz=1e9f, maxx=-1e9f, maxy=-1e9f, maxz=-1e9f;
    for (int i = 0; i < mesh.vert_count; i++) {
        const MeshVert& v = mesh.verts[i];
        if (v.x < minx) minx = v.x; if (v.x > maxx) maxx = v.x;
        if (v.y < miny) miny = v.y; if (v.y > maxy) maxy = v.y;
        if (v.z < minz) minz = v.z; if (v.z > maxz) maxz = v.z;
    }
    float diag = sqrtf((maxx-minx)*(maxx-minx) + (maxy-miny)*(maxy-miny) + (maxz-minz)*(maxz-minz));
    float nlen = diag * 0.02f;
    if (nlen < 1.0f) nlen = 1.0f;

    /* Check if mesh has per-vertex normals */
    bool has_vnorms = false;
    for (int i = 0; i < mesh.vert_count && !has_vnorms; i++) {
        if (mesh.verts[i].nx != 0 || mesh.verts[i].ny != 0 || mesh.verts[i].nz != 0)
            has_vnorms = true;
    }

    glNewList(list_id, GL_COMPILE);
    glBegin(GL_LINES);

    if (has_vnorms) {
        /* Per-vertex normals (skinned meshes) */
        for (int i = 0; i < mesh.vert_count; i++) {
            const MeshVert& v = mesh.verts[i];
            float nx = v.nx, ny = v.ny, nz = v.nz;
            if (nx == 0.0f && ny == 0.0f && nz == 0.0f) continue;
            float mag = sqrtf(nx*nx + ny*ny + nz*nz);
            if (mag < 0.0001f) continue;
            nx /= mag; ny /= mag; nz /= mag;
            glColor3f(0.2f, 0.4f, 1.0f);
            glVertex3f(v.x, v.y, v.z);
            glColor3f(0.6f, 0.8f, 1.0f);
            glVertex3f(v.x + nx * nlen, v.y + ny * nlen, v.z + nz * nlen);
        }
    } else {
        /* Face normals (static meshes — rooms, doors) */
        for (int i = 0; i < mesh.tri_count; i++) {
            const MeshTri& t = mesh.tris[i];
            if ((int)t.idx[0] >= mesh.vert_count || (int)t.idx[1] >= mesh.vert_count ||
                (int)t.idx[2] >= mesh.vert_count) continue;
            const MeshVert& a = mesh.verts[t.idx[0]];
            const MeshVert& b = mesh.verts[t.idx[1]];
            const MeshVert& c = mesh.verts[t.idx[2]];
            /* Face centroid */
            float cx = (a.x+b.x+c.x)/3.0f;
            float cy = (a.y+b.y+c.y)/3.0f;
            float cz = (a.z+b.z+c.z)/3.0f;
            /* Cross product for face normal */
            float ex=b.x-a.x, ey=b.y-a.y, ez=b.z-a.z;
            float fx=c.x-a.x, fy=c.y-a.y, fz=c.z-a.z;
            float nx=ey*fz-ez*fy, ny=ez*fx-ex*fz, nz=ex*fy-ey*fx;
            float mag = sqrtf(nx*nx+ny*ny+nz*nz);
            if (mag < 0.0001f) continue;
            nx /= mag; ny /= mag; nz /= mag;
            glColor3f(0.2f, 0.4f, 1.0f);
            glVertex3f(cx, cy, cz);
            glColor3f(0.6f, 0.8f, 1.0f);
            glVertex3f(cx + nx*nlen, cy + ny*nlen, cz + nz*nlen);
        }
    }

    glEnd();
    glEndList();
}

static void build_bone_list(int list_id, const EmdSkeleton& skel) {
    glNewList(list_id, GL_COMPILE);
    /* Draw bone lines: parent → child */
    glLineWidth(3.0f);
    glBegin(GL_LINES);
    for (int i = 0; i < skel.bone_count; i++) {
        const EmdBone& b = skel.bones[i];
        int par = (b.parent == 0xFF) ? -1 : (int)b.parent;
        if (par < 0 || par >= skel.bone_count) continue;
        glColor3f(0.0f, 1.0f, 0.3f);
        glVertex3f(skel.abs_pos[par][0], skel.abs_pos[par][1], skel.abs_pos[par][2]);
        glVertex3f(skel.abs_pos[i][0], skel.abs_pos[i][1], skel.abs_pos[i][2]);
    }
    glEnd();
    /* Draw bone joints as points */
    glPointSize(8.0f);
    glBegin(GL_POINTS);
    for (int i = 0; i < skel.bone_count; i++) {
        /* Root = yellow, leaves = cyan, others = green */
        const EmdBone& b = skel.bones[i];
        int par = (b.parent == 0xFF) ? -1 : (int)b.parent;
        if (par < 0) glColor3f(1.0f, 1.0f, 0.0f);
        else         glColor3f(0.0f, 1.0f, 0.5f);
        glVertex3f(skel.abs_pos[i][0], skel.abs_pos[i][1], skel.abs_pos[i][2]);
    }
    glEnd();
    glLineWidth(1.0f);
    glPointSize(1.0f);
    glEndList();
}

/*─── Build scene overlay display list (zones, collisions, cameras, spawns) ───*/
/* The element selected in the overlay tree is drawn again on top, white
   and thicker (markers larger), so it can be told apart. */
static bool s_ov_hl;
static void ov_color(f32 r, f32 g, f32 b, f32 a = 1.0f) {
    if (s_ov_hl) glColor4f(1.0f, 1.0f, 1.0f, 1.0f); else glColor4f(r, g, b, a);
}
static void ov_width(f32 w) { glLineWidth(s_ov_hl ? w + 3.0f : w); }

/* Highlight: a tall white pin standing on the element (viewer coords) */
static void ov_pin(f32 x, f32 y, f32 z) {
    glLineWidth(2.0f);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_LINES); glVertex3f(x, y, z); glVertex3f(x, y + 2500.0f, z); glEnd();
    glPointSize(12.0f);
    glBegin(GL_POINTS); glVertex3f(x, y + 2500.0f, z); glEnd();
    glPointSize(1.0f);
}

static void ov_draw_zone(const RdtSceneOverlay& ov, int i) {
    const RdtFloorZone& z = ov.zones[i];
    if (z.kind == RDT_ZONE_TRIGGER) {
        /* SCD 0x28 zone — color by type number.
           Type only determines data layout size, not gameplay role.
           Only type 4 is confirmed as door (exe calls sub_426DFC). */
        switch (z.target_index) {
            case 0: ov_color(1.0f, 0.4f, 0.1f); break;  /* orange = type 0 */
            case 1: ov_color(0.2f, 0.7f, 1.0f); break;  /* light blue = type 1 */
            case 2: ov_color(0.9f, 0.9f, 0.2f); break;  /* yellow = type 2 */
            case 3: ov_color(0.2f, 0.9f, 0.5f); break;  /* mint = type 3 */
            case 4: ov_color(0.0f, 0.9f, 0.9f); break;  /* cyan = type 4 (door) */
            default: ov_color(0.8f, 0.3f, 0.8f); break; /* purple = other */
        }
        ov_width((z.target_index == 4) ? 3.0f : 2.0f);
    } else {
        ov_width(2.0f);
        ov_color(1.0f, 1.0f, 0.0f);         /* yellow = floor zone */
    }

    f32 y0 = ov.zone_y[i] + -(f32)z.y;
    f32 y1 = y0 + (f32)z.height;

    if (z.height > 0 && z.kind == RDT_ZONE_TRIGGER) {
        /* 3D wireframe box: floor quad + ceiling quad + verticals */
        /* Floor */
        glBegin(GL_LINE_LOOP);
        for (int c = 0; c < 4; c++)
            glVertex3f(-(f32)z.x[c], y0, (f32)z.z[c]);
        glEnd();
        /* Ceiling */
        glBegin(GL_LINE_LOOP);
        for (int c = 0; c < 4; c++)
            glVertex3f(-(f32)z.x[c], y1, (f32)z.z[c]);
        glEnd();
        /* Vertical edges */
        glBegin(GL_LINES);
        for (int c = 0; c < 4; c++) {
            glVertex3f(-(f32)z.x[c], y0, (f32)z.z[c]);
            glVertex3f(-(f32)z.x[c], y1, (f32)z.z[c]);
        }
        glEnd();
    } else {
        /* Flat floor outline */
        glBegin(GL_LINE_LOOP);
        for (int c = 0; c < 4; c++)
            glVertex3f(-(f32)z.x[c], y0, (f32)z.z[c]);
        glEnd();
    }
    if (s_ov_hl)
        ov_pin(-(z.x[0] + z.x[1] + z.x[2] + z.x[3]) * 0.25f, y0, (z.z[0] + z.z[1] + z.z[2] + z.z[3]) * 0.25f);
}

/* Collision rects (ptr[1]): walkable floor areas — green outline */
static void ov_draw_rect(const RdtSceneOverlay& ov, int i) {
    ov_width(2.0f);
    ov_color(0.0f, 0.8f, 0.2f);
    const RdtCollisionRect& r = ov.collisions[i];
    f32 x0 = -(f32)r.x;
    f32 x1 = -(f32)(r.x + (s16)r.w);
    f32 z0 = (f32)r.z;
    f32 z1 = (f32)(r.z + (s16)r.h);
    f32 ry = ov.rect_y[i];
    glBegin(GL_LINE_LOOP);
    glVertex3f(x0, ry, z0);
    glVertex3f(x1, ry, z0);
    glVertex3f(x1, ry, z1);
    glVertex3f(x0, ry, z1);
    glEnd();
    if (s_ov_hl) ov_pin((x0 + x1) * 0.5f, ry, (z0 + z1) * 0.5f);
}

/* Dino Crisis 2 collision polygons (room slot 2): green outline, drawn
   on the room's floor plane */
static void ov_draw_poly(const RdtSceneOverlay& ov, int i) {
    ov_width(2.0f);
    ov_color(0.0f, 0.8f, 0.2f);
    const RdtPoly& p = ov.polys[i];
    f32 cx = 0, cz = 0;
    glBegin(GL_LINE_LOOP);
    for (int k = 0; k < p.count; k++) {
        const s16* pt = ov.poly_pts[p.first + k];
        glVertex3f(-(f32)pt[0], ov.floor_y, (f32)pt[1]);
        cx += -(f32)pt[0]; cz += (f32)pt[1];
    }
    glEnd();
    if (s_ov_hl && p.count) ov_pin(cx / p.count, ov.floor_y, cz / p.count);
}

/* Script cameras (0x4C, 0x2E): blue eye → red target */
static void ov_draw_script_cam(const RdtSceneOverlay& ov, int i) {
    const RdtCameraEntry& cam = ov.cameras[i];
    glPointSize(s_ov_hl ? 16.0f : 10.0f);
    glBegin(GL_POINTS);
    ov_color(0.2f, 0.4f, 1.0f);
    glVertex3f(-(f32)cam.eye_x, -(f32)cam.eye_y, (f32)cam.eye_z);
    ov_color(1.0f, 0.2f, 0.2f);
    glVertex3f(-(f32)cam.tgt_x, -(f32)cam.tgt_y, (f32)cam.tgt_z);
    glEnd();
    ov_width(1.0f);
    glBegin(GL_LINES);
    ov_color(0.2f, 0.4f, 1.0f);
    glVertex3f(-(f32)cam.eye_x, -(f32)cam.eye_y, (f32)cam.eye_z);
    ov_color(1.0f, 0.2f, 0.2f);
    glVertex3f(-(f32)cam.tgt_x, -(f32)cam.tgt_y, (f32)cam.tgt_z);
    glEnd();
    glPointSize(1.0f);
    if (s_ov_hl) ov_pin(-(f32)cam.eye_x, -(f32)cam.eye_y, (f32)cam.eye_z);
}

/* Cross of half-size s (object spawns, character placements) */
static void ov_cross(f32 x, f32 y, f32 z, f32 s) {
    glBegin(GL_LINES);
    glVertex3f(x - s, y, z); glVertex3f(x + s, y, z);
    glVertex3f(x, y - s, z); glVertex3f(x, y + s, z);
    glVertex3f(x, y, z - s); glVertex3f(x, y, z + s);
    glEnd();
}

/* Object spawns (RdtObjectSpawn): magenta crosses */
static void ov_draw_spawn(const RdtSceneOverlay& ov, int i) {
    ov_width(2.0f);
    ov_color(1.0f, 0.3f, 1.0f);
    const RdtObjectSpawn& sp = ov.spawns[i];
    ov_cross(-(f32)sp.px, -(f32)sp.py, (f32)sp.pz, s_ov_hl ? 400.0f : 200.0f);
    if (s_ov_hl) ov_pin(-(f32)sp.px, -(f32)sp.py, (f32)sp.pz);
}

/* Character placements (0x20): orange crosses */
static void ov_draw_char(const RdtSceneOverlay& ov, int i) {
    ov_width(2.0f);
    ov_color(1.0f, 0.6f, 0.0f);
    const RdtCharPlace& cp = ov.chars[i];
    ov_cross(-(f32)cp.px, -(f32)cp.py, (f32)cp.pz, s_ov_hl ? 600.0f : 300.0f);
    if (s_ov_hl) ov_pin(-(f32)cp.px, -(f32)cp.py, (f32)cp.pz);
}

/* Camera cut zones (ptr[2]): violet rect outlines */
static void ov_draw_camcut(const RdtSceneOverlay& ov, int i) {
    ov_width(1.5f);
    const RdtCameraCutZone& cc = ov.camcuts[i];
    float r = 0.5f + 0.15f * (cc.cam_group % 4);
    float g = 0.3f;
    float b = 0.8f + 0.05f * (cc.cam_group % 3);
    ov_color(r, g, b);
    /* x,z = top-left; w,h = dimensions in XZ */
    f32 x0 = -(f32)cc.x;
    f32 x1 = -(f32)(cc.x + cc.w);
    f32 z0 = (f32)cc.z;
    f32 z1 = (f32)(cc.z + cc.h);
    f32 cy = ov.camcut_y[i] + 10.0f;
    glBegin(GL_LINE_LOOP);
    glVertex3f(x0, cy, z0);
    glVertex3f(x1, cy, z0);
    glVertex3f(x1, cy, z1);
    glVertex3f(x0, cy, z1);
    glEnd();
    if (s_ov_hl) ov_pin((x0 + x1) * 0.5f, cy, (z0 + z1) * 0.5f);
}

/* Scene lights (0x3A): colored octahedron wireframes */
static void ov_draw_light(const RdtSceneOverlay& ov, int i) {
    const RdtSceneLight& li = ov.lights[i];
    if (!li.has_pos) return;
    ov_width(2.0f);
    f32 x = -(f32)li.px, y = -(f32)li.py, z = (f32)li.pz;
    f32 r = li.has_color ? li.r / 255.0f : 0.9f;
    f32 g = li.has_color ? li.g / 255.0f : 0.85f;
    f32 b = li.has_color ? li.b / 255.0f : 0.75f;
    f32 s = s_ov_hl ? 300.0f : 150.0f;
    /* Wireframe octahedron */
    ov_color(r, g, b);
    glBegin(GL_LINES);
    /* Equator */
    glVertex3f(x+s, y, z); glVertex3f(x, y, z+s);
    glVertex3f(x, y, z+s); glVertex3f(x-s, y, z);
    glVertex3f(x-s, y, z); glVertex3f(x, y, z-s);
    glVertex3f(x, y, z-s); glVertex3f(x+s, y, z);
    /* Top pole */
    glVertex3f(x, y+s, z); glVertex3f(x+s, y, z);
    glVertex3f(x, y+s, z); glVertex3f(x-s, y, z);
    glVertex3f(x, y+s, z); glVertex3f(x, y, z+s);
    glVertex3f(x, y+s, z); glVertex3f(x, y, z-s);
    /* Bottom pole */
    glVertex3f(x, y-s, z); glVertex3f(x+s, y, z);
    glVertex3f(x, y-s, z); glVertex3f(x-s, y, z);
    glVertex3f(x, y-s, z); glVertex3f(x, y, z+s);
    glVertex3f(x, y-s, z); glVertex3f(x, y, z-s);
    glEnd();
    /* Floor radius circle using actual light radius */
    ov_width(1.0f);
    ov_color(r, g, b, 0.3f);
    f32 rad = (li.radius > 0) ? (f32)li.radius : 800.0f;
    glBegin(GL_LINE_LOOP);
    for (int a = 0; a < 24; a++) {
        f32 ang = (f32)a * (2.0f * 3.14159f / 24.0f);
        glVertex3f(x + rad * cosf(ang), ov.floor_y, z + rad * sinf(ang));
    }
    glEnd();
    if (s_ov_hl) ov_pin(x, y, z);
}

/* Enemy spawns (0x5B): red X on a circle with a post */
static void ov_draw_enemy(const RdtSceneOverlay& ov, int i) {
    ov_width(3.0f);
    ov_color(1.0f, 0.15f, 0.1f);  /* red */
    const RdtItemSpawn& es = ov.items[i];
    f32 x = -(f32)es.px, y = -(f32)es.py, z = (f32)es.pz;
    f32 s = 250.0f;
    /* X marker (two diagonals) */
    glBegin(GL_LINES);
    glVertex3f(x - s, y,     z - s); glVertex3f(x + s, y,     z + s);
    glVertex3f(x - s, y,     z + s); glVertex3f(x + s, y,     z - s);
    /* Vertical line showing height */
    glVertex3f(x, y,     z); glVertex3f(x, y + s*2, z);
    /* Small crossbar at top */
    glVertex3f(x - s*0.4f, y + s*2, z); glVertex3f(x + s*0.4f, y + s*2, z);
    glEnd();
    /* Circle on ground */
    glBegin(GL_LINE_LOOP);
    for (int a = 0; a < 16; a++) {
        f32 ang = (f32)a * 6.2832f / 16.0f;
        glVertex3f(x + s * cosf(ang), y, z + s * sinf(ang));
    }
    glEnd();
    if (s_ov_hl) ov_pin(x, y, z);
}

/* Game cameras: the trigger zones (ptr[3]) that switch to camera cam_id,
   dashed, per-camera color */
static void ov_draw_camera(const RdtSceneOverlay& ov, int cam_id) {
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(1, 0xF0F0);
    ov_width(2.0f);
    float hue = (float)(cam_id % 8) / 8.0f;
    float r = 0.5f + 0.5f * sinf(hue * 6.28f);
    float g = 0.5f + 0.5f * sinf(hue * 6.28f + 2.09f);
    float b = 0.5f + 0.5f * sinf(hue * 6.28f + 4.19f);
    ov_color(r, g, b);
    for (int i = 0; i < ov.n_cam_threads; i++) {
        const RdtSceneOverlay::CamThread& ct = ov.cam_threads[i];
        if (ct.cam_id != cam_id) continue;
        glBegin(GL_LINE_LOOP);
        for (int c = 0; c < 4; c++)
            glVertex3f(-(f32)ct.x[c], ov.cam_thread_y[i] + 15.0f, (f32)ct.z[c]);
        glEnd();
    }
    glDisable(GL_LINE_STIPPLE);
    for (int i = 0; i < ov.n_cam_threads && s_ov_hl; i++) {
        const RdtSceneOverlay::CamThread& ct = ov.cam_threads[i];
        if (ct.cam_id == cam_id)
            ov_pin(-(ct.x[0] + ct.x[1] + ct.x[2] + ct.x[3]) * 0.25f, ov.cam_thread_y[i] + 15.0f,
                   (ct.z[0] + ct.z[1] + ct.z[2] + ct.z[3]) * 0.25f);
    }
}

/* Objects (0x42): small white stars.  Mostly effect objects (such as the
   80 by ST10A's broken window) */
static void ov_draw_object(const RdtSceneOverlay& ov, int i) {
    ov_width(2.0f);
    ov_color(1.0f, 1.0f, 1.0f);
    const RdtEnemySpawn& is = ov.enemies[i];
    f32 x = -(f32)is.px, y = -(f32)is.py, z = (f32)is.pz;
    f32 s = s_ov_hl ? 180.0f : 60.0f;
    /* 6-pointed star: 3 intersecting lines */
    glBegin(GL_LINES);
    glVertex3f(x - s, y, z); glVertex3f(x + s, y, z);
    glVertex3f(x, y - s, z); glVertex3f(x, y + s, z);
    glVertex3f(x, y, z - s); glVertex3f(x, y, z + s);
    /* Diamond diagonals */
    f32 d = s * 0.7f;
    glVertex3f(x - d, y + d, z); glVertex3f(x + d, y - d, z);
    glVertex3f(x - d, y - d, z); glVertex3f(x + d, y + d, z);
    glVertex3f(x, y + d, z - d); glVertex3f(x, y - d, z + d);
    glVertex3f(x, y - d, z - d); glVertex3f(x, y + d, z + d);
    glEnd();
    if (s_ov_hl) ov_pin(x, y, z);
}

/* Fog volumes (0x3D): translucent sphere wireframe */
static void ov_draw_fog(const RdtSceneOverlay& ov, int i) {
    ov_width(1.0f);
    ov_color(0.5f, 0.6f, 0.8f, 0.4f);  /* pale blue */
    const RdtFogParams& fp = ov.fog[i];
    f32 r_far = (f32)fp.dist_far;
    if (r_far < 100) r_far = 2000;
    /* Draw sphere rings at fog far distance */
    f32 cx = 0, cy = -(f32)fp.dist_near, cz = 0;
    /* Horizontal ring */
    glBegin(GL_LINE_LOOP);
    for (int a = 0; a < 32; a++) {
        f32 ang = (f32)a * 6.2832f / 32.0f;
        glVertex3f(cx + r_far * cosf(ang), cy, cz + r_far * sinf(ang));
    }
    glEnd();
    /* Vertical ring */
    glBegin(GL_LINE_LOOP);
    for (int a = 0; a < 32; a++) {
        f32 ang = (f32)a * 6.2832f / 32.0f;
        glVertex3f(cx + r_far * cosf(ang), cy + r_far * sinf(ang), cz);
    }
    glEnd();
}

/* One overlay element: type is an OverlayVis type, i its index (cam_id
   for cameras) */
static void ov_draw(const RdtSceneOverlay& ov, int type, int i) {
    switch (type) {
    case OverlayVis::TRIGGERS:
    case OverlayVis::FLOOR_ZONES: if (i < ov.n_zones) ov_draw_zone(ov, i); break;
    case OverlayVis::RECTS:       if (i < ov.n_collisions) ov_draw_rect(ov, i);
                                  else if (i - ov.n_collisions < ov.n_polys) ov_draw_poly(ov, i - ov.n_collisions);
                                  break;
    case OverlayVis::SCRIPT_CAMS: if (i < ov.n_cameras) ov_draw_script_cam(ov, i); break;
    case OverlayVis::SPAWNS:      if (i < ov.n_spawns) ov_draw_spawn(ov, i); break;
    case OverlayVis::CHARS:       if (i < ov.n_chars) ov_draw_char(ov, i); break;
    case OverlayVis::CAM_CUTS:    if (i < ov.n_camcuts) ov_draw_camcut(ov, i); break;
    case OverlayVis::LIGHTS:      if (i < ov.n_lights) ov_draw_light(ov, i); break;
    case OverlayVis::ENEMIES:     if (i < ov.n_items) ov_draw_enemy(ov, i); break;
    case OverlayVis::CAMERAS:     ov_draw_camera(ov, i); break;
    case OverlayVis::OBJECTS:     if (i < ov.n_enemies) ov_draw_object(ov, i); break;
    case OverlayVis::FOG:         if (i < ov.n_fog) ov_draw_fog(ov, i); break;
    }
}

static void build_overlay_list(int list_id, const RdtSceneOverlay& ov, const OverlayVis& vis) {
    glNewList(list_id, GL_COMPILE);

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    s_ov_hl = false;

    /* Drawn in this order, later types on top (no depth test) */
    for (int i = 0; i < ov.n_zones; i++) {
        int t = ov.zones[i].kind == RDT_ZONE_TRIGGER ? OverlayVis::TRIGGERS : OverlayVis::FLOOR_ZONES;
        if (vis.shown(t, i)) ov_draw(ov, t, i);
    }
    static const int order[] = {
        OverlayVis::RECTS, OverlayVis::SCRIPT_CAMS, OverlayVis::SPAWNS, OverlayVis::CHARS,
        OverlayVis::CAM_CUTS, OverlayVis::LIGHTS, OverlayVis::ENEMIES, OverlayVis::CAMERAS,
        OverlayVis::OBJECTS, OverlayVis::FOG,
    };
    for (int k = 0; k < (int)(sizeof(order) / sizeof(order[0])); k++) {
        int t = order[k];
        if (!vis.on[t]) continue;
        if (t == OverlayVis::CAMERAS) {
            /* Each camera once, in the order of its first zone */
            for (int i = 0; i < ov.n_cam_threads; i++) {
                int id = ov.cam_threads[i].cam_id;
                bool first = true;
                for (int j = 0; j < i && first; j++) first = ov.cam_threads[j].cam_id != id;
                if (first && vis.shown(t, id)) ov_draw(ov, t, id);
            }
            continue;
        }
        int n = t == OverlayVis::RECTS ? ov.n_collisions + ov.n_polys : t == OverlayVis::SCRIPT_CAMS ? ov.n_cameras :
                t == OverlayVis::SPAWNS ? ov.n_spawns : t == OverlayVis::CHARS ? ov.n_chars :
                t == OverlayVis::CAM_CUTS ? ov.n_camcuts : t == OverlayVis::LIGHTS ? ov.n_lights :
                t == OverlayVis::ENEMIES ? ov.n_items : t == OverlayVis::OBJECTS ? ov.n_enemies : ov.n_fog;
        for (int i = 0; i < n; i++)
            if (vis.shown(t, i)) ov_draw(ov, t, i);
    }

    /* The element selected in the tree, on top */
    if (vis.sel_type >= 0 && vis.sel_item >= 0 && vis.shown(vis.sel_type, vis.sel_item)) {
        s_ov_hl = true;
        ov_draw(ov, vis.sel_type, vis.sel_item);
        s_ov_hl = false;
    }

    glLineWidth(1.0f);
    glPointSize(1.0f);
    glEndList();
}

/* Compute bounding box from face-referenced vertices only */
/* Frame the camera on the mesh.  Outdoor rooms have sky and sea planes
   reaching +/-32000 units; framing those would shrink the walkable area to a
   speck, so faces entirely within FRAME_LIMIT are framed when there are any. */
#define FRAME_LIMIT 15000.0f
static void mesh_bounds(const Mesh& mesh, float* cx, float* cy, float* cz, float* dist) {
    float mnx=1e9f, mxx=-1e9f, mny=1e9f, mxy=-1e9f, mnz=1e9f, mxz=-1e9f;
    for (int pass = 0; pass < 2 && mnx > mxx; pass++) {
        for (int i = 0; i < mesh.tri_count; i++) {
            if (mesh.tris[i].alt) continue;
            bool in_frame = true;
            for (int vi = 0; vi < 3 && pass == 0; vi++) {
                int idx = mesh.tris[i].idx[vi];
                if (idx < 0 || idx >= mesh.vert_count) continue;
                const MeshVert& v = mesh.verts[idx];
                if (fabsf(v.x) > FRAME_LIMIT || fabsf(v.y) > FRAME_LIMIT ||
                    fabsf(v.z) > FRAME_LIMIT) in_frame = false;
            }
            if (!in_frame) continue;
            for (int vi = 0; vi < 3; vi++) {
                int idx = mesh.tris[i].idx[vi];
                if (idx < 0 || idx >= mesh.vert_count) continue;
                const MeshVert& v = mesh.verts[idx];
                if (v.x < mnx) mnx = v.x;
                if (v.x > mxx) mxx = v.x;
                if (v.y < mny) mny = v.y;
                if (v.y > mxy) mxy = v.y;
                if (v.z < mnz) mnz = v.z;
                if (v.z > mxz) mxz = v.z;
            }
        }
    }
    if (mnx > mxx) { mnx = mxx = mny = mxy = mnz = mxz = 0; }
    *cx = (mnx+mxx)*0.5f; *cy = (mny+mxy)*0.5f; *cz = (mnz+mxz)*0.5f;
    float dx = mxx-mnx, dy = mxy-mny, dz = mxz-mnz;
    *dist = sqrtf(dx*dx+dy*dy+dz*dz) * 1.2f;
    if (*dist < 100) *dist = 100;
}

/* Distance from the origin to the farthest vertex. */
static float mesh_reach(const Mesh& mesh) {
    float r2 = 0;
    for (int i = 0; i < mesh.vert_count; i++) {
        const MeshVert& v = mesh.verts[i];
        float d2 = v.x*v.x + v.y*v.y + v.z*v.z;
        if (d2 > r2) r2 = d2;
    }
    return sqrtf(r2);
}

/* Far clip for the orbit camera: cam_dist*20 keeps depth precision, but must
   still reach every vertex, which lies at most cam_dist + |target| + reach
   from the eye. */
static double orbit_far_clip(float cam_dist, float tx, float ty, float tz, float reach) {
    double far_clip = cam_dist * 20.0;
    if (far_clip < 10000.0) far_clip = 10000.0;
    double need = cam_dist + sqrt((double)tx*tx + (double)ty*ty + (double)tz*tz) + reach;
    if (far_clip < need) far_clip = need;
    return far_clip;
}

void ViewerPanel3D::build_mesh_lists(const Mesh& mesh) {
    /* clut_base_y is set by upload_archive_texture to match the atlas.
       If not set (single-texture path), compute from mesh faces. */
    if (num_pal_rows > 1 && clut_base_y == 0 && mesh.tri_count > 0) {
        int min_cy = 0x1FF;
        for (int i = 0; i < mesh.tri_count; i++) {
            int cy = (mesh.tris[i].clut >> 6) & 0x1FF;
            if (cy < min_cy) min_cy = cy;
        }
        clut_base_y = min_cy;
    }

    /* Build display lists split by semi-transparency mode (ABR).
       ABR is encoded in tpage bits 5-6:
         0 = opaque (or 50/50 STP with per-pixel flag)
         1 = additive (god rays, light shafts)
         2 = subtractive (shadows)
         3 = quarter-additive (subtle glow)
       Opaque faces → gl_list_id,  additive (ABR 1+3) → gl_blend_id,
       subtractive (ABR 2) → gl_sub_id. */
    /* See-through opaque faces go in their own list, drawn without culling */
    u8* see_through = 0;
    bool any_see_through = false;
    if (has_texture && uv_atlas_rgba && mesh.tri_count > 0) {
        see_through = (u8*)malloc(mesh.tri_count);
        if (see_through) {
            mark_see_through(mesh, see_through, uv_atlas_rgba, uv_atlas_w, uv_atlas_h,
                             tex_vram_x, tex_bpp, clut_base_y, tex_vram_y,
                             num_sub_pals, clut_base_x);
            for (int fi = 0; fi < mesh.tri_count && !any_see_through; fi++)
                any_see_through = see_through[fi] && !mesh.tris[fi].alt &&
                                  ((mesh.tris[fi].tpage >> 5) & 3) == 0;
        }
    }
    gl_list_id = glGenLists(1);
    build_solid_list(gl_list_id, mesh, tex_w, tex_h, tex_vram_x, tex_bpp,
                     num_pal_rows, clut_base_y, true, tex_vram_y, 1,
                     num_sub_pals, clut_base_x, 0, see_through, any_see_through ? 0 : -1);
    gl_2side_id = 0;
    if (any_see_through) {
        gl_2side_id = glGenLists(1);
        build_solid_list(gl_2side_id, mesh, tex_w, tex_h, tex_vram_x, tex_bpp,
                         num_pal_rows, clut_base_y, true, tex_vram_y, 1,
                         num_sub_pals, clut_base_x, 0, see_through, 1);
    }
    free(see_through);

    /* Scan for semi-transparent faces */
    bool has_additive = false, has_subtractive = false;
    for (int fi = 0; fi < mesh.tri_count; fi++) {
        int abr = (mesh.tris[fi].tpage >> 5) & 3;
        if (abr == 1 || abr == 3) has_additive = true;
        if (abr == 2) has_subtractive = true;
    }

    /* Additive blend list (ABR 1 = full additive, ABR 3 = quarter additive) */
    if (has_additive) {
        gl_blend_id = glGenLists(1);
        build_solid_list(gl_blend_id, mesh, tex_w, tex_h, tex_vram_x, tex_bpp,
                         num_pal_rows, clut_base_y, true, tex_vram_y, 1,
                         num_sub_pals, clut_base_x, 1);
    } else {
        gl_blend_id = 0;
    }

    /* Subtractive blend list (ABR 2) */
    if (has_subtractive) {
        gl_sub_id = glGenLists(1);
        build_solid_list(gl_sub_id, mesh, tex_w, tex_h, tex_vram_x, tex_bpp,
                         num_pal_rows, clut_base_y, true, tex_vram_y, 1,
                         num_sub_pals, clut_base_x, 2);
    } else {
        gl_sub_id = 0;
    }

    /* Unplaced sections (MeshTri.alt), shown with H */
    gl_alt_id = 0;
    for (int fi = 0; fi < mesh.tri_count; fi++) {
        if (!mesh.tris[fi].alt) continue;
        gl_alt_id = glGenLists(1);
        build_solid_list(gl_alt_id, mesh, tex_w, tex_h, tex_vram_x, tex_bpp,
                         num_pal_rows, clut_base_y, true, tex_vram_y, 2,
                         num_sub_pals, clut_base_x, -1);
        break;
    }
    gl_wire_id = glGenLists(1); build_wire_list(gl_wire_id, mesh);
    if (gl_normals_id) { glDeleteLists(gl_normals_id, 1); gl_normals_id = 0; }
    gl_normals_id = glGenLists(1); build_normals_list(gl_normals_id, mesh);
    gl_bone_id = 0;
}

/* K: the room as one game camera shows it.  The display lists are rebuilt
   without the model slots that camera's setup thread hides, and a camera
   with a shot puts the view where the game puts it. */
void ViewerPanel3D::select_camera(int sel) {
    if (!hRC || !has_mesh || is_emd || !im_tris || sel >= n_cams) return;
    cam_sel = sel;
    shot_locked = false;
    shot_kind = RDT_SHOT_NONE;
    shot_cur = -1;
    shot_zoom = 1;
    set_view_hide(sel >= 0 ? cam_list[sel].hide : 0);
    if (sel >= 0 && cam_list[sel].shot >= 0) {
        /* The player starts in the middle of the zone that switches the
           game to this shot, on the floor under it */
        int si = cam_list[sel].shot;
        const RdtSceneOverlay::CamThread& ct = cached_overlay.cam_threads[si];
        shot_player[0] = shot_player[2] = 0;
        for (int k = 0; k < 4; k++) {
            shot_player[0] += ct.x[k] * 0.25f;
            shot_player[2] += ct.z[k] * 0.25f;
        }
        shot_player[1] = -cached_overlay.cam_thread_y[si];
        shot_cur = si;
        update_shot(true);
    }
    render();
}

void ViewerPanel3D::set_view_hide(u64 hide) {
    if (hide == view_hide) return;
    view_hide = hide;
    Mesh m;
    m.verts = im_verts; m.vert_count = im_vert_count;
    m.tris = (MeshTri*)malloc((im_tri_count ? im_tri_count : 1) * sizeof(MeshTri));
    if (!m.tris) { m.verts = 0; return; }
    for (int i = 0; i < im_tri_count; i++)
        if (tri_in_view(im_tris[i])) m.tris[m.tri_count++] = im_tris[i];

    wglMakeCurrent(hDC, hRC);
    if (gl_list_id) glDeleteLists(gl_list_id, 1);
    if (gl_alt_id) glDeleteLists(gl_alt_id, 1);
    if (gl_2side_id) { glDeleteLists(gl_2side_id, 1); gl_2side_id = 0; }
    if (gl_wire_id) glDeleteLists(gl_wire_id, 1);
    if (gl_blend_id) glDeleteLists(gl_blend_id, 1);
    if (gl_sub_id) glDeleteLists(gl_sub_id, 1);
    if (gl_pick_id) { glDeleteLists(gl_pick_id, 1); gl_pick_id = 0; }
    pick_tri = -1; pick_section = 0; pick_group_count = 0; pick_info[0] = 0;
    build_mesh_lists(m);
    wglMakeCurrent(0, 0);
    m.verts = 0;  /* owned by im_verts */
}

/* Recomputes the shot in use for the player's position.  While the view is
   on the shot (or lock asks for it), the orbit camera is put on it: target
   at the orbit centre, eye at the orbit distance and angles, so dragging
   carries on from it. */
void ViewerPanel3D::update_shot(bool lock) {
    if (shot_cur < 0) return;
    shot_kind = rdt_camera_shot(cached_overlay.cam_threads[shot_cur].desc,
                                shot_player, shot_eye, shot_tgt);
    if (shot_kind == RDT_SHOT_NONE) return;
    if (lock) shot_locked = true;
    if (!shot_locked) return;
    cam_x = -shot_tgt[0]; cam_y = -shot_tgt[1]; cam_z = shot_tgt[2];   /* viewer: x and y negated */
    f32 dx = -shot_eye[0] - cam_x, dy = -shot_eye[1] - cam_y, dz = shot_eye[2] - cam_z;
    f32 d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d < 1) d = 1;
    cam_dist = d;
    cam_pitch = asinf(dy / d) * 180.0f / 3.14159265f;
    cam_yaw = atan2f(dx, dz) * 180.0f / 3.14159265f;
    cam_upx = 0; cam_upy = 1; cam_upz = 0;
}

/* WASD / arrows: walk the player along the view's heading and keep it on
   the floor.  Walking into another shot's trigger zone cuts to that shot,
   as the game does; the view follows the shot unless the mouse freed it. */
void ViewerPanel3D::move_shot_player(f32 fwd, f32 right) {
    if (cam_sel < 0 || shot_cur < 0) return;
    f32 yr = cam_yaw * 3.14159265f / 180.0f;
    f32 fx = -sinf(yr), fz = -cosf(yr);       /* eye to target, on the floor */
    f32 vx = -shot_player[0] + fx * fwd - fz * right;
    f32 vz =  shot_player[2] + fz * fwd + fx * right;
    f32 vy = floor_below(vx, vz, -shot_player[1]);
    shot_player[0] = -vx; shot_player[1] = -vy; shot_player[2] = vz;

    int z = shot_zone_at(shot_player);
    if (z >= 0 && z != shot_cur) {
        const u8* d = cached_overlay.cam_threads[z].desc;
        for (int k = 0; k < n_cams; k++) {
            if (cam_list[k].shot < 0) continue;
            const u8* o = cached_overlay.cam_threads[cam_list[k].shot].desc;
            if (o[0] != d[0] || o[2] != d[2] || memcmp(o + 0x24, d + 0x24, 0x20) != 0) continue;
            if (k != cam_sel) { cam_sel = k; set_view_hide(cam_list[k].hide); }
            break;
        }
        shot_cur = z;
    }
    update_shot(false);
    render();
}

/* The placed shot whose trigger zone holds p (raw coordinates) at about
   its height; the shot in use while p is still inside its zone. */
int ViewerPanel3D::shot_zone_at(const f32 p[3]) const {
    int found = -1;
    for (int i = 0; i < cached_overlay.n_cam_threads; i++) {
        const RdtSceneOverlay::CamThread& ct = cached_overlay.cam_threads[i];
        if (fabsf(cached_overlay.cam_thread_y[i] + p[1]) > 1500) continue;
        bool in = false;
        for (int a = 0, b = 3; a < 4; b = a++)
            if ((ct.z[a] > p[2]) != (ct.z[b] > p[2]) &&
                p[0] < ct.x[b] + (f32)(ct.x[a] - ct.x[b]) * (p[2] - ct.z[b]) / (f32)(ct.z[a] - ct.z[b]))
                in = !in;
        if (!in) continue;
        if (i == shot_cur) return i;
        f32 e[3], t[3];
        if (found < 0 && rdt_camera_shot(ct.desc, p, e, t) != RDT_SHOT_NONE) found = i;
    }
    return found;
}

/* Timer while WASD / arrows are held: walk at about the game's pace
   (Shift runs), relative to the view's heading. */
void ViewerPanel3D::shot_tick() {
    f32 fwd = 0, right = 0;
    if (GetFocus() == hwnd) {
        if ((GetAsyncKeyState('W') | GetAsyncKeyState(VK_UP))    & 0x8000) fwd += 1;
        if ((GetAsyncKeyState('S') | GetAsyncKeyState(VK_DOWN))  & 0x8000) fwd -= 1;
        if ((GetAsyncKeyState('D') | GetAsyncKeyState(VK_RIGHT)) & 0x8000) right += 1;
        if ((GetAsyncKeyState('A') | GetAsyncKeyState(VK_LEFT))  & 0x8000) right -= 1;
    }
    if ((fwd == 0 && right == 0) || shot_cur < 0) {
        KillTimer(hwnd, 9003);
        shot_moving = false;
        return;
    }
    DWORD now = GetTickCount();
    f32 dt = (now - shot_tick_ms) / 1000.0f;
    shot_tick_ms = now;
    if (dt > 0.1f) dt = 0.1f;
    f32 v = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 4500.0f : 1800.0f;
    move_shot_player(fwd * v * dt, right * v * dt);
}

/* Highest upward face under (vx, vz) that is at most a step above vy;
   vy itself when there is none (viewer coordinates). */
f32 ViewerPanel3D::floor_below(f32 vx, f32 vz, f32 vy) const {
    f32 best = 0;
    bool found = false;
    for (int i = 0; i < im_tri_count; i++) {
        const MeshTri& t = im_tris[i];
        if (t.alt) continue;
        const MeshVert& a = im_verts[t.idx[0]];
        const MeshVert& b = im_verts[t.idx[1]];
        const MeshVert& c = im_verts[t.idx[2]];
        if ((b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z) <= 0) continue;  /* not upward */
        f32 det = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
        if (fabsf(det) < 1e-3f) continue;
        f32 u = ((vx - a.x) * (c.z - a.z) - (c.x - a.x) * (vz - a.z)) / det;
        f32 v = ((b.x - a.x) * (vz - a.z) - (vx - a.x) * (b.z - a.z)) / det;
        if (u < 0 || v < 0 || u + v > 1) continue;
        f32 y = a.y + u * (b.y - a.y) + v * (c.y - a.y);
        if (y > vy + 700) continue;
        if (!found || y > best) { best = y; found = true; }
    }
    return found ? best : vy;
}

/* Field of view: the game's (H=320 on a 320x240 screen) while the view is
   on a shot, fitted so the whole 4:3 frame shows, times the scroll zoom. */
double ViewerPanel3D::view_fovy(double aspect) const {
    if (!shot_locked || cam_sel < 0) return 45.0;
    double half = 120.0 / 320.0;
    if (aspect < 4.0 / 3.0) half = (160.0 / 320.0) / aspect;
    return 2.0 * atan(half * shot_zoom) * 180.0 / 3.14159265;
}

/* The shot's trigger zone and the player, then either the game's 4:3
   frame (outside it dimmed) while the view is on the shot, or the game
   camera's view pyramid once the mouse has freed the view.  Called with
   the scene's matrices current. */
void ViewerPanel3D::draw_shot_overlay(int w, int vh) {
    if (cam_sel < 0 || shot_cur < 0) return;
    const RdtSceneOverlay::CamThread& ct = cached_overlay.cam_threads[shot_cur];
    glPushAttrib(GL_ENABLE_BIT | GL_LINE_BIT | GL_POINT_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT);
    glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);

    f32 zy = cached_overlay.cam_thread_y[shot_cur] + 8;
    glLineWidth(2.0f);
    glColor3ub(0, 230, 255);
    glBegin(GL_LINE_LOOP);
    for (int k = 0; k < 4; k++) glVertex3f(-(f32)ct.x[k], zy, (f32)ct.z[k]);
    glEnd();

    f32 px = -shot_player[0], py = -shot_player[1], pz = shot_player[2];
    glColor3ub(255, 140, 0);
    glBegin(GL_LINES);
    glVertex3f(px, py, pz);           glVertex3f(px, py + 1700, pz);
    glVertex3f(px - 250, py + 5, pz); glVertex3f(px + 250, py + 5, pz);
    glVertex3f(px, py + 5, pz - 250); glVertex3f(px, py + 5, pz + 250);
    glEnd();
    glPointSize(8.0f);
    glBegin(GL_POINTS); glVertex3f(px, py + 1700, pz); glEnd();

    if (!shot_locked && shot_kind != RDT_SHOT_NONE) {
        /* Eye, the pyramid out to the target's distance, and the target */
        f32 E[3] = { -shot_eye[0], -shot_eye[1], shot_eye[2] };
        f32 T[3] = { -shot_tgt[0], -shot_tgt[1], shot_tgt[2] };
        f32 f[3] = { T[0] - E[0], T[1] - E[1], T[2] - E[2] };
        f32 d = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (d > 1) {
            for (int k = 0; k < 3; k++) f[k] /= d;
            f32 r[3] = { -f[2], 0, f[0] };            /* f x up */
            f32 rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
            if (rl < 1e-4f) { r[0] = 1; r[2] = 0; rl = 1; }
            r[0] /= rl; r[2] /= rl;
            f32 u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
            f32 c[4][3];
            static const f32 sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
            for (int k = 0; k < 4; k++)
                for (int j = 0; j < 3; j++)
                    c[k][j] = E[j] + f[j] * d + r[j] * sx[k] * 0.5f * d + u[j] * sy[k] * 0.375f * d;
            glColor3ub(255, 230, 60);
            glBegin(GL_LINES);
            for (int k = 0; k < 4; k++) { glVertex3fv(E); glVertex3fv(c[k]); }
            for (int j = 0; j < 3; j += 2) {
                f32 a[3] = { E[0], E[1], E[2] }, b[3] = { E[0], E[1], E[2] };
                a[j] -= 150; b[j] += 150;
                glVertex3fv(a); glVertex3fv(b);
            }
            glEnd();
            glBegin(GL_LINE_LOOP);
            for (int k = 0; k < 4; k++) glVertex3fv(c[k]);
            glEnd();
            glBegin(GL_POINTS); glVertex3fv(E); glVertex3fv(T); glEnd();
        }
    }

    if (shot_locked) {
        f32 fw = (f32)w, fh = (f32)vh;
        if (fw * 3 > fh * 4) fw = fh * 4 / 3; else fh = fw * 3 / 4;
        fw /= shot_zoom; fh /= shot_zoom;
        f32 x0 = (w - fw) / 2, y0 = (vh - fh) / 2, x1 = x0 + fw, y1 = y0 + fh;
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0, w, 0, vh, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0, 0, 0, 0.55f);
        glBegin(GL_QUADS);
        glVertex2f(0, 0);   glVertex2f(x0, 0);         glVertex2f(x0, (f32)vh);        glVertex2f(0, (f32)vh);
        glVertex2f(x1, 0);  glVertex2f((f32)w, 0);     glVertex2f((f32)w, (f32)vh);    glVertex2f(x1, (f32)vh);
        glVertex2f(x0, 0);  glVertex2f(x1, 0);         glVertex2f(x1, y0);             glVertex2f(x0, y0);
        glVertex2f(x0, y1); glVertex2f(x1, y1);        glVertex2f(x1, (f32)vh);        glVertex2f(x0, (f32)vh);
        glEnd();
        glLineWidth(1.0f);
        glColor4f(1.0f, 0.9f, 0.25f, 0.7f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1);
        glEnd();
        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
    }
    glPopAttrib();
}

void ViewerPanel3D::set_mesh(const Mesh& mesh) {
    if (!hRC) return;

    /* Clear any animation state from previous EMD */
    if (anim_model) {
        anim_model = 0;
        anim_playing = false;
        if (anim_timer_id) { KillTimer(hwnd, anim_timer_id); anim_timer_id = 0; }
        if (anim_track) { DestroyWindow(anim_track); anim_track = 0; }
        if (anim_label) { DestroyWindow(anim_label); anim_label = 0; }
        if (anim_clip_combo) { DestroyWindow(anim_clip_combo); anim_clip_combo = 0; }
        /* Restore full viewport (no control bar) */
        RECT rc; GetClientRect(hwnd, &rc);
        resize(rc.right, rc.bottom);
    }

    wglMakeCurrent(hDC, hRC);
    if (has_mesh) {
        if (gl_list_id) glDeleteLists(gl_list_id, 1);
        if (gl_alt_id) glDeleteLists(gl_alt_id, 1);
        if (gl_2side_id) { glDeleteLists(gl_2side_id, 1); gl_2side_id = 0; }
        if (gl_wire_id) glDeleteLists(gl_wire_id, 1);
        if (gl_bone_id) glDeleteLists(gl_bone_id, 1);
        if (gl_blend_id) glDeleteLists(gl_blend_id, 1);
        if (gl_sub_id) glDeleteLists(gl_sub_id, 1);
        if (gl_normals_id) glDeleteLists(gl_normals_id, 1);
        gl_normals_id = 0;
        if (gl_overlay_id) glDeleteLists(gl_overlay_id, 1);
        gl_overlay_id = 0;
        if (gl_pick_id) glDeleteLists(gl_pick_id, 1);
        gl_pick_id = 0; pick_tri = -1; pick_section = 0; pick_info[0] = 0;
    }
    cam_sel = -1; n_cams = 0; view_hide = 0; shot_locked = false; shot_kind = 0; shot_cur = -1; shot_zoom = 1;
    build_mesh_lists(mesh);

    /* Cache mesh for immediate-mode drawing (VM fallback) */
    free(im_verts); free(im_tris);
    im_vert_count = mesh.vert_count;
    im_tri_count = mesh.tri_count;
    im_verts = (MeshVert*)malloc(im_vert_count * sizeof(MeshVert));
    im_tris = (MeshTri*)malloc(im_tri_count * sizeof(MeshTri));
    if (im_verts) memcpy(im_verts, mesh.verts, im_vert_count * sizeof(MeshVert));
    if (im_tris) memcpy(im_tris, mesh.tris, im_tri_count * sizeof(MeshTri));
    im_flip_normals = true;

    has_mesh = true; is_emd = false; has_skeleton = false;
    show_lighting = false;  /* static meshes: flat shading by default */
    tri_count = mesh.tri_count; vert_count = mesh.vert_count; bone_count = 0;

    /* Exit walk mode on model switch */
    if (walk_mode) {
        walk_mode = false;
        memset(walk_keys, 0, sizeof(walk_keys));
        KillTimer(hwnd, 9002);
        ReleaseCapture();
        ShowCursor(TRUE);
    }

    /* Cache tris + verts for UV editor */
    free(uv_tris); uv_tris = 0; uv_tri_count = 0;
    free(uv_verts); uv_verts = 0; uv_vert_count = 0;
    free(sel_mask); sel_mask = 0; sel_count = 0;
    if (gl_sel_id) { glDeleteLists(gl_sel_id, 1); gl_sel_id = 0; }
    if (mesh.tris && mesh.tri_count > 0) {
        uv_tris = (MeshTri*)malloc(mesh.tri_count * sizeof(MeshTri));
        if (uv_tris) {
            memcpy(uv_tris, mesh.tris, mesh.tri_count * sizeof(MeshTri));
            uv_tri_count = mesh.tri_count;
        }
        sel_mask = (bool*)calloc(mesh.tri_count, sizeof(bool));
    }
    if (mesh.verts && mesh.vert_count > 0) {
        uv_verts = (MeshVert*)malloc(mesh.vert_count * sizeof(MeshVert));
        if (uv_verts) {
            memcpy(uv_verts, mesh.verts, mesh.vert_count * sizeof(MeshVert));
            uv_vert_count = mesh.vert_count;
        }
    }

    mesh_bounds(mesh, &cam_x, &cam_y, &cam_z, &cam_dist);
    scene_reach = mesh_reach(mesh);
    cam_yaw = 180; cam_pitch = 10; cam_upx = 0; cam_upy = 1; cam_upz = 0;
    wglMakeCurrent(0, 0);
    render();
}

void ViewerPanel3D::set_overlay(const RdtSceneOverlay& overlay) {
    if (!hRC) return;
    cached_overlay = overlay;  /* keep copy for export */
    /* K list: every shot that places the camera (a camera's identical
       shots for different trigger zones once), then cameras with no shot
       that still hide slots; in camera order */
    n_cams = 0;
    for (int i = 0; i < overlay.n_cam_threads && n_cams < 64; i++) {
        const RdtSceneOverlay::CamThread& ct = overlay.cam_threads[i];
        f32 p[3] = {0, 0, 0}, e[3], t[3];
        if (rdt_camera_shot(ct.desc, p, e, t) == RDT_SHOT_NONE) continue;
        bool dup = false;
        for (int k = 0; k < n_cams && !dup; k++) {
            const u8* o = overlay.cam_threads[cam_list[k].shot].desc;
            dup = o[0] == ct.desc[0] && o[2] == ct.desc[2] && memcmp(o + 0x24, ct.desc + 0x24, 0x20) == 0;
        }
        if (dup) continue;
        CamEntry& ce = cam_list[n_cams++];
        ce.shot = i; ce.cam_id = ct.cam_id; ce.hide = 0;
    }
    for (int v = 0; v < overlay.n_cam_views; v++) {
        bool has = false;
        for (int k = 0; k < n_cams; k++)
            if (cam_list[k].cam_id == overlay.cam_views[v].cam_id) {
                cam_list[k].hide = overlay.cam_views[v].hide_mask;
                has = true;
            }
        if (!has && n_cams < 64) {
            CamEntry& ce = cam_list[n_cams++];
            ce.shot = -1; ce.cam_id = overlay.cam_views[v].cam_id; ce.hide = overlay.cam_views[v].hide_mask;
        }
    }
    for (int i = 1; i < n_cams; i++)
        for (int j = i; j > 0 && cam_list[j].cam_id < cam_list[j - 1].cam_id; j--) {
            CamEntry tmp = cam_list[j]; cam_list[j] = cam_list[j - 1]; cam_list[j - 1] = tmp;
        }
    /* Every element of the new room is shown; the types hidden stay hidden */
    memset(ov_vis.hidden, 0, sizeof(ov_vis.hidden));
    ov_vis.sel_type = ov_vis.sel_item = -1;
    fill_ov_tree();
    wglMakeCurrent(hDC, hRC);
    if (gl_overlay_id) glDeleteLists(gl_overlay_id, 1);
    gl_overlay_id = glGenLists(1);
    build_overlay_list(gl_overlay_id, cached_overlay, ov_vis);
    /* Cache collision rects for walk-through collision */
    n_walk_cols = (overlay.n_collisions < MAX_WALK_COLS) ? overlay.n_collisions : MAX_WALK_COLS;
    for (int i = 0; i < n_walk_cols; i++) walk_cols[i] = overlay.collisions[i];
    /* Cache spawn points for walk mode start position */
    n_walk_chars = (overlay.n_chars < 16) ? overlay.n_chars : 16;
    for (int i = 0; i < n_walk_chars; i++) walk_chars[i] = overlay.chars[i];
    n_walk_spawns = (overlay.n_spawns < 64) ? overlay.n_spawns : 64;
    for (int i = 0; i < n_walk_spawns; i++) walk_spawns[i] = overlay.spawns[i];
    /* Cache scene lights from SCD 0x3A */
    n_room_lights = (overlay.n_lights < 8) ? overlay.n_lights : 8;
    for (int i = 0; i < n_room_lights; i++) room_lights[i] = overlay.lights[i];
    room_ambient[0] = overlay.ambient_r;
    room_ambient[1] = overlay.ambient_g;
    room_ambient[2] = overlay.ambient_b;
    /* Auto-enable room lights if any were found with position data */
    if (n_room_lights > 0) {
        bool any_pos = false;
        for (int i = 0; i < n_room_lights; i++)
            if (room_lights[i].has_pos) { any_pos = true; break; }
        use_room_lights = any_pos;
    } else {
        use_room_lights = false;
    }
    walk_grid_build();  /* rasterize rects into O(1) lookup grid */
    wglMakeCurrent(0, 0);
    render();
}

/*─── Overlay element tree ───────────────────────────────────────
   A checkbox per element type (hides all of it) and, under it, one per
   instance.  Item lParam = type * 4096 + instance + 1 (instance -1 for a
   type's own row).  Selecting an instance highlights it in the view. */
#define OV_TREE_W 290
#define WM_OV_TREE_CHECK (WM_APP + 31)   /* lParam = HTREEITEM whose checkbox changed */

static HTREEITEM ov_tree_add(HWND tv, HTREEITEM parent, const char* text,
                             int type, int item, bool checked) {
    TVINSERTSTRUCTA tvi;
    memset(&tvi, 0, sizeof(tvi));
    tvi.hParent = parent;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
    tvi.item.pszText = (LPSTR)text;
    tvi.item.lParam = (LPARAM)(type * 4096 + item + 1);
    tvi.item.stateMask = TVIS_STATEIMAGEMASK;
    tvi.item.state = INDEXTOSTATEIMAGEMASK(checked ? 2 : 1);
    return (HTREEITEM)SendMessageA(tv, TVM_INSERTITEMA, 0, (LPARAM)&tvi);
}

static void ov_tree_theme(HWND tv) {
    typedef HRESULT (WINAPI* SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);
    static SetWindowThemeFn set_theme = 0;
    static bool looked = false;
    if (!looked) {
        looked = true;
        HMODULE ux = LoadLibraryA("uxtheme.dll");
        if (ux) set_theme = (SetWindowThemeFn)(void*)GetProcAddress(ux, "SetWindowTheme");
    }
    TreeView_SetBkColor(tv, T.listBg);
    TreeView_SetTextColor(tv, T.listText);
    if (set_theme) set_theme(tv, T.isDark ? L"DarkMode_Explorer" : L"Explorer", NULL);
}

void ViewerPanel3D::fill_ov_tree() {
    if (!hwnd) return;
    if (!ov_tree) {
        ov_tree = CreateWindowExA(0, WC_TREEVIEWA, "",
            WS_CHILD | WS_VSCROLL | WS_CLIPSIBLINGS | TVS_HASBUTTONS | TVS_LINESATROOT |
            TVS_SHOWSELALWAYS | TVS_FULLROWSELECT,
            0, 0, OV_TREE_W, 100, hwnd, 0, GetModuleHandle(0), 0);
        if (!ov_tree) return;
        /* Checkboxes set after creation, before any item, as the control asks */
        SetWindowLongA(ov_tree, GWL_STYLE, GetWindowLongA(ov_tree, GWL_STYLE) | TVS_CHECKBOXES);
        SendMessageA(ov_tree, WM_SETFONT, (WPARAM)g_app.hFontUI, TRUE);
        ov_tree_dark = -1;
    }
    SendMessageA(ov_tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(ov_tree);

    static const char* names_dc1[OverlayVis::N_TYPES] = {
        "Cameras", "Script cameras (0x4C/0x2E)", "Camera cut zones", "Trigger zones (0x28)",
        "Floor zones", "Collision rects", "Enemy spawns (0x5B)", "Characters (0x20)",
        "Objects (0x42)", "Object spawns", "Lights (0x3A)", "Fog (0x3D)",
    };
    /* A Dino Crisis 2 room fills the same lists from its own sections and
       opcodes (dc2_room_overlay) */
    static const char* names_dc2[OverlayVis::N_TYPES] = {
        "Cameras", "Script cameras", "Camera cut zones", "Doors, triggers, items (0x30/0x31/0x35)",
        "Zones (slot 7)", "Collision polygons (slot 2)", "Enemy spawns (0x1A)", "Characters",
        "Markers (slot 1)", "Object spawns", "Lights", "Fog",
    };
    const RdtSceneOverlay& ov = cached_overlay;
    const char* const* names = ov.dc2 ? names_dc2 : names_dc1;
    char buf[128];
    for (int t = 0; t < OverlayVis::N_TYPES; t++) {
        HTREEITEM h = ov_tree_add(ov_tree, TVI_ROOT, names[t], t, -1, ov_vis.on[t]);
        int n = 0;
#define OV_ITEM(idx) do { ov_tree_add(ov_tree, h, buf, t, (idx), true); n++; } while (0)
        switch (t) {
        case OverlayVis::CAMERAS: {
            /* Each camera number once, in order, with the zones that switch to it */
            bool seen[256] = {};
            for (int i = 0; i < ov.n_cam_threads; i++) seen[ov.cam_threads[i].cam_id] = true;
            for (int id = 0; id < 256; id++) {
                if (!seen[id]) continue;
                int zones = 0;
                bool fixed = false, tracking = false;
                for (int i = 0; i < ov.n_cam_threads; i++) {
                    const RdtSceneOverlay::CamThread& ct = ov.cam_threads[i];
                    if (ct.cam_id != id) continue;
                    zones++;
                    f32 p[3] = { 0, -ov.cam_thread_y[i], 0 }, e[3], g[3];
                    for (int k = 0; k < 4; k++) { p[0] += ct.x[k] * 0.25f; p[2] += ct.z[k] * 0.25f; }
                    int kind = rdt_camera_shot(ct.desc, p, e, g);
                    if (kind == RDT_SHOT_FIXED) fixed = true;
                    if (kind == RDT_SHOT_TRACKING) tracking = true;
                }
                _snprintf(buf, sizeof(buf) - 1, "Camera %d  (%s%d zone%s)", id,
                          fixed && tracking ? "fixed/tracking, " : fixed ? "fixed, " :
                          tracking ? "tracking, " : "", zones, zones == 1 ? "" : "s");
                OV_ITEM(id);
            }
        } break;
        case OverlayVis::SCRIPT_CAMS:
            for (int i = 0; i < ov.n_cameras; i++) {
                const RdtCameraEntry& c = ov.cameras[i];
                _snprintf(buf, sizeof(buf) - 1, "#%d  eye (%d, %d, %d)", i + 1, c.eye_x, c.eye_y, c.eye_z);
                OV_ITEM(i);
            }
            break;
        case OverlayVis::CAM_CUTS:
            for (int i = 0; i < ov.n_camcuts; i++) {
                _snprintf(buf, sizeof(buf) - 1, "#%d  group %d", i + 1, ov.camcuts[i].cam_group);
                OV_ITEM(i);
            }
            break;
        case OverlayVis::TRIGGERS:
        case OverlayVis::FLOOR_ZONES:
            for (int i = 0; i < ov.n_zones; i++) {
                const RdtFloorZone& z = ov.zones[i];
                if ((z.kind == RDT_ZONE_TRIGGER) != (t == OverlayVis::TRIGGERS)) continue;
                if (ov.dc2 && t == OverlayVis::FLOOR_ZONES)
                    _snprintf(buf, sizeof(buf) - 1, "#%d  (%d, %d)", n + 1, z.x[0], z.z[0]);
                else if (ov.dc2 && z.arg < 0)
                    _snprintf(buf, sizeof(buf) - 1, "#%d  %s, set by the script", n + 1,
                              z.target_index == DC2_ZONE_DOOR ? "Door" :
                              z.target_index == DC2_ZONE_ITEM ? "Item" : "Trigger");
                else if (ov.dc2 && z.target_index == DC2_ZONE_DOOR)
                    _snprintf(buf, sizeof(buf) - 1, "#%d  Door to ST%03X", n + 1, (unsigned)(u16)z.arg);
                else if (ov.dc2 && z.target_index == DC2_ZONE_ITEM)
                    _snprintf(buf, sizeof(buf) - 1, "#%d  Item 0x%02X", n + 1, (unsigned)(u16)z.arg);
                else if (ov.dc2)
                    _snprintf(buf, sizeof(buf) - 1, "#%d  Trigger, routine %d", n + 1, z.arg);
                else if (t == OverlayVis::TRIGGERS)
                    _snprintf(buf, sizeof(buf) - 1, "#%d  type %d%s", n + 1, z.target_index,
                              z.target_index == 4 ? " (item)" : "");
                else
                    _snprintf(buf, sizeof(buf) - 1, "#%d  step %d", n + 1, z.target_index);
                OV_ITEM(i);
            }
            break;
        case OverlayVis::RECTS:
            for (int i = 0; i < ov.n_collisions; i++) {
                const RdtCollisionRect& r = ov.collisions[i];
                _snprintf(buf, sizeof(buf) - 1, "#%d  (%d, %d)  %d x %d", i + 1, r.x, r.z, r.w, r.h);
                OV_ITEM(i);
            }
            for (int i = 0; i < ov.n_polys; i++) {
                _snprintf(buf, sizeof(buf) - 1, "#%d  %d points  flags 0x%02X", ov.polys[i].index,
                          ov.polys[i].count, ov.polys[i].flags);
                OV_ITEM(ov.n_collisions + i);
            }
            break;
        case OverlayVis::ENEMIES:
            for (int i = 0; i < ov.n_items; i++) {
                const RdtItemSpawn& e = ov.items[i];
                _snprintf(buf, sizeof(buf) - 1, "Slot %d  type %d  (%d, %d, %d)  %d\xB0", e.slot, e.type,
                          e.px, e.py, e.pz, ((e.rot_y & 0xFFF) * 360 + 2048) / 4096 % 360);
                OV_ITEM(i);
            }
            break;
        case OverlayVis::CHARS:
            for (int i = 0; i < ov.n_chars; i++) {
                const RdtCharPlace& c = ov.chars[i];
                _snprintf(buf, sizeof(buf) - 1, "Slot %d  type %d  (%d, %d, %d)  %d\xB0", c.slot, c.type,
                          c.px, c.py, c.pz, ((c.rot_y & 0xFFF) * 360 + 2048) / 4096 % 360);
                OV_ITEM(i);
            }
            break;
        case OverlayVis::OBJECTS:
            for (int i = 0; i < ov.n_enemies && i < OverlayVis::MAX_ITEMS; i++) {
                const RdtEnemySpawn& o = ov.enemies[i];
                _snprintf(buf, sizeof(buf) - 1, "Type %d  (%d, %d, %d)", o.type, o.px, o.py, o.pz);
                OV_ITEM(i);
            }
            break;
        case OverlayVis::SPAWNS:
            for (int i = 0; i < ov.n_spawns; i++) {
                const RdtObjectSpawn& s = ov.spawns[i];
                _snprintf(buf, sizeof(buf) - 1, "Slot %d  (%d, %d, %d)", s.slot, s.px, s.py, s.pz);
                OV_ITEM(i);
            }
            break;
        case OverlayVis::LIGHTS:
            for (int i = 0; i < ov.n_lights; i++) {
                const RdtSceneLight& l = ov.lights[i];
                if (!l.has_pos) continue;  /* not drawn */
                _snprintf(buf, sizeof(buf) - 1, "Slot %d  (%d, %d, %d)", l.slot, l.px, l.py, l.pz);
                OV_ITEM(i);
            }
            break;
        case OverlayVis::FOG:
            for (int i = 0; i < ov.n_fog; i++) {
                _snprintf(buf, sizeof(buf) - 1, "#%d  range %d to %d", i + 1, ov.fog[i].dist_near, ov.fog[i].dist_far);
                OV_ITEM(i);
            }
            break;
        }
#undef OV_ITEM
        if (n == 0) { TreeView_DeleteItem(ov_tree, h); continue; }
        _snprintf(buf, sizeof(buf) - 1, "%s  (%d)", names[t], n);
        TVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = TVIF_TEXT;
        it.hItem = h;
        it.pszText = buf;
        SendMessageA(ov_tree, TVM_SETITEMA, 0, (LPARAM)&it);
    }
    SendMessageA(ov_tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ov_tree, NULL, TRUE);
}

/* The tree sits at the right of the view while the overlay is drawn
   (not in walk mode); the view's viewport is narrowed by ov_tree_w. */
void ViewerPanel3D::layout_ov_tree() {
    if (!ov_tree || !hwnd) { ov_tree_w = 0; return; }
    RECT rc; GetClientRect(hwnd, &rc);
    int tw = OV_TREE_W;
    if (tw > rc.right / 2) tw = rc.right / 2;
    bool want = show_overlay && gl_overlay_id && has_mesh && !is_emd && !walk_mode && tw >= 120;
    if (!want) {
        if (IsWindowVisible(ov_tree)) ShowWindow(ov_tree, SW_HIDE);
        ov_tree_w = 0;
        return;
    }
    if (ov_tree_dark != (int)T.isDark) {
        ov_tree_theme(ov_tree);
        ov_tree_dark = (int)T.isDark;
    }
    RECT cur; GetWindowRect(ov_tree, &cur);
    MapWindowPoints(NULL, hwnd, (POINT*)&cur, 2);
    if (!IsWindowVisible(ov_tree) || cur.left != rc.right - tw || cur.right != rc.right ||
        cur.top != 0 || cur.bottom != rc.bottom)
        SetWindowPos(ov_tree, NULL, rc.right - tw, 0, tw, rc.bottom,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ov_tree_w = tw;
}

void ViewerPanel3D::ov_tree_checked(HTREEITEM item) {
    if (!ov_tree || !item) return;
    TVITEMA it;
    memset(&it, 0, sizeof(it));
    it.mask = TVIF_PARAM | TVIF_STATE;
    it.hItem = item;
    it.stateMask = TVIS_STATEIMAGEMASK;
    if (!SendMessageA(ov_tree, TVM_GETITEMA, 0, (LPARAM)&it) || it.lParam <= 0) return;
    bool checked = ((it.state & TVIS_STATEIMAGEMASK) >> 12) == 2;
    int type = (int)(it.lParam / 4096), idx = (int)(it.lParam % 4096) - 1;
    if (type >= OverlayVis::N_TYPES) return;
    if (idx < 0) ov_vis.on[type] = checked;
    else if (idx < OverlayVis::MAX_ITEMS) ov_vis.hidden[type][idx] = !checked;
    rebuild_overlay();
}

void ViewerPanel3D::ov_tree_selected(LPARAM lp) {
    int type = lp > 0 ? (int)(lp / 4096) : -1, idx = lp > 0 ? (int)(lp % 4096) - 1 : -1;
    if (idx < 0) type = -1;
    if (type == ov_vis.sel_type && idx == ov_vis.sel_item) return;
    ov_vis.sel_type = type;
    ov_vis.sel_item = idx;
    rebuild_overlay();
}

void ViewerPanel3D::rebuild_overlay() {
    if (!hRC || !gl_overlay_id) return;
    wglMakeCurrent(hDC, hRC);
    build_overlay_list(gl_overlay_id, cached_overlay, ov_vis);  /* replaces the list */
    wglMakeCurrent(0, 0);
    render();
}

void ViewerPanel3D::set_emd(const EmdModel& emd) {
    if (!hRC) return;
    cam_sel = -1; n_cams = 0; view_hide = 0; shot_locked = false; shot_kind = 0; shot_cur = -1; shot_zoom = 1;

    /* Exit walk mode on model switch */
    if (walk_mode) {
        walk_mode = false;
        memset(walk_keys, 0, sizeof(walk_keys));
        KillTimer(hwnd, 9002);
        ReleaseCapture();
        ShowCursor(TRUE);
    }

    /* Don't reset num_pal_rows / clut_base_y — upload_archive_texture() already
       configured them for the CLUT atlas.  EMD faces carry their own CLUT Y which
       maps to the correct atlas row through build_solid_list. */
    wglMakeCurrent(hDC, hRC);
    if (has_mesh) {
        if (gl_list_id) glDeleteLists(gl_list_id, 1);
        if (gl_alt_id) glDeleteLists(gl_alt_id, 1);
        if (gl_2side_id) { glDeleteLists(gl_2side_id, 1); gl_2side_id = 0; }
        if (gl_wire_id) glDeleteLists(gl_wire_id, 1);
        if (gl_bone_id) glDeleteLists(gl_bone_id, 1);
        if (gl_blend_id) glDeleteLists(gl_blend_id, 1);
        if (gl_sub_id) glDeleteLists(gl_sub_id, 1);
        if (gl_normals_id) glDeleteLists(gl_normals_id, 1);
        gl_normals_id = 0;
    }
    gl_blend_id = 0;
    gl_sub_id = 0;
    if (gl_overlay_id) { glDeleteLists(gl_overlay_id, 1); gl_overlay_id = 0; }
    if (gl_alt_id) { glDeleteLists(gl_alt_id, 1); gl_alt_id = 0; }
    if (gl_2side_id) { glDeleteLists(gl_2side_id, 1); gl_2side_id = 0; }
    if (gl_pick_id) { glDeleteLists(gl_pick_id, 1); gl_pick_id = 0; }
    pick_tri = -1; pick_section = 0; pick_info[0] = 0;
    gl_list_id = glGenLists(1); build_solid_list(gl_list_id, emd.mesh, tex_w, tex_h, tex_vram_x, tex_bpp, num_pal_rows, clut_base_y, false, tex_vram_y, 0, num_sub_pals, clut_base_x);
    gl_wire_id = glGenLists(1); build_wire_list(gl_wire_id, emd.mesh);
    if (gl_normals_id) { glDeleteLists(gl_normals_id, 1); gl_normals_id = 0; }
    gl_normals_id = glGenLists(1); build_normals_list(gl_normals_id, emd.mesh);
    gl_bone_id = glGenLists(1); build_bone_list(gl_bone_id, emd.skeleton);

    /* Cache mesh for immediate-mode drawing (VM fallback) */
    free(im_verts); free(im_tris);
    im_vert_count = emd.mesh.vert_count;
    im_tri_count = emd.mesh.tri_count;
    im_verts = (MeshVert*)malloc(im_vert_count * sizeof(MeshVert));
    im_tris = (MeshTri*)malloc(im_tri_count * sizeof(MeshTri));
    if (im_verts) memcpy(im_verts, emd.mesh.verts, im_vert_count * sizeof(MeshVert));
    if (im_tris) memcpy(im_tris, emd.mesh.tris, im_tri_count * sizeof(MeshTri));
    im_flip_normals = false;  /* EMD normals are already correct */

    has_mesh = true; is_emd = true; has_skeleton = true;
    show_lighting = true;  /* skinned meshes: lit by default */
    skeleton = emd.skeleton;
    tri_count = emd.mesh.tri_count;
    vert_count = emd.mesh.vert_count;
    bone_count = emd.skeleton.bone_count;

    /* Cache tris + verts for UV editor */
    free(uv_tris); uv_tris = 0; uv_tri_count = 0;
    free(uv_verts); uv_verts = 0; uv_vert_count = 0;
    free(sel_mask); sel_mask = 0; sel_count = 0;
    if (gl_sel_id) { glDeleteLists(gl_sel_id, 1); gl_sel_id = 0; }
    if (emd.mesh.tris && emd.mesh.tri_count > 0) {
        uv_tris = (MeshTri*)malloc(emd.mesh.tri_count * sizeof(MeshTri));
        if (uv_tris) {
            memcpy(uv_tris, emd.mesh.tris, emd.mesh.tri_count * sizeof(MeshTri));
            uv_tri_count = emd.mesh.tri_count;
        }
        sel_mask = (bool*)calloc(emd.mesh.tri_count, sizeof(bool));
    }
    if (emd.mesh.verts && emd.mesh.vert_count > 0) {
        uv_verts = (MeshVert*)malloc(emd.mesh.vert_count * sizeof(MeshVert));
        if (uv_verts) {
            memcpy(uv_verts, emd.mesh.verts, emd.mesh.vert_count * sizeof(MeshVert));
            uv_vert_count = emd.mesh.vert_count;
        }
    }

    mesh_bounds(emd.mesh, &cam_x, &cam_y, &cam_z, &cam_dist);
    scene_reach = mesh_reach(emd.mesh);
    cam_yaw = 180; cam_pitch = 10; cam_upx = 0; cam_upy = 1; cam_upz = 0;
    wglMakeCurrent(0, 0);
    render();
}

void ViewerPanel3D::clear_mesh() {
    /* Stop animation playback */
    if (anim_timer_id) { KillTimer(hwnd, anim_timer_id); anim_timer_id = 0; }
    anim_playing = false;
    anim_model = 0;
    if (anim_track)      { DestroyWindow(anim_track); anim_track = 0; }
    if (anim_label)      { DestroyWindow(anim_label); anim_label = 0; }
    if (anim_clip_combo) { DestroyWindow(anim_clip_combo); anim_clip_combo = 0; }

    if (hRC && has_mesh) {
        wglMakeCurrent(hDC, hRC);
        if (gl_list_id) glDeleteLists(gl_list_id, 1);
        if (gl_alt_id) glDeleteLists(gl_alt_id, 1);
        if (gl_2side_id) { glDeleteLists(gl_2side_id, 1); gl_2side_id = 0; }
        if (gl_wire_id) glDeleteLists(gl_wire_id, 1);
        if (gl_bone_id) glDeleteLists(gl_bone_id, 1);
        if (gl_sel_id)  glDeleteLists(gl_sel_id, 1);
        wglMakeCurrent(0, 0);
    }
    has_mesh = false; is_emd = false; has_skeleton = false;
    gl_list_id = 0; gl_wire_id = 0; gl_bone_id = 0; gl_sel_id = 0;
    gl_blend_id = 0; gl_sub_id = 0;
    tri_count = 0; vert_count = 0; bone_count = 0;

    /* Clear UV editor cache + selection */
    free(uv_tris); uv_tris = 0; uv_tri_count = 0;
    free(uv_verts); uv_verts = 0; uv_vert_count = 0;
    free(sel_mask); sel_mask = 0; sel_count = 0;
    free(uv_tex_rgba); uv_tex_rgba = 0; uv_tex_w = 0; uv_tex_h = 0;

    /* Clear immediate-mode mesh cache */
    free(im_verts); im_verts = 0; im_vert_count = 0;
    free(im_tris); im_tris = 0; im_tri_count = 0;

    /* Clear room lights */
    n_room_lights = 0;
    use_room_lights = false;
}

void ViewerPanel3D::show_uv_editor() {
    if (!has_mesh || !uv_tris || uv_tri_count < 1) return;
    ShowUVEditor(hwnd, uv_tris, uv_tri_count,
                 uv_tex_rgba, uv_tex_w, uv_tex_h, uv_tex_vram_x, tex_bpp,
                 uv_tex_vram_y, sel_mask, uv_tri_count,
                 uv_atlas_rgba, uv_atlas_w, uv_atlas_h,
                 num_pal_rows, uv_cache_clut_row,
                 uv_crop_px_x, uv_crop_px_w,
                 num_sub_pals);
    render();
}

void ViewerPanel3D::clear_selection() {
    if (sel_mask && uv_tri_count > 0)
        memset(sel_mask, 0, uv_tri_count * sizeof(bool));
    sel_count = 0;
}

void ViewerPanel3D::rebuild_sel_list() {
    /* Selection rendering is now inline in render().
       This just updates the count for HUD display. */
    sel_count = 0;
    if (!sel_mask || uv_tri_count < 1) return;
    for (int i = 0; i < uv_tri_count; i++)
        if (sel_mask[i]) sel_count++;
}

void ViewerPanel3D::clear_texture() {
    if (!hRC) return;
    wglMakeCurrent(hDC, hRC);
    if (gl_tex_id) { glDeleteTextures(1, &gl_tex_id); gl_tex_id = 0; }
    wglMakeCurrent(0, 0);
    free(uv_tex_rgba); uv_tex_rgba = 0;
    uv_tex_w = 0; uv_tex_h = 0;
    tex_w = 0; tex_h = 0;
    has_texture = false;
}

void ViewerPanel3D::set_texture(const u8* rgba, int w, int h) {
    if (!hRC || !rgba || w < 1 || h < 1) return;

    /* Cache one palette-row slice for UV editor, cropped to the tpage
       region the current mesh actually uses (uv_crop_x / uv_crop_w in pixels).
       When uv_crop_w == 0, cache the full width (room mesh case).
       
       Atlas layout: 2D sparse — each compact slice is padded_h=512 tall.
       For the UV editor default view, use 8bpp (sub=0) for the requested CLUT row. */
    int total_v_slices = num_sub_pals;
    if (total_v_slices < 1) total_v_slices = 1;
    int slice_h = h / total_v_slices;
    if (slice_h < 1) slice_h = h;

    int cache_row = uv_cache_clut_row;
    if (cache_row < 0 || cache_row >= num_pal_rows) cache_row = 0;
    /* Look up 8bpp slice (sub=0) for this CLUT row via 2D slice_map */
    int slice_idx = slice_map[cache_row * 65 + 0];
    if (slice_idx < 0) slice_idx = 0; /* fallback */
    size_t row_byte_offset = (size_t)slice_idx * slice_h * w * 4;

    int crop_x = uv_crop_px_x;
    int crop_w = uv_crop_px_w;
    if (crop_w <= 0 || crop_x < 0 || crop_x + crop_w > w) {
        crop_x = 0; crop_w = w;  /* full width */
    }

    free(uv_tex_rgba); uv_tex_rgba = 0;
    uv_tex_rgba = (u8*)malloc((size_t)crop_w * slice_h * 4);
    if (uv_tex_rgba) {
        /* Copy X-cropped columns from the correct CLUT row slice.
           Full slice height preserved — UVs reference absolute VRAM Y
           positions so the texture image must match the full VRAM layout. */
        const u8* src_row = rgba + row_byte_offset;
        for (int y = 0; y < slice_h; y++) {
            const u8* src_line = src_row + ((size_t)y * w + crop_x) * 4;
            u8* dst_line = uv_tex_rgba + (size_t)y * crop_w * 4;
            memcpy(dst_line, src_line, (size_t)crop_w * 4);
        }
        uv_tex_w = crop_w; uv_tex_h = slice_h;
    } else {
        uv_tex_w = 0; uv_tex_h = 0;
    }

    /* Cache the full atlas for UV editor palette cycling and GLB export. */
    free(uv_atlas_rgba); uv_atlas_rgba = 0;
    if (total_v_slices >= 1) {
        size_t asz = (size_t)w * h * 4;
        uv_atlas_rgba = (u8*)malloc(asz);
        if (uv_atlas_rgba) {
            memcpy(uv_atlas_rgba, rgba, asz);
            uv_atlas_w = w;
            uv_atlas_h = h;
        }
    } else {
        uv_atlas_w = 0;
        uv_atlas_h = 0;
    }

    wglMakeCurrent(hDC, hRC);
    if (gl_tex_id) glDeleteTextures(1, &gl_tex_id);

    glGenTextures(1, &gl_tex_id);
    glBindTexture(GL_TEXTURE_2D, gl_tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    tex_w = w; tex_h = h;
    has_texture = true;
    wglMakeCurrent(0, 0);
}

/* ─── Animation control methods ──────────────────────────────── */
void ViewerPanel3D::set_emd_anim(EmdModel* emd) {
    anim_model = emd;
    anim_clip = -1;   /* -1 = bind pose (no animation) */
    anim_frame = 0;
    anim_playing = false;
    if (anim_timer_id) { KillTimer(hwnd, anim_timer_id); anim_timer_id = 0; }

    /* Destroy old controls */
    if (anim_track)      { DestroyWindow(anim_track); anim_track = 0; }
    if (anim_label)      { DestroyWindow(anim_label); anim_label = 0; }
    if (anim_clip_combo) { DestroyWindow(anim_clip_combo); anim_clip_combo = 0; }

    if (!emd || emd->clip_count < 1) { anim_model = 0; return; }

    /* Get parent window size for positioning */
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    /* Fallback positions if panel not yet sized */
    if (w < 200) w = 600;
    if (h < 80) h = 400;

    /* Create clip selector combo box */
    anim_clip_combo = CreateWindowA("COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | CBS_DROPDOWNLIST | WS_VSCROLL,
        4, h - 58, 90, 300, hwnd, (HMENU)5001, GetModuleHandle(0), 0);
    /* First entry: bind pose (no animation) */
    SendMessageA(anim_clip_combo, CB_ADDSTRING, 0, (LPARAM)"Bind Pose");
    char buf[32];
    for (int i = 0; i < emd->clip_count; i++) {
        snprintf(buf, sizeof(buf), "Clip %d (%df)", i, emd->clips[i].frames);
        SendMessageA(anim_clip_combo, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessageA(anim_clip_combo, CB_SETCURSEL, 0, 0); /* select "Bind Pose" */

    /* Create trackbar (slider) — hidden initially for bind pose */
    anim_track = CreateWindowA(TRACKBAR_CLASSA, "",
        WS_CHILD | WS_CLIPSIBLINGS | TBS_AUTOTICKS,
        98, h - 58, w - 180, 24, hwnd, (HMENU)5002, GetModuleHandle(0), 0);
    SendMessageA(anim_track, TBM_SETRANGE, TRUE, MAKELPARAM(0, 0));
    SendMessageA(anim_track, TBM_SETPOS, TRUE, 0);

    /* Create frame label */
    anim_label = CreateWindowA("STATIC", "Bind Pose",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_CENTER,
        w - 78, h - 54, 74, 18, hwnd, (HMENU)5003, GetModuleHandle(0), 0);

    /* Apply dark theme to controls */
    ApplyDarkToControl(anim_clip_combo);
    ApplyDarkToControl(anim_track);
    ApplyDarkToControl(anim_label);

    /* Force viewport recalc to account for control bar */
    resize(w, h);

    /* Stay on bind pose — don't call anim_set_frame, mesh already has bind pose vertices */
}

void ViewerPanel3D::anim_set_frame(int frame) {
    if (!anim_model || anim_model->clip_count < 1) return;
    if (anim_clip < 0) return;  /* bind pose — no frame to set */
    if (anim_clip >= anim_model->clip_count) return;
    int max_f = anim_model->clips[anim_clip].frames;
    if (frame < 0) frame = 0;
    if (frame >= max_f) frame = max_f - 1;
    anim_frame = frame;

    /* Compute animated vertices */
    compute_anim_frame(*anim_model, anim_clip, anim_frame);

    /* Rebuild display lists */
    if (hRC) {
        wglMakeCurrent(hDC, hRC);
        if (gl_list_id) { glDeleteLists(gl_list_id, 1); gl_list_id = 0; }
        if (gl_alt_id)  { glDeleteLists(gl_alt_id, 1); gl_alt_id = 0; }
        if (gl_2side_id) { glDeleteLists(gl_2side_id, 1); gl_2side_id = 0; }
        if (gl_wire_id) { glDeleteLists(gl_wire_id, 1); gl_wire_id = 0; }
        if (gl_bone_id) { glDeleteLists(gl_bone_id, 1); gl_bone_id = 0; }
        gl_list_id = glGenLists(1); build_solid_list(gl_list_id, anim_model->mesh, tex_w, tex_h, tex_vram_x, tex_bpp, num_pal_rows, clut_base_y, false, tex_vram_y, 0, num_sub_pals, clut_base_x);
        gl_wire_id = glGenLists(1); build_wire_list(gl_wire_id, anim_model->mesh);
        if (gl_normals_id) { glDeleteLists(gl_normals_id, 1); gl_normals_id = 0; }
        gl_normals_id = glGenLists(1); build_normals_list(gl_normals_id, anim_model->mesh);
        gl_bone_id = glGenLists(1); build_bone_list(gl_bone_id, anim_model->skeleton);
        skeleton = anim_model->skeleton;
        wglMakeCurrent(0, 0);
    }

    /* Update trackbar + label */
    if (anim_track) SendMessageA(anim_track, TBM_SETPOS, TRUE, frame);
    if (anim_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d/%d", frame, max_f);
        SetWindowTextA(anim_label, buf);
    }
    render();
}

void ViewerPanel3D::anim_toggle_play() {
    if (!anim_model || anim_model->clip_count < 1) return;
    if (anim_clip < 0) return;  /* can't play bind pose */
    anim_playing = !anim_playing;
    if (anim_playing) {
        anim_timer_id = SetTimer(hwnd, 9001, 33, 0); /* ~30fps */
    } else {
        if (anim_timer_id) { KillTimer(hwnd, anim_timer_id); anim_timer_id = 0; }
    }
}

void ViewerPanel3D::anim_select_clip(int clip) {
    if (!anim_model) return;

    /* clip=-1 means bind pose (combo index 0 = "Bind Pose") */
    if (clip < 0 || clip >= anim_model->clip_count) {
        anim_clip = -1;
        anim_frame = 0;
        anim_playing = false;
        if (anim_timer_id) { KillTimer(hwnd, anim_timer_id); anim_timer_id = 0; }

        /* Hide trackbar, show "Bind Pose" label */
        if (anim_track) ShowWindow(anim_track, SW_HIDE);
        if (anim_label) SetWindowTextA(anim_label, "Bind Pose");

        /* Restore bind pose: recompute vertices from pool + static bone positions */
        if (anim_model->mesh.verts && anim_model->vert_src && anim_model->pool) {
            int n_parts2 = anim_model->parts_count;
            f32 abs_pos2[50][3];
            for (int bi = 0; bi < n_parts2; bi++) {
                int parent = anim_model->parts_info[bi].parent;
                f32 bx = (f32)anim_model->parts_info[bi].bx;
                f32 by = (f32)anim_model->parts_info[bi].by;
                f32 bz = (f32)anim_model->parts_info[bi].bz;
                if (parent == -1 || parent >= n_parts2) {
                    abs_pos2[bi][0] = bx; abs_pos2[bi][1] = by; abs_pos2[bi][2] = bz;
                } else {
                    abs_pos2[bi][0] = abs_pos2[parent][0] + bx;
                    abs_pos2[bi][1] = abs_pos2[parent][1] + by;
                    abs_pos2[bi][2] = abs_pos2[parent][2] + bz;
                }
            }
            for (int vi = 0; vi < anim_model->mesh.vert_count && vi < anim_model->vert_src_count; vi++) {
                const EmdModel::VertSrc& vs = anim_model->vert_src[vi];
                if (vs.pool_idx < 0 || vs.pool_idx >= anim_model->pool_count) continue;
                const EmdModel::PoolVert& pv = anim_model->pool[vs.pool_idx];
                int bidx = (int)pv.bone_idx;
                if (bidx >= n_parts2) bidx = 0;
                f32 lx = (f32)pv.x, ly = (f32)pv.y, lz = (f32)pv.z;
                if (vs.is_mirror) {
                    anim_model->mesh.verts[vi].x = (lx + abs_pos2[bidx][0]);
                } else {
                    anim_model->mesh.verts[vi].x = -(lx + abs_pos2[bidx][0]);
                }
                anim_model->mesh.verts[vi].y = -(ly + abs_pos2[bidx][1]);
                anim_model->mesh.verts[vi].z = -(lz + abs_pos2[bidx][2]);
            }
            /* Restore skeleton positions */
            for (int bi = 0; bi < n_parts2; bi++) {
                anim_model->skeleton.abs_pos[bi][0] = -abs_pos2[bi][0];
                anim_model->skeleton.abs_pos[bi][1] = -abs_pos2[bi][1];
                anim_model->skeleton.abs_pos[bi][2] = -abs_pos2[bi][2];
            }
        }

        /* Rebuild display lists with bind pose */
        if (hRC) {
            wglMakeCurrent(hDC, hRC);
            if (gl_list_id) { glDeleteLists(gl_list_id, 1); gl_list_id = 0; }
            if (gl_wire_id) { glDeleteLists(gl_wire_id, 1); gl_wire_id = 0; }
            if (gl_bone_id) { glDeleteLists(gl_bone_id, 1); gl_bone_id = 0; }
            gl_list_id = glGenLists(1); build_solid_list(gl_list_id, anim_model->mesh, tex_w, tex_h, tex_vram_x, tex_bpp, num_pal_rows, clut_base_y, false, tex_vram_y, 0, num_sub_pals, clut_base_x);
            gl_wire_id = glGenLists(1); build_wire_list(gl_wire_id, anim_model->mesh);
            if (gl_normals_id) { glDeleteLists(gl_normals_id, 1); gl_normals_id = 0; }
            gl_normals_id = glGenLists(1); build_normals_list(gl_normals_id, anim_model->mesh);
            gl_bone_id = glGenLists(1); build_bone_list(gl_bone_id, anim_model->skeleton);
            skeleton = anim_model->skeleton;
            wglMakeCurrent(0, 0);
        }
        render();
        return;
    }

    anim_clip = clip;
    int max_f = anim_model->clips[clip].frames;
    if (anim_track) {
        SendMessageA(anim_track, TBM_SETRANGE, TRUE, MAKELPARAM(0, max_f - 1));
        ShowWindow(anim_track, SW_SHOW);
    }
    anim_set_frame(0);
}

/* ── Immediate-mode mesh drawing (VM fallback, no display lists) ──
   Draws the cached mesh directly each frame. Slower than display lists
   but works on VM drivers that choke on compiled lists. */
static void draw_mesh_immediate(const MeshVert* verts, int nv,
                                const MeshTri* tris, int nt,
                                bool flip_normals, bool wire, bool show_alt = false)
{
    if (!verts || !tris || nt == 0) return;

    if (wire) {
        glBegin(GL_LINES);
        for (int i = 0; i < nt; i++) {
            const MeshTri& t = tris[i];
            if ((int)t.idx[0] >= nv || (int)t.idx[1] >= nv || (int)t.idx[2] >= nv) continue;
            if (t.alt) continue;
            for (int e = 0; e < 3; e++) {
                const MeshVert& va = verts[t.idx[e]];
                const MeshVert& vb = verts[t.idx[(e+1)%3]];
                glVertex3f(va.x, va.y, va.z);
                glVertex3f(vb.x, vb.y, vb.z);
            }
        }
        glEnd();
        return;
    }

    glBegin(GL_TRIANGLES);
    for (int i = 0; i < nt; i++) {
        const MeshTri& t = tris[i];
        if ((int)t.idx[0] >= nv || (int)t.idx[1] >= nv || (int)t.idx[2] >= nv) continue;
        if (t.alt && !show_alt) continue;
        const MeshVert& v0 = verts[t.idx[0]];
        const MeshVert& v1 = verts[t.idx[1]];
        const MeshVert& v2 = verts[t.idx[2]];

        /* Face normal for flat shading hint (even with lighting off,
           some drivers use it for two-sided color) */
        float ax = v1.x-v0.x, ay = v1.y-v0.y, az = v1.z-v0.z;
        float bx = v2.x-v0.x, by = v2.y-v0.y, bz = v2.z-v0.z;
        float fnx = ay*bz-az*by, fny = az*bx-ax*bz, fnz = ax*by-ay*bx;
        float flen = sqrtf(fnx*fnx+fny*fny+fnz*fnz);
        if (flen > 0.0001f) { fnx /= flen; fny /= flen; fnz /= flen; }
        if (flip_normals) { fnx = -fnx; fny = -fny; fnz = -fnz; }
        glNormal3f(fnx, fny, fnz);

        if (t.alt) glColor3ub(255, 96, 255);
        else       glColor3ub(t.color.r, t.color.g, t.color.b);
        glVertex3f(v0.x, v0.y, v0.z);
        glVertex3f(v1.x, v1.y, v1.z);
        glVertex3f(v2.x, v2.y, v2.z);
    }
    glEnd();
}

void ViewerPanel3D::render() {
    if (!hRC || !hwnd) return;
    layout_ov_tree();
    wglMakeCurrent(hDC, hRC);

    RECT rc; GetClientRect(hwnd, &rc);
    int w = view_w(rc.right), h = rc.bottom;
    int bar_h = anim_model ? 66 : 0;
    int vh = h - bar_h;
    if (w < 1) w = 1; if (vh < 1) vh = 1;
    glViewport(0, bar_h, w, vh);  /* the overlay tree can change the width */

    /* Theme-aware clear color */
    if (T.isDark) glClearColor(0.06f, 0.06f, 0.10f, 1.0f);
    else          glClearColor(0.82f, 0.83f, 0.85f, 1.0f);

    /* Scissor-restrict glClear to viewport area only (prevents bar flicker) */
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, bar_h, w, vh);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    /* Subtle gradient background (bottom → top) for modern viewport feel */
    {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0, 1, 0, 1, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glBegin(GL_QUADS);
        if (T.isDark) {
            glColor3f(0.04f, 0.04f, 0.07f); /* bottom - darker */
            glVertex2f(0, 0); glVertex2f(1, 0);
            glColor3f(0.10f, 0.10f, 0.16f); /* top - slightly lighter */
            glVertex2f(1, 1); glVertex2f(0, 1);
        } else {
            glColor3f(0.78f, 0.79f, 0.82f);
            glVertex2f(0, 0); glVertex2f(1, 0);
            glColor3f(0.88f, 0.89f, 0.92f);
            glVertex2f(1, 1); glVertex2f(0, 1);
        }
        glEnd();
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
        glEnable(GL_DEPTH_TEST);
    }

    /* Update projection with near/far scaled to camera distance.
       Fixed near=1 far=100000 causes z-fighting at distance because
       the 24-bit depth buffer can't resolve coplanar faces when the
       near/far ratio is 1:100000. Scaling near to ~0.5% of cam_dist
       keeps the ratio around 1:1000 regardless of zoom. */
    {
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        double aspect = (double)w / (double)vh;
        double near_clip, far_clip;
        if (walk_mode) {
            near_clip = 10.0;
            far_clip  = 200000.0;
        } else {
            near_clip = cam_dist * 0.005;
            if (near_clip < 0.5) near_clip = 0.5;
            far_clip = orbit_far_clip(cam_dist, cam_x, cam_y, cam_z, scene_reach);
        }
        gluPerspective(walk_mode ? 45.0 : view_fovy(aspect), aspect, near_clip, far_clip);
    }

    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    float yr, pr, ex, ey, ez;
    if (walk_mode) {
        /* First-person: eye at walk position, look along yaw/pitch */
        yr = walk_yaw * 3.14159f / 180.0f;
        pr = walk_pitch * 3.14159f / 180.0f;
        ex = walk_x;
        ey = walk_y;
        ez = walk_z;
        float tx = ex + cosf(pr) * sinf(yr);
        float ty = ey + sinf(pr);
        float tz = ez + cosf(pr) * cosf(yr);
        gluLookAt(ex, ey, ez, tx, ty, tz, 0, 1, 0);
    } else {
        yr = cam_yaw * 3.14159f / 180.0f; pr = cam_pitch * 3.14159f / 180.0f;
        ex = cam_x + cam_dist * cosf(pr) * sinf(yr);
        ey = cam_y + cam_dist * sinf(pr);
        ez = cam_z + cam_dist * cosf(pr) * cosf(yr);
        gluLookAt(ex, ey, ez, cam_x, cam_y, cam_z,
                  cam_upx, cam_upy, cam_upz);
    }

    /* Update lights — skip on VM/software (lighting disabled) */
    if (!sw_renderer && !in_vm) {
        if (use_room_lights && n_room_lights > 0) {
            /* ── Room lights from SCD 0x3A: positional in world space ── */
            /* Disable default lights first */
            glDisable(GL_LIGHT0); glDisable(GL_LIGHT1); glDisable(GL_LIGHT2);

            /* Set ambient from ptr[0] room data, scaled like PSX GTE (value*16/4096) */
            float ar = room_ambient[0] / 256.0f;
            float ag = room_ambient[1] / 256.0f;
            float ab = room_ambient[2] / 256.0f;
            float amb_model[] = {ar, ag, ab, 1.0f};
            glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb_model);

            for (int li = 0; li < n_room_lights && li < 8; li++) {
                const RdtSceneLight& rl = room_lights[li];
                if (!rl.has_pos) continue;
                GLenum light = GL_LIGHT0 + li;
                glEnable(light);
                /* Position: world-space, w=1 for positional light.
                   Must be set AFTER modelview is loaded so it's in world coords. */
                float p[] = {-(float)rl.px, -(float)rl.py, (float)rl.pz, 1.0f};
                glLightfv(light, GL_POSITION, p);
                /* PSX light color — scale to 0.5 max to avoid blowout from
                   multiple overlapping positional lights */
                float r = rl.has_color ? rl.r / 512.0f : 0.4f;
                float g = rl.has_color ? rl.g / 512.0f : 0.38f;
                float b = rl.has_color ? rl.b / 512.0f : 0.35f;
                float d[] = {r, g, b, 1.0f};
                float a[] = {0.0f, 0.0f, 0.0f, 1.0f};
                glLightfv(light, GL_DIFFUSE, d);
                glLightfv(light, GL_AMBIENT, a);
                /* Attenuation: match PSX hard cutoff at radius.
                   GL att = 1/(c + l*d + q*d²). At d=radius, want ~0.05. */
                float rad = (rl.radius > 0) ? (float)rl.radius : 2000.0f;
                glLightf(light, GL_CONSTANT_ATTENUATION, 1.0f);
                glLightf(light, GL_LINEAR_ATTENUATION, 0.0f);
                glLightf(light, GL_QUADRATIC_ATTENUATION, 19.0f / (rad * rad));
            }
        } else {
            /* ── Default headlamp: 3-point directional in eye space ── */
            float amb_model[] = {0.1f, 0.1f, 0.1f, 1.0f};
            glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb_model);
            glEnable(GL_LIGHT0); glEnable(GL_LIGHT1); glEnable(GL_LIGHT2);
            /* Restore full 3-point light colors + attenuation
               (room light path may have overwritten these) */
            { float a0[] = {0.15f,0.15f,0.15f,1}, d0[] = {0.70f,0.70f,0.68f,1};
              glLightfv(GL_LIGHT0, GL_AMBIENT, a0);
              glLightfv(GL_LIGHT0, GL_DIFFUSE, d0); }
            { float a1[] = {0.0f,0.0f,0.0f,1}, d1[] = {0.35f,0.30f,0.25f,1};
              glLightfv(GL_LIGHT1, GL_AMBIENT, a1);
              glLightfv(GL_LIGHT1, GL_DIFFUSE, d1); }
            { float a2[] = {0.0f,0.0f,0.0f,1}, d2[] = {0.20f,0.22f,0.30f,1};
              glLightfv(GL_LIGHT2, GL_AMBIENT, a2);
              glLightfv(GL_LIGHT2, GL_DIFFUSE, d2); }
            for (int li = 0; li < 3; li++) {
                GLenum light = GL_LIGHT0 + li;
                glLightf(light, GL_CONSTANT_ATTENUATION, 1.0f);
                glLightf(light, GL_LINEAR_ATTENUATION, 0.0f);
                glLightf(light, GL_QUADRATIC_ATTENUATION, 0.0f);
            }
            /* Disable any extra room lights (3..7) */
            for (int li = 3; li < 8; li++) glDisable(GL_LIGHT0 + li);
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();
            float hp[] = {0.1f, 0.3f, 1.0f, 0.0f};
            glLightfv(GL_LIGHT0, GL_POSITION, hp);
            float fp[] = {-0.5f, 0.8f, 0.3f, 0.0f};
            glLightfv(GL_LIGHT1, GL_POSITION, fp);
            float rp[] = {0.3f, -0.4f, -0.8f, 0.0f};
            glLightfv(GL_LIGHT2, GL_POSITION, rp);
            glPopMatrix();
        }
    }

    /* Grid */
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (show_grid) {
        /* Minor grid lines */
        if (T.isDark) glColor4f(0.18f, 0.18f, 0.24f, 0.5f);
        else          glColor4f(0.68f, 0.68f, 0.72f, 0.5f);
        glBegin(GL_LINES);
        for (int i = -10; i <= 10; i++) {
            if (i == 0) continue; /* skip center, draw separately */
            float f = (float)i * 200.0f;
            glVertex3f(f, 0, -2000); glVertex3f(f, 0, 2000);
            glVertex3f(-2000, 0, f); glVertex3f(2000, 0, f);
        }
        glEnd();

        /* Major axis lines (center cross) */
        if (T.isDark) glColor4f(0.30f, 0.30f, 0.40f, 0.8f);
        else          glColor4f(0.45f, 0.45f, 0.52f, 0.8f);
        glLineWidth(1.5f);
        glBegin(GL_LINES);
        glVertex3f(0, 0, -2000); glVertex3f(0, 0, 2000);
        glVertex3f(-2000, 0, 0); glVertex3f(2000, 0, 0);
        glEnd();
        glLineWidth(1.0f);
    }
    glDisable(GL_BLEND);

    /* Mesh */
    if (has_mesh) {

        if (show_wireframe) {
            glDisable(GL_LIGHTING);
            glDisable(GL_TEXTURE_2D);
            glEnable(GL_COLOR_MATERIAL);
            if (in_vm) {
                /* Immediate-mode wireframe for VM (no display lists) */
                if (T.isDark) glColor3f(0.7f, 0.8f, 0.9f);
                else          glColor3f(0.2f, 0.2f, 0.3f);
                draw_mesh_immediate(im_verts, im_vert_count, im_tris, im_tri_count, im_flip_normals, true);
            } else {
                if (gl_wire_id) glCallList(gl_wire_id);
            }
        } else {

            if (sw_renderer || in_vm) {
                /* ── VM / Software: immediate-mode unlit vertex-color rendering.
                   No display lists, no lighting, no materials.
                   This is the REAL fix for VM GL drivers that choke on
                   compiled display lists with mixed fixed-function state. ── */
                glDisable(GL_LIGHTING);
                glDisable(GL_LIGHT0); glDisable(GL_LIGHT1); glDisable(GL_LIGHT2);
                glDisable(GL_NORMALIZE);
                glDisable(GL_COLOR_MATERIAL);
                glDisable(GL_CULL_FACE);
                glDisable(GL_TEXTURE_2D);

                glEnable(GL_ALPHA_TEST);
                glAlphaFunc(GL_GREATER, 0.01f);

                draw_mesh_immediate(im_verts, im_vert_count, im_tris, im_tri_count, im_flip_normals, false, show_alt_geo);

                glDisable(GL_ALPHA_TEST);
            } else {
                /* ── Hardware renderer: full lighting + texture pipeline ── */
                bool want_tex = show_textured && has_texture && gl_tex_id;

            if (want_tex) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, gl_tex_id);
                glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

                if (show_vcolors) {
                    /* Texture × vertex colors  (original PSX look) */
                    glEnable(GL_COLOR_MATERIAL);
                    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
                } else {
                    /* Texture only — override vertex colors with white material
                       so lighting still works but colors don't tint the texture */
                    glDisable(GL_COLOR_MATERIAL);
                    float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
                    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, white);
                }
            } else {
                glDisable(GL_TEXTURE_2D);
                if (show_vcolors) {
                    /* Vertex / face colors only */
                    glEnable(GL_COLOR_MATERIAL);
                    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
                } else {
                    /* No texture, no vcol — neutral material */
                    glDisable(GL_COLOR_MATERIAL);
                    float gray[] = {
                        T.isDark ? 0.55f : 0.60f,
                        T.isDark ? 0.55f : 0.60f,
                        T.isDark ? 0.62f : 0.60f,
                        1.0f
                    };
                    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, gray);
                }
            }

            if (show_lighting && !sw_renderer) glEnable(GL_LIGHTING);
            else                                glDisable(GL_LIGHTING);

            /* Backface culling toggle + darkened back faces */
            if (show_cull && !sw_renderer) {
                glEnable(GL_CULL_FACE);
            } else {
                glDisable(GL_CULL_FACE);
                /* Darken back-facing polygons so they're distinguishable. */
                if (show_lighting && !sw_renderer) {
                    float dark[] = {0.15f, 0.15f, 0.18f, 1.0f};
                    glMaterialfv(GL_BACK, GL_AMBIENT_AND_DIFFUSE, dark);
                }
            }

            /* Alpha test: discard fully-transparent texels (pixel==0x0000, 0x8000) */
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GREATER, 0.01f);
            /* Enable blending so STP CLUT entries (alpha=128) blend at 50%
               with the framebuffer.  Opaque texels (alpha=255) are unaffected:
               1.0*src + 0.0*dst = src.  This gives correct PSX ABR=0 behavior
               for blood/decal overlays that use per-pixel STP transparency.
               GL_LEQUAL allows coplanar decals to pass depth test against the
               floor they overlay (blood splatters sit on the same plane). */
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthFunc(GL_LEQUAL);
            if (gl_list_id) glCallList(gl_list_id);
            if (gl_2side_id) {
                glDisable(GL_CULL_FACE);
                glCallList(gl_2side_id);
                if (show_cull) glEnable(GL_CULL_FACE);
            }
            if (show_alt_geo && gl_alt_id) glCallList(gl_alt_id);
            glDepthFunc(GL_LESS);
            glDisable(GL_BLEND);
            /* ── Semi-transparent pass: additive blend (god rays, ABR 1+3) ── */
            if (gl_blend_id) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);  /* additive: dst + src*α */
                glDepthMask(GL_FALSE);               /* don't write depth for translucent */
                glDisable(GL_LIGHTING);               /* STP faces use flat texture color */
                /* Light patches lie on the floor they light: pull them
                   forward so they don't z-fight with it */
                glDepthFunc(GL_LEQUAL);
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-1.0f, -4.0f);
                glCallList(gl_blend_id);
                glDisable(GL_POLYGON_OFFSET_FILL);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
                if (show_lighting && !sw_renderer) glEnable(GL_LIGHTING);
            }

            /* ── Semi-transparent pass: subtractive (shadows, ABR 2) ── */
            if (gl_sub_id) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_ZERO, GL_SRC_COLOR);  /* dst *= src (darken) */
                glDepthMask(GL_FALSE);
                glDisable(GL_LIGHTING);
                glDepthFunc(GL_LEQUAL);
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-1.0f, -4.0f);
                glCallList(gl_sub_id);
                glDisable(GL_POLYGON_OFFSET_FILL);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
                if (show_lighting && !sw_renderer) glEnable(GL_LIGHTING);
            }

            glDisable(GL_TEXTURE_2D);
            glDisable(GL_ALPHA_TEST);
            } /* end hardware renderer solid path */
        } /* end wireframe/solid branch */

        /* Restore COLOR_MATERIAL for subsequent draws */
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

        /* Scene overlay: zones, collisions, cameras, spawns */
        if (show_overlay && gl_overlay_id && !is_emd) {
            glDisable(GL_LIGHTING);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glCallList(gl_overlay_id);
            glDisable(GL_BLEND);
            glEnable(GL_DEPTH_TEST);
        }

        /* Debug: vertex normal direction lines */
        if (show_normals && gl_normals_id) {
            glDisable(GL_LIGHTING);
            glDisable(GL_TEXTURE_2D);
            glLineWidth(1.0f);
            glCallList(gl_normals_id);
        }

        /* Object pick highlight */
        if (gl_pick_id && pick_tri >= 0) {
            glCallList(gl_pick_id);
        }

        /* ── Fireballs ── */
        if (walk_mode) {
            glEnable(GL_DEPTH_TEST);  /* render in world space with depth */
            glDisable(GL_LIGHTING);
            glDisable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);  /* additive for glow */
            for (int i = 0; i < MAX_FIREBALLS; i++) {
                const Fireball& fb = fireballs[i];
                if (!fb.active) continue;
                float t = 1.0f - fb.life / 3.0f;  /* 0→1 over lifetime */
                float r0 = 80.0f * (1.0f - t * 0.7f);  /* core radius shrinks */
                float r1 = 160.0f * (1.0f - t * 0.5f); /* glow radius */

                /* Draw as camera-facing billboard quads (2 layers) */
                /* Get right/up vectors from current modelview matrix */
                float mv[16];
                glGetFloatv(GL_MODELVIEW_MATRIX, mv);
                float rx = mv[0], ry = mv[4], rz = mv[8];   /* right */
                float ux = mv[1], uy = mv[5], uz = mv[9];   /* up */

                /* Outer glow */
                glBegin(GL_TRIANGLE_FAN);
                glColor4f(1.0f, 0.4f - t*0.3f, 0.05f, 0.5f - t*0.4f);
                glVertex3f(fb.x, fb.y, fb.z);
                glColor4f(1.0f, 0.2f, 0.0f, 0.0f);
                for (int s = 0; s <= 12; s++) {
                    float a = (float)s * 6.2831853f / 12.0f;
                    float cx = cosf(a), cy = sinf(a);
                    glVertex3f(fb.x + (rx*cx + ux*cy)*r1,
                               fb.y + (ry*cx + uy*cy)*r1,
                               fb.z + (rz*cx + uz*cy)*r1);
                }
                glEnd();

                /* Core */
                glBegin(GL_TRIANGLE_FAN);
                glColor4f(1.0f, 0.95f - t*0.4f, 0.5f - t*0.4f, 1.0f);
                glVertex3f(fb.x, fb.y, fb.z);
                glColor4f(1.0f, 0.6f - t*0.3f, 0.1f, 0.3f);
                for (int s = 0; s <= 8; s++) {
                    float a = (float)s * 6.2831853f / 8.0f;
                    float cx = cosf(a), cy = sinf(a);
                    glVertex3f(fb.x + (rx*cx + ux*cy)*r0,
                               fb.y + (ry*cx + uy*cy)*r0,
                               fb.z + (rz*cx + uz*cy)*r0);
                }
                glEnd();

                /* Trail — 3 fading spheres behind */
                for (int ti = 1; ti <= 3; ti++) {
                    float tf = (float)ti * 0.004f;
                    float bx = fb.x - fb.vx * tf;
                    float by = fb.y - fb.vy * tf;
                    float bz = fb.z - fb.vz * tf;
                    float tr = r0 * (1.0f - (float)ti * 0.25f);
                    float ta = (0.4f - t*0.3f) * (1.0f - (float)ti * 0.3f);
                    glBegin(GL_TRIANGLE_FAN);
                    glColor4f(1.0f, 0.3f, 0.0f, ta);
                    glVertex3f(bx, by, bz);
                    glColor4f(1.0f, 0.15f, 0.0f, 0.0f);
                    for (int s = 0; s <= 6; s++) {
                        float a = (float)s * 6.2831853f / 6.0f;
                        float cx2 = cosf(a), cy2 = sinf(a);
                        glVertex3f(bx + (rx*cx2 + ux*cy2)*tr,
                                   by + (ry*cx2 + uy*cy2)*tr,
                                   bz + (rz*cx2 + uz*cy2)*tr);
                    }
                    glEnd();
                }
            }
            glDisable(GL_BLEND);
        }

        /* Selection overlay — draw selected tris inline (red, blended) */
        if (sel_mask && uv_tris && uv_verts && uv_tri_count > 0) {
            int sc = 0;
            for (int i = 0; i < uv_tri_count; i++) if (sel_mask[i]) sc++;
            if (sc > 0) {
                glDisable(GL_LIGHTING);
                glDisable(GL_TEXTURE_2D);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthFunc(GL_LEQUAL);
                glDisable(GL_CULL_FACE);
                glColor4f(1.0f, 0.1f, 0.05f, 0.6f);

        
                /* Filled overlay */
                glBegin(GL_TRIANGLES);
                for (int i = 0; i < uv_tri_count; i++) {
                    if (!sel_mask[i]) continue;
                    const MeshTri& t = uv_tris[i];
                    if ((int)t.idx[0] >= uv_vert_count ||
                        (int)t.idx[1] >= uv_vert_count ||
                        (int)t.idx[2] >= uv_vert_count) continue;
                    for (int vi = 0; vi < 3; vi++) {
                        const MeshVert& vv = uv_verts[t.idx[vi]];
                        glVertex3f(vv.x, vv.y, vv.z);
                    }
                }
                glEnd();

                /* Wireframe outline */
                glColor4f(1.0f, 1.0f, 0.2f, 0.9f);
                glLineWidth(2.0f);
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glBegin(GL_TRIANGLES);
                for (int i = 0; i < uv_tri_count; i++) {
                    if (!sel_mask[i]) continue;
                    const MeshTri& t = uv_tris[i];
                    if ((int)t.idx[0] >= uv_vert_count ||
                        (int)t.idx[1] >= uv_vert_count ||
                        (int)t.idx[2] >= uv_vert_count) continue;
                    for (int vi = 0; vi < 3; vi++) {
                        const MeshVert& vv = uv_verts[t.idx[vi]];
                        glVertex3f(vv.x, vv.y, vv.z);
                    }
                }
                glEnd();
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glLineWidth(1.0f);

                        glEnable(GL_CULL_FACE);
                glDepthFunc(GL_LESS);
                glDisable(GL_BLEND);
                sel_count = sc;
            }
        }

        /* Bone overlay */
        if (has_skeleton && show_bones && gl_bone_id) {
            glDisable(GL_LIGHTING);
            glDisable(GL_DEPTH_TEST);
            glCallList(gl_bone_id);

            /* Draw bone index labels */
            if (show_bone_labels && gl_font_base && skeleton.bone_count > 0) {
                if (T.isDark) glColor3f(0.85f, 0.90f, 1.0f);
                else          glColor3f(0.1f, 0.1f, 0.2f);
                glListBase(gl_font_base);
                for (int i = 0; i < skeleton.bone_count; i++) {
                    char lbl[48];
                    const char* name = get_bone_name(skeleton.bone_count, i);
                    if (name) _snprintf(lbl, 47, "%d:%s", i, name);
                    else      _snprintf(lbl, 47, "%d", i);
                    glRasterPos3f(skeleton.abs_pos[i][0],
                                  skeleton.abs_pos[i][1],
                                  skeleton.abs_pos[i][2]);
                    glCallLists((GLsizei)strlen(lbl), GL_UNSIGNED_BYTE, lbl);
                }
            }

            glEnable(GL_DEPTH_TEST);
        }
    }

    if (!walk_mode) draw_shot_overlay(w, vh);

    /* ── HUD overlay (rendered into back buffer BEFORE swap) ── */
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_COLOR_MATERIAL);

    /* Switch to 2D orthographic */
    int sw = w;
    int sh = vh;
    if (sw < 1) sw = 1; if (sh < 1) sh = 1;

    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, sw, 0, sh, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

    if (show_hud) {

    /* Build info string */
    char info[256];
    if (bone_count > 0) {
        const char* skel_type = "unknown";
        switch (bone_count) {
            case  7: skel_type = "Compy"; break;
            case 15: skel_type = "Human"; break;
            case 18: skel_type = "Pteranodon"; break;
            case 20: skel_type = "T-Rex"; break;
            case 21: skel_type = "Raptor"; break;
            case 22: skel_type = "Therizino"; break;
        }
        _snprintf(info, 255, "Verts: %d  Faces: %d  Bones: %d (%s)  [%s] [%s] [%s]%s%s%s%s",
            vert_count, tri_count, bone_count, skel_type,
            show_wireframe ? "Wire" : "Solid",
            show_lighting ? "Lit" : "Flat",
            show_cull ? "Cull" : "2Side",
            (has_skeleton && show_bones) ? " [Bones]" : "",
            (show_textured && has_texture) ? " [Tex]" : "",
            show_vcolors ? " [VCol]" : "",
            show_normals ? " [Normals]" : "");
    } else
        _snprintf(info, 255, "Verts: %d  Faces: %d  [%s] [%s] [%s]%s%s%s%s%s%s  CLUT:%d+%d",
            vert_count, tri_count,
            show_wireframe ? "Wire" : "Solid",
            show_lighting ? "Lit" : "Flat",
            show_cull ? "Cull" : "2Side",
            (show_textured && has_texture) ? " [Tex]" : "",
            show_vcolors ? " [VCol]" : "",
            (show_overlay && gl_overlay_id) ? " [Overlay]" : "",
            (show_alt_geo && gl_alt_id) ? " [Unplaced]" : "",
            walk_mode ? (walk_noclip ? " [Noclip]" : (walk_rect >= 0 ? " [Walk:bound]" : " [Walk:free]")) : "",
            show_normals ? " [Normals]" : "",
            clut_base_y, num_pal_rows);
    if (use_room_lights && n_room_lights > 0) {
        int il = (int)strlen(info);
        _snprintf(info + il, 255 - il, " [RoomLit:%d]", n_room_lights);
    }
    if (cam_sel >= 0 && !is_emd) {
        int il = (int)strlen(info);
        const char* how = shot_cur < 0 ? "" :
                          shot_kind == RDT_SHOT_TRACKING ? " tracking" : " fixed";
        _snprintf(info + il, 255 - il, " [Cam %d%s%s]", cam_list[cam_sel].cam_id, how,
                  shot_cur < 0 ? "" : !shot_locked ? ", free view (J: back)" : "");
        if (shot_locked && shot_zoom != 1) {
            il = (int)strlen(info);
            _snprintf(info + il, 255 - il, " [View x%.2f]", shot_zoom);
        }
    }

    /* Render mode indicator */
    if (sw_renderer) {
        int il = (int)strlen(info);
        _snprintf(info + il, 255 - il, "  [SW Mode]");
    } else if (in_vm) {
        int il = (int)strlen(info);
        _snprintf(info + il, 255 - il, "  [VM GPU]");
    }

    /* GL renderer name line */
    char gl_info[160];
    const char* mode_str = sw_renderer ? " [SW]" : in_vm ? " [VM]" : "";
    _snprintf(gl_info, 159, "GL: %s%s  (F11=cycle: Normal/VM/SW)",
        gl_renderer_name[0] ? gl_renderer_name : "unknown", mode_str);

    /* Info line  -  top left, accent-tinted */
    if (T.isDark) glColor3f(0.35f, 0.75f, 1.0f);
    else          glColor3f(0.0f, 0.45f, 0.0f);
    glRasterPos2i(8, sh - 16);
    glListBase(gl_font_base);
    glCallLists((GLsizei)strlen(info), GL_UNSIGNED_BYTE, info);

    /* GL renderer line — second line, dimmer */
    if (T.isDark) glColor3f(0.28f, 0.50f, 0.65f);
    else          glColor3f(0.35f, 0.35f, 0.40f);
    glRasterPos2i(8, sh - 32);
    glCallLists((GLsizei)strlen(gl_info), GL_UNSIGNED_BYTE, gl_info);

    /* Object pick info — third/fourth line, yellow accent */
    if (pick_tri >= 0 && pick_info[0]) {
        glColor3f(1.0f, 0.9f, 0.2f);
        /* Split on newline to render two lines */
        const char* nl = strchr(pick_info, '\n');
        if (nl) {
            int len1 = (int)(nl - pick_info);
            glRasterPos2i(8, sh - 48);
            glCallLists(len1, GL_UNSIGNED_BYTE, pick_info);
            glRasterPos2i(8, sh - 64);
            glCallLists((GLsizei)strlen(nl + 1), GL_UNSIGNED_BYTE, nl + 1);
        } else {
            glRasterPos2i(8, sh - 48);
            glCallLists((GLsizei)strlen(pick_info), GL_UNSIGNED_BYTE, pick_info);
        }
    }

    /* Controls line  -  bottom left, dimmed */
    const char* ctrl = walk_mode ?
        "WASD:move  Shift:run  Space:jump  P:noclip  LMB:fireball  Mouse:look  ESC/F:exit walk" :
        (cam_sel >= 0 && shot_cur >= 0 ?
        "K/Shift+K:camera  WASD/Arrows:move player  Shift:run  Scroll:widen view  J:back to shot  LMB:orbit  RMB:pan  R:reset  O:overlay  H:unplaced  T:tex  L:light" :
        anim_model ?
        "LMB:orbit  RMB:pan  Scroll:zoom  Alt+LMB:snap  W:wire  L:light  T:tex  V:vcol  C:cull  B:bones  U:uv  N:normals  Space:play" :
        "LMB:orbit  RMB:pan  Scroll:zoom  Alt+LMB:snap  Ctrl+LMB:pick  W:wire  L:light  T:tex  V:vcol  C:cull  U:uv  O:overlay  H:unplaced  K:camera  I:roomlit  F:walk  N:normals  R:reset  Esc:deselect");
    if (T.isDark) glColor3f(0.38f, 0.40f, 0.48f);
    else          glColor3f(0.40f, 0.40f, 0.45f);
    glRasterPos2i(8, 6);
    glCallLists((GLsizei)strlen(ctrl), GL_UNSIGNED_BYTE, ctrl);

    /* Overlay legend — right side, only when overlay is visible: the types
       the room has and the overlay tree shows (zone rows: that zone type) */
    if (show_overlay && gl_overlay_id && !is_emd && !cached_overlay.dc2) {
        const RdtSceneOverlay& ov = cached_overlay;
        struct LegendRow { float r, g, b; int type, zone; const char* label; };
        static const LegendRow rows[] = {
            { 0.0f, 0.8f, 0.2f, OverlayVis::RECTS, -1,       "Green  = Collision rect (ptr[1])" },
            { 1.0f, 0.4f, 0.1f, OverlayVis::TRIGGERS, 0,     "Orange = Zone type 0 (0x28)" },
            { 0.2f, 0.7f, 1.0f, OverlayVis::TRIGGERS, 1,     "LtBlue = Zone type 1 (0x28)" },
            { 0.9f, 0.9f, 0.2f, OverlayVis::TRIGGERS, 23,    "Yellow = Zone type 2/3 (0x28)" },
            { 0.0f, 0.9f, 0.9f, OverlayVis::TRIGGERS, 4,     "Cyan   = Zone type 4 item (0x28)" },
            { 0.8f, 0.3f, 0.8f, OverlayVis::TRIGGERS, 5,     "Purple = Zone, other type (0x28)" },
            { 1.0f, 1.0f, 0.0f, OverlayVis::FLOOR_ZONES, -1, "Yellow = Floor zone" },
            { 0.6f, 0.3f, 0.85f,OverlayVis::CAM_CUTS, -1,    "Violet = Camera cut zone" },
            { 0.9f, 0.85f, 0.5f,OverlayVis::LIGHTS, -1,      "Gold   = Scene light (0x3A)" },
            { 0.2f, 0.4f, 1.0f, OverlayVis::SCRIPT_CAMS, -1, "Blue   = Script camera eye" },
            { 1.0f, 0.2f, 0.2f, OverlayVis::SCRIPT_CAMS, -1, "Red pt = Script camera target" },
            { 1.0f, 0.3f, 1.0f, OverlayVis::SPAWNS, -1,      "Pink   = Object spawn" },
            { 1.0f, 1.0f, 1.0f, OverlayVis::OBJECTS, -1,     "White  = Object (0x42)" },
            { 1.0f, 0.6f, 0.0f, OverlayVis::CHARS, -1,       "Orange = Character spawn" },
            { 1.0f, 0.15f,0.1f, OverlayVis::ENEMIES, -1,     "Red X  = Enemy spawn (0x5B)" },
            { 0.5f, 0.6f, 0.8f, OverlayVis::FOG, -1,         "PaleBlue= Fog volume (0x3D)" },
            { 0.7f, 0.5f, 0.9f, OverlayVis::CAMERAS, -1,     "Dashed = Camera trigger zones (ptr[3])" },
        };
        int n_rows = (int)(sizeof(rows) / sizeof(rows[0]));
        int legend_x = w - 280;
        int legend_y = sh - 18;
        int shown = 0;
        for (int li = 0; li < n_rows; li++) {
            const LegendRow& row = rows[li];
            if (!ov_vis.on[row.type]) continue;
            int n = 0;
            switch (row.type) {
            case OverlayVis::RECTS:       n = ov.n_collisions; break;
            case OverlayVis::CAM_CUTS:    n = ov.n_camcuts; break;
            case OverlayVis::LIGHTS:      for (int i = 0; i < ov.n_lights; i++) n += ov.lights[i].has_pos; break;
            case OverlayVis::SCRIPT_CAMS: n = ov.n_cameras; break;
            case OverlayVis::SPAWNS:      n = ov.n_spawns; break;
            case OverlayVis::OBJECTS:     n = ov.n_enemies; break;
            case OverlayVis::CHARS:       n = ov.n_chars; break;
            case OverlayVis::ENEMIES:     n = ov.n_items; break;
            case OverlayVis::FOG:         n = ov.n_fog; break;
            case OverlayVis::CAMERAS:     n = ov.n_cam_threads; break;
            default:
                for (int i = 0; i < ov.n_zones; i++) {
                    const RdtFloorZone& z = ov.zones[i];
                    if ((z.kind == RDT_ZONE_TRIGGER) != (row.type == OverlayVis::TRIGGERS)) continue;
                    int zt = z.target_index;
                    if (row.zone < 0 || zt == row.zone || (row.zone == 23 && (zt == 2 || zt == 3)) ||
                        (row.zone == 5 && zt > 4)) n++;
                }
            }
            if (n == 0) continue;
            glColor3f(row.r, row.g, row.b);
            glRasterPos2i(legend_x, legend_y - shown * 14);
            glCallLists((GLsizei)strlen(row.label), GL_UNSIGNED_BYTE, row.label);
            shown++;
        }
    }

    /* ── Walk mode crosshair ── */
    if (walk_mode) {
        int cx = w / 2, cy = sh / 2;
        glColor3f(0.9f, 0.9f, 0.9f);
        glLineWidth(1.5f);
        glBegin(GL_LINES);
        glVertex2i(cx - 10, cy); glVertex2i(cx + 10, cy);
        glVertex2i(cx, cy - 10); glVertex2i(cx, cy + 10);
        glEnd();
        glLineWidth(1.0f);
    }

    } /* end show_hud */

    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();

    /* ── Scene compass gizmo (bottom-right corner) ── */
    {
        int gsize = 60;  /* gizmo viewport size */
        int gx = w - gsize - 8;
        int gy = bar_h + 8;
        glViewport(gx, gy, gsize, gsize);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(-1.6, 1.6, -1.6, 1.6, -10, 10);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

        /* Apply camera rotation only (no translation, no distance) */
        float gyr = cam_yaw * 3.14159f / 180.0f;
        float gpr = cam_pitch * 3.14159f / 180.0f;
        float gex = cosf(gpr) * sinf(gyr);
        float gey = sinf(gpr);
        float gez = cosf(gpr) * cosf(gyr);
        float gup = (cosf(gpr) >= 0.0f) ? 1.0f : -1.0f;
        gluLookAt(gex, gey, gez, 0, 0, 0, 0, gup, 0);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glLineWidth(2.0f);

        /* Draw axes: X=red, Y=green, Z=blue */
        glBegin(GL_LINES);
        glColor3f(0.95f, 0.25f, 0.25f); glVertex3f(0,0,0); glVertex3f(1,0,0);
        glColor3f(0.25f, 0.90f, 0.25f); glVertex3f(0,0,0); glVertex3f(0,1,0);
        glColor3f(0.30f, 0.50f, 0.95f); glVertex3f(0,0,0); glVertex3f(0,0,1);
        glEnd();

        /* Axis labels */
        if (gl_font_base) {
            glListBase(gl_font_base);
            glColor3f(0.95f, 0.30f, 0.30f);
            glRasterPos3f(1.15f, 0.0f, 0.0f);
            glCallLists(1, GL_UNSIGNED_BYTE, "X");
            glColor3f(0.30f, 0.95f, 0.30f);
            glRasterPos3f(0.0f, 1.15f, 0.0f);
            glCallLists(1, GL_UNSIGNED_BYTE, "Y");
            glColor3f(0.35f, 0.55f, 1.0f);
            glRasterPos3f(0.0f, 0.0f, 1.15f);
            glCallLists(1, GL_UNSIGNED_BYTE, "Z");
        }

        /* Negative axis ticks (dimmed, shorter) */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(1.0f);
        glBegin(GL_LINES);
        glColor4f(0.95f, 0.25f, 0.25f, 0.35f); glVertex3f(0,0,0); glVertex3f(-0.5f,0,0);
        glColor4f(0.25f, 0.90f, 0.25f, 0.35f); glVertex3f(0,0,0); glVertex3f(0,-0.5f,0);
        glColor4f(0.30f, 0.50f, 0.95f, 0.35f); glVertex3f(0,0,0); glVertex3f(0,0,-0.5f);
        glEnd();
        glDisable(GL_BLEND);

        glLineWidth(1.0f);
        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW); glPopMatrix();

        /* Restore viewport */
        glViewport(0, bar_h, w, vh);
    }

    /* ── Animation bar background (rendered into back buffer to prevent flicker) ── */
    if (anim_model && bar_h > 0) {
        /* Render bar as a GL quad in full-window ortho space */
        glViewport(0, 0, w, h);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0, w, 0, h, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

        if (T.isDark) glColor3ub((u8)(T.bg & 0xFF), (u8)((T.bg>>8)&0xFF), (u8)((T.bg>>16)&0xFF));
        else          glColor3f(0.75f, 0.75f, 0.75f);
        glBegin(GL_QUADS);
            glVertex2i(0, 0);
            glVertex2i(w, 0);
            glVertex2i(w, bar_h);
            glVertex2i(0, bar_h);
        glEnd();

        /* Separator line at top of bar */
        if (T.isDark) glColor3ub(55, 55, 65);
        else          glColor3ub(180, 180, 190);
        glBegin(GL_LINES);
            glVertex2i(0, bar_h); glVertex2i(w, bar_h);
        glEnd();

        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW); glPopMatrix();

        /* Restore viewport to scene area */
        glViewport(0, bar_h, w, vh);
    }

    glEnable(GL_DEPTH_TEST);
    SwapBuffers(hDC);
    wglMakeCurrent(0, 0);
}

void ViewerPanel3D::on_mouse_down(int x, int y, int btn) {
    if (walk_mode) {
        if (btn == 0) {
            /* Shoot fireball in look direction */
            float yr = walk_yaw * 3.14159f / 180.0f;
            float pr = walk_pitch * 3.14159f / 180.0f;
            float FB_SPEED = 60000.0f;
            for (int i = 0; i < MAX_FIREBALLS; i++) {
                Fireball& fb = fireballs[i];
                if (fb.active) continue;
                fb.x = walk_x; fb.y = walk_y - 200.0f; fb.z = walk_z;
                fb.vx = cosf(pr) * sinf(yr) * FB_SPEED;
                fb.vy = sinf(pr) * FB_SPEED;
                fb.vz = cosf(pr) * cosf(yr) * FB_SPEED;
                fb.life = 3.0f;
                fb.active = true;
                break;
            }
        }
        return;  /* no orbit/pan in walk mode */
    }
    /* Ctrl+LMB = pick object */
    if (btn == 0 && (GetKeyState(VK_CONTROL) & 0x8000) && has_mesh && im_tris && im_tri_count > 0) {
        do_pick(x, y);
        return;
    }
    if (btn == 0) { dragging = true; panning = false; }
    else if (btn == 1) { panning = true; dragging = false; }
    shot_locked = false;  /* orbit on from the shot */
    last_mx = x; last_my = y;
    /* Save screen-space anchor for infinite drag (3ds Max style) */
    warp_anchor.x = x; warp_anchor.y = y;
    ClientToScreen(hwnd, &warp_anchor);
    ShowCursor(FALSE);
    SetCapture(hwnd);
}

void ViewerPanel3D::do_pick(int mx, int my) {
    if (!hRC || !hwnd) return;
    wglMakeCurrent(hDC, hRC);
    RECT rc; GetClientRect(hwnd, &rc);
    int w = view_w(rc.right - rc.left);
    int h = rc.bottom - rc.top;
    int bar_h = anim_model ? 66 : 0;
    int vh = h - bar_h;
    if (w < 1 || vh < 1 || !im_tris || im_tri_count <= 0) return;

    /* Save GL state */
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    /* Set up identical camera as render() */
    glViewport(0, bar_h, w, vh);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    double aspect = (double)w / (double)vh;
    double near_clip = cam_dist * 0.005;
    if (near_clip < 0.5) near_clip = 0.5;
    double far_clip = orbit_far_clip(cam_dist, cam_x, cam_y, cam_z, scene_reach);
    gluPerspective(view_fovy(aspect), aspect, near_clip, far_clip);

    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    float yr = cam_yaw * 3.14159f / 180.0f;
    float pr = cam_pitch * 3.14159f / 180.0f;
    float ex = cam_x + cam_dist * cosf(pr) * sinf(yr);
    float ey = cam_y + cam_dist * sinf(pr);
    float ez = cam_z + cam_dist * cosf(pr) * cosf(yr);
    gluLookAt(ex, ey, ez, cam_x, cam_y, cam_z, cam_upx, cam_upy, cam_upz);

    /* Render pick pass: each tri gets a unique flat color encoding its index */
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glShadeModel(GL_FLAT);
    if (is_emd) glFrontFace(GL_CW); else glFrontFace(GL_CCW);
    glEnable(GL_DEPTH_TEST);

    glBegin(GL_TRIANGLES);
    for (int i = 0; i < im_tri_count; i++) {
        /* Encode tri index as color: index+1 so 0 = background */
        int id = i + 1;
        const MeshTri& t = im_tris[i];
        if (t.alt && !show_alt_geo) continue;
        if (!tri_in_view(t)) continue;
        glColor3ub((u8)(id & 0xFF), (u8)((id >> 8) & 0xFF), (u8)((id >> 16) & 0xFF));
        for (int vi = 0; vi < 3; vi++) {
            const MeshVert& v = im_verts[t.idx[vi]];
            glVertex3f(v.x, v.y, v.z);
        }
    }
    glEnd();
    glFinish();

    /* Read pixel at click position (GL origin is bottom-left) */
    int gl_x = mx;
    int gl_y = vh - 1 - (my - bar_h);
    if (gl_y < 0 || gl_y >= vh || gl_x < 0 || gl_x >= w) {
        glPopAttrib();
        return;
    }
    u8 pixel[4] = {};
    glReadPixels(gl_x, gl_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

    /* Decode tri index */
    int id = pixel[0] | (pixel[1] << 8) | (pixel[2] << 16);
    int hit = id - 1;  /* -1 = background (no hit) */

    /* Restore GL state */
    glPopAttrib();

    /* Clear previous pick */
    if (gl_pick_id) { glDeleteLists(gl_pick_id, 1); gl_pick_id = 0; }
    pick_tri = -1; pick_section = 0; pick_group_count = 0; pick_info[0] = 0;

    if (hit < 0 || hit >= im_tri_count) {
        render();
        return;
    }

    pick_tri = hit;
    pick_section = im_tris[hit].src_off;
    pick_tpage = im_tris[hit].tpage;
    pick_clut  = im_tris[hit].clut;

    /* Group by material (tpage + clut) — much finer than section,
       especially for rooms where all geometry is one big section. */
    int group_tris = 0;
    for (int i = 0; i < im_tri_count; i++) {
        if (im_tris[i].tpage == pick_tpage && im_tris[i].clut == pick_clut)
            group_tris++;
    }
    pick_group_count = group_tris;

    /* Compute bounding box of material group */
    float bx0=1e9f, by0=1e9f, bz0=1e9f, bx1=-1e9f, by1=-1e9f, bz1=-1e9f;
    for (int i = 0; i < im_tri_count; i++) {
        if (im_tris[i].tpage != pick_tpage || im_tris[i].clut != pick_clut) continue;
        const MeshTri& t = im_tris[i];
        for (int vi = 0; vi < 3; vi++) {
            const MeshVert& v = im_verts[t.idx[vi]];
            if (v.x < bx0) bx0 = v.x; if (v.x > bx1) bx1 = v.x;
            if (v.y < by0) by0 = v.y; if (v.y > by1) by1 = v.y;
            if (v.z < bz0) bz0 = v.z; if (v.z > bz1) bz1 = v.z;
        }
    }

    /* Count unique sections in this material group */
    u32 seen_secs[64]; int n_seen = 0;
    for (int i = 0; i < im_tri_count; i++) {
        if (im_tris[i].tpage != pick_tpage || im_tris[i].clut != pick_clut) continue;
        u32 so = im_tris[i].src_off;
        bool found = false;
        for (int s = 0; s < n_seen; s++) if (seen_secs[s] == so) { found = true; break; }
        if (!found && n_seen < 64) seen_secs[n_seen++] = so;
    }

    /* Build info string */
    int tpx = (pick_tpage & 0xF) * 64;
    int tpy = ((pick_tpage >> 4) & 1) * 256;
    int abr = (pick_tpage >> 5) & 3;
    int bpp = (pick_tpage >> 7) & 3;
    const char* bpp_s = bpp == 0 ? "4bpp" : bpp == 1 ? "8bpp" : "15bpp";
    int cx = (pick_clut & 0x3F) * 16;
    int cy = (pick_clut >> 6) & 0x1FF;
    _snprintf(pick_info, 511,
        "Sec: 0x%05X | Faces: %d | Tpage: 0x%04X (%s ABR=%d TX=%d TY=%d) | CLUT: 0x%04X (X=%d Y=%d)%s\n"
        "Tri[%d] alt=%d | BBox: (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)",
        pick_section, group_tris,
        pick_tpage, bpp_s, abr, tpx, tpy,
        pick_clut, cx, cy,
        n_seen > 1 ? " [multi-sec]" : "",
        hit, im_tris[hit].alt,
        bx0, by0, bz0, bx1, by1, bz1);

    /* Build highlight display list: yellow wireframe of material group */
    gl_pick_id = glGenLists(1);
    glNewList(gl_pick_id, GL_COMPILE);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    /* Semi-transparent yellow fill */
    glColor4f(1.0f, 0.9f, 0.0f, 0.15f);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < im_tri_count; i++) {
        if (im_tris[i].tpage != pick_tpage || im_tris[i].clut != pick_clut) continue;
        const MeshTri& t = im_tris[i];
        for (int vi = 0; vi < 3; vi++) {
            const MeshVert& v = im_verts[t.idx[vi]];
            glVertex3f(v.x, v.y, v.z);
        }
    }
    glEnd();
    glDisable(GL_POLYGON_OFFSET_FILL);
    /* Bright yellow wireframe outline */
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glLineWidth(2.0f);
    glColor4f(1.0f, 1.0f, 0.0f, 0.8f);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < im_tri_count; i++) {
        if (im_tris[i].tpage != pick_tpage || im_tris[i].clut != pick_clut) continue;
        const MeshTri& t = im_tris[i];
        for (int vi = 0; vi < 3; vi++) {
            const MeshVert& v = im_verts[t.idx[vi]];
            glVertex3f(v.x, v.y, v.z);
        }
    }
    glEnd();
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glLineWidth(1.0f);
    glDepthFunc(GL_LESS);
    glEndList();

    render();
}

void ViewerPanel3D::on_mouse_up(int x, int y, int btn) {
    (void)x; (void)y; (void)btn;
    if (walk_mode) return;  /* cursor stays captured in walk mode */
    if (dragging || panning) ShowCursor(TRUE);
    dragging = false; panning = false; ReleaseCapture();
}

void ViewerPanel3D::on_mouse_move(int x, int y, int btn_state) {
    (void)btn_state;
    int dx = x - last_mx, dy = y - last_my;

    /* Clamp deltas to prevent wild jumps from cursor warp glitches,
       VM mouse capture issues, or DPI scaling mismatches. */
    if (dx > 200) dx = 200; if (dx < -200) dx = -200;
    if (dy > 200) dy = 200; if (dy < -200) dy = -200;
    if (walk_mode) {
        /* Accumulate mouse deltas — walk_tick() will consume them.
           No SetCursorPos here: it generates WM_MOUSEMOVE feedback
           that starves WM_TIMER and causes movement stutter. */
        walk_mouse_dx += dx;
        walk_mouse_dy += dy;
        last_mx = x; last_my = y;
        return;
    } else if (dragging) {
        bool alt_held = (GetKeyState(VK_MENU) & 0x8000) != 0;
        static bool alt_was_held = false;

        if (alt_held && !alt_was_held) {
            /* Ortho snap */
            float y = fmodf(cam_yaw, 360.0f);
            if (y < 0) y += 360.0f;
            if (y < 45.0f || y >= 315.0f) cam_yaw = 0.0f;
            else if (y < 135.0f) cam_yaw = 90.0f;
            else if (y < 225.0f) cam_yaw = 180.0f;
            else cam_yaw = 270.0f;
            if (cam_pitch > 45.0f) cam_pitch = 89.9f;
            else if (cam_pitch < -45.0f) cam_pitch = -89.9f;
            else cam_pitch = 0.0f;
            if (cam_pitch > 45.0f || cam_pitch < -45.0f) {
                float yr = cam_yaw * 3.14159f / 180.0f;
                cam_upx = sinf(yr); cam_upy = 0; cam_upz = cosf(yr);
                if (cam_pitch > 0) { cam_upx = -cam_upx; cam_upz = -cam_upz; }
            } else {
                cam_upx = 0; cam_upy = 1; cam_upz = 0;
            }
            render();
        } else if (!alt_held) {
            if (in_vm) {
                /* ── Simple yaw/pitch orbit for VM environments ──
                   No matrix math, no up-vector tracking, no corruption.
                   SetCursorPos warp is unreliable in VMs, and the matrix
                   orbit accumulates drift without the warp anchor. */
                cam_yaw   -= dx * 0.3f;
                cam_pitch += dy * 0.3f;
                if (cam_pitch > 89.0f) cam_pitch = 89.0f;
                if (cam_pitch < -89.0f) cam_pitch = -89.0f;
                cam_upx = 0; cam_upy = 1; cam_upz = 0;
            } else {
                /* ── View-matrix orbit for hardware renderer ── */
                float drx = dx * -0.003f;
                float dry = dy * -0.003f;
                float yr = cam_yaw * 3.14159f / 180.0f;
                float pr = cam_pitch * 3.14159f / 180.0f;
                float ex = cam_dist * cosf(pr) * sinf(yr);
                float ey = cam_dist * sinf(pr);
                float ez = cam_dist * cosf(pr) * cosf(yr);
                float fl = sqrtf(ex*ex + ey*ey + ez*ez);
                if (fl < 0.001f) fl = 0.001f;
                float fx = -ex/fl, fy = -ey/fl, fz = -ez/fl;
                float rx = fy*cam_upz - fz*cam_upy;
                float ry = fz*cam_upx - fx*cam_upz;
                float rz = fx*cam_upy - fy*cam_upx;
                float rl = sqrtf(rx*rx + ry*ry + rz*rz);
                if (rl > 0.001f) { rx /= rl; ry /= rl; rz /= rl; }
                float cp = cosf(dry), sp = sinf(dry);
                float ex2 = ex*(rx*rx*(1-cp)+cp)    + ey*(rx*ry*(1-cp)-rz*sp) + ez*(rx*rz*(1-cp)+ry*sp);
                float ey2 = ex*(ry*rx*(1-cp)+rz*sp) + ey*(ry*ry*(1-cp)+cp)    + ez*(ry*rz*(1-cp)-rx*sp);
                float ez2 = ex*(rz*rx*(1-cp)-ry*sp) + ey*(rz*ry*(1-cp)+rx*sp) + ez*(rz*rz*(1-cp)+cp);
                float ux2 = cam_upx*(rx*rx*(1-cp)+cp)    + cam_upy*(rx*ry*(1-cp)-rz*sp) + cam_upz*(rx*rz*(1-cp)+ry*sp);
                float uy2 = cam_upx*(ry*rx*(1-cp)+rz*sp) + cam_upy*(ry*ry*(1-cp)+cp)    + cam_upz*(ry*rz*(1-cp)-rx*sp);
                float uz2 = cam_upx*(rz*rx*(1-cp)-ry*sp) + cam_upy*(rz*ry*(1-cp)+rx*sp) + cam_upz*(rz*rz*(1-cp)+cp);
                float cy = cosf(drx), sy = sinf(drx);
                float ex3 = ex2 * cy + ez2 * sy;
                float ey3 = ey2;
                float ez3 = -ex2 * sy + ez2 * cy;
                float ux3 = ux2 * cy + uz2 * sy;
                float uy3 = uy2;
                float uz3 = -ux2 * sy + uz2 * cy;
                float new_dist = sqrtf(ex3*ex3 + ey3*ey3 + ez3*ez3);
                if (new_dist < 0.001f) new_dist = 0.001f;
                cam_pitch = asinf(ey3 / new_dist) * 180.0f / 3.14159f;
                cam_yaw = atan2f(ex3, ez3) * 180.0f / 3.14159f;
                cam_upx = ux3; cam_upy = uy3; cam_upz = uz3;
                float ul = sqrtf(cam_upx*cam_upx + cam_upy*cam_upy + cam_upz*cam_upz);
                if (ul > 0.001f) { cam_upx /= ul; cam_upy /= ul; cam_upz /= ul; }
            }
        }
        alt_was_held = alt_held;
    } else if (panning) {
        if (in_vm) {
            /* ── Simple screen-space pan for VM environments ── */
            float scale = cam_dist * 0.002f;
            float yr = cam_yaw * 3.14159f / 180.0f;
            /* Right vector (simplified, Y-up) */
            float rx = cosf(yr), rz = -sinf(yr);
            cam_x -= rx * dx * scale;
            cam_z -= rz * dx * scale;
            cam_y += dy * scale;
        } else {
            /* Pan in screen-space using camera right and up vectors */
            float yr = cam_yaw * 3.14159f / 180.0f;
            float pr = cam_pitch * 3.14159f / 180.0f;
            float scale = cam_dist * 0.002f;
            float fx = -cosf(pr) * sinf(yr);
            float fy = -sinf(pr);
            float fz = -cosf(pr) * cosf(yr);
            float rx = fy*cam_upz - fz*cam_upy;
            float ry = fz*cam_upx - fx*cam_upz;
            float rz = fx*cam_upy - fy*cam_upx;
            float rl = sqrtf(rx*rx + ry*ry + rz*rz);
            if (rl > 0.001f) { rx /= rl; ry /= rl; rz /= rl; }
            cam_x -= (rx * dx - cam_upx * dy) * scale;
            cam_y -= (ry * dx - cam_upy * dy) * scale;
            cam_z -= (rz * dx - cam_upz * dy) * scale;
        }
    }
    last_mx = x; last_my = y;
    if (dragging || panning) {
        render();
        if (!in_vm) {
            /* Warp cursor back to anchor for infinite drag.
             * Skip in VMs — SetCursorPos fights with VM mouse integration,
             * causing wrong deltas and wild orbit/pan behavior. */
            SetCursorPos(warp_anchor.x, warp_anchor.y);
            POINT cl = warp_anchor; ScreenToClient(hwnd, &cl);
            last_mx = cl.x; last_my = cl.y;
        }
    }
}

void ViewerPanel3D::on_mouse_wheel(int delta) {
    if (walk_mode) {
        /* In walk mode, scroll adjusts eye height */
        walk_y += delta * 0.5f;
        render();
        return;
    }
    if (shot_locked) {
        /* On a shot the eye stays put: scroll widens or narrows the view
           around the game's frame */
        shot_zoom *= 1.0f - delta * 0.001f;
        if (shot_zoom < 0.25f) shot_zoom = 0.25f;
        if (shot_zoom > 6.0f) shot_zoom = 6.0f;
        render();
        return;
    }
    cam_dist -= delta * cam_dist * 0.001f;
    if (cam_dist < 10) cam_dist = 10;
    if (cam_dist > 50000) cam_dist = 50000;
    render();
}

/* ─── Walk-through collision helpers ─── */
static bool walk_point_in_rect(float px, float pz, const RdtCollisionRect& r) {
    /* Convert rect to viewer coords with small epsilon for boundary tolerance */
    const float EPS = 15.0f;  /* mm tolerance for rect boundary crossing */
    float x0 = -(float)r.x;
    float x1 = -(float)(r.x + (s16)r.w);
    float z0 = (float)r.z;
    float z1 = (float)(r.z + (s16)r.h);
    if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
    if (z0 > z1) { float t = z0; z0 = z1; z1 = t; }
    return px >= (x0 - EPS) && px <= (x1 + EPS) && pz >= (z0 - EPS) && pz <= (z1 + EPS);
}

static int walk_find_rect(float px, float pz, const RdtCollisionRect* cols, int n) {
    for (int i = 0; i < n; i++)
        if (walk_point_in_rect(px, pz, cols[i])) return i;
    return -1;
}

/* ─── Walkable grid: rasterize collision rects into a bitmap ─── */
void ViewerPanel3D::walk_grid_build() {
    free(walk_grid); walk_grid = 0;
    walk_grid_w = walk_grid_h = 0;
    if (n_walk_cols == 0) return;

    /* Find world-space bounds of all collision rects */
    float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
    for (int i = 0; i < n_walk_cols; i++) {
        const RdtCollisionRect& r = walk_cols[i];
        float x0 = -(float)r.x;
        float x1 = -(float)(r.x + (s16)r.w);
        float z0 = (float)r.z;
        float z1 = (float)(r.z + (s16)r.h);
        if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
        if (z0 > z1) { float t = z0; z0 = z1; z1 = t; }
        if (x0 < minx) minx = x0; if (x1 > maxx) maxx = x1;
        if (z0 < minz) minz = z0; if (z1 > maxz) maxz = z1;
    }

    /* Add margin */
    minx -= WALK_CELL * 2; minz -= WALK_CELL * 2;
    maxx += WALK_CELL * 2; maxz += WALK_CELL * 2;
    walk_grid_ox = minx;
    walk_grid_oz = minz;
    walk_grid_w = (int)((maxx - minx) / WALK_CELL) + 1;
    walk_grid_h = (int)((maxz - minz) / WALK_CELL) + 1;
    if (walk_grid_w > 2000) walk_grid_w = 2000;
    if (walk_grid_h > 2000) walk_grid_h = 2000;

    int total = walk_grid_w * walk_grid_h;
    walk_grid = (u8*)calloc(total, 1);
    if (!walk_grid) { walk_grid_w = walk_grid_h = 0; return; }

    /* Rasterize each rect into the grid */
    for (int i = 0; i < n_walk_cols; i++) {
        const RdtCollisionRect& r = walk_cols[i];
        float x0 = -(float)r.x;
        float x1 = -(float)(r.x + (s16)r.w);
        float z0 = (float)r.z;
        float z1 = (float)(r.z + (s16)r.h);
        if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
        if (z0 > z1) { float t = z0; z0 = z1; z1 = t; }

        int gx0 = (int)((x0 - walk_grid_ox) / WALK_CELL);
        int gx1 = (int)((x1 - walk_grid_ox) / WALK_CELL);
        int gz0 = (int)((z0 - walk_grid_oz) / WALK_CELL);
        int gz1 = (int)((z1 - walk_grid_oz) / WALK_CELL);
        if (gx0 < 0) gx0 = 0; if (gx1 >= walk_grid_w) gx1 = walk_grid_w - 1;
        if (gz0 < 0) gz0 = 0; if (gz1 >= walk_grid_h) gz1 = walk_grid_h - 1;

        for (int gz = gz0; gz <= gz1; gz++)
            for (int gx = gx0; gx <= gx1; gx++)
                walk_grid[gz * walk_grid_w + gx] = 1;
    }
}

bool ViewerPanel3D::walk_grid_test(float wx, float wz) const {
    if (!walk_grid) return true;  /* no grid = free walk */
    int gx = (int)((wx - walk_grid_ox) / WALK_CELL);
    int gz = (int)((wz - walk_grid_oz) / WALK_CELL);
    if (gx < 0 || gx >= walk_grid_w || gz < 0 || gz >= walk_grid_h)
        return false;
    return walk_grid[gz * walk_grid_w + gx] != 0;
}

void ViewerPanel3D::walk_tick() {
    if (!walk_mode) return;

    const float dt = 0.016f;  /* ~60fps */
    const float GRAVITY = -80000.0f;   /* mm/s² */
    const float JUMP_VEL = 12000.0f;   /* mm/s */
    const float WALK_SPEED = 8000.0f;  /* mm/s */
    const float RUN_SPEED = 20000.0f;  /* mm/s */
    const float EYE_H = 1700.0f;      /* mm */

    /* ── Mouse look: consume accumulated deltas, warp cursor once ── */
    if (walk_mouse_dx != 0 || walk_mouse_dy != 0) {
        int mdx = walk_mouse_dx, mdy = walk_mouse_dy;
        if (mdx > 200) mdx = 200; if (mdx < -200) mdx = -200;
        if (mdy > 200) mdy = 200; if (mdy < -200) mdy = -200;
        walk_mouse_dx = 0;
        walk_mouse_dy = 0;
        walk_yaw -= mdx * 0.15f;
        walk_pitch -= mdy * 0.12f;
        if (walk_pitch > 89.0f) walk_pitch = 89.0f;
        if (walk_pitch < -89.0f) walk_pitch = -89.0f;
        /* Single cursor warp per frame — prevents screen-edge lockup */
        RECT rc; GetClientRect(hwnd, &rc);
        int cx = (rc.right - rc.left) / 2;
        int cy = (rc.bottom - rc.top) / 2;
        POINT ctr = { cx, cy };
        ClientToScreen(hwnd, &ctr);
        SetCursorPos(ctr.x, ctr.y);
        last_mx = cx; last_my = cy;
    }

    /* ── Movement with velocity smoothing ── */
    float speed = walk_keys[4] ? RUN_SPEED : WALK_SPEED;
    float yr = walk_yaw * 3.14159f / 180.0f;

    float fwd_x = sinf(yr);
    float fwd_z = cosf(yr);
    float rgt_x = cosf(yr);
    float rgt_z = -sinf(yr);

    /* Target velocity from input */
    float tvx = 0, tvz = 0;
    if (walk_keys[0]) { tvx += fwd_x; tvz += fwd_z; }
    if (walk_keys[2]) { tvx -= fwd_x; tvz -= fwd_z; }
    if (walk_keys[1]) { tvx += rgt_x; tvz += rgt_z; }
    if (walk_keys[3]) { tvx -= rgt_x; tvz -= rgt_z; }

    float mlen = sqrtf(tvx*tvx + tvz*tvz);
    if (mlen > 0.001f) { tvx = tvx/mlen * speed; tvz = tvz/mlen * speed; }
    else { tvx = 0; tvz = 0; }

    /* Smooth acceleration/deceleration */
    const float ACCEL = 12.0f;   /* approach rate (higher = snappier) */
    const float DECEL = 8.0f;    /* decel rate when releasing keys */
    bool has_input = (tvx != 0 || tvz != 0);
    float rate = has_input ? ACCEL : DECEL;
    float blend = 1.0f - expf(-rate * dt);
    walk_vx += (tvx - walk_vx) * blend;
    walk_vz += (tvz - walk_vz) * blend;

    /* Kill tiny residual drift */
    if (!has_input && walk_vx*walk_vx + walk_vz*walk_vz < 100.0f)
        walk_vx = walk_vz = 0;

    float dx = walk_vx * dt;
    float dz = walk_vz * dt;
    bool moving = (dx != 0 || dz != 0);

    /* ── Jump / Gravity ── */
    if (walk_noclip) {
        /* Noclip: space = up, shift = down, no gravity */
        if (walk_keys[5]) walk_y += speed * dt;  /* space = fly up */
        if (walk_keys[4]) walk_y -= speed * dt;  /* shift = fly down */
        walk_vy = 0;
    } else {
        bool on_ground = (walk_y <= walk_floor_y + EYE_H + 1.0f);
        if (walk_keys[5] && on_ground) walk_vy = JUMP_VEL;
        walk_vy += GRAVITY * dt;
        walk_y += walk_vy * dt;
        if (walk_y < walk_floor_y + EYE_H) {
            walk_y = walk_floor_y + EYE_H;
            walk_vy = 0;
        }
    }

    /* ── XZ collision via grid (O(1) per test) ── */
    if (moving) {
        float nx = walk_x + dx;
        float nz = walk_z + dz;

        if (walk_noclip || !walk_grid) {
            walk_x = nx; walk_z = nz;
        } else if (walk_grid_test(nx, nz)) {
            walk_x = nx; walk_z = nz;
        } else {
            /* Wall slide: try each axis, kill velocity on blocked axis */
            if (walk_grid_test(nx, walk_z))
                walk_x = nx;
            else
                walk_vx *= 0.2f;  /* dampen blocked axis */

            if (walk_grid_test(walk_x, nz))
                walk_z = nz;
            else
                walk_vz *= 0.2f;
        }
    }

    /* ── Update fireballs ── */
    for (int i = 0; i < MAX_FIREBALLS; i++) {
        Fireball& fb = fireballs[i];
        if (!fb.active) continue;
        fb.x += fb.vx * dt;
        fb.y += fb.vy * dt;
        fb.z += fb.vz * dt;
        fb.vy += GRAVITY * 0.15f * dt;  /* light gravity arc */
        fb.life -= dt;
        /* Bounce off floor plane (Y = walk_floor_y) */
        if (fb.y < walk_floor_y + 50.0f) {
            fb.y = walk_floor_y + 50.0f;
            fb.vy = -fb.vy * 0.5f;  /* lose energy on bounce */
            /* Kill if too slow */
            if (fabsf(fb.vy) < 200.0f) fb.active = false;
        }
        if (fb.life <= 0) fb.active = false;
    }

    render();
}

void ViewerPanel3D::on_key_up(int vk) {
    if (walk_mode) {
        if (vk == 'W') walk_keys[0] = false;
        if (vk == 'A') walk_keys[1] = false;
        if (vk == 'S') walk_keys[2] = false;
        if (vk == 'D') walk_keys[3] = false;
        if (vk == VK_SHIFT) walk_keys[4] = false;
        if (vk == VK_SPACE) walk_keys[5] = false;
    }
}

void ViewerPanel3D::on_key(int vk) {
    /* Walk mode intercepts WASD + shift + space + ESC */
    if (walk_mode) {
        if (vk == 'W') { walk_keys[0] = true; return; }
        if (vk == 'A') { walk_keys[1] = true; return; }
        if (vk == 'S') { walk_keys[2] = true; return; }
        if (vk == 'D') { walk_keys[3] = true; return; }
        if (vk == VK_SHIFT) { walk_keys[4] = true; return; }
        if (vk == VK_SPACE) { walk_keys[5] = true; return; }
        if (vk == 'P') {
            walk_noclip = !walk_noclip;
            return;
        }
        if (vk == VK_ESCAPE || vk == 'F') {
            /* Exit walk mode */
            walk_mode = false;
            walk_noclip = false;
            memset(walk_keys, 0, sizeof(walk_keys));
            KillTimer(hwnd, 9002);
            ReleaseCapture();
            ShowCursor(TRUE);
            dragging = false; panning = false;
            render();
            return;
        }
    }
    /* On a shot, WASD / arrows walk the player while held */
    if (cam_sel >= 0 && shot_cur >= 0 && !anim_model &&
        (vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' ||
         vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)) {
        if (!shot_moving) {
            shot_moving = true;
            shot_tick_ms = GetTickCount() - 16;
            SetTimer(hwnd, 9003, 16, NULL);
            shot_tick();
        }
        return;
    }
    switch (vk) {
    case 'J':
        /* Back onto the shot after orbiting or zooming away */
        if (cam_sel >= 0 && shot_cur >= 0) {
            shot_zoom = 1;
            update_shot(true);
            render();
        }
        break;
    case 'F':
        walk_mode = !walk_mode;
        if (walk_mode) {
            /* Enter walk mode: try to start at a character spawn or object
               spawn point from the level's SCD data. Fall back to orbit
               camera target if no spawns exist. */
            float spawn_x = cam_x, spawn_z = cam_z, spawn_y = 0.0f;
            bool found_spawn = false;

            /* Prefer character placements (player start) */
            if (n_walk_chars > 0) {
                spawn_x = -(float)walk_chars[0].px;
                spawn_y = -(float)walk_chars[0].py;
                spawn_z =  (float)walk_chars[0].pz;
                found_spawn = true;
            } else if (n_walk_spawns > 0) {
                spawn_x = -(float)walk_spawns[0].px;
                spawn_y = -(float)walk_spawns[0].py;
                spawn_z =  (float)walk_spawns[0].pz;
                found_spawn = true;
            }

            walk_x = spawn_x;
            walk_z = spawn_z;
            walk_floor_y = found_spawn ? spawn_y : 0.0f;
            walk_y = walk_floor_y + 1700.0f;  /* eye height above floor */
            walk_vy = 0.0f;
            walk_vx = 0.0f; walk_vz = 0.0f;
            walk_mouse_dx = 0; walk_mouse_dy = 0;
            walk_yaw = cam_yaw; walk_pitch = 0;
            walk_noclip = false;
            walk_rect = walk_find_rect(walk_x, walk_z, walk_cols, n_walk_cols);
            memset(walk_keys, 0, sizeof(walk_keys));
            memset(fireballs, 0, sizeof(fireballs));
            SetTimer(hwnd, 9002, 16, NULL);  /* ~60fps tick */
            /* Capture mouse + hide cursor for FPS-style look */
            SetCapture(hwnd);
            ShowCursor(FALSE);
            /* Center the cursor so first frame doesn't jump */
            RECT rc; GetClientRect(hwnd, &rc);
            POINT ctr = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
            ClientToScreen(hwnd, &ctr);
            SetCursorPos(ctr.x, ctr.y);
            last_mx = (rc.right - rc.left) / 2;
            last_my = (rc.bottom - rc.top) / 2;
        } else {
            KillTimer(hwnd, 9002);
            memset(walk_keys, 0, sizeof(walk_keys));
            /* Release mouse + show cursor */
            ReleaseCapture();
            ShowCursor(TRUE);
            dragging = false; panning = false;
        }
        render();
        break;
    case 'W': if (!walk_mode) { show_wireframe = !show_wireframe; render(); } break;
    case 'L': show_lighting = !show_lighting; render(); break;
    case 'T': show_textured = !show_textured; render(); break;
    case 'V': show_vcolors = !show_vcolors; render(); break;
    case 'B': show_bones = !show_bones; render(); break;
    case 'O': show_overlay = !show_overlay; render(); break;
    case 'I':
        if (n_room_lights > 0) {
            use_room_lights = !use_room_lights;
            render();
        }
        break;
    case 'N': if (!walk_mode) { show_normals = !show_normals; render(); } break;
    case 'C': show_cull = !show_cull; render(); break;
    case 'G': show_grid = !show_grid; render(); break;
    case 'H': if (gl_alt_id) { show_alt_geo = !show_alt_geo; render(); } break;
    case 'K':
        /* Cycle: all slots, then each camera's setup; Shift+K goes back */
        if (n_cams > 0 && !is_emd) {
            int v = cam_sel + ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            if (v >= n_cams) v = -1;
            if (v < -1) v = n_cams - 1;
            select_camera(v);
        }
        break;
    case 'U': show_uv_editor(); break;
    case 'D': break; /* debug dump removed from hotkey — use Export menu */
    case 'R':
        cam_yaw = 180; cam_pitch = 10; cam_upx = 0; cam_upy = 1; cam_upz = 0;
        shot_locked = false;
        render();
        break;
    case VK_SPACE:
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            /* Ctrl+Space: toggle nav panel (Blender style) */
            g_app.nav_hidden = !g_app.nav_hidden;
            PostMessage(g_app.hMain, WM_SIZE, 0, 0);
        } else {
            anim_toggle_play();
        }
        break;
    case VK_F2: show_hud = !show_hud; render(); break;
    case VK_ESCAPE:
        /* Clear object pick selection */
        if (pick_tri >= 0) {
            pick_tri = -1; pick_section = 0; pick_group_count = 0; pick_info[0] = 0;
            if (gl_pick_id) { glDeleteLists(gl_pick_id, 1); gl_pick_id = 0; }
            render();
        }
        break;
    case VK_F11: {
        /* Cycle render modes: Normal → VM GPU → Software → Normal */
        if (!sw_renderer && !in_vm) {
            /* Normal → VM GPU */
            in_vm = true; sw_renderer = false;
        } else if (in_vm && !sw_renderer) {
            /* VM GPU → Software */
            in_vm = true; sw_renderer = true;
        } else {
            /* Software → Normal */
            in_vm = false; sw_renderer = false;
        }
        wglMakeCurrent(hDC, hRC);
        if (sw_renderer || in_vm) {
            /* Both VM and SW: disable all fragile state, use immediate draw */
            glDisable(GL_LIGHTING);
            glDisable(GL_LIGHT0); glDisable(GL_LIGHT1); glDisable(GL_LIGHT2);
            glDisable(GL_NORMALIZE);
            glDisable(GL_LINE_SMOOTH);
            glDisable(GL_CULL_FACE);
            glDisable(GL_COLOR_MATERIAL);
        } else {
            glEnable(GL_LIGHTING);
            glEnable(GL_LIGHT0); glEnable(GL_LIGHT1); glEnable(GL_LIGHT2);
            glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
            glEnable(GL_COLOR_MATERIAL);
            glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
            glEnable(GL_NORMALIZE);
            glEnable(GL_LINE_SMOOTH);
            if (show_cull) glEnable(GL_CULL_FACE);
        }
        cam_upx = 0; cam_upy = 1; cam_upz = 0;
        wglMakeCurrent(0, 0);
        {
            const char* mode = sw_renderer ? "Software" : in_vm ? "VM GPU" : "Normal";
            char msg[160];
            _snprintf(msg, 159, "GL: %s [%s]  F11=cycle modes",
                gl_renderer_name, mode);
            g_app.set_status(msg);
        }
        render();
    } break;
    case VK_LEFT:
        if (anim_model && anim_model->clip_count > 0)
            anim_set_frame(anim_frame - 1);
        break;
    case VK_RIGHT:
        if (anim_model && anim_model->clip_count > 0)
            anim_set_frame(anim_frame + 1);
        break;
    }
}

LRESULT CALLBACK Viewer3DProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        g_viewer3d.render(); EndPaint(hwnd, &ps); } return 0;
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        g_viewer3d.resize(w, h);
        /* Reposition anim controls on resize  -  only if panel is large enough */
        if (w > 200 && h > 80) {
            if (g_viewer3d.anim_track)
                MoveWindow(g_viewer3d.anim_track, 98, h - 58, w - 180, 24, TRUE);
            if (g_viewer3d.anim_label)
                MoveWindow(g_viewer3d.anim_label, w - 78, h - 54, 74, 18, TRUE);
            if (g_viewer3d.anim_clip_combo)
                MoveWindow(g_viewer3d.anim_clip_combo, 4, h - 58, 90, 300, TRUE);
        }
        g_viewer3d.render();
    } return 0;
    case WM_LBUTTONDOWN: SetFocus(hwnd); g_viewer3d.on_mouse_down(LOWORD(lp), HIWORD(lp), 0); return 0;
    case WM_RBUTTONDOWN: g_viewer3d.on_mouse_down(LOWORD(lp), HIWORD(lp), 1); return 0;
    case WM_LBUTTONUP: g_viewer3d.on_mouse_up(LOWORD(lp), HIWORD(lp), 0); return 0;
    case WM_RBUTTONUP: g_viewer3d.on_mouse_up(LOWORD(lp), HIWORD(lp), 1); return 0;
    case WM_MOUSEMOVE: g_viewer3d.on_mouse_move(LOWORD(lp), HIWORD(lp), (int)wp); return 0;
    case WM_MOUSEWHEEL: g_viewer3d.on_mouse_wheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
    case WM_KEYDOWN: g_viewer3d.on_key((int)wp); return 0;
    case WM_KEYUP:   g_viewer3d.on_key_up((int)wp); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == 9001 && g_viewer3d.anim_playing && g_viewer3d.anim_model
            && g_viewer3d.anim_clip >= 0
            && g_viewer3d.anim_clip < g_viewer3d.anim_model->clip_count) {
            int max_f = g_viewer3d.anim_model->clips[g_viewer3d.anim_clip].frames;
            int next = g_viewer3d.anim_frame + 1;
            if (next >= max_f) next = 0;
            g_viewer3d.anim_set_frame(next);
        }
        if (wp == 9002 && g_viewer3d.walk_mode) {
            g_viewer3d.walk_tick();
        }
        if (wp == 9003) g_viewer3d.shot_tick();
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == g_viewer3d.anim_track) {
            int pos = (int)SendMessageA(g_viewer3d.anim_track, TBM_GETPOS, 0, 0);
            g_viewer3d.anim_set_frame(pos);
        }
        return 0;
    case WM_NOTIFY: {
        /* Overlay tree: a checkbox changes after NM_CLICK / the space key
           is handled, so its new state is read from a posted message */
        NMHDR* nm = (NMHDR*)lp;
        if (!g_viewer3d.ov_tree || nm->hwndFrom != g_viewer3d.ov_tree) break;
        if (nm->code == NM_CLICK) {
            TVHITTESTINFO ht;
            memset(&ht, 0, sizeof(ht));
            GetCursorPos(&ht.pt);
            ScreenToClient(nm->hwndFrom, &ht.pt);
            HTREEITEM h = TreeView_HitTest(nm->hwndFrom, &ht);
            if (h && (ht.flags & TVHT_ONITEMSTATEICON)) PostMessageA(hwnd, WM_OV_TREE_CHECK, 0, (LPARAM)h);
        } else if (nm->code == TVN_KEYDOWN && ((NMTVKEYDOWN*)lp)->wVKey == VK_SPACE) {
            HTREEITEM h = TreeView_GetSelection(nm->hwndFrom);
            if (h) PostMessageA(hwnd, WM_OV_TREE_CHECK, 0, (LPARAM)h);
        } else if (nm->code == TVN_SELCHANGEDA) {
            g_viewer3d.ov_tree_selected(((NMTREEVIEWA*)lp)->itemNew.lParam);
        }
        return 0;
    }
    case WM_OV_TREE_CHECK:
        g_viewer3d.ov_tree_checked((HTREEITEM)lp);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 5001 && HIWORD(wp) == CBN_SELCHANGE) {
            int sel = (int)SendMessageA(g_viewer3d.anim_clip_combo, CB_GETCURSEL, 0, 0);
            if (sel >= 0) g_viewer3d.anim_select_clip(sel - 1); /* 0=bind pose(-1), 1+=clip 0+ */
        }
        return 0;
    /* Dark theme color messages for animation child controls */
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (T.isDark) {
            SetBkColor((HDC)wp, T.listBg);
            SetTextColor((HDC)wp, T.listText);
            return (LRESULT)g_hBrBg;
        }
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/*═══════════════════════════════════════════════════════════════════
 *  Video Panel  -  MPEG-1 movie playback with audio
 *═══════════════════════════════════════════════════════════════════*/
extern VideoPanel g_video;

#define IDC_VIDEO_SLIDER  5001
#define IDC_VIDEO_PLAY    5002
#define IDC_VIDEO_LABEL   5003

bool VideoPanel::load(const u8* data, size_t size) {
    stop();
    cur_frame = 0;
    if (hBmp) { DeleteObject(hBmp); hBmp = 0; dib_bits = 0; }
    dib_w = dib_h = 0;
    video.clear();
    if (!decode_mpeg_video(data, size, video)) return false;
    if (slider) {
        SendMessage(slider, TBM_SETRANGE, TRUE, MAKELONG(0, video.frame_count - 1));
        SendMessage(slider, TBM_SETPOS, TRUE, 0);
    }

    /* Create persistent DIB matching video dimensions */
    if (video.width > 0 && video.height > 0) {
        BITMAPINFO bmi; memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = video.width;
        bmi.bmiHeader.biHeight = -video.height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        hBmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
        if (hBmp) { dib_w = video.width; dib_h = video.height; }
    }

    show_frame(0);
    return true;
}

void VideoPanel::show_frame(int idx) {
    if (idx < 0 || idx >= video.frame_count) return;
    cur_frame = idx;

    int w = video.frames[idx].width;
    int h = video.frames[idx].height;
    if (w <= 0 || h <= 0 || !video.frames[idx].rgba) return;

    /* Update persistent DIB pixels  -  no GDI object recreation */
    if (hBmp && dib_bits && dib_w == w && dib_h == h) {
        const u8* src = video.frames[idx].rgba;
        u8* dst = (u8*)dib_bits;
        int npix = w * h;
        for (int i = 0; i < npix; i++) {
            dst[i*4+0] = src[i*4+2]; /* B */
            dst[i*4+1] = src[i*4+1]; /* G */
            dst[i*4+2] = src[i*4+0]; /* R */
            dst[i*4+3] = 255;
        }
    }

    if (slider) SendMessage(slider, TBM_SETPOS, TRUE, idx);
    if (label) {
        char buf[64];
        _snprintf(buf, 63, "Frame %d / %d", idx + 1, video.frame_count);
        SetWindowTextA(label, buf);
    }
    if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
}

void VideoPanel::start_audio() {
    if (!video.has_audio || !video.audio_pcm || video.audio_samples <= 0) return;

    /* Calculate audio offset based on current video frame */
    double time_offset = (double)cur_frame / (double)video.fps;
    int sample_offset = (int)(time_offset * video.audio_samplerate);
    if (sample_offset >= video.audio_samples) return;

    int remaining = video.audio_samples - sample_offset;

    WAVEFORMATEX wfx; memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = video.audio_samplerate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = video.audio_samplerate * 4;

    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, 0) != MMSYSERR_NOERROR) return;

    memset(&wave_hdr, 0, sizeof(wave_hdr));
    wave_hdr.lpData = (LPSTR)(video.audio_pcm + sample_offset * 2);
    wave_hdr.dwBufferLength = remaining * 4; /* stereo s16 = 4 bytes per frame */
    waveOutPrepareHeader(hWaveOut, &wave_hdr, sizeof(wave_hdr));
    waveOutWrite(hWaveOut, &wave_hdr, sizeof(wave_hdr));

    audio_start_frame = cur_frame;
}

void VideoPanel::stop_audio() {
    if (hWaveOut) {
        waveOutReset(hWaveOut);
        waveOutUnprepareHeader(hWaveOut, &wave_hdr, sizeof(wave_hdr));
        waveOutClose(hWaveOut);
        hWaveOut = 0;
    }
}

void VideoPanel::play() {
    if (video.frame_count < 2) return;
    playing = true;
    int ms = (int)(1000.0f / video.fps);
    if (ms < 1) ms = 1;
    timer_id = SetTimer(hwnd, 7001, ms, NULL);
    if (play_btn) SetWindowTextA(play_btn, "||");
    start_audio();
}

void VideoPanel::stop() {
    playing = false;
    if (timer_id && hwnd) KillTimer(hwnd, 7001);
    timer_id = 0;
    if (play_btn) SetWindowTextA(play_btn, ">");
    stop_audio();
}

void VideoPanel::toggle_play() {
    if (playing) stop(); else play();
}

void VideoPanel::on_timer() {
    if (!playing) return;
    int next = cur_frame + 1;
    if (next >= video.frame_count) { stop(); next = 0; }
    show_frame(next);
}

void VideoPanel::paint(HDC hdc, RECT& rc) {
    /* Double-buffered paint to eliminate flicker */
    int pw = rc.right - rc.left;
    int ph = rc.bottom - rc.top;
    HDC bufdc = CreateCompatibleDC(hdc);
    HBITMAP bufbmp = CreateCompatibleBitmap(hdc, pw, ph);
    HBITMAP oldbuf = (HBITMAP)SelectObject(bufdc, bufbmp);

    /* Fill background */
    HBRUSH bg = CreateSolidBrush(T.panelBg);
    FillRect(bufdc, &rc, bg);
    DeleteObject(bg);

    if (!hBmp || video.frame_count == 0) {
        SetBkMode(bufdc, TRANSPARENT);
        SetTextColor(bufdc, T.text);
        DrawTextA(bufdc, "No video loaded", -1, &rc, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    } else {
        /* Draw current frame centered, aspect-correct */
        int ch = ph - 40; /* leave room for controls */
        if (ch < 10) ch = 10;
        int vw = video.width, vh = video.height;
        float sx = (float)pw / vw, sy = (float)ch / vh;
        float s = (sx < sy) ? sx : sy;
        int dw = (int)(vw * s), dh = (int)(vh * s);
        int dx = rc.left + (pw - dw) / 2;
        int dy = rc.top + (ch - dh) / 2;

        HDC memdc = CreateCompatibleDC(bufdc);
        HBITMAP old = (HBITMAP)SelectObject(memdc, hBmp);
        SetStretchBltMode(bufdc, HALFTONE);
        StretchBlt(bufdc, dx, dy, dw, dh, memdc, 0, 0, vw, vh, SRCCOPY);
        SelectObject(memdc, old);
        DeleteDC(memdc);
    }

    /* Copy buffer to screen */
    BitBlt(hdc, 0, 0, pw, ph, bufdc, 0, 0, SRCCOPY);
    SelectObject(bufdc, oldbuf);
    DeleteObject(bufbmp);
    DeleteDC(bufdc);
}

LRESULT CALLBACK VideoPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_video.hwnd = hwnd;
        g_video.play_btn = CreateWindowA("BUTTON", ">", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            4, 4, 32, 24, hwnd, (HMENU)IDC_VIDEO_PLAY, GetModuleHandle(0), 0);
        g_video.slider = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD|WS_VISIBLE|TBS_NOTICKS,
            40, 4, 300, 24, hwnd, (HMENU)IDC_VIDEO_SLIDER, GetModuleHandle(0), 0);
        g_video.label = CreateWindowA("STATIC", "No video", WS_CHILD|WS_VISIBLE|SS_LEFT,
            348, 8, 160, 20, hwnd, (HMENU)IDC_VIDEO_LABEL, GetModuleHandle(0), 0);
        return 0;

    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        int cy = h - 30;
        if (cy < 0) cy = 0;
        if (g_video.play_btn) MoveWindow(g_video.play_btn, 4, cy, 32, 24, TRUE);
        if (g_video.slider) MoveWindow(g_video.slider, 40, cy, w - 220, 24, TRUE);
        if (g_video.label) MoveWindow(g_video.label, w - 170, cy + 4, 160, 20, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        g_video.paint(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; /* suppress background erase  -  we paint everything */

    case WM_TIMER:
        if (wp == 7001) g_video.on_timer();
        return 0;

    case WM_HSCROLL:
        if ((HWND)lp == g_video.slider) {
            int pos = (int)SendMessage(g_video.slider, TBM_GETPOS, 0, 0);
            if (pos != g_video.cur_frame) {
                if (g_video.playing) g_video.stop();
                g_video.show_frame(pos);
            }
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_VIDEO_PLAY)
            g_video.toggle_play();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_SPACE) g_video.toggle_play();
        else if (wp == VK_LEFT) {
            g_video.stop();
            g_video.show_frame(g_video.cur_frame > 0 ? g_video.cur_frame - 1 : 0);
        }
        else if (wp == VK_RIGHT) {
            g_video.stop();
            g_video.show_frame(g_video.cur_frame < g_video.video.frame_count - 1 ?
                              g_video.cur_frame + 1 : g_video.cur_frame);
        }
        return 0;

    case WM_DESTROY:
        g_video.stop();
        g_video.hwnd = 0;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/*=================================================================
 *  Save Editor Panel  -  Dino Crisis save file editing
 *=================================================================*/
extern SavePanel g_save;

/* Brush for save panel dark bg (created once) */
static HBRUSH s_saveBrBg = NULL;
static HBRUSH s_saveBrEdit = NULL;

/* Control IDs */
#define IDC_SAVE_DIFF     6001
#define IDC_SAVE_ROOM     6002
#define IDC_SAVE_PRESET   6003
#define IDC_SAVE_TIME_H   6004
#define IDC_SAVE_TIME_M   6005
#define IDC_SAVE_TIME_S   6006
#define IDC_SAVE_CONT     6007
#define IDC_SAVE_ARR      6008
#define IDC_SAVE_BTN      6009
#define IDC_SAVE_INV_BASE 6100
#define IDC_SAVE_QTY_BASE 6120
#define IDC_SAVE_FLAG_BASE 6200
#define IDC_SAVE_PRESET_BTN 6300
#define IDC_SAVE_CK_LABEL  6301

/* Track all child controls for scroll repositioning */
struct SaveCtrl { HWND hw; int base_y; };
static SaveCtrl s_ctrls[128];
static int s_ctrl_count = 0;
static int s_scroll_pos = 0;

/* Subclass proc for dark combo boxes - paints the closed state dark */
static LRESULT CALLBACK DarkComboProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR id, DWORD_PTR ref) {
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_PAINT && T.isDark) {
        HDC hdc = GetDC(hwnd);
        RECT rc; GetClientRect(hwnd, &rc);

        /* Paint the selected item text area */
        RECT textRc = rc;
        int btnW = GetSystemMetrics(SM_CXVSCROLL);
        textRc.right -= btnW;
        textRc.left += 1; textRc.top += 1; textRc.bottom -= 1;
        HBRUSH bg = CreateSolidBrush(T.listBg);
        FillRect(hdc, &textRc, bg);
        DeleteObject(bg);

        /* Draw selected text */
        int sel = (int)SendMessage(hwnd, CB_GETCURSEL, 0, 0);
        if (sel >= 0) {
            char txt[256] = {0};
            SendMessageA(hwnd, CB_GETLBTEXT, sel, (LPARAM)txt);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, T.listText);
            HFONT hf = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
            HFONT old = hf ? (HFONT)SelectObject(hdc, hf) : NULL;
            textRc.left += 3;
            DrawTextA(hdc, txt, -1, &textRc, DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            if (old) SelectObject(hdc, old);
        }

        /* Paint the drop-down button area dark */
        RECT btnRc = rc;
        btnRc.left = rc.right - btnW;
        HBRUSH btnBg = CreateSolidBrush(T.panelBg);
        FillRect(hdc, &btnRc, btnBg);
        DeleteObject(btnBg);

        /* Draw a small arrow triangle */
        int cx = (btnRc.left + btnRc.right) / 2;
        int cy = (btnRc.top + btnRc.bottom) / 2;
        POINT arrow[3] = {{cx-4, cy-2}, {cx+4, cy-2}, {cx, cy+3}};
        HBRUSH arBr = CreateSolidBrush(T.text);
        HPEN arPen = CreatePen(PS_SOLID, 1, T.text);
        HBRUSH ob = (HBRUSH)SelectObject(hdc, arBr);
        HPEN op = (HPEN)SelectObject(hdc, arPen);
        Polygon(hdc, arrow, 3);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(arBr); DeleteObject(arPen);

        /* Border */
        HPEN bPen = CreatePen(PS_SOLID, 1, T.border);
        HPEN obp = (HPEN)SelectObject(hdc, bPen);
        HBRUSH nb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, obp); SelectObject(hdc, nb);
        DeleteObject(bPen);

        ReleaseDC(hwnd, hdc);
    }
    return r;
}

static HWND sc_create(HWND parent, const char* cls, const char* text, DWORD style,
                      int x, int y, int w, int h, int id) {
    HWND hw = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, parent, (HMENU)(intptr_t)id, GetModuleHandle(0), 0);
    if (s_ctrl_count < 128) {
        s_ctrls[s_ctrl_count].hw = hw;
        s_ctrls[s_ctrl_count].base_y = y;
        s_ctrl_count++;
    }
    /* Subclass combos for dark painting */
    if (T.isDark && strcmp(cls, "COMBOBOX") == 0)
        SetWindowSubclass(hw, DarkComboProc, 0, 0);
    return hw;
}

static void sc_label(HWND parent, const char* text, int x, int y, int w, int h) {
    sc_create(parent, "STATIC", text, SS_LEFT, x, y, w, h, 0);
}

static void reposition_controls() {
    HDWP dwp = BeginDeferWindowPos(s_ctrl_count);
    for (int i = 0; i < s_ctrl_count; i++) {
        RECT r; GetWindowRect(s_ctrls[i].hw, &r);
        POINT pt = {r.left, 0}; ScreenToClient(GetParent(s_ctrls[i].hw), &pt);
        int w = r.right - r.left, h = r.bottom - r.top;
        dwp = DeferWindowPos(dwp, s_ctrls[i].hw, NULL,
            pt.x, s_ctrls[i].base_y - s_scroll_pos, w, h,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    EndDeferWindowPos(dwp);
}

bool SavePanel::load(const u8* data, u32 size, const char* fname) {
    if (!dc_save_parse(data, size, save)) return false;
    strncpy(filename, fname, MAX_PATH - 1);
    dirty = false;
    update_controls();
    return true;
}

void SavePanel::save_file() {
    read_controls();
    dc_save_build(save);

    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "Dino Crisis Save (*.dns)\0*.dns\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Save Dino Crisis Save File";
    ofn.lpstrDefExt = "dns";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    strncpy(path, filename, MAX_PATH - 1);

    if (GetSaveFileNameA(&ofn)) {
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(save.raw, 1, DC_SAVE_SIZE, f);
            fclose(f);
            dirty = false;
            if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
        }
    }
}

void SavePanel::read_controls() {
    if (diff_combo) save.difficulty = (u8)SendMessage(diff_combo, CB_GETCURSEL, 0, 0);
    if (arrange_chk) save.arrange = (SendMessage(arrange_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (continues_edit) {
        char buf[16]; GetWindowTextA(continues_edit, buf, 16);
        save.continues = (u8)atoi(buf);
    }
    if (time_h) { char b[8]; GetWindowTextA(time_h, b, 8); save.hours = atoi(b); }
    if (time_m) { char b[8]; GetWindowTextA(time_m, b, 8); save.minutes = atoi(b); }
    if (time_s) { char b[8]; GetWindowTextA(time_s, b, 8); save.seconds = atoi(b); }

    for (int i = 0; i < 10; i++) {
        if (inv_item[i]) {
            int sel = (int)SendMessage(inv_item[i], CB_GETCURSEL, 0, 0);
            if (sel == 0) { save.inv[i].id = 0; save.inv[i].qty = 0; save.inv[i].fl = 0; }
            else if (sel > 0 && sel <= DC_SUPPLY_COUNT) {
                save.inv[i].id = DC_SUPPLY_IDS[sel - 1];
                if (save.inv[i].fl == 0) save.inv[i].fl = 1;
            }
        }
        if (inv_qty[i]) {
            char b[8]; GetWindowTextA(inv_qty[i], b, 8);
            save.inv[i].qty = (u8)atoi(b);
        }
    }
}

void SavePanel::update_controls() {
    if (diff_combo) SendMessage(diff_combo, CB_SETCURSEL, save.difficulty, 0);
    if (arrange_chk) SendMessage(arrange_chk, BM_SETCHECK, save.arrange ? BST_CHECKED : BST_UNCHECKED, 0);
    if (continues_edit) { char b[8]; _snprintf(b, 7, "%d", save.continues); SetWindowTextA(continues_edit, b); }
    if (time_h) { char b[8]; _snprintf(b, 7, "%d", save.hours); SetWindowTextA(time_h, b); }
    if (time_m) { char b[8]; _snprintf(b, 7, "%d", save.minutes); SetWindowTextA(time_m, b); }
    if (time_s) { char b[8]; _snprintf(b, 7, "%d", save.seconds); SetWindowTextA(time_s, b); }

    if (room_combo) {
        int sel = -1;
        for (int i = 0; i < DC_ROOM_COUNT; i++)
            if (DC_ROOMS[i].rm == save.room && DC_ROOMS[i].stg == save.stage) { sel = i; break; }
        SendMessage(room_combo, CB_SETCURSEL, sel >= 0 ? sel : 0, 0);
    }

    for (int i = 0; i < 10; i++) {
        if (inv_item[i]) {
            int sel = 0;
            for (int j = 0; j < DC_SUPPLY_COUNT; j++)
                if (DC_SUPPLY_IDS[j] == save.inv[i].id) { sel = j + 1; break; }
            SendMessage(inv_item[i], CB_SETCURSEL, sel, 0);
        }
        if (inv_qty[i]) {
            char b[8]; _snprintf(b, 7, "%d", save.inv[i].qty);
            SetWindowTextA(inv_qty[i], b);
        }
    }

    for (int i = 0; i < DC_FLAG_ITEM_COUNT && i < 16; i++) {
        if (flag_chk[i])
            SendMessage(flag_chk[i], BM_SETCHECK,
                dc_flag_item_on(save, i) ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    /* Update checksum display */
    if (ck_label && save.valid) {
        const u8* g = save.raw + 0x100;
        u32 ck1_stored = (u32)g[0] | ((u32)g[1]<<8) | ((u32)g[2]<<16) | ((u32)g[3]<<24);
        u32 ck2_stored = (u32)g[4] | ((u32)g[5]<<8) | ((u32)g[6]<<16) | ((u32)g[7]<<24);
        u32 ck1_calc = dc_checksum(save.raw, DC_GS_OFFSET+8, DC_GS_SIZE-8);
        u32 ck2_calc = dc_checksum(save.raw, 0x108, 120);
        char ckbuf[128];
        _snprintf(ckbuf, 127, "CK1: %08X %s   CK2: %08X %s   (calc: %08X / %08X)",
            ck1_stored, (ck1_stored == ck1_calc) ? "OK" : "BAD",
            ck2_stored, (ck2_stored == ck2_calc) ? "OK" : "BAD",
            ck1_calc, ck2_calc);
        SetWindowTextA(ck_label, ckbuf);
    }

    if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
}

void SavePanel::apply_preset(int idx) {
    if (idx < 0 || idx >= DC_PRESET_COUNT) return;
    read_controls();
    save.weapon_mask = DC_PRESETS[idx].wm;
    memcpy(save.ev_flags, DC_PRESETS[idx].flags, 96);
    dirty = true;
    update_controls();
}

void SavePanel::apply_room(int idx) {
    if (idx < 0 || idx >= DC_ROOM_COUNT) return;
    read_controls();
    save.room = DC_ROOMS[idx].rm; save.stage = DC_ROOMS[idx].stg;
    save.px = DC_ROOMS[idx].x; save.pz = DC_ROOMS[idx].z;
    save.py = DC_ROOMS[idx].y; save.prot = DC_ROOMS[idx].rot;
    dirty = true;
    update_controls();
}

void SavePanel::toggle_flag(int idx) {
    read_controls();
    dc_flag_item_toggle(save, idx, !dc_flag_item_on(save, idx));
    dirty = true;
    update_controls();
}

void SavePanel::paint(HDC hdc, RECT& rc) {
    if (!s_saveBrBg) s_saveBrBg = CreateSolidBrush(T.panelBg);
    FillRect(hdc, &rc, s_saveBrBg);

    if (!save.valid) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, T.text);
        DrawTextA(hdc, "No save file loaded", -1, &rc, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
}



/*═══════════════════════════════════════════════════════════════════
 *  SavePanel dark theme finalization with progress popup.
 *  Uses a top-level WS_POPUP so no sibling/child clipping can
 *  stomp it.  Each ApplyDarkToControl takes ~150ms.
 *═══════════════════════════════════════════════════════════════════*/
static void sp_pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* Simple WndProc for the progress popup — just paints background */
static COLORREF s_prog_bg;
static COLORREF s_prog_fg;
static LRESULT CALLBACK ProgPopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH hbr = CreateSolidBrush(s_prog_bg);
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);

        /* Draw border */
        HPEN hp = CreatePen(PS_SOLID, 1, T.isDark ? RGB(70,70,80) : RGB(180,180,190));
        HGDIOBJ old_pen = SelectObject(hdc, hp);
        HGDIOBJ old_br = SelectObject(hdc, (HBRUSH)GetStockObject(NULL_BRUSH));
        Rectangle(hdc, 0, 0, rc.right, rc.bottom);
        SelectObject(hdc, old_pen); SelectObject(hdc, old_br);
        DeleteObject(hp);

        /* Title text */
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, s_prog_fg);
        HFONT hf = CreateFontA(-15, 0,0,0, FW_SEMIBOLD, 0,0,0,
            DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, "Segoe UI");
        if (!hf) hf = CreateFontA(-15, 0,0,0, FW_BOLD, 0,0,0,
            DEFAULT_CHARSET,0,0, DEFAULT_QUALITY, DEFAULT_PITCH|FF_SWISS, "Tahoma");
        HFONT old = (HFONT)SelectObject(hdc, hf);
        RECT tr = { 0, 14, rc.right, 38 };
        DrawTextA(hdc, "Building Save Editor...", -1, &tr,
            DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, old);
        DeleteObject(hf);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_CTLCOLORSTATIC) {
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, s_prog_fg);
        static HBRUSH s_pBrush = NULL;
        if (!s_pBrush) s_pBrush = CreateSolidBrush(s_prog_bg);
        return (LRESULT)s_pBrush;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void SavePanel_FinalizeDark(HWND hPanelArea)
{
    if (!T.isDark || s_ctrl_count == 0) return;

    /* Register popup class (once) */
    static BOOL reg = FALSE;
    if (!reg) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ProgPopupProc;
        wc.hInstance = GetModuleHandle(0);
        wc.hCursor = LoadCursor(NULL, IDC_WAIT);
        wc.lpszClassName = "DCProgressPopup";
        if (RegisterClassExA(&wc)) reg = TRUE;
    }

    s_prog_bg = T.isDark ? RGB(28, 28, 34) : RGB(245, 245, 248);
    s_prog_fg = T.isDark ? RGB(210, 210, 215) : RGB(30, 30, 35);

    /* Position popup centered over the panel area */
    RECT par;
    GetWindowRect(hPanelArea, &par);
    int popW = 320, popH = 100;
    int popX = par.left + (par.right - par.left - popW) / 2;
    int popY = par.top + (par.bottom - par.top - popH) / 2;

    HWND hPopup = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "DCProgressPopup", "",
        WS_POPUP | WS_VISIBLE,
        popX, popY, popW, popH,
        g_app.hMain, NULL, GetModuleHandle(0), 0);

    /* Progress bar inside popup */
    int barW = 280, barH = 16;
    int barX = (popW - barW) / 2, barY = 46;
    HWND hProg = CreateWindowExA(0, PROGRESS_CLASSA, "",
        WS_CHILD | WS_VISIBLE,
        barX, barY, barW, barH,
        hPopup, NULL, GetModuleHandle(0), 0);

    int total = s_ctrl_count + 2;
    SendMessage(hProg, PBM_SETRANGE32, 0, total);
    SendMessage(hProg, PBM_SETBARCOLOR, 0, (LPARAM)T.accent);
    SendMessage(hProg, PBM_SETBKCOLOR, 0, (LPARAM)(T.isDark ? RGB(50,50,58) : RGB(220,220,228)));

    /* Percentage label */
    HWND hPct = CreateWindowExA(0, "STATIC", "0%",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        barX, barY + barH + 6, barW, 18,
        hPopup, NULL, GetModuleHandle(0), 0);
    HFONT hfSmall = CreateFontA(-11, 0,0,0, FW_NORMAL, 0,0,0,
        DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, "Segoe UI");
    if (hfSmall) SendMessage(hPct, WM_SETFONT, (WPARAM)hfSmall, FALSE);

    /* Force initial paint of popup */
    UpdateWindow(hPopup);
    RedrawWindow(hPopup, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    sp_pump();

    /* Apply dark theme: panel first */
    ApplyDarkToControl(g_save.hwnd);
    SendMessage(hProg, PBM_SETPOS, 1, 0);
    sp_pump();

    /* Apply dark theme in batches of 8 for better responsiveness.
       SetWindowTheme is ~50-300ms per control on Win10/11. */
    for (int i = 0; i < s_ctrl_count; i++) {
        ApplyDarkToControl(s_ctrls[i].hw);

        /* Update progress every 4 controls to reduce overhead */
        if ((i & 3) == 3 || i == s_ctrl_count - 1) {
            int pos = i + 2;
            SendMessage(hProg, PBM_SETPOS, pos, 0);
            int pct = (pos * 100) / total;
            char buf[8];
            _snprintf(buf, 7, "%d%%", pct);
            SetWindowTextA(hPct, buf);
            RedrawWindow(hPct, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            RedrawWindow(hProg, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            sp_pump();
        }
    }

    /* Final panel pass */
    ApplyDarkToControl(g_save.hwnd);
    SendMessage(hProg, PBM_SETPOS, total, 0);
    SetWindowTextA(hPct, "100%");
    RedrawWindow(hPopup, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    sp_pump();

    /* Cleanup */
    if (hfSmall) DeleteObject(hfSmall);
    DestroyWindow(hPopup);
}

LRESULT CALLBACK SavePanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_save.hwnd = hwnd;
        s_ctrl_count = 0;
        s_scroll_pos = 0;

        /* NOTE: ApplyDarkToControl is deferred to SavePanel_FinalizeDark()
           which runs post-creation with a progress overlay.  This is because
           each SetWindowTheme/AllowDarkModeForWindow call takes ~150ms and
           doing ~80 of them synchronously here freezes the UI for 10-20s. */

        int y = 8, lh = 20, gap = 4, col1 = 8, col2 = 110;
        int w_full = 420;

        sc_label(hwnd, "DINO CRISIS SAVE EDITOR", col1, y, 300, lh);
        y += lh + 2;

        /* Checksum display */
        g_save.ck_label = sc_create(hwnd, "STATIC", "CK1: ----  CK2: ----", SS_LEFT,
            col1, y, w_full, lh, IDC_SAVE_CK_LABEL);
        y += lh + gap;

        /* Difficulty + Arrange */
        sc_label(hwnd, "Difficulty:", col1, y+2, 90, lh);
        g_save.diff_combo = sc_create(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,
            col2, y, 120, 200, IDC_SAVE_DIFF);
        for (int i = 0; i < 4; i++) SendMessageA(g_save.diff_combo, CB_ADDSTRING, 0, (LPARAM)DC_DIFF_NAMES[i]);
        g_save.arrange_chk = sc_create(hwnd, "BUTTON", "Arrange", BS_AUTOCHECKBOX,
            col2+130, y+2, 80, lh, IDC_SAVE_ARR);
        y += lh + gap;

        /* Play Time + Continues */
        sc_label(hwnd, "Play Time:", col1, y+2, 90, lh);
        g_save.time_h = sc_create(hwnd, "EDIT", "0", WS_BORDER|ES_NUMBER|ES_CENTER,
            col2, y, 32, lh, IDC_SAVE_TIME_H);
        sc_label(hwnd, ":", col2+34, y+2, 8, lh);
        g_save.time_m = sc_create(hwnd, "EDIT", "0", WS_BORDER|ES_NUMBER|ES_CENTER,
            col2+44, y, 32, lh, IDC_SAVE_TIME_M);
        sc_label(hwnd, ":", col2+78, y+2, 8, lh);
        g_save.time_s = sc_create(hwnd, "EDIT", "0", WS_BORDER|ES_NUMBER|ES_CENTER,
            col2+88, y, 32, lh, IDC_SAVE_TIME_S);
        sc_label(hwnd, "Continues:", col2+140, y+2, 70, lh);
        g_save.continues_edit = sc_create(hwnd, "EDIT", "0", WS_BORDER|ES_NUMBER|ES_CENTER,
            col2+216, y, 40, lh, IDC_SAVE_CONT);
        y += lh + gap + 4;

        /* Room */
        sc_label(hwnd, "Room:", col1, y+2, 50, lh);
        g_save.room_combo = sc_create(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,
            col1+55, y, w_full-55, 400, IDC_SAVE_ROOM);
        for (int i = 0; i < DC_ROOM_COUNT; i++) {
            char buf[128];
            const char* stg = (DC_ROOMS[i].stg >= 1 && DC_ROOMS[i].stg <= 6) ? DC_STAGE_NAMES[DC_ROOMS[i].stg] : "?";
            _snprintf(buf, 127, "[%s] %s", stg, DC_ROOMS[i].name);
            SendMessageA(g_save.room_combo, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        y += lh + gap + 4;

        /* Weapons & Key Items */
        sc_label(hwnd, "WEAPONS + KEY ITEMS:", col1, y, 240, lh);
        y += lh + 2;
        sc_label(hwnd, "[*] Handgun (always equipped)", col1+4, y, 280, lh);
        y += lh + 2;
        for (int i = 0; i < DC_FLAG_ITEM_COUNT && i < 16; i++) {
            g_save.flag_chk[i] = sc_create(hwnd, "BUTTON", DC_FLAG_ITEMS[i].name,
                BS_AUTOCHECKBOX, col1+4, y, w_full-8, lh, IDC_SAVE_FLAG_BASE + i);
            y += lh + 1;
        }
        y += gap;

        /* Presets */
        sc_label(hwnd, "Progress Preset:", col1, y+2, 110, lh);
        g_save.preset_combo = sc_create(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,
            col2+10, y, w_full-col2-80, 400, IDC_SAVE_PRESET);
        for (int i = 0; i < DC_PRESET_COUNT; i++) {
            char buf[128];
            _snprintf(buf, 127, "#%d %s", DC_PRESETS[i].id, DC_PRESETS[i].name);
            SendMessageA(g_save.preset_combo, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        sc_create(hwnd, "BUTTON", "Apply", BS_PUSHBUTTON,
            w_full-60, y, 60, 22, IDC_SAVE_PRESET_BTN);
        y += lh + gap + 4;

        /* Inventory */
        sc_label(hwnd, "SUPPLY INVENTORY:", col1, y, 200, lh);
        y += lh + 2;
        for (int i = 0; i < 10; i++) {
            char num[4]; _snprintf(num, 3, "%d", i);
            sc_label(hwnd, num, col1, y+2, 14, lh);
            g_save.inv_item[i] = sc_create(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,
                col1+18, y, 250, 400, IDC_SAVE_INV_BASE + i);
            SendMessageA(g_save.inv_item[i], CB_ADDSTRING, 0, (LPARAM)"(Empty)");
            for (int j = 0; j < DC_SUPPLY_COUNT; j++) {
                const char* name = "???";
                for (int k = 0; k < DC_ITEM_COUNT; k++)
                    if (DC_ITEMS[k].id == DC_SUPPLY_IDS[j]) { name = DC_ITEMS[k].name; break; }
                SendMessageA(g_save.inv_item[i], CB_ADDSTRING, 0, (LPARAM)name);
            }
            g_save.inv_qty[i] = sc_create(hwnd, "EDIT", "0", WS_BORDER|ES_NUMBER|ES_CENTER,
                col1+274, y, 36, lh, IDC_SAVE_QTY_BASE + i);
            y += lh + 2;
        }
        y += gap;

        /* Save Button */
        g_save.save_btn = sc_create(hwnd, "BUTTON", "SAVE FILE...", BS_PUSHBUTTON,
            col1, y, w_full, 28, IDC_SAVE_BTN);
        y += 36;

        g_save.content_h = y;

        /* Set font on all controls — dark theming applied by FinalizeDark */
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        for (int i = 0; i < s_ctrl_count; i++)
            SendMessage(s_ctrls[i].hw, WM_SETFONT, (WPARAM)hFont, FALSE);

        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        g_save.paint(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    /* Dark theme colors for child controls */
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
        if (T.isDark) {
            SetBkColor((HDC)wp, T.listBg);
            SetTextColor((HDC)wp, T.listText);
            if (!s_saveBrEdit) s_saveBrEdit = CreateSolidBrush(T.listBg);
            return (LRESULT)s_saveBrEdit;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (T.isDark) {
            SetBkColor((HDC)wp, T.panelBg);
            SetTextColor((HDC)wp, T.text);
            if (!s_saveBrBg) s_saveBrBg = CreateSolidBrush(T.panelBg);
            return (LRESULT)s_saveBrBg;
        }
        break;

    /* Owner-draw combo items (dark themed) */
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lp;
        if (mis->CtlType == ODT_COMBOBOX) {
            mis->itemHeight = 18;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
        if (dis->CtlType == ODT_COMBOBOX) {
            COLORREF bgCol, txCol;
            if (dis->itemState & ODS_SELECTED) {
                bgCol = T.isDark ? T.listSel : GetSysColor(COLOR_HIGHLIGHT);
                txCol = T.isDark ? T.listText : GetSysColor(COLOR_HIGHLIGHTTEXT);
            } else {
                bgCol = T.isDark ? T.listBg : GetSysColor(COLOR_WINDOW);
                txCol = T.isDark ? T.listText : GetSysColor(COLOR_WINDOWTEXT);
            }
            HBRUSH hbr = CreateSolidBrush(bgCol);
            FillRect(dis->hDC, &dis->rcItem, hbr);
            DeleteObject(hbr);

            if (dis->itemID != (UINT)-1) {
                char txt[256] = {0};
                SendMessageA(dis->hwndItem, CB_GETLBTEXT, dis->itemID, (LPARAM)txt);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, txCol);
                RECT tr = dis->rcItem;
                tr.left += 4;
                DrawTextA(dis->hDC, txt, -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            }

            if (dis->itemState & ODS_FOCUS)
                DrawFocusRect(dis->hDC, &dis->rcItem);

            return TRUE;
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == IDC_SAVE_BTN)
            g_save.save_file();
        else if (id == IDC_SAVE_PRESET_BTN) {
            int sel = (int)SendMessage(g_save.preset_combo, CB_GETCURSEL, 0, 0);
            if (sel >= 0) g_save.apply_preset(sel);
        }
        else if (id == IDC_SAVE_ROOM && code == CBN_SELCHANGE) {
            int sel = (int)SendMessage(g_save.room_combo, CB_GETCURSEL, 0, 0);
            if (sel >= 0) g_save.apply_room(sel);
        }
        else if (id >= IDC_SAVE_FLAG_BASE && id < IDC_SAVE_FLAG_BASE + DC_FLAG_ITEM_COUNT)
            g_save.toggle_flag(id - IDC_SAVE_FLAG_BASE);
        else if (code == CBN_SELCHANGE || code == EN_CHANGE)
            g_save.dirty = true;
        return 0;
    }

    case WM_VSCROLL: {
        SCROLLINFO si; memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si); si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int old_pos = si.nPos;
        switch (LOWORD(wp)) {
            case SB_LINEUP:    si.nPos -= 24; break;
            case SB_LINEDOWN:  si.nPos += 24; break;
            case SB_PAGEUP:    si.nPos -= (int)si.nPage; break;
            case SB_PAGEDOWN:  si.nPos += (int)si.nPage; break;
            case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
        }
        int max_pos = si.nMax - (int)si.nPage + 1;
        if (max_pos < 0) max_pos = 0;
        if (si.nPos < 0) si.nPos = 0;
        if (si.nPos > max_pos) si.nPos = max_pos;
        if (si.nPos != old_pos) {
            s_scroll_pos = si.nPos;
            si.fMask = SIF_POS;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            reposition_controls();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        SendMessage(hwnd, WM_VSCROLL, delta > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
        return 0;
    }

    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        SCROLLINFO si; memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE;
        si.nMin = 0;
        si.nMax = g_save.content_h;
        si.nPage = rc.bottom;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        return 0;
    }

    case WM_DESTROY:
        g_save.hwnd = 0;
        if (s_saveBrBg) { DeleteObject(s_saveBrBg); s_saveBrBg = NULL; }
        if (s_saveBrEdit) { DeleteObject(s_saveBrEdit); s_saveBrEdit = NULL; }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/*═══════════════════════════════════════════════════════════════════
 *  Register panel window classes
 *═══════════════════════════════════════════════════════════════════*/
/*═══════════════════════════════════════════════════════════════════
 *  Weapon BIN Editor Panel  —  Full rewrite with dropdowns + aim block
 *═══════════════════════════════════════════════════════════════════*/
extern WeaponPanel g_weapon;

/* Fire mode dropdown items */
static const char* WEP_FIRE_MODES[] = {
    "0 - Single Shot", "1 - Semi-Auto", "2 - Burst",
    "3 - unk3", "4 - unk4", "5 - unk5",
    "6 - Dart (base)", "7 - Dart (alt)", 0
};
/* Ammo type dropdown items */
static const char* WEP_AMMO_TYPES[] = {
    "0 - None", "1 - Dart", "2 - Pistol", "3 - Rifle",
    "4 - Explosive", "5 - Heavy", 0
};

/* Weapon names per file */
static const char* WEP10_NAMES[] = {
    "9mm Handgun", "Glock 40 S&W", "Shotgun Shells",
    "Shotgun Slugs", "Grenade Rounds", "Heat Rounds", 0
};
static const char* WEP11_NAMES[] = {
    "9mm Normal", "9mm Aimed", "9mm Upward", "9mm Crouch",
    "9mm Running", "9mm Strafing", "9mm Critical", "40 S&W Burst",
    "SG Buckshot", "SG Slug", "SG Grenade", "SG Heat", "SG Special", 0
};
static const char* WEP20_NAMES[] = {
    "An. Dart S (Small)", "An. Dart M (Medium)", "An. Dart L / Poison", 0
};
static const char* WEP22_NAMES[] = {
    "Shotgun Bullets", "Slug Bullets", "Grenade Bullets", "Heat Bullets", 0
};
static const char* WEP30_NAMES[] = {
    "Base Weapon", "Explosive Pistol", "Rifle Explosive",
    "Heavy Shotgun", "Heavy Slug", "Heavy Heat",
    "An. Dart Type A", "An. Dart Type B", 0
};

static const char** wep_get_name_table(const char* fname) {
    /* Extract just the filename from full path */
    const char* p = fname;
    const char* last = fname;
    while (*p) { if (*p == '\\' || *p == '/') last = p + 1; p++; }
    /* Build lowercase copy of first 5 chars */
    char lc[8] = {};
    for (int i = 0; i < 5 && last[i]; i++)
        lc[i] = (last[i] >= 'A' && last[i] <= 'Z') ? (last[i] + 32) : last[i];
    if (strcmp(lc, "wep10") == 0 || strcmp(lc, "wep12") == 0) return WEP10_NAMES;
    if (strcmp(lc, "wep11") == 0 || strcmp(lc, "wep13") == 0) return WEP11_NAMES;
    if (strcmp(lc, "wep20") == 0 || strcmp(lc, "wep21") == 0) return WEP20_NAMES;
    if (strcmp(lc, "wep22") == 0 || strcmp(lc, "wep23") == 0) return WEP22_NAMES;
    if (strcmp(lc, "wep30") == 0 || strcmp(lc, "wep31") == 0) return WEP30_NAMES;
    return 0;
}

/* Returns a human-readable weapon identity + variant description */
static const char* wep_get_description(const char* fname) {
    const char* p = fname;
    const char* last = fname;
    while (*p) { if (*p == '\\' || *p == '/') last = p + 1; p++; }
    char lc[8] = {};
    for (int i = 0; i < 5 && last[i]; i++)
        lc[i] = (last[i] >= 'A' && last[i] <= 'Z') ? (last[i] + 32) : last[i];
    if (strcmp(lc, "wep10") == 0) return "Handgun (9mm Parabellum) - Original";
    if (strcmp(lc, "wep12") == 0) return "Handgun (9mm Parabellum) - Arrange";
    if (strcmp(lc, "wep11") == 0) return "Glock (40 S&W) - All Fire Modes - Original";
    if (strcmp(lc, "wep13") == 0) return "Glock (40 S&W) - All Fire Modes - Arrange";
    if (strcmp(lc, "wep20") == 0) return "An. Dart Gun - Original";
    if (strcmp(lc, "wep21") == 0) return "An. Dart Gun - Arrange";
    if (strcmp(lc, "wep22") == 0) return "Shotgun / Grenade / Heat - Original";
    if (strcmp(lc, "wep23") == 0) return "Shotgun / Grenade / Heat - Arrange";
    if (strcmp(lc, "wep30") == 0) return "Heavy Weapons + Darts (Unknown Set A)";
    if (strcmp(lc, "wep31") == 0) return "Heavy Weapons (Unknown Set B)";
    return "Unknown Weapon Overlay";
}

/* Field descriptions for all 16 s16 slots in the 32-byte record */
static const char* WEP_ALL_FIELD_NAMES[16] = {
    "Fire Mode", "pad1", "Ammo Type", "pad3",
    "unk4", "unk5", "Spread", "Recoil",
    "Power", "Range", "unk10", "Flag",
    "Blast Radius", "Blast Falloff", "unk14", "unk15"
};

/* Control IDs, one range per kind; the ranges must not overlap for
   WEP_MAX_RECORDS records (record edits run to 9100 + 16*15 + 15). */
#define WEP_COMBO_MODE   9000   /* + record */
#define WEP_COMBO_AMMO   9020   /* + record */
#define WEP_EDIT_BASE    9100   /* + 16*record + field */
#define WEP_SAVE_BTN     9399
#define WEP_AIM_EDIT     9400   /* + 0..13 */

/* Layout constants */
#define WEP_ROW_H        28
#define WEP_LABEL_W      145
#define WEP_EDIT_W        52
#define WEP_MODE_W       130
#define WEP_AMMO_W       108
#define WEP_PAD           4
#define WEP_SECTION_GAP   12

/* Track which record is "selected" for the dropdown combos */
static int s_wep_sel_rec = 0;

/* Aim/camera block: the first non-pointer word after the pointer tables,
   skipping single nulls between them.  -1 if there is none. */
static int wep_find_aim_block(const u8* data, size_t size, int ptrtbl_start) {
    int o = ptrtbl_start;
    while (o + 4 <= (int)size) {
        u32 v = rd_u32(data + o);
        if ((v & 0xFF000000) != 0x80000000 && v != 0) return o;
        if (v == 0) {
            u32 next = (o + 8 <= (int)size) ? rd_u32(data + o + 4) : 0;
            if ((next & 0xFF000000) != 0x80000000 && next != 0) return o + 4;
        }
        o += 4;
    }
    return -1;
}

bool WeaponPanel::load(const u8* buf, size_t sz, const char* fname) {
    free(data);
    data = (u8*)malloc(sz);
    if (!data) return false;
    memcpy(data, buf, sz);
    data_size = sz;
    strncpy(filename, fname, MAX_PATH - 1);

    code_end = 0;
    for (int o = 0; o <= (int)sz - 4; o += 4) {
        if (rd_u32(data + o) == 0x03E00008) code_end = o + 8;
    }
    if (code_end == 0) code_end = (int)sz;

    ptrtbl_start = code_end;
    for (int o = code_end; o <= (int)sz - 4; o += 4) {
        u32 v = rd_u32(data + o);
        if ((v & 0xFF000000) == 0x80000000) { ptrtbl_start = o; break; }
    }

    int data_sz = ptrtbl_start - code_end;
    n_records = data_sz / 32;
    if (n_records > WEP_MAX_RECORDS) n_records = WEP_MAX_RECORDS;

    /* Trim terminator records (all zeros) */
    for (int i = 0; i < n_records; i++) {
        int off = code_end + i * 32;
        bool all_zero = true;
        for (int f = 0; f < 32; f++)
            if (data[off + f] != 0) { all_zero = false; break; }
        if (all_zero) { n_records = i; break; }
    }

    for (int i = 0; i < n_records; i++) {
        int off = code_end + i * 32;
        for (int f = 0; f < 16; f++)
            records[i].fields[f] = (s16)rd_u16(data + off + f * 2);
    }

    aim_off = wep_find_aim_block(data, data_size, ptrtbl_start);
    s_wep_sel_rec = 0;
    return true;
}

static HWND wep_create_label(HWND parent, const char* text, int x, int y, int w, int h, HFONT font) {
    HWND lbl = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, 0, g_app.hInst, 0);
    SendMessage(lbl, WM_SETFONT, (WPARAM)font, 0);
    return lbl;
}

static HWND wep_create_edit(HWND parent, int val, int x, int y, int w, int id, HFONT font) {
    char buf[16]; _snprintf(buf, 15, "%d", val);
    HWND ed = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", buf,
        WS_CHILD | WS_VISIBLE | ES_CENTER | WS_TABSTOP,
        x, y, w, 22, parent, (HMENU)(LONG_PTR)id, g_app.hInst, 0);
    SendMessage(ed, WM_SETFONT, (WPARAM)font, 0);
    return ed;
}

void WeaponPanel::create_controls() {
    if (!hwnd) return;
    HFONT hFont = g_app.hFontUI;
    const char** names = wep_get_name_table(filename);

    int y = WEP_PAD;
    int x0 = WEP_PAD;

    /* ── Title section ── */
    const char* fn = strrchr(filename, '\\');
    if (!fn) fn = strrchr(filename, '/');
    fn = fn ? fn + 1 : filename;
    const char* desc = wep_get_description(filename);

    /* Line 1: Weapon identity (bold-style, prominent) */
    char title[300];
    _snprintf(title, 299, "%s    %s", fn, desc);
    status_lbl = CreateWindowExA(0, "STATIC", title,
        WS_CHILD | WS_VISIBLE | SS_LEFT, x0, y, 800, 18, hwnd, 0, g_app.hInst, 0);
    HFONT hBold = CreateFontA(-14, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, "Segoe UI");
    if (hBold) SendMessage(status_lbl, WM_SETFONT, (WPARAM)hBold, 0);
    else SendMessage(status_lbl, WM_SETFONT, (WPARAM)hFont, 0);
    y += 20;

    /* Line 2: Technical details (smaller) */
    char detail[200];
    _snprintf(detail, 199, "Code: %d bytes   |   %d weapon records   |   Pointers: 0x%X - 0x%X",
        code_end, n_records, ptrtbl_start, (int)data_size);
    HWND det = CreateWindowExA(0, "STATIC", detail,
        WS_CHILD | WS_VISIBLE | SS_LEFT, x0, y, 600, 16, hwnd, 0, g_app.hInst, 0);
    SendMessage(det, WM_SETFONT, (WPARAM)hFont, 0);
    y += 22;

    /* ── Section: Weapon Records ── */
    wep_create_label(hwnd, "-- Weapon Parameters --", x0, y, 250, 18, hFont);
    y += 20;

    /* Column headers */
    int cols[] = { 6, 7, 8, 9, 11, 12, 13 }; /* field indices to show as edits */
    const char* col_names[] = { "Spread", "Recoil", "Power", "Range", "Flag", "Blast", "Falloff" };
    int n_cols = 7;

    /* Header: Name | Mode | Ammo | Spread | Recoil | Power | Range | Flag | Blast | Falloff */
    int cx = x0 + WEP_LABEL_W;
    wep_create_label(hwnd, "Mode", cx, y, WEP_MODE_W, 16, hFont); cx += WEP_MODE_W + WEP_PAD;
    wep_create_label(hwnd, "Ammo", cx, y, WEP_AMMO_W, 16, hFont); cx += WEP_AMMO_W + WEP_PAD;
    for (int c = 0; c < n_cols; c++) {
        wep_create_label(hwnd, col_names[c], cx, y, WEP_EDIT_W, 16, hFont);
        cx += WEP_EDIT_W + WEP_PAD;
    }
    y += 18;

    /* Per-record rows */
    for (int r = 0; r < n_records; r++) {
        const char* wname = (names && names[r]) ? names[r] : "Record";
        char rlbl[80];
        _snprintf(rlbl, 79, "[%d] %s", r, wname);

        rec_ui[r].lbl = CreateWindowExA(0, "STATIC", rlbl,
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            x0, y + 3, WEP_LABEL_W - 6, 18, hwnd, 0, g_app.hInst, 0);
        SendMessage(rec_ui[r].lbl, WM_SETFONT, (WPARAM)hFont, 0);

        cx = x0 + WEP_LABEL_W;

        /* Fire Mode dropdown */
        {
            HWND combo = CreateWindowExA(0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                cx, y - 1, WEP_MODE_W, 200, hwnd,
                (HMENU)(LONG_PTR)(WEP_COMBO_MODE + r), g_app.hInst, 0);
            SendMessage(combo, WM_SETFONT, (WPARAM)hFont, 0);
            for (int i = 0; WEP_FIRE_MODES[i]; i++)
                SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)WEP_FIRE_MODES[i]);
            int mode_val = records[r].fields[0];
            if (mode_val >= 0 && mode_val <= 7)
                SendMessage(combo, CB_SETCURSEL, mode_val, 0);
            rec_ui[r].edits[0] = combo; /* slot 0 = mode combo */
        }
        cx += WEP_MODE_W + WEP_PAD;

        /* Ammo Type dropdown */
        {
            HWND combo = CreateWindowExA(0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                cx, y - 1, WEP_AMMO_W, 200, hwnd,
                (HMENU)(LONG_PTR)(WEP_COMBO_AMMO + r), g_app.hInst, 0);
            SendMessage(combo, WM_SETFONT, (WPARAM)hFont, 0);
            /* Add standard ammo types + special dart types */
            for (int i = 0; WEP_AMMO_TYPES[i]; i++)
                SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)WEP_AMMO_TYPES[i]);
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"34 - Spec Dart A");
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"36 - Spec Dart B");
            int ammo_val = records[r].fields[2];
            int sel = ammo_val;
            if (ammo_val == 34) sel = 6;
            else if (ammo_val == 36) sel = 7;
            else if (ammo_val > 5) sel = 0;
            SendMessage(combo, CB_SETCURSEL, sel, 0);
            rec_ui[r].edits[1] = combo; /* slot 1 = ammo combo */
        }
        cx += WEP_AMMO_W + WEP_PAD;

        /* Numeric fields: Spread[6], Recoil[7], Power[8], Range[9], Flag[11], Blast[12], Falloff[13] */
        for (int c = 0; c < n_cols; c++) {
            int fi = cols[c];
            int id = WEP_EDIT_BASE + r * 16 + fi;
            rec_ui[r].edits[2 + c] = wep_create_edit(hwnd, records[r].fields[fi],
                cx, y, WEP_EDIT_W, id, hFont);
            cx += WEP_EDIT_W + WEP_PAD;
        }
        y += WEP_ROW_H;
    }

    y += WEP_SECTION_GAP;

    /* ── Section: Aim Parameters (common block found in all weapon files) ── */
    /* Located right after the first PSX pointer table (aim_off, from load):
       8 s16 values: aim_neg_x, aim_base, aim_neg_y, aim_far, aim_near, aim_far2, aim_pos_x, aim_pos_base
       Then 6 s16: camera offsets (3 pairs of s16) */
    memset(aim_ui, 0, sizeof(aim_ui));
    if (aim_off >= 0 && aim_off + 26 <= (int)data_size) {
        wep_create_label(hwnd, "-- Aim / Camera Parameters --", x0, y, 280, 18, hFont);
        y += 20;

        static const char* aim_labels[] = {
            "Aim Neg X", "Aim Base Y", "Aim Neg Y", "Aim Far Dist",
            "Aim Near Dist", "Aim Far Dist 2", "Aim Pos X", "Aim Pos Base"
        };
        for (int i = 0; i < 8 && aim_off + i * 4 + 4 <= (int)data_size; i++) {
            s16 val = (s16)rd_u16(data + aim_off + i * 4);
            int id = WEP_AIM_EDIT + i;
            wep_create_label(hwnd, aim_labels[i], x0, y + 3, 120, 18, hFont);
            aim_ui[i] = wep_create_edit(hwnd, val, x0 + 124, y, 70, id, hFont);
            if (i % 2 == 0 && i + 1 < 8 && aim_off + (i+1) * 4 + 4 <= (int)data_size) {
                /* Put two per row */
                s16 val2 = (s16)rd_u16(data + aim_off + (i+1) * 4);
                wep_create_label(hwnd, aim_labels[i+1], x0 + 210, y + 3, 120, 18, hFont);
                aim_ui[i+1] = wep_create_edit(hwnd, val2, x0 + 334, y, 70, WEP_AIM_EDIT + i + 1, hFont);
                i++; /* skip next in loop */
            }
            y += WEP_ROW_H;
        }

        /* Camera offset block: 6 s16 values */
        int cam_off = aim_off + 32;
        if (cam_off + 12 <= (int)data_size) {
            y += WEP_PAD;
            static const char* cam_labels[] = {
                "Cam Off X+", "Cam Off Y+", "Cam Off Z+",
                "Cam Off X-", "Cam Off Y-", "Cam Off Z-"
            };
            for (int i = 0; i < 6; i += 3) {
                for (int j = 0; j < 3; j++) {
                    s16 val = (s16)rd_u16(data + cam_off + (i + j) * 2);
                    int id = WEP_AIM_EDIT + 8 + i + j;
                    int xp = x0 + j * 160;
                    wep_create_label(hwnd, cam_labels[i + j], xp, y + 3, 90, 18, hFont);
                    aim_ui[8 + i + j] = wep_create_edit(hwnd, val, xp + 94, y, 56, id, hFont);
                }
                y += WEP_ROW_H;
            }
        }
    }

    y += WEP_SECTION_GAP;

    /* ── Save button ── */
    save_btn = CreateWindowExA(0, "BUTTON", "  Save to File  ",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x0, y, 130, 30, hwnd, (HMENU)(LONG_PTR)WEP_SAVE_BTN, g_app.hInst, 0);
    SendMessage(save_btn, WM_SETFONT, (WPARAM)hFont, 0);

    content_h = y + 44;
    scroll_y = 0;
}

void WeaponPanel::update_controls() {
    for (int r = 0; r < n_records; r++) {
        /* Mode combo */
        if (rec_ui[r].edits[0]) {
            int mode_val = records[r].fields[0];
            if (mode_val >= 0 && mode_val <= 7)
                SendMessage(rec_ui[r].edits[0], CB_SETCURSEL, mode_val, 0);
        }
        /* Ammo combo */
        if (rec_ui[r].edits[1]) {
            int ammo_val = records[r].fields[2];
            int sel = ammo_val;
            if (ammo_val == 34) sel = 6;
            else if (ammo_val == 36) sel = 7;
            else if (ammo_val > 5) sel = 0;
            SendMessage(rec_ui[r].edits[1], CB_SETCURSEL, sel, 0);
        }
        /* Numeric fields */
        int cols[] = { 6, 7, 8, 9, 11, 12, 13 };
        for (int c = 0; c < 7; c++) {
            HWND ed = rec_ui[r].edits[2 + c];
            if (!ed) continue;
            char val[16];
            _snprintf(val, 15, "%d", (int)records[r].fields[cols[c]]);
            SetWindowTextA(ed, val);
        }
    }
}

void WeaponPanel::read_controls() {
    int cols[] = { 6, 7, 8, 9, 11, 12, 13 };
    static const int ammo_map[] = { 0, 1, 2, 3, 4, 5, 34, 36 };

    for (int r = 0; r < n_records; r++) {
        /* Mode combo → fields[0] */
        if (rec_ui[r].edits[0]) {
            int sel = (int)SendMessage(rec_ui[r].edits[0], CB_GETCURSEL, 0, 0);
            if (sel >= 0) records[r].fields[0] = (s16)sel;
        }
        /* Ammo combo → fields[2] */
        if (rec_ui[r].edits[1]) {
            int sel = (int)SendMessage(rec_ui[r].edits[1], CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < 8) records[r].fields[2] = (s16)ammo_map[sel];
        }
        /* Numeric fields */
        for (int c = 0; c < 7; c++) {
            HWND ed = rec_ui[r].edits[2 + c];
            if (!ed) continue;
            char val[16] = {};
            GetWindowTextA(ed, val, 15);
            int v = atoi(val);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            records[r].fields[cols[c]] = (s16)v;
        }
    }
}

void WeaponPanel::save_file() {
    if (!data || data_size == 0) return;
    read_controls();

    /* Write weapon records back */
    for (int r = 0; r < n_records; r++) {
        int off = code_end + r * 32;
        if (off + 32 > (int)data_size) break;
        for (int f = 0; f < 16; f++)
            wr_u16(data + off + f * 2, (u16)records[r].fields[f]);
    }

    /* Write aim params back */
    if (aim_off >= 0) {
        for (int i = 0; i < 8 && aim_off + i * 4 + 4 <= (int)data_size; i++) {
            HWND ed = aim_ui[i];
            if (ed) {
                char val[16] = {};
                GetWindowTextA(ed, val, 15);
                wr_u16(data + aim_off + i * 4, (u16)(s16)atoi(val));
            }
        }
        int cam_off = aim_off + 32;
        for (int i = 0; i < 6 && cam_off + i * 2 + 2 <= (int)data_size; i++) {
            HWND ed = aim_ui[8 + i];
            if (ed) {
                char val[16] = {};
                GetWindowTextA(ed, val, 15);
                wr_u16(data + cam_off + i * 2, (u16)(s16)atoi(val));
            }
        }
    }

    char path[MAX_PATH];
    strncpy(path, filename, MAX_PATH - 1);
    if (ui_save_file(g_app.hMain, path, MAX_PATH,
        "BIN Files (*.bin)\0*.bin\0All Files (*.*)\0*.*\0",
        "Save Weapon BIN", "bin")) {
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(data, 1, data_size, f);
            fclose(f);
            char msg[256];
            _snprintf(msg, 255, "Saved %d bytes to %s", (int)data_size, path);
            g_app.set_status(msg);
        }
    }
}

void WeaponPanel::paint(HDC hdc, RECT& rc) {
    extern AppTheme T;
    HBRUSH bg = CreateSolidBrush(T.isDark ? T.panelBg : GetSysColor(COLOR_3DFACE));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
}

LRESULT CALLBACK WeaponPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    extern AppTheme T;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        g_weapon.paint(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(T.isDark ? T.panelBg : GetSysColor(COLOR_3DFACE));
        FillRect((HDC)wp, &rc, bg);
        DeleteObject(bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (T.isDark) {
            SetBkColor((HDC)wp, T.listBg);
            SetTextColor((HDC)wp, T.listText);
            static HBRUSH hBr = 0;
            if (!hBr) hBr = CreateSolidBrush(T.listBg);
            return (LRESULT)hBr;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == WEP_SAVE_BTN) {
            g_weapon.save_file();
            return 0;
        }
        break;
    case WM_MOUSEWHEEL: {
        RECT rc; GetClientRect(hwnd, &rc);
        int visible_h = rc.bottom - rc.top;
        int max_scroll = g_weapon.content_h - visible_h;
        if (max_scroll < 0) max_scroll = 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int step = (delta > 0) ? -40 : 40;
        int new_scroll = g_weapon.scroll_y + step;
        if (new_scroll < 0) new_scroll = 0;
        if (new_scroll > max_scroll) new_scroll = max_scroll;
        int dy = g_weapon.scroll_y - new_scroll;
        if (dy != 0) {
            g_weapon.scroll_y = new_scroll;
            ScrollWindow(hwnd, 0, dy, NULL, NULL);
            UpdateWindow(hwnd);
        }
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}


/*═══════════════════════════════════════════════════════════════════
 *  SCD Disassembler Panel
 *═══════════════════════════════════════════════════════════════════*/

ScdPanel g_scd;

bool ScdPanel::load(const u8* rdt_data, size_t rdt_size, u32 base_addr) {
    loaded = false;
    sel_line = -1;
    scroll_y = 0;
    text_data.init();
    if (!scd_disassemble(rdt_data, rdt_size, base_addr, disasm))
        return false;

    /* Extract text strings and append to disassembly as text blocks */
    rdt_extract_text(rdt_data, rdt_size, base_addr, text_data);
    if (text_data.n_blocks > 0) {
        ScdLine& sep = disasm.add();
        sep.thread = 0xFF;
        _snprintf(sep.text, sizeof(sep.text),
                  "; ═══ Text Strings (%d blocks, %d chars) ═══",
                  text_data.n_blocks, text_data.total_chars);

        for (int i = 0; i < text_data.n_blocks; i++) {
            const RdtTextBlock& tb = text_data.blocks[i];
            /* Block header */
            ScdLine& hdr = disasm.add();
            hdr.offset = (u32)tb.offset;
            hdr.thread = 0xFF;
            hdr.is_label = true;
            _snprintf(hdr.text, sizeof(hdr.text), "msg_%d:", i);

            /* Text content — split by newlines */
            const char* p = tb.text;
            while (*p) {
                ScdLine& tl = disasm.add();
                tl.offset = (u32)tb.offset;
                tl.thread = 0xFE; /* special: text line */
                const char* nl = p;
                while (*nl && *nl != '\n') nl++;
                int len = (int)(nl - p);
                if (len > 240) len = 240;
                _snprintf(tl.text, sizeof(tl.text), "    \"%.*s\"", len, p);
                p = nl;
                if (*p == '\n') p++;
            }
        }
    }

    loaded = true;
    return true;
}

/* Dino Crisis 2 room: the slot-5 script.  Its text is not decoded. */
bool ScdPanel::load_dc2(const u8* room, size_t room_size, u32 base_addr) {
    sel_line = -1;
    scroll_y = 0;
    text_data.init();
    loaded = dc2_scd_disassemble(room, room_size, base_addr, disasm);
    return loaded;
}

void ScdPanel::paint(HDC hdc, RECT& rc) {
    extern AppTheme T;
    int w = rc.right, h = rc.bottom;

    /* Background */
    HBRUSH bg = CreateSolidBrush(T.isDark ? RGB(14, 16, 24) : RGB(255, 255, 255));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (!loaded || disasm.n_lines == 0) {
        SetTextColor(hdc, T.isDark ? RGB(100, 110, 130) : RGB(128, 128, 128));
        SetBkMode(hdc, TRANSPARENT);
        HFONT old = (HFONT)SelectObject(hdc, font);
        DrawTextA(hdc, "No SCD data loaded. Select an RDT entry to view scripts.", -1,
                  &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        return;
    }

    HFONT old = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);

    /* Measure line height */
    TEXTMETRICA tm;
    GetTextMetricsA(hdc, &tm);
    line_h = tm.tmHeight + 2;
    vis_lines = h / line_h + 1;

    /* Column layout */
    int col_offset = 8;         /* offset column X */
    int col_hex = 70;           /* hex column X */
    int col_asm = show_hex ? 280 : 70;  /* asm column X */
    int col_comment = col_asm + 520;    /* comment column X */

    /* Header bar */
    RECT hdr = { 0, 0, w, line_h + 2 };
    HBRUSH hdr_bg = CreateSolidBrush(T.isDark ? RGB(22, 26, 36) : RGB(230, 230, 235));
    FillRect(hdc, &hdr, hdr_bg);
    DeleteObject(hdr_bg);
    SetTextColor(hdc, T.isDark ? RGB(140, 150, 170) : RGB(80, 80, 80));
    TextOutA(hdc, col_offset, 1, "Offset", 6);
    if (show_hex) TextOutA(hdc, col_hex, 1, "Hex", 3);
    TextOutA(hdc, col_asm, 1, "Disassembly", 11);
    if (show_comments) TextOutA(hdc, col_comment, 1, "Comment", 7);

    /* Separator line */
    HPEN sep = CreatePen(PS_SOLID, 1, T.isDark ? RGB(40, 45, 55) : RGB(200, 200, 200));
    HPEN oldpen = (HPEN)SelectObject(hdc, sep);
    MoveToEx(hdc, 0, line_h + 2, NULL);
    LineTo(hdc, w, line_h + 2);
    SelectObject(hdc, oldpen);
    DeleteObject(sep);

    int y_start = line_h + 4;

    for (int i = 0; i < vis_lines + 1; i++) {
        int li = scroll_y + i;
        if (li < 0 || li >= disasm.n_lines) continue;
        const ScdLine& l = disasm.lines[li];
        int y = y_start + i * line_h;
        if (y > h) break;

        /* Selection highlight */
        if (li == sel_line) {
            RECT sel = { 0, y - 1, w, y + line_h + 1 };
            HBRUSH selbg = CreateSolidBrush(T.isDark ? RGB(30, 50, 70) : RGB(200, 220, 255));
            FillRect(hdc, &sel, selbg);
            DeleteObject(selbg);
        }

        /* Thread separator lines: different background */
        if (l.thread == 0xFF || l.text[0] == ';') {
            SetTextColor(hdc, T.isDark ? RGB(80, 160, 80) : RGB(0, 128, 0));
            TextOutA(hdc, col_asm, y, l.text, (int)strlen(l.text));
            continue;
        }

        /* Text string lines (decoded from RDT) */
        if (l.thread == 0xFE) {
            SetTextColor(hdc, T.isDark ? RGB(220, 180, 120) : RGB(160, 100, 20));
            TextOutA(hdc, col_asm, y, l.text, (int)strlen(l.text));
            continue;
        }

        /* Label lines */
        if (l.is_label) {
            SetTextColor(hdc, T.isDark ? RGB(220, 200, 100) : RGB(150, 120, 0));
            TextOutA(hdc, col_asm - 4, y, l.text, (int)strlen(l.text));
            continue;
        }

        /* Empty lines */
        if (l.text[0] == 0) continue;

        /* Offset column */
        char off_buf[16];
        _snprintf(off_buf, sizeof(off_buf), "%04X", l.offset);
        SetTextColor(hdc, T.isDark ? RGB(90, 100, 120) : RGB(150, 150, 150));
        TextOutA(hdc, col_offset, y, off_buf, (int)strlen(off_buf));

        /* Hex column */
        if (show_hex && l.hex[0]) {
            SetTextColor(hdc, T.isDark ? RGB(80, 90, 110) : RGB(160, 160, 170));
            TextOutA(hdc, col_hex, y, l.hex, (int)strlen(l.hex));
        }

        /* Disassembly column — color by category */
        COLORREF asm_color;
        if (disasm.dc2) {
            asm_color = cat_colors[dc2_scd_op_category(l.opcode)];
        } else if (l.opcode < 0x70) {
            int cat = g_scd_opcodes[l.opcode].category;
            asm_color = cat_colors[cat];
        } else if (l.opcode == 0xFF) {
            asm_color = T.isDark ? RGB(80, 160, 80) : RGB(0, 128, 0);
        } else {
            asm_color = cat_colors[9]; /* unknown → gray */
        }

        /* Branch targets: draw arrow indicator */
        if (l.is_branch) {
            SetTextColor(hdc, T.isDark ? RGB(255, 200, 80) : RGB(200, 150, 0));
            TextOutA(hdc, col_asm - 16, y, ">", 1);
        }

        SetTextColor(hdc, asm_color);
        TextOutA(hdc, col_asm, y, l.text, (int)strlen(l.text));

        /* Comment column */
        if (show_comments && l.comment[0]) {
            SetTextColor(hdc, T.isDark ? RGB(70, 80, 90) : RGB(160, 160, 170));
            TextOutA(hdc, col_comment, y, l.comment, (int)strlen(l.comment));
        }
    }

    SelectObject(hdc, old);
}

void ScdPanel::on_scroll(int delta) {
    scroll_y -= delta / 40;
    if (scroll_y < 0) scroll_y = 0;
    int max_scroll = disasm.n_lines - vis_lines + 2;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_y > max_scroll) scroll_y = max_scroll;
    InvalidateRect(hwnd, 0, FALSE);
}

void ScdPanel::on_click(int y) {
    int y_start = line_h + 4;
    int li = scroll_y + (y - y_start) / line_h;
    if (li >= 0 && li < disasm.n_lines)
        sel_line = li;
    else
        sel_line = -1;
    InvalidateRect(hwnd, 0, FALSE);
}

void ScdPanel::on_key(int vk) {
    if (!loaded) return;
    if (vk == VK_DOWN) {
        if (sel_line < disasm.n_lines - 1) sel_line++;
        /* Auto-scroll */
        if (sel_line >= scroll_y + vis_lines - 2)
            scroll_y = sel_line - vis_lines + 3;
    }
    if (vk == VK_UP) {
        if (sel_line > 0) sel_line--;
        if (sel_line < scroll_y)
            scroll_y = sel_line;
    }
    if (vk == VK_PRIOR) { /* Page Up */
        sel_line -= vis_lines; if (sel_line < 0) sel_line = 0;
        scroll_y -= vis_lines; if (scroll_y < 0) scroll_y = 0;
    }
    if (vk == VK_NEXT) { /* Page Down */
        sel_line += vis_lines;
        if (sel_line >= disasm.n_lines) sel_line = disasm.n_lines - 1;
        scroll_y += vis_lines;
        int mx = disasm.n_lines - vis_lines + 2;
        if (mx < 0) mx = 0;
        if (scroll_y > mx) scroll_y = mx;
    }
    if (vk == VK_HOME) { sel_line = 0; scroll_y = 0; }
    if (vk == VK_END) {
        sel_line = disasm.n_lines - 1;
        scroll_y = disasm.n_lines - vis_lines + 2;
        if (scroll_y < 0) scroll_y = 0;
    }
    /* G: follow branch target */
    if (vk == 'G' && sel_line >= 0 && sel_line < disasm.n_lines) {
        const ScdLine& l = disasm.lines[sel_line];
        if (l.is_branch && l.branch_target >= 0) {
            /* Find line with matching offset */
            for (int i = 0; i < disasm.n_lines; i++) {
                if (disasm.lines[i].offset == (u32)l.branch_target && !disasm.lines[i].is_label) {
                    sel_line = i;
                    scroll_y = i - vis_lines / 3;
                    if (scroll_y < 0) scroll_y = 0;
                    break;
                }
            }
        }
    }
    /* H: toggle hex column */
    if (vk == 'H') { show_hex = !show_hex; }
    InvalidateRect(hwnd, 0, FALSE);
}

LRESULT CALLBACK ScdPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        /* Double-buffer */
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        g_scd.paint(mem, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL:
        g_scd.on_scroll(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        g_scd.on_click(HIWORD(lp));
        return 0;
    case WM_KEYDOWN:
        g_scd.on_key((int)wp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}


void register_panel_classes(HINSTANCE hInst) {
    WNDCLASSEXA wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.hInstance = hInst;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = NULL;

    wc.lpfnWndProc = HexPanelProc;     wc.lpszClassName = "DCHexPanel";     RegisterClassExA(&wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ImagePanelProc;   wc.lpszClassName = "DCImagePanel";   RegisterClassExA(&wc);
    wc.style = 0;
    wc.lpfnWndProc = PalettePanelProc; wc.lpszClassName = "DCPalettePanel"; RegisterClassExA(&wc);
    wc.lpfnWndProc = AudioPanelProc;   wc.lpszClassName = "DCAudioPanel";   RegisterClassExA(&wc);
    wc.lpfnWndProc = VideoPanelProc;   wc.lpszClassName = "DCVideoPanel";   RegisterClassExA(&wc);
    wc.lpfnWndProc = SavePanelProc;    wc.lpszClassName = "DCSavePanel";    RegisterClassExA(&wc);
    wc.style = 0;
    wc.lpfnWndProc = WeaponPanelProc;  wc.lpszClassName = "DCWeaponPanel";  RegisterClassExA(&wc);
    wc.lpfnWndProc = ScdPanelProc;     wc.lpszClassName = "DCScdPanel";     RegisterClassExA(&wc);
    wc.lpfnWndProc = Viewer3DProc;     wc.lpszClassName = "DC3DPanel";
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    RegisterClassExA(&wc);
}
