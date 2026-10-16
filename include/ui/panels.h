/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  UI Panel Declarations
 *═══════════════════════════════════════════════════════════════════*/
#ifndef DC_UI_PANELS_H
#define DC_UI_PANELS_H

#include "ui/app.h"
#include "formats/dat.h"
#include "formats/mesh.h"
#include "formats/video.h"
#include "core/audio.h"
#include "formats/save_editor.h"
#include <string.h>

struct HexPanel {
    HWND   hwnd;
    const u8* data;
    size_t data_size;
    int    scroll_pos;
    int    bytes_per_row;
    HFONT  font;

    /* Idle animation state (procedural particle viz when no data loaded) */
    UINT_PTR idle_timer;
    int      idle_frame;
    u32*     idle_buf;       /* BGRA backbuffer */
    int      idle_w, idle_h;
    bool     click_armed;   /* true if LBUTTONDOWN happened in this panel */

    HexPanel() : hwnd(0), data(0), data_size(0),
                 scroll_pos(0), bytes_per_row(16), font(0),
                 idle_timer(0), idle_frame(0),
                 idle_buf(0), idle_w(0), idle_h(0),
                 click_armed(false) {}
    void set_data(const u8* d, size_t sz);
    void paint(HDC hdc, RECT& rc);
    void paint_idle(HDC hdc, RECT& rc);
    void start_idle();
    void stop_idle();
};

/* Dino Crisis 2: decode the background of the camera whose package starts
   at dbs_off in the open room's ST*.DBS into 320x240 RGBA (app.cpp).  False
   when there is none. */
bool dc2_camera_background(u32 dbs_off, u8* rgba);

struct ImagePanel {
    HWND    hwnd;
    HBITMAP hBmp;
    int     img_w, img_h;
    int     bpp, pal_row, sub_pal;
    int     max_pal_rows;          /* available palette rows for current texture */

    /* What was last rendered, for re-rendering on palette row change.
       cur_tex/cur_pal point at the copies in src_*: callers may pass an
       entry on their stack, or one whose data they free afterwards
       (LZSS0 textures, item bank CLUTs). */
    const DatEntry* cur_tex;
    const DatEntry* cur_pal;
    bool    cur_is_linear;         /* true = render_linear, false = render_entry */
    DatEntry src_tex, src_pal;
    Buffer   src_tex_data, src_pal_data;

    /* zoom / pan state */
    double  zoom;          /* current zoom factor (1.0 = 1 texel = 1 pixel) */
    double  pan_x, pan_y;  /* offset of image origin in client coords */
    bool    dragging;
    int     drag_x, drag_y;
    double  drag_pan_x, drag_pan_y;
    bool    fitted;         /* true = auto-fit mode (reset on first zoom) */
    int     fit_to_screen;  /* 0 = 100% centered on load, 1 = fit to panel */

    /* Room CLUT mode: decode each texel with the colour depth and CLUT of
       the faces that sample it (render_texture_by_faces), C toggles it. */
    bool        face_cluts;          /* user setting */
    bool        showing_face_cluts;  /* current image was rendered that way */
    TexFaceUse* face_uses;           /* textured faces of the open archive */
    int         n_face_uses;
    bool        face_uses_ready;     /* face_uses collected for this archive */

    /* Dino Crisis 2 background (M): 0 = picture, 1 = mask tiles tinted
       over it, 2 = mask tiles alone */
    int         dc2_mask_mode;

    ImagePanel() : hwnd(0), hBmp(0), img_w(0), img_h(0),
                   bpp(8), pal_row(0), sub_pal(0), max_pal_rows(1),
                   cur_tex(0), cur_pal(0), cur_is_linear(false),
                   zoom(1.0), pan_x(0), pan_y(0),
                   dragging(false), drag_x(0), drag_y(0),
                   drag_pan_x(0), drag_pan_y(0), fitted(true),
                   fit_to_screen(1),
                   face_cluts(true), showing_face_cluts(false),
                   face_uses(0), n_face_uses(0), face_uses_ready(false), dc2_mask_mode(0) {}
    ~ImagePanel() { if (hBmp) DeleteObject(hBmp); free(face_uses); }

