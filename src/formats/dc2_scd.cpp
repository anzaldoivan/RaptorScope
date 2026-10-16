/*═══════════════════════════════════════════════════════════════════
 *  RaptorScope  -  Dino Crisis 2 Room Script
 *
 *  Slot 5 of a room blob is the program of a small stack VM (K47, K48).
 *  This file walks it: opcode lengths, the routine directory, the
 *  records that doors, triggers, item pickups and enemy spawns are
 *  built from, and the listing shown in the SCD panel.
 *
 *  Sources: dinorand SCRIPT-DC2.md, AOT-SYSTEM-RE.md, ENEMY-SPAWNER-RE.md,
 *  data/dc2/scd-vm-opcodes.json, and tools/dc2_re/decode_script.py,
 *  whose walk this follows step for step.
 *═══════════════════════════════════════════════════════════════════*/
#include "formats/dc2.h"
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#define _snprintf snprintf
#endif

/*─── Opcode table ───────────────────────────────────────────────*/

/* Unused dispatch slots: the table's handler for them does nothing
   (0x489B00).  0x34, 0x3C, 0x3D and 0x3F sit inside the 2-byte range and
   are stepped like their neighbours; the others stop a walk. */
static bool op_is_nop(u8 op)
{
    return op == 0x00 || (op >= 0x09 && op <= 0x0F) || op == 0x34 ||
           op == 0x3C || op == 0x3D || op == 0x3F || (op >= 0x5A && op <= 0x5F);
}

/* Lengths are each handler's own advance of the instruction pointer
   (opcode_lengths.py): 0x01-0x03 six bytes, 0x05-0x08 four, 0x10-0x59
   two.  0x04 (return) and 0x19 (jump) set the pointer instead; a listing
   shows them as 2 and 4 bytes. */
int dc2_scd_op_len(u8 op)
{
    if (op >= 0x01 && op <= 0x03) return 6;
    if (op == DC2_OP_RETURN) return 2;
    if (op >= 0x05 && op <= 0x08) return 4;
    if (op == DC2_OP_JUMP) return 4;
    if (op >= 0x10 && op <= 0x59) return 2;
    return 0;
}

bool dc2_scd_op_invalid(u8 op)
{
    return dc2_scd_op_len(op) == 0 && !op_is_nop(op);
}

const char* dc2_scd_op_name(u8 op)
{
    switch (op) {
    case 0x01: return "SET_VAR";        case 0x02: return "STORE_VAR";
    case 0x03: return "ALU";            case 0x04: return "RETURN";
    case 0x05: return "PUSH";           case 0x06: return "POP_STORE";
    case 0x07: return "COND_SKIP";      case 0x08: return "CALL";
    case 0x10: return "SLEEP";          case 0x12: return "BIND_SYSCALL";
    case 0x15: return "THREAD_START";   case 0x16: return "THREAD_KILL";
    case 0x17: return "THREAD_SUSPEND"; case 0x18: return "THREAD_RESUME";
    case 0x19: return "JUMP";           case 0x1A: return "SPAWN";
    case 0x1C: return "FLAG_SET";       case 0x1D: return "FLAG_QUERY";
    case 0x1E: return "ACTOR_CMD";      case 0x1F: return "SYSCALL";
    case 0x20: return "ENTRY_STATE";    case 0x21: return "ACTOR_ANIM";
    case 0x22: return "ROUTINE_SWITCH"; case 0x23: return "ENEMY_PRELOAD";
    case 0x29: return "SCENE_STACK";    case 0x2B: return "ACTOR_CMD2";
    case 0x2C: return "GIVE_ITEM";      case 0x2E: return "ENGINE_CALL3";
    case 0x30: return "AOT_DOOR";       case 0x31: return "AOT_TRIGGER";
    case 0x32: return "AOT_STATE";      case 0x33: return "AOT_FLAG";
    case 0x35: return "AOT_TAKE_ITEM";  case 0x3A: return "AOT_KEY_USE";
    case 0x3E: return "AOT_DISPATCH";   case 0x41: return "EVENT_WAIT";
    case 0x42: return "EVENT_TEST";     case 0x46: return "ACTOR_MOTION";
    case 0x48: return "RANDOM";         case 0x4B: return "WAIT_CAMERA_ZONE";
    case 0x4F: return "WAVE_SPAWN";     case 0x58: return "ACTOR_FLAGS";
    }
    if (op_is_nop(op)) return "NOP";
    return dc2_scd_op_invalid(op) ? "INVALID" : "OP";
}

