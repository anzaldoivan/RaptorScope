/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  Win32 Application Framework
 *  Dark theme modeled on WBC Viewer (proven working approach)
 *═══════════════════════════════════════════════════════════════════*/
#include "ui/app.h"
#include "ui/panels.h"
#include "formats/dat.h"
#include "formats/mesh.h"
#include "formats/dc2.h"
#include "formats/scd.h"
#include "formats/texture.h"
#include "formats/tim.h"
#include "core/lzss.h"
#include "core/audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <commctrl.h>
#include <shellapi.h>
#include <GL/gl.h>

/* GET_X_LPARAM / GET_Y_LPARAM — avoid dependency on windowsx.h */
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))
#endif

#include "ui/about_dialog.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "glu32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "shell32.lib")
#endif

App g_app;

/* Window titles start with this; the status bar shows the title without it */
#define TITLE_PREFIX     "RaptorScope - "
#define TITLE_PREFIX_LEN (sizeof(TITLE_PREFIX) - 1)

/* Panel instances (shared with panels.cpp) */
HexPanel      g_hex;
ImagePanel    g_image;
PalettePanel  g_palette;
AudioPanel    g_audio;
ViewerPanel3D g_viewer3d;
VideoPanel    g_video;
SavePanel     g_save;
WeaponPanel   g_weapon;
static HWND   g_panels[PANEL_COUNT];

/* Persistent storage for EMD model (animation needs it to live across frames) */
static EmdModel* g_emd_model = 0;

/* Dino Crisis 2: the .TEX package beside the open one (E00.TEX for
   E00.DAT), which holds an enemy model's texture.  Loaded on first use.
   g_dc2_skin_alt: it is a player's alternate-skin file instead, standing
   in for a skin the package lacks (dc2_alt_skin_name). */
static DatArchive* g_dc2_skin = 0;
static bool        g_dc2_skin_alt = false;

/* EMD header offsets discovered during tree building (for multi-EMD entries) */
#define MAX_EMDS_PER_ENTRY 16
struct EntryEmdInfo {
    size_t offsets[MAX_EMDS_PER_ENTRY];
    int    count;
    int    entry_idx;
};
static EntryEmdInfo g_emd_info[64]; /* up to 64 entries tracked */
static int g_emd_info_count = 0;

static EntryEmdInfo* get_emd_info(int entry_idx) {
    for (int i = 0; i < g_emd_info_count; i++)
        if (g_emd_info[i].entry_idx == entry_idx)
            return &g_emd_info[i];
    return 0;
}

/*═══════════════════════════════════════════════════════════════════
 *  Theme  -  static color tables (like WBC viewer)
 *═══════════════════════════════════════════════════════════════════*/
static const AppTheme DARK_THEME = {
    RGB(30,30,30),    /* bg */
    RGB(38,38,42),    /* panelBg */
    RGB(210,210,210), /* text */
    RGB(140,140,140), /* textDim */
    RGB(55,55,60),    /* border */
    RGB(0,120,215),   /* accent */
    RGB(45,45,50),    /* menuBg */
    RGB(65,65,72),    /* menuHover */
    RGB(50,80,130),   /* menuPress */
    RGB(30,30,30),    /* listBg */
    RGB(55,90,140),   /* listSel */
    RGB(210,210,210), /* listText */
    RGB(35,35,40),    /* statusBg */
    RGB(180,180,180), /* statusText */
    RGB(15,15,30),    /* hexBg */
    RGB(100,180,255), /* hexAddr */
    RGB(180,220,180), /* hexByte */
    RGB(220,180,100), /* hexAscii */
    TRUE
};

static const AppTheme LIGHT_THEME = {
    RGB(240,240,240),   /* bg */
    RGB(248,248,248),   /* panelBg */
    RGB(30,30,30),      /* text */
    RGB(100,100,100),   /* textDim */
    RGB(210,210,215),   /* border */
    RGB(0,120,215),     /* accent */
    RGB(245,245,248),   /* menuBg */
    RGB(218,222,230),   /* menuHover */
    RGB(190,210,240),   /* menuPress */
    RGB(255,255,255),   /* listBg */
    RGB(200,220,245),   /* listSel */
    RGB(30,30,30),      /* listText */
    RGB(238,238,242),   /* statusBg */
    RGB(60,60,60),      /* statusText */
    RGB(255,255,255),   /* hexBg */
    RGB(0,0,180),       /* hexAddr */
    RGB(0,80,0),        /* hexByte */
    RGB(128,0,0),       /* hexAscii */
    FALSE
};

AppTheme T;
HBRUSH g_hBrBg = NULL;
HBRUSH g_hBrPanel = NULL;

/*═══════════════════════════════════════════════════════════════════
 *  UxTheme / DWM dynamic loading (same ordinals as WBC viewer)
 *═══════════════════════════════════════════════════════════════════*/
typedef HRESULT (WINAPI *pfnDwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);
typedef BOOL    (WINAPI *pfnAllowDarkWnd)(HWND, BOOL);
typedef BOOL    (WINAPI *pfnSetAppMode)(int);
typedef void    (WINAPI *pfnFlushMenu)(void);
typedef HRESULT (WINAPI *pfnSetWndTheme)(HWND, LPCWSTR, LPCWSTR);

static pfnDwmSetAttr    pDwmSetAttr = NULL;
static pfnAllowDarkWnd  pAllowDark = NULL;
static pfnSetAppMode    pSetAppMode = NULL;
static pfnFlushMenu     pFlushMenu = NULL;
static pfnSetWndTheme   pSetWndTheme = NULL;

void LoadThemeApis(void)
{
    HMODULE hDwm = LoadLibraryA("dwmapi.dll");
    if (hDwm)
        pDwmSetAttr = (pfnDwmSetAttr)(void*)GetProcAddress(hDwm, "DwmSetWindowAttribute");

    HMODULE hUx = LoadLibraryA("uxtheme.dll");
    if (!hUx) return;
    pSetWndTheme = (pfnSetWndTheme)(void*)GetProcAddress(hUx, "SetWindowTheme");
    /* Ordinal 132: AllowDarkModeForWindow (WBC uses 132) */
    pAllowDark = (pfnAllowDarkWnd)(void*)GetProcAddress(hUx, MAKEINTRESOURCEA(132));
    /* Ordinal 135: SetPreferredAppMode (1903+) */
    pSetAppMode = (pfnSetAppMode)(void*)GetProcAddress(hUx, MAKEINTRESOURCEA(135));
    /* Ordinal 136: FlushMenuThemes */
    pFlushMenu = (pfnFlushMenu)(void*)GetProcAddress(hUx, MAKEINTRESOURCEA(136));
}

BOOL DetectSystemDarkMode(void)
{
    HKEY hKey = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, sz = sizeof(val);
        RegQueryValueExA(hKey, "AppsUseLightTheme", NULL, NULL, (LPBYTE)&val, &sz);
        RegCloseKey(hKey);
        return (val == 0);
    }
    return FALSE;
}

static void RecreateBrushes(void)
{
    if (g_hBrBg) DeleteObject(g_hBrBg);
    if (g_hBrPanel) DeleteObject(g_hBrPanel);
    g_hBrBg = CreateSolidBrush(T.bg);
    g_hBrPanel = CreateSolidBrush(T.panelBg);
}

void ApplyTheme(BOOL dark)
{
    T = dark ? DARK_THEME : LIGHT_THEME;
    RecreateBrushes();

    /* 1. Force global app mode preference (CRITICAL for popup menus) */
    if (pSetAppMode) pSetAppMode(dark ? 2 : 0);  /* 2 = ForceDark */

    /* 2. Flush menu themes immediately */
    if (pFlushMenu) pFlushMenu();

    if (g_app.hMain) {
        /* 3. Apply dark title bar */
        if (pDwmSetAttr) {
            BOOL v = dark;
            if (FAILED(pDwmSetAttr(g_app.hMain, 20, &v, sizeof(v))))
                pDwmSetAttr(g_app.hMain, 19, &v, sizeof(v));
        }

        /* 4. Apply DarkMode_Explorer to TreeView scrollbars */
        if (pSetWndTheme && g_app.hTree)
            pSetWndTheme(g_app.hTree, dark ? L"DarkMode_Explorer" : NULL, NULL);

        /* 5. Update TreeView colors */
        if (g_app.hTree) {
            TreeView_SetBkColor(g_app.hTree, T.listBg);
            TreeView_SetTextColor(g_app.hTree, T.listText);
        }

        /* 6. Recreate menus */
        CreateAppMenus();

        /* 7. Force redraws */
        SetWindowPos(g_app.hMain, NULL, 0,0,0,0,
            SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
        InvalidateRect(g_app.hMain, NULL, TRUE);
        if (g_app.hTree) { InvalidateRect(g_app.hTree, NULL, TRUE); UpdateWindow(g_app.hTree); }
        if (g_app.hMenuBar) { InvalidateRect(g_app.hMenuBar, NULL, TRUE); UpdateWindow(g_app.hMenuBar); }
        if (g_app.hStatus) { InvalidateRect(g_app.hStatus, NULL, TRUE); UpdateWindow(g_app.hStatus); }
    }
}

void ApplyDarkToControl(HWND hwnd)
{
    if (!hwnd) return;
    if (T.isDark && pSetWndTheme)
        pSetWndTheme(hwnd, L"DarkMode_Explorer", NULL);
    if (T.isDark && pAllowDark)
        pAllowDark(hwnd, TRUE);
}

/*═══════════════════════════════════════════════════════════════════
 *  Custom Menu Bar (like WBC viewer  -  simple child window)
 *═══════════════════════════════════════════════════════════════════*/
MenuBarItem g_mbItems[MAX_MENUBAR_ITEMS];
int g_mbCount = 0;
int g_mbHot = -1;

void CreateAppMenus(void)
{
    g_mbCount = 0;

    static HMENU hFile = NULL;
    static HMENU hEntry = NULL;
    static HMENU hExport = NULL;
    static HMENU hView = NULL;
    static HMENU hHelp = NULL;

    if (hFile) DestroyMenu(hFile);
    if (hEntry) DestroyMenu(hEntry);
    if (hExport) DestroyMenu(hExport);
    if (hView) DestroyMenu(hView);
    if (hHelp) DestroyMenu(hHelp);

    hFile = CreatePopupMenu();
    AppendMenuA(hFile, MF_STRING, IDM_FILE_OPEN,    "&Open Archive...\tCtrl+O");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SAVE_AS, "Save &As...");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_CLOSE,   "&Close Archive");
    AppendMenuA(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFile, MF_STRING, IDM_FILE_EXIT,    "E&xit");

    hEntry = CreatePopupMenu();
    AppendMenuA(hEntry, MF_STRING, IDM_ENTRY_EXPORT,     "Save &Selected...");
    AppendMenuA(hEntry, MF_STRING | MF_GRAYED, IDM_ENTRY_EXPORT_ALL, "Save &All...  (TODO)");
    AppendMenuA(hEntry, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hEntry, MF_STRING, IDM_ENTRY_IMPORT, "&Import / Replace...\tCtrl+I");
    AppendMenuA(hEntry, MF_STRING | MF_GRAYED, IDM_ENTRY_ADD,    "A&dd Entry...  (TODO)");
    AppendMenuA(hEntry, MF_STRING | MF_GRAYED, IDM_ENTRY_DELETE, "De&lete Entry  (TODO)");

    hExport = CreatePopupMenu();
    AppendMenuA(hExport, MF_STRING, IDM_EXPORT_OBJ, "Export as &OBJ (3D Mesh)...");
    AppendMenuA(hExport, MF_STRING, IDM_EXPORT_GLB, "Export &GLB (Character + Animations, or Room)...");
    AppendMenuA(hExport, MF_STRING, IDM_EXPORT_SMD, "Export as &SMD (Skeletal Mesh)...");
    AppendMenuA(hExport, MF_STRING, IDM_EXPORT_WAV, "Export as &WAV (Audio)...");
    AppendMenuA(hExport, MF_STRING, IDM_EXPORT_PNG, "Export as &PNG (Image)...");
    AppendMenuA(hExport, MF_STRING | MF_GRAYED, IDM_EXPORT_TGA, "Export as &TGA + Cue...  (TODO)");
    AppendMenuA(hExport, MF_SEPARATOR, 0, 0);
    AppendMenuA(hExport, MF_STRING, IDM_EXPORT_DEC, "Export &Decompressed (LZSS)...");
    AppendMenuA(hExport, MF_STRING, IDM_EXPORT_ATLAS, "Export Texture &Atlas (BMP)...");
    AppendMenuA(hExport, MF_STRING, IDM_EXPORT_COLL, "Export &Collision Mesh (OBJ)...");

    hView = CreatePopupMenu();
    AppendMenuA(hView, MF_STRING | (g_image.fit_to_screen ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_FIT_SCREEN, "&Fit to Screen on Load");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING, IDM_VIEW_SCD, "SCD &Disassembly\tF4");

    hHelp = CreatePopupMenu();
    AppendMenuA(hHelp, MF_STRING, IDM_HELP_ABOUT, "&About...");

    /* Build menu bar items array */
    MenuBarItem miFile  = { "&File",   hFile,   {0,0,0,0} };
    g_mbItems[g_mbCount++] = miFile;
    MenuBarItem miEntry = { "&Entry",  hEntry,  {0,0,0,0} };
    g_mbItems[g_mbCount++] = miEntry;
    MenuBarItem miExp   = { "E&xport", hExport, {0,0,0,0} };
    g_mbItems[g_mbCount++] = miExp;
    MenuBarItem miView  = { "&View",   hView,   {0,0,0,0} };
    g_mbItems[g_mbCount++] = miView;
    MenuBarItem miHelp  = { "&Help",   hHelp,   {0,0,0,0} };
    g_mbItems[g_mbCount++] = miHelp;
}

LRESULT CALLBACK MenuBarProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        HBRUSH hb = CreateSolidBrush(T.menuBg);
        FillRect(hdc, &ps.rcPaint, hb);
        DeleteObject(hb);
        SetBkMode(hdc, TRANSPARENT);
        HFONT hOld = (HFONT)SelectObject(hdc, g_app.hFontUI);

        /* Calculate layout on the fly (like WBC) */
        int x = 8;
        for (int i = 0; i < g_mbCount; i++) {
            SIZE sz;
            GetTextExtentPoint32A(hdc, g_mbItems[i].label,
                (int)strlen(g_mbItems[i].label), &sz);
            g_mbItems[i].rc.left = x;
            g_mbItems[i].rc.top = 0;
            g_mbItems[i].rc.right = x + sz.cx + 14;
            g_mbItems[i].rc.bottom = MENUBAR_H;

            if (i == g_mbHot) {
                HBRUSH hh = CreateSolidBrush(T.menuHover);
                FillRect(hdc, &g_mbItems[i].rc, hh);
                DeleteObject(hh);
            }
            SetTextColor(hdc, T.text);
            RECT tr = g_mbItems[i].rc;
            tr.left += 7;
            DrawTextA(hdc, g_mbItems[i].label, -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x = g_mbItems[i].rc.right;
        }
        SelectObject(hdc, hOld);

        /* Bottom border */
        HPEN hp = CreatePen(PS_SOLID, 1, T.border);
        HPEN hpo = (HPEN)SelectObject(hdc, hp);
        MoveToEx(hdc, 0, MENUBAR_H - 1, NULL);
        LineTo(hdc, 2000, MENUBAR_H - 1);
        SelectObject(hdc, hpo);
        DeleteObject(hp);

        EndPaint(hWnd, &ps);
        return 0;
    }
    else if (msg == WM_MOUSEMOVE) {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        int oldHot = g_mbHot;
        g_mbHot = -1;
        POINT pt;
        pt.x = mx;
        pt.y = my;
        for (int i = 0; i < g_mbCount; i++) {
            if (PtInRect(&g_mbItems[i].rc, pt)) g_mbHot = i;
        }
        if (g_mbHot != oldHot) InvalidateRect(hWnd, NULL, FALSE);
        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hWnd;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
    }
    else if (msg == WM_MOUSELEAVE) {
        g_mbHot = -1;
        InvalidateRect(hWnd, NULL, FALSE);
    }
    else if (msg == WM_LBUTTONDOWN) {
        if (g_mbHot >= 0) {
            RECT r = g_mbItems[g_mbHot].rc;
            POINT pt;
            pt.x = r.left;
            pt.y = r.bottom;
            ClientToScreen(hWnd, &pt);
            /* TrackPopupMenu sends WM_COMMAND to g_app.hMain (like WBC) */
            TrackPopupMenu(g_mbItems[g_mbHot].hSub,
                TPM_LEFTALIGN | TPM_TOPALIGN,
                pt.x, pt.y, 0, g_app.hMain, NULL);
        }
    }
    return DefWindowProcA(hWnd, msg, wp, lp);
}

/*═══════════════════════════════════════════════════════════════════
 *  Owner-draw popup menus (dark themed drop-downs)
 *═══════════════════════════════════════════════════════════════════*/


/*═══════════════════════════════════════════════════════════════════
 *  File Dialogs
 *═══════════════════════════════════════════════════════════════════*/
bool ui_open_file(HWND parent, char* path, int max_len,
                  const char* filter, const char* title) {
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFile = path;
    ofn.nMaxFile = max_len;
    ofn.lpstrFilter = filter;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    path[0] = '\0';
    return GetOpenFileNameA(&ofn) != 0;
}

bool ui_save_file(HWND parent, char* path, int max_len,
                  const char* filter, const char* title, const char* ext) {
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFile = path;
    ofn.nMaxFile = max_len;
    ofn.lpstrFilter = filter;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = ext;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    path[0] = '\0';
    return GetSaveFileNameA(&ofn) != 0;
}

/*═══════════════════════════════════════════════════════════════════
 *  App constructor
 *═══════════════════════════════════════════════════════════════════*/
App::App()
    : hInst(0), hMain(0), hMenuBar(0), hStatus(0), hTree(0),
      hPanelArea(0), hFontUI(0), hFontMono(0), archive(0),
      active_panel(-1), nav_hidden(false),
      raw_hex_buf(0), raw_hex_size(0)
{}

/*═══════════════════════════════════════════════════════════════════
 *  Archive Operations
 *═══════════════════════════════════════════════════════════════════*/
static HTREEITEM tree_add(HWND hTree, HTREEITEM parent, const char* text, LPARAM lp);

