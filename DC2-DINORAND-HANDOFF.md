# Documentation handoff for dinorand: Dino Crisis 2 findings

Findings from porting dinorand's Dino Crisis 2 readers to RaptorScope and running them over a whole install. Each entry gives the dinorand file it concerns, the proposed wording, the evidence, and how sure it is. Nothing in dinorand was edited; its owner session applies what it accepts.

**Install measured:** the GOG build's `rebirth/Data` (526 files) and `rebirth/Speech` (305 files), read-only. Three rooms in that folder have local `.bak` siblings (ST104, ST105, ST205), so the folder is a working copy, not a pristine one. **Reproduce:** build `tests/dc2_corpus.cpp`, run it with `DINORAND_DC2_DIR`, then `tests/check_dc2_corpus.py` with `DINORAND_ROOT`; the report lists the SHA-256 of every input file.

Confidence labels: **corpus** = holds on every file of the install; **visual** = also confirmed by rendering the decoded data; **observed** = seen, not explained.

## 1. Slot 2 header is read one word short

- **Target:** `tools/dc2_re/decode_s2.py` (`parse_s2`), `docs/reference/dc2/rooms/ROOM-SECTION-TABLE.md` ("s2 full decode").
- **Proposed wording:** "s2 = `u8 count, u8 count2, u16 0, u32 offset2`, then `count` u32 record offsets relative to the section. A record is `u8 flags, u8 npoints, u8 index, u8 param`, 4 bytes of unknown meaning, `npoints` u32 attribute words, then `npoints + 1` `(s16, s16)` pairs, the last repeating the first: 12 + 8 x npoints bytes (44 for 4 points). `offset2`, when not 0, starts `count2` 20-byte records of unknown meaning."
- **Evidence:** with this layout, in all 89 rooms the first offset is `8 + 4 x count`, 838 of 840 records are exactly `12 + 8 x npoints` long, all 840 close on their first point, and byte +2 equals the record's number. `parse_s2` reads `{u32 count, offsets}`, which takes `offset2` (0 in 79 rooms) for record 0 and shifts every record bound by one; in the 10 rooms with a second block it also reads `count2` into the count (ST103: `0x0307`).
- **Figures the document should replace:** 840 records, at most 32 in a room (ST104), not "1 to 388"; flags 0x01 x 487, 0x41 x 340, 13 others; second block in 10 rooms (ST103, ST106, ST109, ST10E, ST203, ST204, ST602, ST606, ST802, ST804), 54 records.
- **Open:** ST702 record 2 carries 588 extra bytes and ST904 record 9 carries 12.
- **Confidence:** corpus. That the pairs are (x, z) is visual: plotted with the script's door and trigger corners, ST102's outline, zones and doors line up.

## 2. Slot 7 record layout

