/* Unit tests for Dino Crisis 2 support.  Every input is built here in code;
   no game file is read.  Exits 0 when all checks pass.

   Build and run:
     g++ -std=c++11 -Iinclude -D_snprintf=snprintf tests/test_dc2.cpp \
         src/formats/dat.cpp src/formats/dc2_room.cpp src/formats/dc2_scd.cpp \
         src/formats/dc2_model.cpp src/core/lzss.cpp -o test_dc2 && ./test_dc2 */
#include "formats/dat.h"
#include "formats/dc2.h"
#include "core/lzss.h"
#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_failed = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

/*─── Fixture builders ───────────────────────────────────────────*/

static void put16(Buffer& b, size_t off, u16 v) { wr_u16(b.data + off, v); }
static void put32(Buffer& b, size_t off, u32 v) { wr_u32(b.data + off, v); }
static void add16(Buffer& b, u16 v) { u8 t[2]; wr_u16(t, v); b.append(t, 2); }
static void add32(Buffer& b, u32 v) { u8 t[4]; wr_u32(t, v); b.append(t, 4); }

struct PkgEntry { u32 type; const u8* data; u32 size; u16 x, y, w, h; };

/* A package as the game stores it: one header sector, then each payload
   padded to a sector.  DC2: 32-byte entries, filler slots of "dummy
   header    " plus 16 zeros, zero padding.  DC1: 16-byte entries, the
   filler back to back, and the filler as padding. */
static void make_package(Buffer& out, const PkgEntry* e, int n, bool dc2)
{
    static const char dummy[] = "dummy header    ";
    size_t stride = dc2 ? 32 : 16;
    out.clear();
    out.resize(DAT_SECTOR);
    for (int i = 0; i < n; i++) {
        size_t o = i * stride;
        put32(out, o, e[i].type); put32(out, o + 4, e[i].size);
        put16(out, o + 8, e[i].x); put16(out, o + 10, e[i].y);
        put16(out, o + 12, e[i].w); put16(out, o + 14, e[i].h);
    }
    for (size_t o = n * stride; o + stride <= DAT_SECTOR; o += stride)
        memcpy(out.data + o, dummy, 16);
    for (int i = 0; i < n; i++) {
        size_t start = out.size, aligned = dc_align_sector(e[i].size);
        if (e[i].size) out.append(e[i].data, e[i].size);
        out.resize(start + aligned);
        for (size_t p = start + e[i].size; !dc2 && p + 16 <= start + aligned; p += 16)
            memcpy(out.data + p, dummy, 16);
    }
}

static const u32 BASE = 0x005E0000;

/* A room blob: the 12-pointer directory, then the given sections in slot
   order.  A null section pointer leaves the slot null. */
static void make_room_blob(Buffer& out, const Buffer* sections[11])
{
    out.clear();
    out.resize(48);
    for (int i = 0; i < 11; i++) {
        if (!sections[i]) continue;
        put32(out, i * 4, BASE + (u32)out.size);
        out.append(sections[i]->data, sections[i]->size);
    }
    put32(out, 44, BASE + (u32)out.size);
}

/* A slot-5 section: 0x1C header bytes, a routine directory, the routines */
static void make_script(Buffer& out, const Buffer* routines, int n)
{
    out.clear();
    out.resize(0x1C + n * 4);
    for (int i = 0; i < n; i++) {
        put32(out, 0x1C + i * 4, (u32)(out.size - 0x1C));
        out.append(routines[i].data, routines[i].size);
    }
}

static void emit(Buffer& b, u8 op, u8 b1 = 0) { b.push(op); b.push(b1); }
static void push(Buffer& b, u8 mode, s16 value) { b.push(0x05); b.push(mode); add16(b, (u16)value); }

/* Pushes that give the block these literal words, block[0] first */
static void push_block(Buffer& b, const s16* block, int n)
{
    for (int i = n - 1; i >= 0; i--) push(b, 0, block[i]);
}

/* A DCM model: parts[] = {x, y, z, parent, n_tris, n_quads} with n_tris
   triangles and n_quads quads each over three vertices per unit of byte 7. */
struct PartSpec { s16 x, y, z; s8 parent; u8 vert_triples; u16 tris, quads; };

static void make_dcm(Buffer& out, const PartSpec* parts, int n_parts, u32 base)
{
    int nv = 0, nt = 0, nq = 0;
    for (int i = 0; i < n_parts; i++) { nv += parts[i].vert_triples * 3; nt += parts[i].tris; nq += parts[i].quads; }
    u32 vtx = 0x1C + n_parts * 0x14, nrm = vtx + nv * 8, tri = nrm + nv * 8, quad = tri + nt * 12;
    out.clear();
    out.resize(quad + nq * 16);
    put32(out, 0, base + vtx); put32(out, 4, base + nrm);
    put32(out, 8, base + tri); put32(out, 12, base + quad);
    put16(out, 0x10, (u16)nt); put16(out, 0x12, (u16)nq);
    put32(out, 0x14, (u32)n_parts);
    put16(out, 0x18, 0x009E);    /* tpage: page x 14, y 1, 8bpp */
    put16(out, 0x1A, 0x7FC0);    /* clut: x 0, y 511 */
    u32 t = tri, q = quad;
    for (int i = 0; i < n_parts; i++) {
        size_t o = 0x1C + i * 0x14;
        put16(out, o, (u16)parts[i].x); put16(out, o + 2, (u16)parts[i].y); put16(out, o + 4, (u16)parts[i].z);
        out.data[o + 6] = (u8)parts[i].parent;
        out.data[o + 7] = parts[i].vert_triples;
        put32(out, o + 8, base + t); put32(out, o + 12, base + q);
        put16(out, o + 0x10, parts[i].tris); put16(out, o + 0x12, parts[i].quads);
        t += parts[i].tris * 12; q += parts[i].quads * 16;
    }
    for (int i = 0; i < nv; i++) {
        put16(out, vtx + i * 8, (u16)(i * 10)); put16(out, vtx + i * 8 + 2, (u16)(i + 1));
        put16(out, vtx + i * 8 + 4, (u16)-i);
        put16(out, nrm + i * 8 + 2, 4096);
    }
    for (int i = 0; i < nt; i++) {
        u8* r = out.data + tri + i * 12;
        wr_u16(r, 0); wr_u16(r + 2, 1); wr_u16(r + 4, 2);
        for (int k = 0; k < 6; k++) r[6 + k] = (u8)(10 + k);
    }
    for (int i = 0; i < nq; i++) {
        u8* r = out.data + quad + i * 16;
        wr_u16(r, 0); wr_u16(r + 2, 1); wr_u16(r + 4, 2); wr_u16(r + 6, (u16)(nv - 1));
        for (int k = 0; k < 8; k++) r[8 + k] = (u8)(20 + k);
    }
}

