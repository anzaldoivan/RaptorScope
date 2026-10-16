/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  3D Mesh Formats (Room, Door, EMD Character)
 *═══════════════════════════════════════════════════════════════════*/
#ifndef DC_FORMATS_MESH_H
#define DC_FORMATS_MESH_H

#include "core/types.h"
#include "formats/dat.h"
#include <stdlib.h>
#include <string.h>

/*─── Triangle face ──────────────────────────────────────────────*/
struct MeshTri {
    u32  idx[3];       /* vertex indices into parent vertex array */
    RGBA8 color;
    u16  tpage;
    u16  clut;         /* PS1 CLUT field (bits 0-5=X/16, bits 6-14=Y) */
    u8   uv[3][2];    /* (u, v) per vertex corner */
    u8   alt;          /* 1 = conditional variant geometry (SCD branch) */
    u8   slot;         /* room model slot the section is placed in (0x23) */
    u8   gouraud;      /* 1 = vcol holds the face's corner colours */
    u8   vcol[3][3];   /* RGB per corner as stored (128 = texture as is), in uv order */
    u32  src_off;      /* section header offset in RDT (0=unknown), for object picking */
};

/*─── Mesh vertex ────────────────────────────────────────────────*/
struct MeshVert {
    f32 x, y, z;
    f32 nx, ny, nz;    /* per-vertex normal (0,0,0 = not set) */
};

/*─── Generic mesh (shared by room, door, EMD) ───────────────────*/
struct Mesh {
    MeshVert* verts;
    int       vert_count;
    MeshTri*  tris;
    int       tri_count;

    Mesh() : verts(0), vert_count(0), tris(0), tri_count(0) {}
    ~Mesh() { free(verts); free(tris); }

    void alloc(int nv, int nt) {
        free(verts); free(tris);
        verts = (MeshVert*)calloc(nv, sizeof(MeshVert));
        tris  = (MeshTri*) calloc(nt, sizeof(MeshTri));
        vert_count = nv;
        tri_count  = nt;
    }

private:
    Mesh(const Mesh&);
    Mesh& operator=(const Mesh&);
};

/* Compute smooth normals by accumulating face normals at shared vertex
   positions.  Call after mesh is fully populated.  Skips vertices that
   already have a non-zero normal (e.g. from parsed EMD normal data). */
void mesh_compute_smooth_normals(Mesh& mesh);

/*─── Skeleton bone (EMD character models) ───────────────────────*/
struct EmdBone {
    s16 bx, by, bz;       /* local offset from parent */
    u8  parent;
    u8  flags;
    int vert_start;        /* first vertex in V_pool belonging to this bone */
    int vert_count;
    int tri_ptr_offset;    /* file offset to tri data */
    int quad_ptr_offset;   /* file offset to quad data */
    int n_tris;
    int n_quads;
};

struct EmdSkeleton {
    EmdBone bones[50];
    int     bone_count;
    f32     abs_pos[50][3];  /* world-space accumulated positions */
};

/*─── Animation clip (extracted from EMD data) ─────────────────*/
struct EmdAnimClip {
    int  offset;       /* offset in decompressed data of clip header */
    int  frame_data;   /* offset to first frame */
    int  frames;       /* frames in this clip: the stored count + 1, or the
                          stored count for the few clips with no valid extra frame */
    int  frame_size;   /* bytes per frame (from pointer table) */
};

/*─── EMD Character model ────────────────────────────────────────*/
struct EmdModel {
    Mesh       mesh;            /* expanded triangle soup */
    EmdSkeleton skeleton;
    int        n_anims;
    bool       valid;

    /* Per-vertex source tracking for animation */
    struct VertSrc {
        int  pool_idx;
        bool is_mirror;
        int  dest_bone;
    };
    VertSrc*  vert_src;
    int       vert_src_count;

    /* Raw vertex pool (local coords + bone idx) */
    struct PoolVert { s16 x, y, z; u16 bone_idx; };
    PoolVert* pool;
    int       pool_count;

