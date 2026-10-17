/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  DAT Archive Format Implementation
 *═══════════════════════════════════════════════════════════════════*/
#include "formats/dat.h"
#include "core/lzss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*─── Type name tables ───────────────────────────────────────────*/
static const char* s_type_names[] = {
    "DATA", "TEXTURE", "PALETTE", "SNDH", "SNDB",
    "SNDE", "UNK6", "LZSS0", "LZSS1", "TEX_LIN",
    "SOUND", "MP3", "UNK7", "LZSS2"
};
static const char* s_type_exts[] = {
    ".bin", ".raw", ".pal", ".gian", ".vag",
    ".seq", ".bin", ".lz", ".lz", ".raw",
    ".bin", ".mp3", ".bin", ".lz"
};
static const char* s_type_descs[] = {
    "Room scripts, triggers, metadata",
    "Raw 8bpp pixel data (stripped TIM)",
    "BGR555 CLUT (256 colors x h rows)",
    "Gian VAG sound header (PS1 audio)",
    "VAG sound body",
    "Sequence-like sound data (not SEQ)",
    "Unknown",
    "LZSS compressed (models, backgrounds, room data)",
    "LZSS compressed 8bpp texture",
    "Linear 8bpp texture (item sprite bank)",
    "DC2 sound bank (layout not decoded)",
    "DC2 music stream (MPEG-1 Layer III)",
    "DC2 unknown (type 7)",
    "DC2 LZSS compressed (type 8)"
};

const char* dat_type_name(int type) {
    if (type >= 0 && type < DAT_TYPE_COUNT) return s_type_names[type];
    return "???";
}
const char* dat_type_ext(int type) {
    if (type >= 0 && type < DAT_TYPE_COUNT) return s_type_exts[type];
    return ".bin";
}
const char* dat_type_desc(int type) {
    if (type >= 0 && type < DAT_TYPE_COUNT) return s_type_descs[type];
    return "Unknown";
}

/* Forward declarations for item sprite bank */
static bool is_item_sprite_bank(const u8* raw, size_t raw_size);
static bool parse_item_bank(u8* raw, size_t raw_size, DatArchive& archive);

/*─── Parse .dat from file ───────────────────────────────────────*/
bool dat_parse_file(const char* path, DatArchive& archive)
{
    archive.free_data();

    FILE* f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < DAT_SECTOR) { fclose(f); return false; }

    u8* raw = (u8*)malloc(fsize);
    if (!raw) { fclose(f); return false; }
    if ((long)fread(raw, 1, fsize, f) != fsize) {
        free(raw); fclose(f); return false;
    }
    fclose(f);

    archive.raw = raw;
    archive.raw_size = (size_t)fsize;
    archive.filename.set(path);

    return dat_parse_memory(raw, (size_t)fsize, archive);
}

/*─── Dino Crisis 2 detection and type numbers ───────────────────*/

/* DC2 stores the same package with 32-byte header entries, so the second
   16 bytes of the file are the first entry's padding and are zero.  A DC1
   package can have zeros there too (an empty DATA entry); its unused
   header slots are then 16-byte "dummy header" fillers back to back,
   where DC2 follows each filler with 16 zero bytes.
   (dinorand FORMAT-DC2.md, K6; the zero probe is the SDK's rule.) */
bool dat_is_dc2(const u8* raw, size_t raw_size)
{
    static const u8 dummy_sig[] = "dummy header";
    if (raw_size < DAT_SECTOR) return false;
    for (int i = 16; i < 32; i++)
        if (raw[i]) return false;
    for (size_t off = 32; off + 32 <= DAT_SECTOR; off += 16)
        if (memcmp(raw + off, dummy_sig, 12) == 0 &&
            memcmp(raw + off + 16, dummy_sig, 12) == 0)
            return false;
    return true;
}

/* DC2 type number -> DatEntryType.  0, 1 and 2 are the same as DC1's; 5
   and 6 are DC1's LZSS0 and LZSS1 (same codec, K7). */
static const u32 s_dc2_types[9] = {
    DAT_DATA, DAT_TEXTURE, DAT_PALETTE, DAT_DC2_SOUND, DAT_DC2_MP3,
    DAT_LZSS0, DAT_LZSS1, DAT_DC2_UNK7, DAT_DC2_LZSS2
};

/*─── Parse .dat from memory ─────────────────────────────────────*/

/* Entries in the header sector at h.  A DC2 header also ends at a
   zero-size entry: ITEM.DAT's first sector is all zeros and is not a
   package. */
static int header_count(const u8* h, bool dc2)
{
    static const u8 dummy_sig[] = "dummy header";
    size_t stride = dc2 ? DAT_ENTRY_DC2 : DAT_ENTRY_DC1;
    int count = 0;
    for (size_t off = 0; off + stride <= DAT_SECTOR; off += stride, count++) {
        if (memcmp(h + off, dummy_sig, 12) == 0) break;
        if (rd_u32(h + off) > 8) break;
        if (dc2 && rd_u32(h + off + 4) == 0) break;
    }
    return count;
}