bool App::open_archive(const char* path) {
    close_archive();

    /* Check for RIFF/WAV file first (DC1 music .dat files are WAV) */
    {
        FILE* wf = fopen(path, "rb");
        if (wf) {
            u8 hdr[12];
            if (fread(hdr, 1, 12, wf) == 12 &&
                hdr[0]=='R' && hdr[1]=='I' && hdr[2]=='F' && hdr[3]=='F' &&
                hdr[8]=='W' && hdr[9]=='A' && hdr[10]=='V' && hdr[11]=='E') {
                fseek(wf, 0, SEEK_END); long sz = ftell(wf); fseek(wf, 0, SEEK_SET);
                u8* wav_buf = (u8*)malloc(sz);
                if (wav_buf && fread(wav_buf, 1, sz, wf) == (size_t)sz) {
                    fclose(wf);
                    if (g_audio.load_wav(wav_buf, sz, path)) {
                        free(wav_buf);
                        if (hTree) TreeView_DeleteAllItems(hTree);
                        const char* fn = strrchr(path, '\\');
                        if (!fn) fn = strrchr(path, '/');
                        fn = fn ? fn + 1 : path;
                        char label[256];
                        _snprintf(label, 255, "%s  (WAV Audio, %d Hz, %d-ch)",
                                  fn, g_audio.sample_rate, g_audio.channels);
                        tree_add(hTree, TVI_ROOT, label, -1);
                        active_panel = -1;
                        switch_panel(PANEL_AUDIO);
                        SetFocus(g_audio.hwnd);
                        float dur = (float)g_audio.wav_total_samples / g_audio.sample_rate;
                        char msg[256];
                        _snprintf(msg, 255, "WAV: %s — %d Hz, %d-ch, %.1fs — Space to play",
                                  fn, g_audio.sample_rate, g_audio.channels, dur);
                        set_status(msg);
                        return true;
                    }
                    free(wav_buf);
                    return false;
                }
                free(wav_buf);
            }
            fclose(wf);
        }
    }

    archive = new DatArchive();
    if (!dat_parse_file(path, *archive)) {
        delete archive; archive = 0;
        /* Fallback: load raw file and try specialized BIN viewers */
        FILE* f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 64*1024*1024) {
                free(raw_hex_buf);
                raw_hex_buf = (u8*)malloc(sz);
                raw_hex_size = (size_t)sz;
                if (raw_hex_buf) {
                    fread(raw_hex_buf, 1, sz, f); fclose(f);
                    g_hex.set_data(raw_hex_buf, (size_t)sz);
                    active_panel = -1;
                    switch_panel(PANEL_HEX);
                    InvalidateRect(g_hex.hwnd, 0, TRUE);

                    const char* fname = strrchr(path, '\\');
                    if (!fname) fname = strrchr(path, '/');
                    fname = fname ? fname + 1 : path;

                    if (hTree) TreeView_DeleteAllItems(hTree);

                    bool handled = false;

                    /* ── Dev Manifest: u32 count + "END\0" + string table ── */
                    if (sz >= 48 && raw_hex_buf[4] == 'E' && raw_hex_buf[5] == 'N'
                        && raw_hex_buf[6] == 'D' && raw_hex_buf[7] == 0
                        && raw_hex_buf[8] == 'D' && raw_hex_buf[9] == ':') {
                        u32 hdr_count = rd_u32(raw_hex_buf);
                        char root_label[256];
                        _snprintf(root_label, 255, "%s  (Dev Manifest, header=%d)", fname, (int)hdr_count);
                        HTREEITEM hRoot = tree_add(hTree, TVI_ROOT, root_label, -1);

                        /* Parse all null-terminated path strings */
                        struct ManifestEntry { int off; char path[128]; char charcode[8]; char type[8]; };
                        ManifestEntry* entries = (ManifestEntry*)calloc(2048, sizeof(ManifestEntry));
                        int n_entries = 0;

                        int pos = 8;
                        while (pos < (int)sz - 4 && n_entries < 2048) {
                            if (raw_hex_buf[pos] >= 0x20 && raw_hex_buf[pos] < 0x7F) {
                                int start = pos;
                                while (pos < (int)sz && raw_hex_buf[pos] != 0) pos++;
                                int slen = pos - start;
                                if (slen >= 4 && slen < 120) {
                                    ManifestEntry& e = entries[n_entries];
                                    e.off = start;
                                    memcpy(e.path, raw_hex_buf + start, slen);
                                    e.path[slen] = 0;
                                    /* Extract character code: D:/DINO/CHAR/XXX/... */
                                    e.charcode[0] = 0;
                                    const char* cp = strstr(e.path, "CHAR/");
                                    if (cp) {
                                        cp += 5;
                                        int ci = 0;
                                        while (*cp && *cp != '/' && ci < 7) e.charcode[ci++] = *cp++;
                                        e.charcode[ci] = 0;
                                    }
                                    /* Type from extension */
                                    const char* dot = strrchr(e.path, '.');
                                    if (dot) { strncpy(e.type, dot+1, 7); e.type[7] = 0; }
                                    else e.type[0] = 0;
                                    n_entries++;
                                }
                                pos = (pos + 4) & ~3;
                            } else {
                                pos++;
                            }
                        }

                        /* Group by character code */
                        /* Collect unique charcodes in order */
                        char seen_codes[256][8];
                        int n_codes = 0;
                        for (int i = 0; i < n_entries; i++) {
                            bool found = false;
                            for (int j = 0; j < n_codes; j++)
                                if (strcmp(seen_codes[j], entries[i].charcode) == 0) { found = true; break; }
                            if (!found && n_codes < 256) {
                                strncpy(seen_codes[n_codes], entries[i].charcode, 7);
                                seen_codes[n_codes][7] = 0;
                                n_codes++;
                            }
                        }

                        /* Map character codes to friendly names */
                        struct CharName { const char* code; const char* name; };
                        static const CharName char_names[] = {
                            {"P00","Regina (main)"}, {"P01","Regina (alt)"},
                            {"P10","Regina Costume A"}, {"P20","Regina Costume B"}, {"P30","Regina Costume C"},
                            {"P40","T-Rex Boss"},
                            {"E00","Raptor"}, {"E01","Raptor (alt)"}, {"E10","T-Rex"},
                            {"E11","T-Rex (alt)"}, {"E20","Pteranodon"}, {"E40","Therizinosaurus"},
                            {"E50","Compsognathus"},
                            {"H10","Hand Item 0"}, {"H11","Hand Item 1"}, {"H12","Hand Item 2"},
                            {"H13","Hand Item 3"}, {"H20","Hand Item 4"}, {"H21","Hand Item 5"},
                            {"H30","Hand Item 6"}, {"H31","Hand Item 7"},
                            {0,0}
                        };

                        for (int ci = 0; ci < n_codes; ci++) {
                            /* Count entries for this code */
                            int cnt = 0;
                            for (int i = 0; i < n_entries; i++)
                                if (strcmp(entries[i].charcode, seen_codes[ci]) == 0) cnt++;

                            /* Find friendly name */
                            const char* friendly = NULL;
                            for (int k = 0; char_names[k].code; k++)
                                if (strcmp(char_names[k].code, seen_codes[ci]) == 0)
                                    { friendly = char_names[k].name; break; }

                            char grp_label[128];
                            if (friendly)
                                _snprintf(grp_label, 127, "%s  %s  (%d files)", seen_codes[ci], friendly, cnt);
                            else
                                _snprintf(grp_label, 127, "%s  (%d files)", seen_codes[ci], cnt);
                            HTREEITEM hGrp = tree_add(hTree, hRoot, grp_label, -1);

                            for (int i = 0; i < n_entries; i++) {
                                if (strcmp(entries[i].charcode, seen_codes[ci]) != 0) continue;
                                /* Show just the filename part */
                                const char* slash = strrchr(entries[i].path, '/');
                                const char* display = slash ? slash + 1 : entries[i].path;
                                char item_label[160];
                                _snprintf(item_label, 159, "%s  [0x%04X]", display, entries[i].off);
                                tree_add(hTree, hGrp, item_label, entries[i].off);
                            }
                        }

                        TreeView_Expand(hTree, hRoot, TVE_EXPAND);
                        free(entries);
                        handled = true;

                        char buf2[512];
                        _snprintf(buf2, 511, TITLE_PREFIX "%s (Dev Manifest, %d assets)", path, n_entries);
                        SetWindowTextA(hMain, buf2);
                        set_status(buf2 + TITLE_PREFIX_LEN);
                    }

                    /* ── MIPS Weapon Overlay: detect by ADDIU SP prologue ── */
                    if (!handled && sz >= 64) {
                        u32 w0 = rd_u32(raw_hex_buf);
                        /* Check for ADDIU SP, SP, -N (0x27BDxxxx) at offset 4 or 0xC.
                           wep10-wep23 have it at offset 4 (8-byte header).
                           wep30-wep31 have it at offset 0xC (12-byte header). */
                        bool is_mips = false;
                        if (w0 < 256) {
                            u32 w4 = rd_u32(raw_hex_buf + 4);
                            u32 wC = rd_u32(raw_hex_buf + 0xC);
                            is_mips = ((w4 & 0xFFFF0000) == 0x27BD0000) ||
                                      ((wC & 0xFFFF0000) == 0x27BD0000);
                        }

                        if (is_mips) {
                            /* Find code/data boundary: last JR RA */
                            int code_end = 0;
                            for (int o = 0; o <= (int)sz - 4; o += 4) {
                                if (rd_u32(raw_hex_buf + o) == 0x03E00008)
                                    code_end = o + 8;
                            }
                            if (code_end == 0) code_end = (int)sz;

                            /* Find pointer table: scan for 0x80XXXXXX sequences */
                            int ptrtbl_start = code_end;
                            for (int o = code_end; o <= (int)sz - 4; o += 4) {
                                u32 v = rd_u32(raw_hex_buf + o);
                                if ((v & 0xFF000000) == 0x80000000) {
                                    ptrtbl_start = o;
                                    break;
                                }
                            }

                            /* Quick weapon identity from filename */
                            const char* tw = "MIPS Overlay";
                            {
                                char tlc[8] = {};
                                for (int ti = 0; ti < 5 && fname[ti]; ti++)
                                    tlc[ti] = (fname[ti] >= 'A' && fname[ti] <= 'Z') ? (fname[ti]+32) : fname[ti];
                                if (strcmp(tlc,"wep10")==0) tw = "Handgun 9mm (Original)";
                                else if (strcmp(tlc,"wep12")==0) tw = "Handgun 9mm (Arrange)";
                                else if (strcmp(tlc,"wep11")==0) tw = "Glock 40 S&W (Original)";
                                else if (strcmp(tlc,"wep13")==0) tw = "Glock 40 S&W (Arrange)";
                                else if (strcmp(tlc,"wep20")==0) tw = "An. Dart Gun (Original)";
                                else if (strcmp(tlc,"wep21")==0) tw = "An. Dart Gun (Arrange)";
                                else if (strcmp(tlc,"wep22")==0) tw = "Shotgun/Grenade/Heat (Original)";
                                else if (strcmp(tlc,"wep23")==0) tw = "Shotgun/Grenade/Heat (Arrange)";
                                else if (strcmp(tlc,"wep30")==0) tw = "Heavy Weapons (Set A)";
                                else if (strcmp(tlc,"wep31")==0) tw = "Heavy Weapons (Set B)";
                            }
                            char root_label[256];
                            _snprintf(root_label, 255, "%s  -  %s  (%d bytes)", fname, tw, (int)sz);
                            HTREEITEM hRoot = tree_add(hTree, TVI_ROOT, root_label, -1);

                            /* Code section */
                            char label[128];
                            _snprintf(label, 127, "Code  0x0000-0x%04X  (%d bytes, ~%d functions)",
                                code_end, code_end, code_end / 80);
                            tree_add(hTree, hRoot, label, 0);

                            /* Data section (params between code and pointer table) */
                            if (ptrtbl_start > code_end) {
                                int data_sz = ptrtbl_start - code_end;
                                _snprintf(label, 127, "Data  0x%04X-0x%04X  (%d bytes)",
                                    code_end, ptrtbl_start, data_sz);
                                HTREEITEM hData = tree_add(hTree, hRoot, label, code_end);

                                /* Parse data as 32-byte parameter records */
                                int rec_size = 32;
                                int n_recs = data_sz / rec_size;
                                for (int i = 0; i < n_recs; i++) {
                                    int roff = code_end + i * rec_size;
                                    s16 v0 = (s16)rd_u16(raw_hex_buf + roff);
                                    s16 v1 = (s16)rd_u16(raw_hex_buf + roff + 4);
                                    s16 dmg = (s16)rd_u16(raw_hex_buf + roff + 16);
                                    s16 rng = (s16)rd_u16(raw_hex_buf + roff + 18);
                                    _snprintf(label, 127, "Param[%d]  mode=%d ammo=%d dmg=%d rng=%d  [0x%04X]",
                                        i, v0, v1, dmg, rng, roff);
                                    tree_add(hTree, hData, label, roff);
                                }
                            }

                            /* Pointer tables */
                            if (ptrtbl_start < (int)sz) {
                                int ptrtbl_sz = (int)sz - ptrtbl_start;
                                _snprintf(label, 127, "Pointers  0x%04X-0x%04X  (%d bytes, %d entries)",
                                    ptrtbl_start, (int)sz, ptrtbl_sz, ptrtbl_sz / 4);
                                HTREEITEM hPtrs = tree_add(hTree, hRoot, label, ptrtbl_start);

                                for (int o = ptrtbl_start; o <= (int)sz - 4; o += 4) {
                                    u32 v = rd_u32(raw_hex_buf + o);
                                    if (v == 0)
                                        _snprintf(label, 127, "[%d] NULL", (o - ptrtbl_start) / 4);
                                    else if ((v & 0xFF000000) == 0x80000000)
                                        _snprintf(label, 127, "[%d] 0x%08X  (PSX)", (o - ptrtbl_start) / 4, v);
                                    else
                                        _snprintf(label, 127, "[%d] 0x%08X", (o - ptrtbl_start) / 4, v);
                                    tree_add(hTree, hPtrs, label, o);
                                }
                            }

                            TreeView_Expand(hTree, hRoot, TVE_EXPAND);
                            handled = true;

                            /* Load weapon editor panel */
                            if (g_weapon.load(raw_hex_buf, (size_t)sz, path)) {
                                active_panel = -1;
                                switch_panel(PANEL_WEP);
                                /* Destroy old controls if re-loading */
                                HWND child = GetWindow(g_weapon.hwnd, GW_CHILD);
                                while (child) {
                                    HWND next = GetWindow(child, GW_HWNDNEXT);
                                    DestroyWindow(child);
                                    child = next;
                                }
                                g_weapon.create_controls();
                                InvalidateRect(g_weapon.hwnd, 0, TRUE);
                            }

                            char buf2[512];
                            /* Quick weapon identity from filename */
                            const char* wid = "Weapon";
                            const char* wfn = strrchr(path, '\\');
                            if (!wfn) wfn = strrchr(path, '/');
                            wfn = wfn ? wfn + 1 : path;
                            char wlc[8] = {};
                            for (int wi = 0; wi < 5 && wfn[wi]; wi++)
                                wlc[wi] = (wfn[wi] >= 'A' && wfn[wi] <= 'Z') ? (wfn[wi]+32) : wfn[wi];
                            if (strcmp(wlc,"wep10")==0||strcmp(wlc,"wep12")==0) wid = "Handgun 9mm";
                            else if (strcmp(wlc,"wep11")==0||strcmp(wlc,"wep13")==0) wid = "Glock 40 S&W";
                            else if (strcmp(wlc,"wep20")==0||strcmp(wlc,"wep21")==0) wid = "An. Dart Gun";
                            else if (strcmp(wlc,"wep22")==0||strcmp(wlc,"wep23")==0) wid = "Shotgun/Grenade/Heat";
                            else if (strcmp(wlc,"wep30")==0||strcmp(wlc,"wep31")==0) wid = "Heavy Weapons";
                            _snprintf(buf2, 511, TITLE_PREFIX "%s - %s Editor", path, wid);
                            SetWindowTextA(hMain, buf2);
                            set_status(buf2 + TITLE_PREFIX_LEN);
                        }
                    }

                    /* ── Text Script: u32 count < 64, followed by printable ASCII + control codes ── */
                    if (!handled && sz >= 32) {
                        u32 tc = rd_u32(raw_hex_buf);
                        /* Heuristic: count < 64, bytes 4-20 contain mostly printable ASCII or control codes 0x01-0x0F */
                        if (tc > 0 && tc < 64) {
                            int ascii_count = 0, ctrl_count = 0;
                            for (int ci = 4; ci < (int)sz && ci < 128; ci++) {
                                u8 b = raw_hex_buf[ci];
                                if (b >= 0x20 && b <= 0x7E) ascii_count++;
                                else if (b >= 0x01 && b <= 0x1F) ctrl_count++;
                            }
                            /* Must have significant ASCII content */
                            if (ascii_count > 30 && ctrl_count > 5) {
                                /* Parse text blocks (null-terminated) */
                                char root_label[256];
                                _snprintf(root_label, 255, "%s  (Text Script, %d sections)", fname, (int)tc);
                                HTREEITEM hRoot = tree_add(hTree, TVI_ROOT, root_label, -1);

                                /* Extract all text blocks and build a combined display string */
                                /* We'll put the text into a large buffer and show in hex panel area
                                   as a multiline read-only EDIT control */
                                int text_cap = (int)sz * 2 + 4096;
                                char* text_buf = (char*)calloc(text_cap, 1);
                                int text_len = 0;
                                int block_num = 0;
                                int pos = 4;

                                while (pos < (int)sz && text_len < text_cap - 256) {
                                    int blk_start = pos;
                                    /* Find end of this null-terminated block */
                                    while (pos < (int)sz && raw_hex_buf[pos] != 0) pos++;
                                    int blk_len = pos - blk_start;
                                    pos++; /* skip null */

                                    if (blk_len == 0) continue;

                                    /* Check text quality — skip binary/code blocks.
                                       Require: meaningful letter content with spaces (actual words),
                                       high printable ratio, no long binary runs. */
                                    {
                                        int letters = 0, spaces = 0, printable = 0;
                                        int max_nontext_run = 0, cur_nontext = 0;
                                        for (int ti = blk_start; ti < blk_start + blk_len; ti++) {
                                            u8 tb = raw_hex_buf[ti];
                                            if ((tb >= 'A' && tb <= 'Z') || (tb >= 'a' && tb <= 'z'))
                                                { letters++; printable++; cur_nontext = 0; }
                                            else if (tb == ' ')
                                                { spaces++; printable++; cur_nontext = 0; }
                                            else if ((tb >= 0x20 && tb <= 0x7E) || tb == 0x09 || tb == 0x0A || tb == 0x07)
                                                { printable++; cur_nontext = 0; }
                                            else
                                                { cur_nontext++; if (cur_nontext > max_nontext_run) max_nontext_run = cur_nontext; }
                                        }
                                        /* Must have real words: 8+ letters, at least 1 space or newline,
                                           60%+ printable, no long binary runs */
                                        if (letters < 8) continue;
                                        if (spaces < 1 && blk_len > 10) continue;
                                        if (printable * 100 / blk_len < 60) continue;
                                        if (max_nontext_run > 4) continue;
                                    }

                                    /* Decode: strip kerning pairs and control codes.
                                       Format: 0x09 + digit = 2-byte kerning pair.
                                       0x0A = newline. 0x07 = page break.
                                       0x80+ = extended glyph (bitmap font index). */
                                    char decoded[512];
                                    int di = 0;
                                    bool has_text = false;
                                    for (int ci = blk_start; ci < blk_start + blk_len && di < 500; ci++) {
                                        u8 b = raw_hex_buf[ci];
                                        if (b == 0x09) {
                                            /* Kerning pair: skip this byte AND the next (digit value) */
                                            ci++; /* consume the digit */
                                        } else if (b >= 0x20 && b <= 0x7E) {
                                            decoded[di++] = (char)b;
                                            has_text = true;
                                        } else if (b == 0x0A) {
                                            decoded[di++] = '\r'; decoded[di++] = '\n';
                                        } else if (b == 0x07) {
                                            decoded[di++] = '\r'; decoded[di++] = '\n';
                                            const char* sep = "--- page break ---\r\n";
                                            int sl = (int)strlen(sep);
                                            if (di + sl < 500) { memcpy(decoded + di, sep, sl); di += sl; }
                                        }
                                        /* 0x01-0x06, 0x08, 0x0B-0x1F: other control, skip */
                                        /* 0x80+: extended glyphs, skip for now */
                                    }
                                    decoded[di] = 0;

                                    if (has_text && di > 1) {
                                        /* Add to tree */
                                        char tree_text[80];
                                        /* Show first ~60 chars as preview */
                                        char preview[64];
                                        int pi = 0;
                                        for (int ci = 0; ci < di && pi < 58; ci++) {
                                            if (decoded[ci] == '\r' || decoded[ci] == '\n') {
                                                if (pi > 0 && preview[pi-1] != ' ') preview[pi++] = ' ';
                                            } else {
                                                preview[pi++] = decoded[ci];
                                            }
                                        }
                                        preview[pi] = 0;
                                        _snprintf(tree_text, 79, "[%d] %s", block_num, preview);
                                        tree_add(hTree, hRoot, tree_text, blk_start);

                                        /* Add to text display buffer */
                                        int hdr_len = _snprintf(text_buf + text_len, text_cap - text_len,
                                            "=== Block %d (offset 0x%04X) ===\r\n", block_num, blk_start);
                                        if (hdr_len > 0) text_len += hdr_len;
                                        int body_len = _snprintf(text_buf + text_len, text_cap - text_len,
                                            "%s\r\n\r\n", decoded);
                                        if (body_len > 0) text_len += body_len;
                                        block_num++;
                                    }
                                }

                                TreeView_Expand(hTree, hRoot, TVE_EXPAND);

                                /* Create standalone text view as direct child of panel area.
                                   Don't overlay on hex — parent it to hPanelArea and hide hex. */
                                RECT pr;
                                GetClientRect(hPanelArea, &pr);
                                /* Destroy previous text view if any */
                                HWND old_tv = GetDlgItem(hPanelArea, 9500);
                                if (old_tv) DestroyWindow(old_tv);
                                HWND hTextView = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", text_buf,
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                    ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                    0, 0, pr.right, pr.bottom,
                                    hPanelArea, (HMENU)9500, hInst, 0);
                                SendMessage(hTextView, WM_SETFONT, (WPARAM)hFontMono, 0);
                                /* Hide all real panels */
                                for (int pi = 0; pi < PANEL_COUNT; pi++)
                                    if (g_panels[pi]) ShowWindow(g_panels[pi], SW_HIDE);
                                active_panel = -1;

                                free(text_buf);
                                handled = true;

                                char buf2[512];
                                _snprintf(buf2, 511, TITLE_PREFIX "%s (Text Script, %d blocks)", path, block_num);
                                SetWindowTextA(hMain, buf2);
                                set_status(buf2 + TITLE_PREFIX_LEN);
                            }
                        }
                    }

                    /* ── Raw 320x240 15-bit image: Dino Crisis 2 *.PXL
                       (M_TITLE.PXL, CAPLOGO.PXL, WARNING*.PXL), the same
                       pixels as a DC1 background entry (dinorand K109) ── */
                    if (!handled && sz == 320 * 240 * 2) {
                        u8* rgba = (u8*)calloc(320 * 240, 4);
                        if (rgba) {
                            render_16bpp(raw_hex_buf, 320, 240, rgba);
                            g_image.set_bitmap(rgba, 320, 240);
                            free(rgba);
                            switch_panel(PANEL_IMAGE);
                            InvalidateRect(g_image.hwnd, 0, FALSE);
                            char node[256];
                            _snprintf(node, 255, "%s  (Image 320x240 16bpp)", fname);
                            tree_add(hTree, TVI_ROOT, node, -1);
                            char buf2[512];
                            _snprintf(buf2, 511, TITLE_PREFIX "%s (raw image 320x240 16bpp)", path);
                            SetWindowTextA(hMain, buf2);
                            set_status(buf2 + TITLE_PREFIX_LEN);
                            handled = true;
                        }
                    }

                    /* ── Generic raw hex fallback ── */
                    if (!handled && hTree) {
                        char node[256];
                        _snprintf(node, 255, "%s  (%d bytes)", fname, (int)sz);
                        tree_add(hTree, TVI_ROOT, node, -1);
                        char buf2[512];
                        _snprintf(buf2, 511, TITLE_PREFIX "%s (raw hex)", path);
                        SetWindowTextA(hMain, buf2);
                        set_status(buf2 + TITLE_PREFIX_LEN);
                    }

                    return true;
                }
            }
            fclose(f);
        }
        set_status("ERROR: Failed to parse archive");
        return false;
    }
    build_tree();
    char buf[512];
    if (archive->is_item_bank) {
        int num_items = archive->count;
        _snprintf(buf, 511, TITLE_PREFIX "%s - Item Sprite Bank (%d sprites)", path, num_items);
    } else {
        _snprintf(buf, 511, TITLE_PREFIX "%s - %d entries%s", path, archive->count,
                  archive->is_dc2 ? " (Dino Crisis 2)" : "");
    }
    SetWindowTextA(hMain, buf);
    set_status(buf + TITLE_PREFIX_LEN);
    return true;
}