    /* Animation clips */
    EmdAnimClip* clips;
    int          clip_count;

    /* Raw decompressed data (needed for animation playback) */
    u8*    raw_dec;
    size_t raw_dec_size;
    u32    base_addr;   /* PSX base address for pointer resolution */
    size_t norm_pool_off; /* offset of normal pool in raw_dec (targets[1]) */

    /* Part info for FK chain */
    struct PartInfo { s16 bx, by, bz; int parent; };
    PartInfo* parts_info;
    int       parts_count;

    EmdModel() : n_anims(0), valid(false), vert_src(0),
                 vert_src_count(0), pool(0), pool_count(0),
                 clips(0), clip_count(0), raw_dec(0), raw_dec_size(0),
                 base_addr(0), norm_pool_off(0), parts_info(0), parts_count(0) {}
    ~EmdModel() { free(vert_src); free(pool); free(clips);
                  free(raw_dec); free(parts_info); }
};

/*─── RDT Layout  (deterministic scene structure) ────────────────*/
#define RDT_MAX_SECTIONS  64
#define RDT_MAX_EMDS      16

struct RdtMeshSection {
    size_t offset;       /* file offset of 12-byte section header   */
    u16    tri_count;
    u16    quad_count;
};

struct RdtEmdEntry {
    size_t offset;       /* file offset of 24-byte EMD header       */
    int    vert_count;
    int    tri_count;
    int    quad_count;
    int    part_count;
    size_t data_end;     /* first byte after this EMD's quad data   */
};

struct RdtLayout {
    /* Header */
    u32    base;              /* PSX base address for this scene     */
    u32    hdr_ptrs[7];      /* 7 raw PSX pointer values            */

    /* Room mesh sections */
    RdtMeshSection sections[RDT_MAX_SECTIONS];
    int    section_count;
    int    total_room_tris;   /* sum of all section tri_count       */
    int    total_room_quads;  /* sum of all section quad_count      */
    size_t room_mesh_end;     /* first byte after last section      */

    /* EMD character models */
    RdtEmdEntry emds[RDT_MAX_EMDS];
    int    emd_count;
    size_t emds_end;          /* first byte after last EMD          */

    /* Validity */
    bool   valid;

    RdtLayout() : base(0), section_count(0), total_room_tris(0),
                  total_room_quads(0), room_mesh_end(0), emd_count(0),
                  emds_end(0), valid(false) {
        memset(hdr_ptrs, 0, sizeof(hdr_ptrs));
        memset(sections, 0, sizeof(sections));
        memset(emds, 0, sizeof(emds));
    }
};

/*─── Parsing functions ──────────────────────────────────────────*/

/* Walk decompressed RDT data deterministically:
   7-ptr header → mesh sections → EMD models.
   Fills layout.  Returns true if at least sections OR emds found.
   dec/dec_size = already-decompressed LZSS0 data.
   base = PSX base address. */
bool parse_rdt_layout(const u8* dec, size_t dec_size, u32 base,
                      RdtLayout& layout);

/* Convenience: decompress + parse layout in one call. */
bool parse_rdt_layout_from_entry(const u8* data, size_t size,
                                 u16 ey, u16 ex, RdtLayout& layout);

/* Room RDTs are normally LZSS0, but a few rooms (ST50B, ST60E) store theirs
   uncompressed as a DATA entry.  True for such entries. */
bool is_raw_rdt_entry(const DatEntry& e);

/* Parse room mesh.  Original API decompresses internally. */
bool parse_room_mesh(const u8* data, size_t size, u32 base_addr,
                     Mesh& mesh);

/* Parse room mesh from pre-decompressed buffer.
   Optional exclude_offsets: array of section file-offsets to skip
   (used to avoid rendering sections that only appear via non-zero xforms). */
bool parse_room_mesh_dec(const u8* dec, size_t dec_size, u32 base_addr,
                         Mesh& mesh,
                         const u32* exclude_offsets = 0, int n_exclude = 0);

/* Parse door mesh.  Original API decompresses internally. */
bool parse_door_mesh(const u8* data, size_t size, u32 base_addr,
                     Mesh& mesh);