/*─── Container ──────────────────────────────────────────────────*/

static void test_detection()
{
    u8 a[100], b[3000];
    for (int i = 0; i < 100; i++) a[i] = (u8)i;
    for (int i = 0; i < 3000; i++) b[i] = (u8)(i * 7);
    PkgEntry e[] = { { 5, a, 100, 0, 0x5E, 0, 0 }, { 0, b, 3000, 0x10, 0x20, 0, 0 } };

    Buffer dc2, dc1;
    make_package(dc2, e, 2, true);
    CHECK(dat_is_dc2(dc2.data, dc2.size));
    { DatArchive ar; CHECK(dat_parse_memory(dc2.data, dc2.size, ar)); CHECK(ar.is_dc2); CHECK(ar.count == 2); }

    /* DC1: types 7 and 0 are valid there too */
    PkgEntry e1[] = { { 7, a, 100, 0, 0x8010, 0, 0 }, { 0, b, 3000, 1, 2, 3, 4 } };
    make_package(dc1, e1, 2, false);
    CHECK(!dat_is_dc2(dc1.data, dc1.size));
    { DatArchive ar; CHECK(dat_parse_memory(dc1.data, dc1.size, ar)); CHECK(!ar.is_dc2); CHECK(ar.count == 2);
      CHECK(ar.entries[0].type == DAT_LZSS0); CHECK(ar.entries[0].raw_type == 7); CHECK(ar.entries[0].load_base == 0);
      CHECK(ar.entries[1].offset == 0x1000); CHECK(ar.entries[1].h == 4); }

    /* DC1 package whose second entry is an empty DATA entry: bytes 16-31
       are zero, as in DC2, but the filler slots are 16 bytes apart */
    PkgEntry e2[] = { { 7, a, 100, 0, 0x8010, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 }, { 0, b, 3000, 0, 0, 0, 0 } };
    make_package(dc1, e2, 3, false);
    CHECK(!dat_is_dc2(dc1.data, dc1.size));
    { DatArchive ar; CHECK(dat_parse_memory(dc1.data, dc1.size, ar)); CHECK(!ar.is_dc2); CHECK(ar.count == 3); }

    /* Shorter than a sector */
    { DatArchive ar; CHECK(!dat_parse_memory(dc2.data, DAT_SECTOR - 1, ar)); CHECK(!dat_is_dc2(dc2.data, DAT_SECTOR - 1)); }

    /* DC2's ITEM.DAT: 1310720 bytes whose first sector is all zeros.  Not
       a package, and not DC1's item sprite bank either. */
    { Buffer item; item.resize(1310720);
      DatArchive ar; CHECK(!dat_parse_memory(item.data, item.size, ar)); CHECK(!ar.is_item_bank);
      CHECK(!dat_is_item_bank(item.data, item.size)); }
}

static void test_types_and_offsets()
{
    static const u32 want[9] = { DAT_DATA, DAT_TEXTURE, DAT_PALETTE, DAT_DC2_SOUND, DAT_DC2_MP3,
                                 DAT_LZSS0, DAT_LZSS1, DAT_DC2_UNK7, DAT_DC2_LZSS2 };
    u8 payload[5000];
    memset(payload, 0xAB, sizeof(payload));
    PkgEntry e[9];
    for (int i = 0; i < 9; i++) { PkgEntry t = { (u32)i, payload, (u32)(i == 4 ? 2049 : 10 + i), 0x380, 0x100, 0x80, 0x100 }; e[i] = t; }
    Buffer pkg;
    make_package(pkg, e, 9, true);
    DatArchive ar;
    CHECK(dat_parse_memory(pkg.data, pkg.size, ar));
    CHECK(ar.count == 9);
    u32 expect_off = DAT_SECTOR;
    for (int i = 0; i < ar.count && i < 9; i++) {
        CHECK(ar.entries[i].type == want[i]);
        CHECK(ar.entries[i].raw_type == (u32)i);
        CHECK(ar.entries[i].offset == expect_off);
        CHECK(ar.entries[i].offset % DAT_SECTOR == 0);
        expect_off += dc_align_sector(e[i].size);
    }
    CHECK(ar.entries[5].offset - ar.entries[4].offset == 2 * DAT_SECTOR);
    CHECK(ar.tail_off == pkg.size);

    /* The rect of a texture (1), palette (2) or compressed texture (6) and
       the load address of anything else are the same eight bytes */
    CHECK(ar.entries[1].x == 0x380 && ar.entries[1].y == 0x100 && ar.entries[1].w == 0x80 && ar.entries[1].h == 0x100);
    CHECK(ar.entries[2].w == 0x80 && ar.entries[6].h == 0x100);
    CHECK(ar.entries[5].load_base == 0x01000380);
    for (int i = 0; i < 9; i++) CHECK(dat_type_name(want[i])[0] != '?');
    CHECK(strcmp(dat_type_ext(DAT_DC2_MP3), ".mp3") == 0);

    /* A type above 8 ends the table, and so does the filler */
    put32(pkg, 3 * 32, 9);
    { DatArchive a2; CHECK(dat_parse_memory(pkg.data, pkg.size, a2)); CHECK(a2.count == 3); }
    memcpy(pkg.data + 2 * 32, "dummy header    ", 16);
    { DatArchive a2; CHECK(dat_parse_memory(pkg.data, pkg.size, a2)); CHECK(a2.count == 2); }

    /* An entry that runs past the end of the file has no data, and the
       entry after it is still where the header puts it */
    PkgEntry t[] = { { 0, payload, 100, 0, 0, 0, 0 }, { 0, payload, 5000, 0, 0, 0, 0 }, { 0, payload, 7, 0, 0, 0, 0 } };
    make_package(pkg, t, 3, true);
    { DatArchive a2; CHECK(dat_parse_memory(pkg.data, DAT_SECTOR * 3, a2)); CHECK(a2.count == 3);
      CHECK(a2.entries[0].data != 0); CHECK(a2.entries[1].data == 0 && a2.entries[1].size == 0);
      CHECK(a2.entries[2].offset == DAT_SECTOR * 5); CHECK(a2.entries[2].data == 0); }
}

