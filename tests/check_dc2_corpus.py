#!/usr/bin/env python3
"""Compare tests/dc2_corpus output with dinorand's own Dino Crisis 2 readers.

    DINORAND_DC2_DIR=<install> ./dc2_corpus > corpus.tsv
    DINORAND_ROOT=<dinorand checkout> python3 tests/check_dc2_corpus.py corpus.tsv

The oracles are dinorand's tools/dc2_re scripts (dc2pkg, decode_s1_s7,
decode_s2, decode_script, decode_cameras, decode_dbs) and its dcmtool.  Where an oracle is known to be
wrong for a check, the check says so and uses the rule written out here
instead.  Stdlib only.  Reads the install, writes nothing.

Exit status: 0 when every differential check passes or the run is skipped
(no install, or no dinorand checkout), 1 when one differs.  The audit
table at the end compares totals with the figures dinorand's documents
state; a mismatch there is printed as a finding and does not fail the run,
because those figures were measured on other copies of the data.
"""
import collections
import hashlib
import json
import os
import struct
import subprocess
import sys
import types
import zlib

BASE = 0x005E0000   # load address the oracles hard-code for a room blob


def skipped(why):
    print("SKIPPED\t" + why)
    sys.exit(0)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    lines = [l.rstrip("\n").split("\t") for l in open(sys.argv[1], encoding="utf-8")]
    if not lines or lines[0][0] == "SKIPPED":
        skipped("corpus was skipped: " + (lines[0][1] if lines and len(lines[0]) > 1 else "empty"))
    root = os.environ.get("DINORAND_ROOT")
    if not root or not os.path.isdir(os.path.join(root, "tools", "dc2_re")):
        skipped("DINORAND_ROOT is not set to a dinorand checkout")
    data_dir = next(l[1] for l in lines if l[0] == "DIR")
    game_dir = os.path.dirname(data_dir)

    sys.path.insert(0, os.path.join(root, "tools", "dc2_re"))
    import dc2pkg
    import decode_s1_s7
    import decode_s2
    import decode_script
    import decode_cameras
    import decode_dbs

    rows = collections.defaultdict(list)
    for l in lines:
        rows[l[0]].append(l[1:])
    by_file = lambda kind: _group(rows[kind])

    report = Report()

    # ---- inputs -------------------------------------------------------
    print("# Inputs (SHA-256)")
    files = {}
    for r in rows["PKG"]:
        path = os.path.join(data_dir, r[0])     # the original of a patched file, as the corpus tool
        raw = open(path + ".bak" if os.path.exists(path + ".bak") else path, "rb").read()
        files[r[0]] = raw
        print("%s  Data/%s" % (hashlib.sha256(raw).hexdigest(), r[0]))
    for r in rows["WAV"]:
        raw = open(os.path.join(game_dir, "Speech", r[0]), "rb").read()
        print("%s  Speech/%s" % (hashlib.sha256(raw).hexdigest(), r[0]))
    print()

    # ---- packages -----------------------------------------------------
    ents = by_file("ENT")
    oracle_ents = {}
    hidden = []
    for r in rows["PKG"]:
        name, raw = r[0], files[r[0]]
        try:
            full = dc2pkg.parse(raw)[0] if len(raw) >= 2048 else []
        except Exception:
            full = []
        if name.upper().endswith(".DBS"):
            # dc2pkg reads one package; a .DBS is a run of them (decode_dbs)
            full = [types.SimpleNamespace(type=t, size=sz, raw_off=off, data=raw[off:off + sz], decomp=None)
                    for _, pk in decode_dbs.packages(raw)[0] for t, sz, off, _ in pk]
        # dc2pkg keeps zero-size entries (ITEM.DAT's all-zero header gives 64
        # of them); the viewer ends the table there, as dinorand's C#
        # GianPackage does.
        cut = next((i for i, e in enumerate(full) if e.size == 0), len(full))
        if any(e.size for e in full[cut:]):
            hidden.append(name)
        if r[2] == "not-a-package":
            report.check("package entries", name, 0, cut if len(raw) >= 32 and raw[16:32] == bytes(16) else 0)
            continue
        if r[3] != "dc2":
            report.fail("package entries", name, "parsed as a DC1 package")
            continue
        oracle_ents[name] = full[:cut]
        mine = [(int(e[2]), int(e[3]), int(e[4])) for e in ents.get(name, [])]
        report.check("package entries", name, mine, [(e.type, e.size, e.raw_off) for e in full[:cut]])
        report.check("rebuild identity", name, r[4], "rebuild-same")
    if hidden:
        report.note("package entries", "entries after a zero-size entry in: " + ", ".join(hidden))

    # ---- LZSS ---------------------------------------------------------
    blobs = {}
    for name, es in ents.items():
        for e in es:
            i, rtype = int(e[1]), int(e[2])
            o = oracle_ents[name][i]
            report.check("payload bytes", "%s#%d" % (name, i), e[10], "%08x" % zlib.crc32(o.data))
            if rtype in (5, 6, 8):
                dec = o.decomp if o.decomp is not None else dc2pkg.lzss_dec(o.data)
                report.check("LZSS", "%s#%d" % (name, i), (int(e[11]), e[12]), (len(dec), "%08x" % zlib.crc32(dec)))
                if rtype == 5:
                    blobs[(name, i)] = (dec, int(e[9], 16))

    # ---- rooms --------------------------------------------------------
    room_blob = {}
    for r in rows["ROOM"]:
        name = r[0]
        cands = [(d, b) for (n, i), (d, b) in blobs.items() if n == name and len(d) == int(r[1])]
        d, base = cands[0]
        room_blob[name] = d
        report.check("room load address", name, base, BASE)
        slots = decode_s2.slots(d)
        want = []
        for i in range(12):
            if slots[i] is None:
                want.append("-")
            elif i == 11:
                want.append("%d:0" % slots[i])
            else:
                want.append("%d:%d" % decode_s2.bounds(d, i))
        report.check("room directory", name, r[2:14], want)

    s1 = by_file("S1")
    for name, d in room_blob.items():
        _, recs = decode_s1_s7.decode_s1(d)
        want = [(r["t"], r["k"], r["id"], r["x"], r["y"], r["z"], r["flag"], r["p"] & 0xFFFF, r["q"] & 0xFFFF)
                for r in recs]
        report.check("slot 1 markers", name, [tuple(int(v) for v in r[2:11]) for r in s1.get(name, [])], want)

    # Slot 2.  decode_s2.parse_s2 reads the header as {u32 count, offsets},
    # which takes the second-block offset for record 0.  The rule checked
    # here is the one the viewer implements (see dc2_room.cpp), written out
    # again independently; parse_s2's count is compared only as far as it
    # can agree.
    s2r = by_file("S2R")
    oracle_count_differs = []
    for r in rows["S2"]:
        name, d = r[0], room_blob[r[0]]
        o, size = decode_s2.bounds(d, 2)
        count, second = d[o], struct.unpack_from("<I", d, o + 4)[0]
        offs = [struct.unpack_from("<I", d, o + 8 + 4 * i)[0] for i in range(count)]
        end = second or size
        want = []
        for i, ro in enumerate(offs):
            nxt = min(offs[i + 1] if i + 1 < count else end, end)
            n = d[o + ro + 1]
            need = 12 + 8 * n
            if ro + need > nxt:
                continue
            pts = d[o + ro + 8 + 4 * n: o + ro + need]
            want.append((d[o + ro], n, d[o + ro + 2], d[o + ro + 3], nxt - ro - need,
                         1 if pts[:4] == pts[-4:] else 0, "%08x" % zlib.crc32(pts)))
        mine = [tuple(int(v) for v in x[2:8]) + (x[8],) for x in s2r.get(name, [])]
        report.check("slot 2 polygons", name, mine, want)
        report.check("slot 2 second block", name, int(r[3]), d[o + 1] if second else 0)
        if decode_s2.parse_s2(d)[2] != count:
            oracle_count_differs.append(name)
    report.note("slot 2 polygons", "decode_s2.parse_s2 reports a different count in %d rooms: %s"
                % (len(oracle_count_differs), ", ".join(oracle_count_differs)))
    odd = ["%s#%s (+%s bytes)" % (x[0], x[1], x[6]) for x in rows["S2R"] if x[6] != "0"]
    report.note("slot 2 polygons", "records longer than 12 + 8 x points: " + (", ".join(odd) or "none"))
    report.note("slot 2 polygons", "records not closed on their first point: %d of %d"
                % (sum(1 for x in rows["S2R"] if x[7] != "1"), len(rows["S2R"])))

    # Slot 7.  The oracle gives the slot's size and first word only.
    undecoded = []
    for r in rows["S7"]:
        name, d = r[0], room_blob[r[0]]
        meta, _ = decode_s1_s7.decode_s7(d)
        report.check("slot 7 size", name, int(r[1]), meta[1] if meta else 0)
        if meta and meta[1] >= 16:
            report.check("slot 7 count byte", name, int(r[2]), meta[2] & 0xFF)
        if int(r[2]) and r[3] == "0":
            undecoded.append(name)
    report.note("slot 7 zones", "count byte set but not read as zones (first data offset is not "
                "16 + 32 x count): " + (", ".join(undecoded) or "none"))

    # ---- cameras, camera zones, sound zones (decode_cameras) ----------
    cam_rows, cz_rows, sz_rows = by_file("CAM"), by_file("CZ"), by_file("SZ")
    ints = lambda row, a, b: [int(v) for v in row[a:b]]
    pairs = lambda v: [[v[i], v[i + 1]] for i in range(0, len(v), 2)]
    oracle_cams = {}
    for name, d in room_blob.items():
        want = decode_cameras.decode(d)
        oracle_cams[name] = want["cameras"]
        report.check("cameras (slot 9, 10)", name,
                     [(ints(c, 2, 5), int(c[5]), ints(c, 6, 9), int(c[9]), int(c[10]), int(c[11]))
                      for c in cam_rows.get(name, [])],
                     [(c["eye"], c["proj"], c["target"], c["roll"], c["dbs_offset"], c["dbs_size"])
                      for c in want["cameras"]])
        report.check("camera zones (slot 3)", name,
                     [(ints(z, 2, 7), ints(z, 7, 9), pairs(ints(z, 9, 17)), pairs(ints(z, 17, 25)))
                      for z in cz_rows.get(name, [])],
                     [([z["flags"], z["camera"], z["b5"], z["floor"], z["floor_span"]], z["w8"], z["trigger"],
                       z["visible"]) for z in want["zones"]])
        report.check("sound zones (slot 6)", name,
                     [(ints(z, 2, 6), pairs(ints(z, 6, 14))) for z in sz_rows.get(name, [])],
                     [([z["b0"], z["on"], z["floor"], z["b3"]], z["quad"]) for z in want["sound_zones"]])

    # ---- backgrounds (decode_dbs) -------------------------------------
    bg_rows, bgt_rows = by_file("BG"), by_file("BGT")
    shapes, n_bg, n_tiles = collections.Counter(), 0, 0
    for r in rows["PKG"]:
        name = r[0]
        if not name.upper().endswith(".DBS") or r[2] == "not-a-package":
            continue
        raw = files[name]
        pk, end = decode_dbs.packages(raw)
        report.check("background packages tile the file", name, (int(r[7]), end), (len(pk), len(raw)))
        want, want_tiles = [], []
        for p in pk:
            b = decode_dbs.background(raw, p)
            kind = 0 if len(p[1]) < 4 else 1 if p[1][3][0] == 0 else 2
            px = b["cells"] if kind == 1 else b["extra"][1] if kind == 2 else b""
            w, h = decode_dbs.jpeg_size(b["jpeg"])
            want.append((p[0], kind, len(b["jpeg"]), "%08x" % zlib.crc32(b["jpeg"]), w, h, len(b["tiles"]),
                         len(px), "%08x" % zlib.crc32(px)))
            want_tiles.append(",".join("%d,%d,%d" % t for t in b["tiles"]))
            shapes[tuple(e[0] for e in p[1])] += 1
            n_tiles += len(b["tiles"])
            if kind == 1:
                report.check("mask cells are 648 bytes a tile", "%s#%d" % (name, len(want) - 1), len(px),
                             648 * len(b["tiles"]))
        n_bg += len(pk)
        mine = [(int(b[2]), int(b[4]), int(b[5]), b[6], int(b[7]), int(b[8]), int(b[9]), int(b[10]), b[11])
                for b in bg_rows.get(name, [])]
        report.check("backgrounds", name, mine, want)
        report.check("mask tiles", name, [",".join(t[3:]) for t in bgt_rows.get(name, [])], want_tiles)
        # camera i of the room shows package i
        room = name[:-4].upper() + ".DAT"
        if room in oracle_cams:
            report.check("camera i shows background i", name,
                         [(int(c[10]), int(c[11])) for c in cam_rows.get(room, [])],
                         [(p[0], sum((e[1] + 2047 & ~2047) for e in p[1]) + 2048) for p in pk])
        else:
            report.note("backgrounds", "%s has no room file" % name)

    # ---- script -------------------------------------------------------
    doors, trigs, items, spawns = by_file("DOOR"), by_file("TRIG"), by_file("ITEM"), by_file("SPAWN")

    def opnd(text):         # "name=value/mode" or "name=-"
        v = text.split("=", 1)[1]
        return (None, None) if v == "-" else tuple(int(x) for x in v.split("/"))

    for r in rows["SCR"]:
        name, d = r[0], room_blob[r[0]]
        start, size = decode_script.script_section(d)
        routs, _ = decode_script.routines(d, start, size)
        ops = [x for s, e in routs for x in decode_script.disasm(d, s, e)]
        bad = ["%d:%02x" % (x[0], x[1]) for x in ops
               if x[1] not in decode_script.LEN and x[1] not in (decode_script.RETURN, decode_script.JUMP)
               and x[1] not in decode_script.NOP_OPS]
        report.check("script walk", name, (int(r[1]), int(r[2]), r[4:]), (len(routs), len(ops), bad))

        want = [(x["op_off"], (x["dest_word"], x["dest_mode"]), (x["lock_index"], x["lock_mode"]),
                 (x["unlock_flag_id"], x["unlock_flag_mode"]), (x["flags"], x["flags_mode"]),
                 (x["door_type"], x["door_type_mode"]), x["slot"])
                for x in decode_script.door_commits(d, start, size)]
        mine = [(int(x[1]), opnd(x[2]), opnd(x[3]), opnd(x[4]), opnd(x[5]), opnd(x[6]), opnd(x[7])[0])
                for x in doors.get(name, [])]
        report.check("doors (0x30)", name, mine, want)

        want = [(x["op_off"], x["thread"], x["routine"], x["slot"], x["pos"])
                for x in decode_script.item_commits(d, start, size)]
        mine = []
        for x in trigs.get(name, []):
            q0, q1 = opnd(x[5]), opnd(x[6])
            pos = [q0[0], q1[0]] if q0[1] == 0 and q1[1] == 0 else None
            mine.append((int(x[1]), opnd(x[3])[0], opnd(x[2])[0], opnd(x[4])[0], pos))
        report.check("triggers (0x31)", name, mine, want)

        want = [(x["op_off"], (x["item_id"], x["item_operand"]["mode"]), x["flag5"], x["p3"], x["slot"])
                for x in decode_script.take_item_commits(d, start, size)]
        mine = [(int(x[1]), opnd(x[2]), opnd(x[3])[0], opnd(x[4])[0], opnd(x[5])[0]) for x in items.get(name, [])]
        report.check("item pickups (0x35)", name, mine, want)

        want = []
        for s, e in routs:
            rops = decode_script.disasm(d, s, e)
            for idx, x in enumerate(rops):
                if x[1] == decode_script.SPAWN:
                    m, nrun = decode_script.block_from_pushrun(rops, idx)
                    want.append((x[0], nrun) + tuple(m.get(k, (None, None))
                                                     for k in (0x24, 0x20, 0x0C, 0x1C, 0x18, 0x14, 0x10, 0x00)))
        mine = [(int(x[1]), int(x[2])) + tuple(opnd(v) for v in x[3:11]) for x in spawns.get(name, [])]
        report.check("spawns (0x1A)", name, mine, want)

    # dinorand's generated graphs (data/dc2): per-room counts
    for fname, key, mine in (("door-graph.json", "door_count", doors), ("spawn-graph.json", "spawn_count", spawns)):
        graph = json.load(open(os.path.join(root, "data", "dc2", fname), encoding="utf-8"))["rooms"]
        diff = ["ST%s %d vs %d" % (k, len(mine.get("ST%s.DAT" % k, [])), v[key]) for k, v in sorted(graph.items())
                if len(mine.get("ST%s.DAT" % k, [])) != v[key]]
        report.note(fname, "rooms whose count differs from the generated file: " + (", ".join(diff) or "none"))
    stats = json.load(open(os.path.join(root, "data", "dc2", "item-sources.json"), encoding="utf-8"))["stats"]
    report.note("item-sources.json", "sat1_trigger %s (here %d), stats: %s"
                % (stats.get("sat1_trigger"), len(rows["TRIG"]), json.dumps(stats)))

    # ---- models -------------------------------------------------------
    models = by_file("MODEL")
    tool = os.path.join(root, "tools", "DinoCrisis.Model.Cli", "bin", "Debug", "net8.0", "dcmtool.dll")
    dotnet = os.environ.get("DOTNET", os.path.expanduser("~/.dotnet/dotnet"))
    if os.path.exists(tool) and os.path.exists(dotnet):
        out = subprocess.run([dotnet, tool, "list", data_dir], capture_output=True, text=True).stdout
        want, cur = collections.defaultdict(list), None
        for line in out.splitlines():
            w = line.split()
            if line.startswith("  ["):
                want[cur].append((int(w[2]), int(w[8]), int(w[4]), int(w[6])))   # parts verts tris quads
            elif line.endswith("model(s)") and ":" in line:
                cur = line.split(":")[0]
        only_mine = []
        for name in sorted(set(models) | set(want)):
            mine = [tuple(int(v) for v in x[2:6]) for x in models.get(name, [])]
            if name in want:
                report.check("models", name, mine, want[name])
            else:
                only_mine.append("%s (%d)" % (name, len(mine)))
        report.note("models", "read here but refused by dcmtool: " + (", ".join(only_mine) or "none"))
    else:
        report.note("models", "dcmtool not built or dotnet not found; models not compared")
    bad = [x[0] for x in rows["MODEL"] if x[9] != "1"]
    report.check("models", "every probed model builds", bad, [])
    no_tex = sorted(set(x[0] for x in rows["MODEL"] if x[12] == "-1"))
    report.note("models", "no texture for the header's tpage in the model's own package (%d files; "
                "E*.DAT use the .TEX of the same name): %s" % (len(no_tex), ", ".join(no_tex)))
    other_pal = sorted(set(x[0] for x in rows["MODEL"] if x[12] != "-1" and x[14] not in ("-1", x[13])))
    report.note("models", "header CLUT is not on the palette stored after the model's texture (%d files; "
                "the atlas uses the stored one): %s" % (len(other_pal), ", ".join(other_pal) or "none"))

    # ---- music (K96): 32-byte records {4, size, track, flag, 16 x 0} ----
    mp3 = by_file("MP3")
    for name, raw in files.items():
        if name[:3] not in ("MS_", "ME_", "MF_"):
            continue
        want, off, pos = [], 0, 2048
        while off + 32 <= 2048 and struct.unpack_from("<I", raw, off)[0] == 4:
            size, track, flag = struct.unpack_from("<III", raw, off + 4)
            want.append((track, flag, size, "%08x" % zlib.crc32(raw[pos:pos + size])))
            # an MPEG audio frame sync, or an ID3 tag in front of one
            mpeg = raw[pos:pos + 3] == b"ID3" or (raw[pos] == 0xFF and raw[pos + 1] & 0xE0 == 0xE0)
            report.check("music is MPEG audio", "%s#%d" % (name, off // 32), mpeg, True)
            pos += (size + 2047) & ~2047
            off += 32
        report.check("music tracks", name, [(int(x[2]), int(x[3]), int(x[4]), x[5]) for x in mp3.get(name, [])], want)

    # ---- voice --------------------------------------------------------
    for r in rows["WAV"]:
        stem = int(os.path.splitext(r[0])[0])
        # RIFF, PCM, channels, rate, bits, data chunk fits: what AudioPanel::load_wav needs
        want = ("1", "1", "2", "37800", "16", "1") if stem >= 1000 else ("1", "1", "1", "18900", "16", "1")
        report.check("voice files", r[0], tuple(r[1:7]), want)

    music = [n for n in mp3 if n[:3] in ("MS_", "ME_", "MF_")]
    report.note("music", "MP3 entries outside MS_/ME_/MF_ files: "
                + (", ".join("%s (%d)" % (n, len(mp3[n])) for n in sorted(mp3) if n not in music) or "none"))

    report.summary()

    # ---- audit --------------------------------------------------------
    per_stage = collections.Counter(r[0][2] for r in rows["ROOM"])
    present = lambda slot: sum(1 for r in rows["ROOM"] if r[2 + slot] != "-")
    rooms_with = lambda kind: len(set(r[0] for r in rows[kind]))
    invalid = sum(int(r[3]) for r in rows["SCR"])
    audit = [
        ("room files", "89", len(rows["ROOM"]), "K14, K36"),
        ("rooms per stage 0-9", "6,17,6,7,12,5,9,8,12,7",
         ",".join(str(per_stage[str(i)]) for i in range(10)), "K14, K36"),
        ("slot 4 present", "36", present(4), "ROOM-SECTION-TABLE.md"),
        ("slot 7 present", "79", present(7), "ROOM-SECTION-TABLE.md"),
        ("slot 8 present", "61", present(8), "ROOM-SECTION-TABLE.md"),
        ("slot 1 records", "588", len(rows["S1"]), "ROOM-SECTION-TABLE.md"),
        ("doors / rooms", "181 / 84", "%d / %d" % (len(rows["DOOR"]), rooms_with("DOOR")), "K81"),
        ("0x31 records / rooms", "168 / 54", "%d / %d" % (len(rows["TRIG"]), rooms_with("TRIG")), "K129"),
        ("0x35 records / rooms", "71 / 45", "%d / %d" % (len(rows["ITEM"]), rooms_with("ITEM")), "K129"),
        ("spawns", "441", len(rows["SPAWN"]), "ENEMY-SPAWNER-RE.md"),
        ("invalid script ops", "5", invalid, "K48"),
        ("valid models", "133", len(rows["MODEL"]), "K127"),
        ("music files / MP3 payloads", "65 / 141", "%d / %d" % (len(music), sum(len(mp3[n]) for n in music)), "K96"),
        ("voice files", "305", len(rows["WAV"]), "VOICE-DECODE-REPORT.md"),
        ("cameras", "1169", len(rows["CAM"]), "K143"),
        (".DBS files / backgrounds", "89 / 1169", "%d / %d" % (len(bg_rows), len(rows["BG"])), "K142"),
        ("background shapes", "779 / 361 / 29", "%d / %d / %d" % (shapes[(0, 0, 2, 0)], shapes[(0, 0)],
                                                                    shapes[(0, 0, 2, 8)]), "K142"),
        ("mask tiles", "107779", n_tiles, "K142"),
        ("camera zones", "788", len(rows["CZ"]), "decode_cameras.py census"),
    ]
    print("\n# Audit: measured totals against dinorand's documents")
    print("%-28s %-24s %-24s %-8s %s" % ("figure", "documented", "measured", "", "source"))
    for what, doc, got, src in audit:
        print("%-28s %-24s %-24s %-8s %s" % (what, doc, got, "same" if str(got) == doc else "DIFFERS", src))
    sys.exit(1 if report.failed else 0)


def _group(rows):
    out = collections.defaultdict(list)
    for r in rows:
        out[r[0]].append(r)
    return out


class Report:
    def __init__(self):
        self.passed = collections.Counter()
        self.failures = collections.defaultdict(list)
        self.notes = []
        self.order = []
        self.failed = False

    def _seen(self, check):
        if check not in self.order:
            self.order.append(check)

    def check(self, check, what, mine, oracle):
        self._seen(check)
        if _norm(mine) == _norm(oracle):
            self.passed[check] += 1
        else:
            self.fail(check, what, "viewer %r, oracle %r" % (_short(mine), _short(oracle)))

    def fail(self, check, what, why):
        self._seen(check)
        self.failed = True
        self.failures[check].append("%s: %s" % (what, why))

    def note(self, check, text):
        self.notes.append((check, text))

    def summary(self):
        print("# Differential checks")
        for c in self.order:
            bad = self.failures.get(c, [])
            print("%-24s %5d same  %5d differ" % (c, self.passed[c], len(bad)))
            for b in bad[:10]:
                print("    " + b)
            if len(bad) > 10:
                print("    ... and %d more" % (len(bad) - 10))
        print("\n# Findings")
        for c, t in self.notes:
            print("%s: %s" % (c, t))
        print("\nRESULT: " + ("DIFFERENCES FOUND" if self.failed else "all differential checks passed"))


def _norm(v):
    if isinstance(v, (list, tuple)):
        return [_norm(x) for x in v]
    return v


def _short(v):
    s = repr(v)
    return s if len(s) < 300 else s[:300] + "..."


if __name__ == "__main__":
    main()
