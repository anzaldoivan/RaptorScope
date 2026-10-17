/*
 *  RaptorScope  -  SCD Script Disassembler (implementation)
 *  Dino Crisis (PC) bytecode disassembly.
 */
#include "formats/scd.h"
#include "formats/mesh.h"
#include "core/types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static u16 rd16(const u8* p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
static s16 rs16(const u8* p) { return (s16)rd16(p); }

/* Format hex bytes */
static void fmt_hex(char* buf, size_t bufsz, const u8* data, int len) {
    buf[0] = 0;
    int pos = 0;
    for (int i = 0; i < len && pos + 3 < (int)bufsz; i++) {
        pos += _snprintf(buf + pos, bufsz - pos, "%02X ", data[i]);
    }
    if (pos > 0 && buf[pos-1] == ' ') buf[pos-1] = 0;
}

/* Format a single operand field as hex */
static void fmt_operands(char* buf, size_t bufsz, u8 op, const u8* data, int len,
                         size_t inst_off) {
    buf[0] = 0;
    if (len <= 1) return;

    /* Special formatting for known opcodes */
    switch (op) {
    case 0x01: /* TASK_START: s8 task (-1 = allocate), s16 sub; handler 0x4A3103 */
        _snprintf(buf, bufsz, "task=%d, sub=%d", (s8)data[1], rs16(data + 2));
        return;
    case 0x02: /* EVT_NEXT */
        _snprintf(buf, bufsz, "frames=%d", rs16(data + 2));
        return;
    case 0x03: /* EVT_CHAIN */
        _snprintf(buf, bufsz, "thread=%d, target=0x%04X", data[1], rd16(data + 2));
        return;
    case 0x04: /* EVT_END */
        return; /* no operands worth showing */
    case 0x05: /* EVT_JUMP */
        _snprintf(buf, bufsz, "thread=%d", data[1]);
        return;
    case 0x07: /* GOTO */
        _snprintf(buf, bufsz, "target=%d", rs16(data + 2));
        return;
    case 0x08: /* THREAD_CLR */
        _snprintf(buf, bufsz, "slot=%d", data[1]);
        return;
    case 0x09: /* THREAD_CTRL */
        _snprintf(buf, bufsz, "slot=%d, cmd=%d, val=%d", data[1], rd16(data+2), rd16(data+4));
        return;
    case 0x0B: /* LOOP_SET */
        _snprintf(buf, bufsz, "count=%d", rs16(data + 2));
        return;
    case 0x0C: /* JUMP */
        _snprintf(buf, bufsz, "offset=%d → @0x%04X", rs16(data + 2),
                  (int)inst_off + (int)rs16(data + 2));
        return;
    case 0x0D: /* NOP_0D: the handler only advances pc by 4 */
        return;
    case 0x0E: /* JUMP_IF */
        _snprintf(buf, bufsz, "offset=%d → @0x%04X", rs16(data + 2),
                  (int)inst_off + (int)rs16(data + 2));
        return;
    case 0x16: /* CK_CMP */
        _snprintf(buf, bufsz, "var=%d, op=%d", data[1], data[2]);
        return;
    case 0x20: /* CHAR_SET */
        _snprintf(buf, bufsz, "id=%d, x=%d, y=%d, z=%d, dir=%d",
                  data[1], rs16(data+4), rs16(data+6), rs16(data+8), rs16(data+10));
        return;
    case 0x22: /* WORK_SET */
        _snprintf(buf, bufsz, "type=%d, id=%d", data[1], data[2]);
        return;
    case 0x23: { /* SEC_INST */
        u8 slot = data[1];
        u8 render = data[3];
        u32 sec_raw = rd32(data + 8);
        s16 px = rs16(data + 12);
        s16 py = rs16(data + 14);
        s16 pz = rs16(data + 16);
        s16 rx = rs16(data + 18);
        s16 ry = rs16(data + 20);
        s16 rz = rs16(data + 22);
        _snprintf(buf, bufsz, "slot=%d, render=%d, sec=0x%08X, pos=(%d,%d,%d), rot=(%d,%d,%d)",
                  slot, render, sec_raw, px, py, pz, rx, ry, rz);
        return;
    }
    case 0x28: { /* ZONE_DEF */
        u8 slot = data[1];
        char action[160];
        dc1_aot_format_record(data, len, action, sizeof(action));
        if (len >= 20)
            _snprintf(buf, bufsz, "slot=%d, %s, quad=(%d,%d)..(%d,%d)", slot, action,
                      rs16(data+4), rs16(data+6), rs16(data+16), rs16(data+18));
        else
            _snprintf(buf, bufsz, "slot=%d, %s", slot, action);
        return;
    }
    case 0x2E: /* CAM_SET: eye, target, transition of each (0 = cut) */
        _snprintf(buf, bufsz, "eye=(%d,%d,%d), tgt=(%d,%d,%d), eye_t=%d, tgt_t=%d",
                  rs16(data+2), rs16(data+4), rs16(data+6),
                  rs16(data+8), rs16(data+10), rs16(data+12),
                  rs16(data+16), rs16(data+18));
        return;
    case 0x35: /* POS_SET */
        _snprintf(buf, bufsz, "x=%d, y=%d, z=%d", rs16(data+2), rs16(data+4), rs16(data+6));
        return;
    case 0x2A: /* WORK_FIELD */
        _snprintf(buf, bufsz, "field=%d, value=%d", data[2], rs16(data+4));
        return;
    case 0x36: /* WORK_POS */
        _snprintf(buf, bufsz, "x=%d, y=%d, z=%d", rs16(data+2), rs16(data+4), rs16(data+6));
        return;
    case 0x37: /* WORK_ROT */
        _snprintf(buf, bufsz, "rx=%d, ry=%d, rz=%d", rs16(data+2), rs16(data+4), rs16(data+6));
        return;
    case 0x3A: /* BEHAV_SET: installs a behaviour on the bound character; not a
                  light (byte 1 is always 0, byte 2 is 1-15) */
        _snprintf(buf, bufsz, "behaviour=%d, 0x%04X, 0x%04X, 0x%04X",
                  data[2], (u16)rs16(data+4), (u16)rs16(data+6), (u16)rs16(data+8));
        return;
    case 0x42: /* OBJ_SET */
        _snprintf(buf, bufsz, "id=%d, type=%d, x=%d, y=%d, z=%d",
                  data[1], data[2], rs16(data+4), rs16(data+6), rs16(data+8));
        return;
    case 0x4C: { /* CAMERA: handler 0x428CD1 takes the eye at +2 and
                    the target at +18, each followed by its transition */
        s16 ex = rs16(data+2),  ey = rs16(data+4),  ez = rs16(data+6);
        s16 tx = rs16(data+18), ty = rs16(data+20), tz = rs16(data+22);
        _snprintf(buf, bufsz, "type=%d, eye=(%d,%d,%d), tgt=(%d,%d,%d), eye_t=%d, tgt_t=%d",
                  data[1], ex, ey, ez, tx, ty, tz, rs16(data+8), rs16(data+24));
        return;
    }
    case 0x59: /* CHAR_PLACE: +0x10 is the room's own index (DinoRand cont.104) */
        if (len >= 20) {
            _snprintf(buf, bufsz, "category=%d, model=0x%08X, motion=0x%08X, room=%d",
                      data[2], rd32(data+4), rd32(data+8), data[0x10]);
        }
        return;
    case 0x5B: { /* EM_SET: a dinosaur that was near the player in room +0x14
                    follows them in (handler 0x42A80D in the GOG exe; DinoRand
                    STATIC-SCD-RE cont.103) */
        if (len >= 44) {
            _snprintf(buf, bufsz, "slot=%d, type=%d, x=%d, y=%d, z=%d, model=0x%08X, from room %d, "
                      "chance=%d, distance=%d, %s, camera=%d, sub=%d, cam=(%d,%d,%d), tgt=(%d,%d,%d)",
                      data[1], data[2], rs16(data+4), rs16(data+6), rs16(data+8), rd32(data+12),
                      data[0x14], data[0x15], (u16)rs16(data+0x16), data[0x19] ? "plain" : "cinematic",
                      data[0x1A], data[0x1B], rs16(data+0x1C), rs16(data+0x1E), rs16(data+0x20),
                      rs16(data+0x24), rs16(data+0x26), rs16(data+0x28));
        }
        return;
    }
    }

    /* Generic: show raw bytes after opcode */
    int pos = 0;
    for (int i = 1; i < len && pos + 6 < (int)bufsz; i++) {
        if (i > 1) pos += _snprintf(buf + pos, bufsz - pos, ", ");
        pos += _snprintf(buf + pos, bufsz - pos, "0x%02X", data[i]);
    }
}