    void clear_face_usage();               /* call when the archive changes */
    void keep_source(const DatEntry& tex, const DatEntry* pal);  /* copy into src_* */
    void render_entry(const DatEntry& tex, const DatEntry* pal, bool do_deswizzle = true);
    void render_linear(const DatEntry& tex, const DatEntry* pal);
    void rerender();                           /* re-render with current pal_row */
    void set_bitmap(const u8* rgba, int w, int h);
    void paint(HDC hdc, RECT& rc);
    void reset_view();                    /* reset to default (fit or 100%) */
    void zoom_at(int cx, int cy, double factor); /* zoom toward cursor */
};

struct PalettePanel {
    HWND   hwnd;
    RGBA8  colors[4096];
    int    num_rows, selected_row;
    int    scroll_y;          /* vertical scroll offset in pixels */
    int    content_h;         /* total content height for scrollbar */
    bool   dragging;
    int    drag_y, drag_scroll;

    PalettePanel() : hwnd(0), num_rows(0), selected_row(0),
                     scroll_y(0), content_h(0),
                     dragging(false), drag_y(0), drag_scroll(0) {}
    void set_palette(const u8* data, size_t sz);
    void paint(HDC hdc, RECT& rc);
    void update_scroll(int panel_h);
};

struct AudioSample {
    s16*    pcm;
    int     count;      /* number of PCM samples */
    int     start_off;  /* byte offset in SNDB body */
    int     end_off;
    int     rate;       /* playback rate (Hz) from the bank's Gian tones */
    bool    rate_known; /* false: rate is a guess (no tone, or an instrument) */
};

#define MAX_AUDIO_SAMPLES 64

enum AudioMode { AMODE_NONE, AMODE_SNDB, AMODE_SNDH, AMODE_SNDE, AMODE_WAV };

struct AudioPanel {
    HWND     hwnd;
    s16*     pcm_data;       /* currently playing sample's PCM (mono or stereo interleaved) */
    int      pcm_count;      /* mono: sample count. stereo: frame count */
    int      sample_rate;
    int      channels;       /* 1=mono, 2=stereo */
    bool     playing;
    HWAVEOUT hWaveOut;

    AudioSample samples[MAX_AUDIO_SAMPLES];
    int      num_samples;
    int      cur_sample;     /* currently selected sample index */
    int      scroll_off;     /* vertical scroll offset for sample list */

    AudioMode  mode;

    /* SNDH state */
    GianHeader gian;

    /* SNDE state */
    SeqHeader  seq_hdr;
    SeqNote*   seq_notes;
    int        seq_note_count;
    f32        seq_duration;
    int        seq_ch_mask;
    s16*       seq_pcm;      /* rendered stereo PCM */
    int        seq_frames;   /* number of stereo frames */
    bool       seq_rendered;
    const DatEntry* active_snde; /* currently selected SNDE entry */
    int        zoom;         /* percent: 50-250, default 130 */
    UINT_PTR   audio_timer;  /* playback position polling timer */
    f32        play_time;    /* current playback position in seconds */

    AudioPanel() : hwnd(0), pcm_data(0), pcm_count(0),
                   sample_rate(22050), channels(1), playing(false), hWaveOut(0),
                   num_samples(0), cur_sample(0), scroll_off(0),
                   mode(AMODE_NONE), seq_notes(0), seq_note_count(0),
                   seq_duration(0), seq_ch_mask(0),
                   seq_pcm(0), seq_frames(0), seq_rendered(false), active_snde(0), zoom(130),
                   audio_timer(0), play_time(0.0f), wav_total_samples(0)
                   { wav_name[0] = 0; }
    ~AudioPanel() { stop(); free_samples(); free(seq_notes); free(seq_pcm); }

    void decode_sndb(const DatEntry& entry);
    void decode_sndh(const DatEntry& entry);
    void decode_snde(const DatEntry& entry);
    void render_seq();       /* synthesize SEQ → stereo PCM using archive's SNDH+SNDB */
    bool load_wav(const u8* data, size_t size, const char* filename);
    void select_sample(int idx);
    void play();
    void stop();
    void free_samples();
    void paint(HDC hdc, RECT& rc);
    void paint_sndb(HDC buf, int cw, int ch);
    void paint_sndh(HDC buf, int cw, int ch);
    void paint_snde(HDC buf, int cw, int ch);
    void paint_wav(HDC buf, int cw, int ch);

    char wav_name[MAX_PATH]; /* display name for standalone WAV */
    int  wav_total_samples;  /* total sample count for duration calc */
};

/* Which overlay elements are drawn: the checkbox tree beside the 3D view
   while the overlay is on.  A type's checkbox hides all of that type and
   each instance has its own.  Cameras are indexed by camera number
   (cam_id, as K shows it), every other type by its RdtSceneOverlay index. */
