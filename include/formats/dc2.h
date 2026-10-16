/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  Dino Crisis 2 (PC) Formats
 *
 *  Rooms (12-slot section directory, collision, zones), the room
 *  script VM, and DCM models with their textures.  The packages
 *  themselves are read by dat.cpp (DatArchive::is_dc2).
 *
 *  Format facts come from the dinorand project's Dino Crisis 2
 *  research and are cited by its registry row (K<n>) or document.
 *  Nothing here reads Dino2.exe: every offset is taken from the file.
 *═══════════════════════════════════════════════════════════════════*/
#ifndef DC_FORMATS_DC2_H
#define DC_FORMATS_DC2_H

#include "core/types.h"
#include "formats/dat.h"
#include "formats/mesh.h"
#include "formats/scd.h"

/*═══════════════════════════════════════════════════════════════════
 *  Room section directory
 *
 *  A room is the LZSS0 entry of ST*.DAT.  Decompressed, it starts with
 *  12 absolute pointers (K20, ROOM-SECTION-TABLE.md):
 *    [0] draw mesh            [6]  camera-switch zones
 *    [1] markers              [7]  zones
 *    [2] collision polygons   [8]  prop geometry
 *    [3] cameras              [9]  16-byte block
 *    [4] optional table       [10] unknown
 *    [5] script               [11] end of the blob
 *  Only slots 1, 2, 5 and 7 are decoded here.
 *═══════════════════════════════════════════════════════════════════*/
#define DC2_ROOM_SLOTS 12

struct Dc2RoomDir {
    bool present[DC2_ROOM_SLOTS];
    u32  off[DC2_ROOM_SLOTS];     /* offset in the blob */
    u32  size[DC2_ROOM_SLOTS];    /* up to the next higher slot offset */
};

/* Read the directory.  base is the entry's load address (DatEntry::load_base).
   False unless every non-null pointer lies in the blob and slot 11 points
   at its end, which is what tells a room from other LZSS0 data. */
bool dc2_room_directory(const u8* dec, size_t dec_size, u32 base, Dc2RoomDir& dir);

/* Short name of a slot for the tree, "" when out of range */
const char* dc2_room_slot_name(int slot);

/*─── Slot 1: markers (20-byte records) ──────────────────────────*/
struct Dc2Marker {
    u16 t;          /* +0   1 in every record        */
    u16 k;          /* +2   1000 in every record     */
    u32 id;         /* +4   bit-shaped value         */
    s16 x, y, z;    /* +8   position (y is 0)        */
    u16 flag;       /* +14  0x8000 in every record   */
    u16 p, q;       /* +16, +18                      */
};
int dc2_room_markers(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                     Dc2Marker* out, int max_out);

/*─── Slot 2: collision / floor polygons ─────────────────────────*/
struct Dc2Polygon {
    u8  flags;      /* +0  0x01 or 0x41 in most records          */
    u8  npoints;    /* +1                                        */
    u8  index;      /* +2  the record's own number               */
    u8  param;      /* +3                                        */
    u32 pts_off;    /* blob offset of the npoints+1 (x, z) pairs */
    u32 extra;      /* bytes in the record beyond 12 + 8 x npoints (0 normally) */
};
/* n_secondary receives the count of 20-byte records in the slot's second
   block, which is not decoded. */
int dc2_room_polygons(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                      Dc2Polygon* out, int max_out, int* n_secondary = 0);

/*─── Slot 7: zones (32-byte records) ────────────────────────────*/
struct Dc2Zone {
    s16 x[4], z[4]; /* +0   four (x, z) corners                  */
    u16 data_off;   /* +16  offset in the slot of the zone's data (not decoded) */
};
int dc2_room_zones(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                   Dc2Zone* out, int max_out);

/*═══════════════════════════════════════════════════════════════════
 *  Room script (slot 5)
 *
 *  A stack VM (K47, K48).  Opcodes are one byte and their length is
 *  fixed: 0x01-0x03 are 6 bytes, 0x05-0x08 are 4, 0x10-0x59 are 2.
 *  0x04 returns.  Routines are found through an offset directory at
 *  slot + 0x1C (K81).
 *═══════════════════════════════════════════════════════════════════*/
#define DC2_MAX_ROUTINES 64

