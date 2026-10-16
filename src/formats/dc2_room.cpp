/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  Dino Crisis 2 Rooms
 *
 *  The 12-slot section directory of an ST*.DAT room blob, the three
 *  sections whose records are decoded (1 markers, 2 collision
 *  polygons, 7 zones), and the scene overlay built from them and from
 *  the room script.
 *
 *  Sources: dinorand ROOM-SECTION-TABLE.md (K20) and its tools
 *  label_sections.py, decode_s1_s7.py and decode_s2.py.
 *═══════════════════════════════════════════════════════════════════*/
#include "formats/dc2.h"
#include <string.h>

/*─── Section directory ──────────────────────────────────────────*/

/* Every room starts with 12 absolute pointers.  A null one means the
   room has no such section (ST500 has no slot 4 or 7).  A section runs to
   the next higher pointer; slot 11 is the end of the blob in all 89 rooms,
   so it doubles as the test for "this blob is a room". */
bool dc2_room_directory(const u8* dec, size_t dec_size, u32 base, Dc2RoomDir& dir)
{
    memset(&dir, 0, sizeof(dir));
    if (!dec || dec_size < DC2_ROOM_SLOTS * 4 || dec_size > 0x7FFFFFFFu) return false;

    for (int i = 0; i < DC2_ROOM_SLOTS; i++) {
        u32 v = rd_u32(dec + i * 4);
        if (v == 0) continue;
        if (v < base || v - base > dec_size) return false;
        dir.present[i] = true;
        dir.off[i] = v - base;
    }
    if (!dir.present[11] || dir.off[11] != dec_size) return false;

    for (int i = 0; i < DC2_ROOM_SLOTS; i++) {
        if (!dir.present[i]) continue;
        u32 next = (u32)dec_size;
        for (int j = 0; j < DC2_ROOM_SLOTS; j++)
            if (dir.present[j] && dir.off[j] > dir.off[i] && dir.off[j] < next)
                next = dir.off[j];
        dir.size[i] = next - dir.off[i];
    }
    return true;
}

const char* dc2_room_slot_name(int slot)
{
    /* Labels from ROOM-SECTION-TABLE.md.  Slots 4, 9 and 10 have no
       established meaning (K137), and 1 and 7 no established purpose. */
    static const char* names[DC2_ROOM_SLOTS] = {
        "Draw mesh (not decoded)", "Markers", "Collision polygons",
        "Cameras (not decoded)", "Table (unknown)", "Script",
        "Camera-switch zones (not decoded)", "Zones",
        "Prop geometry (not decoded)", "Block (unknown)", "Unknown", "End"
    };
    return (slot >= 0 && slot < DC2_ROOM_SLOTS) ? names[slot] : "";
}

/*─── Slot 1: markers ────────────────────────────────────────────*/

/* u16 count, u16 header word, then 20-byte records.  ST100 has 7. */
int dc2_room_markers(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                     Dc2Marker* out, int max_out)
{
    if (!dir.present[1] || dir.size[1] < 4) return 0;
    size_t o = dir.off[1], end = o + dir.size[1];
    if (end > dec_size) return 0;
    int count = rd_u16(dec + o), n = 0;
    for (int i = 0; i < count && n < max_out; i++) {
        size_t p = o + 4 + (size_t)i * 20;
        if (p + 20 > end) break;
        Dc2Marker& m = out[n++];
        m.t    = rd_u16(dec + p);
        m.k    = rd_u16(dec + p + 2);
        m.id   = rd_u32(dec + p + 4);
        m.x    = rd_s16(dec + p + 8);
        m.y    = rd_s16(dec + p + 10);
        m.z    = rd_s16(dec + p + 12);
        m.flag = rd_u16(dec + p + 14);
        m.p    = rd_u16(dec + p + 16);
        m.q    = rd_u16(dec + p + 18);
    }
    return n;
}

/*─── Slot 2: collision / floor polygons ─────────────────────────*/

/* Header: u8 count, u8 count of a second block, u16 0, u32 offset of the
   second block (0 when there is none), then one u32 record offset per
   polygon, all relative to the slot.

   Record (12 + 8 x npoints bytes; 44 for the common 4-point polygon):
     +0  u8 flags   +1  u8 npoints   +2  u8 index   +3  u8 param
     +4  4 bytes, meaning unknown
     +8  one u32 attribute word per point
     ..  npoints + 1 (x, z) pairs of s16; the last repeats the first

   Checked on the 89 rooms: 838 of 840 records are exactly that long and
   close on their first point.  ST702's record 2 and ST904's record 9 are
   longer; their extra bytes are counted in Dc2Polygon::extra and not read.

   dinorand's decode_s2.py reads the header as a u32 count followed by the
   offsets, which takes the second-block offset for the first record; its
   record lengths are shifted by one as a result.  The second block (20
   bytes per record in the 10 rooms that have one) is not decoded. */