/* Categories are ScdOpcodeInfo's: 0 flow, 1 thread, 2 var, 3 scene,
   4 entity, 5 camera, 6 item, 8 effect, 9 nop */
int dc2_scd_op_category(u8 op)
{
    if (dc2_scd_op_len(op) == 0 || op_is_nop(op)) return 9;
    switch (op) {
    case 0x03: case 0x04: case 0x07: case 0x08: case 0x19: case 0x22: return 0;
    case 0x10: case 0x15: case 0x16: case 0x17: case 0x18: return 1;
    case 0x01: case 0x02: case 0x05: case 0x06: return 2;
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x3A: case 0x3E: return 3;
    case 0x1A: case 0x1E: case 0x21: case 0x23: case 0x2B: case 0x46:
    case 0x4F: case 0x58: return 4;
    case 0x29: case 0x4B: return 5;
    case 0x2C: case 0x35: return 6;
    }
    return 8;
}

/* The operand resolver (0x48BA50) handles modes 1, 2 and 6 and returns
   the value itself for everything else, so 0 and 4 are both literals
   (K122). */
int dc2_scd_mode(u8 mode_byte)
{
    return (mode_byte == 1 || mode_byte == 2 || mode_byte == 6) ? (int)mode_byte : (int)DC2_MODE_LITERAL;
}

/*─── Routine directory ──────────────────────────────────────────*/

/* At slot + 0x1C is a table of u32 routine offsets, relative to the
   table.  Entry 0 is the room's init routine, where nearly all doors and
   items are set up.  There is no count: the game indexes the table
   blindly (0x48BF20), so it ends where the first routine begins, and
   reading stops at the lowest offset seen so far or at a value that is
   not an offset in the slot.  Entries are not always ascending (ST002's
   init at 0x48 comes before a routine at 0x28). (K81) */
int dc2_scd_routines(const u8* dec, size_t dec_size, const Dc2RoomDir& dir,
                     Dc2Routine* out, int max_out, u32* dir_order)
{
    if (!dir.present[5] || dir.size[5] < 0x20) return 0;
    size_t start = dir.off[5], size = dir.size[5];
    if (start + size > dec_size) return 0;
    size_t table = start + 0x1C;

    u32 starts[DC2_MAX_ROUTINES];
    int n = 0;
    size_t bound = size;
    for (size_t i = 0; i * 4 < bound && n < DC2_MAX_ROUTINES; i++) {
        if (table + i * 4 + 4 > dec_size) break;
        u32 v = rd_u32(dec + table + i * 4);
        if (v >= size) break;
        if (dir_order) dir_order[n] = (u32)(table + v);
        starts[n++] = (u32)(table + v);
        if (v < bound) bound = v;
    }

    /* Sort ascending and drop duplicates; each routine runs to the next */
    for (int i = 1; i < n; i++) {
        u32 v = starts[i];
        int j = i;
        while (j > 0 && starts[j - 1] > v) { starts[j] = starts[j - 1]; j--; }
        starts[j] = v;
    }
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || starts[m - 1] != starts[i]) starts[m++] = starts[i];
    if (m > max_out) m = max_out;
    if (m == 0 && max_out > 0) {
        /* No table: one routine straight after the header (ST104) */
        out[0].start = (u32)(start + 0x20);
        out[0].end = (u32)(start + size);
        return 1;
    }
    for (int i = 0; i < m; i++) {
        out[i].start = starts[i];
        out[i].end = (i + 1 < m) ? starts[i + 1] : (u32)(start + size);
    }
    return m;
}

/*─── Walker ─────────────────────────────────────────────────────*/

/* Called for every instruction.  pushes[0] is the blob offset of the
   0x05 just before it, pushes[1] the one before that, for the unbroken
   run of pushes that precedes the instruction. */
#define DC2_MAX_RUN 32
typedef void (*Dc2OpFn)(void* ctx, const u8* dec, int routine, u32 off, u8 op, int len,
                        const u32* pushes, int n_pushes);

/* Step each routine from its start by opcode length.  A walk ends at a
   return, at an opcode with no length, or at the next routine. */