static void test_round_trip()
{
    u8 a[5000], b[70];
    for (int i = 0; i < 5000; i++) a[i] = (u8)(i * 13 + (i >> 8));
    for (int i = 0; i < 70; i++) b[i] = (u8)(200 - i);
    Buffer lz;
    CHECK(lzss_compress(a, sizeof(a), lz));
    PkgEntry e[] = { { 3, b, 70, 4, 0, 0, 0 }, { 1, a, 4096, 896, 256, 128, 16 }, { 2, b, 64, 0, 511, 32, 1 },
                     { 5, lz.data, (u32)lz.size, 0, 0x5E, 0, 0 } };
    Buffer pkg;
    make_package(pkg, e, 4, true);

    DatArchive ar;
    CHECK(dat_parse_memory(pkg.data, pkg.size, ar));
    size_t n = 0;
    u8* built = dat_build(ar.entries, ar.count, n, true);
    CHECK(built && n == pkg.size && memcmp(built, pkg.data, n) == 0);
    free(built);

    /* LZSS through a type-5 entry */
    Buffer dec;
    CHECK(dat_entry_payload(ar.entries[3], dec));
    CHECK(dec.size == sizeof(a) && memcmp(dec.data, a, sizeof(a)) == 0);

    /* Bytes after the last entry (ST*.DBS) survive a rebuild */
    Buffer tailed;
    tailed.append(pkg.data, pkg.size);
    for (int i = 0; i < 3000; i++) tailed.push((u8)(i ^ 0x5A));
    { DatArchive a2; CHECK(dat_parse_memory(tailed.data, tailed.size, a2)); CHECK(a2.tail_off == pkg.size);
      u8* t = dat_build(a2.entries, a2.count, n, true, tailed.data + a2.tail_off, tailed.size - a2.tail_off);
      CHECK(t && n == tailed.size && memcmp(t, tailed.data, n) == 0);
      free(t); }

    /* Replace the compressed entry, rebuild, parse again: still 32-byte
       entries, and the new bytes come back out */
    u8 repl[9000];
    for (int i = 0; i < 9000; i++) repl[i] = (u8)(i % 97);
    CHECK(dat_replace_entry(ar.entries[3], repl, sizeof(repl), true));
    built = dat_build(ar.entries, ar.count, n, true);
    CHECK(built != 0);
    if (built) {
        CHECK(dat_is_dc2(built, n));
        CHECK(rd_u32(built + 3 * 32) == 5 && rd_u32(built + 3 * 32 + 8) == 0x005E0000);
        CHECK(memcmp(built + 4 * 32, "dummy header    ", 16) == 0 && rd_u32(built + 4 * 32 + 16) == 0);
        DatArchive a2;
        CHECK(dat_parse_memory(built, n, a2));
        CHECK(a2.is_dc2 && a2.count == 4 && a2.entries[3].raw_type == 5);
        Buffer d2;
        CHECK(dat_entry_payload(a2.entries[3], d2) && d2.size == sizeof(repl) && memcmp(d2.data, repl, sizeof(repl)) == 0);
        CHECK(a2.entries[1].size == 4096 && memcmp(a2.entries[1].data, a, 4096) == 0);
        free(built);
    }

    /* A table that does not fit in the header sector is refused */
    DatEntry many[65];
    CHECK(dat_build(many, 65, n, true) == 0);
    built = dat_build(many, 64, n, true);
    CHECK(built != 0);
    free(built);

    /* DC1 output is unchanged: 16-byte entries, filler padding */
    PkgEntry d1[] = { { 7, lz.data, (u32)lz.size, 0, 0x8010, 0, 0 }, { 2, b, 64, 0, 480, 16, 2 } };
    make_package(pkg, d1, 2, false);
    { DatArchive a1; CHECK(dat_parse_memory(pkg.data, pkg.size, a1));
      u8* t = dat_build(a1.entries, a1.count, n);
      CHECK(t && n == pkg.size && memcmp(t, pkg.data, n) == 0);
      free(t); }
}

/*─── Room ───────────────────────────────────────────────────────*/

static void test_directory()
{
    Buffer s0, s1, s5;
    s0.resize(40, 1); s1.resize(24, 2); s5.resize(0x30, 0);
    const Buffer* sec[11] = { &s0, &s1, 0, 0, 0, &s5, 0, 0, 0, 0, 0 };
    Buffer blob;
    make_room_blob(blob, sec);

    Dc2RoomDir dir;
    CHECK(dc2_room_directory(blob.data, blob.size, BASE, dir));
    CHECK(dir.present[0] && dir.off[0] == 48 && dir.size[0] == 40);
    CHECK(dir.present[1] && dir.off[1] == 88 && dir.size[1] == 24);
    CHECK(!dir.present[2] && !dir.present[4] && !dir.present[7]);
    CHECK(dir.present[5] && dir.off[5] == 112 && dir.size[5] == 0x30);
    CHECK(dir.present[11] && dir.off[11] == blob.size && dir.size[11] == 0);
    CHECK(dc2_room_slot_name(2)[0] && !dc2_room_slot_name(12)[0]);

    /* Size is the gap to the next higher pointer, whatever the slot order */
    put32(blob, 0, BASE + 88); put32(blob, 4, BASE + 48);
    CHECK(dc2_room_directory(blob.data, blob.size, BASE, dir));
    CHECK(dir.off[0] == 88 && dir.size[0] == 24 && dir.off[1] == 48 && dir.size[1] == 40);

    /* Rejected: a pointer below the base, one past the blob, slot 11 not
       at the end, slot 11 null, a blob too short for the table */
    Buffer bad;
    bad.append(blob.data, blob.size); put32(bad, 8, BASE - 4);
    CHECK(!dc2_room_directory(bad.data, bad.size, BASE, dir));
    put32(bad, 8, BASE + (u32)bad.size + 1);
    CHECK(!dc2_room_directory(bad.data, bad.size, BASE, dir));
    put32(bad, 8, 0); put32(bad, 44, BASE + (u32)bad.size - 4);
    CHECK(!dc2_room_directory(bad.data, bad.size, BASE, dir));
    put32(bad, 44, 0);
    CHECK(!dc2_room_directory(bad.data, bad.size, BASE, dir));
    CHECK(!dc2_room_directory(blob.data, 47, BASE, dir));
    /* The wrong load address fails too */
    CHECK(!dc2_room_directory(blob.data, blob.size, 0x80100000u, dir));
}

