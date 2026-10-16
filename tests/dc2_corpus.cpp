/* Dino Crisis 2 corpus census.  Reads every file of an install through the
   formats layer and prints one tab-separated line of facts per package,
   entry, room record and model, for tests/check_dc2_corpus.py to compare
   with dinorand's own readers.  Read-only; nothing from the game is kept.

   Usage: DINORAND_DC2_DIR=<install> ./dc2_corpus > corpus.tsv
   Without the variable it prints SKIPPED and exits 0.

   Build: g++ -std=c++11 -Iinclude -D_snprintf=snprintf tests/dc2_corpus.cpp
          src/formats/dat.cpp src/formats/dc2_*.cpp src/core/lzss.cpp -o dc2_corpus */
#include "formats/dat.h"
#include "formats/dc2.h"
#include "core/lzss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

/* ponytail: CRC-32 (zlib's, so the checker gets it from Python's stdlib)
   stands in for SHA-256 when comparing payloads; the checker records the
   SHA-256 of each input file itself. */
static unsigned long long crc(const u8* p, size_t n)
{
    static u32 table[256];
    if (!table[1])
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static bool ext_is(const char* name, const char* ext)
{
    const char* dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static void op(const char* name, const Dc2Operand& o)
{
    if (!o.present) printf("\t%s=-", name);
    else printf("\t%s=%d/%d", name, o.value, o.mode);
}

static void room(const char* name, const u8* d, size_t n, u32 base)
{
    Dc2RoomDir dir;
    if (!dc2_room_directory(d, n, base, dir)) return;
    printf("ROOM\t%s\t%u", name, (unsigned)n);
    for (int i = 0; i < DC2_ROOM_SLOTS; i++)
        if (dir.present[i]) printf("\t%u:%u", dir.off[i], dir.size[i]); else printf("\t-");
    printf("\n");

    static Dc2Marker marks[512];
    int nm = dc2_room_markers(d, n, dir, marks, 512);
    for (int i = 0; i < nm; i++)
        printf("S1\t%s\t%d\t%u\t%u\t%u\t%d\t%d\t%d\t%u\t%u\t%u\n", name, i, marks[i].t, marks[i].k,
               marks[i].id, marks[i].x, marks[i].y, marks[i].z, marks[i].flag, marks[i].p, marks[i].q);

    static Dc2Polygon polys[256];
    int n2 = 0, np = dc2_room_polygons(d, n, dir, polys, 256, &n2);
    printf("S2\t%s\t%d\t%d\t%d\n", name, dir.present[2] ? d[dir.off[2]] : 0, np, n2);
    for (int i = 0; i < np; i++) {
        const Dc2Polygon& p = polys[i];
        const u8* pts = d + p.pts_off;
        bool closed = memcmp(pts, pts + p.npoints * 4, 4) == 0;
        printf("S2R\t%s\t%d\t%u\t%u\t%u\t%u\t%u\t%d\t%08llx\n", name, i, p.flags, p.npoints, p.index,
               p.param, p.extra, closed ? 1 : 0, crc(pts, (p.npoints + 1) * 4));
    }

    static Dc2Zone zones[256];
    int nz = dc2_room_zones(d, n, dir, zones, 256);
    printf("S7\t%s\t%u\t%d\t%d\n", name, dir.present[7] ? dir.size[7] : 0,
           dir.present[7] && dir.size[7] >= 16 ? d[dir.off[7]] : 0, nz);
    for (int i = 0; i < nz; i++) {
        printf("S7R\t%s\t%d", name, i);
        for (int c = 0; c < 4; c++) printf("\t%d\t%d", zones[i].x[c], zones[i].z[c]);
        printf("\t%u\n", zones[i].data_off);
    }

    Dc2ScdStats st;
    dc2_scd_stats(d, n, dir, st);
    printf("SCR\t%s\t%d\t%d\t%d", name, st.routines, st.ops, st.invalid);
    for (int i = 0; i < st.invalid && i < 16; i++) printf("\t%u:%02x", st.invalid_off[i], d[st.invalid_off[i]]);
    printf("\n");

    static Dc2Door doors[256];
    int nd = dc2_scd_doors(d, n, dir, doors, 256);
    for (int i = 0; i < nd; i++) {
        printf("DOOR\t%s\t%u", name, doors[i].op_off);
        op("dest", doors[i].dest); op("lock", doors[i].lock); op("flag", doors[i].unlock_flag);
        op("flags", doors[i].flags); op("type", doors[i].door_type); op("slot", doors[i].slot);
        for (int k = 0; k < 8; k++) op("q", doors[i].quad[k]);
        printf("\n");
    }
    static Dc2Trigger trigs[256];
    int ntr = dc2_scd_triggers(d, n, dir, trigs, 256);
    for (int i = 0; i < ntr; i++) {
        printf("TRIG\t%s\t%u", name, trigs[i].op_off);
        op("routine", trigs[i].routine); op("thread", trigs[i].thread); op("slot", trigs[i].slot);
        for (int k = 0; k < 8; k++) op("q", trigs[i].quad[k]);
        printf("\n");
    }
    static Dc2TakeItem items[256];
    int ni = dc2_scd_take_items(d, n, dir, items, 256);
    for (int i = 0; i < ni; i++) {
        printf("ITEM\t%s\t%u", name, items[i].op_off);
        op("item", items[i].item); op("flag5", items[i].taken_flag); op("p3", items[i].p3);
        op("slot", items[i].slot);
        for (int k = 0; k < 8; k++) op("q", items[i].quad[k]);
        printf("\n");
    }
    static Dc2Spawn spawns[256];
    int ns = dc2_scd_spawns(d, n, dir, spawns, 256);
    for (int i = 0; i < ns; i++) {
        printf("SPAWN\t%s\t%u\t%d", name, spawns[i].op_off, spawns[i].n_pushes);
        op("slot", spawns[i].slot); op("type", spawns[i].type); op("model", spawns[i].model_base);
        op("x", spawns[i].x); op("y", spawns[i].y); op("z", spawns[i].z); op("rot", spawns[i].rot);
        op("gate", spawns[i].gate);
        printf("\n");
    }

    static Dc2Camera cams[256];
    int ncam = dc2_room_cameras(d, n, dir, cams, 256);
    for (int i = 0; i < ncam; i++) {
        const Dc2Camera& c = cams[i];
        printf("CAM\t%s\t%d\t%d\t%d\t%d\t%u\t%d\t%d\t%d\t%d\t%u\t%u\n", name, i, c.eye[0], c.eye[1], c.eye[2],
               c.proj, c.target[0], c.target[1], c.target[2], c.roll, c.dbs_off, c.dbs_size);
    }
    static Dc2CamZone cz[256];
    int ncz = dc2_room_cam_zones(d, n, dir, cz, 256);
    for (int i = 0; i < ncz; i++) {
        const Dc2CamZone& z = cz[i];
        printf("CZ\t%s\t%d\t%u\t%u\t%u\t%u\t%u\t%d\t%d", name, i, z.flags, z.camera, z.b5, z.floor, z.span,
               z.w8[0], z.w8[1]);
        for (int c = 0; c < 4; c++) printf("\t%d\t%d", z.x[c], z.z[c]);
        for (int c = 0; c < 4; c++) printf("\t%d\t%d", z.vx[c], z.vz[c]);
        printf("\n");
    }
    static Dc2SoundZone sz[256];
    int nsz = dc2_room_sound_zones(d, n, dir, sz, 256);
    for (int i = 0; i < nsz; i++) {
        printf("SZ\t%s\t%d\t%u\t%u\t%d\t%u", name, i, sz[i].b0, sz[i].on, sz[i].floor, sz[i].b3);
        for (int c = 0; c < 4; c++) printf("\t%d\t%d", sz[i].x[c], sz[i].z[c]);
        printf("\n");
    }

    RdtSceneOverlay* ov = new RdtSceneOverlay();
    dc2_room_overlay(d, n, base, *ov);
    printf("OVL\t%s\t%d\t%d\t%d\t%d\t%d\t%d\n", name, ov->n_polys, ov->n_poly_pts, ov->n_zones,
           ov->n_items, ov->n_enemies, ov->n_cam_threads);
    delete ov;
}

static void package(const char* dirpath, const char* name)
{
    char path[1024];
    /* A file dinorand has patched keeps its original beside it as
       <name>.bak; dinorand's readers take that one, and so does this */
    snprintf(path, sizeof(path), "%s/%s.bak", dirpath, name);
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(path, sizeof(path), "%s/%s", dirpath, name);
        f = fopen(path, "rb");
    }
    if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    u8* raw = (u8*)malloc(sz > 0 ? sz : 1);
    if (fread(raw, 1, sz, f) != (size_t)sz) { fclose(f); free(raw); return; }
    fclose(f);

    DatArchive a;
    if (!dat_parse_memory(raw, (size_t)sz, a)) {
        printf("PKG\t%s\t%ld\tnot-a-package\n", name, sz);
        free(raw);
        return;
    }
    size_t built_size = 0;
    u8* built = dat_build(a.entries, a.count, built_size, a.is_dc2);
    bool same = built && built_size == (size_t)sz && memcmp(built, raw, sz) == 0;
    size_t first_diff = 0;
    if (built && !same)
        while (first_diff < built_size && first_diff < (size_t)sz && built[first_diff] == raw[first_diff])
            first_diff++;
    printf("PKG\t%s\t%ld\t%d\t%s\t%s\t%lu\t%lu\t%d\n", name, sz, a.count, a.is_dc2 ? "dc2" : "dc1",
           same ? "rebuild-same" : "rebuild-differs", (unsigned long)built_size, (unsigned long)first_diff,
           a.n_packages);
    free(built);

    /* ST*.DBS: one background per package */
    for (int p = 0; ext_is(name, ".dbs") && p < a.n_packages; p++) {
        Dc2Background bg;
        if (!dc2_background(a, p, bg)) { printf("BG\t%s\t%d\tnot-a-background\n", name, p); continue; }
        const DatEntry* e = a.entries + bg.first;
        static Dc2MaskTile tiles[DC2_MAX_MASK_TILES];
        int nt = dc2_mask_tiles(e[0].data, e[0].size, tiles, DC2_MAX_MASK_TILES);
        Buffer cells;
        if (bg.kind == DC2_BG_MASKED) dc2_mask_cells(e[3], cells);
        else if (bg.kind == DC2_BG_PICTURE) dat_entry_payload(e[3], cells);
        printf("BG\t%s\t%d\t%u\t%d\t%d\t%u\t%08llx\t%d\t%d\t%d\t%lu\t%08llx\n", name, p, bg.offset,
               bg.first, bg.kind, e[1].size, crc(e[1].data, e[1].size), bg.w, bg.h, bg.n_tiles,
               (unsigned long)cells.size, crc(cells.data, cells.size));
        printf("BGT\t%s\t%d\t%d", name, p, nt);
        for (int i = 0; i < nt; i++) printf("\t%d,%d,%d", tiles[i].x, tiles[i].y, tiles[i].depth);
        printf("\n");
    }

    for (int i = 0; i < a.count; i++) {
        const DatEntry& e = a.entries[i];
        Buffer dec;
        bool lz = e.type == DAT_LZSS0 || e.type == DAT_LZSS1 || e.type == DAT_DC2_LZSS2;
        if (lz) dat_entry_payload(e, dec);
        printf("ENT\t%s\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%08x\t%08llx\t%lu\t%08llx\n", name, i,
               e.raw_type, e.size, e.offset, e.x, e.y, e.w, e.h, e.load_base,
               e.data ? crc(e.data, e.size) : 0ULL, (unsigned long)dec.size,
               lz ? crc(dec.data, dec.size) : 0ULL);

        if (e.type == DAT_LZSS0 && a.is_dc2 && dec.size) {
            Dc2ModelInfo mi;
            if (dc2_model_probe(dec.data, dec.size, e.load_base, mi)) {
                EmdModel m;
                bool ok = dc2_parse_model(dec.data, dec.size, e.load_base, m);
                int tex = -1, pal = -1;
                dc2_model_texture(a, mi.tpage, mi.clut, tex, pal);
                /* The palette stored after the texture, with no other
                   texture between: the one the atlas gives it */
                int own = -1;
                for (int j = tex + 1; tex >= 0 && j < a.count && own < 0; j++) {
                    u32 t = a.entries[j].type;
                    if (t == DAT_PALETTE) own = j;
                    else if (t == DAT_TEXTURE || t == DAT_LZSS1) break;
                }
                printf("MODEL\t%s\t%d\t%d\t%d\t%d\t%d\t%08x\t%04x\t%04x\t%d\t%d\t%d\t%d\t%d\t%d\n", name, i,
                       mi.parts, mi.verts, mi.tris, mi.quads, e.load_base, mi.tpage, mi.clut,
                       ok ? 1 : 0, m.mesh.vert_count, m.mesh.tri_count, tex, pal, own);
            }
            room(name, dec.data, dec.size, e.load_base);
        }
        if (e.type == DAT_DC2_MP3)
            printf("MP3\t%s\t%d\t%u\t%u\t%u\t%08llx\n", name, i, (unsigned)e.load_base,
                   (unsigned)rd_u32(raw + i * DAT_ENTRY_DC2 + 12), e.size, e.data ? crc(e.data, e.size) : 0ULL);
    }
    free(raw);
}

/* Speech/NNNN.dat is a plain RIFF/WAVE file; the viewer opens it by that
   signature (AudioPanel::load_wav).  Print what the loader goes by: PCM
   format tag, channels, rate, bits, and whether the "fmt " chunk is
   followed by a "data" chunk that fits in the file. */
static void voice(const char* dirpath, const char* name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dirpath, name);
    FILE* f = fopen(path, "rb");
    if (!f) return;
    u8 h[44];
    size_t got = fread(h, 1, sizeof(h), f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    bool riff = got == sizeof(h) && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4);
    bool fmt = riff && !memcmp(h + 12, "fmt ", 4) && rd_u32(h + 16) == 16;
    bool data = fmt && !memcmp(h + 36, "data", 4) && rd_u32(h + 40) > 0 &&
                44 + (unsigned long)rd_u32(h + 40) <= (unsigned long)size;
    printf("WAV\t%s\t%d\t%u\t%u\t%u\t%u\t%d\n", name, riff ? 1 : 0, fmt ? rd_u16(h + 20) : 0,
           fmt ? rd_u16(h + 22) : 0, fmt ? rd_u32(h + 24) : 0, fmt ? rd_u16(h + 34) : 0, data ? 1 : 0);
}