bool scd_disassemble(const u8* dec, size_t dec_size, u32 base_addr,
                     ScdDisasm& out)
{
    out.clear();
    if (dec_size < 32) return false;

    /* Find ptr[5] = SCD section */
    u32 ptrs[7];
    for (int i = 0; i < 7; i++) {
        ptrs[i] = rd32(dec + i * 4);
        if (ptrs[i] < base_addr) return false;
        ptrs[i] -= base_addr;
        if (ptrs[i] >= dec_size) return false;
    }
    size_t scd_off = ptrs[5];

    /* Determine SCD section end (next section or dec_size) */
    size_t scd_end = dec_size;
    for (int i = 0; i < 7; i++) {
        if (ptrs[i] > scd_off && ptrs[i] < scd_end)
            scd_end = ptrs[i];
    }

    const u8* scd = dec + scd_off;
    size_t scd_size = scd_end - scd_off;

    /* Parse thread offset table: array of u32 relative offsets.  The first
       thread starts right after the table, so the first offset / 4 is the
       thread count (reading on would take code bytes as offsets). */
    if (scd_size < 4) return false;
    u32 table_size = rd32(scd);
    if (table_size < 4 || table_size > scd_size || (table_size & 3) != 0) return false;
    int n_threads = 0;
    for (int i = 0; i < 64 && (u32)i * 4 < table_size; i++) {
        u32 toff = rd32(scd + i * 4);
        if (toff < table_size || toff >= scd_size) break;
        out.thread_offsets[i] = toff;
        n_threads++;
    }
    if (n_threads == 0) return false;
    out.n_threads = n_threads;

    /* Add header: thread offset table */
    {
        ScdLine& h = out.add();
        h.offset = 0;
        h.thread = 0xFF;
        h.opcode = 0xFF;
        _snprintf(h.text, sizeof(h.text), "; === SCD Script (%d threads, %d bytes) ===",
                  n_threads, (int)scd_size);
    }
    for (int i = 0; i < n_threads; i++) {
        ScdLine& h = out.add();
        h.offset = (u32)(i * 4);
        h.thread = 0xFF;
        h.opcode = 0xFF;
        _snprintf(h.text, sizeof(h.text), ";   Thread %d: offset 0x%04X",
                  i, out.thread_offsets[i]);
    }
    {
        ScdLine& h = out.add();
        h.thread = 0xFF;
        _snprintf(h.text, sizeof(h.text), "");
    }

    /* First pass: collect all branch targets to generate labels */
    bool* is_target = (bool*)calloc(scd_size, 1);

    size_t code_end = 0;
    for (int ti = 0; ti < n_threads; ti++) {
        size_t pc = out.thread_offsets[ti];
        size_t end = scd_thread_end(scd, scd_size, out.thread_offsets, out.n_threads, out.thread_offsets[ti]);
        if (end > code_end) code_end = end;
        while (pc < end) {
            int sz = scd_inst_size(scd, scd_size, pc);
            if (sz == 0) break;
            u8 op = scd[pc];
            /* Track branch targets */
            if ((op == 0x0C || op == 0x0D || op == 0x0E) && pc + 4 <= scd_size) {
                s16 rel = rs16(scd + pc + 2);
                int tgt = (int)pc + (int)rel;
                if (tgt >= 0 && tgt < (int)scd_size)
                    is_target[tgt] = true;
            }
            pc += sz;
        }
    }

    /* Second pass: disassemble each thread */
    for (int ti = 0; ti < n_threads; ti++) {
        /* Thread header */
        {
            ScdLine& h = out.add();
            h.thread = (u8)ti;
            h.offset = out.thread_offsets[ti];
            _snprintf(h.text, sizeof(h.text), "; ─── Thread %d @ 0x%04X ───",
                      ti, out.thread_offsets[ti]);
        }

        size_t pc = out.thread_offsets[ti];
        size_t end = scd_thread_end(scd, scd_size, out.thread_offsets, out.n_threads, out.thread_offsets[ti]);
        int indent = 0;

        while (pc < end) {
            /* Insert label if this is a branch target */
            if (is_target[pc]) {
                ScdLine& lbl = out.add();
                lbl.offset = (u32)pc;
                lbl.thread = (u8)ti;
                lbl.is_label = true;
                _snprintf(lbl.text, sizeof(lbl.text), "loc_%04X:", (unsigned)pc);
            }

            u8 op = scd[pc];
            int sz = scd_inst_size(scd, scd_size, pc);

            if (sz == 0) {
                /* Not an opcode (>= 0x72): show the byte once, then stop. */
                ScdLine& u = out.add();
                u.offset = (u32)pc; u.thread = (u8)ti; u.opcode = op; u.size = 1;
                fmt_hex(u.hex, sizeof(u.hex), scd + pc, 1);
                _snprintf(u.text, sizeof(u.text), "    UNK_%02X", op);
                _snprintf(u.comment, sizeof(u.comment), "; not an opcode, listing stops");
                break;
            }
            if (pc + sz > scd_size) break;

            ScdLine& l = out.add();
            l.offset = (u32)pc;
            l.thread = (u8)ti;
            l.opcode = op;
            l.size = sz;
            l.indent = indent;

            /* Get opcode name */
            const char* name;
            char name_buf[32];
            if (op == 0x28 && pc + 3 <= scd_size) {
                u8 typ = scd[pc + 2];
                l.op28_type = typ;
                name = "AOT";
            } else if (op < 0x70) {
                name = g_scd_opcodes[op].name;
            } else {
                _snprintf(name_buf, sizeof(name_buf), "UNK_%02X", op);
                name = name_buf;
            }

            /* Format hex */
            fmt_hex(l.hex, sizeof(l.hex), scd + pc, sz > 16 ? 16 : sz);
            if (sz > 16) {
                int hlen = (int)strlen(l.hex);
                _snprintf(l.hex + hlen, sizeof(l.hex) - hlen, " ...");
            }

            /* Format operands */
            char operands[192];
            fmt_operands(operands, sizeof(operands), op, scd + pc, sz, pc);

            /* Assemble line */
            if (operands[0])
                _snprintf(l.text, sizeof(l.text), "    %-14s %s", name, operands);
            else
                _snprintf(l.text, sizeof(l.text), "    %-14s", name);

            /* Comment */
            if (op < 0x70 && g_scd_opcodes[op].desc[0] && op != 0x00) {
                _snprintf(l.comment, sizeof(l.comment), "; %s", g_scd_opcodes[op].desc);
            }

            /* Track branch targets */
            if ((op == 0x0C || op == 0x0D || op == 0x0E) && pc + 4 <= scd_size) {
                l.is_branch = true;
                s16 rel = rs16(scd + pc + 2);
                l.branch_target = (s32)((int)pc + (int)rel);
            }

            /* Adjust indent for control flow (heuristic) */
            if (op == 0x06) { /* IF_END */
                if (indent > 0) indent--;
            }

            pc += sz;
        }

        /* Blank line between threads */
        ScdLine& blank = out.add();
        blank.thread = (u8)ti;
        blank.text[0] = 0;
    }

    if (code_end < scd_size) {
        ScdLine& t = out.add();
        t.offset = (u32)code_end;
        t.thread = 0xFF;
        _snprintf(t.text, sizeof(t.text), "; 0x%04X-0x%04X: message text, not script",
                  (unsigned)code_end, (unsigned)scd_size);
    }

    free(is_target);
    return out.n_lines > 0;
}