void App::close_archive() {
    /* Clear cached pointers before freeing archive data */
    g_image.cur_tex = 0;
    g_image.cur_pal = 0;
    g_image.clear_face_usage();
    delete archive; archive = 0;
    delete g_dc2_skin; g_dc2_skin = 0;
    free(raw_hex_buf); raw_hex_buf = 0; raw_hex_size = 0;
    if (hTree) TreeView_DeleteAllItems(hTree);
    /* Destroy any text overlay controls */
    if (g_hex.hwnd) {
        HWND hText = GetDlgItem(g_hex.hwnd, 9500);
        if (hText) DestroyWindow(hText);
    }
    if (hPanelArea) {
        HWND hText = GetDlgItem(hPanelArea, 9500);
        if (hText) DestroyWindow(hText);
    }
    g_hex.set_data(0, 0);
    switch_panel(PANEL_HEX);
    /* Restore nav panel if hidden */
    if (nav_hidden) {
        nav_hidden = false;
        if (hMain) PostMessage(hMain, WM_SIZE, 0, 0);
    }
    if (hMain) SetWindowTextA(hMain, "Dino Crisis Archive Tool v2.2");
    set_status("Ready");
}

/*═══════════════════════════════════════════════════════════════════
 *  Entry classification  -  probe LZSS0 sub-types
 *═══════════════════════════════════════════════════════════════════*/
enum EntrySubType {
    SUB_NONE = 0,
    SUB_ROOM_MESH,
    SUB_DOOR_MESH,
    SUB_EMD_MODEL,
    SUB_COMPRESSED,
    SUB_BACKGROUND,
    SUB_MIPS_CODE,
    SUB_RDT_SCENE,       /* room mesh + EMDs in one RDT package */
    SUB_WEAPON_DATA,     /* weapon skeleton + GPU display list */
    SUB_STANDALONE_EMD   /* standalone character model (p/h files) */
};

/* Entry type for display/loading purposes.  Uncompressed room RDTs are
   shown and loaded through the LZSS0 path, just without decompressing. */
static u32 effective_entry_type(const DatEntry& e) {
    return is_raw_rdt_entry(e) ? (u32)DAT_LZSS0 : e.type;
}

static EntrySubType classify_lzss0(const DatEntry& e);
static EntrySubType classify_lzss0(const DatEntry& e) {
    if (!(e.y & 0x8000)) return SUB_COMPRESSED;
    if (is_mips_code(e.data, e.size, e.y, e.x)) return SUB_MIPS_CODE;

    /* Use deterministic RDT layout walk */
    RdtLayout layout;
    if (parse_rdt_layout_from_entry(e.data, e.size, e.y, e.x, layout)) {
        if (layout.section_count > 0 && layout.emd_count > 0)
            return SUB_RDT_SCENE;
        if (layout.section_count > 0)
            return SUB_ROOM_MESH;
        if (layout.emd_count > 0)
            return SUB_EMD_MODEL;
        /* Valid RDT header but no mesh/EMD — metadata-only room
           (pre-rendered background, has cameras/collision/scripts) */
        if (layout.valid)
            return SUB_RDT_SCENE;
    }

    if (is_door_mesh_entry(e.data, e.size, e.y, e.w, e.h)) return SUB_DOOR_MESH;
    if (is_weapon_data(e.data, e.size, e.y, e.x)) return SUB_WEAPON_DATA;

    /* Fallback: try room mesh probe */
    Buffer dec;
    if (lzss_decompress(e.data, e.size, dec) && dec.size > 0x20) {
        u32 base = ((u32)(e.y & 0x7FFF) << 16) | (u32)e.x | 0x80000000u;

        /* Standalone character EMD (p/h files): skeleton + mesh at end */
        if (is_standalone_emd_dec(dec.data, dec.size, base))
            return SUB_STANDALONE_EMD;

        u32 p0 = rd_u32(dec.data);
        u32 p0_off = p0 - base;
        if (p0_off > 0 && p0_off < dec.size && p0_off > 0x40) return SUB_ROOM_MESH;
    }
    return SUB_COMPRESSED;
}

static const char* sub_type_label(EntrySubType st) {
    switch (st) {
    case SUB_ROOM_MESH:  return "Room Mesh";
    case SUB_DOOR_MESH:  return "Door Mesh";
    case SUB_EMD_MODEL:  return "Character (EMD)";
    case SUB_RDT_SCENE:  return "RDT Scene";
    case SUB_BACKGROUND: return "Background";
    case SUB_MIPS_CODE:  return "Code Overlay";
    case SUB_WEAPON_DATA: return "Weapon Data";
    case SUB_STANDALONE_EMD: return "Character Mesh (EMD)";
    default: return "Compressed Data";
    }
}

/*═══════════════════════════════════════════════════════════════════
 *  Build Tree  -  categorized hierarchical view
 *═══════════════════════════════════════════════════════════════════*/
static HTREEITEM tree_add(HWND hTree, HTREEITEM parent, const char* text, LPARAM lp) {
    TVINSERTSTRUCTA tvi;
    memset(&tvi, 0, sizeof(tvi));
    tvi.hParent = parent;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM;
    tvi.item.pszText = (LPSTR)text;
    tvi.item.lParam = lp;
    return TreeView_InsertItem(hTree, &tvi);
}

/*─── Dino Crisis 2: tree node of an LZSS0 entry ─────────────────*/
/* A room (the scene, then one child per section of its directory; the
   child's sub-selector is slot + 1), a model, a 320x240 picture, or data
   nobody has decoded. */
static void dc2_tree_add(HWND hTree, HTREEITEM parent, int i, const DatEntry& e)
{
    char label[160];
    Buffer dec;
    if (!dat_entry_payload(e, dec) || dec.size == 0) {
        _snprintf(label, 159, "#%d  Compressed Data  %u bytes", i, e.size);
        tree_add(hTree, parent, label, i);
        return;
    }
    Dc2RoomDir dir;
    Dc2ModelInfo mi;
    if (dc2_model_probe(dec.data, dec.size, e.load_base, mi)) {
        _snprintf(label, 159, "#%d  Model  %d parts, %d verts, %d faces  %u bytes",
                  i, mi.parts, mi.verts, mi.tris + mi.quads * 2, e.size);
        tree_add(hTree, parent, label, i);
    } else if (dc2_room_directory(dec.data, dec.size, e.load_base, dir)) {
        _snprintf(label, 159, "#%d  Room  %u -> %u bytes", i, e.size, (unsigned)dec.size);
        HTREEITEM hRoom = tree_add(hTree, parent, label, -1);
        tree_add(hTree, hRoom, "Scene (collision, zones, doors, items, spawns)", i);
        for (int k = 0; k < DC2_ROOM_SLOTS - 1; k++) {
            if (!dir.present[k]) continue;
            _snprintf(label, 159, "Slot %d  %s  %u bytes", k, dc2_room_slot_name(k), dir.size[k]);
            tree_add(hTree, hRoom, label, (LPARAM)(((k + 1) << 16) | i));
        }
        TreeView_Expand(hTree, hRoom, TVE_EXPAND);
    } else if (dec.size == 320u * 240u * 2u) {
        _snprintf(label, 159, "#%d  Background 320x240 16bpp  %u bytes", i, e.size);
        tree_add(hTree, parent, label, i);
    } else {
        _snprintf(label, 159, "#%d  Compressed Data  %u -> %u bytes  load@0x%08X",
                  i, e.size, (unsigned)dec.size, (unsigned)e.load_base);
        tree_add(hTree, parent, label, i);
    }
}

void App::build_tree() {
    TreeView_DeleteAllItems(hTree);
    g_emd_info_count = 0;
    if (!archive || archive->count == 0) return;

    /* ──── Item Sprite Bank: custom tree layout ──── */
    if (archive->is_item_bank) {
        char label[128];
        int num_items = archive->count;
        _snprintf(label, 127, "Item Sprites (%d)", num_items);
        HTREEITEM hItems = tree_add(hTree, TVI_ROOT, label, -1);

        for (int i = 0; i < num_items; i++) {
            const DatEntry& tex = archive->entries[i];
            int pw = tex.w * 2;
            _snprintf(label, 127, "Item %d  %dx%u", i, pw, tex.h);
            tree_add(hTree, hItems, label, i);
        }

        TreeView_Expand(hTree, hItems, TVE_EXPAND);
        return;
    }

    /* ──── Normal DAT archive tree ──── */

    /* Category parent nodes (lParam = -1 means no entry) */
    HTREEITEM hTextures = NULL, hPalettes = NULL;
    HTREEITEM hSounds = NULL;
    HTREEITEM hModels = NULL, hData = NULL;

    /* Helper: is this entry a real texture (vs EMD skeleton header in type 3+4 pair)?
       PS1 VRAM is 1024x512 halfwords; entries with x>1024 or 1x1 dims are metadata, not textures. */
    auto is_real_texture = [](const DatEntry& e) -> bool {
        if (e.type != DAT_TEXTURE && e.type != DAT_LZSS1 && e.type != DAT_TEXTURE_LINEAR &&
            e.type != DAT_DC2_LZSS2)
            return false;
        if (e.x > 1024 || (e.w < 2 && e.h < 2))
            return false;
        return true;
    };

    /* Count per category first to build labels */
    int nTex=0, nPal=0, nSndH=0, nSndB=0, nSndE=0, nLzss0=0, nData=0;
    for (int i = 0; i < archive->count; i++) {
        const DatEntry& e = archive->entries[i];
        if (is_real_texture(e)) { nTex++; continue; }
        switch (effective_entry_type(e)) {
        case DAT_TEXTURE: case DAT_LZSS1: case DAT_TEXTURE_LINEAR: case DAT_DC2_LZSS2: nData++; break; /* non-texture LZSS1 → data */
        case DAT_PALETTE: nPal++; break;
        case DAT_SNDH: nSndH++; break;
        case DAT_SNDB: nSndB++; break;
        case DAT_SNDE: case DAT_DC2_SOUND: case DAT_DC2_MP3: nSndE++; break;
        case DAT_LZSS0: nLzss0++; break;
        default: nData++; break;
        }
    }

    char label[128];

    /* Textures */
    if (nTex > 0) {
        _snprintf(label, 127, "Textures (%d)", nTex);
        hTextures = tree_add(hTree, TVI_ROOT, label, -1);
    }

    /* Palettes */
    if (nPal > 0) {
        _snprintf(label, 127, "Palettes (%d)", nPal);
        hPalettes = tree_add(hTree, TVI_ROOT, label, -1);
    }

    /* Sound Bank */
    int nSndTotal = nSndH + nSndB + nSndE;
    if (nSndTotal > 0) {
        _snprintf(label, 127, "Sound Bank (%d)", nSndTotal);
        hSounds = tree_add(hTree, TVI_ROOT, label, -1);
    }

    /* 3D / Compressed */
    if (nLzss0 > 0) {
        _snprintf(label, 127, "Models & Scenes (%d)", nLzss0);
        hModels = tree_add(hTree, TVI_ROOT, label, -1);
    }

    /* Data/Scripts */
    if (nData > 0) {
        _snprintf(label, 127, "Data / Scripts (%d)", nData);
        hData = tree_add(hTree, TVI_ROOT, label, -1);
    }

    /* Populate entries into categories */
    for (int i = 0; i < archive->count; i++) {
        const DatEntry& e = archive->entries[i];

        switch (effective_entry_type(e)) {
        case DAT_TEXTURE: case DAT_LZSS1: case DAT_TEXTURE_LINEAR: case DAT_DC2_LZSS2:
            if (is_real_texture(e)) {
                int tx = e.x / 64;
                int ty = e.y / 256;
                int tpage = ty * 16 + tx;
                _snprintf(label, 127, "#%d  %s  %ux%u  VRAM(%d,%d) p%02d  %u bytes",
                          i, dat_type_name(e.type), e.w, e.h, e.x, e.y, tpage, e.size);
                tree_add(hTree, hTextures, label, i);
            } else {
                /* Non-texture type 3/4 (EMD header/body) → put in Data */
                _snprintf(label, 127, "#%d  %s (EMD data?)  %u bytes",
                          i, dat_type_name(e.type), e.size);
                tree_add(hTree, hData ? hData : hModels, label, i);
            }
            break;

        case DAT_DATA: {
            /* Detect uncompressed 16bpp backgrounds (320x240 = 153600 bytes)
               and raw texture data stored as DATA type */
            bool is_image = false;
            if (e.size == 320u * 240u * 2u) {
                _snprintf(label, 127, "#%d  Background 320x240 16bpp (raw)  %u bytes", i, e.size);
                tree_add(hTree, hTextures, label, i);
                is_image = true;
            } else if ((e.y & 0x8000) && e.w > 0 && e.h > 0) {
                /* Has VRAM coords and dimensions - likely a raw texture */
                _snprintf(label, 127, "#%d  Raw Texture  %ux%u  %u bytes", i, e.w, e.h, e.size);
                tree_add(hTree, hTextures, label, i);
                is_image = true;
            }
            if (!is_image) {
                _snprintf(label, 127, "#%d  %s  %u bytes", i, dat_type_name(e.type), e.size);
                tree_add(hTree, hData, label, i);
            }
        } break;

        case DAT_PALETTE:
            _snprintf(label, 127, "#%d  %d rows  VRAM(%d,%d)  %u bytes",
                      i, (int)(e.size / 512), e.x, e.y, e.size);
            tree_add(hTree, hPalettes, label, i);
            break;

        case DAT_SNDH: {
            GianHeader gh;
            if (e.data && e.size >= 12 && parse_gian_header(e.data, e.size, gh)) {
                _snprintf(label, 127, "#%d  Gian Header  %dp/%dt/%dv  %u bytes",
                    i, gh.num_programs, gh.tone_count, gh.num_vag, e.size);
            } else {
                _snprintf(label, 127, "#%d  SNDH  %u bytes", i, e.size);
            }
            tree_add(hTree, hSounds, label, i);
        } break;
        case DAT_SNDB: {
            /* Count VAG samples by scanning end flags */
            int nsamp = 0;
            const u8* sd = e.data;
            int si = 0;
            while (si < (int)e.size - 15 && nsamp < MAX_AUDIO_SAMPLES) {
                if (sd[si + 1] & 1) {
                    int se = si + 16;
                    if (se + 16 <= (int)e.size && sd[se + 1] == 7) se += 16;
                    if (se - si >= 16) nsamp++;
                    si = se;
                } else si += 16;
            }
            _snprintf(label, 127, "#%d  SNDB  %d sample%s  %u bytes",
                i, nsamp, nsamp != 1 ? "s" : "", e.size);
            tree_add(hTree, hSounds, label, i);
        } break;
        case DAT_SNDE: {
            SeqHeader sh;
            if (e.data && e.size >= 15 && parse_seq_header(e.data, e.size, sh)) {
                _snprintf(label, 127, "#%d  SEQ  %.0f BPM  %u bytes", i, sh.bpm, e.size);
            } else {
                _snprintf(label, 127, "#%d  SNDE  %u bytes", i, e.size);
            }
            tree_add(hTree, hSounds, label, i);
        } break;

        case DAT_DC2_SOUND:
            _snprintf(label, 127, "#%d  Sound bank (not decoded)  %u bytes", i, e.size);
            tree_add(hTree, hSounds, label, i);
            break;
        case DAT_DC2_MP3:
            /* The word after the size is the track number (dinorand K96) */
            _snprintf(label, 127, "#%d  MP3 track %u  %u bytes", i, (unsigned)e.load_base, e.size);
            tree_add(hTree, hSounds, label, i);
            break;

        case DAT_LZSS0: {
            if (archive->is_dc2) { dc2_tree_add(hTree, hModels, i, e); break; }
            /* Use RDT layout for structured info + classification */
            RdtLayout layout;
            bool has_layout = false;
            EntrySubType st = SUB_COMPRESSED;

            bool raw = (e.type != DAT_LZSS0);  /* uncompressed RDT */
            if (e.y & 0x8000) {
                if (!raw && is_mips_code(e.data, e.size, e.y, e.x)) {
                    st = SUB_MIPS_CODE;
                } else {
                    if (raw) {
                        u32 base = ((u32)(e.y & 0x7FFF) << 16) | (u32)e.x | 0x80000000u;
                        has_layout = parse_rdt_layout(e.data, e.size, base, layout);
                    } else {
                        has_layout = parse_rdt_layout_from_entry(e.data, e.size,
                                                                 e.y, e.x, layout);
                    }
                    if (has_layout) {
                        if (layout.section_count > 0 && layout.emd_count > 0)
                            st = SUB_RDT_SCENE;
                        else if (layout.section_count > 0)
                            st = SUB_ROOM_MESH;
                        else if (layout.emd_count > 0)
                            st = SUB_EMD_MODEL;
                        else if (layout.valid)
                            st = SUB_RDT_SCENE; /* metadata-only room */
                    }
                    if (st == SUB_COMPRESSED &&
                        is_door_mesh_entry(e.data, e.size, e.y, e.w, e.h))
                        st = SUB_DOOR_MESH;
                    if (st == SUB_COMPRESSED &&
                        is_weapon_data(e.data, e.size, e.y, e.x))
                        st = SUB_WEAPON_DATA;
                }
            }

            int n_emds = has_layout ? layout.emd_count : 0;
            bool has_sections = has_layout && layout.section_count > 0;

            /* Store EMD offsets for later model loading */
            if (n_emds > 0 && g_emd_info_count < 64) {
                EntryEmdInfo& info = g_emd_info[g_emd_info_count++];
                info.entry_idx = i;
                info.count = n_emds;
                for (int k = 0; k < n_emds && k < MAX_EMDS_PER_ENTRY; k++)
                    info.offsets[k] = layout.emds[k].offset;
            }

            if (has_sections || n_emds > 0) {
                /* RDT scene or multi-model: container node with sub-items */
                if (has_sections && n_emds > 0) {
                    _snprintf(label, 127, "#%d  RDT Scene  %d sections + %d EMD%s  %u bytes",
                        i, layout.section_count, n_emds,
                        n_emds > 1 ? "s" : "", e.size);
                } else if (has_sections) {
                    int total_faces = layout.total_room_tris +
                                      layout.total_room_quads * 2;
                    _snprintf(label, 127, "#%d  Room Mesh  %d sections, %d faces  %u bytes",
                        i, layout.section_count, total_faces, e.size);
                } else {
                    _snprintf(label, 127, "#%d  Characters  (%d EMD%s)  %u bytes",
                        i, n_emds, n_emds > 1 ? "s" : "", e.size);
                }

                bool needs_children = (has_sections && n_emds > 0) ||
                                      n_emds > 1 ||
                                      (has_sections && n_emds == 1);
                if (needs_children) {
                    HTREEITEM hParent = tree_add(hTree, hModels, label, -1);
                    if (has_sections) {
                        _snprintf(label, 127, "Room Mesh (%d sections, %d tri + %d quad)",
                            layout.section_count, layout.total_room_tris,
                            layout.total_room_quads);
                        tree_add(hTree, hParent, label, (LPARAM)((1 << 16) | i));
                    }
                    for (int k = 0; k < n_emds; k++) {
                        const RdtEmdEntry& em = layout.emds[k];
                        _snprintf(label, 127, "EMD %d  %dv/%dt/%dq/%dp  @ 0x%X",
                            k + 1, em.vert_count, em.tri_count,
                            em.quad_count, em.part_count, (unsigned)em.offset);
                        tree_add(hTree, hParent, label, (LPARAM)(((k + 2) << 16) | i));
                    }
                    TreeView_Expand(hTree, hParent, TVE_EXPAND);
                } else {
                    tree_add(hTree, hModels, label, i);
                }
            } else if (has_layout && layout.valid && st == SUB_RDT_SCENE) {
                /* Metadata-only room (no mesh, has cameras/collision/scripts) */
                _snprintf(label, 127, "#%d  RDT Metadata (no mesh)  %u bytes",
                    i, e.size);
                tree_add(hTree, hModels, label, i);
            } else {
                if (st == SUB_WEAPON_DATA) {
                    Buffer dec;
                    if (lzss_decompress(e.data, e.size, dec)) {
                        _snprintf(label, 127, "#%d  Weapon Data  %u -> %u bytes  base=0x%05X",
                            i, e.size, (unsigned)dec.size,
                            (unsigned)(((u32)(e.y & 0x7FFF) << 16) | (u32)e.x));
                    } else {
                        _snprintf(label, 127, "#%d  Weapon Data  %u bytes", i, e.size);
                    }
                } else if (st == SUB_STANDALONE_EMD) {
                    Buffer dec;
                    if (lzss_decompress(e.data, e.size, dec)) {
                        _snprintf(label, 127, "#%d  Character Mesh  %u -> %u bytes  base=0x%05X",
                            i, e.size, (unsigned)dec.size,
                            (unsigned)(((u32)(e.y & 0x7FFF) << 16) | (u32)e.x));
                    } else {
                        _snprintf(label, 127, "#%d  Character Mesh  %u bytes", i, e.size);
                    }
                } else if (st == SUB_MIPS_CODE) {
                    Buffer dec;
                    if (lzss_decompress(e.data, e.size, dec)) {
                        _snprintf(label, 127, "#%d  Code Overlay  %u -> %u bytes  load@0x%05X",
                            i, e.size, (unsigned)dec.size,
                            (unsigned)(((u32)(e.y & 0x7FFF) << 16) | (u32)e.x));
                    } else {
                        _snprintf(label, 127, "#%d  Code Overlay  %u bytes", i, e.size);
                    }
                } else {
                    _snprintf(label, 127, "#%d  %s  %u bytes", i, sub_type_label(st), e.size);
                }
                tree_add(hTree, hModels, label, i);
            }
        } break;

        default:
            _snprintf(label, 127, "#%d  %s  %u bytes", i, dat_type_name(e.type), e.size);
            tree_add(hTree, hData, label, i);
            break;
        }
    }

    /* Expand all categories */
    if (hTextures) TreeView_Expand(hTree, hTextures, TVE_EXPAND);
    if (hPalettes) TreeView_Expand(hTree, hPalettes, TVE_EXPAND);
    if (hSounds) TreeView_Expand(hTree, hSounds, TVE_EXPAND);
    if (hModels) TreeView_Expand(hTree, hModels, TVE_EXPAND);
    if (hData) TreeView_Expand(hTree, hData, TVE_EXPAND);

    /* Auto-select the first leaf entry so the splash screen doesn't linger.
       Walk tree depth-first to find a node with lParam >= 0 (entry index). */
    HTREEITEM hFirst = TreeView_GetRoot(hTree);
    while (hFirst) {
        HTREEITEM hChild = TreeView_GetChild(hTree, hFirst);
        if (hChild) {
            /* Check if this child is a leaf with a valid entry */
            TVITEMA tvi;
            memset(&tvi, 0, sizeof(tvi));
            tvi.mask = TVIF_PARAM;
            tvi.hItem = hChild;
            TreeView_GetItem(hTree, &tvi);
            if (tvi.lParam >= 0) {
                TreeView_SelectItem(hTree, hChild);
                break;
            }
            /* Try grandchild (for container nodes like RDT Scene) */
            HTREEITEM hGrand = TreeView_GetChild(hTree, hChild);
            if (hGrand) {
                TreeView_SelectItem(hTree, hGrand);
                break;
            }
            TreeView_SelectItem(hTree, hChild);
            break;
        }
        hFirst = TreeView_GetNextSibling(hTree, hFirst);
    }
}