/* Slot 2 polygon record: 4 header bytes, 4 unknown, n attribute words,
   n + 1 points */
static void add_polygon(Buffer& s, u8 flags, u8 index, int n, s16 x0)
{
    s.push(flags); s.push((u8)n); s.push(index); s.push(0);
    add32(s, 0xFF00);
    for (int i = 0; i < n; i++) add32(s, 1);
    for (int i = 0; i <= n; i++) { add16(s, (u16)(x0 + (i % n) * 100)); add16(s, (u16)(-(i % n) * 7)); }
}

static void test_slot2()
{
    Buffer s2;
    s2.resize(8 + 3 * 4);
    s2.data[0] = 3;
    u32 offs[3];
    offs[0] = (u32)s2.size; add_polygon(s2, 0x41, 0, 4, 1000);
    offs[1] = (u32)s2.size; add_polygon(s2, 0x01, 1, 25, -3000);
    offs[2] = (u32)s2.size; add_polygon(s2, 0x01, 2, 3, 50);
    for (int i = 0; i < 3; i++) put32(s2, 8 + i * 4, offs[i]);
    CHECK(offs[1] - offs[0] == 44);     /* a 4-point polygon is 44 bytes */

    const Buffer* sec[11] = { 0, 0, &s2, 0, 0, 0, 0, 0, 0, 0, 0 };
    Buffer blob;
    make_room_blob(blob, sec);
    Dc2RoomDir dir;
    CHECK(dc2_room_directory(blob.data, blob.size, BASE, dir));

    Dc2Polygon p[8];
    int second = -1;
    CHECK(dc2_room_polygons(blob.data, blob.size, dir, p, 8, &second) == 3);
    CHECK(second == 0);
    CHECK(p[0].flags == 0x41 && p[0].npoints == 4 && p[0].index == 0 && p[0].extra == 0);
    CHECK(p[1].npoints == 25 && p[1].index == 1 && p[2].npoints == 3);
    CHECK(rd_s16(blob.data + p[0].pts_off) == 1000 && rd_s16(blob.data + p[0].pts_off + 4) == 1100);
    CHECK(rd_s16(blob.data + p[0].pts_off + 6) == -7);
    CHECK(memcmp(blob.data + p[1].pts_off, blob.data + p[1].pts_off + 25 * 4, 4) == 0);   /* closed */
    CHECK(rd_s16(blob.data + p[1].pts_off) == -3000);

    /* Overlay: the polygons without their closing point, and the outline
       mesh the viewer frames (one zero-area triangle per edge) */
    {
        RdtSceneOverlay* ov = new RdtSceneOverlay();
        CHECK(dc2_room_overlay(blob.data, blob.size, BASE, *ov));
        CHECK(ov->n_polys == 3 && ov->n_poly_pts == 4 + 25 + 3);
        CHECK(ov->polys[1].first == 4 && ov->polys[1].count == 25 && ov->polys[0].flags == 0x41);
        CHECK(ov->poly_pts[0][0] == 1000 && ov->poly_pts[1][0] == 1100 && ov->poly_pts[1][1] == -7);
        Mesh outline;
        dc2_room_outline_mesh(*ov, outline);
        CHECK(outline.vert_count == 32 && outline.tri_count == 32);
        CHECK(outline.verts[1].x == -1100.0f && outline.verts[1].y == 0.0f && outline.verts[1].z == -7.0f);
        CHECK(outline.tris[0].idx[0] == 0 && outline.tris[0].idx[1] == 1 && outline.tris[0].idx[2] == 1);
        CHECK(outline.tris[3].idx[0] == 3 && outline.tris[3].idx[1] == 0);     /* last edge closes the loop */
        CHECK(outline.tris[28].idx[1] == 4 && outline.tris[31].idx[1] == 29);
        delete ov;
    }

    /* More records than the caller has room for: clamped, nothing past the array */
    Dc2Polygon two[3];
    memset(two, 0xEE, sizeof(two));
    CHECK(dc2_room_polygons(blob.data, blob.size, dir, two, 2) == 2);
    CHECK(two[2].flags == 0xEE && two[2].pts_off == 0xEEEEEEEEu);

    /* A second block: its records are counted, and the last polygon ends there */
    Buffer with2;
    with2.append(s2.data, s2.size);
    with2.data[1] = 2; put32(with2, 4, (u32)with2.size);
    with2.resize(with2.size + 40, 0x77);
    sec[2] = &with2;
    make_room_blob(blob, sec);
    CHECK(dc2_room_directory(blob.data, blob.size, BASE, dir));
    CHECK(dc2_room_polygons(blob.data, blob.size, dir, p, 8, &second) == 3);
    CHECK(second == 2 && p[2].extra == 0);

    /* Truncated: the last record claims more points than the slot holds */
    Buffer cut;
    cut.append(s2.data, s2.size - 8);
    sec[2] = &cut;
    make_room_blob(blob, sec);
    CHECK(dc2_room_directory(blob.data, blob.size, BASE, dir));
    CHECK(dc2_room_polygons(blob.data, blob.size, dir, p, 8) == 2);

    /* A count the offset table cannot hold, and a slot too small for a header */
    Buffer tiny;
    tiny.resize(8); tiny.data[0] = 200;
    sec[2] = &tiny;
    make_room_blob(blob, sec);
    CHECK(dc2_room_directory(blob.data, blob.size, BASE, dir));
    CHECK(dc2_room_polygons(blob.data, blob.size, dir, p, 8) == 0);
    tiny.resize(4);
    make_room_blob(blob, sec);
    CHECK(dc2_room_directory(blob.data, blob.size, BASE, dir));
    CHECK(dc2_room_polygons(blob.data, blob.size, dir, p, 8) == 0);
}