static int walk(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2OpFn fn, void* ctx)
{
    Dc2Routine routs[DC2_MAX_ROUTINES];
    int n = dc2_scd_routines(dec, dec_size, dir, routs, DC2_MAX_ROUTINES);
    for (int r = 0; r < n; r++) {
        u32 pushes[DC2_MAX_RUN];
        int n_pushes = 0;
        size_t ip = routs[r].start;
        while (ip < routs[r].end && ip < dec_size) {
            u8 op = dec[ip];
            int len = dc2_scd_op_len(op);
            int shown = len ? len : 1;
            if (ip + shown > dec_size) shown = (int)(dec_size - ip);
            fn(ctx, dec, r, (u32)ip, op, shown, pushes, n_pushes);
            if (op == DC2_OP_RETURN || len == 0) break;
            if (op == DC2_OP_PUSH && shown == 4) {
                int keep = n_pushes < DC2_MAX_RUN - 1 ? n_pushes : DC2_MAX_RUN - 1;
                memmove(pushes + 1, pushes, keep * sizeof(u32));
                pushes[0] = (u32)ip;
                n_pushes = keep + 1;
            } else {
                n_pushes = 0;
            }
            ip += len;
        }
    }
    return n;
}

/* Operand at a block offset: a push is 05 <mode> <s16 value> */
static Dc2Operand operand(const u8* dec, const u32* pushes, int n_pushes, int block_off)
{
    Dc2Operand o;
    memset(&o, 0, sizeof(o));
    int k = block_off / 4;
    if (k >= n_pushes) return o;
    o.present = true;
    o.mode = dec[pushes[k] + 1];
    o.value = rd_s16(dec + pushes[k] + 2);
    o.value_off = pushes[k] + 2;
    return o;
}

/* Eight corner words from descending block offsets starting at first */
static void quad(const u8* dec, const u32* pushes, int n_pushes, int first, Dc2Operand* q)
{
    for (int i = 0; i < 8; i++)
        q[i] = operand(dec, pushes, n_pushes, first - i * 4);
}

/*─── Record scanners ────────────────────────────────────────────*/

template<typename T> struct Collect { T* out; int n, max; };

/* 0x30 copies its block into a 0x28-byte record: flags from block+0x44,
   the words at record +4..+0x12 from block+0x3C down to +0x20, the
   destination from +0x0C, the lock index from +0x04 and the unlock flag
   from +0x00.  The slot is block+0x48.  Record +4..+0x12 are the four
   (x, z) corners in a trigger record (K99); the door builder fills the
   same bytes, so they are read as the door's quad. */
static void door_fn(void* ctx, const u8* dec, int, u32 off, u8 op, int,
                    const u32* p, int np)
{
    Collect<Dc2Door>& c = *(Collect<Dc2Door>*)ctx;
    if (op != DC2_OP_DOOR || c.n >= c.max) return;
    Dc2Door& d = c.out[c.n++];
    d.op_off      = off;
    d.unlock_flag = operand(dec, p, np, 0x00);
    d.lock        = operand(dec, p, np, 0x04);
    d.door_type   = operand(dec, p, np, 0x08);
    d.dest        = operand(dec, p, np, 0x0C);
    d.flags       = operand(dec, p, np, 0x44);
    d.slot        = operand(dec, p, np, 0x48);
    quad(dec, p, np, 0x3C, d.quad);
}

static void trigger_fn(void* ctx, const u8* dec, int, u32 off, u8 op, int,
                       const u32* p, int np)
{
    Collect<Dc2Trigger>& c = *(Collect<Dc2Trigger>*)ctx;
    if (op != DC2_OP_TRIGGER || c.n >= c.max) return;
    Dc2Trigger& t = c.out[c.n++];
    t.op_off  = off;
    t.routine = operand(dec, p, np, 0x00);
    t.thread  = operand(dec, p, np, 0x04);
    t.slot    = operand(dec, p, np, 0x30);
    quad(dec, p, np, 0x24, t.quad);
}

static void take_item_fn(void* ctx, const u8* dec, int, u32 off, u8 op, int,
                         const u32* p, int np)
{
    Collect<Dc2TakeItem>& c = *(Collect<Dc2TakeItem>*)ctx;
    if (op != DC2_OP_TAKE_ITEM || c.n >= c.max) return;
    Dc2TakeItem& t = c.out[c.n++];
    t.op_off     = off;
    t.p3         = operand(dec, p, np, 0x0C);
    t.item       = operand(dec, p, np, 0x10);
    t.taken_flag = operand(dec, p, np, 0x14);
    t.slot       = operand(dec, p, np, 0x40);
    quad(dec, p, np, 0x34, t.quad);
}

/* 0x1A fills an actor from its block: slot +0x24, type +0x20, position
   x +0x1C, y +0x18, z +0x14, and the model's load address +0x0C.  Type,
   slot and x/y are usually literals; a raptor's model address and z
   come from the room's global table (mode 6). */