enum { DC2_OP_RETURN = 0x04, DC2_OP_PUSH = 0x05, DC2_OP_JUMP = 0x19,
       DC2_OP_SPAWN = 0x1A, DC2_OP_DOOR = 0x30, DC2_OP_TRIGGER = 0x31,
       DC2_OP_TAKE_ITEM = 0x35 };

/* Length of an instruction as the walker steps it.  0x04 gives 2 and 0x19
   gives 4.  0 means the walk stops there: an unused opcode slot, or an
   opcode outside the table. */
int  dc2_scd_op_len(u8 op);
/* True for opcodes outside the dispatch table (0x60 and up).  The walker
   reports these; it never steps over them. */
bool dc2_scd_op_invalid(u8 op);
const char* dc2_scd_op_name(u8 op);
int  dc2_scd_op_category(u8 op);    /* ScdOpcodeInfo category, for colours */

/* Operand mode byte: 1 = variable, 2 = stack, 6 = global table; every
   other value is a literal (K122). */
enum { DC2_MODE_LITERAL = 0, DC2_MODE_VAR = 1, DC2_MODE_STACK = 2, DC2_MODE_GLOBAL = 6 };
int dc2_scd_mode(u8 mode_byte);

struct Dc2Routine { u32 start, end; };   /* blob offsets */

/* Routine starts from the directory, ascending, each ending at the next.
   dir_order (optional, DC2_MAX_ROUTINES entries) receives the starts in
   directory order, entry 0 being the room's init routine. */
int dc2_scd_routines(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                     Dc2Routine* out, int max_out, u32* dir_order = 0);

/* One operand of the block the pushes before an opcode build.  The last
   push is block + 0, the one before it block + 4, and so on. */
struct Dc2Operand {
    bool present;   /* a push supplies this block offset */
    u8   mode;      /* raw mode byte                      */
    s16  value;
    u32  value_off; /* blob offset of the value word      */
    bool literal() const { return present && dc2_scd_mode(mode) == DC2_MODE_LITERAL; }
};

/* 0x30: door (AOT-SYSTEM-RE.md, builder 0x489750) */
struct Dc2Door {
    u32 op_off;
    Dc2Operand dest;        /* block+0x0C  destination, stage << 8 | room (K81) */
    Dc2Operand lock;        /* block+0x04  lock message index, 0 = unlocked (K78) */
    Dc2Operand unlock_flag; /* block+0x00  group-8 flag that unlocks it */
    Dc2Operand flags;       /* block+0x44 */
    Dc2Operand door_type;   /* block+0x08 */
    Dc2Operand slot;        /* block+0x48 */
    Dc2Operand quad[8];     /* block+0x3C..0x20  x0 z0 x1 z1 x2 z2 x3 z3 */
};
/* 0x31: trigger that starts a script routine (K99, builder 0x489880) */
struct Dc2Trigger {
    u32 op_off;
    Dc2Operand routine;     /* block+0x00 */
    Dc2Operand thread;      /* block+0x04 */
    Dc2Operand slot;        /* block+0x30 */
    Dc2Operand quad[8];     /* block+0x24..0x08 */
};
/* 0x35: item pickup (K113, K129, builder 0x489B10) */
struct Dc2TakeItem {
    u32 op_off;
    Dc2Operand item;        /* block+0x10  item id */
    Dc2Operand taken_flag;  /* block+0x14  group-5 flag set when taken */
    Dc2Operand p3;          /* block+0x0C */
    Dc2Operand slot;        /* block+0x40 */
    Dc2Operand quad[8];     /* block+0x34..0x18 */
};
/* 0x1A: enemy spawn (K47, handler 0x487B70) */
struct Dc2Spawn {
    u32 op_off;
    int n_pushes;
    Dc2Operand gate;        /* block+0x00 */
    Dc2Operand model_base;  /* block+0x0C */
    Dc2Operand rot;         /* block+0x10 */
    Dc2Operand z, y, x;     /* block+0x14, 0x18, 0x1C */
    Dc2Operand type;        /* block+0x20 */
    Dc2Operand slot;        /* block+0x24 */
};

int dc2_scd_doors(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2Door* out, int max_out);
int dc2_scd_triggers(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2Trigger* out, int max_out);
int dc2_scd_take_items(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2TakeItem* out, int max_out);
int dc2_scd_spawns(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2Spawn* out, int max_out);