static void test_slot1_and_7()
{
    /* Slot 1: u16 count, u16, 20-byte records */
    Buffer s1;
    add16(s1, 2); add16(s1, 0);
    add16(s1, 1); add16(s1, 1000); add32(s1, 0x280); add16(s1, (u16)-1500); add16(s1, 0); add16(s1, 2200);
    add16(s1, 0x8000); add16(s1, 166); add16(s1, 2);
    s1.resize(s1.size + 20);
    put16(s1, 24 + 8, 77);

    /* Slot 7: 16-byte header, 32-byte records, then the zones' data */
    Buffer s7;
    s7.resize(16 + 2 * 32 + 8);
    s7.data[0] = 2;
    static const s16 corners[8] = { -100, 200, 300, 200, 300, -400, -100, -400 };
    for (int i = 0; i < 8; i++) put16(s7, 16 + i * 2, (u16)corners[i]);
    put16(s7, 16 + 16, 16 + 2 * 32);
    put16(s7, 48, 5); put16(s7, 48 + 16, 16 + 2 * 32 + 4);

    const Buffer* sec[11] = { 0, &s1, 0, 0, 0, 0, 0, &s7, 0, 0, 0 };
    Buffer blob;
    make_room_blob(blob, sec);
    Dc2RoomDir dir;
    CHECK(dc2_room_directory(blob.data, blob.size, BASE, dir));

    Dc2Marker m[4];
    CHECK(dc2_room_markers(blob.data, blob.size, dir, m, 4) == 2);
    CHECK(m[0].t == 1 && m[0].k == 1000 && m[0].id == 0x280);
    CHECK(m[0].x == -1500 && m[0].y == 0 && m[0].z == 2200);
    CHECK(m[0].flag == 0x8000 && m[0].p == 166 && m[0].q == 2);
    CHECK(m[1].x == 77);
    CHECK(dc2_room_markers(blob.data, blob.size, dir, m, 1) == 1);

    Dc2Zone z[4];
    CHECK(dc2_room_zones(blob.data, blob.size, dir, z, 4) == 2);
    CHECK(z[0].x[0] == -100 && z[0].z[0] == 200 && z[0].x[2] == 300 && z[0].z[2] == -400 && z[0].z[3] == -400);
    CHECK(z[0].data_off == 80 && z[1].x[0] == 5 && z[1].data_off == 84);
    CHECK(dc2_room_zones(blob.data, blob.size, dir, z, 1) == 1);

    /* A count the slot is too small for stops the marker list early */
    put16(blob, dir.off[1], 9);
    CHECK(dc2_room_markers(blob.data, blob.size, dir, m, 4) == 2);

    /* Slot 7 is only read as zones when the first zone's data starts
       right after the records, and when the records fit */
    put16(blob, dir.off[7] + 32, 99);
    CHECK(dc2_room_zones(blob.data, blob.size, dir, z, 4) == 0);
    put16(blob, dir.off[7] + 32, 80);
    blob.data[dir.off[7]] = 3;
    CHECK(dc2_room_zones(blob.data, blob.size, dir, z, 4) == 0);
    blob.data[dir.off[7]] = 0;
    CHECK(dc2_room_zones(blob.data, blob.size, dir, z, 4) == 0);
}

/*─── Script ─────────────────────────────────────────────────────*/

static void test_op_table()
{
    CHECK(dc2_scd_op_len(0x01) == 6 && dc2_scd_op_len(0x02) == 6 && dc2_scd_op_len(0x03) == 6);
    CHECK(dc2_scd_op_len(0x05) == 4 && dc2_scd_op_len(0x06) == 4 && dc2_scd_op_len(0x07) == 4 && dc2_scd_op_len(0x08) == 4);
    for (int op = 0x10; op <= 0x59; op++)
        CHECK(dc2_scd_op_len((u8)op) == (op == 0x19 ? 4 : 2));
    CHECK(dc2_scd_op_len(0x04) == 2);

    /* Unused slots outside the 2-byte range stop a walk but are opcodes */
    static const u8 stops[] = { 0x00, 0x09, 0x0A, 0x0F, 0x5A, 0x5F };
    for (size_t i = 0; i < sizeof(stops); i++) {
        CHECK(dc2_scd_op_len(stops[i]) == 0);
        CHECK(!dc2_scd_op_invalid(stops[i]));
        CHECK(strcmp(dc2_scd_op_name(stops[i]), "NOP") == 0);
    }
    /* 0x60 and up are not opcodes */
    for (int op = 0x60; op <= 0xFF; op++) {
        CHECK(dc2_scd_op_len((u8)op) == 0);
        CHECK(dc2_scd_op_invalid((u8)op));
    }
    for (int op = 0x01; op <= 0x59; op++) CHECK(!dc2_scd_op_invalid((u8)op));
    CHECK(strcmp(dc2_scd_op_name(0x30), "AOT_DOOR") == 0 && strcmp(dc2_scd_op_name(0x1A), "SPAWN") == 0);
    CHECK(strcmp(dc2_scd_op_name(0x34), "NOP") == 0 && dc2_scd_op_category(0x34) == 9);
    CHECK(strcmp(dc2_scd_op_name(0xFF), "INVALID") == 0);

    /* Operand modes: 1, 2 and 6 are references, everything else a literal */
    CHECK(dc2_scd_mode(1) == DC2_MODE_VAR && dc2_scd_mode(2) == DC2_MODE_STACK && dc2_scd_mode(6) == DC2_MODE_GLOBAL);
    for (int m = 0; m < 256; m++)
        if (m != 1 && m != 2 && m != 6) CHECK(dc2_scd_mode((u8)m) == DC2_MODE_LITERAL);
}

/* Blob with only a script section; returns its directory */
static Dc2RoomDir script_room(Buffer& blob, const Buffer* routines, int n)
{
    Buffer s5;
    make_script(s5, routines, n);
    const Buffer* sec[11] = { 0, 0, 0, 0, 0, &s5, 0, 0, 0, 0, 0 };
    make_room_blob(blob, sec);
    Dc2RoomDir dir;
    CHECK(dc2_room_directory(blob.data, blob.size, BASE, dir));
    return dir;
}

