/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  Dino Crisis Save Editor (implementation)
 *═══════════════════════════════════════════════════════════════════*/
#include "formats/save_editor.h"
#include <string.h>

/* ═══ ITEM TABLE ═══ */
const DcItem DC_ITEMS[] = {
    {0x00,"(Empty)","none",0},
    {0x01,"Shotgun","weapon",0},{0x02,"Shotgun Custom","weapon",0},
    {0x03,"SG + Stock","weapon",0},{0x04,"SG Custom+Stock","weapon",0},
    {0x05,"Handgun","weapon",0},{0x06,"HG + Sight","weapon",0},
    {0x07,"Handgun Custom","weapon",0},{0x08,"HG Custom+Sight","weapon",0},
    {0x09,"Grenade Gun","weapon",0},{0x0A,"GG Custom","weapon",0},
    {0x0B,"Shotgun Parts","upgrade",0},{0x0C,"Shotgun Stocks","upgrade",0},
    {0x0D,"Handgun Sights","upgrade",0},{0x0E,"Handgun Slides","upgrade",0},
    {0x0F,"GG Parts","upgrade",0},
    {0x10,"SG Bullets","ammo",10},{0x11,"Slag Bullets","ammo",10},
    {0x12,"An. Dart S","ammo",3},{0x13,"An. Dart M","ammo",3},
    {0x14,"An. Dart L","ammo",3},{0x15,"Poison Dart","ammo",3},
    {0x16,"9mm Parabellum","ammo",34},{0x17,"40S&W Bullets","ammo",30},
    {0x18,"Grenade Bullets","ammo",6},{0x19,"Heat Bullets","ammo",6},
    {0x1A,"Inf. Grenades","ammo",1},
    {0x1B,"Hemostat","heal",2},{0x1C,"Med Pak S","heal",2},
    {0x1D,"Med Pak M","heal",2},{0x1E,"Med Pak L","heal",2},
    {0x1F,"Resuscitation","heal",2},{0x20,"An. Aid","heal",1},
    {0x21,"Recovery Aid","heal",1},{0x22,"Intensifier","heal",1},
    {0x23,"Multiplier","heal",1},
    /* 0x2B..0x85: dinorand data/dc1/items.json allItems (from the
       residentevil123 forum item-ID table t3242); names shortened. */
    {0x2B,"Plug","key",0},{0x2C,"Locker Room Key","key",0},
    {0x2D,"Small Size Key","key",0},{0x2E,"Entrance Key","key",0},
    {0x2F,"BG Room B1 Key","key",0},{0x30,"BG Area Key","key",0},
    {0x31,"C.O. Area Key","key",0},{0x32,"Port Card Key","key",0},
    {0x34,"ID Card Colonel","key",0},{0x35,"ID Card Communicator","key",0},
    {0x36,"ID Card Researcher","key",0},{0x37,"Key Card Lv. D","key",0},
    {0x38,"Key Card Lv. C","key",0},{0x39,"Key Card Lv. B","key",0},
    {0x3A,"Key Card Lv. A","key",0},{0x3B,"ID Card Superintendent","key",0},
    {0x3C,"C.O. Pass Card","key",0},{0x3D,"Panel Key 1","key",0},
    {0x3E,"Panel Key 2","key",0},{0x3F,"Battery R (uncharged)","key",0},
    {0x40,"Battery R (charged)","key",0},{0x41,"F.C. Device (no print)","key",0},
    {0x42,"F.C. Device (print)","key",0},{0x43,"B1 Crane Card","key",0},
    {0x44,"Screwdriver","key",0},{0x45,"Antenna Key","key",0},
    {0x46,"B1 Key Chip","key",0},{0x47,"Key Card L","key",0},
    {0x48,"Key Card R","key",0},{0x49,"B3 Crane Card 1","key",0},
    {0x4A,"B3 Crane Card 2","key",0},{0x4B,"B3 Crane Card 3","key",0},
    {0x4C,"Stabilizer","key",0},{0x4D,"Initializer","key",0},
    {0x4E,"Protect P. 1-A","key",0},{0x4F,"Protect P. 2-A","key",0},
    {0x50,"Protect P. 1-B","key",0},{0x51,"Protect P. 2-B","key",0},
    {0x52,"Core Parts 1","key",0},{0x53,"Core Parts 2","key",0},
    {0x54,"Energy Tank","key",0},{0x55,"Nucleum","key",0},
    {0x56,"B2 Key Chip 1","key",0},{0x57,"B2 Key Chip 2","key",0},
    {0x58,"Pulse Receiver","key",0},{0x59,"Battery R (large)","key",0},
    {0x5A,"Battery W (large)","key",0},{0x5B,"Secret Disc (data)","key",0},
    {0x5D,"Researcher Memo (1281)","key",0},{0x5E,"B1 Key Chip (3695)","key",0},
    {0x5F,"B2 Key Chip 1 (no number)","key",0},{0x60,"B2 Key Chip 2 (0392)","key",0},
    {0x61,"Planning Disc","key",0},{0x62,"DDK Input Disc H","key",0},
    {0x63,"DDK Input Disc N","key",0},{0x64,"DDK Input Disc L","key",0},
    {0x65,"DDK Input Disc E","key",0},{0x66,"DDK Input Disc W","key",0},
    {0x67,"DDK Input Disc S","key",0},{0x68,"DDK Input Disc D","key",0},
    {0x69,"DDK Code Disc H","key",0},{0x6A,"DDK Code Disc N","key",0},
    {0x6B,"DDK Code Disc L","key",0},{0x6C,"DDK Code Disc E","key",0},
    {0x6D,"DDK Code Disc W","key",0},{0x6E,"DDK Code Disc S","key",0},
    {0x6F,"DDK Code Disc D","key",0},{0x78,"Regina Custom Part","upgrade",0},
    {0x79,"Regina Custom Part","upgrade",0},{0x7A,"Regina Custom Part","upgrade",0},
    {0x7B,"Regina Custom Part","upgrade",0},{0x7C,"Regina Custom Part","upgrade",0},
    {0x7D,"Regina Custom Part","upgrade",0},{0x7E,"Regina Custom Part","upgrade",0},
    {0x7F,"Regina Custom Part","upgrade",0},{0x80,"Regina Custom Part","upgrade",0},
    {0x81,"Regina Custom Part","upgrade",0},{0x82,"Regina Custom Part","upgrade",0},
    {0x83,"Regina Custom Part","upgrade",0},{0x84,"Regina Custom Part","upgrade",0},
    {0x85,"Regina Custom Part","upgrade",0},
};
const int DC_ITEM_COUNT = sizeof(DC_ITEMS)/sizeof(DC_ITEMS[0]);

