# Experimental GLB animation export modification

See [EXPORTING.md](EXPORTING.md) for the new exporter, Windows build instructions, validation, and known limitations. Upstream documentation follows.

<div align="center">

# 🦖 RaptorScope

**A native Win32 + OpenGL viewer and editor for Dino Crisis (1999 PC) game archives, with partial support for Dino Crisis 2**

[![License: Unlicense](https://img.shields.io/badge/License-Unlicense-blue.svg)](https://unlicense.org/)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows%20XP%2B-0078D6?logo=windows)](https://www.microsoft.com/windows)
[![Language: C++](https://img.shields.io/badge/Language-C%2B%2B-00599C?logo=cplusplus)](https://isocpp.org/)
[![OpenGL: 1.1+](https://img.shields.io/badge/OpenGL-1.1%2B-5586A4?logo=opengl)](https://www.opengl.org/)
[![Dependencies: None](https://img.shields.io/badge/Dependencies-None-brightgreen)](#requirements)
[![Built with Claude](https://img.shields.io/badge/Built%20with-Claude%20Opus%204.6-CC785C?logo=anthropic)](https://www.anthropic.com/)
[![Lines of Code](https://img.shields.io/badge/Lines%20of%20Code-32K%2B-informational)](#architecture)

---

<img src="preview.jpg" alt="RaptorScope Preview" width="800">

*3D room viewer with textured PSX geometry, overlay visualization, walk-through mode, and object picking*

</div>

---

## Overview

RaptorScope is a comprehensive reverse-engineering toolkit for **Dino Crisis** (PC, 1999). It parses the game's proprietary `.DAT` archive format and provides interactive viewers for every data type: room geometry, character models, textures, palettes, audio, and SCD bytecode scripts.

The tool reconstructs the PSX GPU rendering pipeline in software — including 4bpp/8bpp CLUT-indexed textures, VRAM deswizzling, BGR555 color conversion, semi-transparency (STP) blending, and per-pixel palette lookup — to produce accurate previews of how assets appear on original hardware.

## Features

### Archive Browser
- Open and browse `.DAT` archive files with automatic entry type detection
- Export individual entries or entire archives to disk
- Import replacement entries for modding workflows
- Drag-and-drop file loading

### 3D Room Viewer
- **Full room geometry** reconstructed from RDT (Room Data Table) files, including the two rooms that store their RDT uncompressed (ST50B, ST60E)
- **Mixed BPP rendering**: 4bpp and 8bpp textures coexist in a single atlas via 2D sparse sub-palette allocation
- **SCD script walker**: Decodes every script thread straight through, both sides of each branch, to extract camera shots (0x4C, and its short form 0x2E), objects (0x42: a type and position for each, mostly effect objects such as the 80 by ST10A's broken window), enemy spawns (0x5B), and scene lights (0x3A)
- **Trigger zones**: Every 0x28 zone in the script area is found by scanning it, not by the walk, so zones set after an 0x01 (such as ST103's door triggers) are shown
- **Scripted placement**: The room is built the way the game builds it — only the sections its scripts place into model slots are drawn. Placements (0x23, and item pickup zones 0x28 type 4) are found by scanning the whole script area, then later script writes to the slot (0x2A, 0x36, 0x37) adjust position and rotation, e.g. lifting items onto desks
- **Item pickup models**: Type-4 zones (0x28) put their item model at the zone centre, as the game does
- **Unplaced sections (H key)**: Sections the game stores but never places (unused leftovers, such as an unused DDK in ST103) are hidden by default; **H** shows them tinted magenta at their raw positions
- **Game cameras (K key)**: **K** steps through the room's camera shots (**Shift+K** back). Each shot puts the view where the game puts its camera, with the game's field of view (H=320 on a 320x240 screen); the game's 4:3 frame is outlined with the area outside it dimmed, and the trigger zone that switches to the shot is outlined. Scrolling widens or narrows the view around the frame without moving the eye. Most rooms also keep a separate copy of their walls for each camera, and the camera's setup thread (script thread N for camera N) shows only its own copies with 0x48 03 <slot> <0/1>, so in a shot only that camera's sections are drawn, as in the game. Drawing every section at once, the default, shows details that differ between copies twice, such as ST10A's corkboard
- **Walking through shots**: Each shot starts with a player marker in the middle of its trigger zone. **WASD** or the **arrow keys** walk it along the floor (**Shift** runs); shots that follow the player (fixed cameras that turn to watch her, cameras that slide along a corridor, side-on, orbiting and rail cameras) follow it as the game's camera handler would, and walking into another shot's trigger zone cuts to that shot. Dragging the view leaves the shot to orbit freely: the game camera is then drawn as its eye and view pyramid, which keeps following the player, and **J** goes back onto the shot. Shots that only shift the previous camera (types 10, 11, 19-21, 32, 33) have no position of their own; K still shows their sections
- **Outdoor scenes**: Sky domes and sea planes out to ±32,000 units are kept; the camera frames the walkable area and the far clip plane reaches the sky
- **PSX-accurate transparency**: 0x0000 masking, 0x8000 transparent black, STP semi-transparency for 4bpp decals (blood splatters)
- **PSX vertex color modulation**: Room and door faces are Gouraud-shaded from their corner colours, scaled 2x to match hardware `(tex * vtx) / 128` behavior, so shading baked into a face shows as it does in game (ST108's wall box). Colours above 128, which brighten the texture on the PSX, stop at the texture's own colour
- **Draw order inside a section**: The game draws some model slots (0x23 render mode 2 or 3) with no depth sorting inside the section: the first quad is drawn last, on top. Rooms rely on it for windows, signs and floor markings set a little behind the surface they show on, without a hole cut for them, such as ST101's restroom windows 22 units behind the wall. The viewer moves each such face 2 units in front of the face covering it, so it shows as it does in game. GLB, OBJ and SMD exports include the move

### Walk-Through Mode
- First-person camera with WASD movement, mouse look, jump, and sprint
- Collision detection via walkable grid built from ptr[1] collision rects
- Noclip fly mode (P key) for unrestricted exploration
- Spawn at character placement points from SCD data
- Fireball projectiles (LMB) with physics and floor bounce

### Object Picking
- **Ctrl+Click** any face in the 3D viewport to select its material group
- Highlights all faces sharing the same tpage + CLUT combination
- HUD displays section offset, face count, tpage decode (BPP, ABR, TX, TY), CLUT coordinates, and bounding box
- Essential for debugging texture issues and identifying specific geometry

### Overlay Visualization
Toggle with **O key** to see all parsed SCD data as colored wireframe overlays:

| Color | Source |
|---|---|
| Green | Collision rectangles (ptr[1]) |
| Orange | Zone type 0 (0x28) |
| Light Blue | Zone type 1 (0x28) |
| Yellow | Zone types 2/3 (0x28) |
| Cyan | Item pickup zones, type 4 (0x28) |
| Purple | Zones of other types (0x28) |
| Yellow (flat) | Floor zones (ptr[6]), where footsteps sound different |
| Violet | Camera cut zones (ptr[2]) |
| Gold | Scene lights (0x3A + ptr[0]) |
| Blue dot | Script camera eye position (0x4C, 0x2E) |
| Red dot | Script camera target (0x4C, 0x2E) |
| Dashed | Camera trigger zones (ptr[3]), coloured per camera |
| Red X | Enemy spawn (0x5B), found by scanning the script area; one parked off stage for a cutscene (ST100) is shown where the cutscene moves it |
| White star | Object (0x42) |
| Orange dot | Character spawn (0x20) |
| Pale blue rings | Fog (0x3D) |

Zones and rectangles carry only X/Z, so each is drawn on the floor beneath it: the height of the upward-facing horizontal faces that lie under it (a ceiling never counts, even when it has more geometry than the floor, as in ST105). A shape over the room's main floor stays on it, so a desk or counter inside a zone does not lift it; a shape on another level, such as ST201's balcony or ST202's platforms, is drawn at that level.

**Overlay elements panel**: while the overlay is on, a panel on the right of the view lists the room's overlay elements by type, with a count for each:
- Untick a type to hide all of it, such as every camera or every enemy spawn.
- Expand a type to tick or untick single elements, such as only Camera 3 in ST10A. Cameras are listed by the number **K** shows, and each covers the trigger zones that switch the game to it.
- Floor zones show their footstep step as the room starts. Step 0 changes nothing until a script raises it, as with ST100's two zones.
- Click a row to highlight that element in white, with a tall pin above it.
- The legend lists only the types that are shown and present in the room.
- A hidden type stays hidden when you open another room; single elements are shown again.

### Character Model Viewer
- EMD model parsing with skeletal hierarchy
- Bone visualization and animation playback
- Per-part mesh rendering with UV mapping

### Texture and Palette Viewer
- 4bpp, 8bpp, and 16bpp texture preview
- **Face-based decoding** for room textures: each area is decoded with the bit depth and CLUT its faces actually use, so pages that mix 4bpp and 8bpp display correctly (**C** switches to the single-CLUT view)
- Areas that no 3D face in the archive uses are dimmed and decoded with their page's most common CLUT. They are not necessarily unused: the game also draws 2D screens straight from the texture, such as ST300's code-input screen (BACK/OK buttons, letter tiles and labels), and those can show in the wrong colours
- Multi-row CLUT visualization with sub-palette cycling (**Up/Down**)
- VRAM deswizzle (64x32 block tiling) for raw PS1 texture data
- UV editor overlay for texture coordinate inspection

### Audio Player
- VAG ADPCM decoding with waveform visualization
- Multi-sample SNDB bank playback at each sample's own rate, derived from the Gian (SNDH) tone that plays it; the rate is shown per sample and used for WAV export
- SEQ music playback using the bank's instruments at the correct pitch
- Note: the sound banks inside room files are the original PlayStation samples. The PC game plays its own higher-quality WAVs from `Sound\SE` (and music from `Sound\BGM`), which RaptorScope opens directly

### Save File Editor
- Reverse-engineered save file format with inventory editing
- Event flag bit patterns for room teleportation
- Item slot manipulation

### Additional Features
- **Dark theme**: Automatic Windows 10/11 dark mode detection with dark title bar
- **Export formats**: OBJ, SMD (Source engine), GLB (characters with animations, and textured rooms), BMP texture export
- **Hex viewer**: Raw binary inspection of any archive entry with offset display
- **Software renderer fallback**: GDI Generic compatibility for VMs without GPU acceleration
- **F11 render mode cycling**: Normal / VM GPU / Software rendering

## Dino Crisis 2

The viewer also opens the PC version of Dino Crisis 2 (GOG build). The game is told apart by the files themselves: its packages use 32-byte header entries. Nothing reads `Dino2.exe`.

Only what the dinorand project has decoded is shown. A room's visible scene is not among it: neither its mesh nor its pre-rendered backgrounds are decoded, so a room appears as its logic layer.

| File | What you get |
|---|---|
| Any package (`*.DAT`, `*.TEX`, `*.DBS`) | Entry tree, hex view, Save Selected, Export Decompressed, Import / Replace, Save As |
| `E*.DAT`, `WEP_*.DAT`, `DOOR*.DAT`, `KOF_*.DAT` and other model packages | The model in its bind pose in the 3D viewer, with OBJ, SMD and GLB export. An enemy's skin is taken from the `.TEX` of the same name in the same folder (`E00.TEX` for `E00.DAT`). `WEP_P000.DAT` and `WEP_P100.DAT` carry no skin and are shown with `WP00A.DAT` / `WP10A.DAT` from the same folder as a stand-in (alternate skin) |
| `ST*.DAT` | **Scene**: collision polygons, zones, doors with their destination room, triggers, item pickups, enemy spawns and markers, drawn as an overlay (O). **F4**: the room script. One node per section of the room, each opening as hex |
| Textures and palettes in any package | The image panel. A texture that a model of the same package samples is decoded with that model's depth and CLUT (**C** switches to one palette). A palette shorter than 256 colours makes the texture 4-bit; **Up/Down** steps through its 16-colour sub-palettes |
| `TITLE.DAT`, `*.PXL` | 320x240 pictures |
| `MS_*.DAT`, `ME_*.DAT`, `MF_*.DAT` | Each music track listed; Save Selected writes it as an `.mp3`. There is no MP3 playback |
| `Speech\NNNN.dat` | Plays as audio (the files are WAV) |

Not available for Dino Crisis 2, because the formats are not decoded: the room mesh and prop geometry, cameras and camera-switch zones, backgrounds (`ST*.DBS` holds them after its package), animation, message text, sound effects, saves, and the weapon editor (the weapon data is in the executable). `SC*.DAT`, `MAP*.DAT`, `ITEM.DAT`, `DEMO*.TRG` and room sections 0, 3, 4, 6, 8, 9 and 10 open as hex.

Limits of what is shown:

- A door, trigger, item or spawn whose position the script computes while the game runs is listed in the script (F4) but not drawn. Most raptor spawns are like this.
- `WEP_P000.DAT` and `WEP_P100.DAT` have no skin of their own. The alternate-skin file shown in its place is one the game loads only when a flag is set, so the default skin may differ. Models in `SC*.DAT`, `RES0*.DAT` and `M_TITLE.DAT` whose texture is not in their package open untextured.
- A texture no model samples (every room texture) is shown with the palette stored after it and, at 4-bit, its first sub-palette; which one the game uses is not known.
- `M_RESULT.DAT`'s model is shown with the palette stored after its texture, not the one its header names, which does not fit that texture.
- Room names are in the executable, so the tree shows file names.

## Building

### MinGW (Recommended)

```bash
cd raptorscope
mingw32-make -f Makefile.mingw
```

Debug build with symbols:
```bash
mingw32-make -f Makefile.mingw DEBUG=1
```

### Code::Blocks

Open `projects/codeblocks/RaptorScope.cbp` and build (F9).

### Visual Studio 2022

Use the solution in `projects/vs2022/` or create a new project importing all `src/**/*.cpp` and `include/` as the include path. Link against `opengl32.lib`, `glu32.lib`, `winmm.lib`, `comctl32.lib`, `comdlg32.lib`, `shlwapi.lib`.

## Requirements

| Requirement | Details |
|---|---|
| **Compiler** | MinGW GCC, MSVC 2015+, or any C++ compiler (pre-C++11 compatible) |
| **OS** | Windows XP SP3 or later |
| **Graphics** | OpenGL 1.1+ (uses display lists and immediate mode) |
| **Dependencies** | **None** — pure Win32 API + OpenGL, no external libraries |

The entire project compiles with zero external dependencies. All format parsing, decompression, audio decoding, and rendering is implemented from scratch.

## Architecture

```
raptorscope/
├── include/
│   ├── core/           Types, LZSS, color conversion, audio decoding
│   ├── formats/        DAT archive, texture, mesh/RDT, TIM/IMD, save files
│   ├── ui/             Application state, panel definitions
│   └── ext/            ddraw_ext.h (optional DirectDraw overlay framework)
├── src/
│   ├── core/           LZSS decompressor, BGR555 to RGBA, VAG/SEQ audio
│   ├── formats/        Archive parser, VRAM deswizzle, room/EMD mesh builder
│   ├── ui/             Win32 window framework, OpenGL panels, hex/image/3D/audio
│   └── main.cpp        WinMain entry point
├── res/                Application icon, RC script, XP manifest
├── projects/           IDE project files (Code::Blocks, VS2022)
├── Makefile.mingw      GNU Make build script
├── LICENSE             The Unlicense (public domain)
└── README.md           This file
```

**~32,000 lines of C++** across 30 source files. Key modules:

| Module | Lines | Purpose |
|---|---|---|
| `panels.cpp` | 6,100 | OpenGL 3D viewer, walk mode, object picking, all panel rendering |
| `app.cpp` | 3,600 | Archive loading, texture atlas builder, mesh pipeline orchestration |
| `mesh.cpp` | 3,300 | RDT room parsing, SCD script walker, mesh instancing, overlay extraction |
| `save_editor.cpp` | 1,800 | Save file reverse engineering and editing UI |
| `panels.h` | 540 | ViewerPanel3D state: camera, walk mode, atlas, picking, animation |
| `mesh.h` | 440 | Mesh/overlay structs, section xforms, collision rects, camera entries |

## Format Documentation

### DAT Archive Structure

The `.DAT` file begins with a 2048-byte header containing 16-byte entries:

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | Entry type (0=Data, 1=Texture, 2=Palette, 3=Sound header (Gian), 4=Sound bank (VAG), 5=Sequence, 7=LZSS, 8=LZSS Texture) |
| 0x04 | 4 | Compressed/raw size in bytes |
| 0x08 | 2 | VRAM X position (halfwords) |
| 0x0A | 2 | VRAM Y position |
| 0x0C | 2 | Width (halfwords) |
| 0x0E | 2 | Height (rows) |

Entry data follows at 2048-byte aligned offsets after the header.

### RDT Room Format

Type 7 entries decompress via LZSS to an RDT structure (ST50B and ST60E store theirs uncompressed as a type 0 entry):

- **Header**: 7 PSX pointers (base `0x80100000`) to data sections
- **ptr[0]**: Static room lights + ambient RGB
- **ptr[1]**: Collision rectangle groups (16 groups x variable rects)
- **ptr[2]**: Camera cut trigger zones
- **ptr[3]**: Camera thread zones
- **ptr[5]**: SCD bytecode (room initialization scripts)
- **ptr[6]**: Floor zones, where footsteps play a different sound: a count byte and three bytes of padding, then 20-byte records of four XZ corners, a level at +16 (matched against the walker) and a step at +17. Inside a zone the footstep sound is 0xB0 + 2 × step instead of 0xB0 (`sub_40B8A9`). Opcode 0x64 changes a zone's step: ST100 starts its two zones at 0 and turns them on mid-room
- **Mesh sections**: Starting at offset 0x1C, sequential 12-byte headers (tri_ptr, quad_ptr, tri_count, quad_count). Sections are only drawn where a script places them; some rooms carry unused leftovers

### SCD Bytecode

The room initialization script (ptr[5]) uses a multi-threaded virtual machine:

- **Thread table**: Array of u32 offsets from SCD base; the first offset / 4 is the thread count
- **Key opcodes**: 0x20 (character spawn), 0x23 (section instance), 0x28 (zone definition), 0x2E (camera eye + target, short form of 0x4C), 0x3A (scene light), 0x3C (model slot render mode and OT depth), 0x48 (show/hide; type 3 = model slot), 0x42 (object: type at +2, position at +4/+6/+8), 0x4C (camera entry), 0x5B (enemy spawn: character slot +1, type +2, position +4/+6/+8, rotation +10, model and animation +12/+16; handler 0x42A96D), 0x59 (second character placement, 20 bytes: category +2, model +4, motion +8, a third pointer +12, instance index +16; it places the raptor in 52 rooms and the Therizinosaurus in 15), 0x5A (4 + 8 × byte 3 bytes), 0x64 (sets floor zone +1's footstep step to +2; handler 0x42BE92)
- **Opcode 0x23** (32 bytes): Places a mesh section into a model slot at a world position with rotation and render mode. Verified against `sub_426806` in DINO.exe; the opcode dispatch table starts at VA `0x006576A0` (the two words before it are 0 and 1)
- **Opcode 0x28** (size by type): a zone's four XZ corners at +4..+19; a real zone repeats its type at +20 and has 1 at +23, which no other 0x28 byte in the scripts does
- **Opcode 0x28 type 4** (44 bytes): Item pickup zone. Its model (slot at +34, section at +36) is placed on the floor at the zone centre (`sub_426DFC` → `sub_448E3B`) and spins while the item is there
- **Opcode 0x5B** (44 bytes): Enemy spawn. A cutscene can spawn its enemy off stage and move it in later: ST100's raptor is spawned at x = z = 0x7000 and appears where the cutscene moves it, (2024, 0, 3008). Every other spawn lies within ±15104, so the overlay shows a spawn beyond ±20000 where its thread first moves it on stage
- **Work target**: 0x22 03 `<slot>` selects a model slot and 0x22 02 `<slot>` a character (the entry that 0x20 or 0x5B filled); 0x2A sets one of its fields (3/4/5 = x/y/z, 6/7/8 = rotation, via `sub_47416C`), 0x36 sets x/y/z and 0x37 the rotation
- **Control flow**: 0x0C (unconditional jump), 0x0E (conditional relative jump on the last test result), 0x0F (gosub: pushes pc+4 and jumps) and 0x10 (return), 0x05 (jump to another event subroutine), 0x04 (end of thread; every thread finishes with `04 00 00 00`). 0x01 and 0x0A are 4-byte opcodes that scripts continue past; decoding each script straight through with the opcode sizes lands exactly on the next script in every room. Message text follows the last thread in the same block
- **Exe build**: the DINO.exe addresses in this document and in the source come from a different build than the GOG "Classic REbirth" exe. In that exe, code in 0x422000-0x42BE92 sits 0x160 lower (the 0x42A96D above is 0x42A80D), and the data addresses do not carry over

### Dino Crisis 2

Sources are the dinorand project's documents; `K<n>` is a row of its Dino Crisis 2 registry. Everything below was checked against all files of the GOG build by `tests/dc2_corpus.cpp`.

**Package.** As the DC1 archive, with 32-byte header entries: the 16 bytes of a DC1 entry followed by 16 zero bytes. Unused header slots are `"dummy header    "` plus 16 zeros, and payload padding is zeros. Types: 0 data, 1 texture, 2 palette, 3 sound bank, 4 MP3 stream, 5 LZSS data, 6 LZSS texture, 7 unknown, 8 LZSS texture. Types 1 and 6 are block-tiled like DC1 textures; type 8 is stored row by row. For data entries the first word after the size is the address the entry is loaded at, and every pointer inside the entry is relative to it. `.TEX` files are ordinary packages. `ST*.DBS` is a package followed by more data (the backgrounds, not decoded).

**Room** (`ST*.DAT`, the type 5 entry, loaded at `0x5E0000`). The blob starts with 12 absolute pointers; a null pointer means the room has no such section, and a section runs to the next higher pointer.

| Slot | Contents | Read |
|---|---|---|
| 0 | Draw mesh | no |
| 1 | Markers: `u16 count, u16`, then 20-byte records `{u16 1, u16 1000, u32 id, s16 x, y, z, u16 0x8000, u16, u16}` | yes |
| 2 | Collision polygons | yes |
| 3 | Cameras | no |
| 4 | Table of unknown purpose (36 rooms) | no |
| 5 | Script | yes |
| 6 | Camera-switch zones | no |
| 7 | Zones (79 rooms) | corners only |
| 8 | Prop geometry (61 rooms) | no |
| 9, 10 | Unknown | no |
| 11 | End of the blob | |

Slot 2: `u8 count, u8 count2, u16 0, u32 offset2`, then `count` record offsets relative to the slot. A record is `u8 flags, u8 npoints, u8 index, u8 param`, 4 unknown bytes, `npoints` u32 attribute words, then `npoints + 1` pairs of `s16 x, z` whose last repeats the first: 12 + 8 x npoints bytes. `offset2` leads to `count2` 20-byte records that are not decoded.

Slot 7: a 16-byte header whose first byte is the zone count, then 32-byte records: four `s16 x, z` corners, a `u16` offset to the zone's own data, and 14 more bytes. Only the corners are read.

**Script** (slot 5). A stack machine. Opcodes are one byte: `0x01`-`0x03` are 6 bytes long, `0x05`-`0x08` are 4, `0x10`-`0x59` are 2; `0x04` returns. The routines are found through a table of `u32` offsets at slot + `0x1C`, relative to the table, which ends where the first routine begins. `0x05 <mode> <s16>` pushes a value; mode 1 is a variable, 2 a stack slot, 6 the room's global table, and anything else a literal.

Records are built from the run of pushes in front of an opcode. The last push is block + 0, the one before it block + 4, and so on:

| Opcode | Record | Fields (block offset) |
|---|---|---|
| `0x30` | Door | unlock flag `+0x00`, lock `+0x04`, destination `+0x0C` (stage in the high byte, room in the low: `0x0102` is ST102), corners `+0x3C`..`+0x20`, slot `+0x48` |
| `0x31` | Trigger | routine `+0x00`, thread `+0x04`, corners `+0x24`..`+0x08`, slot `+0x30` |
| `0x35` | Item pickup | item id `+0x10`, taken flag `+0x14`, corners `+0x34`..`+0x18`, slot `+0x40` |
| `0x1A` | Enemy spawn | model address `+0x0C`, z `+0x14`, y `+0x18`, x `+0x1C`, type `+0x20`, slot `+0x24` |

Corners are four `x, z` pairs, first pair at the highest offset.

**Textures.** A texture is stored as whole 64-byte by 32-row tiles even when its height is not a multiple of 32 (a 64x136 texture has 160 rows of data), in both games. A palette shorter than 512 bytes is 16-colour sub-palettes side by side, 32 bytes each, and several palettes can share a VRAM row at different x.

**Model** (DCM, a type 5 entry). Header: vertex, normal, triangle and quad pool pointers, `u16` triangle and quad counts, `u32` part count, `u16 tpage, clut`. Then the parts, 20 bytes each: `s16 x, y, z` offset from the parent, `s8 parent`, `u8 n`, triangle and quad pointers, `u16` triangle and quad counts. Vertices and normals are 8 bytes, triangles 12 (three indices, three UVs) and quads 16. Part *i* owns the next 3 x *n* vertices of the pool and its bone places them. A quad is a strip: (v0, v1, v2) and (v1, v3, v2). All faces share the header's tpage and clut.

### PSX Transparency Rules

The tool implements the PSX GPU transparency pipeline:

| BGR555 Value | STP Bit | Context | Alpha | Behavior |
|---|---|---|---|---|
| `0x0000` | 0 | Any | 0 | Pixel masked (not drawn) |
| `0x8000` | 1 | Any | 0 | STP black = effectively transparent |
| STP=1 + color | 1 | 4bpp sub-palette | 128 | Semi-transparent decal (blood) |
| STP=1 + color | 1 | 8bpp palette | 255 | Default VRAM state, NOT transparency |
| STP=0 + color | 0 | Any | 255 | Fully opaque |

In a Dino Crisis 2 atlas, index 0 with `0x8000` is opaque black: its models are drawn opaque and use the index for dark texels.

### Texture Atlas

The tool composites all archive textures into a single OpenGL atlas using 2D sparse allocation:

- Each unique `(CLUT_row, sub_palette)` pair gets exactly one 512-pixel-tall slice
- 8bpp and 4bpp rendering use **separate slots** (65 per row: slot 0 = 8bpp, slots 1-64 = 4bpp sub-palettes)
- This prevents 4bpp/8bpp collision when a 4bpp face has CLUT X equal to the palette base X
- Typical rooms use 10-40 slices instead of the theoretical maximum, keeping atlas height within GPU texture size limits

## Keyboard Shortcuts

### 3D Viewer (Orbit Mode)
| Key | Action |
|---|---|
| **LMB drag** | Orbit camera |
| **RMB drag** | Pan camera |
| **Scroll** | Zoom |
| **Ctrl+LMB** | Pick object (select material group) |
| **W** | Toggle wireframe |
| **L** | Toggle lighting |
| **T** | Toggle textured rendering |
| **V** | Toggle vertex colors |
| **C** | Toggle backface culling (see-through room faces such as fences and the signs painted on them are always drawn from both sides, as the game does) |
| **O** | Toggle overlay visualization and its elements panel |
| **H** | Show/hide unplaced (unused) room sections |
| **K** / **Shift+K** | Next / previous game camera shot (its view, field of view and sections) |
| **WASD** / **Arrow keys** | On a shot, walk the player the camera follows (**Shift**: run); W toggles wireframe only when no shot is selected |
| **J** | Back onto the shot after orbiting away or scrolling |
| **I** | Toggle room scene lights |
| **N** | Toggle vertex normal display |
| **G** | Toggle ground grid |
| **F** | Enter walk-through mode |
| **R** | Reset camera to default |
| **U** | Open UV editor |
| **Esc** | Clear pick selection |
| **F2** | Toggle HUD info |
| **F11** | Cycle render mode (Normal / VM / Software) |

### Walk-Through Mode
| Key | Action |
|---|---|
| **WASD** | Move |
| **Mouse** | Look |
| **Shift** | Sprint |
| **Space** | Jump |
| **P** | Toggle noclip fly mode |
| **LMB** | Shoot fireball |
| **Esc / F** | Exit walk mode |

## AI Disclosure

> **This project was developed entirely using [Anthropic's Claude Opus 4.6](https://www.anthropic.com/) large language model.**
>
> All source code — including the Win32 application framework, OpenGL rendering pipeline, PSX format reverse engineering, SCD bytecode parser, texture atlas system, LZSS decompressor, VAG audio decoder, save file editor, and every feature described in this README — was written by Claude through iterative conversation with a human collaborator providing direction, test data, and visual feedback.
>
> **Human collaborator (Corey) provided:**
> - Game binary files (DINO.exe, .DAT archives) for analysis
> - Screenshots and descriptions of rendering issues
> - Domain knowledge of PSX hardware behavior and Dino Crisis internals
> - Feature requests, bug reports, and visual QA
>
> **Claude Opus 4.6 provided:**
> - All C++ source code (~32,000 lines)
> - Binary format reverse engineering and struct definitions
> - DINO.exe disassembly analysis (SCD dispatch tables, GPU render modes, model table structures)
> - PSX GPU emulation logic (CLUT indexing, STP transparency, VRAM deswizzling, vertex color modulation)
> - 2D sparse texture atlas architecture
> - This README and all documentation
>
> Later fixes (uncompressed rooms, face-based texture decoding, VAG playback rates, scripted room placement and outdoor geometry) were made with Claude Opus 5.5 via Claude Code, using the same workflow.

## Acknowledgments

- **Capcom** — Dino Crisis (1999), one of the great survival horror games
- **Anthropic** — Claude Opus 4.6 AI model used for all development
- The Dino Crisis modding and reverse engineering community

## License

This project is released into the **public domain** under [The Unlicense](LICENSE). You are free to use, modify, distribute, and build upon this work for any purpose without restriction.

---

<div align="center">

*"An experiment in AI-assisted game reverse engineering"*

**[Dino Crisis](https://en.wikipedia.org/wiki/Dino_Crisis) (c) 1999 Capcom. This tool is an independent fan project for preservation and modding purposes.**

</div>