struct OverlayVis {
    enum { CAMERAS, SCRIPT_CAMS, CAM_CUTS, TRIGGERS, FLOOR_ZONES, RECTS,
           ENEMIES, CHARS, OBJECTS, SPAWNS, LIGHTS, FOG, N_TYPES };
    enum { MAX_ITEMS = 512 };
    bool on[N_TYPES];               /* kept from room to room */
    u8   hidden[N_TYPES][MAX_ITEMS];  /* cleared for each room */
    int  sel_type, sel_item;        /* tree selection, drawn highlighted; -1 = none */
    OverlayVis() : sel_type(-1), sel_item(-1) {
        for (int t = 0; t < N_TYPES; t++) on[t] = true;
        memset(hidden, 0, sizeof(hidden));
    }
    bool shown(int t, int i) const { return on[t] && (i < 0 || i >= MAX_ITEMS || !hidden[t][i]); }
};

struct ViewerPanel3D {
    HWND    hwnd;
    HDC     hDC;
    HGLRC   hRC;
    f32     cam_yaw, cam_pitch, cam_dist;
    f32     cam_x, cam_y, cam_z;
    f32     scene_reach;  /* farthest vertex from the origin (far clip) */
    f32     cam_upx, cam_upy, cam_upz;  /* camera up vector for orbit */
    bool    dragging, panning;
    int     last_mx, last_my;
    POINT   warp_anchor;       /* screen-space anchor for infinite mouse drag */
    bool    has_mesh;
    bool    is_emd;   /* EMD needs CW front face due to Y,Z negate */
    int     gl_list_id, gl_wire_id, gl_bone_id, gl_blend_id, gl_sub_id;
    int     gl_overlay_id;    /* RDT scene overlay (zones, collisions, doors, cameras) */
    int     gl_alt_id;        /* sections no script places (H) */
    int     gl_2side_id;      /* see-through room faces, drawn without culling */
    int     gl_normals_id;    /* debug: vertex normal lines */
    int     tri_count, vert_count, bone_count;

    /* Display mode toggles */
    bool    show_wireframe;
    bool    show_lighting;
    bool    show_bones;
    bool    show_bone_labels;
    bool    show_textured;
    bool    show_vcolors;   /* vertex / face colors */
    bool    show_overlay;   /* RDT scene overlay */
    OverlayVis ov_vis;      /* overlay elements shown */
    HWND    ov_tree;        /* checkbox tree of overlay elements, right of the view */
    int     ov_tree_w;      /* width the tree takes from the view, 0 = hidden */
    int     ov_tree_dark;   /* theme the tree's colours were set for, -1 = none */
    bool    show_alt_geo;   /* show unplaced sections (H) */
    /* Game cameras (K).  Each entry is a camera shot (a ptr[3] descriptor
       that places the camera) or, for cameras with no such shot, just the
       model slots the camera's setup thread hides. */
    struct CamEntry { int shot; u8 cam_id; u64 hide; };  /* shot = cached_overlay.cam_threads index, -1 = none */
    CamEntry cam_list[64];
    int     n_cams;
    int     cam_sel;        /* index into cam_list, -1 = every slot, free camera */
    u64     view_hide;      /* model slots hidden in the selected camera */
    bool    shot_locked;    /* view follows the selected shot (mouse frees it) */
    int     shot_kind;      /* RDT_SHOT_* of the selected shot */
    int     shot_cur;       /* cached_overlay.cam_threads index of the shot in use, -1 = none */
    f32     shot_player[3]; /* player position the shot tracks, raw game coordinates */
    f32     shot_eye[3], shot_tgt[3];  /* the shot's eye and target, raw game coordinates */
    f32     shot_zoom;      /* scroll on a shot: field of view as a multiple of the game's */
    bool    shot_moving;    /* WASD / arrow timer running */
    DWORD   shot_tick_ms;
    /* Dino Crisis 2: the selected camera's background, drawn behind the
       scene while the view is on the shot, and the height the flat overlay
       shapes are drawn at.  Ground heights are not decoded: the shapes go
       on the camera zone's floor (1000 units a floor), and PgUp/PgDn move
       them by dc2_floor_adj. */
    unsigned int gl_bg_id;
    bool    has_bg;
    f32     dc2_floor_adj;  /* viewer units, up is positive */
    bool    show_normals;   /* debug: draw vertex normal lines */
    bool    show_cull;      /* backface culling toggle (C key) */
    bool    show_grid;      /* ground grid toggle (G key) */
    bool    show_hud;       /* info/hotkey overlay toggle (F2 key) */