int dc2_room_polygons(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                      Dc2Polygon* out, int max_out, int* n_secondary)
{
    if (n_secondary) *n_secondary = 0;
    if (!dir.present[2] || dir.size[2] < 8) return 0;
    size_t o = dir.off[2], size = dir.size[2];
    if (o + size > dec_size) return 0;

    int count = dec[o];
    u32 second = rd_u32(dec + o + 4);
    if (rd_u16(dec + o + 2) != 0 || second > size) return 0;
    if (8 + (size_t)count * 4 > size) return 0;
    size_t polys_end = second ? second : size;
    if (n_secondary && second) *n_secondary = dec[o + 1];

    int n = 0;
    for (int i = 0; i < count && n < max_out; i++) {
        size_t r = rd_u32(dec + o + 8 + (size_t)i * 4);
        size_t next = (i + 1 < count) ? rd_u32(dec + o + 12 + (size_t)i * 4) : polys_end;
        if (next > polys_end) next = polys_end;
        if (r + 8 > next) continue;                  /* truncated header */
        size_t need = 12 + 8 * (size_t)dec[o + r + 1];
        if (r + need > next) continue;               /* truncated points */
        Dc2Polygon& p = out[n++];
        p.flags   = dec[o + r];
        p.npoints = dec[o + r + 1];
        p.index   = dec[o + r + 2];
        p.param   = dec[o + r + 3];
        p.pts_off = (u32)(o + r + 8 + 4 * (size_t)p.npoints);
        p.extra   = (u32)(next - r - need);
    }
    return n;
}

/*─── Slot 7: zones ──────────────────────────────────────────────*/

/* A 16-byte header whose first byte is the zone count, then one 32-byte
   record per zone: four (x, z) corners, a u16 offset to the zone's own
   data further down the slot, and 14 more bytes.  The first zone's data
   starts right after the records, at 16 + 32 x count, which is the check
   that the slot holds zones at all (ST103: 7 zones, first offset 0xF0).
   What the zones do, and the data they point to, is not decoded; ST102,
   ST10D and other rooms with enemies have them and corridors do not. */
int dc2_room_zones(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                   Dc2Zone* out, int max_out)
{
    if (!dir.present[7] || dir.size[7] < 16 + 32) return 0;
    size_t o = dir.off[7], size = dir.size[7];
    if (o + size > dec_size) return 0;

    int count = dec[o];
    size_t recs_end = 16 + 32 * (size_t)count;
    if (count == 0 || recs_end > size) return 0;
    if (rd_u16(dec + o + 16 + 16) != recs_end) return 0;

    int n = 0;
    for (int i = 0; i < count && n < max_out; i++) {
        const u8* r = dec + o + 16 + 32 * (size_t)i;
        Dc2Zone& z = out[n++];
        for (int c = 0; c < 4; c++) {
            z.x[c] = rd_s16(r + c * 4);
            z.z[c] = rd_s16(r + c * 4 + 2);
        }
        z.data_off = rd_u16(r + 16);
    }
    return n;
}

/*─── Overlay ────────────────────────────────────────────────────*/

/* Add a zone from eight script operands (x0 z0 .. x3 z3).  A zone whose
   corners the script computes cannot be placed and is left out. */
static void add_script_zone(RdtSceneOverlay& ov, const Dc2Operand* quad,
                            int type, const Dc2Operand& arg, const Dc2Operand& slot)
{
    if (ov.n_zones >= (int)(sizeof(ov.zones) / sizeof(ov.zones[0]))) return;
    for (int i = 0; i < 8; i++)
        if (!quad[i].literal()) return;
    RdtFloorZone& z = ov.zones[ov.n_zones++];
    memset(&z, 0, sizeof(z));
    for (int c = 0; c < 4; c++) {
        z.x[c] = quad[c * 2].value;
        z.z[c] = quad[c * 2 + 1].value;
    }
    z.kind = RDT_ZONE_TRIGGER;
    z.target_index = (u8)type;
    z.collision_group = slot.literal() ? (u8)slot.value : 0;
    z.arg = arg.literal() ? arg.value : (s16)-1;
}