const u8 DC_SUPPLY_IDS[] = {
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,
    0x1B,0x1C,0x1D,0x1E,0x1F,0x20,0x21,0x22,0x23,
    0x0B,0x0C,0x0D,0x0E,0x0F
};
const int DC_SUPPLY_COUNT = sizeof(DC_SUPPLY_IDS)/sizeof(DC_SUPPLY_IDS[0]);

/* ═══ ROOMS ═══ */
const char* DC_STAGE_NAMES[] = {"?","1F","2F","B1","Outdoors","B2","B3"};
const char* DC_DIFF_NAMES[] = {"Easy","Normal","Hard","Very Hard"};

const DcRoom DC_ROOMS[] = {
    {13,1,"Backyard of the Facility",65183,4038,1200,1957},
    {14,1,"Passageway to Backup Gen.",6600,0,1200,55076},
    {15,1,"Backup Generator Room 1F",61268,2048,1200,3553},
    {3,1,"Management Office",62788,2048,1200,2024},
    {2,1,"Mgmt. Office Hallway",1426,3094,1200,63345},
    {0,4,"Front Area of Entrance",62479,2048,1200,61405},
    {3,2,"Hall 2F",63973,1062,1200,8793},
    {2,2,"Chief's Room",1520,920,1200,3195},
    {7,1,"Main Entrance",2372,1680,1200,1958},
    {9,1,"Lecture Room",5757,4094,1200,597},
    {11,3,"Backup Generator Room B1",61374,2048,1200,3543},
    {8,3,"Medical Room",2636,0,1200,589},
    {11,1,"Office",61986,3032,1200,57749},
    {5,4,"Large Size Elevator",9192,824,1200,7781},
    {8,4,"Elevator Power Room",60445,1092,1200,3196},
    {12,3,"Carrying Out Room B1",64076,1024,1200,9278},
    {4,3,"Computer Room (B1)",59748,2048,1200,62547},
    {3,3,"Gas Experiment Room",65438,3072,1200,59828},
    {5,3,"Library Room",4958,3190,1200,58468},
    {0,3,"Experiment Simulation Room",453,2054,1200,59211},
    {0,2,"Comm. Antenna Room",62848,2048,1200,64524},
    {2,4,"Hangar",54379,1074,1200,6679},
    {10,4,"Underground Passageway",62709,2108,1200,65436},
    {0,6,"Carrying Out Room B3",9085,3136,1200,60548},
    {5,6,"General Weapons Storage",4713,4028,1200,12357},
    {2,5,"Experiment Room Hall (B2)",59575,2232,1200,58347},
    {4,6,"Rest Station (B3)",5595,0,1200,63164},
    {6,5,"Security Pass Room",61909,3072,582,60388},
    {7,5,"Parts Storage",9648,0,582,59483},
    {12,5,"Power Freq. Room",4956,0,582,58540},
    {11,5,"Third Energy Control Room",59727,2048,582,93},
    {14,5,"Dr. Kirk's Personal Lab",3656,3104,712,61268},
    {9,6,"Disembark. Immigration Off.",3728,34,712,4545},
};
const int DC_ROOM_COUNT = sizeof(DC_ROOMS)/sizeof(DC_ROOMS[0]);