/*═══════════════════════════════════════════════════════════════════
 *  DC1 Text String Decoder
 *═══════════════════════════════════════════════════════════════════*/

static char dc1_glyph(u8 code) {
    if (code == 0x00) return ' ';
    /* Uppercase A-Z: 0x02..0x34, stride 2 */
    if (code >= 0x02 && code <= 0x34 && (code & 1) == 0)
        return (char)('A' + (code - 0x02) / 2);
    /* Lowercase a-z: 0x36..0x68, stride 2 */
    if (code >= 0x36 && code <= 0x68 && (code & 1) == 0)
        return (char)('a' + (code - 0x36) / 2);
    /* Digits 0-9: 0x6A..0x7C, stride 2 */
    if (code >= 0x6A && code <= 0x7C && (code & 1) == 0)
        return (char)('0' + (code - 0x6A) / 2);
    /* Punctuation */
    switch (code) {
    case 0x82: return '.';
    case 0x84: return ',';
    case 0x86: return '!';
    case 0x88: return '?';
    case 0x8A: return '-';
    case 0x8C: return ':';
    case 0x8E: return ';';
    case 0x90: return '"';
    case 0x92: return '\'';
    case 0x94: return '(';
    case 0x96: return ')';
    case 0x98: return '/';
    case 0x9A: return '.';
    case 0x9E: return '/';
    }
    return 0; /* unknown */
}

