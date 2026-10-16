# RaptorScope DC1 GLB Export — experimental Windows build

This modified RaptorScope adds GLB export of the currently opened Dino Crisis (1999 PC) EMD character model, its rigid skeleton/skin, detected animation clips, and the current viewer texture atlas when available. It is based on upstream commit `45ed5c44fdfbd7a416d2df85a1c39cdc0246b047` from https://github.com/coreynguyen/RaptorScope, under The Unlicense. This is an unofficial modification, not an upstream release.

## Use on Windows

1. Extract the ZIP to a folder. Run `RaptorScope-DC1-GLB.exe` (64-bit Windows 10/11 target). No installation or additional runtime DLLs are bundled or needed beyond Windows system libraries.
2. Open your own **Dino Crisis 1 PC** DAT archive using File > Open. Select an EMD character model/character child in the archive tree so the character appears in the 3D viewer. (Rooms export too; see **Room export** below.)
3. Check the model, textures, and animation clips in the viewer. If a motion already looks wrong there, this exporter will preserve that decoding; it does not fix the upstream animation detector.
4. Choose **Export > Export GLB (Character + Animations, or Room)**. Save the `.glb` file.
5. In Blender, choose **File > Import > glTF 2.0 (.glb/.gltf)**. The file contains joint nodes, a rigidly skinned mesh, and separate clips named `Anim_0`, `Anim_1`, etc. Blender's exact Action/NLA presentation depends on its glTF importer version. Select the armature and inspect the Action Editor/NLA Editor to choose a clip.

Coordinates keep the RaptorScope viewer's axes. Game units (about a millimetre each) are converted to metres, so a raptor imports about 1.8 m tall. The bind pose is reconstructed from the source pool, not the frame currently displayed in the viewer.

## Room export

Open a room (an RDT scene entry) in the 3D viewer and choose the same **Export GLB** command. The file holds the room as the viewer shows it:

- One object per room section, named `section_0x...` after its offset in the RDT, under a root named after the DAT file.
- The viewer's texture atlas as an embedded PNG, with each face's UVs mapped onto it, and the vertex colours as `COLOR_0`.
- Opaque faces use an alpha-masked material (`opaque`). Faces with a PS1 semi-transparency mode get a 50% blended material named after the mode (`semi_additive`, `semi_subtractive`, `semi_quarter_additive`), because glTF has no additive or subtractive blending; the mode number is kept in the material's `extras`.
- All materials are double-sided, as the game draws room faces.
- Sections no script places (shown magenta with **H**) are left out, as are the overlay markers.
- Windows, signs and floor markings that the game draws over a surface in front of them (such as ST101's restroom windows, 22 units behind the wall) are moved 2 units in front of that surface, as in the viewer, so they show in any renderer with a depth buffer.

Coordinates are the viewer's (Y up), converted from game units to metres, the same axes and scale as character exports.

## OBJ and SMD export

**Export OBJ** and **Export SMD** write the entry selected in the archive tree. For a character, that is its model in the bind pose. For a room, it is the room as the viewer builds it, with the same sections as a room GLB and no unplaced ones. The two rooms stored uncompressed (ST50B, ST60E) export as well. Neither format carries textures; OBJ writes each face's colour as a `# vc r g b` comment after it.

OBJ positions use the viewer's axes and are in metres, so an OBJ lines up with a GLB of the same room. SMD positions are still in game units.

## Limitations

- **Experimental and not yet validated on real DC1 assets or launched under Windows.** The Windows programs were cross-compiled successfully; synthetic GLB tests ran on Linux. No copyrighted game assets are included.
- Upstream RaptorScope identifies clips and frame sizes heuristically. Its source includes contradictory historical comments about rotation order. This modification shares the current executable decoder with playback, rather than inventing a new interpretation. Correct-looking viewer animation is a prerequisite; matching the game requires real asset comparisons. A clip has its stored count plus one frames, so clips export one frame longer than before; the few clips with no valid extra frame keep the stored count.
- Exports per-bone rotations and static local bone translations. Entity world rotation/velocity metadata is ignored by the upstream skeletal playback function and is also omitted here. Original movement across the world is not reconstructed.
- The GUI uses **30 fps**, matching the viewer's approximately 33 ms animation timer. This is a playback default, not a verified original-game timing table. Clip names do not identify idle/walk/attack automatically.
- Textures are embedded as PNG when the viewer has successfully loaded an atlas. Single- and multiple-palette-slice atlases are supported, with the same tpage/CLUT UV mapping as the viewer. If the atlas is unavailable, the model exports without an image. Standard glTF alpha masking does not reproduce all PS1 semitransparency blend modes.
- Exports triangle-corner vertices and flat geometric normals. This preserves UV seams and rigid assignments; it does not attempt to rebuild a welded, smooth-shaded mesh.
- Rooms with many palettes have tall atlases, and the PNG is stored uncompressed, so a room GLB can run to tens of megabytes.
- Invalid/cyclic/out-of-order bone hierarchies, unsupported atlas sizes, or out-of-bounds detected clips cause export failure. The existing archive/model parser is unchanged apart from shared playback refactoring.
- This targets **DC1 PC**, not the DC1 PlayStation disc format. Dino Crisis 2 (PC) models export in their bind pose, without animation: OBJ, SMD and GLB from the GUI, GLB from `dc1_export`. Enemy models are textured when the `.TEX` of the same name is beside the package. A Dino Crisis 2 room has no mesh to export.

## Command-line companion

The included `dc1_export.exe` uses the same parser/exporter but has no viewer texture atlas, so its GLBs contain rig and motion **without textures**.

```bat
dc1_export.exe "C:\games\Dino Crisis\example.DAT"
dc1_export.exe "C:\games\Dino Crisis\example.DAT" 3 "C:\exports\character.glb"
dc1_export.exe "C:\games\Dino Crisis\example.DAT" 3 "C:\exports\character.glb" 60
```

The first command lists model candidate entry indices. Use the printed index in the export command; `3` is only an example. An optional fifth argument after fps supplies an explicit EMD header offset (decimal or `0x...`) for multi-model entries. The GUI is the easier route for selecting a specific model and exporting its textures.

## Rebuild

Full modified source is in `source/`. With MinGW-w64's `g++` and `windres` on PATH, run `source\build-windows.cmd`. It creates both programs in `source\bin\Release`. C++11 is used because the existing UI contains lambdas, despite the upstream README's older compiler claim. The exporter itself was also compiled in C++98 mode during tests. The `.inc` exporter is included from `mesh.cpp`, so existing IDE projects do not need an added translation unit.

A developer can compile the command-line tool on Linux:

```sh
g++ -std=c++11 -O2 -Iinclude tools/dc1_export.cpp src/formats/dat.cpp src/formats/mesh.cpp src/formats/dc2_model.cpp src/core/lzss.cpp -o dc1_export
```

## Validation performed

- Windows x64 GUI and CLI compile/link with MinGW-w64 GCC 13, statically linking compiler runtime libraries. The GUI imports Windows system DLLs only.
- C++ synthetic fixture with a three-bone hierarchy, packed 12-bit rotations crossing word boundaries, two independent clips, and mirrored vertex assignments.
- Independent Python glTF skinning computation matches the viewer decoder at every tested frame within `0.0001` game units.
- Identical GLB bytes before and after viewer playback confirms export does not use an animated current mesh as its rest pose.
- PNG CRCs, decompression, expected pixels/alpha, GLB chunk alignment, buffer bounds, position bounds, normalized quaternions, weights, and frame timestamps checked.
- Khronos glTF Validator: animated fixture **0 errors, 0 warnings**. Static fixture **0 errors, 0 warnings** (one unused UV information message, because it intentionally has no image).
- Out-of-bounds animation data and an invalid parent hierarchy are rejected.

To repeat fixture tests from the source root (Python requires NumPy):

```sh
mkdir -p build/check
g++ -std=c++98 -Iinclude tests/export_fixture.cpp src/formats/mesh.cpp src/core/lzss.cpp -o build/check/export_fixture
build/check/export_fixture build/check/fixture.glb > build/check/viewer_positions.txt
python3 tests/check_glb.py
```

The next validation step is one DC1 PC DAT containing a dinosaur/character that animates correctly in the upstream viewer, then checking the exported GLB in Blender against that viewer and the game.