bool dat_parse_memory(u8* raw, size_t raw_size, DatArchive& archive)
{
    if (raw_size < DAT_SECTOR) return false;

    /* Check for item sprite bank format (item.dat / item2.dat) */
    if (is_item_sprite_bank(raw, raw_size))
        return parse_item_bank(raw, raw_size, archive);

    /* Dino Crisis 2 packages have 32-byte header entries; the layout is
       otherwise the same. */
    bool dc2 = dat_is_dc2(raw, raw_size);
    size_t stride = dc2 ? DAT_ENTRY_DC2 : DAT_ENTRY_DC1;

    /* Count the entries.  A DC2 file can be several packages end to end,
       each with its own header sector: ST*.DBS has one per camera of its
       room, and they tile all 89 files exactly (dinorand K142).  No other
       file of the game has anything after its first package. */
    int count = 0, packages = 0;
    for (size_t pos = 0; pos + DAT_SECTOR <= raw_size; ) {
        int n = header_count(raw + pos, dc2);
        if (n == 0) break;
        count += n;
        packages++;
        if (!dc2) break;
        size_t len = DAT_SECTOR;
        for (int i = 0; i < n; i++)
            len += dc_align_sector(rd_u32(raw + pos + i * stride + 4));
        if (len > raw_size - pos) break;
        pos += len;
    }

    if (count == 0) {
        archive.count = 0;
        archive.entries = 0;
        return false;  /* no entries = not a valid DAT archive */
    }

    archive.entries = new DatEntry[count];
    archive.count = count;
    archive.is_dc2 = dc2;
    archive.n_packages = packages;

    /* Parse each package's header entries and assign data pointers.  An
       entry that runs past the end of the file gets no data, and the
       entries after it keep their places. */
    size_t pos = 0;
    int i = 0;
    for (int p = 0; p < packages; p++) {
        int n = header_count(raw + pos, dc2);
        size_t data_off = pos + DAT_SECTOR;
        for (int k = 0; k < n; k++, i++) {
            const u8* h = raw + pos + k * stride;
            DatEntry& e = archive.entries[i];
            e.raw_type = rd_u32(h);
            e.type = dc2 ? s_dc2_types[e.raw_type] : e.raw_type;
            e.size = rd_u32(h + 4);
            e.x    = rd_u16(h + 8);
            e.y    = rd_u16(h + 10);
            e.w    = rd_u16(h + 12);
            e.h    = rd_u16(h + 14);
            if (dc2) e.load_base = rd_u32(h + 8);
            e.package = (u16)p;
            u32 stored = e.size;
            e.offset = (u32)data_off;
            if (data_off + e.size <= raw_size) {
                e.data = raw + data_off;
            } else {
                e.data = 0;
                e.size = 0;
            }
            data_off += dc_align_sector(stored);
        }
        pos = data_off;
    }

    return true;
}

/*─── Entry payload ─────────────────────────────────────────────*/
bool dat_entry_payload(const DatEntry& entry, Buffer& out)
{
    if (entry.type == DAT_LZSS0 || entry.type == DAT_LZSS1 ||
        entry.type == DAT_DC2_LZSS2)
        return lzss_decompress(entry.data, entry.size, out);
    out.clear();
    if (entry.data && entry.size > 0) out.append(entry.data, entry.size);
    return out.size > 0;
}