static void test_routines_and_walk()
{
    Buffer r[3];
    push(r[0], 0, 5); emit(r[0], 0x10); emit(r[0], 0x04);                    /* 3 ops */
    r[1].push(0x01); r[1].resize(6); emit(r[1], 0x19); r[1].resize(10);      /* 6-byte op, jump */
    emit(r[1], 0x20); emit(r[1], 0x04);                                      /* ...walk goes on: 4 ops */
    emit(r[2], 0x11); r[2].push(0xFF); emit(r[2], 0x12);                     /* stops at 0xFF: 2 ops */

    Buffer blob;
    Dc2RoomDir dir = script_room(blob, r, 3);
    Dc2Routine routs[DC2_MAX_ROUTINES];
    u32 order[DC2_MAX_ROUTINES];
    CHECK(dc2_scd_routines(blob.data, blob.size, dir, routs, DC2_MAX_ROUTINES, order) == 3);
    u32 table = dir.off[5] + 0x1C;
    CHECK(routs[0].start == table + 12 && routs[0].end == routs[1].start);
    CHECK(routs[1].start == table + 12 + r[0].size);
    CHECK(routs[2].end == dir.off[5] + dir.size[5]);
    CHECK(order[0] == routs[0].start);     /* entry 0 is the init routine */

    Dc2ScdStats st;
    CHECK(dc2_scd_stats(blob.data, blob.size, dir, st));
    CHECK(st.routines == 3 && st.ops == 3 + 4 + 2);
    CHECK(st.invalid == 1 && st.invalid_off[0] == routs[2].start + 2);

    /* The directory is not always ascending: swap entries 0 and 1 */
    u32 e0 = rd_u32(blob.data + table), e1 = rd_u32(blob.data + table + 4);
    put32(blob, table, e1); put32(blob, table + 4, e0);
    CHECK(dc2_scd_routines(blob.data, blob.size, dir, routs, DC2_MAX_ROUTINES, order) == 3);
    CHECK(routs[0].start == table + 12 && order[0] == routs[1].start && order[1] == routs[0].start);

    /* An entry that is not an offset inside the slot ends the directory */
    put32(blob, table + 4, dir.size[5]);
    CHECK(dc2_scd_routines(blob.data, blob.size, dir, routs, DC2_MAX_ROUTINES) == 1);
    put32(blob, table + 4, 0xFFFFFFF0u);
    CHECK(dc2_scd_routines(blob.data, blob.size, dir, routs, DC2_MAX_ROUTINES) == 1);

    /* A single-entry directory (offset 4) does not grow a second routine
       out of the first opcode word: "push 0" reads as the offset 5, but
       the table ends where the first routine starts (ST103) */
    Buffer one;
    push(one, 0, 0); emit(one, 0x04);
    dir = script_room(blob, &one, 1);
    CHECK(dc2_scd_routines(blob.data, blob.size, dir, routs, DC2_MAX_ROUTINES) == 1);
    CHECK(routs[0].start == dir.off[5] + 0x20);
    CHECK(dc2_scd_stats(blob.data, blob.size, dir, st) && st.ops == 2 && st.invalid == 0);

    /* An instruction cut off by the end of the blob is listed, not read past */
    Buffer cut;
    emit(cut, 0x10); cut.push(0x01); cut.push(0x00);
    dir = script_room(blob, &cut, 1);
    CHECK(dc2_scd_stats(blob.data, blob.size, dir, st) && st.ops == 2);
    ScdDisasm dis; dis.init();
    CHECK(dc2_scd_disassemble(blob.data, blob.size, BASE, dis));
    CHECK(dis.dc2 && dis.n_lines == 4 && dis.lines[3].size == 2);
    dis.clear();
}