void App::set_status(const char* text) {
    if (hStatus) SetWindowTextA(hStatus, text);
}

int App::selected_entry_idx() {
    HTREEITEM hSel = TreeView_GetSelection(hTree);
    if (!hSel) return -1;
    TVITEMA tvi;
    memset(&tvi, 0, sizeof(tvi));
    tvi.mask = TVIF_PARAM;
    tvi.hItem = hSel;
    TreeView_GetItem(hTree, &tvi);
    return (int)tvi.lParam;  /* -1 for category nodes, otherwise entry_idx | (sub<<16) */
}

static void on_entry_select(int sel_param);

void App::refresh_selection() {
    int sel = selected_entry_idx();
    if (sel >= 0) on_entry_select(sel);
}

/*═══════════════════════════════════════════════════════════════════
 *  Panel Switching
 *═══════════════════════════════════════════════════════════════════*/
void App::switch_panel(int panel_id)
{
    if (panel_id == active_panel) return;

    /* Lazy-create SavePanel on first use */
    if (panel_id == PANEL_SAVE && !g_panels[PANEL_SAVE]) {
        RECT rc;
        GetClientRect(hPanelArea, &rc);
        g_panels[PANEL_SAVE] = CreateWindowExA(0, "DCSavePanel", "",
            WS_CHILD | WS_CLIPCHILDREN | WS_VSCROLL,
            0, 0, rc.right, rc.bottom, hPanelArea, 0, hInst, 0);
        g_save.hwnd = g_panels[PANEL_SAVE];
        /* Apply dark theme with progress overlay (slow — ~150ms per control) */
        SavePanel_FinalizeDark(hPanelArea);
    }

    /* Lazy-create WeaponPanel on first use */
    if (panel_id == PANEL_WEP && !g_panels[PANEL_WEP]) {
        RECT rc;
        GetClientRect(hPanelArea, &rc);
        g_panels[PANEL_WEP] = CreateWindowExA(0, "DCWeaponPanel", "",
            WS_CHILD | WS_CLIPCHILDREN,
            0, 0, rc.right, rc.bottom, hPanelArea, 0, hInst, 0);
        g_weapon.hwnd = g_panels[PANEL_WEP];
    }

    for (int i = 0; i < PANEL_COUNT; i++)
        if (g_panels[i]) ShowWindow(g_panels[i], (i == panel_id) ? SW_SHOW : SW_HIDE);
    active_panel = panel_id;
    if (panel_id == PANEL_3D && g_viewer3d.hRC) {
        RECT rc;
        GetClientRect(hPanelArea, &rc);
        int w = rc.right, h = rc.bottom;
        g_viewer3d.resize(w, h);
        /* Force reposition animation controls after panel becomes visible */
        if (w > 200 && h > 80) {
            if (g_viewer3d.anim_track)
                MoveWindow(g_viewer3d.anim_track, 98, h - 58, w - 180, 24, TRUE);
            if (g_viewer3d.anim_label)
                MoveWindow(g_viewer3d.anim_label, w - 78, h - 54, 74, 18, TRUE);
            if (g_viewer3d.anim_clip_combo)
                MoveWindow(g_viewer3d.anim_clip_combo, 4, h - 58, 90, 300, TRUE);
        }
        g_viewer3d.render();
    }
}

/*═══════════════════════════════════════════════════════════════════
 *  Layout
 *═══════════════════════════════════════════════════════════════════*/
static void layout_main(HWND hwnd)
{
    if (!g_app.hTree || !g_app.hPanelArea || !g_app.hStatus) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;

    /* Menu bar at top */
    MoveWindow(g_app.hMenuBar, 0, 0, w, MENUBAR_H, TRUE);

    /* Status bar at bottom (simple STATIC, like WBC) */
    MoveWindow(g_app.hStatus, 0, h - STATUS_H, w, STATUS_H, TRUE);

    /* Content area */
    int contentTop = MENUBAR_H;
    int contentH = h - MENUBAR_H - STATUS_H;

    /* Left: entry list, Right: panel area */
    int lw;
    if (g_app.nav_hidden) {
        lw = 0;
        ShowWindow(g_app.hTree, SW_HIDE);
    } else {
        lw = (w * 35) / 100;
        if (lw < 250) lw = 250;
        if (lw > w - 200) lw = w - 200;
        ShowWindow(g_app.hTree, SW_SHOW);
    }
    MoveWindow(g_app.hTree, 0, contentTop, lw, contentH, TRUE);
    MoveWindow(g_app.hPanelArea, lw, contentTop, w - lw, contentH, TRUE);

    /* Size all panels to fill panel area */
    RECT pr;
    GetClientRect(g_app.hPanelArea, &pr);
    for (int i = 0; i < PANEL_COUNT; i++)
        if (g_panels[i]) MoveWindow(g_panels[i], 0, 0, pr.right, pr.bottom, TRUE);
    /* Resize standalone text view if it exists */
    HWND hTextView = GetDlgItem(g_app.hPanelArea, 9500);
    if (hTextView) MoveWindow(hTextView, 0, 0, pr.right, pr.bottom, TRUE);
    if (g_viewer3d.hRC) g_viewer3d.resize(pr.right, pr.bottom);
}

/*═══════════════════════════════════════════════════════════════════
 *  Build the 3D viewer's atlas (build_texture_atlas, texture.cpp) from
 *  the archive's textures and the CLUTs the mesh's faces use, and
 *  upload it.
 *═══════════════════════════════════════════════════════════════════*/
/* The part of the atlas the UV editor shows: the texture pages the mesh's
   faces sample, and the CLUT row of its first face.  atlas_w is the
   atlas width in pixels.  set_texture cuts the editor's image by these,
   so they are set before it. */
static void set_uv_crop(const Mesh& mesh, int atlas_w)
{
    if (mesh.tri_count <= 0) {
        g_viewer3d.uv_crop_px_x = 0;
        g_viewer3d.uv_crop_px_w = 0;
        g_viewer3d.uv_crop_py_y = 0;
        g_viewer3d.uv_crop_py_h = 0;
        g_viewer3d.uv_tex_vram_x = g_viewer3d.tex_vram_x;
        g_viewer3d.uv_tex_vram_y = g_viewer3d.tex_vram_y;
        return;
    }
    int tex_ppw = (g_viewer3d.tex_bpp == 4) ? 4 : (g_viewer3d.tex_bpp == 16) ? 1 : 2;
    int min_px = 0x7FFFFFFF, max_px = 0;
    int min_vy = 512, max_vy_end = 0;
    for (int fi = 0; fi < mesh.tri_count; fi++) {
        int tx = mesh.tris[fi].tpage & 0xF;
        int ty = (mesh.tris[fi].tpage >> 4) & 1;
        int tp = (mesh.tris[fi].tpage >> 7) & 3;
        int face_ppw = (tp == 0) ? 4 : (tp == 2) ? 1 : 2;
        int px_start = (tx * 64 - (int)g_viewer3d.tex_vram_x) * tex_ppw;
        int page_pw = 256 * tex_ppw / face_ppw;
        int px_end = px_start + page_pw;
        if (px_start < min_px) min_px = px_start;
        if (px_end > max_px) max_px = px_end;
        int vy = ty * 256;
        if (vy < min_vy) min_vy = vy;
        if (vy + 256 > max_vy_end) max_vy_end = vy + 256;
    }
    if (min_px < 0) min_px = 0;
    if (max_px > atlas_w) max_px = atlas_w;

    g_viewer3d.uv_crop_px_x = min_px;
    g_viewer3d.uv_crop_px_w = max_px > min_px ? max_px - min_px : 0;
    g_viewer3d.uv_crop_py_y = min_vy;
    g_viewer3d.uv_crop_py_h = max_vy_end - min_vy;
    g_viewer3d.uv_tex_vram_x = (u16)((int)g_viewer3d.tex_vram_x + min_px / tex_ppw);
    g_viewer3d.uv_tex_vram_y = 0;  /* full slice: UVs use absolute VRAM Y */

    int cy = (mesh.tris[0].clut >> 6) & 0x1FF;
    int row = cy - g_viewer3d.clut_base_y;
    if (row >= g_viewer3d.num_pal_rows) row = g_viewer3d.num_pal_rows - 1;
    if (row < 0) row = 0;
    g_viewer3d.uv_cache_clut_row = row;
}

/* Build the atlas for the CLUTs in g_viewer3d.used_cluts and take over
   its layout.  Returns the image to upload (caller frees), or 0 when the
   viewer's texture stands as it is or was cleared.
   src: the archive to take textures and palettes from, when it is not
   the open one (a Dino Crisis 2 enemy's .TEX).  force rebuilds the atlas
   even when the cache below thinks it has this archive (B13). */
static u8* build_archive_atlas(DatArchive* src, bool force, TexAtlasLayout& lay)
{
    if (!src) src = g_app.archive;
    if (!src) return 0;
    DatArchive& ar = *src;

    /* ── Atlas cache: skip full rebuild if archive+sub-pal config unchanged ── */
    static DatArchive* s_cached_archive = 0;
    static int         s_cached_n_cluts = 0;
    static u16         s_cached_cluts[512];
    bool same_cluts = (g_viewer3d.n_used_cluts == s_cached_n_cluts &&
                       memcmp(s_cached_cluts, g_viewer3d.used_cluts, g_viewer3d.n_used_cluts * 2) == 0);
    if (!force && &ar == s_cached_archive && same_cluts && g_viewer3d.tex_w > 0)
        return 0;
    s_cached_archive = &ar;
    s_cached_n_cluts = g_viewer3d.n_used_cluts;
    memcpy(s_cached_cluts, g_viewer3d.used_cluts, g_viewer3d.n_used_cluts * 2);

    /* Cap atlas height to GL max texture size */
    GLint gl_max = 16384; /* conservative default */
    if (g_viewer3d.hRC && g_viewer3d.hDC) {
        wglMakeCurrent(g_viewer3d.hDC, g_viewer3d.hRC);
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl_max);
        wglMakeCurrent(0, 0);
    }
    if (gl_max < 1024) gl_max = 16384;

    u8* rgba = 0;
    int built = build_texture_atlas(ar, g_viewer3d.used_cluts, g_viewer3d.used_clut_is4bpp,
                                    g_viewer3d.n_used_cluts, gl_max, lay, &rgba);
    if (built == 0) {
        /* No valid texture in this archive — clear any stale texture from previous file */
        g_viewer3d.clear_texture();
        return 0;
    }
    if (built < 0) return 0;

    g_viewer3d.tex_bpp = lay.bpp;
    g_viewer3d.tex_vram_x = (u16)lay.vram_x;
    g_viewer3d.tex_vram_y = (u16)lay.vram_y;
    g_viewer3d.num_pal_rows = lay.num_pal_rows;
    g_viewer3d.clut_base_y = lay.clut_base_y;
    g_viewer3d.num_sub_pals = lay.num_slices;
    g_viewer3d.clut_base_x = lay.clut_base_x;
    memcpy(g_viewer3d.slice_map, lay.slice_map, sizeof(g_viewer3d.slice_map));
    return rgba;
}

/* Texture the 3D viewer for a mesh whose CLUTs are in g_viewer3d.used_cluts */
static void upload_archive_texture(const Mesh& mesh, DatArchive* src = 0, bool force = false)
{
    TexAtlasLayout lay;
    u8* rgba = build_archive_atlas(src, force, lay);
    set_uv_crop(mesh, rgba ? lay.w : g_viewer3d.tex_w);
    if (rgba) g_viewer3d.set_texture(rgba, lay.w, lay.h);
    free(rgba);
}

/* The static mesh of an LZSS0 entry (or raw RDT) as the 3D viewer shows it,
   which the OBJ/SMD exports share.  dd/ds is the entry's payload.

   The game draws only the sections its scripts place in model slots
   (0x23, item zones); mesh_apply_xforms builds exactly those.  Other
   sections are leftovers (unused variants, objects in local coordinates)
   and would pile up at the origin, so the sequential walk is only a
   fallback for rooms without placements.  with_unplaced adds the
   leftovers (such as ST103's unused DDK) at their raw coordinates,
   flagged alt: the viewer hides them unless H is pressed, and exports
   leave them out.  Returns the kind of mesh, or "" if there is none. */
static const char* build_entry_mesh(const u8* dd, size_t ds, u32 base, Mesh& mesh,
                                    RdtSceneOverlay& overlay, bool& has_overlay,
                                    bool with_unplaced)
{
    has_overlay = parse_rdt_overlay(dd, ds, base, overlay);
    bool placed = has_overlay && overlay.n_xforms > 0;
    if (!placed) {
        if (parse_rdt_scene_dec(dd, ds, base, mesh) && mesh.tri_count > 0)
            return "Room Scene";
        if (parse_room_mesh_dec(dd, ds, base, mesh) && mesh.tri_count > 0)
            return "Static Mesh";
        if (parse_standalone_emd_mesh_dec(dd, ds, base, mesh) && mesh.tri_count > 0)
            return "Character Mesh";
        if (parse_door_mesh_dec(dd, ds, base, mesh))
            return "Door Mesh";
        return "";
    }

    mesh.alloc(0, 0);
    mesh_apply_xforms(mesh, dd, ds, base, overlay.xforms, overlay.n_xforms);
    mesh_compute_smooth_normals(mesh);

    RdtLayout lay;
    if (with_unplaced && parse_rdt_layout(dd, ds, base, lay)) {
        RdtSectionXform unplaced[RDT_MAX_SECTIONS];
        int n_unplaced = 0;
        for (int si = 0; si < lay.section_count; si++) {
            u32 so = (u32)lay.sections[si].offset;
            bool used = false;
            for (int xi = 0; xi < overlay.n_xforms && !used; xi++)
                used = overlay.xforms[xi].section_off == so;
            if (used) continue;
            RdtSectionXform& u = unplaced[n_unplaced++];
            memset(&u, 0, sizeof(u));
            u.section_off = so;
        }
        int first_alt = mesh.tri_count;
        mesh_apply_xforms(mesh, dd, ds, base, unplaced, n_unplaced);
        for (int ti = first_alt; ti < mesh.tri_count; ti++)
            mesh.tris[ti].alt = 1;
    }
    return "Room Scene";
}

/* File > Export OBJ / SMD: the selected entry's character model, or else
   its static mesh as the viewer shows it, chosen the way on_entry_select
   chooses.  Characters are parsed afresh, in their bind pose, because the
   viewer animates g_emd_model's vertices in place. */