    /* ─── Walk-through mode (first-person) ─── */
    bool    walk_mode;
    bool    walk_noclip;              /* P key: ignore collision */
    f32     walk_x, walk_y, walk_z;   /* position (viewer coords) */
    f32     walk_yaw, walk_pitch;     /* look direction (degrees) */
    int     walk_rect;                /* index of current collision rect (-1 = unbound) */
    bool    walk_keys[6];             /* 0=W 1=A 2=S 3=D 4=shift 5=space */
    f32     walk_vy;                  /* vertical velocity (jump) */
    f32     walk_vx, walk_vz;         /* horizontal velocity (smoothed) */
    int     walk_mouse_dx, walk_mouse_dy; /* accumulated mouse deltas for walk look */
    f32     walk_floor_y;             /* current floor height */

    /* Fireball projectiles */
    struct Fireball { f32 x, y, z, vx, vy, vz; f32 life; bool active; };
    static const int MAX_FIREBALLS = 16;
    Fireball fireballs[MAX_FIREBALLS];

    /* Stored collision rects for walk-mode floor/wall collision */
    static const int MAX_WALK_COLS = 128;
    RdtCollisionRect walk_cols[MAX_WALK_COLS];
    int     n_walk_cols;
    /* Cached spawn points for walk mode start position */
    RdtCharPlace   walk_chars[16];  int n_walk_chars;
    RdtObjectSpawn walk_spawns[64]; int n_walk_spawns;

    /* Pre-computed walkable grid for smooth collision (built from rects) */
    u8*     walk_grid;         /* 1=walkable, 0=wall */
    int     walk_grid_w, walk_grid_h;  /* grid dimensions in cells */
    float   walk_grid_ox, walk_grid_oz; /* world origin of grid */
    static const int WALK_CELL = 40;   /* mm per grid cell */

    bool walk_grid_test(float wx, float wz) const;
    void walk_grid_build();

    /* GL bitmap font for flicker-free HUD */
    unsigned int gl_font_base;

    /* Texture atlas for textured mode */
    unsigned int gl_tex_id;
    int     tex_w, tex_h;
    u16     tex_vram_x;       /* texture VRAM X in halfwords (for tpage UV offset) */
    u16     tex_vram_y;       /* texture VRAM Y (for tpage TY offset) */
    int     tex_bpp;          /* texture BPP as rendered (4 or 8) */
    int     desired_bpp;      /* BPP to use for atlas rendering (set before upload) */
    int     num_pal_rows;     /* palette rows in atlas (1 = no CLUT atlas) */
    int     clut_base_y;      /* min CLUT Y from mesh faces, for row indexing */
    int     num_sub_pals;     /* total ALLOCATED atlas slices (sparse 2D) */
    int     clut_base_x;      /* 4bpp: min CLUT X from palette, for sub-pal indexing */
    int     max_sub_pal;      /* max sub-palette index used by mesh faces (set before upload) */
    /* 2D sparse atlas: slice_map[row * 65 + slot] = compact atlas slice.
       slot 0 = 8bpp rendering for this row.
       slot 1..64 = 4bpp sub-palette (rel_sub = slot - 1).
       This prevents 4bpp sub=0 (CLUT X == base_x) from colliding with 8bpp. */
    static const int SLOTS_PER_ROW = 65;
    static const int SLICE_MAP_SIZE = 16 * 65;
    int     slice_map[16 * 65];
    /* Unique CLUT values from mesh faces, for atlas builder.
       used_clut_is4bpp[i] = true if at least one 4bpp face uses this CLUT. */
    u16     used_cluts[512];
    bool    used_clut_is4bpp[512];
    /* An 8bpp face with / without MeshTri::solid uses this CLUT.  A row used
       by both kinds gets a second 8bpp slice, solid8_slice[row] (-1 = none),
       where texel 0x8000 stays opaque black; slot 0 is then the masked one. */
    bool    used_clut_solid8[512];
    bool    used_clut_mask8[512];
    int     solid8_slice[16];
    int     n_used_cluts;
    bool    has_texture;

    /* Stored skeleton for bone visualization */
    EmdSkeleton skeleton;
    bool    has_skeleton;

