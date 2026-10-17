/* Read-only DC1 AOT corpus reporter.
   Usage: dc1_aot_corpus paths.txt > report.tsv
   Pass - to read one ST*.DAT path per line from stdin. */
#include "formats/dat.h"
#include "formats/mesh.h"
#include "formats/scd.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void trim(char* s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

static u32 read32(const u8* p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool same_name(const char* a, const char* b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return *a == *b;
}

static bool is_nonroom_archive(const char* path)
{
    const char* name = path;
    for (const char* p = path; *p; p++) if (*p == '/' || *p == '\\') name = p + 1;
    if (same_name(name, "STUFF.DAT")) return true;
    return strlen(name) == 7 && tolower((unsigned char)name[0]) == 's' &&
           tolower((unsigned char)name[1]) == 't' && name[2] >= '1' && name[2] <= '9' &&
           name[3] == '.' && tolower((unsigned char)name[4]) == 'd' &&
           tolower((unsigned char)name[5]) == 'a' && tolower((unsigned char)name[6]) == 't';
}

static int raw_aot_count(const u8* rdt, size_t size, u32 base)
{
    if (size < 28) return -1;
    u32 ptr[7];
    for (int i = 0; i < 7; i++) {
        u32 p = read32(rdt + i * 4);
        if (p < base || p - base >= size) return -1;
        ptr[i] = p - base;
    }
    size_t scd_size = size - ptr[5];
    for (int i = 0; i < 7; i++)
        if (ptr[i] > ptr[5] && ptr[i] - ptr[5] < scd_size) scd_size = ptr[i] - ptr[5];
    const u8* scd = rdt + ptr[5];
    if (scd_size < 4) return -1;
    u32 table_size = read32(scd);
    if (table_size < 4 || table_size > scd_size || (table_size & 3)) return -1;
    int n = (int)(table_size / 4), count = 0;
    if (n > 64) return -1;
    u32 starts[64];
    for (int i = 0; i < n; i++) {
        starts[i] = read32(scd + i * 4);
        if (starts[i] < table_size || starts[i] >= scd_size ||
            (i && starts[i] <= starts[i - 1])) return -1;
    }
    for (int i = 0; i < n; i++) {
        size_t pc = starts[i], end = scd_thread_end(scd, scd_size, starts, n, pc);
        while (pc < end) {
            int len = scd_inst_size(scd, scd_size, pc);
            if (len <= 0 || pc + (size_t)len > end) break;
            if (scd[pc] == 0x28) count++;
            pc += (size_t)len;
        }
    }
    return count;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: dc1_aot_corpus paths.txt\n");
        return 2;
    }
    FILE* manifest = strcmp(argv[1], "-") == 0 ? stdin : fopen(argv[1], "rb");
    if (!manifest) return 2;
    printf("path\traw_aots\tretained_aots\tzones\tmalformed\toverflow\ttypes\n");
    char path[1024];
    int inputs = 0, rooms = 0, skipped = 0, failures = 0, max_aots = 0;
    while (fgets(path, sizeof(path), manifest)) {
        trim(path);
        if (!path[0]) continue;
        inputs++;
        DatArchive archive;
        if (!dat_parse_file(path, archive)) {
            fprintf(stderr, "cannot parse %s\n", path);
            failures++;
            continue;
        }
        bool found = false;
        RdtSceneOverlay best;
        int raw_aots = -1;
        for (int i = 0; i < archive.count && !found; i++) {
            const DatEntry& e = archive.entries[i];
            if (e.type != DAT_LZSS0 && e.type != DAT_DATA) continue;
            Buffer payload;
            if (!dat_entry_payload(e, payload)) continue;
            u32 base = ((u32)(e.y & 0x7FFF) << 16) | (u32)e.x | 0x80000000u;
            RdtSceneOverlay overlay;
            if (parse_rdt_overlay(payload.data, payload.size, base, overlay)) {
                best = overlay;
                raw_aots = raw_aot_count(payload.data, payload.size, base);
                found = true;
            }
        }
        if (!found) {
            if (is_nonroom_archive(path)) skipped++;
            else {
                fprintf(stderr, "no RDT entry in %s\n", path);
                failures++;
            }
            continue;
        }
        if (raw_aots < 0 && best.n_aots_total == 0) raw_aots = 0;
        else if (raw_aots < 0) {
            fprintf(stderr, "cannot walk AOTs in %s\n", path);
            failures++;
        }
        int counts[12] = {}, malformed = 0;
        for (int i = 0; i < best.n_zones; i++) {
            const RdtFloorZone& z = best.zones[i];
            if (z.kind != RDT_ZONE_TRIGGER) continue;
            if (z.target_index < 12) counts[z.target_index]++;
            if (z.malformed) malformed++;
        }
        if (best.n_aots_total > max_aots) max_aots = best.n_aots_total;
        printf("%s\t%d\t%d\t%d\t%d\t%d\t", path, raw_aots, best.n_aots_total, best.n_zones,
               malformed, best.aot_overflow);
        bool comma = false;
        for (int i = 0; i < 12; i++) if (counts[i]) {
            printf("%s%d:%d", comma ? "," : "", i, counts[i]);
            comma = true;
        }
        printf("\n");
        rooms++;
    }
    if (manifest != stdin) fclose(manifest);
    fprintf(stderr, "inputs=%d rooms=%d skipped_nonrooms=%d failures=%d max_retained_aots=%d\n",
            inputs, rooms, skipped, failures, max_aots);
    return failures ? 1 : 0;
}