static void export_selected_mesh(HWND hwnd, bool smd)
{
    const char* title = smd ? "Export SMD" : "Export OBJ";
    int sel = g_app.selected_entry_idx();
    int eidx = sel & 0xFFFF;
    int sub = (sel >> 16) & 0xFF;
    Buffer dec;
    bool usable = sel >= 0 && g_app.archive && eidx < g_app.archive->count;
    if (usable && g_app.archive->is_dc2) {
        /* Dino Crisis 2: a model in its bind pose.  Rooms have no mesh. */
        const DatEntry& e = g_app.archive->entries[eidx];
        EmdModel m;
        if (e.type != DAT_LZSS0 || !dat_entry_payload(e, dec) ||
            !dc2_parse_model(dec.data, dec.size, e.load_base, m)) {
            MessageBoxA(hwnd, "Select a model entry first. A Dino Crisis 2 room has no mesh to export: its geometry is not decoded.",
                        title, MB_OK | MB_ICONINFORMATION);
            return;
        }
        char path[MAX_PATH];
        if (!ui_save_file(hwnd, path, MAX_PATH,
                          smd ? "SMD Files (*.smd)\0*.smd\0" : "OBJ Files (*.obj)\0*.obj\0",
                          title, smd ? "smd" : "obj"))
            return;
        bool ok = smd ? export_emd_smd(path, m) : export_mesh_obj(path, m.mesh);
        if (!ok) {
            MessageBoxA(hwnd, "Export failed: the file could not be written.", title, MB_OK | MB_ICONERROR);
            return;
        }
        char msg[MAX_PATH + 96];
        _snprintf(msg, sizeof(msg) - 1, "Exported model: %d verts, %d faces to %s",
                  m.mesh.vert_count, m.mesh.tri_count, path);
        msg[sizeof(msg) - 1] = 0;
        g_app.set_status(msg);
        return;
    }
    if (usable) {
        const DatEntry& e = g_app.archive->entries[eidx];
        usable = effective_entry_type(e) == DAT_LZSS0 && (e.y & 0x8000) &&
                 dat_entry_payload(e, dec) && dec.size > 0;
    }
    if (!usable) {
        MessageBoxA(hwnd, "Select a room, mesh or character entry first.", title,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    const DatEntry& e = g_app.archive->entries[eidx];
    const u8* dd = dec.data;
    size_t    ds = dec.size;
    u32 base = ((u32)(e.y & 0x7FFF) << 16) | (u32)e.x | 0x80000000u;

    /* sub >= 2: EMD by ordinal (sub-2).  sub=0: an EMD only when there is
       no room mesh, as on_entry_select does. */
    bool want_emd = sub >= 2;
    if (sub == 0) {
        RdtLayout room_layout;
        want_emd = !(parse_rdt_layout(dd, ds, base, room_layout) &&
                     room_layout.section_count > 0);
    }
    EmdModel emd;
    bool is_emd = false;
    if (want_emd) {
        int emd_ordinal = (sub >= 2) ? (sub - 2) : 0;
        int hint = -1;
        EntryEmdInfo* info = get_emd_info(eidx);
        if (info && emd_ordinal < info->count)
            hint = (int)info->offsets[emd_ordinal];
        is_emd = parse_emd_model_dec(dd, ds, e.y, e.x, emd, hint);
    }

    Mesh mesh;
    if (!is_emd) {
        RdtSceneOverlay overlay;
        bool has_overlay = false;
        if (!build_entry_mesh(dd, ds, base, mesh, overlay, has_overlay, false)[0]) {
            MessageBoxA(hwnd, "This entry has no mesh or character model to export.", title,
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    char path[MAX_PATH];
    if (!ui_save_file(hwnd, path, MAX_PATH,
                      smd ? "SMD Files (*.smd)\0*.smd\0" : "OBJ Files (*.obj)\0*.obj\0",
                      title, smd ? "smd" : "obj"))
        return;

    const Mesh& m = is_emd ? emd.mesh : mesh;
    bool ok = smd ? (is_emd ? export_emd_smd(path, emd) : export_mesh_smd(path, mesh))
                  : export_mesh_obj(path, m);
    if (!ok) {
        MessageBoxA(hwnd, "Export failed: the file could not be written.", title,
                    MB_OK | MB_ICONERROR);
        return;
    }
    char msg[MAX_PATH + 96];
    _snprintf(msg, sizeof(msg) - 1, "Exported %s: %d verts, %d faces to %s",
              is_emd ? "character" : "mesh", m.vert_count, m.tri_count, path);
    msg[sizeof(msg) - 1] = 0;
    g_app.set_status(msg);
}

/*═══════════════════════════════════════════════════════════════════
 *  Dino Crisis 2 entry selection
 *═══════════════════════════════════════════════════════════════════*/

/* Show bytes that live only in a local buffer: keep a copy for the hex view */
static void dc2_show_hex(const u8* data, size_t size)
{
    free(g_app.raw_hex_buf);
    g_app.raw_hex_buf = (u8*)malloc(size ? size : 1);
    g_app.raw_hex_size = g_app.raw_hex_buf ? size : 0;
    if (g_app.raw_hex_buf) memcpy(g_app.raw_hex_buf, data, size);
    g_hex.set_data(g_app.raw_hex_buf, g_app.raw_hex_size);
    InvalidateRect(g_hex.hwnd, 0, TRUE);
}

/* The archive holding the texture a model's header names: the open
   package, or else the .TEX of the same name next to it, or else, for
   WEP_P000.DAT and WEP_P100.DAT, the player's alternate-skin file as a
   stand-in (alternate skin).  0 when none has it. */
static DatArchive* dc2_texture_source(u16 tpage, u16 clut)
{
    int tex, pal;
    if (dc2_model_texture(*g_app.archive, tpage, clut, tex, pal)) return g_app.archive;
    if (!g_dc2_skin) {
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH - 1, "%s", g_app.archive->filename.c_str());
        path[MAX_PATH - 1] = 0;
        char* dot = strrchr(path, '.');
        if (dot && strlen(dot) == 4) {
            memcpy(dot, ".TEX", 4);
            g_dc2_skin = new DatArchive();
            g_dc2_skin_alt = false;
            if (!dat_parse_file(path, *g_dc2_skin)) { delete g_dc2_skin; g_dc2_skin = 0; }
        }
        char* base = path + strlen(path);
        while (base > path && base[-1] != '\\' && base[-1] != '/') base--;
        char alt[16];
        if (!g_dc2_skin && dc2_alt_skin_name(base, alt) && (base - path) + strlen(alt) < MAX_PATH) {
            strcpy(base, alt);
            g_dc2_skin = new DatArchive();
            g_dc2_skin_alt = true;
            if (!dat_parse_file(path, *g_dc2_skin)) { delete g_dc2_skin; g_dc2_skin = 0; }
        }
    }
    if (g_dc2_skin && dc2_model_texture(*g_dc2_skin, tpage, clut, tex, pal)) return g_dc2_skin;
    return 0;
}

/* A DCM model, in its bind pose.  Every face has the header's tpage and
   clut, so the atlas needs one CLUT and the UV editor one page. */
static bool dc2_show_model(int idx, const DatEntry& e, const u8* dd, size_t ds)
{
    if (g_emd_model) { delete g_emd_model; g_emd_model = 0; }
    g_emd_model = new EmdModel();
    if (!dc2_parse_model(dd, ds, e.load_base, *g_emd_model)) {
        delete g_emd_model; g_emd_model = 0;
        return false;
    }
    const Mesh& em = g_emd_model->mesh;
    u16 tpage = em.tri_count > 0 ? em.tris[0].tpage : 0;
    u16 clut  = em.tri_count > 0 ? em.tris[0].clut : 0;
    int depth = (tpage >> 7) & 3;      /* 0 = 4bpp, 1 = 8bpp, 2 = 16bpp */

    g_viewer3d.max_sub_pal = 0;
    g_viewer3d.n_used_cluts = 0;
    memset(g_viewer3d.slice_map, -1, sizeof(g_viewer3d.slice_map));
    memset(g_viewer3d.used_clut_is4bpp, 0, sizeof(g_viewer3d.used_clut_is4bpp));
    g_viewer3d.desired_bpp = (depth == 0) ? 4 : (depth == 2) ? 16 : 8;
    if (em.tri_count > 0) {
        g_viewer3d.used_cluts[0] = clut;
        g_viewer3d.used_clut_is4bpp[0] = (depth == 0);
        g_viewer3d.n_used_cluts = 1;
        if (depth == 0) g_viewer3d.max_sub_pal = clut & 0x3F;
    }
    /* Every enemy skin is one page at the same place with the same CLUT,
       and a closed archive's address is soon reused, so the atlas cache
       cannot tell E00's skin from E40's: always rebuild. */
    DatArchive* src = dc2_texture_source(tpage, clut);
    if (src) upload_archive_texture(em, src, true);
    else   { g_viewer3d.clear_texture(); set_uv_crop(em, 0); }

    g_viewer3d.set_emd(*g_emd_model);
    g_viewer3d.set_emd_anim(g_emd_model);

    char buf[256];
    _snprintf(buf, 255, "Entry %d: DC2 Model  -  %d verts, %d faces, %d bones  |  texture: %s  (bind pose; DC2 animation is not decoded)",
              idx, em.vert_count, em.tri_count, g_emd_model->skeleton.bone_count,
              !src ? "not found" : src != g_dc2_skin ? "this package" :
              g_dc2_skin_alt ? "stand-in (alternate skin file)" : "from the .TEX file");
    g_app.set_status(buf);
    g_app.switch_panel(PANEL_3D);
    return true;
}

/* An LZSS0 entry of a DC2 package.  sub = 0: the entry itself; sub = slot
   + 1: one section of a room, as hex.  Returns false to leave the entry
   to the hex view. */
static bool dc2_entry_select(int idx, int sub, const DatEntry& e)
{
    Buffer dec;
    if (!dat_entry_payload(e, dec) || dec.size == 0) return false;
    const u8* dd = dec.data;
    size_t    ds = dec.size;
    char buf[256];

    Dc2ModelInfo mi;
    if (dc2_model_probe(dd, ds, e.load_base, mi))
        return dc2_show_model(idx, e, dd, ds);

    Dc2RoomDir dir;
    if (dc2_room_directory(dd, ds, e.load_base, dir)) {
        int slot = sub - 1;
        if (slot >= 0 && slot < DC2_ROOM_SLOTS && dir.present[slot]) {
            dc2_show_hex(dd + dir.off[slot], dir.size[slot]);
            _snprintf(buf, 255, "Entry %d: Room slot %d  -  %s, %u bytes at 0x%X",
                      idx, slot, dc2_room_slot_name(slot), dir.size[slot], dir.off[slot]);
            g_app.set_status(buf);
            g_app.switch_panel(PANEL_HEX);
            return true;
        }

        /* The draw mesh is not decoded: the scene is the overlay, framed
           by the collision outline. */
        RdtSceneOverlay overlay;
        dc2_room_overlay(dd, ds, e.load_base, overlay);
        Mesh mesh;
        dc2_room_outline_mesh(overlay, mesh);
        g_viewer3d.clear_texture();
        g_viewer3d.uv_crop_px_x = g_viewer3d.uv_crop_px_w = 0;
        g_viewer3d.uv_crop_py_y = g_viewer3d.uv_crop_py_h = 0;
        g_viewer3d.set_mesh(mesh);
        overlay_floor_heights(mesh, overlay);
        g_viewer3d.show_overlay = true;
        g_viewer3d.set_overlay(overlay);
        g_scd.load_dc2(dd, ds, e.load_base);

        _snprintf(buf, 255, "Entry %d: DC2 Room  -  %d collision polygons, %d zones/doors/triggers/items, %d spawns, %d markers  (room mesh and cameras are not decoded)  [O=overlay, F4=SCD]",
                  idx, overlay.n_polys, overlay.n_zones, overlay.n_items, overlay.n_enemies);
        g_app.set_status(buf);
        g_app.switch_panel(PANEL_3D);
        return true;
    }

    /* TITLE.DAT's picture: 320x240, 15-bit, like a DC1 background */
    if (ds == 320u * 240u * 2u) {
        u8* rgba = (u8*)calloc(320 * 240, 4);
        if (rgba) {
            render_16bpp(dd, 320, 240, rgba);
            g_image.set_bitmap(rgba, 320, 240);
            free(rgba);
            _snprintf(buf, 255, "Entry %d: Background 320x240 16bpp (LZSS0), %u -> %u bytes",
                      idx, e.size, (unsigned)ds);
            g_app.set_status(buf);
            g_app.switch_panel(PANEL_IMAGE);
            InvalidateRect(g_image.hwnd, 0, FALSE);
            return true;
        }
    }

    /* Anything else: the decompressed bytes */
    dc2_show_hex(dd, ds);
    _snprintf(buf, 255, "Entry %d: Compressed Data (LZSS0), %u -> %u bytes, load@0x%08X  (decompressed bytes shown)",
              idx, e.size, (unsigned)ds, (unsigned)e.load_base);
    g_app.set_status(buf);
    g_app.switch_panel(PANEL_HEX);
    return true;
}

/*═══════════════════════════════════════════════════════════════════
 *  Entry selection  -  auto-switch panel based on type
 *═══════════════════════════════════════════════════════════════════*/
static void on_entry_select(int sel_param)
{
    /* Decode: low 16 bits = entry index, bits 16-23 = sub-selector
       sub=0: default (auto-detect), sub=1: static mesh only, sub=2: EMD only */
    int idx = sel_param & 0xFFFF;
    int sub = (sel_param >> 16) & 0xFF;

    if (!g_app.archive || idx < 0 || idx >= g_app.archive->count) return;
    const DatEntry& e = g_app.archive->entries[idx];

    /* Always update hex view */
    g_hex.set_data(e.data, e.size);
    InvalidateRect(g_hex.hwnd, 0, TRUE);

    int panel = PANEL_HEX;

    switch (effective_entry_type(e)) {
    case DAT_TEXTURE_LINEAR: {
        /* Item bank: CLUT is at the start of the NEXT block (after this
           block's pixel data), not before the current block's pixels.
           Block layout: [CLUT for prev item (512)] [Pixels for this item (9728)]
           So the correct CLUT offset = pixel_data + pixel_size */
        panel = PANEL_IMAGE;
        const DatEntry* pal = 0;
        DatEntry tmp_pal;
        if (e.data && g_app.archive->raw) {
            ptrdiff_t clut_off = (e.data - g_app.archive->raw) + e.size;
            if (clut_off >= 0 && (size_t)(clut_off + ITEM_BANK_BLOCK_CLUT) <= g_app.archive->raw_size) {
                tmp_pal.type = DAT_PALETTE;
                tmp_pal.size = ITEM_BANK_BLOCK_CLUT;
                tmp_pal.data = g_app.archive->raw + clut_off;
                pal = &tmp_pal;
            }
        }
        /* Item bank sprites are always 8bpp with 256-color embedded CLUT */
        g_image.bpp = 8;
        g_image.sub_pal = 0;
        g_image.pal_row = 0;
        g_image.render_linear(e, pal);
        InvalidateRect(g_image.hwnd, 0, FALSE);
    } break;
    case DAT_TEXTURE: case DAT_LZSS1: case DAT_DC2_LZSS2: {
        panel = PANEL_IMAGE;
        /* Find best palette: nearest FOLLOWING DAT_PALETTE entry
           with no intervening texture entry (tex→pal adjacency).
           Fallback: nearest preceding palette, then first palette. */
        const DatEntry* pal = 0;
        for (int j = idx + 1; j < g_app.archive->count; j++) {
            u32 jt = g_app.archive->entries[j].type;
            if (jt == DAT_PALETTE) { pal = &g_app.archive->entries[j]; break; }
            if (jt == DAT_TEXTURE || jt == DAT_LZSS1 || jt == DAT_TEXTURE_LINEAR ||
                jt == DAT_DC2_LZSS2) break;
        }
        if (!pal) {
            for (int j = idx - 1; j >= 0; j--) {
                if (g_app.archive->entries[j].type == DAT_PALETTE)
                    { pal = &g_app.archive->entries[j]; break; }
            }
        }
        if (!pal) {
            for (int j = 0; j < g_app.archive->count; j++)
                if (g_app.archive->entries[j].type == DAT_PALETTE)
                    { pal = &g_app.archive->entries[j]; break; }
        }
        g_image.bpp = palette_bpp(pal ? pal->size : 0);
        g_image.render_entry(e, pal);
        InvalidateRect(g_image.hwnd, 0, FALSE);
    } break;
    case DAT_PALETTE:
        panel = PANEL_PALETTE;
        g_palette.set_palette(e.data, e.size);
        InvalidateRect(g_palette.hwnd, 0, TRUE);
        break;
    case DAT_SNDB:
        panel = PANEL_AUDIO;
        g_audio.decode_sndb(e);
        {
            char buf[256];
            _snprintf(buf, 255, "Entry %d: SNDB - %d sample%s  [Click/Up/Down] select  [Space] play",
                idx, g_audio.num_samples, g_audio.num_samples != 1 ? "s" : "");
            g_app.set_status(buf);
        }
        break;
    case DAT_SNDH:
        panel = PANEL_AUDIO;
        g_audio.decode_sndh(e);
        {
            char buf[256];
            if (g_audio.gian.valid)
                _snprintf(buf, 255, "Entry %d: Gian Header - %d programs, %d tones, %d VAGs",
                    idx, g_audio.gian.num_programs, g_audio.gian.tone_count, g_audio.gian.num_vag);
            else
                _snprintf(buf, 255, "Entry %d: SNDH  %u bytes", idx, e.size);
            g_app.set_status(buf);
        }
        break;
    case DAT_SNDE:
        panel = PANEL_AUDIO;
        g_audio.decode_snde(e);
        {
            char buf[256];
            if (g_audio.seq_hdr.valid)
                _snprintf(buf, 255, "Entry %d: SEQ - %.0f BPM, %d notes, %.1fs",
                    idx, g_audio.seq_hdr.bpm, g_audio.seq_note_count, g_audio.seq_duration);
            else
                _snprintf(buf, 255, "Entry %d: SNDE  %u bytes", idx, e.size);
            g_app.set_status(buf);
        }
        break;
    case DAT_LZSS0:
        if (g_app.archive->is_dc2) {
            if (dc2_entry_select(idx, sub, e)) return;
            break;
        }
        if (e.y & 0x8000) {
            u32 base = ((u32)(e.y & 0x7FFF) << 16) | (u32)e.x | 0x80000000u;

            /* ── Decompress ONCE, reuse for all parse/detect calls ── */
            Buffer lz_dec;
            if (!dat_entry_payload(e, lz_dec) || lz_dec.size == 0)
                break;
            const u8* dd = lz_dec.data;
            size_t    ds = lz_dec.size;

            /* sub >= 2: specific EMD by ordinal (sub-2).  sub=0 tries an EMD
               first only when there is no room mesh, so a room never opens
               as one of its characters. */
            bool has_room = false;
            if (sub == 0) {
                RdtLayout room_layout;
                has_room = parse_rdt_layout(dd, ds, base, room_layout) &&
                           room_layout.section_count > 0;
            }
            if (sub >= 2 || (sub == 0 && !has_room)) {
                int emd_ordinal = (sub >= 2) ? (sub - 2) : 0;
                int hint = -1;

                /* Look up cached EMD offset from tree building */
                EntryEmdInfo* info = get_emd_info(idx);
                if (info && emd_ordinal < info->count) {
                    hint = (int)info->offsets[emd_ordinal];
                }

                /* Free old persistent EMD model */
                if (g_emd_model) { delete g_emd_model; g_emd_model = 0; }
                g_emd_model = new EmdModel();
                if (parse_emd_model_dec(dd, ds, e.y, e.x, *g_emd_model, hint)) {
                    panel = PANEL_3D;

                    /* Detect dominant BPP and collect unique CLUTs from mesh faces */
                    g_viewer3d.max_sub_pal = 0;
                    g_viewer3d.n_used_cluts = 0;
                    memset(g_viewer3d.slice_map, -1, sizeof(g_viewer3d.slice_map));
                    memset(g_viewer3d.used_clut_is4bpp, 0, sizeof(g_viewer3d.used_clut_is4bpp));
                    if (g_emd_model->mesh.tri_count > 0) {
                        int tp0 = (g_emd_model->mesh.tris[0].tpage >> 7) & 3;
                        g_viewer3d.desired_bpp = (tp0 == 0) ? 4 : (tp0 == 2) ? 16 : 8;
                        for (int fi = 0; fi < g_emd_model->mesh.tri_count; fi++) {
                            int ftp = (g_emd_model->mesh.tris[fi].tpage >> 7) & 3;
                            bool is4 = (ftp == 0);
                            if (is4) {
                                int cx = (g_emd_model->mesh.tris[fi].clut & 0x3F) * 16;
                                int sp = cx / 16;
                                if (sp > g_viewer3d.max_sub_pal)
                                    g_viewer3d.max_sub_pal = sp;
                            }
                            u16 cl = g_emd_model->mesh.tris[fi].clut;
                            int idx = -1;
                            for (int ci = 0; ci < g_viewer3d.n_used_cluts; ci++)
                                if (g_viewer3d.used_cluts[ci] == cl) { idx = ci; break; }
                            if (idx < 0 && g_viewer3d.n_used_cluts < 512) {
                                idx = g_viewer3d.n_used_cluts++;
                                g_viewer3d.used_cluts[idx] = cl;
                            }
                            if (idx >= 0 && is4)
                                g_viewer3d.used_clut_is4bpp[idx] = true;
                        }
                    }

                    /* Upload atlas (cached — only rebuilds if archive/bpp changed) */
                    upload_archive_texture(g_emd_model->mesh);

                    g_viewer3d.set_emd(*g_emd_model);
                    g_viewer3d.set_emd_anim(g_emd_model);
                    char buf[256];
                    _snprintf(buf, 255, "Entry %d: EMD Character %d  -  %d verts, %d faces, %d bones, %d anims",
                        idx, emd_ordinal + 1,
                        g_emd_model->mesh.vert_count, g_emd_model->mesh.tri_count,
                        g_emd_model->skeleton.bone_count, g_emd_model->clip_count);
                    g_app.set_status(buf);
                    g_app.switch_panel(panel);
                    return;
                }
                delete g_emd_model; g_emd_model = 0;
            }

            /* sub=1: static mesh only, or sub=0 fallback */
            {
                Mesh mesh;
                RdtSceneOverlay overlay;
                bool has_overlay = false;
                const char* mesh_type = build_entry_mesh(dd, ds, base, mesh, overlay,
                                                         has_overlay, true);
                if (mesh_type[0]) {
                    panel = PANEL_3D;
                    g_viewer3d.uv_cache_clut_row = 0;
                    int ov_zones = 0, ov_cols = 0, ov_cams = 0, ov_spawns = 0;

                    /* Detect dominant BPP and collect unique CLUT values from ALL faces.
                       upload_archive_texture will build the 2D sparse atlas map from these. */
                    g_viewer3d.max_sub_pal = 0;
                    g_viewer3d.n_used_cluts = 0;
                    memset(g_viewer3d.slice_map, -1, sizeof(g_viewer3d.slice_map));
                    memset(g_viewer3d.used_clut_is4bpp, 0, sizeof(g_viewer3d.used_clut_is4bpp));
                    if (mesh.tri_count > 0) {
                        int tp0 = (mesh.tris[0].tpage >> 7) & 3;
                        g_viewer3d.desired_bpp = (tp0 == 0) ? 4 : (tp0 == 2) ? 16 : 8;
                        for (int fi = 0; fi < mesh.tri_count; fi++) {
                            int ftp = (mesh.tris[fi].tpage >> 7) & 3;
                            bool is4 = (ftp == 0);
                            if (is4) {
                                int cx = (mesh.tris[fi].clut & 0x3F) * 16;
                                int sp = cx / 16;
                                if (sp > g_viewer3d.max_sub_pal)
                                    g_viewer3d.max_sub_pal = sp;
                            }
                            /* Add unique CLUT value with 4bpp flag */
                            u16 cl = mesh.tris[fi].clut;
                            int idx = -1;
                            for (int ci = 0; ci < g_viewer3d.n_used_cluts; ci++)
                                if (g_viewer3d.used_cluts[ci] == cl) { idx = ci; break; }
                            if (idx < 0 && g_viewer3d.n_used_cluts < 512) {
                                idx = g_viewer3d.n_used_cluts++;
                                g_viewer3d.used_cluts[idx] = cl;
                            }
                            if (idx >= 0 && is4)
                                g_viewer3d.used_clut_is4bpp[idx] = true;
                        }
                    }

                    /* Upload atlas (cached) */
                    upload_archive_texture(mesh);

                    /* Apply xform instancing using already-parsed overlay */
                    /* (already done above, before max_sub_pal) */

                    g_viewer3d.set_mesh(mesh);

                    if (has_overlay) {
                        ov_zones  = overlay.n_zones;
                        ov_cols   = overlay.n_collisions;
                        ov_cams   = overlay.n_cameras;
                        ov_spawns = overlay.n_items;   /* 0x5B enemy spawns */
                        overlay_floor_heights(mesh, overlay);
                        g_viewer3d.set_overlay(overlay);
                    }

                    char buf[256];
                    if (has_overlay) {
                        int ov_enemies = overlay.n_enemies;
                        _snprintf(buf, 255, "Entry %d: %s  -  %d v, %d f  |  %d zones, %d cols, %d cams, %d enemies, %d objects [O=overlay, F4=SCD]",
                            idx, mesh_type, mesh.vert_count, mesh.tri_count,
                            ov_zones, ov_cols, ov_cams, ov_spawns, ov_enemies);
                    } else {
                        _snprintf(buf, 255, "Entry %d: %s  -  %d verts, %d faces",
                            idx, mesh_type, mesh.vert_count, mesh.tri_count);
                    }
                    g_app.set_status(buf);

                    /* Also disassemble SCD scripts for the SCD panel */
                    g_scd.load(dd, ds, base);

                    g_app.switch_panel(panel);
                    return;
                }
            }

            /* Weapon data: show info panel with hex dump summary */
            if (is_weapon_data_dec(dd, ds)) {
                int gpu_cmds = 0;
                for (size_t gi = 0; gi + 3 < ds; gi += 4) {
                    u8 cmd = dd[gi + 3];
                    if (cmd >= 0x20 && cmd <= 0x3F) gpu_cmds++;
                }
                char buf[256];
                _snprintf(buf, 255, "Entry %d: Weapon Data  -  %u -> %u bytes, %d GPU commands, base=0x%08X",
                    idx, e.size, (unsigned)ds, gpu_cmds,
                    (unsigned)(((u32)(e.y & 0x7FFF) << 16) | (u32)e.x | 0x80000000u));
                g_app.set_status(buf);
                g_app.switch_panel(PANEL_HEX);
                return;
            }

            /* Code overlay: show info */
            if (is_mips_code_dec(dd, ds)) {
                char buf[256];
                _snprintf(buf, 255, "Entry %d: MIPS Code Overlay  -  %u -> %u bytes, load@0x%08X",
                    idx, e.size, (unsigned)ds,
                    (unsigned)(((u32)(e.y & 0x7FFF) << 16) | (u32)e.x | 0x80000000u));
                g_app.set_status(buf);
                g_app.switch_panel(PANEL_HEX);
                return;
            }

            /* Fallback: try rendering as texture/background data */
            {
                /* Check for 16bpp BGR555 background (320x240 = 153600 bytes) */
                static const size_t BG_SIZE_16BPP = 320 * 240 * 2; /* 153600 */
                size_t pixel_off = 0;
                int bg_w = 320, bg_h = 240;
                bool is_bg = false;

                if (ds == BG_SIZE_16BPP) {
                    is_bg = true;
                } else if (ds > BG_SIZE_16BPP) {
                    size_t prefix = ds - BG_SIZE_16BPP;
                    if (prefix % 8 == 0 && prefix <= 2048) {
                        bool valid = true;
                        for (size_t fi = 0; fi < prefix; fi += 8) {
                            if (dd[fi + 6] != 0 || dd[fi + 7] != 0) {
                                valid = false;
                                break;
                            }
                        }
                        if (valid) {
                            pixel_off = prefix;
                            is_bg = true;
                        }
                    }
                }

                if (is_bg) {
                    u8* rgba = (u8*)calloc(bg_w * bg_h, 4);
                    if (rgba) {
                        render_16bpp(dd + pixel_off, bg_w, bg_h, rgba);
                        g_image.set_bitmap(rgba, bg_w, bg_h);
                        free(rgba);
                        panel = PANEL_IMAGE;
                        char buf[256];
                        _snprintf(buf, 255, "Entry %d: Background 320x240 16bpp (LZSS0), %u -> %u bytes",
                            idx, e.size, (unsigned)ds);
                        g_app.set_status(buf);
                        g_app.switch_panel(panel);
                        InvalidateRect(g_image.hwnd, 0, FALSE);
                        return;
                    }
                }

                /* Non-background: classify and render appropriately */
                {
                    bool has_dims = (e.w > 0 && e.h > 0);
                    const DatEntry* pal = 0;
                    for (int j = idx + 1; j < g_app.archive->count; j++) {
                        if (g_app.archive->entries[j].type == DAT_PALETTE)
                            { pal = &g_app.archive->entries[j]; break; }
                    }
                    if (!pal) {
                        for (int j = idx - 1; j >= 0; j--)
                            if (g_app.archive->entries[j].type == DAT_PALETTE)
                                { pal = &g_app.archive->entries[j]; break; }
                    }

                    if (has_dims && pal) {
                        DatEntry fake;
                        fake.type = DAT_TEXTURE;
                        fake.data = (u8*)dd;
                        fake.size = (u32)ds;
                        fake.x = e.x; fake.y = e.y;
                        fake.w = e.w; fake.h = e.h;
                        g_image.bpp = palette_bpp(pal->size);
                        g_image.render_entry(fake, pal);
                        panel = PANEL_IMAGE;
                        char buf[256];
                        _snprintf(buf, 255, "Entry %d: Compressed Texture (LZSS0), %u -> %u bytes",
                            idx, e.size, (unsigned)ds);
                        g_app.set_status(buf);
                    } else if (ds >= 1024) {
                        int gpu_cmds = 0;
                        for (size_t gi = 0; gi + 3 < ds; gi += 4) {
                            u8 cmd = dd[gi + 3];
                            if (cmd >= 0x20 && cmd <= 0x3F) gpu_cmds++;
                        }
                        if (gpu_cmds > 50) {
                            panel = PANEL_HEX;
                            char buf[256];
                            _snprintf(buf, 255, "Entry %d: Unrecognized Mesh Data (LZSS0), %u -> %u bytes, %d GPU cmds, base=0x%05X",
                                idx, e.size, (unsigned)ds, gpu_cmds,
                                (unsigned)(((u32)(e.y & 0x7FFF) << 16) | (u32)e.x));
                            g_app.set_status(buf);
                        } else {
                        int vw = 256;
                        int vh = (int)(ds / (vw * 2));
                        if (vh < 1) vh = 1;
                        if (vh > 512) vh = 512;
                        u8* rgba = (u8*)calloc((size_t)vw * vh, 4);
                        if (rgba) {
                            render_16bpp(dd, vw, vh, rgba);
                            g_image.set_bitmap(rgba, vw, vh);
                            free(rgba);
                        }
                        panel = PANEL_IMAGE;
                        char buf[256];
                        _snprintf(buf, 255, "Entry %d: VRAM Data (LZSS0), %u -> %u bytes, base=0x%05X",
                            idx, e.size, (unsigned)ds,
                            (unsigned)(((u32)(e.y & 0x7FFF) << 16) | (u32)e.x));
                        g_app.set_status(buf);
                        }
                    } else {
                        panel = PANEL_HEX;
                        char buf[256];
                        _snprintf(buf, 255, "Entry %d: Compressed Data (LZSS0), %u -> %u bytes",
                            idx, e.size, (unsigned)ds);
                        g_app.set_status(buf);
                    }
                    g_app.switch_panel(panel);
                    InvalidateRect(g_image.hwnd, 0, FALSE);
                    return;
                }
            }
        }
        break;
    case DAT_DATA: {
        /* Detect uncompressed 16bpp backgrounds: 153600 = 320*240*2 */
        if (e.size == 320u * 240u * 2u) {
            panel = PANEL_IMAGE;
            u8* rgba = (u8*)calloc(320 * 240, 4);
            if (rgba) {
                render_16bpp(e.data, 320, 240, rgba);
                g_image.set_bitmap(rgba, 320, 240);
                free(rgba);
                char buf[256];
                _snprintf(buf, 255, "Entry %d: Background 320x240 16bpp (raw), %u bytes",
                    idx, e.size);
                g_app.set_status(buf);
                g_app.switch_panel(panel);
                InvalidateRect(g_image.hwnd, 0, FALSE);
                return;
            }
        }
        /* Raw texture with VRAM coords and dimensions */
        if ((e.y & 0x8000) && e.w > 0 && e.h > 0) {
            panel = PANEL_IMAGE;
            const DatEntry* pal = 0;
            for (int j = idx - 1; j >= 0; j--) {
                if (g_app.archive->entries[j].type == DAT_PALETTE) {
                    pal = &g_app.archive->entries[j]; break;
                }
            }
            if (!pal) {
                for (int j = 0; j < g_app.archive->count; j++)
                    if (g_app.archive->entries[j].type == DAT_PALETTE)
                        { pal = &g_app.archive->entries[j]; break; }
            }
            g_image.bpp = palette_bpp(pal ? pal->size : 0);
            g_image.render_entry(e, pal);
            char buf[256];
            _snprintf(buf, 255, "Entry %d: Raw Texture %ux%u, %u bytes",
                idx, e.w, e.h, e.size);
            g_app.set_status(buf);
            g_app.switch_panel(panel);
            InvalidateRect(g_image.hwnd, 0, FALSE);
            return;
        }
    } break;
    }

    g_app.switch_panel(panel);

    char buf[256];
    if (e.type == DAT_TEXTURE || e.type == DAT_LZSS1 || e.type == DAT_TEXTURE_LINEAR) {
        int tx = e.x / 64;
        int ty = e.y / 256;
        int tpage = ty * 16 + tx;
        int pw = (e.w > 0) ? e.w * 2 : 0;  /* pixel width at 8bpp */
        _snprintf(buf, 255, "Entry %d: %s  VRAM(%d,%d) %ux%u  TPage TX=%d TY=%d → p%02d.png  %u bytes",
                  idx, dat_type_name(e.type), e.x, e.y, e.w, e.h, tx, ty, tpage, e.size);
    } else if (e.type == DAT_PALETTE) {
        _snprintf(buf, 255, "Entry %d: Palette  VRAM(%d,%d) %dx%d  %d rows  %u bytes",
                  idx, e.x, e.y, e.w, e.h, (int)(e.size / 512), e.size);
    } else {
        _snprintf(buf, 255, "Entry %d: %s, %u bytes", idx, dat_type_name(e.type), e.size);
    }
    g_app.set_status(buf);
}

/*═══════════════════════════════════════════════════════════════════
 *  Status bar painting (manual, like WBC viewer)
 *═══════════════════════════════════════════════════════════════════*/
static void PaintStatusBar(HDC hdc, const RECT* rc)
{
    HBRUSH sb = CreateSolidBrush(T.statusBg);
    FillRect(hdc, rc, sb);
    DeleteObject(sb);

    /* Top border */
    HPEN hp = CreatePen(PS_SOLID, 1, T.border);
    HPEN hpo = (HPEN)SelectObject(hdc, hp);
    MoveToEx(hdc, rc->left, rc->top, NULL);
    LineTo(hdc, rc->right, rc->top);
    SelectObject(hdc, hpo);
    DeleteObject(hp);

    char txt[256];
    txt[0] = '\0';
    GetWindowTextA(g_app.hStatus, txt, 255);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, T.statusText);
    HFONT hOld = (HFONT)SelectObject(hdc, g_app.hFontUI);
    RECT tr;
    tr.left = rc->left + 6;
    tr.top = rc->top;
    tr.right = rc->right;
    tr.bottom = rc->bottom;
    DrawTextA(hdc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOld);
}

/*═══════════════════════════════════════════════════════════════════
 *  ListView header painting (dark)
 *═══════════════════════════════════════════════════════════════════*/
static WNDPROC g_origHeaderProc = 0;
static LRESULT CALLBACK DarkHeaderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND && T.isDark) return 1;
    if (msg == WM_PAINT && T.isDark) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(T.panelBg);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        int cnt = (int)SendMessageA(hwnd, HDM_GETITEMCOUNT, 0, 0);
        for (int i = 0; i < cnt; i++) {
            RECT ir;
            SendMessageA(hwnd, HDM_GETITEMRECT, i, (LPARAM)&ir);

            /* Column separator */
            HPEN sp = CreatePen(PS_SOLID, 1, T.border);
            HPEN op = (HPEN)SelectObject(hdc, sp);
            MoveToEx(hdc, ir.right - 1, ir.top + 2, 0);
            LineTo(hdc, ir.right - 1, ir.bottom - 2);
            SelectObject(hdc, op);
            DeleteObject(sp);

            /* Column text */
            char buf[64];
            buf[0] = '\0';
            HDITEMA hdi;
            memset(&hdi, 0, sizeof(hdi));
            hdi.mask = HDI_TEXT | HDI_FORMAT;
            hdi.pszText = buf;
            hdi.cchTextMax = 63;
            SendMessageA(hwnd, HDM_GETITEMA, i, (LPARAM)&hdi);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, T.text);
            HFONT of = (HFONT)SelectObject(hdc, g_app.hFontUI);
            RECT tr;
            tr.left = ir.left + 6;
            tr.top = ir.top;
            tr.right = ir.right - 4;
            tr.bottom = ir.bottom;
            UINT fmt = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
            fmt |= (hdi.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT;
            DrawTextA(hdc, buf, -1, &tr, fmt);
            SelectObject(hdc, of);
        }

        /* Bottom border */
        HPEN bp = CreatePen(PS_SOLID, 1, T.border);
        HPEN obp = (HPEN)SelectObject(hdc, bp);
        MoveToEx(hdc, 0, rc.bottom - 1, 0);
        LineTo(hdc, rc.right, rc.bottom - 1);
        SelectObject(hdc, obp);
        DeleteObject(bp);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_origHeaderProc, hwnd, msg, wp, lp);
}