    /* Animation playback */
    EmdModel* anim_model;  /* non-owning pointer to the EmdModel for animation */
    int     anim_clip;
    int     anim_frame;
    bool    anim_playing;
    HWND    anim_track;    /* trackbar / slider */
    HWND    anim_label;    /* frame counter label */
    HWND    anim_clip_combo; /* clip selector */
    UINT_PTR anim_timer_id;

    /* Cached data for UV editor (owned copies) */
    MeshTri*  uv_tris;
    int       uv_tri_count;
    MeshVert* uv_verts;       /* cached vertex positions for selection overlay */
    int       uv_vert_count;
    u8*       uv_tex_rgba;    /* RGBA top-down (cropped to mesh tpage region) */
    int       uv_tex_w, uv_tex_h;
    u16       uv_tex_vram_x;  /* VRAM X origin for UV editor (may differ from tex_vram_x if cropped) */
    u16       uv_tex_vram_y;  /* VRAM Y origin for UV editor */
    int       uv_cache_clut_row; /* which CLUT atlas row to cache for UV editor */
    int       uv_crop_px_x;  /* pixel X offset in atlas to crop for UV editor */
    int       uv_crop_px_w;  /* pixel width to crop (0 = full width) */
    int       uv_crop_py_y;  /* pixel Y offset in atlas slice to crop for UV editor */
    int       uv_crop_py_h;  /* pixel height to crop (0 = full slice height) */

    /* Full atlas for UV editor palette cycling */
    u8*       uv_atlas_rgba;  /* full RGBA atlas (all CLUT rows, full width) */
    int       uv_atlas_w, uv_atlas_h; /* dimensions of full atlas */

    /* Tri selection (shared with UV editor for debug highlight) */
    bool*    sel_mask;        /* [uv_tri_count] true = selected */
    int      sel_count;       /* number of selected tris */
    int      gl_sel_id;       /* GL display list for selection overlay */

    bool     sw_renderer;     /* true if software GL (GDI Generic, llvmpipe, etc.) */
    bool     in_vm;           /* true if running inside a VM (affects mouse warp + GL quirks) */
    char     gl_renderer_name[128]; /* stored GL_RENDERER string for HUD */

    /* Object picking (Ctrl+Click) — groups by material (tpage+clut) */
    int      pick_tri;         /* index of picked triangle (-1 = none) */
    u32      pick_section;     /* section_off of picked tri */
    u16      pick_tpage;       /* material tpage of picked group */
    u16      pick_clut;        /* material clut of picked group */
    int      pick_group_count; /* how many tris share this material */
    int      gl_pick_id;       /* GL display list for highlight overlay */
    char     pick_info[512];   /* info text shown in HUD */

    /* Cached mesh for immediate-mode drawing (VM fallback, no display lists) */
    MeshVert* im_verts;
    int       im_vert_count;
    MeshTri*  im_tris;
    int       im_tri_count;
    bool      im_flip_normals;

    /* Room scene lights from SCD opcode 0x3A */
    RdtSceneLight room_lights[8];
    int           n_room_lights;
    bool          use_room_lights;  /* toggle: room lights vs default 3-point */
    u8            room_ambient[3];  /* RGB from ptr[0]+8 */

