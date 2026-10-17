/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  Dino Crisis Save Editor
 *  Reverse-engineered from 48-save playthrough analysis.
 *  Format: PC (1999) SAVENO*.dns / .dno files (2432 bytes)
 *═══════════════════════════════════════════════════════════════════*/
#ifndef DC_FORMATS_SAVE_EDITOR_H
#define DC_FORMATS_SAVE_EDITOR_H

#include "core/types.h"

#define DC_SAVE_SIZE   2432
#define DC_GS_OFFSET   0x100   /* game state starts here; it is a straight memory
                                  image, so the room at gs+8 and the inventory at
                                  gs+1044 are one consecutive RAM region in the game */
#define DC_GS_SIZE     (DC_SAVE_SIZE - DC_GS_OFFSET)  /* 2176 */

/* ── Item definitions ── */
struct DcItem {
    u8          id;
    const char* name;
    const char* cat;    /* "weapon","ammo","heal","upgrade","key","none" */
    int         max_qty;
};

/* Full item table (0x00..0x85, with gaps) */
extern const DcItem DC_ITEMS[];
extern const int    DC_ITEM_COUNT;
const DcItem* dc_item_by_id(u8 id);  /* DC_ITEMS[0] when the id is not listed */

/* IDs that can go in inventory/cabinet slots */
extern const u8 DC_SUPPLY_IDS[];
extern const int DC_SUPPLY_COUNT;

/* ── Inventory / cabinet slot ── */
struct DcSlot {
    u8 id, qty, fl, pd;
};

/* ── Room definitions ── */
struct DcRoom {
    u8          rm, stg;
    const char* name;
    u16         x, z, y, rot;
};

extern const DcRoom DC_ROOMS[];
extern const int    DC_ROOM_COUNT;

extern const char*  DC_STAGE_NAMES[];  /* indexed 1..6 */
extern const char*  DC_DIFF_NAMES[];   /* 0..3 */

/* ── Parsed save data ── */
struct DcSave {
    u8  raw[DC_SAVE_SIZE]; /* full file */
    bool valid;

    /* Checksums */
    bool ck1_ok, ck2_ok;

    /* Header info */
    u8  room, stage;
    u8  difficulty;     /* 0..3 */
    bool arrange;       /* arrange mode flag */
    u8  continues;
    u8  save_count;
    int hours, minutes, seconds;

    /* Inventory: 10 slots */
    DcSlot inv[10];

    /* Mixing cabinets: 17 banks × 10 slots */
    DcSlot cabs[17][10];

    /* Event flags (gs+72..gs+167, 96 bytes) */
    u8  ev_flags[96];

    /* Weapon mask (gs+71) and ef95 (gs+95 = ev_flags[23]) */
    u8  weapon_mask;

    /* Player position */
    u16 px, pz, py, prot;
};

/* ── Game progress presets ── */
struct DcPreset {
    int         id;
    const char* name;
    const char* weapons;
    const char* keys;
    u8          wm;     /* weapon mask */
    u8          ef;     /* gs+95 value */
    u8          flags[96];
};

extern const DcPreset DC_PRESETS[];
extern const int      DC_PRESET_COUNT;

/* ── Flag-bit toggleable items ── */
struct DcFlagItem {
    const char* name;
    const char* desc;
    const char* color;    /* hex color for UI */
    int         bit_count;
    u8          byte_idx[12]; /* ev_flags byte index */
    u8          bit_num[12];  /* bit within that byte */
    u8          wm_min;       /* min weapon_mask when on */
    u8          ef95_val;     /* gs+95 value to set */
    bool        is_weapon;
};

extern const DcFlagItem DC_FLAG_ITEMS[];
extern const int        DC_FLAG_ITEM_COUNT;

/* ── Parse / Build ── */
bool dc_save_detect(const u8* data, u32 size);
bool dc_save_parse(const u8* data, u32 size, DcSave& out);
void dc_save_build(DcSave& save); /* writes edits back to save.raw[] + recalcs checksums */

/* ── Flag helpers ── */
bool dc_flag_item_on(const DcSave& save, int flag_idx);
void dc_flag_item_toggle(DcSave& save, int flag_idx, bool on);

/* ── Checksum ── */
u32 dc_checksum(const u8* buf, int start, int len);

#endif /* DC_FORMATS_SAVE_EDITOR_H */