/* ═══ FLAG-BIT TOGGLEABLE ITEMS ═══ */
const DcFlagItem DC_FLAG_ITEMS[] = {
    {"Grenade Gun","9 bits + gs+71>=2 + gs+95 bits","#FF6B6B",
     9, {7,7,7,9,15,15,15,49,50}, {2,3,4,2,2,3,4,2,4}, 2, 0x12, true},
    {"Shotgun + All Upgrades","Endgame story block. SG Parts, Stocks, HG Sights, Slides","#FF6B6B",
     11, {4,11,17,18,18,18,23,28,28,32,34}, {2,2,7,0,3,5,7,1,2,2,1}, 3, 0x93, true},
    {"ID Card","7 bits - required for many doors","#DB6BFF",
     7, {1,13,13,21,33,48,48}, {4,1,4,2,4,6,7}, 0, 0, false},
    {"F.C. Device","2 bits - fingerprint collector","#DB6BFF",
     2, {36,40}, {2,3}, 0, 0, false},
    {"Comm. ID Card","3 bits - communication systems","#DB6BFF",
     3, {11,14,36}, {6,7,6}, 0, 0, false},
    {"C.O. Pass Card","8 bits - C.O. area access","#DB6BFF",
     8, {16,18,19,22,25,25,26,34}, {2,6,7,6,2,6,3,5}, 0, 0, false},
    {"Key Card Lv. C","8 bits - level C security doors","#DB6BFF",
     8, {1,18,31,31,31,31,34,40}, {7,4,2,3,4,5,0,6}, 0, 0, false},
    {"Planning Disc","1 bit only - cleanest toggle","#DB6BFF",
     1, {0}, {4}, 0, 0, false},
    {"Pulse Receiver","2 bits - endgame item","#DB6BFF",
     2, {20,21}, {2,1}, 0, 0, false},
};
const int DC_FLAG_ITEM_COUNT = sizeof(DC_FLAG_ITEMS)/sizeof(DC_FLAG_ITEMS[0]);