/* Parse door mesh from pre-decompressed buffer. */
bool parse_door_mesh_dec(const u8* dec, size_t dec_size, u32 base_addr,
                         Mesh& mesh);

/* Parse RDT scene: room mesh + instanced sub-objects with positions. */
bool parse_rdt_scene(const u8* data, size_t size, u32 base_addr,
                     Mesh& mesh);

/* Parse RDT scene from pre-decompressed buffer.
   exclude_offsets: section offsets to skip (only rendered via xform instancing). */
bool parse_rdt_scene_dec(const u8* dec, size_t dec_size, u32 base_addr,
                         Mesh& mesh,
                         const u32* exclude_offsets = 0, int n_exclude = 0);

/* Check if a DatEntry contains a door mesh. */
bool is_door_mesh_entry(const u8* data, size_t size,
                        u16 ey, u16 ew, u16 eh);

/* Check if decompressed LZSS0 blob is a standalone character EMD
   (p/h file entry 3: skeleton + animation + mesh at end).
   Uses 48-byte tris / 64-byte quads with interleaved normals. */
bool is_standalone_emd_dec(const u8* dec, size_t dec_size, u32 base);

/* Parse standalone character EMD mesh from pre-decompressed buffer. */
bool parse_standalone_emd_mesh_dec(const u8* dec, size_t dec_size,
                                   u32 base, Mesh& mesh);

/* Check if a DatEntry contains an EMD character model. */
bool is_emd_entry(const u8* data, size_t size, u16 ey, u16 ex);

/* Find all EMD headers in a decompressed LZSS0 blob.
   Returns count of EMDs found (up to max_out).
   out_offsets[] receives the byte offset of each EMD header. */
int find_emd_headers(const u8* data, size_t size, u16 ey, u16 ex,
                     size_t* out_offsets, int max_out);

/* Check if decompressed LZSS0 data is MIPS R3000 code overlay. */
bool is_mips_code(const u8* data, size_t size, u16 ey, u16 ex);
bool is_weapon_data(const u8* data, size_t size, u16 ey, u16 ex);

/* Pre-decompressed variants of detection functions. */
bool is_door_mesh_entry_dec(const u8* dec, size_t dec_size, u16 ey);
bool is_weapon_data_dec(const u8* dec, size_t dec_size);
bool is_mips_code_dec(const u8* dec, size_t dec_size);

/* Check if decompressed LZSS0 data is an RDT scene package
   (room mesh + instanced objects + metadata). */
bool is_rdt_scene(const u8* data, size_t size, u16 ey, u16 ex);

/* Parse EMD character model.  Fills model with mesh + skeleton.
   If hint_offset >= 0, skip directly to that offset in decompressed data. */
bool parse_emd_model(const u8* entry_data, size_t entry_size,
                     u16 ey, u16 ex, EmdModel& model,
                     int hint_offset = -1);

/* Parse EMD from pre-decompressed buffer. */
bool parse_emd_model_dec(const u8* dec, size_t dec_size,
                         u16 ey, u16 ex, EmdModel& model,
                         int hint_offset = -1);

/* Extract animation clips from EMD entry and store in model. */
void extract_emd_anims(EmdModel& model);

/* Compute animated vertex positions for a given clip/frame.
   Updates model.mesh.verts in place.  Returns false if invalid. */
bool compute_anim_frame(EmdModel& model, int clip_idx, int frame_idx);

/*─── OBJ Export ─────────────────────────────────────────────────*/
bool export_mesh_obj(const char* path, const Mesh& mesh);
/* Shared, non-mutating pose decoder; same motion as viewer playback. */
bool decode_emd_pose(const EmdModel& model, int clip, int frame,
                     f32 positions[50][3], f32 matrices[50][9], f32 rotations[50][3]);
/* Optional viewer atlas; pixels are top-down RGBA. */
struct EmdExportAtlas {
    const u8* rgba;
    int width, height, slices, bpp, vram_x, vram_y, clut_x, clut_y;
    const int* slice_map;
};
bool export_emd_glb(const char* path, const EmdModel& model,
                    const EmdExportAtlas* atlas = 0, f32 fps = 30.0f);