    ViewerPanel3D() : hwnd(0), hDC(0), hRC(0),
                      cam_yaw(0), cam_pitch(20), cam_dist(2000),
                      cam_x(0), cam_y(500), cam_z(0), scene_reach(0),
                      cam_upx(0), cam_upy(1), cam_upz(0),
                      dragging(false), panning(false),
                      last_mx(0), last_my(0),
                      has_mesh(false), is_emd(false),
                      gl_list_id(0), gl_wire_id(0), gl_bone_id(0), gl_blend_id(0), gl_sub_id(0), gl_overlay_id(0), gl_alt_id(0), gl_2side_id(0), gl_normals_id(0),
                      tri_count(0), vert_count(0), bone_count(0),
                      show_wireframe(false), show_lighting(true),
                      show_bones(false), show_bone_labels(false), show_textured(true),
                      show_vcolors(false), show_overlay(false), ov_tree(0), ov_tree_w(0), ov_tree_dark(-1), show_alt_geo(false), n_cams(0), cam_sel(-1), view_hide(0), shot_locked(false), shot_kind(0), shot_cur(-1), shot_player(), shot_eye(), shot_tgt(), shot_zoom(1), shot_moving(false), shot_tick_ms(0), gl_bg_id(0), has_bg(false), dc2_floor_adj(0), show_normals(false),
                      show_cull(true),
                      show_grid(true), show_hud(true),
                      walk_mode(false), walk_noclip(false), walk_x(0), walk_y(-1500), walk_z(0),
                      walk_yaw(0), walk_pitch(0), walk_rect(-1), walk_keys{},
                      walk_vy(0), walk_vx(0), walk_vz(0), walk_mouse_dx(0), walk_mouse_dy(0), walk_floor_y(0), fireballs{},
                      n_walk_cols(0), n_walk_chars(0), n_walk_spawns(0),
                      walk_grid(0), walk_grid_w(0), walk_grid_h(0),
                      walk_grid_ox(0), walk_grid_oz(0),
                      gl_font_base(0), gl_tex_id(0), tex_w(0), tex_h(0), tex_vram_x(0), tex_vram_y(0), tex_bpp(8),
                      desired_bpp(8),
                      num_pal_rows(1), clut_base_y(0),
                      num_sub_pals(1), clut_base_x(0), max_sub_pal(0),
                      has_texture(false), has_skeleton(false),
                      anim_model(0), anim_clip(0), anim_frame(0),
                      anim_playing(false), anim_track(0), anim_label(0),
                      anim_clip_combo(0), anim_timer_id(0),
                      uv_tris(0), uv_tri_count(0),
                      uv_verts(0), uv_vert_count(0),
                      uv_tex_rgba(0), uv_tex_w(0), uv_tex_h(0),
                      uv_tex_vram_x(0), uv_tex_vram_y(0), uv_cache_clut_row(0),
                      uv_crop_px_x(0), uv_crop_px_w(0),
                      uv_crop_py_y(0), uv_crop_py_h(0),
                      uv_atlas_rgba(0), uv_atlas_w(0), uv_atlas_h(0),
                      sel_mask(0), sel_count(0), gl_sel_id(0),
                      sw_renderer(false), in_vm(false),
                      im_verts(0), im_vert_count(0), im_tris(0), im_tri_count(0),
                      im_flip_normals(true),
                      n_room_lights(0), use_room_lights(false),
                      pick_tri(-1), pick_section(0), pick_tpage(0), pick_clut(0), pick_group_count(0), gl_pick_id(0)
                      { gl_renderer_name[0] = 0; room_ambient[0]=room_ambient[1]=room_ambient[2]=0; pick_info[0]=0; memset(slice_map, -1, sizeof(slice_map)); memset(solid8_slice, -1, sizeof(solid8_slice)); n_used_cluts=0; }
    ~ViewerPanel3D() { free(uv_tris); free(uv_verts); free(uv_tex_rgba); free(uv_atlas_rgba); free(sel_mask); free(im_verts); free(im_tris); }

    bool init_gl(HWND parent);
    void shutdown_gl();
    void render();
    void resize(int w, int h);
    void set_mesh(const Mesh& mesh);
    void set_overlay(const RdtSceneOverlay& overlay);
    void select_camera(int sel);   /* K: -1 = every slot, free camera */
    void set_view_hide(u64 hide);  /* rebuild the display lists without these slots */
    void update_shot(bool lock);   /* recompute the shot; lock = put the view on it */
    void move_shot_player(f32 fwd, f32 right);
    int  shot_zone_at(const f32 p[3]) const;  /* shot whose trigger zone holds p, -1 = none */
    void shot_tick();              /* WASD / arrows held: walk the player */
    f32  floor_below(f32 vx, f32 vz, f32 vy) const;  /* viewer coords */
    double view_fovy(double aspect) const;
    void draw_shot_overlay(int w, int vh);
    void dc2_camera_changed();     /* DC2: load the shot's background, place the flat shapes */
    void dc2_place_overlay();      /* DC2: overlay heights from the shot's floor and dc2_floor_adj */
    void set_background(const u8* rgba);  /* 320x240 RGBA, 0 = none */
    void build_mesh_lists(const Mesh& mesh);  /* solid/blend/wire lists, GL context current */
    /* Overlay element tree */
    void fill_ov_tree();           /* list the room's overlay elements */
    void layout_ov_tree();         /* show it beside the view while the overlay is on */
    void ov_tree_checked(HTREEITEM item);   /* a checkbox changed */
    void ov_tree_selected(LPARAM lp);       /* the selection changed */
    void rebuild_overlay();        /* overlay display list for ov_vis */
    int  view_w(int client_w) const { int v = client_w - ov_tree_w; return v < 1 ? 1 : v; }
    bool tri_in_view(const MeshTri& t) const {
        return !view_hide || t.alt || t.slot >= 64 || !((view_hide >> t.slot) & 1);
    }
    RdtSceneOverlay cached_overlay;  /* copy for export access */
    void set_emd(const EmdModel& emd);
    void set_emd_anim(EmdModel* emd);  /* set up animation controls */
    void anim_set_frame(int frame);
    void anim_toggle_play();
    void anim_select_clip(int clip);
    void set_texture(const u8* rgba, int w, int h);
    void clear_texture();
    void clear_mesh();
    void show_uv_editor();
    void rebuild_sel_list();   /* rebuild GL selection overlay */
    void clear_selection();
    void on_mouse_down(int x, int y, int btn);
    void on_mouse_up(int x, int y, int btn);
    void on_mouse_move(int x, int y, int btn_state);
    void do_pick(int mx, int my);
    void on_mouse_wheel(int delta);
    void on_key(int vk);
    void on_key_up(int vk);
    void walk_tick();
};