static void test_scanners()
{
    /* Door: block+0x00 unlock flag, +0x04 lock, +0x08 type, +0x0C
       destination, +0x20..+0x3C quad (z3 x3 .. z0 x0), +0x40, +0x44 flags,
       +0x48 slot: 19 pushes, as in every door of the game */
    s16 door[19] = { 12, 3, 0x0107, 0x0102, 0, 0, 0, 0,
                     -80, 70, -60, 50, -40, 30, -20, 10, 0, 0x80, 4 };
    /* Trigger: +0x00 routine, +0x04 thread, +0x08..+0x24 quad, +0x30 slot */
    s16 trig[13] = { 7, 2, 108, 107, 106, 105, 104, 103, 102, 101, 0, 0, 9 };
    /* Item: +0x04, +0x0C p3, +0x10 item id, +0x14 taken flag, +0x18..+0x34
       quad, +0x40 slot */
    s16 item[17] = { 0, 0xFF, 0, 1, 0x2E, 33, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 6 };
    /* Spawn: +0x00 gate, +0x0C model, +0x10 rotation, +0x14 z, +0x18 y,
       +0x1C x, +0x20 type, +0x24 slot */
    s16 spawn[10] = { 0, 0, 0, 0, 1024, -300, 0, 4500, 2, 3 };

    Buffer r[2];
    push_block(r[0], door, 19); emit(r[0], 0x30);
    push_block(r[0], trig, 13); emit(r[0], 0x31);
    emit(r[0], 0x04);
    push_block(r[1], item, 17); emit(r[1], 0x35);
    push_block(r[1], spawn, 10); emit(r[1], 0x1A);
    /* A second door whose destination comes off the stack (mode 2) and
       whose run of pushes is short */
    emit(r[1], 0x10);
    push(r[1], 0, 1); push(r[1], 2, 15); push(r[1], 0, 9); push(r[1], 0, 2); push(r[1], 0, 1);
    emit(r[1], 0x30);
    /* A spawn whose model and z are read from the global table (mode 6) */
    s16 spawn2[10] = { 0, 0, 0, 13, 0, 21, 0, 100, 0x10, 5 };
    for (int i = 9; i >= 0; i--) push(r[1], (i == 3 || i == 5) ? 6 : 0, spawn2[i]);
    emit(r[1], 0x1A);
    emit(r[1], 0x04);

    Buffer blob;
    Dc2RoomDir dir = script_room(blob, r, 2);
    Dc2ScdStats st;
    CHECK(dc2_scd_stats(blob.data, blob.size, dir, st) && st.invalid == 0 && st.routines == 2);

    Dc2Door d[4];
    CHECK(dc2_scd_doors(blob.data, blob.size, dir, d, 4) == 2);
    CHECK(d[0].dest.literal() && (u16)d[0].dest.value == 0x0102);    /* stage 1, room 2: ST102 */
    CHECK(d[0].lock.literal() && d[0].lock.value == 3);
    CHECK(d[0].unlock_flag.value == 12 && d[0].door_type.value == 0x0107);
    CHECK(d[0].flags.value == 0x80 && d[0].slot.value == 4);
    for (int k = 0; k < 8; k++) CHECK(d[0].quad[k].literal() && d[0].quad[k].value == (k % 2 ? -1 : 1) * (k + 1) * 10);
    CHECK(blob.data[d[0].op_off] == 0x30);
    CHECK(rd_s16(blob.data + d[0].dest.value_off) == 0x0102);        /* where an editor would write */
    /* Second door: destination present but computed; fields past the run missing */
    CHECK(d[1].dest.present && !d[1].dest.literal() && dc2_scd_mode(d[1].dest.mode) == DC2_MODE_STACK);
    CHECK(d[1].dest.value == 15);
    CHECK(d[1].lock.literal() && d[1].lock.value == 2);
    CHECK(!d[1].slot.present && !d[1].slot.literal() && !d[1].quad[0].present);

    Dc2Trigger t[4];
    CHECK(dc2_scd_triggers(blob.data, blob.size, dir, t, 4) == 1);
    CHECK(t[0].routine.value == 7 && t[0].thread.value == 2 && t[0].slot.value == 9);
    for (int k = 0; k < 8; k++) CHECK(t[0].quad[k].value == 101 + k);

    Dc2TakeItem it[4];
    CHECK(dc2_scd_take_items(blob.data, blob.size, dir, it, 4) == 1);
    CHECK(it[0].item.literal() && it[0].item.value == 0x2E && it[0].taken_flag.value == 33);
    CHECK(it[0].p3.value == 1 && it[0].slot.value == 6);
    for (int k = 0; k < 8; k++) CHECK(it[0].quad[k].value == 1 + k);

    Dc2Spawn sp[4];
    CHECK(dc2_scd_spawns(blob.data, blob.size, dir, sp, 4) == 2);
    CHECK(sp[0].n_pushes == 10 && sp[0].type.value == 2 && sp[0].slot.value == 3);
    CHECK(sp[0].x.value == 4500 && sp[0].y.value == 0 && sp[0].z.value == -300 && sp[0].rot.value == 1024);
    CHECK(sp[0].x.literal() && sp[0].z.literal());
    CHECK(sp[1].type.literal() && sp[1].type.value == 0x10);
    CHECK(sp[1].model_base.present && !sp[1].model_base.literal() && sp[1].model_base.value == 13);
    CHECK(!sp[1].z.literal() && dc2_scd_mode(sp[1].z.mode) == DC2_MODE_GLOBAL);

    /* Clamped to the caller's array */
    Dc2Door one[2];
    memset(one, 0xEE, sizeof(one));
    CHECK(dc2_scd_doors(blob.data, blob.size, dir, one, 1) == 1);
    CHECK(one[1].op_off == 0xEEEEEEEEu);

    /* Overlay: the literal door, the trigger and the item become zones;
       the computed door has no corners and is left out, and so is the
       spawn whose z is computed */
    RdtSceneOverlay* ov = new RdtSceneOverlay();
    CHECK(dc2_room_overlay(blob.data, blob.size, BASE, *ov));
    CHECK(ov->dc2 == 1 && ov->n_zones == 3 && ov->n_items == 1 && ov->n_polys == 0);
    CHECK(ov->zones[0].kind == RDT_ZONE_TRIGGER && ov->zones[0].target_index == DC2_ZONE_DOOR);
    CHECK(ov->zones[0].arg == 0x0102 && ov->zones[0].collision_group == 4);
    CHECK(ov->zones[0].x[0] == 10 && ov->zones[0].z[0] == -20 && ov->zones[0].x[3] == 70 && ov->zones[0].z[3] == -80);
    CHECK(ov->zones[1].target_index == DC2_ZONE_TRIGGER && ov->zones[1].arg == 7 && ov->zones[1].x[0] == 101);
    CHECK(ov->zones[2].target_index == DC2_ZONE_ITEM && ov->zones[2].arg == 0x2E);
    CHECK(ov->items[0].type == 2 && ov->items[0].slot == 3 && ov->items[0].px == 4500 && ov->items[0].pz == -300);
    delete ov;

    /* Listing: the door line says where it goes */
    ScdDisasm dis; dis.init();
    CHECK(dc2_scd_disassemble(blob.data, blob.size, BASE, dis));
    bool door_line = false, computed = false, push_line = false;
    for (int i = 0; i < dis.n_lines; i++) {
        if (strstr(dis.lines[i].comment, "door to ST102, lock 3")) door_line = true;
        if (strstr(dis.lines[i].comment, "door to (computed)")) computed = true;
        if (strstr(dis.lines[i].text, "PUSH") && strstr(dis.lines[i].text, "stk[15]")) push_line = true;
    }
    CHECK(door_line && computed && push_line && dis.n_threads == 2);
    dis.clear();

    /* Not a room: no listing, no overlay */
    u8 junk[64];
    memset(junk, 0x11, sizeof(junk));
    ov = new RdtSceneOverlay();
    CHECK(!dc2_scd_disassemble(junk, sizeof(junk), BASE, dis));
    CHECK(!dc2_room_overlay(junk, sizeof(junk), BASE, *ov));
    delete ov;
}

/*─── Model ──────────────────────────────────────────────────────*/