static int by_name(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* Names in a directory, sorted, so the output is stable from run to run */
static int list_dir(const char* path, char*** out)
{
    DIR* d = opendir(path);
    if (!d) return -1;
    int n = 0, cap = 1024;
    char** names = (char**)malloc(cap * sizeof(char*));
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) names = (char**)realloc(names, (cap *= 2) * sizeof(char*));
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(char*), by_name);
    *out = names;
    return n;
}

int main()
{
    const char* root = getenv("DINORAND_DC2_DIR");
    if (!root || !root[0]) { printf("SKIPPED\tDINORAND_DC2_DIR is not set\n"); return 0; }

    /* The game directory is the install itself or its rebirth/ folder
       (dinorand tools/tool_paths.py) */
    char game[900], data[1024], speech[1024];
    struct stat sb;
    snprintf(data, sizeof(data), "%s/rebirth/Data", root);
    snprintf(game, sizeof(game), "%s%s", root, stat(data, &sb) == 0 ? "/rebirth" : "");
    snprintf(data, sizeof(data), "%s/Data", game);
    snprintf(speech, sizeof(speech), "%s/Speech", game);

    char** names;
    int n = list_dir(data, &names);
    if (n < 0) { fprintf(stderr, "no Data directory under %s\n", root); return 1; }
    printf("DIR\t%s\n", data);
    for (int i = 0; i < n; i++)
        if (ext_is(names[i], ".dat") || ext_is(names[i], ".tex") || ext_is(names[i], ".dbs") ||
            ext_is(names[i], ".pxl") || ext_is(names[i], ".trg") || ext_is(names[i], ".bin"))
            package(data, names[i]);

    n = list_dir(speech, &names);
    for (int i = 0; i < n; i++)
        if (ext_is(names[i], ".dat")) voice(speech, names[i]);
    return 0;
}