struct VideoPanel {
    HWND    hwnd;
    HBITMAP hBmp;
    void*   dib_bits;     /* persistent DIB pixel pointer */
    int     dib_w, dib_h; /* DIB dimensions */
    DecodedVideo video;
    int     cur_frame;
    bool    playing;
    UINT_PTR timer_id;
    HWND    slider;
    HWND    play_btn;
    HWND    label;

    /* Audio playback */
    HWAVEOUT hWaveOut;
    WAVEHDR  wave_hdr;
    int      audio_start_frame; /* video frame when audio started */

    VideoPanel() : hwnd(0), hBmp(0), dib_bits(0), dib_w(0), dib_h(0),
                   cur_frame(0), playing(false), timer_id(0),
                   slider(0), play_btn(0), label(0),
                   hWaveOut(0), audio_start_frame(0) {
        memset(&wave_hdr, 0, sizeof(wave_hdr));
    }
    ~VideoPanel() { stop(); if (hBmp) DeleteObject(hBmp); }

    bool load(const u8* data, size_t size);
    void show_frame(int idx);
    void play();
    void stop();
    void toggle_play();
    void start_audio();
    void stop_audio();
    void paint(HDC hdc, RECT& rc);
    void on_timer();
};

/* ═══ Save Editor Panel ═══ */
struct SavePanel {
    HWND    hwnd;
    DcSave  save;
    bool    dirty;
    char    filename[MAX_PATH];

    /* Controls */
    HWND    diff_combo, room_combo, preset_combo;
    HWND    time_h, time_m, time_s;
    HWND    continues_edit;
    HWND    arrange_chk;
    HWND    save_btn;
    HWND    ck_label;     /* checksum display */
    HWND    scroll_area;  /* scrollable child window */
    int     scroll_y;     /* current scroll offset */
    int     content_h;    /* total content height */

    /* Inventory slot controls */
    HWND    inv_item[10], inv_qty[10];

    /* Flag toggle checkboxes */
    HWND    flag_chk[16]; /* up to 16 flag items */

    SavePanel() : hwnd(0), dirty(false), scroll_y(0), content_h(0) {
        memset(&save, 0, sizeof(save));
        memset(filename, 0, sizeof(filename));
        memset(inv_item, 0, sizeof(inv_item));
        memset(inv_qty, 0, sizeof(inv_qty));
        memset(flag_chk, 0, sizeof(flag_chk));
        diff_combo = room_combo = preset_combo = 0;
        time_h = time_m = time_s = 0;
        continues_edit = arrange_chk = save_btn = ck_label = scroll_area = 0;
    }

    bool load(const u8* data, u32 size, const char* fname);
    void save_file();
    void read_controls();    /* pull values from UI controls into save struct */
    void update_controls();  /* push save struct values to UI controls */
    void apply_preset(int idx);
    void apply_room(int idx);
    void toggle_flag(int idx);
    void paint(HDC hdc, RECT& rc);
};

/*─── Weapon BIN Editor Panel ────────────────────────────────────*/
#define WEP_MAX_RECORDS 16
#define WEP_FIELDS_PER_REC 16  /* 16 x s16 = 32 bytes per record */

struct WepRecord {
    s16 fields[WEP_FIELDS_PER_REC];
};