- **Target:** `docs/reference/dc2/rooms/ROOM-SECTION-TABLE.md` (s7), `tools/dc2_re/decode_s1_s7.py` (`decode_s7` returns only size and first word).
- **Proposed wording:** "s7 = a 16-byte header whose byte 0 is the record count, then 32-byte records: four `(s16 x, s16 z)` corners, a `u16` offset (relative to the section) to the record's own data, 14 more bytes. The first record's offset is `16 + 32 x count`."
- **Evidence:** the first-offset rule holds in every room whose count byte is not 0: 50 rooms, 475 records. The other 29 present sections are 4 or 16 bytes or have a zero count.
- **Confidence:** corpus for the structure, visual for the corners (they tile ST102's path). The per-record data and the remaining header bytes are not decoded.

## 3. `.TEX` is an ordinary package; `Dc2TexFile`'s layout is wrong

- **Target:** `tools/DinoCrisis.Model/Textures/Dc2TexFile.cs`, `docs/decisions/dc1/models/DCM-DC2-TEXTURE-PLAN.md` §3.
- **Proposed wording:** "A `.TEX` file is a DC2 package like any other: payloads follow the header sector in table order, each padded to 0x800. A type 1 texture is stored in 64-byte by 32-row tiles, the same order as a type 6 texture after decompression."
- **Evidence:** `E00.TEX` (69632 bytes; identical in `english/` and `rebirth/`) has entries texture 0x10000, palette 0x200. In table order that is 0x800 + 0x10000 + 0x800 = the file size exactly; `E00_02.TEX` (4 entries) also closes exactly, where `Dc2TexFile` needs a 0x800 "trailing pad" that in fact holds the second palette. Rebuilding in table order reproduces all 28 `.TEX` files byte for byte. Decoded in table order and un-tiled, `E00.TEX` is the raptor skin; decoded at `Dc2TexFile`'s offsets (palette 0x800, texture 0x1000, row order) it is noise.
- **Confidence:** visual, and corpus for the layout.

## 4. A DC2 vertex is placed by the part that owns it, not by the face's part

- **Target:** `tools/DinoCrisis.Model/Model/ObjExporter.cs` (`ResolveVertexTransformPart`), `DcmTypes.cs` (the `Child` byte's description).
- **Proposed wording:** "DC2: descriptor byte +7 is the number of vertex triples the part owns. Part *i* owns the next 3 x byte+7 vertices of the pool, and its rest transform places them whichever part's polygon references them. The vertex's pad word repeats the owning part's index."
- **Evidence:** `vertexCount == 3 x sum(byte +7)` holds for all 133 models. The pad word equals the run's part for 238 of 258 vertices in E00, 325 of 339 in WEP_P000 and 360 of 381 in E40, and for every vertex a face uses (placing by pad and placing by run give identical geometry in all three); E00's and E40's unused vertices hold `0x2E2E`. Rendered with run-based placement E00 is a seamless raptor; with polygon-part placement it comes apart at every joint and throws stray vertices.
- **Confidence:** visual.

## 5. DC2 quads are strips

- **Target:** `tools/DinoCrisis.Model/Model/ObjExporter.cs` (`EmitFace` writes `v0 v1 v2 v3` as a ring).
- **Proposed wording:** "A DC2 quad's vertices are in PS1 strip order: triangles (v0, v1, v2) and (v1, v3, v2)."
- **Evidence:** comparing the two triangles' normals for the 320 quads of E00 and WEP_P000: 303 are consistent only as a strip, 10 only as a ring, 7 either way. The strip-order render is clean.
- **Confidence:** visual.

## 6. The 4096 limit on a part's offset rejects a real model

- **Target:** `tools/DinoCrisis.Model/Model/DcmModel.cs` (`Validate`), registry row K127.
- **Proposed wording:** "For DC2, where the header is at offset 0 of the entry and the load base comes from the package, the rest-offset bound is not needed and must not be applied."
- **Evidence:** `E40.DAT` entry 4 (20 parts, 381 vertices) passes every other rule; its root part's y is -4544. It renders, with `E40.TEX`, as a complete red theropod. `dcmtool list` finds 132 models; K127 says 133, which is the count without the bound.
- **Confidence:** visual.

## 7. Entry types 7 and 8

- **Target:** `docs/reference/dc2/format/FORMAT-DC2.md`, `DcmTextureDecoder.cs` (un-tiles `Lzss2`).
- **Proposed wording:** "Type 8 is an LZSS-compressed texture with the usual VRAM rectangle, stored row by row, not tiled. Type 7 occurs once."
- **Evidence:** type census over 2697 entries: 0 x 464, 1 x 321, 2 x 818, 3 x 219, 4 x 145, 5 x 291, 6 x 433, 7 x 1, 8 x 5. The five type 8 entries are entry 3 of ST000, ST004, ST005, ST110 and ST302 `.DBS`; ST000's (128 x 448 halfwords, 114688 bytes decompressed) is a picture of a patrol boat when read row by row and noise when un-tiled. Type 7 is `CORE.DAT` entry 2 (497056 bytes).
- **Confidence:** visual for ST000; the other four were not looked at.

## 8. `.DBS` is a run of packages, one per camera

This entry first said "a package followed by data the header does not describe". That data is simply packages 2..N: dinorand has since decoded it (K142), and the viewer now reads it the same way.

- **Target:** `docs/reference/dc2/format/FORMAT-DC2.md`; nothing to change if it already follows K142.
- **Wording:** "`ST*.DBS` is a run of complete DC2 packages laid end to end, one per camera of the room, each with its own header sector. Camera *i* of the room's slot 9 shows package *i*; slot 10 gives each package's sector and size."
- **Evidence:** 89 of the 90 `.DBS` files (`st80504.DBS` is empty) hold 1169 packages that tile each file exactly, and rebuilding them package by package reproduces all 89 byte for byte. Every room's camera count equals its package count, and every slot-10 record is the offset and length of the package with the same index. Entry types per package: (0, 0, 2, 0) x 779, (0, 0) x 361, (0, 0, 2, 8) x 29; 107779 mask tiles; every JPEG is 320x240. `tests/check_dc2_corpus.py` compares all of it with `decode_dbs.py` and `decode_cameras.py`, with no differences.
- **Effect on entry 7:** its type census counted the first package of each `.DBS` only. Over all packages there are 29 type 8 entries, not 5.
- **Tool note:** `decode_cameras.py --json` prints the JSON and then fails, because the loop over room names also runs for the `--json` argument (`for r in args:` after the `if`/`elif`). The checker imports `decode()` instead.
- **Confidence:** corpus.

## 9. Music

- **Target:** registry row K96.
- **Proposed wording:** add "Four type 4 entries are outside the music files: `M_RESULT.DAT`, `M_TITLE.DAT`, `ST905.DAT` and `ST906.DAT`, entry 0 of each."
- **Evidence:** 65 `MS_/ME_/MF_` files hold 141 MP3 payloads, as K96 says, each starting with an MPEG frame sync or an ID3 tag; the census finds 145 type 4 entries in all.
- **Confidence:** corpus.

## 10. Smaller notes

- **`tools/dc2_re/dc2pkg.py`** keeps zero-size entries, so `ITEM.DAT` (first sector all zeros) parses as 64 empty entries. `GianPackage.TryParse` stops at the first; the two disagree. `MAP.DAT` has non-empty entries after a zero-size one.
- **Player model textures.** 16 packages hold a model whose header tpage no entry of the package covers: E00 and E90 (their skin is in the same-name `.TEX`), M_TITLE, RES00–02, SC08, SC09, SC11, SC19, SC40, SC41, SC46, SC49, WEP_P000 and WEP_P100. For `WEP_P000.DAT` the header says tpage `0x8E`, page (896, 0), and the package's one texture is at (640, 0). Where the player's skin is stored is an open question.
- **Voice.** All 305 files are PCM WAV with one `fmt ` and one `data` chunk: 284 mono 18900 Hz, 21 stereo 37800 Hz, as `VOICE-DECODE-REPORT.md` says.
- **Contradictions the run bears on.** The script walk built on K81's reading of slot 5 + 0x1C as a routine directory gives exactly the documented doors, triggers, items, spawns and 5 invalid opcodes, so K39's "count" reading can be retracted. Slot 5 is the script in all 89 rooms (K47/K48), not geometry.

## 11. Documented figures that reproduce

On this install, unchanged: 89 rooms, per stage 6, 17, 6, 7, 12, 5, 9, 8, 12, 7; slot 4 in 36 rooms, slot 7 in 79, slot 8 in 61; 588 slot 1 records; 181 doors in 84 rooms; 168 `0x31` records in 54 rooms; 71 `0x35` records in 45 rooms; 441 spawns; 5 invalid script opcodes (ST503 x 1, ST607 x 2, ST701 x 2); 65 music files with 141 payloads; 305 voice files. `data/dc2/door-graph.json` and `spawn-graph.json` agree with the census room by room.
