/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  Dino Crisis .DAT Archive Format
 *═══════════════════════════════════════════════════════════════════*/
#ifndef DC_FORMATS_DAT_H
#define DC_FORMATS_DAT_H

#include "core/types.h"
#include <stdlib.h>

#define DAT_SECTOR 0x800

/*─── Entry types ────────────────────────────────────────────────*/
enum DatEntryType {
    DAT_DATA    = 0,  /* Room scripts, triggers, metadata     */
    DAT_TEXTURE = 1,  /* Raw 8bpp pixel data (stripped TIM)   */
    DAT_PALETTE = 2,  /* BGR555 CLUT (256 colors x h rows)    */
    DAT_SNDH    = 3,  /* Gian VAG sound header (PS1 audio)    */
    DAT_SNDB    = 4,  /* VAG sound body                       */
    DAT_SNDE    = 5,  /* SEQ music sequence (MIDI)            */
    DAT_UNK6    = 6,  /* Unknown                              */
    DAT_LZSS0   = 7,  /* LZSS compressed (models, bgs, room)  */
    DAT_LZSS1   = 8,  /* LZSS compressed 8bpp texture         */
    DAT_TEXTURE_LINEAR = 9,  /* Linear 8bpp texture (w/h are pixel dims, no VRAM deswizzle) */
    /* Dino Crisis 2 types with no DC1 equivalent (the number in the file is
       DatEntry::raw_type).  DC2's 0, 1, 2 mean the same as DC1's, and its 5
       and 6 are parsed as DAT_LZSS0 and DAT_LZSS1. */
    DAT_DC2_SOUND = 10,  /* DC2 type 3: sound bank (layout not decoded) */
    DAT_DC2_MP3   = 11,  /* DC2 type 4: MPEG-1 Layer III music stream  */
    DAT_DC2_UNK7  = 12,  /* DC2 type 7: unknown                        */
    DAT_DC2_LZSS2 = 13,  /* DC2 type 8: LZSS compressed                */
    DAT_TYPE_COUNT
};

/* Header entry size: 16 bytes in Dino Crisis, 32 in Dino Crisis 2 */
#define DAT_ENTRY_DC1 16
#define DAT_ENTRY_DC2 32

const char* dat_type_name(int type);
const char* dat_type_ext(int type);
const char* dat_type_desc(int type);

/*─── Archive entry ──────────────────────────────────────────────*/
struct DatEntry {
    u32  type;
    u32  size;
    u16  x, y, w, h;
    u32  offset;      /* offset within the .dat file */
    u8*  data;        /* pointer to entry data */
    u8   flags;       /* bit 0: skip deswizzle (raw linear) */
    bool owned;       /* true if data was malloc'd separately (replaced entry) */
    u32  raw_type;    /* type number as stored in the file (differs from type in DC2) */
    u32  load_base;   /* DC2: first reserve word, the address the entry loads at
                         (same bytes as x, y); 0 in DC1 */

    DatEntry()
        : type(0), size(0), x(0), y(0), w(0), h(0)
        , offset(0), data(0), flags(0), owned(false)
        , raw_type(0), load_base(0) {}
};

#define DAT_FLAG_RAW_LINEAR  0x01  /* render as raw linear, no deswizzle */

/*─── Archive container ──────────────────────────────────────────*/
struct DatArchive {
    DatEntry* entries;
    int       count;
    u8*       raw;        /* full file data (owned) */
    size_t    raw_size;
    Str256    filename;
    bool      is_item_bank;  /* true if parsed as item sprite bank */
    bool      modified;       /* true if any entry was replaced */
    bool      is_dc2;         /* Dino Crisis 2 package: 32-byte header entries */
    size_t    tail_off;       /* offset in raw of the bytes after the last entry;
                                 raw_size when there are none (DC2 ST*.DBS
                                 keeps its background images there) */

    DatArchive() : entries(0), count(0), raw(0), raw_size(0),
                   is_item_bank(false), modified(false),
                   is_dc2(false), tail_off(0) {}
    ~DatArchive() { free_data(); }

    void free_data() {
        if (entries) {
            for (int i = 0; i < count; i++)
                if (entries[i].owned) free(entries[i].data);
            delete[] entries; entries = 0;
        }
        free(raw);        raw = 0;
        count = 0;
        raw_size = 0;
        is_item_bank = false;
        modified = false;
        is_dc2 = false;
        tail_off = 0;
    }

private:
    DatArchive(const DatArchive&);
    DatArchive& operator=(const DatArchive&);
};

/*─── Parse / Build ──────────────────────────────────────────────*/

/* Parse a .dat file from disk.  Returns true on success.
   archive takes ownership of allocated memory. */
bool dat_parse_file(const char* path, DatArchive& archive);

/* Parse a .dat from memory buffer (buffer is NOT copied  -  caller
   must keep it alive as long as archive is used). */
bool dat_parse_memory(u8* raw, size_t raw_size, DatArchive& archive);

/* Entry contents as loaded in memory: LZSS entries (types 7, 8, and DC2's
   type 8) are decompressed, everything else is copied as-is. */
bool dat_entry_payload(const DatEntry& entry, Buffer& out);

/* True if the header sector is a Dino Crisis 2 one (32-byte entries). */
bool dat_is_dc2(const u8* raw, size_t raw_size);

/* Check if raw data is a Dino Crisis item sprite bank (item.dat). */
bool dat_is_item_bank(const u8* raw, size_t raw_size);

/* Item bank layout constants */
#define ITEM_BANK_BLOCK_CLUT   0x200    /* CLUT size (512 bytes) at start of each block */

/* Build .dat file bytes from entries.
   Returns allocated buffer and sets out_size.  Caller owns result.
   dc2 writes a Dino Crisis 2 package: 32-byte header entries holding each
   entry's raw_type, and zero padding.  tail is copied after the last
   entry (DatArchive::tail_off). */
u8* dat_build(const DatEntry* entries, int count, size_t& out_size,
              bool dc2 = false, const u8* tail = 0, size_t tail_size = 0);

/* Write rebuilt .dat to file. */
bool dat_write_file(const char* path, const DatEntry* entries, int count,
                    bool dc2 = false, const u8* tail = 0, size_t tail_size = 0);

/* Replace an entry's data.  If the entry type is LZSS (7, 8 or DC2's 8),
   the caller provides DECOMPRESSED data and this function compresses it.
   Otherwise, data is stored as-is.  The old data is freed if owned.
   Returns true on success. */
bool dat_replace_entry(DatEntry& entry, const u8* new_data, size_t new_size,
                       bool auto_compress);

/*─── Tpage / BPP detection ──────────────────────────────────────*/
struct TpageInfo {
    u16 tpage;
    u16 clut;
    int bpp;       /* 4, 8, or 16 */
    int tx;        /* page X in 64-halfword units */
    int vram_x;    /* tx * 64 */
    int vram_y;    /* 0 or 256 */
    int abr;       /* semi-transparency mode */
};

int parse_tpage_table(const u8* data, size_t size,
                      TpageInfo* out, int max_entries);

int guess_default_bpp(const DatArchive& archive);

#endif /* DC_FORMATS_DAT_H */