struct WeaponPanel {
    HWND    hwnd;
    u8*     data;         /* owned copy of the entire BIN file */
    size_t  data_size;
    char    filename[MAX_PATH];

    /* Parsed weapon params */
    int     code_end;     /* end of MIPS code section */
    int     ptrtbl_start; /* start of pointer tables */
    int     n_records;    /* number of 32-byte param records */
    WepRecord records[WEP_MAX_RECORDS];

    /* UI controls: one edit per meaningful field per record */
    struct RecordUI {
        HWND lbl;           /* "Record N" label */
        HWND edits[10];     /* editable fields */
        HWND labels[10];    /* field name labels */
    };
    RecordUI rec_ui[WEP_MAX_RECORDS];
    int     aim_off;      /* aim/camera block after the pointer tables, -1 = none */
    HWND    aim_ui[14];   /* 8 aim edits, then 6 camera offset edits */
    HWND    save_btn;
    HWND    status_lbl;
    int     scroll_y;
    int     content_h;

    WeaponPanel() : hwnd(0), data(0), data_size(0), code_end(0),
                    ptrtbl_start(0), n_records(0), aim_off(-1), save_btn(0),
                    status_lbl(0), scroll_y(0), content_h(0) {
        memset(filename, 0, sizeof(filename));
        memset(records, 0, sizeof(records));
        memset(rec_ui, 0, sizeof(rec_ui));
        memset(aim_ui, 0, sizeof(aim_ui));
    }
    ~WeaponPanel() { free(data); }

    bool load(const u8* buf, size_t sz, const char* fname);
    void create_controls();
    void update_controls();  /* push record data to edit controls */
    void read_controls();    /* pull edit control values back to records */
    void save_file();
    void paint(HDC hdc, RECT& rc);
};

extern WeaponPanel g_weapon;

/*═══════════════════════════════════════════════════════════════════
 *  SCD Disassembler Panel
 *═══════════════════════════════════════════════════════════════════*/
#include "formats/scd.h"
#include "formats/dc2.h"

struct ScdPanel {
    HWND        hwnd;
    HFONT       font;
    ScdDisasm   disasm;
    int         scroll_y;     /* scroll position (line index) */
    int         sel_line;     /* selected line index (-1 = none) */
    int         vis_lines;    /* visible lines in viewport */
    int         line_h;       /* pixel height per line */
    bool        show_hex;     /* show hex column */
    bool        show_comments;/* show comment column */
    bool        loaded;
    RdtTextData text_data;    /* decoded text strings from RDT */

    /* Color scheme per category */
    COLORREF cat_colors[10];

    ScdPanel() : hwnd(0), font(0), scroll_y(0), sel_line(-1),
                 vis_lines(0), line_h(16), show_hex(true),
                 show_comments(true), loaded(false) {
        disasm.init();
        /* Default category colors (dark theme) */
        cat_colors[0] = RGB(200, 180, 255); /* flow    - lavender */
        cat_colors[1] = RGB(100, 200, 255); /* thread  - cyan */
        cat_colors[2] = RGB(255, 200, 100); /* var     - orange */
        cat_colors[3] = RGB(100, 255, 150); /* scene   - green */
        cat_colors[4] = RGB(255, 150, 100); /* entity  - salmon */
        cat_colors[5] = RGB(100, 150, 255); /* camera  - blue */
        cat_colors[6] = RGB(255, 255, 100); /* item    - yellow */
        cat_colors[7] = RGB(255, 100, 255); /* sound   - magenta */
        cat_colors[8] = RGB(200, 100, 255); /* effect  - purple */
        cat_colors[9] = RGB(120, 120, 120); /* nop     - gray */
    }
    ~ScdPanel() { disasm.clear(); }

    bool load(const u8* rdt_data, size_t rdt_size, u32 base_addr);
    bool load_dc2(const u8* room, size_t room_size, u32 base_addr);
    void paint(HDC hdc, RECT& rc);
    void on_scroll(int delta);
    void on_click(int y);
    void on_key(int vk);
};

extern ScdPanel g_scd;

LRESULT CALLBACK HexPanelProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ImagePanelProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK PalettePanelProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK AudioPanelProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK Viewer3DProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK VideoPanelProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SavePanelProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK WeaponPanelProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ScdPanelProc(HWND, UINT, WPARAM, LPARAM);

void register_panel_classes(HINSTANCE hInst);
void SavePanel_FinalizeDark(HWND hPanelArea);

#endif /* DC_UI_PANELS_H */