static void spawn_fn(void* ctx, const u8* dec, int, u32 off, u8 op, int,
                     const u32* p, int np)
{
    Collect<Dc2Spawn>& c = *(Collect<Dc2Spawn>*)ctx;
    if (op != DC2_OP_SPAWN || c.n >= c.max) return;
    Dc2Spawn& s = c.out[c.n++];
    s.op_off     = off;
    s.n_pushes   = np;
    s.gate       = operand(dec, p, np, 0x00);
    s.model_base = operand(dec, p, np, 0x0C);
    s.rot        = operand(dec, p, np, 0x10);
    s.z          = operand(dec, p, np, 0x14);
    s.y          = operand(dec, p, np, 0x18);
    s.x          = operand(dec, p, np, 0x1C);
    s.type       = operand(dec, p, np, 0x20);
    s.slot       = operand(dec, p, np, 0x24);
}

int dc2_scd_doors(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2Door* out, int max_out)
{
    Collect<Dc2Door> c = { out, 0, max_out };
    walk(dec, dec_size, dir, door_fn, &c);
    return c.n;
}
int dc2_scd_triggers(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2Trigger* out, int max_out)
{
    Collect<Dc2Trigger> c = { out, 0, max_out };
    walk(dec, dec_size, dir, trigger_fn, &c);
    return c.n;
}
int dc2_scd_take_items(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2TakeItem* out, int max_out)
{
    Collect<Dc2TakeItem> c = { out, 0, max_out };
    walk(dec, dec_size, dir, take_item_fn, &c);
    return c.n;
}
int dc2_scd_spawns(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2Spawn* out, int max_out)
{
    Collect<Dc2Spawn> c = { out, 0, max_out };
    walk(dec, dec_size, dir, spawn_fn, &c);
    return c.n;
}

/*─── Totals ─────────────────────────────────────────────────────*/

static void stats_fn(void* ctx, const u8*, int, u32 off, u8 op, int, const u32*, int)
{
    Dc2ScdStats& st = *(Dc2ScdStats*)ctx;
    st.ops++;
    if (!dc2_scd_op_invalid(op)) return;
    if (st.invalid < (int)(sizeof(st.invalid_off) / sizeof(st.invalid_off[0])))
        st.invalid_off[st.invalid] = off;
    st.invalid++;
}

bool dc2_scd_stats(const u8* dec, size_t dec_size, const Dc2RoomDir& dir, Dc2ScdStats& st)
{
    memset(&st, 0, sizeof(st));
    st.routines = walk(dec, dec_size, dir, stats_fn, &st);
    return st.routines > 0;
}

/*─── Disassembly ────────────────────────────────────────────────*/

struct Listing { ScdDisasm* out; u32 slot_off; int routine; };

static void fmt_operand(char* buf, size_t n, u8 mode, int value)
{
    switch (dc2_scd_mode(mode)) {
    case DC2_MODE_VAR:    _snprintf(buf, n, "var[%d]", value); break;
    case DC2_MODE_STACK:  _snprintf(buf, n, "stk[%d]", value); break;
    case DC2_MODE_GLOBAL: _snprintf(buf, n, "glb[%d]", value); break;
    default:              _snprintf(buf, n, "%d", value); break;
    }
    buf[n - 1] = 0;
}

/* "ST102" for a literal operand, "(computed)" otherwise */
static void fmt_value(char* buf, size_t n, const Dc2Operand& o, const char* fmt)
{
    if (o.literal()) _snprintf(buf, n, fmt, (unsigned)(u16)o.value);
    else             _snprintf(buf, n, o.present ? "(computed)" : "(missing)");
    buf[n - 1] = 0;
}

