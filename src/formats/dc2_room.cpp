/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  Dino Crisis 2 Rooms
 *
 *  The 12-slot section directory of an ST*.DAT room blob, the
 *  sections whose records are decoded (1 markers, 2 collision
 *  polygons, 3 camera-switch zones, 6 sound zones, 7 zones, 9 cameras,
 *  10 background directory), the scene overlay built from them and
 *  from the room script, and the backgrounds of ST*.DBS.
 *
 *  Sources: dinorand ROOM-SECTION-TABLE.md (K20, K141-K152) and its
 *  tools label_sections.py, decode_s1_s7.py, decode_s2.py,
 *  decode_cameras.py and decode_dbs.py.
 *═══════════════════════════════════════════════════════════════════*/
#include "formats/dc2.h"
#include "core/lzss.h"
#include <math.h>
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
    /* Labels from ROOM-SECTION-TABLE.md.  Slot 4 has no established
       meaning (K137), and 1 and 7 no established purpose. */
    static const char* names[DC2_ROOM_SLOTS] = {
        "Draw mesh (not decoded)", "Markers", "Collision polygons",
        "Camera-switch zones", "Table (unknown)", "Script",
        "Sound zones", "Zones",
        "Prop geometry (not decoded)", "Cameras", "Background directory", "End"
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

/*─── Slot 9 and 10: cameras and their backgrounds ───────────────*/

/* Slot 9: a 16-byte header {u32 ?, u16 count, u16 0x1ADA, 8 bytes}, then
   one 16-byte record per camera.  The count equals the number of packages
   in the room's .DBS in all 89 rooms; 21 rooms have bytes after the
   records, which are not read.  Slot 10 has one {u32 sector, u32 bytes}
   per camera: its package in the .DBS starts at sector x 0x800, so camera
   i shows background i (K143, K145; ST101 has 6 cameras). */
int dc2_room_cameras(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                     Dc2Camera* out, int max_out)
{
    if (!dir.present[9] || dir.size[9] < 16) return 0;
    size_t o = dir.off[9], size = dir.size[9];
    if (o + size > dec_size || rd_u16(dec + o + 6) != 0x1ADA) return 0;
    int count = rd_u16(dec + o + 4), n = 0;
    size_t dbs = dir.present[10] && dir.off[10] + dir.size[10] <= dec_size ? dir.size[10] : 0;
    for (int i = 0; i < count && n < max_out; i++) {
        size_t r = 16 + 16 * (size_t)i;
        if (r + 16 > size) break;
        const u8* p = dec + o + r;
        Dc2Camera& c = out[n++];
        for (int k = 0; k < 3; k++) {
            c.eye[k]    = rd_s16(p + k * 2);
            c.target[k] = rd_s16(p + 8 + k * 2);
        }
        c.proj = rd_u16(p + 6);
        c.roll = rd_s16(p + 14);
        c.dbs_off = c.dbs_size = 0;
        if ((size_t)i * 8 + 8 <= dbs) {
            c.dbs_off  = rd_u32(dec + dir.off[10] + i * 8) * DAT_SECTOR;
            c.dbs_size = rd_u32(dec + dir.off[10] + i * 8 + 4);
        }
    }
    return n;
}

/* The game's projection, y down: forward is eye to target, right is
   down x forward, and the screen centre is (160, 120).  By eye it fits
   ST101's camera 0 and ST102's camera 2.  Roll turns the picture about the
   forward axis in units of 4096 a turn; it is 0 in all but 36 cameras.
   Its sign is taken from one picture: with this sign the floor line of
   ST504's camera 5 (roll 146) slopes with the painted floor, and with the
   other it slopes against it.  It has not been checked in the game. */
bool dc2_camera_project(const Dc2Camera& cam, const f32 p[3], f32 screen[2])
{
    f32 f[3], v[3];
    for (int k = 0; k < 3; k++) {
        f[k] = (f32)(cam.target[k] - cam.eye[k]);
        v[k] = p[k] - (f32)cam.eye[k];
    }
    f32 fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-3f) return false;
    for (int k = 0; k < 3; k++) f[k] /= fl;
    f32 r[3] = { f[2], 0, -f[0] };                  /* (0, 1, 0) x forward */
    f32 rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (rl < 1e-6f) { r[0] = 1; r[2] = 0; rl = 1; } /* looking straight up or down */
    r[0] /= rl; r[2] /= rl;
    f32 d[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    f32 z = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
    if (z < 1e-3f) return false;
    f32 x = (v[0] * r[0] + v[1] * r[1] + v[2] * r[2]) * cam.proj / z;
    f32 y = (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) * cam.proj / z;
    f32 a = cam.roll * (6.2831853f / 4096.0f);
    screen[0] = 160 + x * cosf(a) - y * sinf(a);
    screen[1] = 120 + x * sinf(a) + y * cosf(a);
    return true;
}

/*─── Slot 3: camera-switch zones ────────────────────────────────*/

/* u32 count, then 44-byte records.  The game walks them in order with the
   player's position; the first zone that holds the player, and whose floor
   range holds the player's floor, selects the camera (K144).  An actor's
   floor is -y / 1000 of the ground under it, so floor 8 is ST100's roof.
   The script can also force a camera (op 0x4A), which is how 412 of the
   485 cameras no zone names are reached. */
int dc2_room_cam_zones(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                       Dc2CamZone* out, int max_out)
{
    if (!dir.present[3] || dir.size[3] < 4) return 0;
    size_t o = dir.off[3], size = dir.size[3];
    if (o + size > dec_size) return 0;
    u32 count = rd_u32(dec + o);
    int n = 0;
    for (u32 i = 0; i < count && n < max_out; i++) {
        if (4 + 44 * ((size_t)i + 1) > size) break;
        const u8* r = dec + o + 4 + 44 * (size_t)i;
        Dc2CamZone& z = out[n++];
        z.flags = r[0]; z.camera = r[4]; z.b5 = r[5]; z.floor = r[6]; z.span = r[7];
        z.w8[0] = rd_s16(r + 8); z.w8[1] = rd_s16(r + 10);
        for (int c = 0; c < 4; c++) {
            z.x[c]  = rd_s16(r + 12 + c * 4); z.z[c]  = rd_s16(r + 14 + c * 4);
            z.vx[c] = rd_s16(r + 28 + c * 4); z.vz[c] = rd_s16(r + 30 + c * 4);
        }
    }
    return n;
}

/*─── Slot 6: sound zones ────────────────────────────────────────*/

/* u32 count, then 20-byte records, read by the sound-effect code (K146);
   they do not switch cameras.  ST40A has 16. */
int dc2_room_sound_zones(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                         Dc2SoundZone* out, int max_out)
{
    if (!dir.present[6] || dir.size[6] < 4) return 0;
    size_t o = dir.off[6], size = dir.size[6];
    if (o + size > dec_size) return 0;
    u32 count = rd_u32(dec + o);
    int n = 0;
    for (u32 i = 0; i < count && n < max_out; i++) {
        if (4 + 20 * ((size_t)i + 1) > size) break;
        const u8* r = dec + o + 4 + 20 * (size_t)i;
        Dc2SoundZone& z = out[n++];
        z.b0 = r[0]; z.on = r[1]; z.floor = (s8)r[2]; z.b3 = r[3];
        for (int c = 0; c < 4; c++) {
            z.x[c] = rd_s16(r + 4 + c * 4);
            z.z[c] = rd_s16(r + 6 + c * 4);
        }
    }
    return n;
}

/*─── Backgrounds (ST*.DBS) ──────────────────────────────────────*/

bool dc2_jpeg_size(const u8* j, size_t n, int& w, int& h)
{
    if (!j || n < 4 || j[0] != 0xFF || j[1] != 0xD8) return false;
    for (size_t i = 2; i + 9 < n; ) {
        if (j[i] != 0xFF) return false;
        if (j[i + 1] == 0xC0 || j[i + 1] == 0xC2) {     /* frame header */
            h = (j[i + 5] << 8) | j[i + 6];
            w = (j[i + 7] << 8) | j[i + 8];
            return true;
        }
        i += 2 + (((size_t)j[i + 2] << 8) | j[i + 3]);
    }
    return false;
}

/* The shapes in the 1169 packages of the game, by entry type: (0, 0) 361
   times, (0, 0, 2, 0) 779 and (0, 0, 2, 8) 29. */
bool dc2_background(const DatArchive& a, int package, Dc2Background& bg)
{
    memset(&bg, 0, sizeof(bg));
    int first = -1, n = 0;
    for (int i = 0; i < a.count; i++)
        if (a.entries[i].package == package) { if (first < 0) first = i; n++; }
    if (!a.is_dc2 || (n != 2 && n != 4)) return false;
    const DatEntry* e = a.entries + first;
    if (e[0].raw_type != 0 || e[1].raw_type != 0 || !e[0].data || e[0].size < 4) return false;
    if (!dc2_jpeg_size(e[1].data, e[1].size, bg.w, bg.h)) return false;
    if (n == 4) {
        if (e[2].raw_type != 2 || (e[3].raw_type != 0 && e[3].raw_type != 8)) return false;
        bg.kind = e[3].raw_type == 0 ? DC2_BG_MASKED : DC2_BG_PICTURE;
    }
    bg.first = first;
    bg.n_tiles = (int)rd_u32(e[0].data);
    bg.offset = e[0].offset - DAT_SECTOR;
    return true;
}

int dc2_background_at(const DatArchive& a, u32 offset)
{
    for (int i = 0; i < a.count; i++)
        if (a.entries[i].offset == offset + DAT_SECTOR &&
            (i == 0 || a.entries[i - 1].package != a.entries[i].package))
            return a.entries[i].package;
    return -1;
}

/* The game reads x = byte 0 << 4 and y = (byte 1 & 0xF) << 4 (reader
   0x4036F0 in dinorand's notes); what the high nibble of byte 1 is for is
   not known. */
int dc2_mask_tiles(const u8* t, size_t size, Dc2MaskTile* out, int max_out)
{
    if (!t || size < 4) return 0;
    u32 count = rd_u32(t);
    int n = 0;
    for (u32 i = 0; i < count && n < max_out; i++) {
        if (4 + 4 * ((size_t)i + 1) > size) break;
        const u8* r = t + 4 + 4 * (size_t)i;
        Dc2MaskTile& m = out[n++];
        m.x = (s16)(r[0] * 16);
        m.y = (s16)((r[1] & 0xF) * 16);
        m.depth = rd_s16(r + 2);
    }
    return n;
}

bool dc2_mask_cells(const DatEntry& entry, Buffer& cells)
{
    return entry.data && lzss_decompress(entry.data, entry.size, cells);
}

void dc2_mask_draw(const Dc2MaskTile* tiles, int n_tiles, const u8* cells, size_t cells_size,
                   u8* rgba, const u8* tint)
{
    const size_t cell = DC2_MASK_CELL * DC2_MASK_CELL * 2;
    for (int i = 0; i < n_tiles && ((size_t)i + 1) * cell <= cells_size; i++) {
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                int px = tiles[i].x + x, py = tiles[i].y + y;
                if (px < 0 || px >= DC2_BG_W || py < 0 || py >= DC2_BG_H) continue;
                u16 c = rd_u16(cells + (size_t)i * cell + ((y + 1) * DC2_MASK_CELL + x + 1) * 2);
                if (!(c & 0x8000)) continue;
                u8* o = rgba + ((size_t)py * DC2_BG_W + px) * 4;
                for (int k = 0; k < 3; k++) {
                    int v = (c >> (5 * k)) & 31;
                    v = (v << 3) | (v >> 2);
                    o[k] = (u8)(tint ? (v + tint[k]) / 2 : v);
                }
                o[3] = 255;
            }
        }
    }
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

    /* Slot 6: sound zones, listed with the floor zones */
    Dc2SoundZone snd[64];
    n = dc2_room_sound_zones(dec, dec_size, dir, snd, 64);
    for (int i = 0; i < n && ov.n_zones < 64; i++) {
        RdtFloorZone& z = ov.zones[ov.n_zones++];
        memset(&z, 0, sizeof(z));
        memcpy(z.x, snd[i].x, sizeof(z.x));
        memcpy(z.z, snd[i].z, sizeof(z.z));
        z.kind = RDT_ZONE_SOUND;
        z.target_index = (u8)i;
        z.collision_group = (u8)snd[i].floor;
        z.flags = snd[i].on;
    }

    /* Slots 9, 10 and 3: cameras.  One thread per camera-switch zone, in
       the order the game tests them, then one for each camera no zone
       names. */
    Dc2Camera cams[64];
    int n_cams = dc2_room_cameras(dec, dec_size, dir, cams, 64);
    Dc2CamZone cz[64];
    int n_cz = dc2_room_cam_zones(dec, dec_size, dir, cz, 64);
    bool named[64] = {};
    const int max_threads = (int)(sizeof(ov.cam_threads) / sizeof(ov.cam_threads[0]));
    for (int i = 0; i < n_cz + n_cams && ov.n_cam_threads < max_threads; i++) {
        int cam = i < n_cz ? cz[i].camera : i - n_cz;
        if (cam >= n_cams || (i >= n_cz && named[cam])) continue;
        named[cam] = true;
        const Dc2Camera& c = cams[cam];
        RdtSceneOverlay::CamThread& ct = ov.cam_threads[ov.n_cam_threads++];
        ct.type = 1;
        ct.cam_id = (u8)cam;
        ct.desc[0] = 1;                 /* a DC1 fixed shot, for rdt_camera_shot */
        ct.desc[2] = (u8)cam;
        for (int k = 0; k < 3; k++) {
            wr_u16(ct.desc + 0x2C + k * 2, (u16)c.target[k]);
            wr_u16(ct.desc + 0x34 + k * 2, (u16)c.eye[k]);
        }
        ct.proj = c.proj;
        ct.roll = c.roll;
        ct.dbs_off = c.dbs_off;
        ct.dbs_size = c.dbs_size;
        if (i < n_cz) {
            memcpy(ct.x, cz[i].x, sizeof(ct.x));
            memcpy(ct.z, cz[i].z, sizeof(ct.z));
            ct.floor = cz[i].floor;
            ct.floor_span = cz[i].span;
            ct.zflags = cz[i].flags;
        } else {
            ct.no_zone = 1;
        }
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