/*═══════════════════════════════════════════════════════════════════
 *  Main WndProc
 *═══════════════════════════════════════════════════════════════════*/
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        layout_main(hwnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        /* Only paint status bar area manually (like WBC) */
        RECT sr;
        sr.left = 0;
        sr.top = rc.bottom - STATUS_H;
        sr.right = rc.right;
        sr.bottom = rc.bottom;
        PaintStatusBar(hdc, &sr);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_hBrBg);
        return 1;
    }

    case WM_MOUSEWHEEL: {
        /* Forward mouse wheel to the child window under the cursor.
           Win32 sends WM_MOUSEWHEEL to the focused window, but we want
           it to go to whatever panel the mouse is hovering over. */
        POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
        HWND target = WindowFromPoint(pt);
        if (target && target != hwnd) {
            return SendMessage(target, msg, wp, lp);
        }
    } break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_FILE_OPEN: {
            char path[MAX_PATH];
            if (ui_open_file(hwnd, path, MAX_PATH,
                "All Supported Files\0*.dat;*.bin;*.imd;*.tim;*.dns;*.dno;*.str;*.avi;*.mpg;*.emd;*.wav;*.dbs;*.tex;*.pxl\0"
                "DAT Archives (*.dat)\0*.dat\0"
                "Dino Crisis 2 (*.dat;*.dbs;*.tex;*.pxl)\0*.dat;*.dbs;*.tex;*.pxl\0"
                "WAV Audio (*.wav)\0*.wav\0"
                "BIN Files (*.bin)\0*.bin\0"
                "IMD Images (*.imd)\0*.imd\0"
                "TIM Images (*.tim)\0*.tim\0"
                "MPEG Video (*.str;*.avi;*.mpg)\0*.str;*.avi;*.mpg\0"
                "EMD Models (*.emd)\0*.emd\0"
                "Saves (*.dns;*.dno)\0*.dns;*.dno\0"
                "All Files (*.*)\0*.*\0",
                "Open Archive, Image, Movie, or Save")) {
                /* Probe header to detect file type */
                FILE* f = fopen(path, "rb");
                if (f) {
                    u8 hdr[32]; size_t n = fread(hdr, 1, 32, f);
                    fseek(f, 0, SEEK_END); long file_sz = ftell(f);
                    fclose(f);
                    if (n >= 4 && is_mpeg_ps(hdr, n)) {
                        /* Load entire file and decode as video */
                        f = fopen(path, "rb");
                        if (f) {
                            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                            u8* buf = (u8*)malloc(sz);
                            if (buf) {
                                fread(buf, 1, sz, f);
                                fclose(f);
                                g_app.close_archive();
                                g_app.set_status("Decoding video...");
                                if (g_video.load(buf, sz)) {
                                    g_app.switch_panel(PANEL_VIDEO);
                                    char msg[512];
                                    _snprintf(msg, 511, "Video: %dx%d, %.1f fps, %d frames - %s",
                                        g_video.video.width, g_video.video.height,
                                        g_video.video.fps, g_video.video.frame_count, path);
                                    g_app.set_status(msg);
                                    char title[512];
                                    _snprintf(title, 511, TITLE_PREFIX "%s", path);
                                    SetWindowTextA(g_app.hMain, title);
                                } else {
                                    g_app.set_status("ERROR: Failed to decode MPEG-1 video");
                                }
                                free(buf);
                            } else { fclose(f); }
                        }
                    } else if (n >= 0x14 && is_imd_image(hdr, (size_t)file_sz)) {
                        /* Dino Crisis standalone IMD image */
                        f = fopen(path, "rb");
                        if (f) {
                            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                            u8* buf = (u8*)malloc(sz);
                            if (buf) {
                                fread(buf, 1, sz, f); fclose(f);
                                ImdImage img;
                                if (parse_imd_image(buf, (size_t)sz, img) && img.rgba) {
                                    g_app.close_archive();
                                    /* Convert RGBA8 to flat u8 RGBA for set_bitmap */
                                    u8* rgba = (u8*)malloc((size_t)img.width * img.height * 4);
                                    if (rgba) {
                                        for (int pi = 0; pi < img.width * img.height; pi++) {
                                            rgba[pi*4+0] = img.rgba[pi].r;
                                            rgba[pi*4+1] = img.rgba[pi].g;
                                            rgba[pi*4+2] = img.rgba[pi].b;
                                            rgba[pi*4+3] = img.rgba[pi].a;
                                        }
                                        g_image.set_bitmap(rgba, img.width, img.height);
                                        free(rgba);
                                    }
                                    g_app.switch_panel(PANEL_IMAGE);
                                    /* Clear tree and add info node */
                                    if (g_app.hTree) {
                                        TreeView_DeleteAllItems(g_app.hTree);
                                        const char* fname = strrchr(path, '\\');
                                        if (!fname) fname = strrchr(path, '/');
                                        fname = fname ? fname + 1 : path;
                                        char node[256];
                                        _snprintf(node, 255, "%s", fname);
                                        HTREEITEM hRoot = tree_add(g_app.hTree, TVI_ROOT, node, -1);
                                        _snprintf(node, 255, "%dx%d, 16bpp BGR555", img.width, img.height);
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        tree_add(g_app.hTree, hRoot, "Transparent: 0x8000", -1);
                                        TreeView_Expand(g_app.hTree, hRoot, TVE_EXPAND);
                                    }
                                    char msg[512];
                                    _snprintf(msg, 511, "IMD Image: %dx%d, 16bpp BGR555 - %s",
                                        img.width, img.height, path);
                                    g_app.set_status(msg);
                                    char title[512];
                                    _snprintf(title, 511, TITLE_PREFIX "%s", path);
                                    SetWindowTextA(g_app.hMain, title);
                                    InvalidateRect(g_image.hwnd, 0, FALSE);
                                } else {
                                    g_app.set_status("ERROR: Failed to parse IMD image");
                                }
                                free(buf);
                            } else { fclose(f); }
                        }
                    } else if (n >= 8 && hdr[0] == 0x10 && (rd_u32(hdr + 4) & 0x7) <= 3) {
                        /* TIM image (some Dino Crisis .imd files are actually TIM format) */
                        f = fopen(path, "rb");
                        if (f) {
                            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                            u8* buf = (u8*)malloc(sz);
                            if (buf) {
                                fread(buf, 1, sz, f); fclose(f);
                                TimImage img;
                                if (parse_tim_image(buf, (size_t)sz, img) && img.rgba) {
                                    g_app.close_archive();
                                    u8* rgba = (u8*)malloc((size_t)img.width * img.height * 4);
                                    if (rgba) {
                                        for (int pi = 0; pi < img.width * img.height; pi++) {
                                            rgba[pi*4+0] = img.rgba[pi].r;
                                            rgba[pi*4+1] = img.rgba[pi].g;
                                            rgba[pi*4+2] = img.rgba[pi].b;
                                            rgba[pi*4+3] = img.rgba[pi].a;
                                        }
                                        g_image.set_bitmap(rgba, img.width, img.height);
                                        free(rgba);
                                    }
                                    g_app.switch_panel(PANEL_IMAGE);
                                    if (g_app.hTree) {
                                        TreeView_DeleteAllItems(g_app.hTree);
                                        const char* fname = strrchr(path, '\\');
                                        if (!fname) fname = strrchr(path, '/');
                                        fname = fname ? fname + 1 : path;
                                        char node[256];
                                        _snprintf(node, 255, "%s", fname);
                                        HTREEITEM hRoot = tree_add(g_app.hTree, TVI_ROOT, node, -1);
                                        _snprintf(node, 255, "%dx%d, %dbpp%s", img.width, img.height, img.bpp,
                                            img.has_clut ? " (CLUT)" : "");
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        if (img.has_clut) {
                                            _snprintf(node, 255, "CLUT: %d colors @ (%d,%d)",
                                                img.clut_colors, img.clut_x, img.clut_y);
                                            tree_add(g_app.hTree, hRoot, node, -1);
                                        }
                                        _snprintf(node, 255, "VRAM: (%d,%d)", img.px_x, img.px_y);
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        TreeView_Expand(g_app.hTree, hRoot, TVE_EXPAND);
                                    }
                                    char msg[512];
                                    _snprintf(msg, 511, "TIM Image: %dx%d, %dbpp%s - %s",
                                        img.width, img.height, img.bpp,
                                        img.has_clut ? " (CLUT)" : "", path);
                                    g_app.set_status(msg);
                                    char title[512];
                                    _snprintf(title, 511, TITLE_PREFIX "%s", path);
                                    SetWindowTextA(g_app.hMain, title);
                                    InvalidateRect(g_image.hwnd, 0, FALSE);
                                } else {
                                    /* Not a valid TIM — try as DAT archive */
                                    g_app.open_archive(path);
                                }
                                free(buf);
                            } else { fclose(f); }
                        }
                    } else if (n >= 2 && hdr[0] == 0x53 && hdr[1] == 0x43) {
                        /* Dino Crisis save file ("SC" magic) */
                        f = fopen(path, "rb");
                        if (f) {
                            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                            u8* buf = (u8*)malloc(sz);
                            if (buf) {
                                fread(buf, 1, sz, f);
                                fclose(f);
                                g_app.close_archive();
                                /* Ensure SavePanel exists before loading data into it */
                                g_app.switch_panel(PANEL_SAVE);
                                if (g_save.load(buf, (u32)sz, path)) {
                                    char msg[256];
                                    _snprintf(msg, 255, "Save: %s R%d, %dh%02dm%02ds, %s",
                                        (g_save.save.stage >= 1 && g_save.save.stage <= 6) ?
                                            DC_STAGE_NAMES[g_save.save.stage] : "?",
                                        g_save.save.room,
                                        g_save.save.hours, g_save.save.minutes, g_save.save.seconds,
                                        DC_DIFF_NAMES[g_save.save.difficulty & 3]);
                                    g_app.set_status(msg);
                                    char title[512];
                                    _snprintf(title, 511, TITLE_PREFIX "Save Editor - %s", path);
                                    SetWindowTextA(g_app.hMain, title);
                                    /* Populate tree with save info */
                                    if (g_app.hTree) {
                                        TreeView_DeleteAllItems(g_app.hTree);
                                        /* Extract just filename from path */
                                        const char* fname = strrchr(path, '\\');
                                        if (!fname) fname = strrchr(path, '/');
                                        fname = fname ? fname + 1 : path;
                                        char node[128];
                                        _snprintf(node, 127, "Save: %s", fname);
                                        HTREEITEM hRoot = tree_add(g_app.hTree, TVI_ROOT, node, -1);
                                        const DcRoom* cur = NULL;
                                        for (int ri = 0; ri < DC_ROOM_COUNT; ri++)
                                            if (DC_ROOMS[ri].rm == g_save.save.room && DC_ROOMS[ri].stg == g_save.save.stage)
                                                { cur = &DC_ROOMS[ri]; break; }
                                        _snprintf(node, 127, "Room: %s R%d [%s]",
                                            cur ? cur->name : "Unknown",
                                            g_save.save.room,
                                            (g_save.save.stage >= 1 && g_save.save.stage <= 6) ?
                                                DC_STAGE_NAMES[g_save.save.stage] : "?");
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        _snprintf(node, 127, "Time: %d:%02d:%02d",
                                            g_save.save.hours, g_save.save.minutes, g_save.save.seconds);
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        _snprintf(node, 127, "Difficulty: %s%s",
                                            DC_DIFF_NAMES[g_save.save.difficulty & 3],
                                            g_save.save.arrange ? " (Arrange)" : "");
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        _snprintf(node, 127, "Continues: %d", g_save.save.continues);
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        _snprintf(node, 127, "Checksum: %s",
                                            (g_save.save.ck1_ok && g_save.save.ck2_ok) ? "OK" : "MISMATCH");
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        /* Expand root */
                                        TreeView_Expand(g_app.hTree, hRoot, TVE_EXPAND);
                                    }
                                } else {
                                    g_app.set_status("ERROR: Failed to parse save file");
                                }
                                free(buf);
                            } else { fclose(f); }
                        }
                    } else {
                        g_app.open_archive(path);
                    }
                }
            }
        } break;
        case IDM_FILE_SAVE_AS:
            if (g_app.archive) {
                char path[MAX_PATH];
                if (ui_save_file(hwnd, path, MAX_PATH,
                    "DAT Archives (*.dat)\0*.dat\0", "Save DAT Archive", "dat")) {
                    if (g_app.archive->is_item_bank && g_app.archive->raw) {
                        /* Item banks: write original raw bytes */
                        FILE* f = fopen(path, "wb");
                        if (f) {
                            fwrite(g_app.archive->raw, 1, g_app.archive->raw_size, f);
                            fclose(f);
                        }
                    } else if (g_app.archive->is_dc2) {
                        /* 32-byte entries; a .DBS is written package by package */
                        const DatArchive& ar = *g_app.archive;
                        if (!dat_write_file(path, ar.entries, ar.count, true))
                            MessageBoxA(hwnd, "The package could not be written.", "Save DAT Archive",
                                        MB_OK | MB_ICONERROR);
                    } else {
                        dat_write_file(path, g_app.archive->entries, g_app.archive->count);
                    }
                }
            } break;
        case IDM_FILE_CLOSE: g_app.close_archive(); break;
        case IDM_FILE_EXIT: PostQuitMessage(0); break;
        case IDM_ENTRY_EXPORT: {
            int sel = g_app.selected_entry_idx();
            int eidx = sel & 0xFFFF;
            if (eidx >= 0 && g_app.archive && eidx < g_app.archive->count) {
                char path[MAX_PATH];
                /* A DC2 music entry is a complete MP3 stream */
                bool mp3 = g_app.archive->entries[eidx].type == DAT_DC2_MP3;
                if (ui_save_file(hwnd, path, MAX_PATH,
                    mp3 ? "MP3 Audio (*.mp3)\0*.mp3\0All Files (*.*)\0*.*\0" : "All Files (*.*)\0*.*\0",
                    "Export Entry", mp3 ? "mp3" : "bin")) {
                    FILE* f = fopen(path, "wb");
                    if (f) {
                        fwrite(g_app.archive->entries[eidx].data, 1,
                               g_app.archive->entries[eidx].size, f);
                        fclose(f);
                    }
                }
            }
        } break;
        case IDM_ENTRY_IMPORT: {
            int sel = g_app.selected_entry_idx();
            int eidx = sel & 0xFFFF;
            if (eidx >= 0 && g_app.archive && eidx < g_app.archive->count) {
                char path[MAX_PATH];
                if (ui_open_file(hwnd, path, MAX_PATH,
                    "All Files (*.*)\0*.*\0", "Import / Replace Entry")) {
                    FILE* f = fopen(path, "rb");
                    if (f) {
                        fseek(f, 0, SEEK_END);
                        size_t fsz = ftell(f);
                        fseek(f, 0, SEEK_SET);
                        u8* fbuf = (u8*)malloc(fsz);
                        if (fbuf && fread(fbuf, 1, fsz, f) == fsz) {
                            DatEntry& e = g_app.archive->entries[eidx];
                            bool is_lzss = (e.type == DAT_LZSS0 || e.type == DAT_LZSS1 ||
                                            e.type == DAT_DC2_LZSS2);

                            /* For LZSS entries: ask if data is raw (needs compression) */
                            bool compress = false;
                            if (is_lzss) {
                                int ans = MessageBoxA(hwnd,
                                    "This entry uses LZSS compression.\n\n"
                                    "Is the imported file DECOMPRESSED data?\n"
                                    "(Yes = compress it, No = store as-is pre-compressed)",
                                    "LZSS Entry", MB_YESNOCANCEL | MB_ICONQUESTION);
                                if (ans == IDCANCEL) { free(fbuf); fclose(f); break; }
                                compress = (ans == IDYES);
                            }

                            if (dat_replace_entry(e, fbuf, fsz, compress)) {
                                g_app.archive->modified = true;
                                g_image.clear_face_usage();  /* meshes may have changed */
                                char msg[512];
                                _snprintf(msg, 511,
                                    "Entry %d replaced: %d bytes%s. Use Save As to write DAT.",
                                    eidx, (int)e.size,
                                    compress ? " (LZSS compressed)" : "");
                                g_app.set_status(msg);

                                /* Refresh the view */
                                on_entry_select(eidx);
                            } else {
                                MessageBoxA(hwnd, "Failed to replace entry.",
                                            "Error", MB_ICONERROR);
                            }
                        }
                        free(fbuf);
                        fclose(f);
                    }
                }
            }
        } break;
        case IDM_EXPORT_OBJ:
            export_selected_mesh(hwnd, false);
            break;
        case IDM_EXPORT_GLB: {
            bool is_char = g_emd_model && g_emd_model->valid && g_viewer3d.is_emd;
            bool is_room = !g_viewer3d.is_emd && g_viewer3d.has_mesh &&
                           g_viewer3d.im_tris && g_viewer3d.im_tri_count > 0;
            if (!is_char && !is_room) {
                MessageBoxA(hwnd, "Open a character model or a room in the 3D viewer first.", "Export GLB", MB_OK | MB_ICONINFORMATION);
                break;
            }
            char path[MAX_PATH];
            if (!ui_save_file(hwnd, path, MAX_PATH, "GLB Files (*.glb)\0*.glb\0",
                              is_char ? "Export GLB (30 fps)" : "Export Room GLB", "glb")) break;
            EmdExportAtlas a;
            a.rgba = g_viewer3d.has_texture ? g_viewer3d.uv_atlas_rgba : 0;
            a.width = g_viewer3d.uv_atlas_w; a.height = g_viewer3d.uv_atlas_h;
            a.slices = g_viewer3d.num_sub_pals > 0 ? g_viewer3d.num_sub_pals : 1;
            a.bpp = g_viewer3d.tex_bpp;
            a.vram_x = g_viewer3d.tex_vram_x; a.vram_y = g_viewer3d.tex_vram_y;
            a.clut_x = g_viewer3d.clut_base_x; a.clut_y = g_viewer3d.clut_base_y;
            a.slice_map = g_viewer3d.slice_map;
            if (is_room) {
                /* name the root node after the DAT file */
                char name[64] = "DC1_Room";
                if (g_app.archive) {
                    const char* fn = g_app.archive->filename.buf;
                    const char* b = strrchr(fn, '\\');
                    const char* b2 = strrchr(fn, '/');
                    if (b2 > b) b = b2;
                    b = b ? b + 1 : fn;
                    if (*b) {
                        _snprintf(name, sizeof(name) - 1, "%s", b);
                        name[sizeof(name) - 1] = 0;
                        char* dot = strrchr(name, '.');
                        if (dot) *dot = 0;
                    }
                }
                bool ok = export_room_glb(path, g_viewer3d.im_verts, g_viewer3d.im_vert_count,
                                          g_viewer3d.im_tris, g_viewer3d.im_tri_count,
                                          a.rgba ? &a : 0, name);
                MessageBoxA(hwnd, ok ? "Room GLB exported, one object per section. Import it in Blender with File > Import > glTF 2.0. Unplaced sections (H) are not included."
                                     : "Export failed. The room mesh or its texture atlas could not be exported.",
                            "Export GLB", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
                break;
            }
            bool ok = export_emd_glb(path, *g_emd_model, a.rgba ? &a : 0);
            MessageBoxA(hwnd, ok ? "GLB exported. Import it in Blender with File > Import > glTF 2.0. Clips use 30 fps to match the viewer; original timing and entity movement metadata are not known."
                                : "Export failed. The model, hierarchy, atlas, or a detected animation clip could not be exported.",
                        "Export GLB", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        } break;
        case IDM_EXPORT_SMD:
            export_selected_mesh(hwnd, true);
            break;
        case IDM_EXPORT_WAV: {
            int sel = g_app.selected_entry_idx();
            int eidx = sel & 0xFFFF;
            if (eidx >= 0 && g_app.archive && eidx < g_app.archive->count) {
                const DatEntry& e = g_app.archive->entries[eidx];
                if (e.type == DAT_SNDB) {
                    if (g_audio.num_samples > 0 && g_audio.cur_sample < g_audio.num_samples) {
                        char defname[64];
                        _snprintf(defname, 63, "sample_%02d", g_audio.cur_sample);
                        char path[MAX_PATH];
                        if (ui_save_file(hwnd, path, MAX_PATH,
                            "WAV Files (*.wav)\0*.wav\0", "Export WAV", "wav")) {
                            AudioSample& s = g_audio.samples[g_audio.cur_sample];
                            if (s.pcm && s.count > 0)
                                write_wav_file(path, s.pcm, s.count, s.rate);
                        }
                    }
                }
            }
        } break;
        case IDM_EXPORT_PNG: {
            /* Export current image panel bitmap as 32-bit BMP (BGRA with alpha).
               Reads pixels back from the DIB section and un-premultiplies alpha. */
            if (!g_image.hBmp || g_image.img_w <= 0 || g_image.img_h <= 0) {
                g_app.set_status("No image to export");
                break;
            }
            char path[MAX_PATH];
            if (ui_save_file(hwnd, path, MAX_PATH,
                "BMP Files (*.bmp)\0*.bmp\0All Files (*.*)\0*.*\0",
                "Export Image", "bmp")) {
                int w = g_image.img_w, h = g_image.img_h;
                /* Read DIB pixels */
                BITMAPINFO bmi; memset(&bmi, 0, sizeof(bmi));
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = w;
                bmi.bmiHeader.biHeight = -h; /* top-down */
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                u8* px = (u8*)malloc((size_t)w * h * 4);
                if (px) {
                    HDC dc = CreateCompatibleDC(NULL);
                    GetDIBits(dc, g_image.hBmp, 0, h, px, &bmi, DIB_RGB_COLORS);
                    DeleteDC(dc);
                    /* Un-premultiply alpha: stored as premul BGRA */
                    for (int i = 0; i < w * h; i++) {
                        u8 b = px[i*4], g = px[i*4+1], r = px[i*4+2], a = px[i*4+3];
                        if (a > 0 && a < 255) {
                            r = (u8)((r * 255 + a/2) / a);
                            g = (u8)((g * 255 + a/2) / a);
                            b = (u8)((b * 255 + a/2) / a);
                        }
                        px[i*4] = b; px[i*4+1] = g; px[i*4+2] = r; px[i*4+3] = a;
                    }
                    /* Write 32-bit BMP */
                    FILE* f = fopen(path, "wb");
                    if (f) {
                        u32 img_size = (u32)(w * h * 4);
                        u32 file_size = 54 + img_size;
                        u8 hdr[54] = {};
                        hdr[0] = 'B'; hdr[1] = 'M';
                        *(u32*)(hdr+2) = file_size;
                        *(u32*)(hdr+10) = 54;
                        *(u32*)(hdr+14) = 40;
                        *(s32*)(hdr+18) = w;
                        *(s32*)(hdr+22) = -h; /* top-down */
                        *(u16*)(hdr+26) = 1;
                        *(u16*)(hdr+28) = 32;
                        *(u32*)(hdr+34) = img_size;
                        fwrite(hdr, 54, 1, f);
                        fwrite(px, 1, img_size, f);
                        fclose(f);
                        char msg[128];
                        _snprintf(msg, 127, "Exported %dx%d BMP (%u bytes)", w, h, file_size);
                        g_app.set_status(msg);
                    }
                    free(px);
                }
            }
        } break;
        case IDM_EXPORT_DEC: {
            int sel = g_app.selected_entry_idx();
            int eidx = sel & 0xFFFF;
            if (eidx >= 0 && g_app.archive && eidx < g_app.archive->count) {
                const DatEntry& e = g_app.archive->entries[eidx];
                if (e.type == DAT_LZSS0 || e.type == DAT_LZSS1 || e.type == DAT_DC2_LZSS2) {
                    Buffer dec;
                    if (lzss_decompress(e.data, e.size, dec) && dec.size > 0) {
                        char path[MAX_PATH];
                        if (ui_save_file(hwnd, path, MAX_PATH,
                            "Binary Files (*.bin)\0*.bin\0All Files (*.*)\0*.*\0",
                            "Export Decompressed", "bin")) {
                            FILE* f = fopen(path, "wb");
                            if (f) {
                                fwrite(dec.data, 1, dec.size, f);
                                fclose(f);
                                u32 ba = ((u32)(e.y & 0x7FFF) << 16) | (u32)e.x | 0x80000000u;
                                if (g_app.archive->is_dc2) ba = e.load_base;
                                char msg[256];
                                _snprintf(msg, 255, "Exported %u bytes (from %u compressed)  base=0x%08X",
                                    (unsigned)dec.size, e.size, ba);
                                g_app.set_status(msg);
                            }
                        }
                    } else {
                        g_app.set_status("ERROR: LZSS decompression failed");
                    }
                } else {
                    /* Non-compressed entry: export raw */
                    char path[MAX_PATH];
                    if (ui_save_file(hwnd, path, MAX_PATH,
                        "Binary Files (*.bin)\0*.bin\0All Files (*.*)\0*.*\0",
                        "Export Raw Data", "bin")) {
                        FILE* f = fopen(path, "wb");
                        if (f) {
                            fwrite(e.data, 1, e.size, f);
                            fclose(f);
                            char msg[256];
                            _snprintf(msg, 255, "Exported %u bytes (raw)", e.size);
                            g_app.set_status(msg);
                        }
                    }
                }
            }
        } break;
        case IDM_EXPORT_ATLAS: {
            /* Export the composited texture atlas as a BMP file */
            if (g_viewer3d.uv_atlas_w > 0 && g_viewer3d.uv_atlas_h > 0 && g_viewer3d.uv_atlas_rgba) {
                char path[MAX_PATH];
                if (ui_save_file(hwnd, path, MAX_PATH,
                    "BMP Files (*.bmp)\0*.bmp\0", "Export Texture Atlas", "bmp")) {
                    int w = g_viewer3d.uv_atlas_w, h = g_viewer3d.uv_atlas_h;
                    /* Write 32-bit BMP */
                    int row_bytes = w * 4;
                    int img_size = row_bytes * h;
                    int file_size = 54 + img_size;
                    u8* bmp = (u8*)calloc(file_size, 1);
                    if (bmp) {
                        /* BMP header */
                        bmp[0] = 'B'; bmp[1] = 'M';
                        *(u32*)(bmp+2) = file_size;
                        *(u32*)(bmp+10) = 54;
                        *(u32*)(bmp+14) = 40;
                        *(s32*)(bmp+18) = w;
                        *(s32*)(bmp+22) = -h; /* top-down */
                        *(u16*)(bmp+26) = 1;
                        *(u16*)(bmp+28) = 32;
                        *(u32*)(bmp+34) = img_size;
                        /* Copy RGBA → BGRA */
                        u8* dst = bmp + 54;
                        const u8* src = g_viewer3d.uv_atlas_rgba;
                        for (int y2 = 0; y2 < h; y2++) {
                            for (int x2 = 0; x2 < w; x2++) {
                                int si = (y2 * w + x2) * 4;
                                int di = (y2 * w + x2) * 4;
                                dst[di+0] = src[si+2]; /* B */
                                dst[di+1] = src[si+1]; /* G */
                                dst[di+2] = src[si+0]; /* R */
                                dst[di+3] = src[si+3]; /* A */
                            }
                        }
                        FILE* f = fopen(path, "wb");
                        if (f) {
                            fwrite(bmp, 1, file_size, f);
                            fclose(f);
                            char msg[256];
                            _snprintf(msg, 255, "Atlas exported: %dx%d (%d KB)", w, h, file_size/1024);
                            g_app.set_status(msg);
                        }
                        free(bmp);
                    }
                }
            } else {
                g_app.set_status("No texture atlas loaded — open an RDT entry first");
            }
        } break;
        case IDM_EXPORT_COLL: {
            /* Export collision rects as flat OBJ mesh */
            if (g_viewer3d.gl_overlay_id) {
                char path[MAX_PATH];
                if (ui_save_file(hwnd, path, MAX_PATH,
                    "OBJ Files (*.obj)\0*.obj\0", "Export Collision Mesh", "obj")) {
                    FILE* f = fopen(path, "w");
                    if (f) {
                        fprintf(f, "# RaptorScope Collision Mesh Export\n");
                        fprintf(f, "# Walkable area from collision rects + floor zones\n\n");
                        const RdtSceneOverlay& ov = g_viewer3d.cached_overlay;
                        int vi = 1;  /* OBJ vertex index (1-based) */

                        /* Collision rects */
                        fprintf(f, "o CollisionRects\n");
                        for (int i = 0; i < ov.n_collisions; i++) {
                            const RdtCollisionRect& cr = ov.collisions[i];
                            float x0 = -(float)cr.x, z0 = (float)cr.z;
                            float x1 = -(float)(cr.x + cr.w), z1 = (float)(cr.z + cr.h);
                            float fy = ov.rect_y[i];
                            fprintf(f, "v %f %f %f\n", x0, fy, z0);
                            fprintf(f, "v %f %f %f\n", x1, fy, z0);
                            fprintf(f, "v %f %f %f\n", x1, fy, z1);
                            fprintf(f, "v %f %f %f\n", x0, fy, z1);
                            fprintf(f, "f %d %d %d %d\n", vi, vi+1, vi+2, vi+3);
                            vi += 4;
                        }

                        /* Floor zones */
                        fprintf(f, "\no FloorZones\n");
                        for (int i = 0; i < ov.n_zones; i++) {
                            const RdtFloorZone& fz = ov.zones[i];
                            for (int c = 0; c < 4; c++) {
                                float zx = -(float)fz.x[c], zz = (float)fz.z[c];
                                float zy = ov.zone_y[i] - (float)fz.y;
                                fprintf(f, "v %f %f %f\n", zx, zy, zz);
                            }
                            fprintf(f, "f %d %d %d %d\n", vi, vi+1, vi+2, vi+3);
                            vi += 4;
                        }

                        fclose(f);
                        char msg[256];
                        _snprintf(msg, 255, "Collision mesh exported: %d rects, %d zones, %d verts",
                                  ov.n_collisions, ov.n_zones, vi - 1);
                        g_app.set_status(msg);
                    }
                }
            } else {
                g_app.set_status("No overlay data — open an RDT entry with O=overlay first");
            }
        } break;
        case IDM_HELP_ABOUT:
            ShowAboutDialog(hwnd);
            break;
        case IDM_VIEW_FIT_SCREEN: {
            g_image.fit_to_screen = !g_image.fit_to_screen;
            CreateAppMenus();
            InvalidateRect(g_app.hMenuBar, NULL, TRUE);
            g_image.reset_view();
            InvalidateRect(g_image.hwnd, 0, FALSE);
        } break;
        case IDM_VIEW_SCD:
            if (g_scd.loaded) {
                g_app.switch_panel(PANEL_SCD);
                g_app.set_status("SCD Disassembly — Arrow keys to navigate, G=follow branch, H=toggle hex");
            }
            break;
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == IDC_ENTRY_LIST && nm->code == TVN_SELCHANGEDA) {
            NMTREEVIEWA* ntv = (NMTREEVIEWA*)lp;
            int idx = (int)ntv->itemNew.lParam;
            if (idx >= 0) {
                if (g_app.archive) {
                    on_entry_select(idx);
                } else if (g_app.raw_hex_buf && idx < (int)g_app.raw_hex_size) {
                    /* Raw hex mode: scroll to offset stored in lParam */
                    int row = idx / g_hex.bytes_per_row;
                    g_hex.scroll_pos = row;
                    SCROLLINFO si; memset(&si, 0, sizeof(si));
                    si.cbSize = sizeof(si); si.fMask = SIF_POS;
                    si.nPos = row;
                    SetScrollInfo(g_hex.hwnd, SB_VERT, &si, TRUE);
                    InvalidateRect(g_hex.hwnd, 0, TRUE);
                    /* Show offset in status bar */
                    char msg[128];
                    _snprintf(msg, 127, "Offset: 0x%04X (%d)", idx, idx);
                    g_app.set_status(msg);
                }
            }
        }
    } return 0;

    /* Dark color messages for ListView / controls */
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (T.isDark) {
            SetBkColor((HDC)wp, T.listBg);
            SetTextColor((HDC)wp, T.listText);
            return (LRESULT)g_hBrBg;
        }
        break;

    case WM_SETTINGCHANGE:
        ApplyTheme(DetectSystemDarkMode());
        return 0;

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wp;
        char path[MAX_PATH];
        if (DragQueryFileA(hDrop, 0, path, MAX_PATH)) {
            /* Check file extension */
            const char* ext = strrchr(path, '.');
            if (ext) {
                /* Probe header to detect type, then dispatch */
                FILE* f = fopen(path, "rb");
                if (f) {
                    u8 hdr[32]; size_t n = fread(hdr, 1, 32, f);
                    fseek(f, 0, SEEK_END); long file_sz = ftell(f);
                    fclose(f);
                    if (n >= 4 && is_mpeg_ps(hdr, n)) {
                        /* Video — load via same path as menu open */
                        f = fopen(path, "rb");
                        if (f) {
                            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                            u8* buf = (u8*)malloc(sz);
                            if (buf) {
                                fread(buf, 1, sz, f); fclose(f);
                                g_app.close_archive();
                                if (g_video.load(buf, sz)) {
                                    g_app.switch_panel(PANEL_VIDEO);
                                    char msg[512];
                                    _snprintf(msg, 511, "Video: %dx%d - %s",
                                        g_video.video.width, g_video.video.height, path);
                                    g_app.set_status(msg);
                                    char title[512];
                                    _snprintf(title, 511, TITLE_PREFIX "%s", path);
                                    SetWindowTextA(g_app.hMain, title);
                                }
                                free(buf);
                            } else fclose(f);
                        }
                    } else if (n >= 0x14 && is_imd_image(hdr, (size_t)file_sz)) {
                        /* Standalone IMD image */
                        f = fopen(path, "rb");
                        if (f) {
                            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                            u8* buf = (u8*)malloc(sz);
                            if (buf) {
                                fread(buf, 1, sz, f); fclose(f);
                                ImdImage img;
                                if (parse_imd_image(buf, (size_t)sz, img) && img.rgba) {
                                    g_app.close_archive();
                                    u8* rgba = (u8*)malloc((size_t)img.width * img.height * 4);
                                    if (rgba) {
                                        for (int pi = 0; pi < img.width * img.height; pi++) {
                                            rgba[pi*4+0] = img.rgba[pi].r;
                                            rgba[pi*4+1] = img.rgba[pi].g;
                                            rgba[pi*4+2] = img.rgba[pi].b;
                                            rgba[pi*4+3] = img.rgba[pi].a;
                                        }
                                        g_image.set_bitmap(rgba, img.width, img.height);
                                        free(rgba);
                                    }
                                    g_app.switch_panel(PANEL_IMAGE);
                                    if (g_app.hTree) {
                                        TreeView_DeleteAllItems(g_app.hTree);
                                        const char* fname = strrchr(path, '\\');
                                        if (!fname) fname = strrchr(path, '/');
                                        fname = fname ? fname + 1 : path;
                                        char node[256];
                                        _snprintf(node, 255, "%s", fname);
                                        HTREEITEM hRoot = tree_add(g_app.hTree, TVI_ROOT, node, -1);
                                        _snprintf(node, 255, "%dx%d, 16bpp BGR555", img.width, img.height);
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        tree_add(g_app.hTree, hRoot, "Transparent: 0x8000", -1);
                                        TreeView_Expand(g_app.hTree, hRoot, TVE_EXPAND);
                                    }
                                    char msg[512];
                                    _snprintf(msg, 511, "IMD Image: %dx%d, 16bpp BGR555 - %s",
                                        img.width, img.height, path);
                                    g_app.set_status(msg);
                                    char title[512];
                                    _snprintf(title, 511, TITLE_PREFIX "%s", path);
                                    SetWindowTextA(g_app.hMain, title);
                                    InvalidateRect(g_image.hwnd, 0, FALSE);
                                } else {
                                    g_app.set_status("ERROR: Failed to parse IMD image");
                                }
                                free(buf);
                            } else fclose(f);
                        }
                    } else if (n >= 8 && hdr[0] == 0x10 && (rd_u32(hdr + 4) & 0x7) <= 3) {
                        /* TIM image (some .imd files are TIM format) */
                        f = fopen(path, "rb");
                        if (f) {
                            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                            u8* buf = (u8*)malloc(sz);
                            if (buf) {
                                fread(buf, 1, sz, f); fclose(f);
                                TimImage img;
                                if (parse_tim_image(buf, (size_t)sz, img) && img.rgba) {
                                    g_app.close_archive();
                                    u8* rgba = (u8*)malloc((size_t)img.width * img.height * 4);
                                    if (rgba) {
                                        for (int pi = 0; pi < img.width * img.height; pi++) {
                                            rgba[pi*4+0] = img.rgba[pi].r;
                                            rgba[pi*4+1] = img.rgba[pi].g;
                                            rgba[pi*4+2] = img.rgba[pi].b;
                                            rgba[pi*4+3] = img.rgba[pi].a;
                                        }
                                        g_image.set_bitmap(rgba, img.width, img.height);
                                        free(rgba);
                                    }
                                    g_app.switch_panel(PANEL_IMAGE);
                                    if (g_app.hTree) {
                                        TreeView_DeleteAllItems(g_app.hTree);
                                        const char* fname = strrchr(path, '\\');
                                        if (!fname) fname = strrchr(path, '/');
                                        fname = fname ? fname + 1 : path;
                                        char node[256];
                                        _snprintf(node, 255, "%s", fname);
                                        HTREEITEM hRoot = tree_add(g_app.hTree, TVI_ROOT, node, -1);
                                        _snprintf(node, 255, "%dx%d, %dbpp%s", img.width, img.height, img.bpp,
                                            img.has_clut ? " (CLUT)" : "");
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        if (img.has_clut) {
                                            _snprintf(node, 255, "CLUT: %d colors @ (%d,%d)",
                                                img.clut_colors, img.clut_x, img.clut_y);
                                            tree_add(g_app.hTree, hRoot, node, -1);
                                        }
                                        _snprintf(node, 255, "VRAM: (%d,%d)", img.px_x, img.px_y);
                                        tree_add(g_app.hTree, hRoot, node, -1);
                                        TreeView_Expand(g_app.hTree, hRoot, TVE_EXPAND);
                                    }
                                    char msg[512];
                                    _snprintf(msg, 511, "TIM Image: %dx%d, %dbpp%s - %s",
                                        img.width, img.height, img.bpp,
                                        img.has_clut ? " (CLUT)" : "", path);
                                    g_app.set_status(msg);
                                    char title[512];
                                    _snprintf(title, 511, TITLE_PREFIX "%s", path);
                                    SetWindowTextA(g_app.hMain, title);
                                    InvalidateRect(g_image.hwnd, 0, FALSE);
                                } else {
                                    /* Not a valid TIM — try as DAT archive */
                                    g_app.open_archive(path);
                                }
                                free(buf);
                            } else fclose(f);
                        }
                    } else if (n >= 2 && hdr[0] == 0x53 && hdr[1] == 0x43) {
                        /* Dino Crisis save file ("SC" magic) */
                        f = fopen(path, "rb");
                        if (f) {
                            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                            u8* buf = (u8*)malloc(sz);
                            if (buf) {
                                fread(buf, 1, sz, f); fclose(f);
                                g_app.close_archive();
                                g_app.switch_panel(PANEL_SAVE);
                                if (g_save.load(buf, (u32)sz, path)) {
                                    char msg[256];
                                    _snprintf(msg, 255, "Save: %s R%d, %dh%02dm%02ds, %s",
                                        (g_save.save.stage >= 1 && g_save.save.stage <= 6) ?
                                            DC_STAGE_NAMES[g_save.save.stage] : "?",
                                        g_save.save.room,
                                        g_save.save.hours, g_save.save.minutes, g_save.save.seconds,
                                        DC_DIFF_NAMES[g_save.save.difficulty & 3]);
                                    g_app.set_status(msg);
                                    char title[512];
                                    _snprintf(title, 511, TITLE_PREFIX "Save Editor - %s", path);
                                    SetWindowTextA(g_app.hMain, title);
                                } else {
                                    g_app.set_status("ERROR: Failed to parse save file");
                                }
                                free(buf);
                            } else fclose(f);
                        }
                    } else {
                        /* Try as DAT archive (or save file via open_archive) */
                        g_app.open_archive(path);
                    }
                }
            }
        }
        DragFinish(hDrop);
    } return 0;
    case WM_DESTROY:
        g_viewer3d.shutdown_gl();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/*═══════════════════════════════════════════════════════════════════
 *  Panel area WndProc
 *═══════════════════════════════════════════════════════════════════*/