static void list_fn(void* ctx, const u8* dec, int routine, u32 off, u8 op, int len,
                    const u32* p, int np)
{
    Listing& ls = *(Listing*)ctx;
    ScdDisasm& out = *ls.out;
    if (routine != ls.routine) {
        ls.routine = routine;
        ScdLine& h = out.add();
        h.thread = 0xFF;
        h.offset = off - ls.slot_off;
        _snprintf(h.text, sizeof(h.text) - 1, "; --- routine %d  @ 0x%04X ---",
                  routine, (unsigned)(off - ls.slot_off));
    }

    ScdLine& l = out.add();
    l.offset = off - ls.slot_off;
    l.thread = (u8)routine;
    l.opcode = op;
    l.size   = len;
    int hp = 0;
    for (int i = 0; i < len && hp + 4 < (int)sizeof(l.hex); i++)
        hp += _snprintf(l.hex + hp, sizeof(l.hex) - hp, "%02X ", dec[off + i]);

    const u8* b = dec + off;
    const char* name = dc2_scd_op_name(op);
    char a[48];
    size_t tn = sizeof(l.text) - 1;
    if (len < dc2_scd_op_len(op)) {
        _snprintf(l.text, tn, "%s  (cut off by the end of the room)", name);
    } else switch (op) {
    case 0x01:  /* var[b1] = resolve(mode b2, word +4) */
        fmt_operand(a, sizeof(a), b[2], rd_s16(b + 4));
        _snprintf(l.text, tn, "%-14s var[%d] = %s", name, b[1], a);
        break;
    case 0x02:  /* store(mode b2, word +4) = var[b1] */
        fmt_operand(a, sizeof(a), b[2], rd_s16(b + 4));
        _snprintf(l.text, tn, "%-14s %s = var[%d]", name, a, b[1]);
        break;
    case 0x03:  /* operator b1, slot b2 (0xFF = stack pointer), mode b3, word +4 */
        fmt_operand(a, sizeof(a), b[3], rd_s16(b + 4));
        if (b[2] == 0xFF) _snprintf(l.text, tn, "%-14s op %d  stack, %s", name, b[1], a);
        else              _snprintf(l.text, tn, "%-14s op %d  var[%d], %s", name, b[1], b[2], a);
        break;
    case 0x05: case 0x06:   /* mode b1, word +2 */
        fmt_operand(a, sizeof(a), b[1], rd_s16(b + 2));
        _snprintf(l.text, tn, "%-14s %s", name, a);
        break;
    case 0x07:
        _snprintf(l.text, tn, "%-14s bit %d", name, b[1]);
        break;
    case 0x08:
        _snprintf(l.text, tn, "%-14s routine %d", name, rd_u16(b + 2));
        l.is_branch = true;
        break;
    default:
        if (dc2_scd_op_invalid(op))
            _snprintf(l.text, tn, "INVALID 0x%02X  (not an opcode; the walk stops here)", op);
        else if (len == 1)
            _snprintf(l.text, tn, "NOP 0x%02X  (unused opcode; the walk stops here)", op);
        else if (name[0] == 'O' && name[1] == 'P' && !name[2])
            _snprintf(l.text, tn, "OP_%02X", op);
        else
            _snprintf(l.text, tn, "%s", name);
        break;
    }

    /* What the record-building opcodes build */
    size_t cn = sizeof(l.comment) - 1;
    char v1[24], v2[24];
    if (op == DC2_OP_DOOR) {
        fmt_value(v1, sizeof(v1), operand(dec, p, np, 0x0C), "ST%03X");
        fmt_value(v2, sizeof(v2), operand(dec, p, np, 0x04), "%u");
        _snprintf(l.comment, cn, "door to %s, lock %s", v1, v2);
    } else if (op == DC2_OP_TRIGGER) {
        fmt_value(v1, sizeof(v1), operand(dec, p, np, 0x00), "%u");
        _snprintf(l.comment, cn, "trigger: routine %s", v1);
    } else if (op == DC2_OP_TAKE_ITEM) {
        fmt_value(v1, sizeof(v1), operand(dec, p, np, 0x10), "0x%02X");
        fmt_value(v2, sizeof(v2), operand(dec, p, np, 0x14), "%u");
        _snprintf(l.comment, cn, "item %s, taken flag %s", v1, v2);
    } else if (op == DC2_OP_SPAWN) {
        fmt_value(v1, sizeof(v1), operand(dec, p, np, 0x20), "%u");
        fmt_value(v2, sizeof(v2), operand(dec, p, np, 0x24), "%u");
        _snprintf(l.comment, cn, "spawn type %s in slot %s", v1, v2);
    }
}

bool dc2_scd_disassemble(const u8* dec, size_t dec_size, u32 base, ScdDisasm& out)
{
    out.clear();
    Dc2RoomDir dir;
    if (!dc2_room_directory(dec, dec_size, base, dir)) return false;
    out.dc2 = true;

    Dc2ScdStats st;
    dc2_scd_stats(dec, dec_size, dir, st);
    ScdLine& h = out.add();
    h.thread = 0xFF;
    _snprintf(h.text, sizeof(h.text) - 1,
              "; Dino Crisis 2 room script: %d routines, %d instructions, %d invalid",
              st.routines, st.ops, st.invalid);

    Listing ls = { &out, dir.off[5], -1 };
    out.n_threads = walk(dec, dec_size, dir, list_fn, &ls);
    return out.n_threads > 0;
}