/* Static room mesh as shown in the viewer: one node per section, textured
   from the atlas when given.  Faces flagged alt are skipped. */
bool export_room_glb(const char* path, const MeshVert* verts, int vert_count,
                     const MeshTri* tris, int tri_count,
                     const EmdExportAtlas* atlas = 0, const char* name = 0);

bool export_emd_smd(const char* path, const EmdModel& emd);
bool export_mesh_smd(const char* path, const Mesh& mesh);

/*─── Bone name lookup (auto-detects skeleton type by bone count) ─*/
const char* get_bone_name(int bone_count, int bone_idx);

/*═══════════════════════════════════════════════════════════════════
 *  RDT Scene Overlay  –  non-mesh spatial data from room scripts
 *
 *  The Dino Crisis RDT blob contains 7 header pointers:
 *    ptr[0] = SCD main script table
 *    ptr[1] = Collision rectangles  (gs+336, 8-byte index + 48-byte rects)
 *    ptr[2] = Camera cut table      (gs+340, 16-byte groups + 20-byte zones)
 *    ptr[3] = Script thread table   (gs+356)
 *    ptr[4] = Runtime slot table    (gs+488, 10 u32 slots)
 *    ptr[5] = SCD bytecode base     (gs+616)
 *    ptr[6] = Floor zones           (gs+620, 20-byte quads)
 *
 *  The SCD bytecode contains:
 *    op 0x23 (32B) – room model registration with world-space transforms
 *    op 0x42 (20B) – dynamic object spawn with position
 *    op 0x20 (24B) – EMD character placement
 *    op 0x4C (32B) – camera eye/target setup
 *    op 0x28 (var) – collision zone setup
 *═══════════════════════════════════════════════════════════════════*/

/*─── Zone: a ptr[6] floor zone or an SCD 0x28 trigger zone ─────*/
enum { RDT_ZONE_FLOOR = 0, RDT_ZONE_TRIGGER = 1 };

struct RdtFloorZone {
    s16 x[4], z[4];          /* 4 corner XZ coords (world-space) */
    s16 y;                    /* floor Y position */
    s16 height;               /* zone height above Y (0 = flat) */
    u8  collision_group;      /* floor zone: level, matched against the walker's +0x48; 0x28 zone: zone slot */
    u8  target_index;         /* floor zone: footstep step (sound 0xB0 + 2 * step); 0x28 zone: type */
    u8  flags;                /* floor zone: record byte +18 (0 in every room); 0x28 zone: 0 */
    u8  kind;                 /* RDT_ZONE_FLOOR or RDT_ZONE_TRIGGER, set by the parser that adds it */
    s16 arg;                  /* DC2 script zone: door destination, routine or item id; -1 = computed by the script */
};

/*─── Collision rectangle ───────────────────────────────────────*/
struct RdtCollisionRect {
    s16 x, z;                /* top-left corner (world XZ) */
    u16 w, h;                /* width, height */
    u8  group;               /* collision group index */
    u8  flags;
};

/*─── Camera cut entry ──────────────────────────────────────────*/
struct RdtCameraEntry {
    s16 eye_x, eye_y, eye_z;       /* camera eye position */
    s16 tgt_x, tgt_y, tgt_z;       /* camera target position */
    u16 fov;
    u16 flags;
};

/*─── Door / room transition ────────────────────────────────────*/
/* ptr[4] is a runtime pointer table filled by SCD opcode 0x28.
   Door zones come from 0x28 type 4, stored in the zones[] array. */

/*─── Camera cut trigger zone ──────────────────────────────────*/
/* From ptr[2] camera cut table — spatial region activating a camera.
   Format: type(u8), flags(u8), pad(u16), x(s16), z(s16), w(s16), h(s16) */