bool dc2_room_overlay(const u8* dec, size_t dec_size, u32 base, RdtSceneOverlay& ov)
{
    Dc2RoomDir dir;
    if (!dc2_room_directory(dec, dec_size, base, dir)) return false;
    memset((void*)&ov, 0, sizeof(ov));
    ov.dc2 = 1;

    /* Slot 2: collision polygons, closing point dropped */
    Dc2Polygon polys[RDT_MAX_POLYS];
    int n = dc2_room_polygons(dec, dec_size, dir, polys, RDT_MAX_POLYS);
    for (int i = 0; i < n; i++) {
        const Dc2Polygon& p = polys[i];
        if (p.npoints == 0 || ov.n_poly_pts + p.npoints > RDT_MAX_POLY_PTS) continue;
        RdtPoly& op = ov.polys[ov.n_polys++];
        op.first = (u16)ov.n_poly_pts;
        op.count = p.npoints;
        op.flags = p.flags;
        op.index = p.index;
        for (int k = 0; k < p.npoints; k++) {
            ov.poly_pts[ov.n_poly_pts][0] = rd_s16(dec + p.pts_off + k * 4);
            ov.poly_pts[ov.n_poly_pts][1] = rd_s16(dec + p.pts_off + k * 4 + 2);
            ov.n_poly_pts++;
        }
    }

    /* Slot 7: zones, drawn as floor zones */
    Dc2Zone zones[64];
    n = dc2_room_zones(dec, dec_size, dir, zones, 64);
    for (int i = 0; i < n && ov.n_zones < 64; i++) {
        RdtFloorZone& z = ov.zones[ov.n_zones++];
        memset(&z, 0, sizeof(z));
        memcpy(z.x, zones[i].x, sizeof(z.x));
        memcpy(z.z, zones[i].z, sizeof(z.z));
        z.kind = RDT_ZONE_FLOOR;
        z.target_index = (u8)i;
    }

    /* Script: doors (0x30), triggers (0x31), item pickups (0x35) */
    Dc2Door doors[64];
    n = dc2_scd_doors(dec, dec_size, dir, doors, 64);
    for (int i = 0; i < n; i++)
        add_script_zone(ov, doors[i].quad, DC2_ZONE_DOOR, doors[i].dest, doors[i].slot);
    Dc2Trigger trigs[64];
    n = dc2_scd_triggers(dec, dec_size, dir, trigs, 64);
    for (int i = 0; i < n; i++)
        add_script_zone(ov, trigs[i].quad, DC2_ZONE_TRIGGER, trigs[i].routine, trigs[i].slot);
    Dc2TakeItem items[64];
    n = dc2_scd_take_items(dec, dec_size, dir, items, 64);
    for (int i = 0; i < n; i++)
        add_script_zone(ov, items[i].quad, DC2_ZONE_ITEM, items[i].item, items[i].slot);

    /* Script: enemy spawns (0x1A) with a literal position */
    Dc2Spawn spawns[64];
    n = dc2_scd_spawns(dec, dec_size, dir, spawns, 64);
    for (int i = 0; i < n && ov.n_items < 32; i++) {
        const Dc2Spawn& s = spawns[i];
        if (!s.x.literal() || !s.y.literal() || !s.z.literal()) continue;
        RdtItemSpawn& e = ov.items[ov.n_items++];
        memset(&e, 0, sizeof(e));
        e.slot  = s.slot.literal() ? (u8)s.slot.value : 0xFF;
        e.type  = s.type.literal() ? (u8)s.type.value : 0xFF;
        e.px = s.x.value; e.py = s.y.value; e.pz = s.z.value;
        e.rot_y = s.rot.literal() ? s.rot.value : 0;
    }

    /* Slot 1: markers */
    Dc2Marker marks[RDT_MAX_OBJECTS];
    n = dc2_room_markers(dec, dec_size, dir, marks, RDT_MAX_OBJECTS);
    for (int i = 0; i < n; i++) {
        RdtEnemySpawn& m = ov.enemies[ov.n_enemies++];
        memset(&m, 0, sizeof(m));
        m.type = (u8)marks[i].q;
        m.id   = (u8)i;
        m.px = marks[i].x; m.py = marks[i].y; m.pz = marks[i].z;
    }
    return true;
}

void dc2_room_outline_mesh(const RdtSceneOverlay& ov, Mesh& mesh)
{
    mesh.alloc(ov.n_poly_pts, ov.n_poly_pts);
    int n = 0;
    for (int i = 0; i < ov.n_polys; i++) {
        const RdtPoly& p = ov.polys[i];
        for (int k = 0; k < p.count; k++, n++) {
            const s16* pt = ov.poly_pts[p.first + k];
            mesh.verts[n].x = -(f32)pt[0];      /* viewer x is -x, as for the overlay */
            mesh.verts[n].z = (f32)pt[1];
            u32 next = (u32)(p.first + (k + 1) % p.count);
            MeshTri& t = mesh.tris[n];
            t.idx[0] = (u32)n; t.idx[1] = next; t.idx[2] = next;
            t.color = RGBA8(0, 204, 51, 255);
        }
    }
}