static LRESULT CALLBACK PanelAreaProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_hBrBg);
        return 1;
    }
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORSTATIC) {
        if (T.isDark) {
            SetBkColor((HDC)wp, T.listBg);
            SetTextColor((HDC)wp, T.listText);
            return (LRESULT)g_hBrBg;
        }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/*═══════════════════════════════════════════════════════════════════
 *  App::init
 *═══════════════════════════════════════════════════════════════════*/
#define MAIN_CLASS "DCArchiveToolMain"

bool App::init(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    /* Declare DPI awareness so Windows doesn't apply coordinate
       virtualization — fixes mouse offset in popup windows */
    {
        typedef BOOL (WINAPI *pfnSetProcessDPIAware)(void);
        HMODULE hUser = GetModuleHandleA("user32.dll");
        if (hUser) {
            pfnSetProcessDPIAware fn = (pfnSetProcessDPIAware)
                GetProcAddress(hUser, "SetProcessDPIAware");
            if (fn) fn();
        }
    }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    LoadThemeApis();

    /* Auto-detect and apply theme BEFORE window creation (like WBC) */
    BOOL useDark = DetectSystemDarkMode();
    ApplyTheme(useDark);

    /* Fonts */
    hFontUI = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    if (!hFontUI)
        hFontUI = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Tahoma");
    hFontMono = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    if (!hFontMono)
        hFontMono = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");

    /* Register window classes */
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(0, IDC_ARROW);

    /* Main window */
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    wc.hbrBackground = NULL;   /* We paint background ourselves */
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassExA(&wc);

    /* Custom menu bar */
    wc.lpfnWndProc = MenuBarProc;
    wc.lpszClassName = "DCMenuBar";
    wc.hIcon = 0;
    wc.hbrBackground = NULL;
    RegisterClassExA(&wc);

    /* Panel area container */
    wc.lpfnWndProc = PanelAreaProc;
    wc.lpszClassName = "DCPanelArea";
    RegisterClassExA(&wc);

    register_panel_classes(hInstance);

    /* Build menus */
    CreateAppMenus();

    /* Make popups owner-drawn if dark */
    /* Create main window */
    hMain = CreateWindowExA(WS_EX_APPWINDOW, MAIN_CLASS,
        "Dino Crisis Archive Tool v2.2",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 780,
        0, 0, hInstance, 0);
    if (!hMain) return false;
    DragAcceptFiles(hMain, TRUE);

    /* Re-apply theme now that window exists (like WBC) */
    ApplyTheme(useDark);

    /* Menu bar */
    hMenuBar = CreateWindowExA(0, "DCMenuBar", "",
        WS_CHILD | WS_VISIBLE, 0, 0, 1200, MENUBAR_H,
        hMain, (HMENU)IDC_MENUBAR, hInstance, 0);

    /* Status bar (STATIC control  -  painted manually like WBC) */
    hStatus = CreateWindowExA(0, "STATIC", " Ready",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0, hMain, (HMENU)IDC_STATUS_BAR, hInstance, 0);

    /* Entry tree */
    hTree = CreateWindowExA(WS_EX_CLIENTEDGE, WC_TREEVIEWA, "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT |
        TVS_SHOWSELALWAYS | TVS_FULLROWSELECT,
        0, MENUBAR_H, 400, 600, hMain, (HMENU)IDC_ENTRY_LIST, hInstance, 0);
    SendMessage(hTree, WM_SETFONT, (WPARAM)hFontUI, TRUE);

    /* Apply dark theme to TreeView */
    if (T.isDark) {
        TreeView_SetBkColor(hTree, T.listBg);
        TreeView_SetTextColor(hTree, T.listText);
        if (pSetWndTheme)
            pSetWndTheme(hTree, L"DarkMode_Explorer", NULL);
    }

    /* Panel area */
    hPanelArea = CreateWindowExA(0, "DCPanelArea", "",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        400, MENUBAR_H, 800, 600, hMain,
        (HMENU)IDC_PANEL_AREA, hInstance, 0);

    /* Create panels */
    g_panels[PANEL_HEX] = CreateWindowExA(0, "DCHexPanel", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 800, 600,
        hPanelArea, 0, hInstance, 0);
    g_hex.hwnd = g_panels[PANEL_HEX];
    g_hex.font = hFontMono;
    g_hex.start_idle();  /* begin idle animation immediately */
    if (T.isDark && pSetWndTheme)
        pSetWndTheme(g_panels[PANEL_HEX], L"DarkMode_Explorer", NULL);

    g_panels[PANEL_IMAGE] = CreateWindowExA(0, "DCImagePanel", "",
        WS_CHILD, 0, 0, 800, 600, hPanelArea, 0, hInstance, 0);
    g_image.hwnd = g_panels[PANEL_IMAGE];

    g_panels[PANEL_3D] = CreateWindowExA(0, "DC3DPanel", "",
        WS_CHILD | WS_CLIPCHILDREN, 0, 0, 800, 600, hPanelArea, 0, hInstance, 0);
    g_viewer3d.hwnd = g_panels[PANEL_3D];

    g_panels[PANEL_AUDIO] = CreateWindowExA(0, "DCAudioPanel", "",
        WS_CHILD, 0, 0, 800, 600, hPanelArea, 0, hInstance, 0);
    g_audio.hwnd = g_panels[PANEL_AUDIO];

    g_panels[PANEL_PALETTE] = CreateWindowExA(0, "DCPalettePanel", "",
        WS_CHILD | WS_VSCROLL, 0, 0, 800, 600, hPanelArea, 0, hInstance, 0);
    g_palette.hwnd = g_panels[PANEL_PALETTE];

    g_panels[PANEL_VIDEO] = CreateWindowExA(0, "DCVideoPanel", "",
        WS_CHILD | WS_CLIPCHILDREN, 0, 0, 800, 600, hPanelArea, 0, hInstance, 0);
    g_video.hwnd = g_panels[PANEL_VIDEO];

    g_panels[PANEL_SCD] = CreateWindowExA(0, "DCScdPanel", "",
        WS_CHILD | WS_CLIPCHILDREN, 0, 0, 800, 600, hPanelArea, 0, hInstance, 0);
    g_scd.hwnd = g_panels[PANEL_SCD];
    g_scd.font = hFontMono;

    /* SavePanel is created lazily on first switch_panel(PANEL_SAVE) —
       its WM_CREATE builds ~70 dark-themed controls which is slow */
    g_panels[PANEL_SAVE] = NULL;

    g_viewer3d.init_gl(g_panels[PANEL_3D]);
    switch_panel(PANEL_HEX);

    ShowWindow(hMain, nCmdShow);
    UpdateWindow(hMain);
    char status_msg[256];
    if (g_viewer3d.sw_renderer)
        _snprintf(status_msg, 255, " Ready - GL: %s [Software Mode - F11 to cycle]", g_viewer3d.gl_renderer_name);
    else if (g_viewer3d.in_vm)
        _snprintf(status_msg, 255, " Ready - GL: %s [VM GPU Mode - F11 to cycle]", g_viewer3d.gl_renderer_name);
    else
        _snprintf(status_msg, 255, " Ready - Open a .DAT archive (Ctrl+O)");
    set_status(status_msg);
    return true;
}

int App::run()
{
    HACCEL hAccel = LoadAcceleratorsA(hInst, MAKEINTRESOURCE(101));
    MSG msg;
    while (GetMessage(&msg, 0, 0, 0)) {
        if (hAccel && TranslateAccelerator(hMain, hAccel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

void App::shutdown()
{
    close_archive();
    if (hFontUI) DeleteObject(hFontUI);
    if (hFontMono) DeleteObject(hFontMono);
    if (g_hBrBg) DeleteObject(g_hBrBg);
    if (g_hBrPanel) DeleteObject(g_hBrPanel);
}