struct RdtCameraCutZone {
    u8  type;                 /* zone shape type */
    u8  cam_group;            /* which camera group this zone belongs to */
    s16 x, z;                 /* top-left corner */
    s16 w, h;                 /* width, height (XZ plane) */
};

/*─── SCD opcode 0x23: section transform ────────────────────────*/
struct RdtSectionXform {
    u32 section_off;          /* file offset of inline section header */
    u8  slot;
    u8  flags;                /* byte[2] */
    u8  render_mode;          /* byte[3]: face sort (DINO.exe 0x44DA6D): 0=nearest vertex, 1=farthest vertex, 2/3=whole section at one depth */
    u8  pad;
    u16 ot_depth;             /* bytes[4..5]: OT depth for mode 2/3 (w74) */
    s16 px, py, pz;
    s16 rx, ry, rz;
};

/* Apply SCD 0x23 section transforms (instancing) to a parsed room mesh. */
void mesh_apply_xforms(Mesh& mesh, const u8* dec, size_t dec_size, u32 base,
                       const RdtSectionXform* xforms, int n_xforms);

/*─── SCD opcode 0x42: object spawn ─────────────────────────────*/
struct RdtObjectSpawn {
    u8  slot;
    u8  mesh_type;
    u8  mesh_ref;
    u8  flags;
    s16 px, py, pz;
    s16 rot_y;
};

/*─── SCD opcode 0x5B: enemy spawn (44B) ──────────────────────*/
struct RdtItemSpawn {
    u8  slot;
    u8  type;
    u8  anim_set;
    s16 px, py, pz;
    s16 rot_y;
    u32 mesh_ptr;
    u32 anim_ptr;
};

/*─── SCD opcode 0x20: EMD character placement ──────────────────*/
struct RdtCharPlace {
    u8  slot;
    u8  flags;
    u8  type;
    u8  anim_set;
    s16 px, py, pz;
    s16 rot_y;
    u32 mesh_ptr;             /* PSX address of EMD data */
    u32 anim_ptr;
};

/*─── SCD opcode 0x3A: scene light source (12B) ────────────────*/
struct RdtSceneLight {
    u8  slot;
    s16 px, py, pz;           /* world-space position */
    s16 radius;               /* light falloff radius */
    u8  r, g, b;              /* light color (0-255) */
    u8  has_pos;              /* set if position was parsed */
    u8  has_color;            /* set if color was parsed */
};

/*─── SCD opcode 0x42: object (20B) ───────────────────────────*/
#define RDT_MAX_OBJECTS 512
struct RdtEnemySpawn {
    u8  type;                 /* object type (byte[2]) */
    u8  id;                   /* object record, 0xFF = any free one (byte[1]) */
    u8  flags;                /* byte[3] */
    s16 px, py, pz;           /* world-space position (+4/+6/+8) */
    s16 rot_y;                /* u16 at +12 */
};

/*─── SCD opcode 0x3D: fog/atmosphere effect (12B) ────────────*/
struct RdtFogParams {
    u8  type;                 /* effect type */
    s16 dist_near, dist_far;  /* fog range */
    u8  r, g, b;              /* fog color */
};

/*─── SCD opcode 0x3C: dynamic light color (8B) ──────────────*/
struct RdtLightColor {
    u8  light_idx;            /* which light slot */
    u8  component;            /* 0=intensity, 1=on/off, 2=color */
    u16 value;                /* intensity/color value */
};

/*─── Dino Crisis 2 collision polygon (room slot 2) ────────────*/
#define RDT_MAX_POLYS    128
#define RDT_MAX_POLY_PTS 1024
struct RdtPoly {
    u16 first, count;         /* points in RdtSceneOverlay::poly_pts */
    u8  flags, index;         /* record bytes +0 and +2 */
};