/*─── Build .dat file bytes ──────────────────────────────────────*/
u8* dat_build(const DatEntry* entries, int count, size_t& out_size, bool dc2)
{
    /* A header is one sector: 128 DC1 entries or 64 DC2 entries.  A new
       package starts where DatEntry::package changes (DC2 ST*.DBS). */
    size_t stride = dc2 ? DAT_ENTRY_DC2 : DAT_ENTRY_DC1;
    out_size = 0;
    if (count < 0) return 0;

    /* Calculate total size */
    size_t total = 0;
    for (int i = 0, run = 0; i < count; i++) {
        bool first = i == 0 || entries[i].package != entries[i - 1].package;
        run = first ? 1 : run + 1;
        if (run * stride > DAT_SECTOR) return 0;
        if (first) total += DAT_SECTOR;
        total += dc_align_sector(entries[i].size);
    }
    if (count == 0) total = DAT_SECTOR;

    u8* buf = (u8*)calloc(total, 1);
    if (!buf) return 0;

    static const u8 dummy[] = "dummy header    ";
    size_t pos = 0;
    for (int first = 0; first < count || first == 0; ) {
        int end = first;
        while (end < count && entries[end].package == entries[first].package) end++;

        /* Write header.  A DC2 entry is the same 16 bytes followed by 16
           zero bytes; the game reads the table at that stride, so a 16-byte
           table crashes it (dinorand DC2-ROOM-CONTAINER-STRIDE-CRASH-RCA.md). */
        size_t hoff = pos;
        for (int i = first; i < end; i++) {
            const DatEntry& e = entries[i];
            wr_u32(buf + hoff,      dc2 ? e.raw_type : e.type);
            wr_u32(buf + hoff + 4,  e.size);
            wr_u16(buf + hoff + 8,  e.x);
            wr_u16(buf + hoff + 10, e.y);
            wr_u16(buf + hoff + 12, e.w);
            wr_u16(buf + hoff + 14, e.h);
            hoff += stride;
        }
        /* Fill rest of header with dummy markers */
        while (hoff + stride <= pos + DAT_SECTOR) {
            memcpy(buf + hoff, dummy, 16);
            hoff += stride;
        }

        /* Write data */
        size_t doff = pos + DAT_SECTOR;
        for (int i = first; i < end; i++) {
            const DatEntry& e = entries[i];
            if (e.data && e.size > 0)
                memcpy(buf + doff, e.data, e.size);
            /* Pad to sector boundary: DC1 with the dummy pattern, DC2 with zeros */
            size_t aligned = dc_align_sector(e.size);
            size_t pad_start = doff + e.size;
            size_t pad_end   = doff + aligned;
            for (size_t p = pad_start; !dc2 && p + 16 <= pad_end; p += 16)
                memcpy(buf + p, dummy, 16);
            doff += aligned;
        }
        pos = doff;
        if (end == first) break;    /* count == 0: one empty header */
        first = end;
    }

    out_size = total;
    return buf;
}

/*─── Write .dat to file ─────────────────────────────────────────*/
bool dat_write_file(const char* path, const DatEntry* entries, int count, bool dc2)
{
    size_t size = 0;
    u8* buf = dat_build(entries, count, size, dc2);
    if (!buf) return false;

    FILE* f = fopen(path, "wb");
    if (!f) { free(buf); return false; }
    bool ok = fwrite(buf, 1, size, f) == size;
    fclose(f);
    free(buf);
    return ok;
}

/*─── Replace entry data ────────────────────────────────────────*/
bool dat_replace_entry(DatEntry& entry, const u8* new_data, size_t new_size,
                       bool auto_compress)
{
    u8* final_data = 0;
    size_t final_size = 0;

    if (auto_compress && (entry.type == DAT_LZSS0 || entry.type == DAT_LZSS1 ||
                          entry.type == DAT_DC2_LZSS2)) {
        /* LZSS compress: new_data is decompressed, store compressed */
        Buffer comp;
        if (!lzss_compress(new_data, new_size, comp))
            return false;
        final_data = (u8*)malloc(comp.size);
        if (!final_data) return false;
        memcpy(final_data, comp.data, comp.size);
        final_size = comp.size;
    } else {
        /* Store raw */
        final_data = (u8*)malloc(new_size);
        if (!final_data) return false;
        memcpy(final_data, new_data, new_size);
        final_size = new_size;
    }

    /* Free old owned data */
    if (entry.owned && entry.data)
        free(entry.data);

    entry.data = final_data;
    entry.size = (u32)final_size;
    entry.owned = true;
    return true;
}

/*─── Item Sprite Bank detection & parsing ──────────────────────*/

/* item.dat / item2.dat from Dino Crisis:
   Flat sprite bank, no sector header.  Layout:
     0x0000 - 0x1FFF   Header pixel block (64x128 @ 8bpp, fill indices)
     0x2000 - 0x2DFF   7 x PSX CLUTs (512 bytes each, 256-color BGR555)
     0x2E00 - 0x29FFF  First sprite strip (64x2504 @ 8bpp, uses shared CLUTs)
     0x2A000 - EOF-0x800  102 x fixed-size blocks @ 0x2800 (10240) bytes:
                           +0x000  CLUT (512 bytes, 256 x BGR555)
                           +0x200  Pixel data (9728 bytes = 64x152 @ 8bpp)
     Last 0x800 bytes: sector padding

   Detection: first 64 bytes are all 0xFE, file size ~= 0x129800 */

#define ITEM_BANK_HEADER_PIX   0x2000   /* 64x128 header pixels         */
#define ITEM_BANK_CLUT_OFF     0x2000   /* offset of shared CLUTs        */
#define ITEM_BANK_SHARED_CLUTS 7
#define ITEM_BANK_STRIP_OFF    0x2E00   /* first sprite strip start      */
#define ITEM_BANK_BLOCKS_OFF   0x2A000  /* fixed-size block array start  */
#define ITEM_BANK_BLOCK_SIZE   0x2800   /* 10240 bytes per block         */
#define ITEM_BANK_BLOCK_CLUT   0x200    /* CLUT size within block        */
#define ITEM_BANK_VRAM_W       64       /* VRAM halfwords = 128px @ 8bpp */