/* Walk totals, as dinorand's decode_script.py counts them */
struct Dc2ScdStats { int routines, ops, invalid; u32 invalid_off[16]; };
bool dc2_scd_stats(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2ScdStats& st);

/* Disassembly for the SCD panel.  Sets out.dc2. */
bool dc2_scd_disassemble(const u8* dec, size_t dec_size, u32 base, ScdDisasm& out);

/*─── Overlay ────────────────────────────────────────────────────*/
/* RdtFloorZone::target_index of the zones a DC2 room adds; the values
   pick the viewer's existing zone colours. */
enum { DC2_ZONE_TRIGGER = 1, DC2_ZONE_ITEM = 2, DC2_ZONE_DOOR = 4 };

/* Fill the overlay from slots 1, 2, 7 and the script: collision polygons
   (polys), slot-7 zones (floor zones), doors, triggers and item pickups
   (trigger zones, arg = destination / routine / item id, -1 when the
   script computes it), spawns (items) and markers (enemies).  Records
   whose position is not a literal are left out. */
bool dc2_room_overlay(const u8* dec, size_t dec_size, u32 base, RdtSceneOverlay& overlay);

/* The room's draw mesh is not decoded, so there is nothing to show but
   the overlay.  This gives the viewer the collision outline as a mesh to
   frame: one zero-area triangle along each polygon edge, in viewer
   coordinates on y = 0.  Filled it draws nothing; in wireframe (W) it is
   the outline. */
void dc2_room_outline_mesh(const RdtSceneOverlay& overlay, Mesh& mesh);

/*═══════════════════════════════════════════════════════════════════
 *  DCM model (K127, DCM-DC2-TEXTURE-PLAN.md)
 *
 *  An LZSS0 entry whose blob starts with the model header:
 *    +0x00 vertex ptr   +0x04 normal ptr   +0x08 triangle ptr
 *    +0x0C quad ptr     +0x10 u16 triangles, u16 quads
 *    +0x14 u32 parts    +0x18 u16 tpage, u16 clut
 *    +0x1C parts, 0x14 bytes each: s16 x, y, z; s8 parent; u8 n;
 *          triangle ptr; quad ptr; u16 triangles; u16 quads
 *  Vertices and normals are 8 bytes (s16 x, y, z, pad), triangles 12
 *  bytes (3 indices, 3 uv), quads 16 (4 indices, 4 uv).  Pointers are
 *  absolute; the entry's load address turns them into offsets.
 *═══════════════════════════════════════════════════════════════════*/
#define DC2_DCM_HEADER   0x1C
#define DC2_DCM_PART     0x14
#define DC2_DCM_MAX_PARTS 50     /* EmdSkeleton holds 50 bones */

struct Dc2ModelInfo {
    int parts, verts, tris, quads;
    u16 tpage, clut;
};

/* Check the header at the start of the blob without building anything. */
bool dc2_model_probe(const u8* dec, size_t dec_size, u32 base, Dc2ModelInfo& info);

/* Parse into the viewer's character model, in its bind pose.  Part i owns
   the next 3 x n vertices of the pool (n = its byte +7) and its bone
   places them. */
bool dc2_parse_model(const u8* dec, size_t dec_size, u32 base, EmdModel& model);

/* Texture and palette entries the model's tpage and clut select by VRAM
   position, in this archive or (for E*.DAT, whose skin is in E*.TEX) a
   second one.  -1 when there is none; pal is -1 for 16bpp. */
bool dc2_model_texture(const DatArchive& archive, u16 tpage, u16 clut,
                       int& tex_entry, int& pal_entry);

/* The alternate-skin file of a player package with no skin of its own:
   "WP<c><n>A.DAT" for "WEP_P<c>0<n>.DAT" (WP00A.DAT for WEP_P000.DAT).
   WEP_P000.DAT and WEP_P100.DAT hold the model and the weapon's texture
   but not the skin page at (896, 0); WEP_P001.DAT and up carry it.  The
   game loads WP<n>A only when a flag is set (loader 0x482420, gate
   0x482631), so it is a stand-in: which file gives the default skin is
   not known.  False when the name is not of that form. */
bool dc2_alt_skin_name(const char* package_name, char out[16]);

#endif /* DC_FORMATS_DC2_H */