bool rdt_extract_text(const u8* dec, size_t dec_size, u32 base_addr,
                      RdtTextData& out)
{
    out.init();
    if (dec_size < 32) return false;

    /* Find SCD section (ptr[5]) to locate text after it */
    u32 ptrs[7];
    for (int i = 0; i < 7; i++) {
        u32 p = rd32(dec + i * 4);
        if (p < base_addr) return false;
        ptrs[i] = p - base_addr;
        if (ptrs[i] >= dec_size) return false;
    }

    /* Text data starts after the last section.
       Walk SCD to find its actual extent. */
    size_t scd_off = ptrs[5];
    size_t max_section_end = 0;
    for (int i = 0; i < 7; i++)
        if (ptrs[i] > max_section_end) max_section_end = ptrs[i];

    /* Scan for the DC1 text pattern: (XX 80) word pairs.
       Text blocks are separated by control sequences. */
    size_t scan = max_section_end;
    /* Skip past SCD bytecode — look for first (XX 80) pair after binary data */
    while (scan + 3 < dec_size) {
        if (dec[scan + 1] == 0x80) {
            char g = dc1_glyph(dec[scan]);
            if (g != 0) break; /* found start of text */
        }
        scan++;
    }

    if (scan >= dec_size - 4) return false;

    /* Parse text blocks */
    size_t pos = scan;
    int blk_start = (int)pos;
    int tpos = 0;
    char buf[2048];
    memset(buf, 0, sizeof(buf));

    while (pos + 1 < dec_size && out.n_blocks < 64) {
        u8 lo = dec[pos];
        u8 hi = dec[pos + 1];

        if (hi == 0x80) {
            /* Regular glyph */
            char g = dc1_glyph(lo);
            if (g && tpos < 2040)
                buf[tpos++] = g;
            pos += 2;
        } else if (lo == 0x00 && hi == 0xC0) {
            /* Newline */
            if (tpos < 2040) buf[tpos++] = '\n';
            pos += 2;
        } else if (lo == 0x00 && (hi == 0x01 || hi == 0x08)) {
            /* Message separator / page break — end current block */
            if (tpos > 0) {
                buf[tpos] = 0;
                RdtTextBlock& blk = out.blocks[out.n_blocks++];
                blk.offset = blk_start;
                blk.raw_size = (int)(pos - blk_start);
                blk.text_len = tpos;
                memcpy(blk.text, buf, tpos + 1);
                out.total_chars += tpos;
            }
            /* Skip control bytes */
            pos += 2;
            while (pos + 1 < dec_size && dec[pos + 1] != 0x80) pos += 2;
            blk_start = (int)pos;
            tpos = 0;
            memset(buf, 0, sizeof(buf));
        } else if (lo == 0x00 && hi == 0xA0) {
            /* Message end with pause */
            if (tpos > 0) {
                buf[tpos] = 0;
                RdtTextBlock& blk = out.blocks[out.n_blocks++];
                blk.offset = blk_start;
                blk.raw_size = (int)(pos - blk_start);
                blk.text_len = tpos;
                memcpy(blk.text, buf, tpos + 1);
                out.total_chars += tpos;
            }
            pos += 2;
            while (pos + 1 < dec_size && dec[pos + 1] != 0x80) pos += 2;
            blk_start = (int)pos;
            tpos = 0;
            memset(buf, 0, sizeof(buf));
        } else {
            /* Unknown pair or end of text */
            pos += 2;
            /* If we haven't seen text in a while, stop */
            if (tpos == 0 && pos > (size_t)blk_start + 20)
                break;
        }
    }

    /* Flush final block */
    if (tpos > 0 && out.n_blocks < 64) {
        buf[tpos] = 0;
        RdtTextBlock& blk = out.blocks[out.n_blocks++];
        blk.offset = blk_start;
        blk.raw_size = (int)(pos - blk_start);
        blk.text_len = tpos;
        memcpy(blk.text, buf, tpos + 1);
        out.total_chars += tpos;
    }

    return out.n_blocks > 0;
}