static bool is_item_sprite_bank(const u8* raw, size_t raw_size)
{
    if (raw_size < ITEM_BANK_BLOCKS_OFF + ITEM_BANK_BLOCK_SIZE)
        return false;

    /* First 64 bytes should all be 0xFE */
    for (int i = 0; i < 64; i++)
        if (raw[i] != 0xFE) return false;

    /* Verify at least one block fits after 0x2A000 */
    size_t blocks_space = raw_size - ITEM_BANK_BLOCKS_OFF;
    int num_blocks = (int)(blocks_space / ITEM_BANK_BLOCK_SIZE);
    if (num_blocks < 1) return false;

    /* Quick CLUT sanity: shared CLUT[0] color 0 should be 0x0000 (black/transparent) */
    u16 c0 = rd_u16(raw + ITEM_BANK_CLUT_OFF);
    if (c0 != 0x0000) return false;

    return true;
}

bool dat_is_item_bank(const u8* raw, size_t raw_size)
{
    return is_item_sprite_bank(raw, raw_size);
}

static bool parse_item_bank(u8* raw, size_t raw_size, DatArchive& archive)
{
    size_t blocks_space = raw_size - ITEM_BANK_BLOCKS_OFF;
    int num_blocks = (int)(blocks_space / ITEM_BANK_BLOCK_SIZE);
    if (num_blocks > 256) num_blocks = 256;

    archive.entries = new DatEntry[num_blocks];
    archive.count = num_blocks;
    archive.is_item_bank = true;

    int pix_per_block = ITEM_BANK_BLOCK_SIZE - ITEM_BANK_BLOCK_CLUT;
    int block_h = pix_per_block / (ITEM_BANK_VRAM_W * 2);  /* 9728 / 128 = 76 */

    for (int i = 0; i < num_blocks; i++) {
        size_t block_off = ITEM_BANK_BLOCKS_OFF + (size_t)i * ITEM_BANK_BLOCK_SIZE;

        DatEntry& e = archive.entries[i];
        e.type   = DAT_TEXTURE_LINEAR;
        e.size   = pix_per_block;
        e.w      = ITEM_BANK_VRAM_W;
        e.h      = (u16)block_h;
        e.x      = 0;
        e.y      = 0;
        e.offset = (u32)(block_off + ITEM_BANK_BLOCK_CLUT);
        e.data   = raw + block_off + ITEM_BANK_BLOCK_CLUT;
        e.flags  = 0;
    }

    return true;
}

/*─── Tpage parsing ──────────────────────────────────────────────*/
static void decode_tpage_val(u16 val, int& bpp, int& tx, int& ty, int& abr)
{
    int tp = (val >> 7) & 3;
    static const int bpp_tab[] = {4, 8, 16, 0};
    bpp = bpp_tab[tp];
    tx  = val & 0xF;
    ty  = (val >> 4) & 1;
    abr = (val >> 5) & 3;
}

int parse_tpage_table(const u8* data, size_t size,
                      TpageInfo* out, int max_entries)
{
    int n = 0;
    for (size_t i = 0; i + 3 < size && n < max_entries; i += 4) {
        u16 tpage = rd_u16(data + i);
        u16 clut  = rd_u16(data + i + 2);
        if (tpage == 0 && clut == 0) continue;

        int bpp, tx, ty, abr;
        decode_tpage_val(tpage, bpp, tx, ty, abr);
        if (bpp == 0) continue;

        out[n].tpage  = tpage;
        out[n].clut   = clut;
        out[n].bpp    = bpp;
        out[n].tx     = tx;
        out[n].vram_x = tx * 64;
        out[n].vram_y = ty * 256;
        out[n].abr    = abr;
        n++;
    }
    return n;
}

int guess_default_bpp(const DatArchive& archive)
{
    int bpp_counts[17]; /* index by bpp value */
    memset(bpp_counts, 0, sizeof(bpp_counts));

    TpageInfo infos[256];
    for (int i = 0; i < archive.count; i++) {
        const DatEntry& e = archive.entries[i];
        if (e.type != DAT_DATA || e.size < 4) continue;
        if (!(e.y & 0x8000)) continue;
        if (!e.data) continue;

        int n = parse_tpage_table(e.data, e.size, infos, 256);
        for (int j = 0; j < n; j++) {
            int b = infos[j].bpp;
            if (b >= 0 && b <= 16) bpp_counts[b]++;
        }
    }

    int best = 8, best_count = 0;
    for (int b = 0; b <= 16; b++) {
        if (bpp_counts[b] > best_count) {
            best_count = bpp_counts[b];
            best = b;
        }
    }
    return best;
}