/* ═══ PRESETS ═══ */
const DcPreset DC_PRESETS[] = {
    {1,"Game Start","HG","None",0,0x00,
     {0,0,0,0,0,6,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    {5,"Entrance Key + Panel Key 2 + DDK-H","HG","Entrance Key, Panel Key 2, DDK Input H",0,0x02,
     {0,8,0x4E,4,0x60,0xBE,0x0D,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,2,0,0x29,0,0,0,0,0,0,0,0,0,0x10,0,0,0,0,0,0,0,0,0,0,0,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    {9,"Key Card L + DDK-N","HG","Key Card L, DDK Input N, DDK Code N",0,0x02,
     {0,0x28,0x4E,0x34,0x60,0xBE,0x0D,0x82,0xED,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0x1B,0,0x29,0,0,0,0,0,0,0,3,0,0x30,0,0,0,0,0,0,0,0,0,0,0,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    {14,"ID Card + F.C. Device + DDK-E","HG","ID Card, F.C. Device, Key Card L, DDK Code E",0,0x02,
     {2,0x38,0x5F,0x71,0x6B,0xBE,0x0D,0x82,0xED,0,0,0,0,0x1A,0x6E,0,0,0,0,0,0,4,0,2,0,0,0,0,0,0,0x1B,0x42,0x29,0x10,0x10,0,4,0,0,0,0x0B,0,0x30,0,0,0,0,0,0xC0,0,0,0,0,0,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    {28,"Grenade Gun acquired","HG + GG","Comm. ID Card, ID Card, F.C. Device",2,0x12,
     {0x0A,0x3A,0x5F,0x71,0x6B,0xBE,0x8F,0x9E,0xED,4,0xC0,0xF9,1,0x36,0xEE,0x1E,0xC3,0,4,0x30,0,4,4,0x12,0,1,0,0x61,0,0,0xFB,0x43,0x29,0x50,0x10,0,0xF4,2,0,0,0x0B,0x10,0xB0,8,0,0,0,0,0xC0,4,0x10,0,0,0x40,0x18,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    {34,"B3 - Key Card Lv. C + DDK-W","HG + GG","Key Card Lv.C, C.O. Pass Card, DDK W",2,0x12,
     {0x0A,0xBA,0x5F,0x73,0x6B,0xBE,0x8F,0x9E,0xED,4,0xC0,0xF9,1,0x36,0xEE,0x7E,0xC7,3,0x54,0xB0,0x80,4,0x67,0x12,0,0x45,0x89,0x61,1,0,0xFB,0x7F,0x69,0x50,0x31,0,0xF4,0x16,0,0,0x4B,0x10,0xB0,0x0C,0,0,0,0,0xC0,4,0x10,2,0,0x40,0x18,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    {41,"All weapons + upgrades","SG + HG + GG + all","All key cards, Plugs, Small Size Key",3,0x93,
     {0x2A,0xBA,0x5F,0x73,0x67,0xBE,0x8F,0x9E,0xED,4,0xC0,0xFD,1,0x36,0xEE,0x7E,0xC7,0x83,0x7D,0xB0,0xC3,0x64,0xD7,0x93,0x73,0x45,0xD8,0x67,7,0,0xFB,0x7F,0x6D,0x50,0x33,0,0xF4,0x16,0x60,0,0x7B,0x10,0xB0,0x0F,0x2E,0x7B,0x4C,0x22,0xFE,0xFF,0x3C,0x32,1,0xC0,0xFF,0x37,0xFB,8,0x39,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x20,0x81,0x40,0,5,0xA1,0x81,0x18,0xF3}},
    {48,"Endgame - Pulse Receiver","SG + HG + GG + all","Pulse Receiver + all endgame items",3,0x93,
     {0x6A,0xBA,0x5F,0x73,0x67,0xBE,0x8F,0x9E,0xED,4,0xC0,0xFD,1,0x36,0xEE,0x7E,0xC7,0xFF,0x7F,0xB0,0xC7,0x67,0xD7,0x93,0x73,0x45,0xDC,0xE7,7,0,0xFB,0xBF,0x6D,0x50,0x73,0,0xF4,0x16,0x60,0,0x7B,0x10,0xB0,0x0F,0x2E,0x7B,0x4C,0x22,0xFE,0xFF,0x3C,0x32,1,0xC0,0xFF,0x37,0xFB,8,0x3F,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x20,0x81,0x40,0,5,0xA1,0x81,0x18,0xF3}},
};
const int DC_PRESET_COUNT = sizeof(DC_PRESETS)/sizeof(DC_PRESETS[0]);

/* ═══ CHECKSUM ═══ */
u32 dc_checksum(const u8* buf, int start, int len) {
    u32 r = 5;
    for (int i = 0; i < len; i++) r = (r + buf[start + i]);
    return r;
}

/* ═══ DETECT ═══ */
bool dc_save_detect(const u8* data, u32 size) {
    if (size != DC_SAVE_SIZE) return false;
    return (data[0] == 0x53 && data[1] == 0x43); /* "SC" magic */
}

/* ═══ PARSE ═══ */
static u16 r16(const u8* p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 r32(const u8* p) { return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }
static void w16(u8* p, u16 v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void w32(u8* p, u32 v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }

bool dc_save_parse(const u8* data, u32 size, DcSave& out) {
    memset(&out, 0, sizeof(out));
    if (!dc_save_detect(data, size)) return false;

    memcpy(out.raw, data, DC_SAVE_SIZE);
    out.valid = true;

    const u8* g = data + DC_GS_OFFSET;

    /* CK1 = checksum of full game state: gs+8 for (GS_SIZE - 8) bytes
       CK2 = checksum of game state header: gs+8 for 120 bytes
       CK1 is stored at gs+0 and CK2 at gs+4, so neither covers a checksum. */
    out.ck1_ok = (r32(g) == dc_checksum(data, DC_GS_OFFSET+8, DC_GS_SIZE-8));
    out.ck2_ok = (r32(g+4) == dc_checksum(data, DC_GS_OFFSET+8, 120));

    out.room = g[8]; out.stage = g[9];
    out.save_count = g[11];

    u32 raw_time = r32(g+12);
    out.hours   = raw_time / 216000;
    out.minutes = (raw_time % 216000) / 3600;
    out.seconds = ((raw_time % 216000) % 3600) / 60;

    out.difficulty = g[16] & 0x7F;
    out.arrange    = !!(g[16] & 0x80);
    out.continues  = g[18];
    out.weapon_mask = g[71];

    /* Inventory */
    for (int i = 0; i < 10; i++) {
        int o = 1044 + i*4;
        out.inv[i] = {g[o], g[o+1], g[o+2], g[o+3]};
    }

    /* Cabinets */
    for (int bl = 0; bl < 17; bl++) {
        int ba = 296 + bl*44;
        for (int s = 0; s < 10; s++) {
            int o = ba + 4 + s*4;
            out.cabs[bl][s] = {g[o], g[o+1], g[o+2], g[o+3]};
        }
    }

    /* Event flags */
    memcpy(out.ev_flags, g+72, 96);

    /* Position */
    out.px   = r16(g+1620);
    out.pz   = r16(g+1622);
    out.py   = r16(g+1624);
    out.prot = r16(g+1616);

    return true;
}

/* ═══ BUILD (write edits back to raw buffer + recalculate checksums) ═══ */
void dc_save_build(DcSave& s) {
    u8* g = s.raw + DC_GS_OFFSET;

    g[8] = s.room; g[9] = s.stage;
    g[68] = s.room; g[69] = s.stage; /* duplicate location */

    g[16] = (s.difficulty & 0x7F) | (s.arrange ? 0x80 : 0);
    g[18] = s.continues;
    g[71] = s.weapon_mask;

    u32 raw_time = s.hours * 216000 + s.minutes * 3600 + s.seconds * 60;
    w32(g+12, raw_time);

    /* Inventory */
    for (int i = 0; i < 10; i++) {
        int o = 1044 + i*4;
        g[o] = s.inv[i].id; g[o+1] = s.inv[i].qty;
        g[o+2] = s.inv[i].fl; g[o+3] = s.inv[i].pd;
    }

    /* Cabinets */
    for (int bl = 0; bl < 17; bl++) {
        int ba = 296 + bl*44;
        for (int sl = 0; sl < 10; sl++) {
            int o = ba + 4 + sl*4;
            g[o] = s.cabs[bl][sl].id; g[o+1] = s.cabs[bl][sl].qty;
            g[o+2] = s.cabs[bl][sl].fl; g[o+3] = s.cabs[bl][sl].pd;
        }
    }

    /* Event flags */
    memcpy(g+72, s.ev_flags, 96);

    /* Position */
    w16(g+1620, s.px); w16(g+1622, s.pz);
    w16(g+1624, s.py); w16(g+1616, s.prot);

    /* Display time in header (Shift-JIS digit encoding) */
    static const u16 D[] = {0x824F,0x8250,0x8251,0x8252,0x8253,0x8254,0x8255,0x8256,0x8257,0x8258};
    s.raw[52] = (u8)(D[s.hours/10] >> 8);   s.raw[53] = (u8)(D[s.hours/10] & 0xFF);
    s.raw[54] = (u8)(D[s.hours%10] >> 8);   s.raw[55] = (u8)(D[s.hours%10] & 0xFF);
    s.raw[58] = (u8)(D[s.minutes/10] >> 8); s.raw[59] = (u8)(D[s.minutes/10] & 0xFF);
    s.raw[60] = (u8)(D[s.minutes%10] >> 8); s.raw[61] = (u8)(D[s.minutes%10] & 0xFF);
    s.raw[64] = (u8)(D[s.seconds/10] >> 8); s.raw[65] = (u8)(D[s.seconds/10] & 0xFF);
    s.raw[66] = (u8)(D[s.seconds%10] >> 8); s.raw[67] = (u8)(D[s.seconds%10] & 0xFF);

    /* Recalculate checksums:
       CK2 first (covers gs+8..gs+127, 120 bytes)
       CK1 second (covers gs+8..end, DC_GS_SIZE-8 bytes; that takes in the 120
       bytes CK2 covers, but not CK2 itself, which is stored at gs+4) */
    w32(g+4, dc_checksum(s.raw, DC_GS_OFFSET+8, 120));
    w32(g, dc_checksum(s.raw, DC_GS_OFFSET+8, DC_GS_SIZE-8));
}

/* ═══ FLAG ITEM HELPERS ═══ */
bool dc_flag_item_on(const DcSave& s, int idx) {
    if (idx < 0 || idx >= DC_FLAG_ITEM_COUNT) return false;
    const DcFlagItem& fi = DC_FLAG_ITEMS[idx];
    for (int i = 0; i < fi.bit_count; i++) {
        if (!(s.ev_flags[fi.byte_idx[i]] & (1 << fi.bit_num[i])))
            return false;
    }
    return true;
}

void dc_flag_item_toggle(DcSave& s, int idx, bool on) {
    if (idx < 0 || idx >= DC_FLAG_ITEM_COUNT) return;
    const DcFlagItem& fi = DC_FLAG_ITEMS[idx];

    for (int i = 0; i < fi.bit_count; i++) {
        if (on) s.ev_flags[fi.byte_idx[i]] |= (1 << fi.bit_num[i]);
        else    s.ev_flags[fi.byte_idx[i]] &= ~(1 << fi.bit_num[i]);
    }

    if (fi.is_weapon) {
        if (on) {
            if (fi.wm_min > s.weapon_mask) s.weapon_mask = fi.wm_min;
            if (fi.ef95_val) s.ev_flags[23] = fi.ef95_val;
        } else {
            /* Grenade Gun off: weapon_mask back to 0 if it was 2 */
            if (fi.wm_min == 2 && s.weapon_mask == 2) s.weapon_mask = 0;
            /* Shotgun off: weapon_mask to min(2, current) */
            if (fi.wm_min == 3) {
                if (s.weapon_mask > 2) s.weapon_mask = 2;
                s.ev_flags[23] &= ~0x80;
            }
        }
    }
}

/* Item lookup helper */
const DcItem* dc_item_by_id(u8 id) {
    for (int i = 0; i < DC_ITEM_COUNT; i++)
        if (DC_ITEMS[i].id == id) return &DC_ITEMS[i];
    return &DC_ITEMS[0]; /* empty */
}