/*─── Complete RDT scene overlay ────────────────────────────────*/
struct RdtSceneOverlay {
    RdtFloorZone      zones[64];       int n_zones;
    RdtCollisionRect  collisions[256]; int n_collisions;
    RdtCameraEntry    cameras[32];     int n_cameras;
    RdtSectionXform   xforms[64];      int n_xforms;
    RdtObjectSpawn    spawns[64];      int n_spawns;
    RdtCharPlace      chars[16];       int n_chars;
    RdtCameraCutZone  camcuts[128];    int n_camcuts;
    RdtSceneLight     lights[8];       int n_lights;
    u8  ambient_r, ambient_g, ambient_b;  /* room ambient from ptr[0]+8 */
    RdtItemSpawn      items[32];       int n_items;
    RdtEnemySpawn     enemies[RDT_MAX_OBJECTS]; int n_enemies;
    RdtFogParams      fog[8];          int n_fog;
    RdtLightColor     light_cols[64];  int n_light_cols;

    /* Dino Crisis 2 room (dc2_room_overlay): the names and labels of the
       overlay tree change, and collision is polygons instead of rects. */
    u8                dc2;
    RdtPoly           polys[RDT_MAX_POLYS];  int n_polys;
    s16               poly_pts[RDT_MAX_POLY_PTS][2];  int n_poly_pts;  /* (x, z) */

    /* Camera thread activation zones from ptr[3].
       Each thread has a 4-corner XZ quad at +4..+19 defining where
       the camera angle activates when the player enters. */
    struct CamThread {
        s16 x[4], z[4];      /* activation quad corners */
        u8  type;             /* thread type (byte[0]) */
        u8  cam_id;           /* camera ID (byte[2]) */
        u8  desc[68];         /* whole descriptor, for rdt_camera_shot */
    };
    CamThread  cam_threads[32]; int n_cam_threads;

    /* Camera setups.  When the game switches to camera N (a ptr[3]
       cam_id) it runs script thread N, which shows or hides model slots
       with 0x48 03 <slot> <1|0>.  Rooms keep a copy of their walls per
       camera (ST10A's corkboard sits at a different spot in slots 2 and
       4), so drawing every slot at once doubles such details. */
    struct CamView {
        u8  cam_id;
        u64 show_mask, hide_mask;   /* bit = model slot */
    };
    CamView    cam_views[32]; int n_cam_views;

    /* Viewer Y of the floor under each flat shape (from mesh data), set by
       overlay_floor_heights; floor_y is the room-wide floor. */
    f32 floor_y;
    f32 zone_y[64], rect_y[256], camcut_y[128], cam_thread_y[32];

    RdtSceneOverlay() { memset(this, 0, sizeof(*this)); }
};

/*─── Parse RDT scene overlay from decompressed blob ────────────*/
bool parse_rdt_overlay(const u8* dec, size_t dec_size, u32 base_addr,
                       RdtSceneOverlay& overlay);

/* Floor heights (viewer Y) for the overlay's flat shapes, found from the
   upward-facing horizontal faces of the placed room mesh: sets floor_y,
   zone_y, rect_y, camcut_y and cam_thread_y. */
void overlay_floor_heights(const Mesh& mesh, RdtSceneOverlay& overlay);

/* Camera shot of a ptr[3] descriptor with the player at player[3]: eye and
   target in raw game coordinates (y down), as DINO.exe's per-type camera
   handlers (table at 0x645FA8) place them.  The game projects with H=320
   on a 320x240 screen.  Returns RDT_SHOT_NONE for types that set no
   camera of their own (none, or relative to the previous shot). */
enum { RDT_SHOT_NONE = 0, RDT_SHOT_FIXED = 1, RDT_SHOT_TRACKING = 2 };
int rdt_camera_shot(const u8* desc, const f32 player[3], f32 eye[3], f32 tgt[3]);

/*─── Texture usage ──────────────────────────────────────────────*/
/* How one textured face samples VRAM: page and colour depth (tpage),
   CLUT position (clut) and the triangle's UVs within the page. */
struct TexFaceUse {
    u16 tpage;
    u16 clut;
    u8  uv[3][2];
};

/* Collect the textured faces of every room, character and door mesh in
   the archive.  *out is malloc'd (caller frees); returns the face count. */
int collect_texture_usage(const DatArchive& archive, TexFaceUse** out);

#endif /* DC_FORMATS_MESH_H */