static void test_model()
{
    static const u32 MB = 0x00633000;
    PartSpec parts[] = { { 0, -900, 0, -1, 1, 1, 0 }, { 10, -200, 30, 0, 1, 1, 1 }, { -5, -100, 0, 1, 2, 0, 1 } };
    Buffer dcm;
    make_dcm(dcm, parts, 3, MB);

    Dc2ModelInfo mi;
    CHECK(dc2_model_probe(dcm.data, dcm.size, MB, mi));
    CHECK(mi.parts == 3 && mi.verts == 12 && mi.tris == 2 && mi.quads == 2);
    CHECK(mi.tpage == 0x009E && mi.clut == 0x7FC0);
    CHECK(rd_u32(dcm.data) - MB == 0x1C + 3 * 0x14);           /* vertices follow the parts */
    CHECK(rd_u32(dcm.data + 12) - rd_u32(dcm.data + 8) == 2 * 12);   /* 12-byte triangles */
    CHECK(dcm.size - (rd_u32(dcm.data + 12) - MB) == 2 * 16);  /* 16-byte quads */

    EmdModel m;
    CHECK(dc2_parse_model(dcm.data, dcm.size, MB, m));
    CHECK(m.valid && m.parts_count == 3 && m.skeleton.bone_count == 3);
    CHECK(m.mesh.tri_count == 2 + 2 * 2 && m.mesh.vert_count == 2 * 3 + 2 * 4);
    CHECK(m.pool_count == 12 && m.vert_src_count == m.mesh.vert_count && m.clip_count == 0);
    CHECK(m.parts_info[0].parent == -1 && m.parts_info[1].parent == 0 && m.parts_info[2].parent == 1);
    CHECK(m.skeleton.bones[0].parent == 0xFF && m.skeleton.bones[2].parent == 1);
    /* Bones accumulate down the chain; the viewer negates all three axes */
    CHECK(m.skeleton.abs_pos[2][0] == -5.0f && m.skeleton.abs_pos[2][1] == 1200.0f && m.skeleton.abs_pos[2][2] == -30.0f);
    /* Each part owns 3 x byte 7 vertices of the pool: 0-2, 3-5, 6-11 */
    CHECK(m.pool[2].bone_idx == 0 && m.pool[3].bone_idx == 1 && m.pool[5].bone_idx == 1);
    CHECK(m.pool[6].bone_idx == 2 && m.pool[11].bone_idx == 2);
    CHECK(m.skeleton.bones[2].vert_start == 6 && m.skeleton.bones[2].vert_count == 6);
    /* Part 0's triangle: vertex 1 is (10, 2, -1) relative to the root at y -900 */
    CHECK(m.mesh.verts[1].x == -10.0f && m.mesh.verts[1].y == 898.0f && m.mesh.verts[1].z == 1.0f);
    CHECK(m.mesh.verts[1].ny == -4096.0f);
    CHECK(m.vert_src[1].pool_idx == 1 && m.vert_src[1].dest_bone == 0 && !m.vert_src[1].is_mirror);
    CHECK(m.mesh.tris[0].idx[0] == 0 && m.mesh.tris[0].idx[2] == 2);
    CHECK(m.mesh.tris[0].uv[0][0] == 10 && m.mesh.tris[0].uv[2][1] == 15);
    CHECK(m.mesh.tris[0].tpage == 0x009E && m.mesh.tris[0].clut == 0x7FC0);
    /* Part 1's triangle uses the same vertices 0-2.  They belong to part 0,
       so part 0's bone still places them: the face's own part does not. */
    CHECK(m.vert_src[4].pool_idx == 1 && m.vert_src[4].dest_bone == 0 && m.mesh.verts[4].y == 898.0f);
    /* Part 1's quad (0, 1, 2, 11) as the strip (0,1,2) (1,3,2); vertex 11
       is part 2's, at (110, 12, -11) from a bone at (5, -1200, 30) */
    const MeshTri& q1 = m.mesh.tris[2];
    const MeshTri& q2 = m.mesh.tris[3];
    CHECK(q1.idx[0] == 6 && q1.idx[1] == 7 && q1.idx[2] == 8);
    CHECK(q2.idx[0] == 7 && q2.idx[1] == 9 && q2.idx[2] == 8);
    CHECK(q1.uv[0][0] == 20 && q2.uv[1][0] == 26 && q2.uv[1][1] == 27 && q2.uv[2][0] == 24);
    CHECK(m.vert_src[9].pool_idx == 11 && m.vert_src[9].dest_bone == 2);
    CHECK(m.mesh.verts[9].x == -115.0f && m.mesh.verts[9].y == 1188.0f && m.mesh.verts[9].z == -19.0f);

    /* The load address matters: the wrong one is not a model */
    CHECK(!dc2_model_probe(dcm.data, dcm.size, MB + 4, mi));
    CHECK(!dc2_model_probe(dcm.data, dcm.size, 0x80100000u, mi));

    /* Each rule, broken one at a time */
#define BROKEN(stmt) do { Buffer b; b.append(dcm.data, dcm.size); stmt; \
        EmdModel bad; CHECK(!dc2_model_probe(b.data, b.size, MB, mi)); \
        CHECK(!dc2_parse_model(b.data, b.size, MB, bad) && !bad.valid); } while (0)
    BROKEN(b.data[0x1C + 2 * 0x14 + 6] = 2);                 /* parent is the part itself */
    BROKEN(b.data[0x1C + 2 * 0x14 + 6] = 5);                 /* parent out of range */
    BROKEN(b.data[0x1C + 6] = 0);                            /* root with a parent */
    BROKEN(put32(b, 0x14, 0));                               /* no parts */
    BROKEN(put32(b, 0x14, 51));                              /* more parts than the viewer holds */
    BROKEN(put32(b, 4, rd_u32(b.data + 4) + 8));             /* vertex and normal counts differ */
    BROKEN(b.data[0x1C + 7] = 2);                            /* vertex count is not 3 x the parts' sum */
    BROKEN(put16(b, 0x10, 3));                               /* triangle total disagrees with the pool */
    BROKEN(put16(b, 0x1C + 0x10, 2));                        /* a part's triangle count */
    BROKEN(put32(b, 0x1C + 0x14 + 8, rd_u32(b.data + 0x1C + 0x14 + 8) + 12));  /* gap between parts */
    BROKEN(put16(b, rd_u32(b.data + 8) - MB + 2, 12));       /* triangle index past the pool */
    BROKEN(put16(b, rd_u32(b.data + 12) - MB + 6, 500));     /* quad index past the pool */
    BROKEN(put32(b, 0, MB - 8));                             /* pointer below the load address */
    BROKEN(b.resize(b.size - 1));                            /* quad pool cut short */
#undef BROKEN
    { EmdModel bad; CHECK(!dc2_parse_model(dcm.data, 20, MB, bad)); }

    /* Texture selection.  tpage 0x9E: page (896, 256), 8bpp.  clut 0x7FC0: (0, 511). */
    u8 px[16] = { 0 };
    PkgEntry e[] = { { 1, px, 16, 640, 0, 64, 136 }, { 2, px, 16, 160, 504, 48, 1 },
                     { 6, px, 16, 896, 256, 128, 256 }, { 2, px, 16, 0, 511, 256, 1 },
                     { 5, px, 16, 0, 0x63, 0, 0 } };
    Buffer pkg;
    make_package(pkg, e, 5, true);
    DatArchive ar;
    CHECK(dat_parse_memory(pkg.data, pkg.size, ar));
    int tex = 0, pal = 0;
    CHECK(dc2_model_texture(ar, 0x009E, 0x7FC0, tex, pal) && tex == 2 && pal == 3);
    /* tpage 0x8A: page (640, 0); clut 0x7E0A: (160, 504) */
    CHECK(dc2_model_texture(ar, 0x008A, 0x7E0A, tex, pal) && tex == 0 && pal == 1);
    /* A page no entry covers; a palette no entry covers */
    CHECK(!dc2_model_texture(ar, 0x0080, 0x7FC0, tex, pal) && tex == -1 && pal == 3);
    CHECK(!dc2_model_texture(ar, 0x009E, 0x0000, tex, pal) && tex == 2 && pal == -1);
    /* 16bpp needs no palette */
    CHECK(dc2_model_texture(ar, 0x011E, 0x0000, tex, pal) && tex == 2 && pal == -1);
}

int main()
{
    test_detection();
    test_types_and_offsets();
    test_round_trip();
    test_directory();
    test_slot2();
    test_slot1_and_7();
    test_op_table();
    test_routines_and_walk();
    test_scanners();
    test_model();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
